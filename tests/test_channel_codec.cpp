#include "channel_codec.h"
#include "wire_vectors.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cassert>
#include <iostream>
#include <random>
#include <string>
#include <vector>

class OpenSslCrypto final : public meshcore_lib::CryptoProvider {
 public:
  bool fail_hmac{};
  bool sha256(const uint8_t *data, size_t size, uint8_t out[32]) override { return SHA256(data, size, out) != nullptr; }
  bool hmac_sha256(const uint8_t key[32], const uint8_t *data, size_t size, uint8_t out[32]) override {
    if (this->fail_hmac)
      return false;
    unsigned length = 0;
    return HMAC(EVP_sha256(), key, 32, data, size, out, &length) != nullptr && length == 32;
  }
  bool aes128_ecb(bool encrypt, const uint8_t key[16], const uint8_t *data, size_t size, uint8_t *out) override {
    auto *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
      return false;
    int length = 0, final_length = 0;
    bool ok = EVP_CipherInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr, encrypt) == 1 &&
              EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
              EVP_CipherUpdate(ctx, out, &length, data, static_cast<int>(size)) == 1 &&
              EVP_CipherFinal_ex(ctx, out + length, &final_length) == 1 &&
              static_cast<size_t>(length + final_length) == size;
    EVP_CIPHER_CTX_free(ctx);
    return ok;
  }
};

std::vector<uint8_t> hex(const char *source) {
  std::string value(source);
  std::vector<uint8_t> result;
  for (size_t i = 0; i < value.size(); i += 2)
    result.push_back(std::stoul(value.substr(i, 2), nullptr, 16));
  return result;
}

