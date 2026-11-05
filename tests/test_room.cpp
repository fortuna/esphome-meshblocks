#include "room_service.h"
#include "room_vectors.h"
#include "openssl_crypto.h"
#include "../components/meshcore/ed25519_identity.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>
using namespace meshcore_lib;
std::vector<uint8_t> hex(const char *text) {
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i < std::strlen(text); i += 2)
    bytes.push_back(std::stoul(std::string(text + i, 2), nullptr, 16));
  return bytes;
}
class MemoryPosts final : public RoomRepository {
 public:
  std::vector<RoomPost> posts;
  bool fail{};
  bool append(const RoomPost &post) override {
    if (this->fail)
      return false;
    if (this->posts.size() == ROOM_HISTORY_CAPACITY)
      this->posts.erase(this->posts.begin());
    this->posts.push_back(post);
    return true;
  }
  size_t count() const override { return this->posts.size(); }
  bool get(size_t i, RoomPost &out) const override {
    if (i >= this->posts.size())
      return false;
    out = this->posts[i];
    return true;
  }
  uint32_t latest_timestamp() const override { return this->posts.empty() ? 0 : this->posts.back().timestamp; }
};
class FakeTransport final : public RoomTransport {
 public:
  struct Sent {
    Frame frame;
    uint32_t token;
    uint64_t not_before;
  };
  std::vector<Sent> sent;
  bool full{};
  bool ack_available(const std::array<uint8_t, 4> &) const override { return true; }
  bool send(const Frame &frame, uint64_t not_before, uint32_t &token) override {
    if (this->full)
      return false;
    token = static_cast<uint32_t>(this->sent.size());
    this->sent.push_back({frame, token, not_before});
    return true;
  }
};
class FakeRandom final : public RandomSource {
 public:
  uint8_t next{};
  bool fill(uint8_t *out, size_t size) override {
    while (size--)
      *out++ = this->next++;
    return true;
  }
};
struct FakeStatus final : NodeStatusSource {
  NodeStatus value;
  NodeStatus snapshot() const override { return this->value; }
};
void receive(RoomService &room, const uint8_t *bytes, size_t size, uint64_t now, uint32_t epoch) {
  PacketView packet;
  if (parse_packet(bytes, size, packet))
    room.receive(packet, now, epoch);
}
void receive(RoomService &room, const Frame &frame, uint64_t now, uint32_t epoch) {
  receive(room, frame.bytes.data(), frame.size, now, epoch);
}
int main() {
  constexpr uint32_t EPOCH = 1793500000;
  OpenSslCrypto crypto_a, crypto_b;
  esphome::meshcore::Ed25519Identity identities_a, identities_b;
  // Cross-check patched upstream arithmetic against independent OpenSSL Ed25519.
  std::mt19937 crypto_random(1793500000);
  for (size_t i = 0; i < 64; ++i) {
    std::array<uint8_t, 32> seed{}, expected_public{};
    for (auto &byte : seed)
      byte = static_cast<uint8_t>(crypto_random());
    LocalIdentity identity;
    const bool accepted = identities_a.derive(seed.data(), identity);
    EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size());
    assert(key);
    size_t public_size = expected_public.size();
    assert(EVP_PKEY_get_raw_public_key(key, expected_public.data(), &public_size) == 1);
    assert(identity.public_key == expected_public);
    assert(accepted == (expected_public[0] != 0 && expected_public[0] != 255));
    std::vector<uint8_t> message(i * 7 + 1);
    for (auto &byte : message)
      byte = static_cast<uint8_t>(crypto_random());
    std::array<uint8_t, 64> signature{}, expected_signature{};
    assert(identities_a.sign(identity, message.data(), message.size(), signature.data()));
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    assert(ctx && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1);
    size_t signature_size = expected_signature.size();
    assert(EVP_DigestSign(ctx, expected_signature.data(), &signature_size, message.data(), message.size()) == 1);
    assert(signature == expected_signature);
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
  }
  LocalIdentity personal, building, client;
  assert(identities_a.derive(hex(ROOM_SEED).data(), personal));
  assert(identities_b.derive(hex(COLLIDING_ROOM_SEED).data(), building));
  assert(identities_b.derive(hex(CLIENT_SEED).data(), client));
  assert(personal.public_key[0] == building.public_key[0] && personal.public_key != building.public_key);
  assert(std::equal(personal.public_key.begin(), personal.public_key.end(), hex(ROOM_PUBLIC).begin()));
  SharedSecret secret, reversed, invalid;
  assert(identities_a.shared_secret(personal, client.public_key, secret));
  assert(identities_b.shared_secret(client, personal.public_key, reversed) && secret == reversed);
  assert(std::equal(secret.begin(), secret.end(), hex(SHARED_SECRET).begin()));
  PublicKey low_order{};
  low_order[0] = 1;
  assert(!identities_a.shared_secret(personal, low_order, invalid));
  RoomProtocol protocol_a(crypto_a, identities_a), protocol_b(crypto_b, identities_b);
  MemoryPosts posts_a, posts_b;
  FakeTransport transport_a, transport_b;
  FakeRandom random_a, random_b;
  FakeStatus status_a, status_b;
  RoomService a(protocol_a, transport_a, posts_a, random_a, status_a),
      b(protocol_b, transport_b, posts_b, random_b, status_b);
  assert(a.configure(personal, "personal", "test-pass-1234"));
  assert(b.configure(building, "building", "building-pass"));
  Frame advert;
  assert(a.advert(EPOCH, advert));
  const auto expected_advert = hex(ROOM_ADVERT);
  assert(advert.size == expected_advert.size() &&
         std::equal(expected_advert.begin(), expected_advert.end(), advert.bytes.begin()));
  Frame flood_advert;
  assert(a.advert(EPOCH, flood_advert, true));
  PacketView direct_view, flood_view;
  assert(parse_packet(advert.bytes.data(), advert.size, direct_view));
  assert(parse_packet(flood_advert.bytes.data(), flood_advert.size, flood_view));
  assert(direct_view.route == 2 && flood_view.route == 1);
  assert(direct_view.type == 4 && flood_view.type == 4 && direct_view.path.size() == 0 && flood_view.path.size() == 0 &&
         flood_view.path.descriptor == 0x40);
  assert(advert.size == flood_advert.size &&
         std::equal(advert.bytes.begin() + 1, advert.bytes.begin() + advert.size, flood_advert.bytes.begin() + 1));
  const auto scope_key = hex(ROOM_SCOPE_NYC_KEY), expected_scoped = hex(ROOM_ADVERT_NYC);
  FloodScope scope, invalid_scope;
  assert(make_flood_scope(scope_key.data(), scope_key.size(), scope));
  Frame scoped_advert;
  assert(a.advert(EPOCH, scoped_advert, true, &scope));
  assert(scoped_advert.size == expected_scoped.size() &&
         std::equal(expected_scoped.begin(), expected_scoped.end(), scoped_advert.bytes.begin()));
  PacketView scoped_view;
  assert(parse_packet(scoped_advert.bytes.data(), scoped_advert.size, scoped_view));
  assert(scoped_view.route == 0 && scoped_view.type == 4 && scoped_view.path.size() == 0);
  assert(scoped_advert.bytes[3] == 0 && scoped_advert.bytes[4] == 0);
  assert(scoped_view.payload_size == flood_view.payload_size &&
         std::equal(flood_view.payload, flood_view.payload + flood_view.payload_size, scoped_view.payload));
  assert(!a.advert(EPOCH, scoped_advert, false, &scope) && scoped_advert.size == 0);
  assert(!a.advert(EPOCH, scoped_advert, true, &invalid_scope) && scoped_advert.size == 0);
  const auto login = hex(ROOM_LOGIN);
  const auto wrong_password = hex(ROOM_WRONG_PASSWORD);
  receive(a, wrong_password.data(), wrong_password.size(), 500, EPOCH + 1);
  assert(a.clients() == 0 && transport_a.sent.empty());
  assert(a.login_diagnostics().password_failures == 1 && a.login_diagnostics().crypto_failures == 0);
  transport_a.full = true;
  receive(a, login.data(), login.size(), 1000, EPOCH + 1);
  assert(a.clients() == 0);  // Backpressure must not create an unacknowledged session.
  transport_a.full = false;
  receive(a, login.data(), login.size(), 2000, EPOCH + 1);
  receive(b, login.data(), login.size(), 2000, EPOCH + 1);
  assert(a.clients() == 1 && a.logins() == 1 && b.clients() == 0 && transport_b.sent.empty());
  assert(transport_a.sent.size() == 1 && transport_a.sent[0].not_before == 2300);
  PacketView packet;
  Plaintext response;
  const auto reply = transport_a.sent.back().frame;
  assert(parse_packet(reply.bytes.data(), reply.size, packet) && packet.type == 8 && packet.flood());
  assert(protocol_b.open_peer(packet, secret, response));
  assert(response.bytes[0] == 0x40 && response.bytes[1] == 1 && response.bytes[9] == 2);  // PATH + login permissions.
  // Stock companion onContactResponse accepts response[4] == 0, reads legacy
  // permission at [6], ACL permission at [7], and firmware level at [12].
  // A PATH envelope adds its descriptor and extra type before those 13 bytes.
  assert(read_u32(response.bytes.data() + 2) == EPOCH + 1);
  assert(response.bytes[6] == 0 && response.bytes[7] == 0 && response.bytes[8] == 0 && response.bytes[14] == 1);
  assert(a.login_diagnostics().replies_sent == 0);  // Queue admission is not transmission or app confirmation.
  a.transmitted(transport_a.sent.back().token, true, 2500);
  a.transmitted(transport_a.sent.back().token, true, 2501);  // A repeated callback is not a second response.
  assert(a.login_diagnostics().replies_sent == 1 && a.login_diagnostics().reply_failures == 0);
  receive(a, login.data(), login.size(), 3000, EPOCH + 2);
  assert(a.logins() == 1);  // Replayed login timestamp.

  Frame frame;
  uint8_t returned_path[] = {0x40, 0xff, 1, 2, 3, 4};
  assert(protocol_b.datagram(8, client, personal.public_key, secret, returned_path, sizeof(returned_path), false,
                             PacketPath{}, frame));
  receive(a, frame, 3500, EPOCH + 2);
  std::array<uint8_t, 32> post{};
  write_u32(post.data(), EPOCH + 3);
  std::memcpy(post.data() + 5, "hello personal", 14);
  assert(protocol_b.datagram(2, client, personal.public_key, secret, post.data(), 19, false, PacketPath{}, frame));
  auto valid_post = frame;
  // Direct packets with remaining nonlocal hops cannot be delivered to the room.
  frame.bytes[1] = 0x41;
  std::move_backward(frame.bytes.begin() + 2, frame.bytes.begin() + frame.size, frame.bytes.begin() + frame.size + 2);
  frame.bytes[2] = 0x44;
  frame.bytes[3] = 0x55;
  frame.size += 2;
  receive(a, frame, 4000, EPOCH + 3);
  assert(a.posts() == 0);
  posts_a.fail = true;
  const auto before_failed_write = transport_a.sent.size();
  receive(a, valid_post, 4100, EPOCH + 3);
  assert(a.posts() == 0 && transport_a.sent.size() == before_failed_write);
  posts_a.fail = false;
  receive(a, valid_post, 4200, EPOCH + 3);
  assert(a.posts() == 1 && a.received_posts() == 1 && b.posts() == 0);
  assert(std::string(posts_a.posts[0].text.data()) == "hello personal");
  receive(a, valid_post, 4300, EPOCH + 3);
  assert(a.posts() == 1 && a.received_posts() == 1);  // Retries ACK but do not store twice.
  auto post_ack = transport_a.sent.back().frame;
  std::array<uint8_t, 4> expected_ack;
  assert(protocol_b.ack_token(post.data(), 19, client.public_key, expected_ack));
  assert(parse_packet(post_ack.bytes.data(), post_ack.size, packet) && packet.type == 3 && !packet.flood());
  assert(std::equal(expected_ack.begin(), expected_ack.end(), packet.payload));

  assert(a.add_system_post("personal is online", EPOCH + 4));
  transport_a.sent.clear();
  for (uint64_t now = 5000; now < 7000 && transport_a.sent.empty(); now += 200)
    a.tick(now, EPOCH + 10);
  assert(transport_a.sent.size() == 1);
  const auto push = transport_a.sent.back();
  const auto expected_push = hex(ROOM_HISTORY_PUSH);
  assert(push.frame.size == expected_push.size() &&
         std::equal(expected_push.begin(), expected_push.end(), push.frame.bytes.begin()));
  assert(parse_packet(push.frame.bytes.data(), push.frame.size, packet) && packet.type == 2);
  assert(protocol_b.open_peer(packet, secret, response) && (response.bytes[4] >> 2) == 2);
  assert(std::equal(personal.public_key.begin(), personal.public_key.begin() + 4, response.bytes.begin() + 5));
  assert(std::string(reinterpret_cast<char *>(response.bytes.data() + 9)) == "personal is online");
  assert(protocol_b.ack_token(response.bytes.data(), 9 + 18, client.public_key, expected_ack));
  assert(std::equal(expected_ack.begin(), expected_ack.end(), hex(ROOM_HISTORY_ACK).begin()));
  assert(protocol_b.ack(expected_ack.data(), false, PacketPath{}, frame));
  receive(a, frame, 7000, EPOCH + 11);
  assert(a.delivered_posts() == 0);  // Queue admission alone must not arm ACK acceptance.
  a.transmitted(push.token, true, 7500);
  receive(a, frame, 7600, EPOCH + 11);
  assert(a.delivered_posts() == 1 && b.delivered_posts() == 0);

  // A second stock-compatible client joins after posts exist, then both clients
  // exchange new posts. Only the stock SIGNED_PLAIN=2 branch displays/ACKs these.
  LocalIdentity second_client;
  SharedSecret second_secret;
  assert(identities_b.derive(hex(SECOND_CLIENT_SEED).data(), second_client));
  assert(identities_b.shared_secret(second_client, personal.public_key, second_secret));
  assert(second_client.public_key[0] != client.public_key[0]);
  size_t processed = transport_a.sent.size();
  const auto second_login = hex(SECOND_CLIENT_LOGIN);
  receive(a, second_login.data(), second_login.size(), 9000, EPOCH + 21);
  assert(a.clients() == 2);
  assert(protocol_b.datagram(8, second_client, personal.public_key, second_secret, returned_path, sizeof(returned_path),
                             false, PacketPath{}, frame));
  receive(a, frame, 9500, EPOCH + 21);
  std::vector<std::string> first_received, second_received;
  auto deliver = [&](uint64_t now) {
    while (processed < transport_a.sent.size()) {
      const auto sent = transport_a.sent[processed++];
      PacketView received;
      assert(parse_packet(sent.frame.bytes.data(), sent.frame.size, received));
      if (received.type != 2)
        continue;
      const bool second = received.payload[0] == second_client.public_key[0];
      const auto &recipient = second ? second_client : client;
      Plaintext decoded;
      assert(protocol_b.open_peer(received, second ? second_secret : secret, decoded));
      assert((decoded.bytes[4] >> 2) == 2);  // Literal pinned stock value, independent of our enum.
      (second ? second_received : first_received)
          .emplace_back(reinterpret_cast<const char *>(decoded.bytes.data() + 9));
      std::array<uint8_t, 4> token;
      assert(protocol_b.ack_token(decoded.bytes.data(),
                                  9 + std::strlen(reinterpret_cast<const char *>(decoded.bytes.data() + 9)),
                                  recipient.public_key, token));
      Frame ack_frame;
      assert(protocol_b.ack(token.data(), false, PacketPath{}, ack_frame));
      a.transmitted(sent.token, true, now + 20);
      receive(a, ack_frame, now + 30, EPOCH + 40);
    }
  };
  for (uint64_t now = 11000; now < 20000; now += 150) {
    a.tick(now, EPOCH + 40);
    deliver(now);
  }
  assert((second_received == std::vector<std::string>{"hello personal", "personal is online"}));
  assert(first_received.empty());  // Own post is excluded; acknowledged history is not replayed.
  auto peer_post = [&](const LocalIdentity &author, const SharedSecret &shared, const char *text, uint64_t now,
                       uint32_t timestamp) {
    std::array<uint8_t, 80> plain{};
    write_u32(plain.data(), timestamp);
    std::memcpy(plain.data() + 5, text, std::strlen(text));
    Frame post_frame;
    assert(protocol_b.datagram(2, author, personal.public_key, shared, plain.data(), 5 + std::strlen(text), false,
                               PacketPath{}, post_frame));
    receive(a, post_frame, now, timestamp);
  };
  peer_post(client, secret, "from first", 21000, EPOCH + 50);
  for (uint64_t now = 22000; now < 30000; now += 150) {
    a.tick(now, EPOCH + 57);
    deliver(now);
  }
  assert(second_received.size() == 3 && second_received.back() == "from first" && first_received.empty());
  peer_post(second_client, second_secret, "from second", 31000, EPOCH + 60);
  for (uint64_t now = 32000; now < 40000; now += 150) {
    a.tick(now, EPOCH + 67);
    deliver(now);
  }
  assert((first_received == std::vector<std::string>{"from second"}));
  assert(second_received.size() == 3 && a.delivered_posts() == 5);
  // Repository survives a fresh service instance; session state starts empty.
  RoomService reboot(protocol_a, transport_a, posts_a, random_a, status_a);
  assert(reboot.configure(personal, "renamed", "test-pass-1234"));
  assert(reboot.posts() == 4 && reboot.clients() == 0 && reboot.public_key() == a.public_key());
  assert(a.set_password("test-pass-1234") && a.clients() == 2);  // Idempotent policy apply.
  assert(!a.set_password("too-long-password") && a.clients() == 2);
  assert(a.set_password("rotated-pass-1") && a.clients() == 0 && a.posts() == 4);
  receive(a, login.data(), login.size(), 50000, EPOCH + 70);
  assert(a.clients() == 0);  // Old password no longer opens a session.
  for (size_t n = 0; n < login.size(); ++n)
    receive(b, login.data(), n, 10000 + n * 300, EPOCH + 20);
  assert(b.clients() == 0);
  std::mt19937 random(20261101);
  for (size_t i = 0; i < 10000; ++i) {
    std::vector<uint8_t> raw(random() % 300);
    for (auto &byte : raw)
      byte = static_cast<uint8_t>(random());
    receive(b, raw.data(), raw.size(), 100000 + i * 300, EPOCH + 30);
  }
  assert(b.clients() == 0 && b.posts() == 0);
  std::cout << "PASS: independent identity/ECDH/advert/login/history-push vectors, two colliding room identities, "
               "two-client history and live delivery, ACK/sync, backpressure, persistence failure, replay, bounds\n";
}
