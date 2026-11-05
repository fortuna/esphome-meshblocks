#include "node_runtime.h"
#include "radio_scheduler.h"
#include "room_vectors.h"
#include "openssl_crypto.h"
#include "../components/meshcore/ed25519_identity.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
#include <memory>
using namespace meshcore_lib;

std::vector<uint8_t> unhex(const char *text) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i < std::strlen(text); i += 2)
    out.push_back(std::stoul(std::string(text + i, 2), nullptr, 16));
  return out;
}
struct Posts final : RoomRepository {
  std::vector<RoomPost> entries;
  bool append(const RoomPost &post) override {
    this->entries.push_back(post);
    return true;
  }
  size_t count() const override { return this->entries.size(); }
  bool get(size_t i, RoomPost &out) const override {
    if (i >= this->entries.size())
      return false;
    out = this->entries[i];
    return true;
  }
  uint32_t latest_timestamp() const override { return this->entries.empty() ? 0 : this->entries.back().timestamp; }
};
struct Random final : RandomSource {
  uint8_t next{};
  bool fill(uint8_t *out, size_t size) override {
    while (size--)
      *out++ = this->next++;
    return true;
  }
};
struct Status final : NodeStatusSource {
  NodeStatus value;
  NodeStatus snapshot() const override { return this->value; }
};
struct Radio final : AsyncPacketRadio {
  std::vector<Frame> sent;
  bool active{};
  bool start(const Frame &frame, uint32_t) override {
    assert(!this->active);
    this->active = true;
    this->sent.push_back(frame);
    return true;
  }
  RadioResult poll() override {
    assert(this->active);
    this->active = false;
    return RadioResult::SENT;
  }
  bool cancel() override {
    this->active = false;
    return true;
  }
};
struct Transport final : RoomTransport {
  NodeRuntime &node;
  RadioScheduler &queue;
  uint32_t next_token{};
  bool guard_acks{true};
  Transport(NodeRuntime &n, RadioScheduler &q) : node(n), queue(q) {}
  bool send(const Frame &frame, uint64_t when, uint32_t &token) override {
    if (!this->queue.enqueue(frame, this->next_token, when))
      return false;
    token = this->next_token++;
    return true;
  }
  bool ack_available(const std::array<uint8_t, 4> &ack) const override {
    return !this->guard_acks || this->node.ack_available(ack);
  }
};
// Deliberately produce collisions to test fail-closed dispatch, independently
// of the astronomically larger search needed to manufacture test keys for them.
struct CollisionCrypto final : CryptoProvider {
  OpenSslCrypto real;
  bool fixed_ack{}, accept_all{};
  bool sha256(const uint8_t *data, size_t size, uint8_t out[32]) override {
    if (!this->fixed_ack)
      return this->real.sha256(data, size, out);
    std::fill_n(out, 32, 0x72);
    return true;
  }
  bool hmac_sha256(const uint8_t key[32], const uint8_t *data, size_t size, uint8_t out[32]) override {
    if (!this->accept_all)
      return this->real.hmac_sha256(key, data, size, out);
    std::fill_n(out, 32, 0);
    return true;
  }
  bool aes128_ecb(bool encrypt, const uint8_t key[16], const uint8_t *data, size_t size, uint8_t *out) override {
    if (!this->accept_all)
      return this->real.aes128_ecb(encrypt, key, data, size, out);
    std::copy_n(data, size, out);
    return true;
  }
};
template<size_t Capacity> struct SizedFixture {
  static constexpr uint32_t EPOCH = 1793500000;
  std::array<RoomService *, Capacity> registry{};
  std::array<ClientService *, 2> client_registry{};
  NodeRuntime node{this->registry, this->client_registry};
  Radio radio;
  std::array<TransmitJob, 4> tx_queue{};
  RadioScheduler scheduler{this->radio, this->tx_queue};
  Transport transport{this->node, this->scheduler};
  CollisionCrypto crypto;
  esphome::meshcore::Ed25519Identity identities;
  RoomProtocol protocol{this->crypto, this->identities};
  Posts personal_posts, building_posts;
  Random random_a, random_b;
  Status status;
  RoomService personal{this->protocol, this->transport, this->personal_posts, this->random_a, this->status};
  RoomService building{this->protocol, this->transport, this->building_posts, this->random_b, this->status};
  LocalIdentity a, b, client;
  SharedSecret secret_a, secret_b;
  explicit SizedFixture(RoomAccess access = RoomAccess::PRIVATE) {
    assert(this->identities.derive(unhex(ROOM_SEED).data(), this->a));
    assert(this->identities.derive(unhex(COLLIDING_ROOM_SEED).data(), this->b));
    assert(this->identities.derive(unhex(CLIENT_SEED).data(), this->client));
    assert(this->a.public_key[0] == this->b.public_key[0] && this->a.public_key != this->b.public_key);
    assert(this->identities.shared_secret(this->client, this->a.public_key, this->secret_a));
    assert(this->identities.shared_secret(this->client, this->b.public_key, this->secret_b));
    assert(this->personal.configure(this->a, "personal", "personal-pass", access));
    assert(this->building.configure(this->b, "building", "building-pass"));
    assert(this->node.add_room(this->personal) && this->node.add_room(this->building));
    assert(this->scheduler.set_send_gap(100));
  }
  Frame login(const LocalIdentity &room, const SharedSecret &secret, const char *password) {
    return this->login_from(this->client, room, secret, password, EPOCH + 1);
  }
  Frame login_from(const LocalIdentity &peer, const LocalIdentity &room, const SharedSecret &secret,
                   const char *password, uint32_t timestamp, bool flood = true) {
    Frame frame;
    frame.bytes[0] = flood ? 0x1d : 0x1e;
    frame.bytes[1] = 0x40;
    frame.bytes[2] = room.public_key[0];
    std::copy(peer.public_key.begin(), peer.public_key.end(), frame.bytes.begin() + 3);
    uint8_t plain[32]{}, mac[32];
    write_u32(plain, timestamp);
    std::copy_n(password, std::strlen(password), plain + 8);
    const size_t encrypted_size = std::strlen(password) == 0 ? 16 : 32;
    assert(this->crypto.aes128_ecb(true, secret.data(), plain, encrypted_size, frame.bytes.data() + 37));
    assert(this->crypto.hmac_sha256(secret.data(), frame.bytes.data() + 37, encrypted_size, mac));
    frame.bytes[35] = mac[0];
    frame.bytes[36] = mac[1];
    frame.size = 37 + encrypted_size;
    return frame;
  }
  void receive(const Frame &frame, uint64_t now, uint32_t epoch = EPOCH + 50) {
    PacketView packet;
    assert(parse_packet(frame.bytes.data(), frame.size, packet));
    this->node.receive(packet, now, epoch);
  }
  void drain(uint64_t now) {
    for (size_t i = 0; this->scheduler.outstanding() && i < 100; ++i, now += 100) {
      TransmitEvent event;
      if (this->scheduler.tick(now, event))
        this->node.transmitted(event.token, event.result == RadioResult::SENT, now);
    }
    assert(!this->scheduler.outstanding());
  }
  void join_both() {
    this->receive(this->login(this->a, this->secret_a, "personal-pass"), 1000);
    this->receive(this->login(this->b, this->secret_b, "building-pass"), 1500);
    this->drain(2000);
    assert(this->personal.clients() == 1 && this->building.clients() == 1);
  }
  void post(const LocalIdentity &room, const SharedSecret &secret, const char *text, uint64_t now) {
    uint8_t plain[64]{};
    write_u32(plain, EPOCH + 2);
    std::copy_n(text, std::strlen(text), plain + 5);
    Frame frame;
    assert(this->protocol.datagram(2, this->client, room.public_key, secret, plain, 5 + std::strlen(text), false,
                                   PacketPath{}, frame));
    this->receive(frame, now);
  }
  std::array<uint8_t, 4> push_ack(const Frame &frame, const SharedSecret &secret, const char *text) {
    PacketView packet;
    Plaintext plain;
    assert(parse_packet(frame.bytes.data(), frame.size, packet) && packet.type == 2);
    assert(this->protocol.open_peer(packet, secret, plain) && (plain.bytes[4] >> 2) == 2);
    assert(std::string(reinterpret_cast<char *>(plain.bytes.data() + 9)) == text);
    std::array<uint8_t, 4> ack;
    assert(this->protocol.ack_token(plain.bytes.data(), 9 + std::strlen(text), this->client.public_key, ack));
    return ack;
  }
  void acknowledge(const std::array<uint8_t, 4> &ack, uint64_t now) {
    Frame frame;
    assert(this->protocol.ack(ack.data(), false, PacketPath{}, frame));
    this->receive(frame, now);
  }
};
using Fixture = SizedFixture<2>;