int main() {
  using namespace meshcore_lib;
  OpenSslCrypto crypto_a, crypto_b;
  ChannelTextCodec codec_a(crypto_a), codec_b(crypto_b);
  const auto testing = hex(TESTING_KEY);
  ChannelKey key_a, key_b;
  assert(codec_a.make_key(testing.data(), testing.size(), key_a));
  assert(codec_b.make_key(testing.data(), testing.size(), key_b));
  auto reference = hex(TESTING_FRAME);
  ChannelMessage message, other;
  assert(codec_a.decode(reference.data(), reference.size(), key_a, message) == DecodeResult::OK);
  assert(message.timestamp == 1793491200 && message.path_hash_size == 2 && message.hops == 0);
  assert(std::string(message.text.data()) == "Peer: probe-peer-001");
  assert(message.sender() == "Peer" && message.body() == "probe-peer-001");
  // Automation values own their payload even after another packet reuses RX.
  const auto saved_event = message;
  message = {};
  assert(saved_event.sender() == "Peer" && saved_event.body() == "probe-peer-001");
  message = saved_event;
  Frame encoded;
  assert(codec_a.encode(key_a, 2, 1793491200, "Peer", "probe-peer-001", encoded));
  assert(encoded.size == reference.size() && std::equal(reference.begin(), reference.end(), encoded.bytes.begin()));
  const auto original_fingerprint = message.fingerprint;
  // Caller-selected timestamps survive encoding exactly, including backward
  // choices. Reusing the same timestamp/text produces a duplicate fingerprint.
  for (uint32_t chosen : {1800000000U, 1U, 1793491200U, UINT32_MAX}) {
    Frame selected, repeated;
    ChannelMessage decoded, duplicate;
    assert(codec_a.encode(key_a, 2, chosen, "Peer", "pong", selected));
    assert(codec_a.decode(selected.bytes.data(), selected.size, key_a, decoded) == DecodeResult::OK);
    assert(decoded.timestamp == chosen);
    assert(codec_a.encode(key_a, 2, chosen, "Peer", "pong", repeated));
    assert(codec_a.decode(repeated.bytes.data(), repeated.size, key_a, duplicate) == DecodeResult::OK);
    assert(decoded.fingerprint == duplicate.fingerprint);
    if (chosen < UINT32_MAX) {
      assert(codec_a.encode(key_a, 2, chosen + 1, "Peer", "pong", repeated));
      assert(codec_a.decode(repeated.bytes.data(), repeated.size, key_a, duplicate) == DecodeResult::OK);
      assert(decoded.fingerprint != duplicate.fingerprint);
    }
  }

  // Paths are bounded and do not change the encrypted message or fingerprint.
  for (uint8_t width = 1; width <= 3; ++width) {
    auto routed = reference;
    routed[1] = ((width - 1) << 6) | 2;
    routed.insert(routed.begin() + 2, 2 * width, 0x42);
    assert(codec_a.decode(routed.data(), routed.size(), key_a, message) == DecodeResult::OK);
    assert(message.hops == 2 && message.path_hash_size == width && message.fingerprint == original_fingerprint);
  }

  // A hash match is only a candidate, never proof of the key.
  auto collision = hex(COLLIDING_KEY);
  ChannelKey colliding;
  assert(codec_a.make_key(collision.data(), collision.size(), colliding));
  assert(colliding.hash == key_a.hash);
  assert(codec_a.decode(reference.data(), reference.size(), colliding, message) == DecodeResult::BAD_MAC);
  assert(codec_a.decode(reference.data(), reference.size(), key_a, message) == DecodeResult::OK);
  auto tampered = reference;
  tampered.back() ^= 1;
  assert(codec_a.decode(tampered.data(), tampered.size(), key_a, message) == DecodeResult::BAD_MAC);
  tampered = reference;
  tampered[2] ^= 1;
  assert(codec_a.decode(tampered.data(), tampered.size(), key_a, message) == DecodeResult::WRONG_CHANNEL);

  for (size_t n = 0; n < reference.size(); ++n)
    assert(codec_a.decode(reference.data(), n, key_a, message) != DecodeResult::OK);
  for (uint8_t path : {uint8_t(0xc0), uint8_t(0x7f), uint8_t(0xbf)}) {
    tampered = reference;
    tampered[1] = path;
    assert(codec_a.decode(tampered.data(), tampered.size(), key_a, message) == DecodeResult::MALFORMED);
  }
  for (uint8_t header : {uint8_t(0x55), uint8_t(0x11), uint8_t(0x16), uint8_t(0x17)}) {
    tampered = reference;
    tampered[0] = header;
    assert(codec_a.decode(tampered.data(), tampered.size(), key_a, message) == DecodeResult::UNSUPPORTED);
  }
  assert(codec_a.decode(nullptr, 0, key_a, message) == DecodeResult::MALFORMED);

  // Scoped envelopes use the same channel crypto and fingerprint as ordinary
  // flooding, while retaining both transport codes as untrusted route metadata.
  const auto nyc_key = hex(NYC_SCOPE_KEY);
  FloodScope nyc;
  assert(codec_a.make_scope(nyc_key.data(), nyc_key.size(), nyc));
  const auto scoped = hex(NYC_SCOPED_FRAME);
  assert(codec_a.encode(key_a, 2, 1793491200, "Peer", "probe-peer-001", encoded, &nyc));
  assert(encoded.size == reference.size() + 4);
  assert(encoded.size == scoped.size() && std::equal(scoped.begin(), scoped.end(), encoded.bytes.begin()));
  assert(codec_b.decode(scoped.data(), scoped.size(), key_b, other) == DecodeResult::OK);
  assert(other.scoped && other.transport_codes[0] == (scoped[1] | (scoped[2] << 8)));
  assert(other.transport_codes[1] == 0 && other.fingerprint == original_fingerprint);
  assert(std::string(other.text.data()) == "Peer: probe-peer-001");
  for (uint8_t width = 1; width <= 3; ++width) {
    auto routed = scoped;
    const size_t hops = MAX_PATH_SIZE / width;
    // 1-byte hashes have a six-bit hop count (63); 2/3-byte hashes hit the
    // separate 64-byte path bound first.
    const size_t count = std::min(hops, size_t{63});
    routed[5] = ((width - 1) << 6) | count;
    routed.insert(routed.begin() + 6, width * count, 0x42);
    assert(codec_a.decode(routed.data(), routed.size(), key_a, message) == DecodeResult::OK);
    assert(message.scoped && message.hops == count && message.path_hash_size == width);
    assert(message.fingerprint == original_fingerprint && message.transport_codes == other.transport_codes);
  }
  for (size_t n = 0; n < scoped.size(); ++n)
    assert(codec_a.decode(scoped.data(), n, key_a, message) != DecodeResult::OK);
  for (uint8_t path : {uint8_t(0xc0), uint8_t(0x7f), uint8_t(0xbf)}) {
    tampered = scoped;
    tampered[5] = path;
    assert(codec_a.decode(tampered.data(), tampered.size(), key_a, message) == DecodeResult::MALFORMED);
  }
  tampered = scoped;
  tampered[1] ^= 0x42;
  tampered[3] = 0x34;
  tampered[4] = 0x12;
  assert(codec_a.decode(tampered.data(), tampered.size(), key_a, message) == DecodeResult::OK);
  assert(message.transport_codes[1] == 0x1234 && message.fingerprint == original_fingerprint);
  tampered.back() ^= 1;
  assert(codec_a.decode(tampered.data(), tampered.size(), key_a, message) == DecodeResult::BAD_MAC);
  assert(codec_a.decode(scoped.data(), scoped.size(), colliding, message) == DecodeResult::BAD_MAC);
  assert(codec_a.decode(reference.data(), reference.size(), key_a, message) == DecodeResult::OK);
  assert(!message.scoped && message.transport_codes[0] == 0 && message.transport_codes[1] == 0);
  for (const auto &fixture :
       {std::pair{ZERO_SCOPE_KEY, ZERO_SCOPED_FRAME}, std::pair{FFFF_SCOPE_KEY, FFFF_SCOPED_FRAME}}) {
    const auto edge_key = hex(fixture.first);
    const auto expected = hex(fixture.second);
    FloodScope scope;
    assert(codec_a.make_scope(edge_key.data(), edge_key.size(), scope));
    assert(codec_a.encode(key_a, 2, 1793491200, "Peer", "probe-peer-001", encoded, &scope));
    assert(encoded.size == expected.size() && std::equal(expected.begin(), expected.end(), encoded.bytes.begin()));
  }
  FloodScope invalid_scope;
  assert(!codec_a.make_scope(nullptr, 16, invalid_scope));
  assert(!codec_a.make_scope(nyc_key.data(), 15, invalid_scope));
  std::array<uint8_t, 16> zero_scope_key{};
  assert(!codec_a.make_scope(zero_scope_key.data(), zero_scope_key.size(), invalid_scope));
  assert(!codec_a.encode(key_a, 2, 1, "P", "text", encoded, &invalid_scope) && encoded.size == 0);

  // The shared transport wrapper never changes a frame on failure, and bounds both path and payload.
  Frame original;
  original.size = reference.size();
  std::copy(reference.begin(), reference.end(), original.bytes.begin());
  auto unchanged = original;
  crypto_a.fail_hmac = true;
  assert(!scope_flood_frame(crypto_a, nyc, unchanged));
  assert(unchanged.size == original.size && unchanged.bytes == original.bytes);
  crypto_a.fail_hmac = false;
  assert(!scope_flood_frame(crypto_a, invalid_scope, unchanged));
  assert(unchanged.size == original.size && unchanged.bytes == original.bytes);
  for (auto bad_header : {0x14, 0x16, 0x17, 0x55}) {
    auto bad = original;
    bad.bytes[0] = bad_header;
    const auto before = bad;
    assert(!scope_flood_frame(crypto_a, nyc, bad) && bad.size == before.size && bad.bytes == before.bytes);
  }
  for (size_t bad_size : {size_t(0), size_t(1), size_t(2), MAX_FRAME_SIZE - 3, MAX_FRAME_SIZE + 1}) {
    auto bad = original;
    bad.size = bad_size;
    const auto before = bad;
    assert(!scope_flood_frame(crypto_a, nyc, bad) && bad.size == before.size && bad.bytes == before.bytes);
  }
  for (auto bad_path : {0xc0, 0xbf, 0x7f}) {
    auto bad = original;
    bad.bytes[1] = bad_path;
    const auto before = bad;
    assert(!scope_flood_frame(crypto_a, nyc, bad) && bad.size == before.size && bad.bytes == before.bytes);
  }
  Frame transport_maximum;
  transport_maximum.bytes[0] = 0x11;  // Opaque advertisement payload; this helper changes only the outer envelope.
  transport_maximum.bytes[1] = 0x60;  // Two-byte hashes, 32 hops = transport_maximum 64 path bytes.
  transport_maximum.size = 2 + MAX_PATH_SIZE + MAX_PAYLOAD_SIZE;
  std::fill(transport_maximum.bytes.begin() + 2, transport_maximum.bytes.begin() + transport_maximum.size, 0x42);
  const auto unwrapped_transport_maximum = transport_maximum;
  assert(scope_flood_frame(crypto_a, nyc, transport_maximum) &&
         transport_maximum.size == unwrapped_transport_maximum.size + 4);
  assert(transport_maximum.bytes[5] == 0x60 &&
         std::equal(unwrapped_transport_maximum.bytes.begin() + 2,
                    unwrapped_transport_maximum.bytes.begin() + unwrapped_transport_maximum.size,
                    transport_maximum.bytes.begin() + 6));

  auto public_key = hex("8b3387e9c5cdea6ac9e5edbaa115cd72");
  auto public_frame = hex(PUBLIC_FRAME);
  ChannelKey public_channel;
  assert(codec_b.make_key(public_key.data(), public_key.size(), public_channel));
  assert(codec_b.decode(public_frame.data(), public_frame.size(), public_channel, other) == DecodeResult::OK);
  assert(std::string(other.text.data()) == "Public peer: hello");
  std::array<uint8_t, 32> key32{};
  for (size_t i = 0; i < key32.size(); ++i)
    key32[i] = i + 1;
  ChannelKey channel32;
  assert(codec_b.make_key(key32.data(), key32.size(), channel32));
  auto frame32 = hex(KEY32_FRAME);
  assert(codec_b.decode(frame32.data(), frame32.size(), channel32, other) == DecodeResult::OK);
  assert(std::string(other.text.data()) == "Peer: key32");

  assert(codec_a.encode(key_a, 2, 1793491203, "P", "12345678", encoded));
  auto aligned = hex(ALIGNED_FRAME);
  assert(encoded.size == 21 && std::equal(aligned.begin(), aligned.end(), encoded.bytes.begin()));
  assert(codec_a.encode(key_a, 2, 1793491204, "P", std::string(157, 'x'), encoded));
  auto maximum = hex(MAX_FRAME);
  assert(encoded.size == maximum.size() && std::equal(maximum.begin(), maximum.end(), encoded.bytes.begin()));
  const auto scoped_maximum = hex(NYC_SCOPED_MAX_FRAME);
  assert(codec_a.encode(key_a, 2, 1793491204, "P", std::string(157, 'x'), encoded, &nyc));
  assert(encoded.size == scoped_maximum.size() &&
         std::equal(scoped_maximum.begin(), scoped_maximum.end(), encoded.bytes.begin()));
  auto routed_maximum = scoped_maximum;
  routed_maximum[5] = 0x60;  // 32 two-byte hops.
  routed_maximum.insert(routed_maximum.begin() + 6, 64, 0x42);
  assert(codec_a.decode(routed_maximum.data(), routed_maximum.size(), key_a, message) == DecodeResult::OK);
  assert(!codec_a.encode(key_a, 2, 1793491204, "P", std::string(158, 'x'), encoded));
  assert(encoded.size == 0);
  assert(!codec_a.encode(key_a, 4, 1793491204, "P", "text", encoded));
  assert(!codec_a.encode(key_a, 2, 0, "P", "text", encoded));
  assert(!codec_a.encode(key_a, 2, 1, "P", std::string("a\0b", 3), encoded));
  assert(codec_a.encode(key_a, 2, 1, std::string(MAX_NAME_SIZE, 'x'), "text", encoded));
  for (const auto &name : {std::string(MAX_NAME_SIZE + 1, 'x'), std::string("bad:name"), std::string("bad\nname"),
                           std::string("caf\xc3\xa9")})
    assert(!codec_a.encode(key_a, 2, 1, name, "text", encoded));
  // Per-send sender selection does not mutate a previous message or identity.
  assert(codec_a.encode(key_a, 2, 1, "Automation", "meshcore-pong-v1", encoded, &nyc));
  assert(codec_b.decode(encoded.bytes.data(), encoded.size, key_b, message) == DecodeResult::OK);
  assert(message.sender() == "Automation" && message.body() == "meshcore-pong-v1" && message.scoped);
  assert(codec_a.encode(key_a, 2, 2, "Other", "prefix: body", encoded));
  assert(codec_b.decode(encoded.bytes.data(), encoded.size, key_b, message) == DecodeResult::OK);
  assert(message.sender() == "Other" && message.body() == "prefix: body");
  // Nonconventional group text remains observable, with no invented sender.
  message = {};
  std::copy_n("bare text", 9, message.text.begin());
  message.text_size = 9;
  assert(message.sender().empty() && message.body() == "bare text");
  const ChannelText owned(std::string("temporary text"));
  assert(owned.view() == "temporary text");
  const ChannelText oversized(std::string(500, 'x'));
  assert(oversized.view().size() > MAX_SENT_TEXT);
  assert(!codec_a.encode(key_a, 2, 3, "P", oversized.view(), encoded));
  const ChannelText embedded_nul(std::string("a\0b", 3));
  assert(!codec_a.encode(key_a, 2, 3, "P", embedded_nul.view(), encoded));
  // Bounded reply composition owns the peer name and retains overflow rejection.
  ChannelText reply("@[");
  std::string peer = "Foo";
  reply.append(peer).append("] esphome-pong").append({});
  peer = "Another peer";
  assert(reply.view() == "@[Foo] esphome-pong");
  assert(codec_a.encode(key_a, 2, 4, "ESPHome Node", reply.view(), encoded, &nyc));
  assert(codec_b.decode(encoded.bytes.data(), encoded.size, key_b, message) == DecodeResult::OK);
  assert(message.sender() == "ESPHome Node" && message.body() == "@[Foo] esphome-pong");
  ChannelText full_reply(std::string(MAX_SENT_TEXT, 'x'));
  full_reply.append("!").append("ignored overflow");
  assert(full_reply.view().size() == MAX_SENT_TEXT + 1);
  assert(!codec_a.encode(key_a, 2, 5, "ESPHome Node", full_reply.view(), encoded));
  assert(!codec_a.make_key(testing.data(), 15, colliding) && !colliding.valid);
  auto unsupported = hex(UNSUPPORTED_TEXT_FRAME);
  assert(codec_a.decode(unsupported.data(), unsupported.size(), key_a, message) == DecodeResult::UNSUPPORTED);
  auto unicode = hex(UTF8_FRAME);
  assert(codec_a.decode(unicode.data(), unicode.size(), key_a, message) == DecodeResult::OK);
  assert(std::string(message.text.data()) == "Peer: caf\xc3\xa9");

  RecentMessages recent_a, recent_b;
  recent_a.remember(original_fingerprint);
  assert(recent_a.contains(original_fingerprint) && !recent_b.contains(original_fingerprint));
  assert(codec_b.decode(scoped.data(), scoped.size(), key_b, other) == DecodeResult::OK);
  assert(recent_a.contains(other.fingerprint) && !recent_b.contains(other.fingerprint));
  assert(codec_b.encode(key_b, 2, 1793491200, "Peer", "probe-peer-001", encoded));
  assert(encoded.size == reference.size() && std::equal(reference.begin(), reference.end(), encoded.bytes.begin()));
  assert(codec_b.decode(reference.data(), reference.size(), key_b, other) == DecodeResult::OK);
  for (size_t i = 0; i < 32; ++i) {
    std::array<uint8_t, 8> digest{};
    digest[0] = static_cast<uint8_t>(i);
    recent_a.remember(digest);
  }
  assert(!recent_a.contains(original_fingerprint));

  // Exercise every input length and arbitrary headers/paths under ASan/UBSan.
  std::mt19937 random(20261031);
  for (size_t i = 0; i < 20000; ++i) {
    std::vector<uint8_t> bytes(random() % 300);
    for (auto &byte : bytes)
      byte = static_cast<uint8_t>(random());
    codec_a.decode(bytes.data(), bytes.size(), key_a, message);
  }
  std::cout << "PASS: independent wire vectors, scopes/reserved codes, bounds, MAC/hash collisions, paths, duplicate "
               "state, two instances, 20,000 malformed inputs\n";
}
