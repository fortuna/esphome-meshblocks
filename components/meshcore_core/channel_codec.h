#pragma once

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Wire behavior adapted from pinned MeshCore; see third_party/meshcore/NOTICE.md.
// This library deliberately has no platform, radio, logging, or allocation APIs.
namespace meshcore_lib {

constexpr size_t MAX_FRAME_SIZE = 255;
constexpr size_t MAX_PATH_SIZE = 64;
constexpr size_t MAX_PAYLOAD_SIZE = 184;
constexpr size_t MAX_CIPHERTEXT_SIZE = 176;
constexpr size_t MAX_RECEIVED_TEXT = MAX_CIPHERTEXT_SIZE - 5;
constexpr size_t MAX_SENT_TEXT = 160;  // Includes the "sender: " prefix.
// Advert app data holds 32 bytes: a flags byte, then the name when no location is sent.
// Stock firmware stores names in char[32], so this also bounds sender and client names.
constexpr size_t MAX_NAME_SIZE = 31;

class CryptoProvider {
 public:
  /// Allow destruction through the crypto port; consumers do not take ownership of injected providers.
  virtual ~CryptoProvider() = default;
  /// Hash exactly size input bytes into a 32-byte digest; false on provider failure. Buffers are not retained.
  virtual bool sha256(const uint8_t *data, size_t size, uint8_t out[32]) = 0;
  /// Authenticate size bytes with the 32-byte key; write a 32-byte MAC only usable when true is returned.
  virtual bool hmac_sha256(const uint8_t key[32], const uint8_t *data, size_t size, uint8_t out[32]) = 0;
  /// Encrypt/decrypt a positive multiple of 16 bytes with no padding; in-place operation is supported.
  /// False reports invalid length or provider failure; callers must discard output on failure.
  virtual bool aes128_ecb(bool encrypt, const uint8_t key[16], const uint8_t *data, size_t size, uint8_t *out) = 0;
};

struct ChannelKey {
  std::array<uint8_t, 32> secret{};
  uint8_t hash{};
  bool valid{false};
};

// Outbound flood scope, separate from the channel's encryption key. A scope
// influences repeater forwarding; it is not a receive ACL or room identity.
struct FloodScope {
  std::array<uint8_t, 16> key{};
  bool valid{false};
};

struct Frame {
  std::array<uint8_t, MAX_FRAME_SIZE> bytes{};
  size_t size{};
};

/// Copy a nonzero 16-byte transport key; false resets out to an invalid scope. No input buffer is retained.
bool make_flood_scope(const uint8_t *key, size_t size, FloodScope &out);
/// Wrap a bounded V1 ordinary flood frame with packet-specific transport codes and a zero return code.
/// The signed/encrypted payload and path are retained. False leaves frame unchanged on invalid scope/frame,
/// insufficient capacity or HMAC failure; this operation neither authorizes peers nor sends a packet.
bool scope_flood_frame(CryptoProvider &crypto, const FloodScope &scope, Frame &frame);

struct ChannelMessage {
  uint32_t timestamp{};
  uint8_t path_hash_size{};
  uint8_t hops{};
  bool scoped{};
  std::array<uint16_t, 2> transport_codes{};
  std::array<char, MAX_RECEIVED_TEXT + 1> text{};
  size_t text_size{};
  std::array<uint8_t, 8> fingerprint{};