void test_scaled_capacity() {
  SizedFixture<8> f;
  Fixture independent;
  std::array<Posts, 7> posts;
  std::array<Random, 7> random;
  std::array<LocalIdentity, 7> identities;
  std::array<SharedSecret, 7> secrets;
  std::array<std::unique_ptr<RoomService>, 7> extra;
  std::array<RoomService *, 8> rooms{&f.personal, &f.building};
  std::array<SharedSecret, 8> all_secrets{f.secret_a, f.secret_b};
  f.join_both();
  for (size_t i = 0; i < extra.size(); ++i) {
    std::array<uint8_t, 32> seed{};
    seed.fill(static_cast<uint8_t>(20 + i));
    assert(f.identities.derive(seed.data(), identities[i]));
    assert(f.identities.shared_secret(f.client, identities[i].public_key, secrets[i]));
    extra[i] = std::make_unique<RoomService>(f.protocol, f.transport, posts[i], random[i], f.status);
    assert(extra[i]->configure(identities[i], "extra", "another-pass"));
    if (i == 6) {
      assert(!f.node.add_room(*extra[i]));  // Ninth identity cannot overflow the supplied registry.
      break;
    }
    assert(f.node.add_room(*extra[i]));
    rooms[i + 2] = extra[i].get();
    all_secrets[i + 2] = secrets[i];
    f.receive(f.login(identities[i], secrets[i], "another-pass"), 3000 + i * 500);
    f.drain(3100 + i * 500);
    assert(extra[i]->clients() == 1 && independent.personal.clients() == 0);
  }
  assert(f.node.capacity() == 8 && f.node.rooms() == 8 && independent.node.capacity() == 2);
  // The final room receives its own post without altering any other history.
  f.post(identities[5], secrets[5], "last room only", 7000);
  f.drain(7100);
  for (size_t i = 0; i < rooms.size(); ++i) {
    assert(rooms[i]->posts() == (i == 7 ? 1 : 0));
    assert(rooms[i]->add_system_post("shared schedule", Fixture::EPOCH + 51));
  }
  const auto before = f.radio.sent.size();
  for (size_t i = 0; i < rooms.size(); ++i) {
    const uint64_t now = 10000 + i * NodeRuntime::PUSH_INTERVAL_MS;
    f.node.tick(now, Fixture::EPOCH + 60);
    f.node.tick(now + 1, Fixture::EPOCH + 60);
    assert(f.scheduler.outstanding() == 1);  // More rooms never multiply admission/airtime.
    f.drain(now + 10);
    const auto ack = f.push_ack(f.radio.sent.back(), all_secrets[i], "shared schedule");
    f.acknowledge(ack, now + 300);
    assert(rooms[i]->delivered_posts() == 1);
  }
  assert(f.radio.sent.size() == before + 8 && independent.personal.delivered_posts() == 0);
  std::array<RoomService *, 0> empty{};
  NodeRuntime channel_only{empty};
  assert(!channel_only.add_room(f.personal) && channel_only.capacity() == 0);
  channel_only.tick(50000, Fixture::EPOCH + 100);
  std::array<RoomService *, 1> one{};
  NodeRuntime single{one};
  assert(single.add_room(f.personal) && !single.add_room(f.building));
}

