#pragma once
#include "channel_codec.h"
#include "node_status.h"

namespace meshcore_lib {
// MeshCore v1.18.0 src/helpers/TxtDataHelpers.h. These are wire values.
enum class TextType : uint8_t { PLAIN = 0, CLI_DATA = 1, SIGNED_PLAIN = 2, CLI_COMMAND = 3 };
enum class AdvertRole : uint8_t { CHAT = 1, ROOM = 3 };
using PublicKey = std::array<uint8_t, 32>;
using SharedSecret = std::array<uint8_t, 32>;
struct LocalIdentity {
  PublicKey public_key{};
  std::array<uint8_t, 64> private_key{};  // Upstream expanded Ed25519 key, never logged.
};
class IdentityCrypto {
 public:
  /// Allow adapter destruction through the identity-crypto port; injected adapters remain caller-owned.
  virtual ~IdentityCrypto() = default;
  /// Derive both keys from a 32-byte seed; use out only on success and keep its private material secret.
  virtual bool derive(const uint8_t seed[32], LocalIdentity &out) = 0;
  /// Sign exactly size borrowed bytes with id into the caller-provided 64-byte signature; false means failure.
  virtual bool sign(const LocalIdentity &id, const uint8_t *data, size_t size, uint8_t signature[64]) = 0;
  /// Derive the 32-byte private shared secret with peer; false means agreement failed or must be rejected.
  virtual bool shared_secret(const LocalIdentity &id, const PublicKey &peer, SharedSecret &out) = 0;
};
class RandomSource {
 public:
  /// Allow adapter destruction through the randomness port; consumers borrow this dependency.
  virtual ~RandomSource() = default;
  /// Fill the caller-owned buffer with size random bytes; false means the output must not be used.
  virtual bool fill(uint8_t *data, size_t size) = 0;
};
struct PacketPath {
  uint8_t descriptor{0x40};
  std::array<uint8_t, MAX_PATH_SIZE> bytes{};
  /// Return the byte count encoded by hash width and hop count; valid() must pass before indexing path storage.
  size_t size() const { return static_cast<size_t>((this->descriptor >> 6) + 1) * (this->descriptor & 63); }
  /// Check the supported 1-3 byte hash width and bounded path length; this does not authenticate the route.
  bool valid() const { return (this->descriptor >> 6) < 3 && this->size() <= this->bytes.size(); }
};
struct PacketView {
  uint8_t type{}, route{};
  PacketPath path;
  const uint8_t *payload{};  // Borrowed only for this synchronous call.
  size_t payload_size{};
  /// Report whether the route code denotes ordinary or scoped flooding; this does not enable forwarding.
  bool flood() const { return this->route == 0 || this->route == 1; }
};
struct Plaintext {
  std::array<uint8_t, MAX_CIPHERTEXT_SIZE> bytes{};
  size_t size{};  // Includes zero padding, as in upstream.
};

/// Read four caller-readable bytes as an unaligned little-endian uint32; no bounds check is performed.
uint32_t read_u32(const uint8_t *p);
/// Write a uint32 into four caller-writable bytes in little-endian order.
void write_u32(uint8_t *p, uint32_t value);
/// Parse a V1 envelope and copy its path; out.payload borrows data until the input buffer changes or expires.
/// False rejects invalid sizes/version/path; parsing performs no authentication or decryption.
bool parse_packet(const uint8_t *data, size_t size, PacketView &out);

// Wire construction only: no radio, client table, policy, timers, or storage.
class RoomProtocol {
 public:
  /// Borrow crypto/identity adapters for this protocol object; both must outlive it and be used serially.
  RoomProtocol(CryptoProvider &crypto, IdentityCrypto &identity_crypto)
      : crypto_(crypto), identities_(identity_crypto) {}
  /// Build a signed room (default) or chat advertisement/contact frame without location, direct with an empty path by
  /// default. flood permits an optional borrowed scope; reject a scope for direct mode or invalid scope/crypto input.
  /// False is an encoding failure, not a TX result; scope changes only routing, not the signed payload.
  bool advert(const LocalIdentity &id, std::string_view name, uint32_t epoch, Frame &out, bool flood = false,
              const FloodScope *scope = nullptr, AdvertRole role = AdvertRole::ROOM);
  /// Extract the peer key, derive a shared secret and verify/decrypt an anonymous login envelope.
  /// Outputs are usable only on success; room password/replay/session policy is checked by the service.
  bool open_anonymous(const PacketView &packet, const LocalIdentity &id, PublicKey &peer, SharedSecret &secret,
                      Plaintext &out);
  /// Verify the short MAC and decrypt a peer envelope using an established secret; retains no packet buffers.
  bool open_peer(const PacketView &packet, const SharedSecret &secret, Plaintext &out);
  /// Zero-pad/encrypt a nonempty bounded payload and construct a peer datagram with the given route/path.
  /// False means invalid bounds/path or crypto failure; successful encoding does not queue or transmit.
  bool datagram(uint8_t type, const LocalIdentity &id, const PublicKey &peer, const SharedSecret &secret,
                const uint8_t *data, size_t size, bool flood, const PacketPath &path, Frame &out);
  /// Build a flood PATH reply carrying the received path and nonempty extra response data; reject invalid bounds.
  bool path_response(const LocalIdentity &id, const PublicKey &peer, const SharedSecret &secret,
                     const PacketPath &received_path, uint8_t extra_type, const uint8_t *extra, size_t extra_size,
                     Frame &out);
  /// Build a raw four-byte ACK, optionally adding unsynced when nonnegative; validates envelope/path bounds only.
  bool ack(const uint8_t token[4], bool flood, const PacketPath &path, Frame &out, int unsynced = -1);
  /// Hash plaintext plus the full peer public key into a four-byte ACK token; false for excessive size/crypto failure.
  bool ack_token(const uint8_t *plain, size_t size, const PublicKey &peer, std::array<uint8_t, 4> &out);
  /// Encode the request tag and 52 stock room-stat bytes; absent optional telemetry is represented as zero.
  static std::array<uint8_t, 56> status_payload(uint32_t request_tag, const NodeStatus &node, uint32_t posts,
                                                uint32_t pushes);

 private:
  bool decrypt_(const SharedSecret &secret, const uint8_t *data, size_t size, Plaintext &out);
  bool envelope_(uint8_t type, bool flood, const PacketPath &path, const uint8_t *payload, size_t size, Frame &out);
  CryptoProvider &crypto_;
  IdentityCrypto &identities_;
};
}  // namespace meshcore_lib