  // Views belong to this value. The conventional sender prefix is unverified.
  /// Borrow the text before the first ": ", or an empty view when no prefix exists. The label is unverified.
  std::string_view sender() const {
    const std::string_view value(this->text.data(), this->text_size);
    const auto separator = value.find(": ");
    return separator == value.npos ? std::string_view{} : value.substr(0, separator);
  }
  /// Borrow the text after the first ": ", or the entire text without that delimiter; valid while this value lives.
  std::string_view body() const {
    const std::string_view value(this->text.data(), this->text_size);
    const auto separator = value.find(": ");
    return separator == value.npos ? value : value.substr(separator + 2);
  }
};

// Owned automation argument/result: copying never allocates or borrows a
// temporary lambda string. Keep one extra byte so overlength inputs fail the
// codec's limit instead of silently transmitting a truncated message.
class ChannelText {
 public:
  /// Create an empty owning text value without allocating.
  ChannelText() = default;
  /// Copy text into bounded storage, retaining one over-limit byte so the send codec rejects oversize input.
  ChannelText(std::string_view value) : size_(std::min(value.size(), this->bytes_.size())) {
    std::copy_n(value.begin(), this->size_, this->bytes_.begin());
  }
  /// Copy a NUL-terminated string; nullptr is treated as empty text.
  ChannelText(const char *value) : ChannelText(std::string_view(value == nullptr ? "" : value)) {}
  /// Copy a string, including embedded NUL bytes; send-time validation decides whether it is admissible.
  ChannelText(const std::string &value) : ChannelText(std::string_view(value)) {}
  /// Append a bounded copy and return this value for chaining; full storage retains an over-limit length.
  ChannelText &append(std::string_view value) {
    const size_t length = std::min(value.size(), this->bytes_.size() - this->size_);
    std::copy_n(value.begin(), length, this->bytes_.begin() + this->size_);
    this->size_ += length;
    return *this;
  }
  /// Borrow the stored bytes until mutation/destruction; the view is not guaranteed to be NUL-terminated.
  std::string_view view() const { return {this->bytes_.data(), this->size_}; }

 private:
  std::array<char, MAX_SENT_TEXT + 1> bytes_{};
  size_t size_{};
};

enum class DecodeResult { OK, MALFORMED, UNSUPPORTED, WRONG_CHANNEL, BAD_MAC, CRYPTO_ERROR };

class ChannelTextCodec {
 public:
  /// Borrow a crypto provider that outlives this codec; encoding/decoding do not own a radio or key store.
  explicit ChannelTextCodec(CryptoProvider &crypto) : crypto_(crypto) {}
  /// Copy a 16- or 32-byte key, zero-pad its secret and derive its routing hash; false for bad inputs/crypto failure.
  bool make_key(const uint8_t *key, size_t size, ChannelKey &out);
  /// Copy a nonzero 16-byte outbound scope key; false resets out to an invalid scope.
  bool make_scope(const uint8_t *key, size_t size, FloodScope &out);
  /// Decode V1 ordinary/scoped flood text with the supplied key into an owning value.
  /// Returns a specific format/key/MAC/crypto result; duplicate suppression and sender authentication are not supplied.
  DecodeResult decode(const uint8_t *frame, size_t size, const ChannelKey &key, ChannelMessage &out);
  /// Encode bounded "sender: text" into an owned flood frame, optionally scoped; true means encoding only.
  /// Uses timestamp exactly (nonzero epoch seconds); invalid fields, over 160 formatted bytes or crypto failure reject.
  bool encode(const ChannelKey &key, uint8_t path_hash_size, uint32_t timestamp, std::string_view sender,
              std::string_view text, Frame &out, const FloodScope *scope = nullptr);

 private:
  CryptoProvider &crypto_;
};

// Fixed-size history, scoped to its owner. Paths are excluded from fingerprints
// and transport codes so relayed/rescoped copies do not become new messages.
// This is not replay protection.
class RecentMessages {
 public:
  /// Check the volatile cache of up to 32 fingerprints; this is duplicate suppression, not replay protection.
  bool contains(const std::array<uint8_t, 8> &hash) const;
  /// Remember a new fingerprint, overwriting the oldest when full; an already remembered value is a no-op.
  void remember(const std::array<uint8_t, 8> &hash);

 private:
  std::array<std::array<uint8_t, 8>, 32> hashes_{};
  size_t count_{};
  size_t next_{};
};

}  // namespace meshcore_lib