void test_open_room() {
  SizedFixture<3> f;
  Posts public_posts;
  Random random;
  RoomService public_room(f.protocol, f.transport, public_posts, random, f.status);
  std::array<uint8_t, 32> seed{};
  seed.fill(23);
  LocalIdentity public_identity;
  SharedSecret public_secret;
  assert(f.identities.derive(seed.data(), public_identity));
  assert(f.identities.shared_secret(f.client, public_identity.public_key, public_secret));
  assert(!public_room.configure(public_identity, "Test - Private", ""));
  assert(!public_room.configure(public_identity, "Test - Public", "retained-pass", static_cast<RoomAccess>(99)));
  assert(public_room.configure(public_identity, "Test - Public", "retained-pass", RoomAccess::PUBLIC));
  assert(f.node.add_room(public_room));
  const auto login = f.login(public_identity, public_secret, "");
  auto corrupt = login;
  corrupt.bytes[corrupt.size - 1] ^= 0x40;
  f.receive(corrupt, 1000);
  assert(public_room.clients() == 0);  // Open access never bypasses identity crypto.
  f.receive(login, 1500);
  assert(public_room.clients() == 1 && f.personal.clients() == 0 && f.building.clients() == 0);
  f.receive(f.login(f.a, f.secret_a, ""), 2000);
  f.receive(f.login(f.b, f.secret_b, ""), 2500);
  assert(f.personal.clients() == 0 && f.building.clients() == 0);  // Defaults remain protected.
  f.receive(f.login(f.a, f.secret_a, "personal-pass"), 3000);
  f.drain(3300);
  assert(f.personal.clients() == 1 && public_room.clients() == 1);
  f.receive(login, 3800);
  assert(public_room.logins() == 1 && public_room.login_diagnostics().replays == 1);
  f.receive(f.login(public_identity, public_secret, "retained-pass"), 4200);
  assert(public_room.logins() == 1 && public_room.login_diagnostics().password_failures == 1);
  assert(!public_room.set_password("changed-pass") && public_room.clients() == 1);
  f.post(public_identity, public_secret, "public only", 4700);
  f.drain(5000);
  assert(public_room.posts() == 1 && f.personal.posts() == 0 && f.building.posts() == 0);
  assert(public_room.add_system_post("open history", Fixture::EPOCH + 51));
  f.node.tick(10000, Fixture::EPOCH + 60);
  assert(f.scheduler.outstanding() == 1);
  f.drain(10010);
  f.acknowledge(f.push_ack(f.radio.sent.back(), public_secret, "open history"), 10500);
  assert(public_room.delivered_posts() == 1 && f.personal.delivered_posts() == 0);
  // Returning to the default policy at reboot uses the retained credential;
  // neither the identity nor the repository changes with the advertised name/policy.
  RoomService locked(f.protocol, f.transport, public_posts, random, f.status);
  assert(locked.configure(public_identity, "Renamed Public", "retained-pass"));
  PacketView packet;
  assert(parse_packet(login.bytes.data(), login.size, packet));
  locked.receive(packet, 20000, Fixture::EPOCH + 60);
  assert(locked.clients() == 0 && locked.posts() == 2 && locked.public_key() == public_room.public_key());
  auto password_login = f.login(public_identity, public_secret, "retained-pass");
  assert(parse_packet(password_login.bytes.data(), password_login.size, packet));
  locked.receive(packet, 20500, Fixture::EPOCH + 60);
  assert(locked.clients() == 1);
}

