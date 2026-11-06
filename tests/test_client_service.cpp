#include "node_runtime.h"
#include "room_vectors.h"
#include "openssl_crypto.h"
#include "../components/meshcore/ed25519_identity.h"
#include <cassert>
#include <iostream>
#include <vector>

using namespace meshcore_lib;
static constexpr uint32_t EPOCH = 1793500000;

std::vector<uint8_t> unhex(std::string_view text) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i < text.size(); i += 2)
    out.push_back(std::stoul(std::string(text.substr(i, 2)), nullptr, 16));
  return out;
}
struct Transport final : RoomTransport {
  NodeRuntime &node;
  std::vector<Frame> frames;
  bool full{}, guard{true};
  explicit Transport(NodeRuntime &runtime) : node(runtime) {}
  bool send(const Frame &frame, uint64_t, uint32_t &token) override {
    if (this->full)
      return false;
    token = this->frames.size();
    this->frames.push_back(frame);
    return true;
  }
  bool ack_available(const std::array<uint8_t, 4> &ack) const override {
    return !this->guard || this->node.ack_available(ack);
  }
};
// Deliberately force the short-token and shared-secret ambiguity that real
// admission prevents; dispatch must still fail closed when all candidates match.
struct CollisionCrypto final : CryptoProvider {
  OpenSslCrypto real;
  bool sha256(const uint8_t *, size_t, uint8_t out[32]) override {
    std::fill_n(out, 32, 0x72);
    return true;
  }
  bool hmac_sha256(const uint8_t key[32], const uint8_t *data, size_t size, uint8_t out[32]) override {
    return this->real.hmac_sha256(key, data, size, out);
  }
  bool aes128_ecb(bool encrypt, const uint8_t key[16], const uint8_t *data, size_t size, uint8_t *out) override {
    return this->real.aes128_ecb(encrypt, key, data, size, out);
  }
};
struct CollisionIdentity final : IdentityCrypto {
  esphome::meshcore::Ed25519Identity real;
  bool derive(const uint8_t seed[32], LocalIdentity &out) override { return this->real.derive(seed, out); }
  bool sign(const LocalIdentity &id, const uint8_t *data, size_t size, uint8_t signature[64]) override {
    return this->real.sign(id, data, size, signature);
  }
  bool shared_secret(const LocalIdentity &, const PublicKey &, SharedSecret &out) override {
    out.fill(0x25);
    return true;
  }
};
struct Fixture {
  std::array<RoomService *, 0> rooms{};
  std::array<ClientService *, 3> clients{};
  std::array<LocalClientDelivery, 2> deliveries{};
  NodeRuntime node{this->rooms, this->clients, this->deliveries};
  Transport transport{this->node};
  OpenSslCrypto crypto;
  esphome::meshcore::Ed25519Identity identities;
  RoomProtocol protocol{this->crypto, this->identities};
  ClientService first{this->protocol, this->identities, this->transport};
  ClientService second{this->protocol, this->identities, this->transport};
  LocalIdentity local{}, peer{}, other{};
  Fixture() {
    assert(this->identities.derive(unhex(CLIENT_SEED).data(), this->local));
    assert(this->identities.derive(unhex(ROOM_SEED).data(), this->peer));
    assert(this->identities.derive(unhex(SECOND_CLIENT_SEED).data(), this->other));
    assert(this->first.configure(this->local) && this->second.configure(this->other));
    assert(this->node.add_client(this->first) && this->node.add_client(this->second));
  }
  void receive(const Frame &frame, uint64_t now = 100) {
    PacketView packet;
    assert(parse_packet(frame.bytes.data(), frame.size, packet));
    this->node.receive(packet, now, EPOCH);
  }
};
Frame vector_frame(const char *text) {
  const auto bytes = unhex(text);
  Frame frame;
  frame.size = bytes.size();
  std::copy(bytes.begin(), bytes.end(), frame.bytes.begin());
  return frame;
}
struct Listener final : ClientMessageListener {
  std::vector<ClientMessage> messages;
  std::vector<ClientSendState> states;
  NodeRuntime *node{};
  ClientService *responder{};
  void on_client_message(const ClientMessage &message) override {
    this->messages.push_back(message);
    if (this->responder && message.body() == "ping")
      assert(this->node->send_local_client_message(*this->responder, message.sender, "pong", message.timestamp + 1, 2));
  }
  void on_client_send_state(ClientSendState state) override { this->states.push_back(state); }
};
void test_local() {
  Fixture f, independent;
  assert(f.first.add_peer(f.second.public_key()) && f.second.add_peer(f.first.public_key()));
  assert(independent.first.add_peer(independent.second.public_key()) &&
         independent.second.add_peer(independent.first.public_key()));
  Listener alice, bob;
  bob.node = &f.node;
  bob.responder = &f.second;
  f.first.set_listener(&alice);
  f.second.set_listener(&bob);
  assert(!f.node.send_local_client_message(independent.first, f.second.public_key(), "ping", EPOCH, 2));
  assert(!f.node.send_local_client_message(f.first, f.first.public_key(), "ping", EPOCH, 2));
  assert(!f.node.send_local_client_message(f.first, f.peer.public_key, "ping", EPOCH, 2));
  assert(f.node.send_local_client_message(f.first, f.second.public_key(), "ping", EPOCH, 2));
  assert(!f.node.send_local_client_message(f.first, f.second.public_key(), "again", EPOCH, 2));
  assert(!f.first.pending_radio() && f.first.send_state() == ClientSendState::QUEUED);
  assert(f.node.next_deadline(5, 0) == 5 && bob.messages.empty());
  f.node.transmitted(0, false, 5);  // Unrelated radio completion must not affect local delivery.
  f.node.tick(6, 0);
  assert(f.first.send_state() == ClientSendState::DELIVERED_LOCALLY);
  assert(bob.messages.size() == 1 && bob.messages[0].body() == "ping" && bob.messages[0].local);
  assert(bob.messages[0].sender_public_key().view() == CLIENT_PUBLIC);
  assert(alice.messages.empty());  // The reply is deferred, never recursive.
  f.node.tick(7, 0);
  assert(alice.messages.size() == 1 && alice.messages[0].body() == "pong");
  assert(alice.messages[0].sender == f.other.public_key);
  assert(f.second.send_state() == ClientSendState::DELIVERED_LOCALLY);
  assert(f.transport.frames.empty() && f.node.next_deadline(8, 0) == UINT64_MAX);
  assert(independent.first.send_state() == ClientSendState::IDLE);
  assert(alice.states == std::vector<ClientSendState>({ClientSendState::QUEUED, ClientSendState::DELIVERED_LOCALLY}));
  // Loopback must not discover the sender key from the local identity registry.
  Fixture unknown;
  Listener unknown_receiver;
  unknown.second.set_listener(&unknown_receiver);
  assert(unknown.node.send_local_client_message(unknown.first, unknown.second.public_key(), "ping", EPOCH, 2));
  unknown.node.tick(1, 0);
  assert(unknown.first.send_state() == ClientSendState::FAILED && unknown_receiver.messages.empty());
  assert(unknown.transport.frames.empty());
  // A one-slot queue applies backpressure without changing the rejected sender.
  std::array<RoomService *, 0> rooms{};
  std::array<ClientService *, 2> clients{};
  std::array<LocalClientDelivery, 1> queue{};
  NodeRuntime small(rooms, clients, queue);
  assert(small.add_client(independent.first) && small.add_client(independent.second));
  assert(small.send_local_client_message(independent.first, independent.second.public_key(), "one", EPOCH, 1));
  assert(!small.send_local_client_message(independent.second, independent.first.public_key(), "full", EPOCH, 1));
  assert(independent.second.send_state() == ClientSendState::IDLE);
  small.tick(1, 0);
  assert(small.send_local_client_message(independent.second, independent.first.public_key(), "two", EPOCH, 1));
  small.tick(2, 0);
}
void test_receive() {
  Fixture f;
  Listener listener;
  // CLIENT_DM is an independently generated normal-text packet to ROOM_SEED.
  ClientService recipient(f.protocol, f.identities, f.transport);
  assert(recipient.configure(f.peer) && f.node.add_client(recipient));
  recipient.set_listener(&listener);
  const Frame dm = vector_frame(CLIENT_DM);
  f.receive(dm);
  assert(listener.messages.empty() && f.transport.frames.empty());
  assert(recipient.add_peer(f.local.public_key) && recipient.add_peer(f.local.public_key));
  Frame damaged = dm;
  damaged.bytes[6] ^= 1;
  f.receive(damaged);
  assert(listener.messages.empty());
  f.transport.full = true;
  f.receive(dm);
  assert(listener.messages.empty());  // Failed receipt admission cannot consume replay state.
  f.transport.full = false;
  f.receive(dm);
  assert(listener.messages.size() == 1 && listener.messages[0].body() == "Hello peer");
  assert(!listener.messages[0].local && listener.messages[0].sender == f.local.public_key);
  const auto receipt = vector_frame(CLIENT_EMPTY_PATH_ACK);
  assert(f.transport.frames.back().size == receipt.size && f.transport.frames.back().bytes == receipt.bytes);
  f.receive(dm);
  assert(listener.messages.size() == 1 && f.transport.frames.size() == 2);
  SharedSecret secret;
  assert(f.identities.shared_secret(f.local, f.peer.public_key, secret));
  std::array<uint8_t, 32> plain{};
  write_u32(plain.data(), EPOCH + 1);
  plain[4] = 1;  // A stock retry changes attempt bits; the callback remains deduplicated.
  std::copy_n("Hello peer", 10, plain.begin() + 5);
  plain[16] = 5;  // Extended attempt byte after NUL, supported by the pinned upstream.
  Frame retry;
  assert(f.protocol.datagram(2, f.local, f.peer.public_key, secret, plain.data(), 17, true, PacketPath{}, retry));
  f.receive(retry);
  assert(listener.messages.size() == 1 && f.transport.frames.size() == 3);
  write_u32(plain.data(), EPOCH);  // Older message never delivers or consumes a receipt slot.
  assert(f.protocol.datagram(2, f.local, f.peer.public_key, secret, plain.data(), 17, true, PacketPath{}, retry));
  f.receive(retry);
  assert(f.transport.frames.size() == 3);
  write_u32(plain.data(), EPOCH + 2);
  assert(f.protocol.datagram(2, f.local, f.peer.public_key, secret, plain.data(), 17, false, PacketPath{}, retry));
  f.receive(retry);
  assert(listener.messages.size() == 2);
  PacketView packet;
  assert(parse_packet(f.transport.frames.back().bytes.data(), f.transport.frames.back().size, packet));
  assert(packet.type == 3 && packet.flood());  // No learned return route; flood the raw receipt.
  assert(!recipient.add_peer(recipient.public_key()));
  for (uint8_t i = 1; i <= 7; ++i) {
    PublicKey key{};
    key.fill(i);
    assert(recipient.add_peer(key));
  }
  PublicKey overflow{};
  overflow.fill(99);
  assert(!recipient.add_peer(overflow));
}
int main() {
  test_local();
  test_receive();
  Fixture a, b;
  PublicKey parsed;
  assert(parse_public_key(CLIENT_PUBLIC, parsed) && parsed == a.local.public_key);
  assert(!parse_public_key("1234", parsed));
  assert(!parse_public_key(std::string(64, '0'), parsed));
  assert(!parse_public_key(std::string(64, 'f'), parsed));
  assert(!parse_public_key(std::string(64, 'z'), parsed));
  assert(!a.first.configure(a.local));
  assert(a.node.add_client(a.first));
  ClientService duplicate(a.protocol, a.identities, a.transport);
  assert(duplicate.configure(a.local) && !a.node.add_client(duplicate));
  assert(!a.node.identity_available(a.local.public_key));
  assert(a.node.identity_available(a.peer.public_key));
  Frame advert;
  assert(a.first.advertisement("Automation", EPOCH, false, advert));
  const auto expected_advert = vector_frame(CLIENT_ADVERT);
  assert(advert.size == expected_advert.size && advert.bytes == expected_advert.bytes);
  FloodScope scope;
  const auto scope_key = unhex(ROOM_SCOPE_NYC_KEY);
  assert(make_flood_scope(scope_key.data(), scope_key.size(), scope));
  assert(!a.first.advertisement("Automation", EPOCH, false, advert, &scope));
  assert(a.first.advertisement("Automation", EPOCH, true, advert, &scope));
  assert(advert.bytes[0] == 0x10 && advert.bytes[106] == 0x81);

  assert(a.first.send_client_message(a.peer.public_key, "Hello peer", EPOCH + 1, 2, 0));
  const auto expected_dm = vector_frame(CLIENT_DM);
  assert(a.transport.frames.back().size == expected_dm.size && a.transport.frames.back().bytes == expected_dm.bytes);
  const auto ack = unhex(CLIENT_DM_ACK);
  assert(std::equal(ack.begin(), ack.end(), a.first.pending_ack().begin()));
  assert(!a.first.send_client_message(a.peer.public_key, "busy", EPOCH + 2, 2, 1));
  assert(b.first.send_state() == ClientSendState::IDLE);
  assert(a.second.send_client_message(a.peer.public_key, "Hello peer", EPOCH + 1, 2, 0));
  assert(a.second.pending_ack() != a.first.pending_ack());
  // The recipient decrypts each message with the selected sender's full identity.
  SharedSecret secret;
  assert(a.identities.shared_secret(a.peer, a.other.public_key, secret));
  PacketView packet;
  Plaintext plain;
  assert(parse_packet(a.transport.frames.back().bytes.data(), a.transport.frames.back().size, packet));
  assert(a.protocol.open_peer(packet, secret, plain));
  assert(read_u32(plain.bytes.data()) == EPOCH + 1 && plain.bytes[4] == 0);
  assert(std::string_view(reinterpret_cast<char *>(plain.bytes.data() + 5), 10) == "Hello peer");
  const auto path_ack = vector_frame(CLIENT_PATH_ACK);
  a.receive(path_ack);  // A receipt before physical TX is not accepted.
  assert(a.first.send_state() == ClientSendState::QUEUED);
  a.node.transmitted(0, true, 100);
  assert(a.node.next_deadline(100, 0) == 30100);
  Frame damaged = path_ack;
  damaged.bytes[6] ^= 1;
  a.receive(damaged, 101);
  assert(a.first.send_state() == ClientSendState::SENT);
  a.receive(path_ack, 102);
  assert(a.first.send_state() == ClientSendState::ACKNOWLEDGED);
  assert(a.second.send_state() == ClientSendState::QUEUED);
  assert(!a.first.send_client_message(a.peer.public_key, "Hello peer", EPOCH + 1, 2, 103));  // Recent receipt guard.
  a.node.transmitted(1, false, 104);
  assert(a.second.send_state() == ClientSendState::FAILED);
  assert(a.first.send_client_message(a.peer.public_key, "Next", EPOCH + 2, 2, 105));
  a.node.tick(100000, EPOCH);  // Queuing alone never starts the timeout.
  assert(a.first.send_state() == ClientSendState::QUEUED);
  a.node.transmitted(2, true, 100001);
  a.node.tick(130000, EPOCH);
  assert(a.first.send_state() == ClientSendState::SENT);
  a.node.tick(130001, EPOCH);
  assert(a.first.send_state() == ClientSendState::TIMED_OUT);
  assert(a.first.send_client_message(a.peer.public_key, "Raw ACK", EPOCH + 3, 1, 130002));
  Frame raw;
  assert(a.protocol.ack(a.first.pending_ack().data(), true, PacketPath{}, raw));
  a.node.transmitted(3, true, 130003);
  a.receive(raw, 130004);
  assert(a.first.send_state() == ClientSendState::ACKNOWLEDGED);
  a.transport.full = true;
  assert(!a.first.send_client_message(a.peer.public_key, "Full", EPOCH + 4, 2, 130005));
  assert(a.first.send_state() == ClientSendState::ACKNOWLEDGED);
  a.transport.full = false;
  assert(!a.first.send_client_message(a.local.public_key, "self", EPOCH, 2, 0));
  assert(!a.first.send_client_message(a.peer.public_key, "", EPOCH, 2, 0));
  assert(!a.first.send_client_message(a.peer.public_key, "abc", 0, 2, 0));
  assert(!a.first.send_client_message(a.peer.public_key, "abc", EPOCH, 4, 0));
  assert(!a.first.send_client_message(a.peer.public_key, std::string(161, 'x'), EPOCH, 2, 0));
  assert(!a.first.send_client_message(a.peer.public_key, std::string_view("a\0b", 3), EPOCH, 2, 0));
  assert(a.first.send_client_message(a.peer.public_key, std::string(160, 'x'), EPOCH + 5, 3, 130006));
  // B's identical operation remains independent of all A's pending/recent receipts.
  assert(b.first.send_client_message(b.peer.public_key, "Hello peer", EPOCH + 1, 2, 0));
  b.node.transmitted(0, true, 1);
  b.receive(path_ack, 2);
  assert(b.first.send_state() == ClientSendState::ACKNOWLEDGED);
  // Real, distinct client identities with the same routing prefix authenticate all candidates.
  Fixture prefixes;
  LocalIdentity collision;
  assert(prefixes.identities.derive(unhex(COLLIDING_ROOM_SEED).data(), collision));
  std::array<RoomService *, 0> no_rooms{};
  std::array<ClientService *, 2> registry{};
  std::array<LocalClientDelivery, 2> local_queue{};
  NodeRuntime router(no_rooms, registry, local_queue);
  Transport transport(router);
  ClientService left(prefixes.protocol, prefixes.identities, transport);
  ClientService right(prefixes.protocol, prefixes.identities, transport);
  assert(left.configure(prefixes.peer) && right.configure(collision));
  assert(left.public_key()[0] == right.public_key()[0]);
  assert(router.add_client(left) && router.add_client(right));
  assert(left.send_client_message(prefixes.local.public_key, "Left", EPOCH, 2, 0));
  assert(right.send_client_message(prefixes.local.public_key, "Right", EPOCH, 2, 0));
  router.transmitted(0, true, 1);
  router.transmitted(1, true, 1);
  SharedSecret reply_secret;
  assert(prefixes.identities.shared_secret(prefixes.local, collision.public_key, reply_secret));
  Frame reply;
  assert(prefixes.protocol.path_response(prefixes.local, collision.public_key, reply_secret, PacketPath{}, 3,
                                         right.pending_ack().data(), 4, reply));
  assert(parse_packet(reply.bytes.data(), reply.size, packet));
  router.receive(packet, 2, 0);  // Client receipt processing does not require source UTC.
  assert(right.send_state() == ClientSendState::ACKNOWLEDGED && left.send_state() == ClientSendState::SENT);
  // Late receipts cannot beat timeout simply by arriving before the next timer tick.
  assert(prefixes.protocol.ack(left.pending_ack().data(), true, PacketPath{}, reply));
  assert(parse_packet(reply.bytes.data(), reply.size, packet));
  router.receive(packet, 30001, 0);
  assert(left.send_state() == ClientSendState::TIMED_OUT);
  Listener local_collision;
  left.set_listener(&local_collision);
  assert(left.add_peer(right.public_key()));
  assert(router.send_local_client_message(right, left.public_key(), "same prefix", EPOCH, 2));
  router.tick(30002, 0);
  assert(local_collision.messages.size() == 1 && local_collision.messages[0].sender == right.public_key());
  assert(local_collision.messages[0].body() == "same prefix");
  CollisionCrypto weak_crypto;
  CollisionIdentity weak_identity;
  RoomProtocol weak_protocol(weak_crypto, weak_identity);
  std::array<RoomService *, 0> empty_rooms{};
  std::array<ClientService *, 2> ambiguous_clients{};
  NodeRuntime ambiguous(empty_rooms, ambiguous_clients);
  Transport unsafe_transport(ambiguous);
  ClientService one(weak_protocol, weak_identity, unsafe_transport);
  ClientService two(weak_protocol, weak_identity, unsafe_transport);
  assert(one.configure(prefixes.peer) && two.configure(collision));
  assert(ambiguous.add_client(one) && ambiguous.add_client(two));
  assert(one.send_client_message(prefixes.local.public_key, "one", EPOCH, 2, 0));
  assert(!two.send_client_message(prefixes.local.public_key, "two", EPOCH, 2, 0));
  unsafe_transport.guard = false;
  assert(two.send_client_message(prefixes.local.public_key, "two", EPOCH, 2, 0));
  ambiguous.transmitted(0, true, 1);
  ambiguous.transmitted(1, true, 1);
  assert(weak_protocol.ack(one.pending_ack().data(), true, PacketPath{}, reply));
  assert(parse_packet(reply.bytes.data(), reply.size, packet));
  ambiguous.receive(packet, 2, EPOCH);
  assert(ambiguous.ambiguous_acks() == 1);
  reply_secret.fill(0x25);
  assert(weak_protocol.path_response(prefixes.local, collision.public_key, reply_secret, PacketPath{}, 3,
                                     two.pending_ack().data(), 4, reply));
  assert(parse_packet(reply.bytes.data(), reply.size, packet));
  ambiguous.receive(packet, 3, EPOCH);
  assert(ambiguous.ambiguous_packets() == 1);
  assert(one.send_state() == ClientSendState::SENT && two.send_state() == ClientSendState::SENT);
  Listener rejected_one, rejected_two;
  one.set_listener(&rejected_one);
  two.set_listener(&rejected_two);
  assert(one.add_peer(prefixes.local.public_key) && two.add_peer(prefixes.local.public_key));
  std::array<uint8_t, 9> text{};
  write_u32(text.data(), EPOCH);
  std::copy_n("ping", 4, text.begin() + 5);
  assert(weak_protocol.datagram(2, prefixes.local, collision.public_key, reply_secret, text.data(), text.size(), true,
                                PacketPath{}, reply));
  assert(parse_packet(reply.bytes.data(), reply.size, packet));
  const size_t sent_before = unsafe_transport.frames.size();
  ambiguous.receive(packet, 4, EPOCH);
  assert(ambiguous.ambiguous_packets() == 2);
  assert(rejected_one.messages.empty() && rejected_two.messages.empty() &&
         unsafe_transport.frames.size() == sent_before);
  PublicKey colliding_peer = prefixes.local.public_key;
  colliding_peer[31] ^= 1;
  assert(one.add_peer(colliding_peer));
  ClientMessage ambiguous_message;
  assert(one.prepare_message(packet, ambiguous_message) == ClientService::Match::AMBIGUOUS);
  std::cout << "PASS: local ping/pong, deferred callbacks, no RF, peer RX/retries/replay/backpressure, client identity "
               "isolation, independent wire vectors, bounds, queue failures, actual-TX ACKs, "
               "PATH/raw receipts, timeout, recent tokens and independent runtimes; ClientService bytes="
            << sizeof(ClientService) << "\n";
}
