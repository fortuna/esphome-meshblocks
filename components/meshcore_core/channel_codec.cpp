#include "channel_codec.h"

#include <algorithm>
#include <cstring>

namespace meshcore_lib {

bool ChannelTextCodec::make_key(const uint8_t *key, size_t size, ChannelKey &out) {
  out = {};
  if (key == nullptr || (size != 16 && size != 32))
    return false;
  std::array<uint8_t, 32> hash{};
  if (!this->crypto_.sha256(key, size, hash.data()))
    return false;
  std::copy_n(key, size, out.secret.begin());
  out.hash = hash[0];
  out.valid = true;
  return true;
}

bool make_flood_scope(const uint8_t *key, size_t size, FloodScope &out) {
  out = {};
  if (key == nullptr || size != out.key.size() || std::all_of(key, key + size, [](uint8_t byte) { return byte == 0; }))
    return false;
  std::copy_n(key, size, out.key.begin());
  out.valid = true;
  return true;
}

bool ChannelTextCodec::make_scope(const uint8_t *key, size_t size, FloodScope &out) {
  return make_flood_scope(key, size, out);
}

bool scope_flood_frame(CryptoProvider &crypto, const FloodScope &scope, Frame &frame) {
  if (!scope.valid || frame.size < 3 || frame.size > MAX_FRAME_SIZE - 4 || (frame.bytes[0] >> 6) != 0 ||
      (frame.bytes[0] & 3) != 1)
    return false;
  const uint8_t width = (frame.bytes[1] >> 6) + 1;
  const size_t path_size = width * (frame.bytes[1] & 63);
  const size_t payload_offset = 2 + path_size;
  if (width > 3 || path_size > MAX_PATH_SIZE || frame.size <= payload_offset ||
      frame.size - payload_offset > MAX_PAYLOAD_SIZE)
    return false;
  std::array<uint8_t, MAX_PAYLOAD_SIZE + 1> input{};
  input[0] = (frame.bytes[0] >> 2) & 15;
  std::copy_n(frame.bytes.begin() + payload_offset, frame.size - payload_offset, input.begin() + 1);
  // The crypto port's zero-extended key has the same HMAC block padding as the original 16-byte transport key.
  std::array<uint8_t, 32> scope_key{}, mac{};
  std::copy(scope.key.begin(), scope.key.end(), scope_key.begin());
  if (!crypto.hmac_sha256(scope_key.data(), input.data(), 1 + frame.size - payload_offset, mac.data()))
    return false;
  uint16_t code = static_cast<uint16_t>(mac[0]) | (static_cast<uint16_t>(mac[1]) << 8);
  if (code == 0)
    code = 1;
  else if (code == 0xffff)
    code = 0xfffe;
  std::copy_backward(frame.bytes.begin() + 1, frame.bytes.begin() + frame.size, frame.bytes.begin() + frame.size + 4);
  frame.bytes[0] &= ~uint8_t(3);
  frame.bytes[1] = static_cast<uint8_t>(code);
  frame.bytes[2] = static_cast<uint8_t>(code >> 8);
  frame.bytes[3] = frame.bytes[4] = 0;
  frame.size += 4;
  return true;
}

DecodeResult ChannelTextCodec::decode(const uint8_t *frame, size_t size, const ChannelKey &key, ChannelMessage &out) {
  out = {};
  if (frame == nullptr || size < 2 || size > MAX_FRAME_SIZE)
    return DecodeResult::MALFORMED;
  const uint8_t header = frame[0];
  if ((header >> 6) != 0 || ((header >> 2) & 15) != 5)
    return DecodeResult::UNSUPPORTED;
  const uint8_t route = header & 3;
  if (route != 0 && route != 1)
    return DecodeResult::UNSUPPORTED;
  const bool scoped = route == 0;
  const size_t path_offset = scoped ? 5 : 1;
  if (size <= path_offset)
    return DecodeResult::MALFORMED;
  const uint8_t width = (frame[path_offset] >> 6) + 1;
  const uint8_t hops = frame[path_offset] & 63;
  const size_t path_size = static_cast<size_t>(width) * hops;
  const size_t payload_offset = path_offset + 1 + path_size;
  if (width > 3 || path_size > MAX_PATH_SIZE || payload_offset >= size)
    return DecodeResult::MALFORMED;
  const uint8_t *payload = frame + payload_offset;
  const size_t payload_size = size - payload_offset;
  if (payload_size < 19 || payload_size > MAX_PAYLOAD_SIZE)
    return DecodeResult::MALFORMED;
  const size_t encrypted_size = payload_size - 3;
  if (encrypted_size % 16 != 0 || encrypted_size > MAX_CIPHERTEXT_SIZE)
    return DecodeResult::MALFORMED;
  if (!key.valid || payload[0] != key.hash)
    return DecodeResult::WRONG_CHANNEL;
  std::array<uint8_t, 32> digest{};
  if (!this->crypto_.hmac_sha256(key.secret.data(), payload + 3, encrypted_size, digest.data()))
    return DecodeResult::CRYPTO_ERROR;
  if (((digest[0] ^ payload[1]) | (digest[1] ^ payload[2])) != 0)
    return DecodeResult::BAD_MAC;
  std::array<uint8_t, MAX_CIPHERTEXT_SIZE> plain{};
  if (!this->crypto_.aes128_ecb(false, key.secret.data(), payload + 3, encrypted_size, plain.data()))
    return DecodeResult::CRYPTO_ERROR;
  if ((plain[4] >> 2) != 0)
    return DecodeResult::UNSUPPORTED;
  size_t text_size = 0;
  while (5 + text_size < encrypted_size && plain[5 + text_size] != 0)
    ++text_size;
  if (text_size == 0)
    return DecodeResult::MALFORMED;
  // Upstream hash excludes path AND transport codes, including on rescoped copies.
  std::array<uint8_t, MAX_PAYLOAD_SIZE + 1> hash_input{};
  hash_input[0] = 5;
  std::copy_n(payload, payload_size, hash_input.begin() + 1);
  if (!this->crypto_.sha256(hash_input.data(), 1 + payload_size, digest.data()))
    return DecodeResult::CRYPTO_ERROR;
  std::copy_n(digest.begin(), out.fingerprint.size(), out.fingerprint.begin());
  out.timestamp = static_cast<uint32_t>(plain[0]) | (static_cast<uint32_t>(plain[1]) << 8) |
                  (static_cast<uint32_t>(plain[2]) << 16) | (static_cast<uint32_t>(plain[3]) << 24);
  out.path_hash_size = width;
  out.hops = hops;
  out.scoped = scoped;
  if (scoped) {
    // Stock companions receive by channel key regardless of transport codes.
    // Keep the metadata, without treating these short codes as authentication.
    for (size_t i = 0; i < out.transport_codes.size(); ++i)
      out.transport_codes[i] = static_cast<uint16_t>(frame[1 + 2 * i]) | (static_cast<uint16_t>(frame[2 + 2 * i]) << 8);
  }
  out.text_size = text_size;
  std::copy_n(plain.begin() + 5, text_size, out.text.begin());
  return DecodeResult::OK;
}

bool ChannelTextCodec::encode(const ChannelKey &key, uint8_t path_hash_size, uint32_t timestamp,
                              std::string_view sender, std::string_view text, Frame &out, const FloodScope *scope) {
  out = {};
  const size_t text_size = sender.size() + 2 + text.size();
  if (!key.valid || (scope != nullptr && !scope->valid) || path_hash_size < 1 || path_hash_size > 3 || timestamp == 0 ||
      sender.empty() || text.empty() || sender.size() > MAX_NAME_SIZE || text.size() > MAX_SENT_TEXT ||
      std::any_of(sender.begin(), sender.end(), [](unsigned char ch) { return ch < 32 || ch > 126 || ch == ':'; }) ||
      text_size > MAX_SENT_TEXT || sender.find('\0') != std::string_view::npos ||
      text.find('\0') != std::string_view::npos)
    return false;
  std::array<uint8_t, MAX_CIPHERTEXT_SIZE> plain{};
  for (size_t i = 0; i < 4; ++i)
    plain[i] = static_cast<uint8_t>(timestamp >> (8 * i));
  // plain[4] = TXT_TYPE_PLAIN, followed by "sender: text", zero-padded only.
  auto dest = std::copy(sender.begin(), sender.end(), plain.begin() + 5);
  *dest++ = ':';
  *dest++ = ' ';
  std::copy(text.begin(), text.end(), dest);
  const size_t encrypted_size = ((5 + text_size + 15) / 16) * 16;
  out.bytes[0] = 0x15;  // V1 group text; add the optional scope after encoding the payload.
  const size_t path_offset = 1;
  out.bytes[path_offset] = (path_hash_size - 1) << 6;
  uint8_t *payload = out.bytes.data() + path_offset + 1;
  payload[0] = key.hash;
  if (!this->crypto_.aes128_ecb(true, key.secret.data(), plain.data(), encrypted_size, payload + 3))
    return false;
  std::array<uint8_t, 32> mac{};
  if (!this->crypto_.hmac_sha256(key.secret.data(), payload + 3, encrypted_size, mac.data()))
    return false;
  payload[1] = mac[0];
  payload[2] = mac[1];
  out.size = path_offset + 4 + encrypted_size;
  if (scope != nullptr && !scope_flood_frame(this->crypto_, *scope, out)) {
    out = {};
    return false;
  }
  return true;
}

bool RecentMessages::contains(const std::array<uint8_t, 8> &hash) const {
  for (size_t i = 0; i < this->count_; ++i)
    if (this->hashes_[i] == hash)
      return true;
  return false;
}

void RecentMessages::remember(const std::array<uint8_t, 8> &hash) {
  if (this->contains(hash))
    return;
  this->hashes_[this->next_] = hash;
  this->next_ = (this->next_ + 1) % this->hashes_.size();
  this->count_ = std::min(this->count_ + 1, this->hashes_.size());
}

}  // namespace meshcore_lib