// Exercise eviction through real encoded frames and the shared runtime/scheduler;
// membership is observed by accepted status requests, not private slot inspection.
struct SessionFixture : Fixture {
  std::array<LocalIdentity, 5> peers;
  std::array<SharedSecret, 5> secrets;
  const char *member_password;
  explicit SessionFixture(RoomAccess access = RoomAccess::PRIVATE)
      : Fixture(access), member_password(access == RoomAccess::PUBLIC ? "" : "personal-pass") {
    this->peers[0] = this->client;
    for (size_t i = 1; i < this->peers.size(); ++i) {
      std::array<uint8_t, 32> seed{};
      seed.fill(static_cast<uint8_t>(80 + i));
      while (!this->identities.derive(seed.data(), this->peers[i]))
        ++seed[0];
    }
    for (size_t i = 0; i < this->peers.size(); ++i)
      assert(this->identities.shared_secret(this->peers[i], this->a.public_key, this->secrets[i]));
  }
  Frame login_peer(size_t i, uint32_t timestamp = EPOCH + 1, bool flood = true, const char *password = nullptr) {
    return this->login_from(this->peers[i], this->a, this->secrets[i], password ? password : this->member_password,
                            timestamp, flood);
  }
  void fill(uint64_t base = 0) {
    for (size_t i = 0; i < 4; ++i) {
      // Wall time deliberately disagrees with activity order.
      this->receive(this->login_peer(i), base + 1000 * (i + 1), EPOCH + 1000 - i);
      this->drain(base + 1000 * (i + 1) + 400);
    }
    assert(this->personal.clients() == 4);
  }
  void input(size_t peer, uint8_t type, const uint8_t *plain, size_t size, uint64_t now) {
    Frame frame;
    assert(this->protocol.datagram(type, this->peers[peer], this->a.public_key, this->secrets[peer], plain, size, false,
                                   PacketPath{}, frame));
    this->receive(frame, now, EPOCH + 50);
  }
  bool status(size_t peer, uint64_t now, uint32_t timestamp = EPOCH + 100) {
    uint8_t plain[5]{};
    write_u32(plain, timestamp);
    plain[4] = 1;
    const auto before = this->personal.login_diagnostics().status_requests;
    this->input(peer, 0, plain, sizeof(plain), now);
    const bool accepted = this->personal.login_diagnostics().status_requests == before + 1;
    this->drain(now + 400);
    return accepted;
  }
};

void test_least_active_eviction() {
  // Every supported inbound activity keeps a member ahead of an idle member.
  // Include 32-bit uptime rollover and backward UTC movement.
  for (auto access : {RoomAccess::PRIVATE, RoomAccess::PUBLIC})
    for (int activity = 0; activity < 7; ++activity) {
      SessionFixture f(access);
      const uint64_t base = (1ULL << 32) - 4500;
      f.fill(base);
      uint8_t plain[16]{};
      write_u32(plain, Fixture::EPOCH + 2);
      if (activity == 0) {
        f.receive(f.login_peer(0, Fixture::EPOCH + 2), base + 7000);
      } else if (activity == 1) {
        plain[5] = 'x';
        f.input(0, 2, plain, 6, base + 7000);
      } else if (activity == 2) {
        assert(f.status(0, base + 7000));
      } else if (activity == 3) {
        plain[4] = 2;
        f.input(0, 0, plain, 9, base + 7000);
      } else if (activity == 4) {
        const uint8_t path[] = {0x40, 0xff};
        f.input(0, 8, path, sizeof(path), base + 7000);
      } else {
        assert(f.personal.add_system_post("history", Fixture::EPOCH + 2));
        f.node.tick(base + 6000, Fixture::EPOCH + 50);
        f.drain(base + 6100);
        const auto ack = f.push_ack(f.radio.sent.back(), f.secrets[0], "history");
        if (activity == 5)
          f.acknowledge(ack, base + 7000);
        else {
          uint8_t path_ack[6] = {0x40, 3};
          std::copy(ack.begin(), ack.end(), path_ack + 2);
          f.input(0, 8, path_ack, sizeof(path_ack), base + 7000);
        }
        assert(f.personal.delivered_posts() == 1);
      }
      f.drain(base + 7400);
      f.receive(f.login_peer(4), base + 8000);
      f.drain(base + 8400);
      assert(f.personal.clients() == 4 && f.personal.login_diagnostics().evictions == 1);
      assert(!f.status(1, base + 9000));  // Idle #1, not oldest-created #0, was removed.
      assert(f.status(0, base + 10000));
      assert(f.status(2, base + 11000) && f.status(3, base + 12000) && f.status(4, base + 13000));
      // Eviction leaves retained history intact and does not touch another room.
      assert(f.personal.posts() == (activity == 1 || activity >= 5 ? 1 : 0));
      assert(f.building.clients() == 0 && f.building.login_diagnostics().evictions == 0);
    }
}

void test_eviction_admission_and_route() {
  SessionFixture f;
  f.fill();
  // Oldest member has a route; mark others active afterward so it stays oldest.
  const uint8_t path[] = {0x41, 0x12, 0x34, 0xff};
  f.input(0, 8, path, sizeof(path), 5000);
  for (size_t i = 1; i < 4; ++i)
    assert(f.status(i, 5000 + 1000 * i));
  f.receive(f.login_peer(4, Fixture::EPOCH + 1, true, "incorrect-pass"), 9000);
  auto corrupt = f.login_peer(4);
  corrupt.bytes[corrupt.size - 1] ^= 0x40;
  f.receive(corrupt, 9500);
  f.receive(f.login_peer(0), 10000);  // Replayed login must not refresh oldest member.
  uint8_t malformed[6]{};
  write_u32(malformed, Fixture::EPOCH + 2);
  malformed[4] = 0x0c;
  malformed[5] = 'x';  // Unsupported CLI text.
  f.input(0, 2, malformed, sizeof(malformed), 10500);
  assert(f.personal.logins() == 4 && f.personal.login_diagnostics().evictions == 0);
  Frame advert;
  assert(f.personal.advert(Fixture::EPOCH + 50, advert));
  uint32_t token;
  for (int i = 0; i < 4; ++i)
    assert(f.transport.send(advert, 11000, token));
  f.receive(f.login_peer(4), 11000);  // Full TX queue cannot evict the victim.
  assert(f.personal.logins() == 4 && f.personal.login_diagnostics().evictions == 0);
  f.drain(11400);
  assert(f.personal.clients() == 4 && !f.status(4, 12000));
  f.receive(f.login_peer(4, Fixture::EPOCH + 1, false), 13000);
  f.drain(13400);
  PacketView reply;
  assert(parse_packet(f.radio.sent.back().bytes.data(), f.radio.sent.back().size, reply));
  assert(reply.type == 1 && reply.flood() && reply.path.size() == 0);  // No inherited victim route.
  assert(f.personal.login_diagnostics().evictions == 1);
  assert(!f.status(0, 14000) && f.status(4, 15000));
  // The evicted member may rejoin with its identity/password; it is not banned.
  f.receive(f.login_peer(0, Fixture::EPOCH + 101), 16000);
  f.drain(16400);
  assert(f.status(0, 17000, Fixture::EPOCH + 102));
  assert(!f.status(1, 18000) && f.personal.clients() == 4);
}

void test_eviction_pending_delivery() {
  SessionFixture f, independent;
  f.receive(f.login(f.b, f.secret_b, "building-pass"), 500);
  f.drain(900);
  f.fill();
  assert(f.personal.add_system_post("old pending push", Fixture::EPOCH + 2));
  const auto before = f.radio.sent.size();
  f.node.tick(6000, Fixture::EPOCH + 50);  // Push queued for oldest member #0.
  assert(f.scheduler.outstanding() == 1);
  f.receive(f.login_peer(4), 7000);  // Replace #0 while its ciphertext is already queued.
  f.drain(7400);
  const auto ack = f.push_ack(f.radio.sent[before], f.secrets[0], "old pending push");
  assert(!f.node.ack_available(ack));  // Reservation survives session replacement.
  f.acknowledge(ack, 8000);
  assert(f.personal.delivered_posts() == 0);  // Late TX callback/ACK cannot complete new member's delivery.
  assert(!f.status(0, 9000) && f.status(4, 10000));
  assert(f.personal.posts() == 1 && f.building.clients() == 1);
  assert(f.building.login_diagnostics().evictions == 0 && independent.personal.clients() == 0);
}

int main() {
  test_least_active_eviction();
  test_eviction_admission_and_route();
  test_eviction_pending_delivery();
  test_scaled_capacity();
  test_open_room();
  Fixture f, independent;
  assert(f.node.next_deadline(0, Fixture::EPOCH) == UINT64_MAX);
  assert(f.node.add_room(f.personal) && f.node.rooms() == 2);  // Idempotent registry.
  Posts duplicate_posts;
  Random duplicate_random;
  RoomService duplicate(f.protocol, f.transport, duplicate_posts, duplicate_random, f.status);
  assert(duplicate.configure(f.a, "duplicate", "personal-pass"));
  std::array<RoomService *, 2> duplicate_registry{};
  NodeRuntime duplicate_guard{duplicate_registry};
  assert(duplicate_guard.add_room(f.personal) && !duplicate_guard.add_room(duplicate));
  assert(!f.node.identity_available(f.a.public_key) && f.node.identity_available(f.a.public_key, &f.personal));
  f.receive(f.login(f.a, f.secret_a, "personal-pass"), 1000);
  assert(f.node.next_deadline(1000, Fixture::EPOCH) == 3000);
  assert(f.node.next_deadline(1000, 0) == UINT64_MAX);  // No usable protocol time.
  assert(independent.node.next_deadline(1000, Fixture::EPOCH) == UINT64_MAX);
  assert(f.personal.clients() == 1 && f.building.clients() == 0 && independent.personal.clients() == 0);
  f.receive(f.login(f.b, f.secret_b, "personal-pass"), 1500);  // Valid ciphertext, wrong room password.
  assert(f.building.clients() == 0 && f.personal.logins() == 1);
  f.receive(f.login(f.b, f.secret_b, "building-pass"), 2000);
  assert(f.node.next_deadline(2000, Fixture::EPOCH) == 3000);  // Earliest room wins.
  f.drain(2500);
  assert(f.building.clients() == 1);
  f.post(f.a, f.secret_a, "personal only", 4000);
  assert(f.personal.posts() == 1 && f.building.posts() == 0);
  f.post(f.b, f.secret_b, "building only", 4500);
  assert(f.personal.posts() == 1 && f.building.posts() == 1 && independent.personal.posts() == 0);
  assert(std::string(f.personal_posts.entries[0].text.data()) == "personal only");
  assert(std::string(f.building_posts.entries[0].text.data()) == "building only");
  f.drain(5000);
  assert(f.personal.add_system_post("personal history", Fixture::EPOCH + 51));
  assert(f.building.add_system_post("building history", Fixture::EPOCH + 51));
  const auto sent_before = f.radio.sent.size();
  f.node.tick(10000, Fixture::EPOCH + 60);
  assert(f.scheduler.outstanding() == 1);  // One global push interval, not one allowance per room.
  assert(f.node.next_deadline(10000, Fixture::EPOCH + 60) == 10150);
  f.node.tick(10001, Fixture::EPOCH + 60);
  assert(f.scheduler.outstanding() == 1);
  f.drain(10010);
  assert(f.radio.sent.size() == sent_before + 1);
  const auto ack_a = f.push_ack(f.radio.sent.back(), f.secret_a, "personal history");
  f.node.tick(11200, Fixture::EPOCH + 60);
  f.drain(11210);
  const auto ack_b = f.push_ack(f.radio.sent.back(), f.secret_b, "building history");
  assert(ack_a != ack_b);
  // A matching raw ACK still belongs to transit while it has an unconsumed hop.
  PacketPath transit_path;
  transit_path.descriptor = 0x41;
  transit_path.bytes[0] = 0x12;
  transit_path.bytes[1] = 0x34;
  Frame transit_ack;
  assert(f.protocol.ack(ack_b.data(), false, transit_path, transit_ack));
  f.receive(transit_ack, 11900);
  assert(f.building.delivered_posts() == 0 && f.personal.delivered_posts() == 0);
  f.acknowledge(ack_b, 12000);
  assert(f.building.delivered_posts() == 1 && f.personal.delivered_posts() == 0);
  f.acknowledge(ack_a, 12100);
  assert(f.personal.delivered_posts() == 1 && independent.personal.delivered_posts() == 0);
  // Multiple services are still constrained by the same physical four-frame queue.
  Frame packet;
  assert(f.personal.advert(Fixture::EPOCH + 70, packet));
  uint32_t token;
  for (int i = 0; i < 4; ++i)
    assert(f.transport.send(packet, 13000, token));
  assert(!f.transport.send(packet, 13000, token) && f.scheduler.outstanding() == 4);
  f.drain(13000);

  // MC1 performs a status request after login before returning from joinRoom.
  // Answer the upstream 52-byte room layout with the request tag and the correct
  // local identity, including when the two local public-key prefixes collide.
  f.status.value.packets_received = 123;
  f.status.value.packets_sent = 45;
  f.status.value.last_rssi = -87;
  f.status.value.last_snr_quarters = -14;
  f.status.value.uptime_seconds = 99;
  assert(f.personal.add_system_post("personal extra", Fixture::EPOCH + 70));
  uint8_t status_request[5]{};
  write_u32(status_request, Fixture::EPOCH + 80);
  status_request[4] = 1;
  uint8_t reciprocal[] = {0x40, 0xff, 1, 2, 3, 4};
  assert(f.protocol.datagram(8, f.client, f.a.public_key, f.secret_a, reciprocal, sizeof(reciprocal), false,
                             PacketPath{}, packet));
  f.receive(packet, 14000);
  assert(f.protocol.datagram(0, f.client, f.a.public_key, f.secret_a, status_request, sizeof(status_request), false,
                             PacketPath{}, packet));
  f.receive(packet, 14100);
  f.drain(14500);
  PacketView status_packet;
  Plaintext status_plain;
  const auto status_frame = f.radio.sent.back();
  assert(parse_packet(status_frame.bytes.data(), status_frame.size, status_packet));
  assert(status_packet.type == 1 && !status_packet.flood());
  assert(f.protocol.open_peer(status_packet, f.secret_a, status_plain));
  assert(status_plain.size == 64 && read_u32(status_plain.bytes.data()) == Fixture::EPOCH + 80);
  assert(read_u32(status_plain.bytes.data() + 12) == 123 && read_u32(status_plain.bytes.data() + 16) == 45);
  assert(status_plain.bytes[10] == 0xa9 && status_plain.bytes[11] == 0xff);  // -87 RSSI, little endian.
  assert(status_plain.bytes[46] == 0xf2 && status_plain.bytes[47] == 0xff);  // -3.5 dB, x4.
  assert(status_plain.bytes[52] == 3 && status_plain.bytes[54] == 1);        // This room's posts/pushes since boot.
  Plaintext wrong_room;
  assert(!f.protocol.open_peer(status_packet, f.secret_b, wrong_room));
  assert(f.personal.login_diagnostics().status_requests == 1 && f.building.login_diagnostics().status_requests == 0);
  // Flood status replies carry the observed path and the same tagged response.
  assert(f.protocol.datagram(0, f.client, f.b.public_key, f.secret_b, status_request, sizeof(status_request), true,
                             transit_path, packet));
  f.receive(packet, 15000);
  f.drain(15400);
  const auto flooded_status = f.radio.sent.back();
  assert(parse_packet(flooded_status.bytes.data(), flooded_status.size, status_packet));
  assert(status_packet.type == 8 && status_packet.flood() &&
         f.protocol.open_peer(status_packet, f.secret_b, status_plain));
  assert(status_plain.bytes[0] == 0x41 && status_plain.bytes[1] == 0x12 && status_plain.bytes[2] == 0x34 &&
         status_plain.bytes[3] == 1);
  assert(read_u32(status_plain.bytes.data() + 4) == Fixture::EPOCH + 80 && status_plain.bytes[4 + 52] == 2);
  assert(f.building.login_diagnostics().status_replies == 1 &&
         independent.building.login_diagnostics().status_requests == 0);
  // No status leakage to an unauthenticated peer, or for transit, or through a full queue.
  auto damaged = packet;
  damaged.bytes[damaged.size - 1] ^= 0x40;
  f.receive(damaged, 16000);
  assert(f.scheduler.outstanding() == 0 && f.building.login_diagnostics().status_requests == 1);
  assert(f.protocol.datagram(0, f.client, f.b.public_key, f.secret_b, status_request, sizeof(status_request), false,
                             transit_path, packet));
  f.receive(packet, 16100);
  assert(f.scheduler.outstanding() == 0);
  for (int i = 0; i < 4; ++i)
    assert(f.transport.send(packet, 17000, token));
  assert(f.protocol.datagram(0, f.client, f.b.public_key, f.secret_b, status_request, sizeof(status_request), false,
                             PacketPath{}, packet));
  f.receive(packet, 16200);
  assert(f.scheduler.outstanding() == 4 && f.building.login_diagnostics().status_replies == 1);
  f.drain(17000);
  f.receive(packet, 18000);  // A read-only request retry may use the same timestamp.
  assert(f.scheduler.outstanding() == 1 && f.building.login_diagnostics().status_replies == 2);
  f.drain(18400);

  Fixture collision;
  collision.join_both();
  collision.crypto.fixed_ack = true;
  assert(collision.personal.add_system_post("one", Fixture::EPOCH + 2));
  assert(collision.building.add_system_post("two", Fixture::EPOCH + 2));
  collision.node.tick(5000, Fixture::EPOCH + 50);
  collision.drain(5000);
  const auto first_push_count = collision.radio.sent.size();
  collision.node.tick(6200, Fixture::EPOCH + 50);
  assert(collision.scheduler.outstanding() == 0 && collision.radio.sent.size() == first_push_count);
  std::array<uint8_t, 4> same_ack{0x72, 0x72, 0x72, 0x72};
  assert(!collision.node.ack_available(same_ack));
  // Simulate a faulty transport bypassing admission: raw ambiguous ACK must still
  // not advance either room. Encrypted PATH ACK remains bound to its room.
  collision.transport.guard_acks = false;
  for (uint64_t now = 7000; now <= 7600; now += 200)
    collision.node.tick(now, Fixture::EPOCH + 50);
  assert(collision.scheduler.outstanding() == 1);
  collision.drain(7700);
  collision.acknowledge(same_ack, 8000);
  assert(collision.node.ambiguous_acks() == 1);
  assert(collision.personal.delivered_posts() == 0 && collision.building.delivered_posts() == 0);
  uint8_t path_ack[] = {0x40, 3, 0x72, 0x72, 0x72, 0x72};
  assert(collision.protocol.datagram(8, collision.client, collision.a.public_key, collision.secret_a, path_ack,
                                     sizeof(path_ack), false, PacketPath{}, packet));
  collision.receive(packet, 8100);
  assert(collision.personal.delivered_posts() == 1 && collision.building.delivered_posts() == 0);
  collision.acknowledge(same_ack, 8200);
  assert(collision.building.delivered_posts() == 1);
  assert(!collision.node.ack_available(same_ack));  // A delayed duplicate cannot complete a new room's push.
  collision.node.tick(5001 + NodeRuntime::ACK_REUSE_GUARD_MS, Fixture::EPOCH + 250);
  assert(collision.node.ack_available(same_ack));

  Fixture ambiguous;
  ambiguous.crypto.accept_all = true;
  ambiguous.receive(ambiguous.login(ambiguous.a, ambiguous.secret_a, "personal-pass"), 1000);
  assert(ambiguous.node.ambiguous_packets() == 1);
  assert(ambiguous.personal.clients() == 0 && ambiguous.building.clients() == 0 &&
         ambiguous.scheduler.outstanding() == 0);
  // Even differing per-room throttle histories cannot bypass the global ambiguity check.
  auto direct = ambiguous.login(ambiguous.a, ambiguous.secret_a, "building-pass");
  PacketView view;
  assert(parse_packet(direct.bytes.data(), direct.size, view));
  ambiguous.building.receive(view, 1400, Fixture::EPOCH + 50);
  ambiguous.receive(ambiguous.login(ambiguous.a, ambiguous.secret_a, "personal-pass"), 1500);
  assert(ambiguous.node.ambiguous_packets() == 2 && ambiguous.personal.clients() == 0);
  // Chat and room identities share full-key uniqueness and short-ACK admission.
  Fixture mixed;
  ClientService duplicate_room(mixed.protocol, mixed.identities, mixed.transport);
  assert(duplicate_room.configure(mixed.a) && !mixed.node.add_client(duplicate_room));
  ClientService outgoing(mixed.protocol, mixed.identities, mixed.transport);
  assert(outgoing.configure(mixed.client) && mixed.node.add_client(outgoing));
  assert(!mixed.node.identity_available(mixed.client.public_key));
  mixed.crypto.fixed_ack = true;
  assert(outgoing.send_client_message(mixed.a.public_key, "Outbound", Fixture::EPOCH, 2, 0));
  assert(!mixed.node.ack_available(outgoing.pending_ack()));
  mixed.drain(1);
  mixed.join_both();
  assert(mixed.personal.add_system_post("Room history", Fixture::EPOCH + 10));
  mixed.node.tick(5000, Fixture::EPOCH + 50);
  assert(mixed.scheduler.outstanding() == 0);  // Room push cannot reuse the client's token.
  mixed.acknowledge(outgoing.pending_ack(), 5001);
  assert(outgoing.send_state() == ClientSendState::ACKNOWLEDGED);
  assert(mixed.personal.delivered_posts() == 0 && !mixed.node.ack_available(outgoing.pending_ack()));

  std::cout << "PASS: least-active eviction/activity/admission/route/late-ACK isolation, explicit open-room "
               "login/post/history isolation and protected defaults, zero/one/two/eight-room capacities, eight-room "
               "delivery/pacing, two-room prefix collisions, credentials/history isolation, all-candidate "
               "authentication, shared pacing/queue, ACK ownership/collisions, independent runtimes\n";
}
