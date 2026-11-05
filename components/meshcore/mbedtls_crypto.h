#pragma once
#include "esphome/components/meshcore_core/channel_codec.h"
#include "mbedtls/md.h"

namespace esphome::meshcore {
class MbedTlsCrypto final : public ::meshcore_lib::CryptoProvider {
 public:
  /// Initialize the reusable HMAC context; failed initialization is reported by subsequent HMAC calls.
  MbedTlsCrypto();
  /// Free the owned mbedTLS context; injected users must have finished using this adapter.
  ~MbedTlsCrypto() override;
  /// Prevent copying the owned SDK context.
  MbedTlsCrypto(const MbedTlsCrypto &) = delete;
  /// Prevent assigning an adapter with uniquely owned SDK resources.
  MbedTlsCrypto &operator=(const MbedTlsCrypto &) = delete;
  /// Hash size borrowed bytes into a 32-byte digest; false reports mbedTLS failure.
  bool sha256(const uint8_t *data, size_t size, uint8_t out[32]) override;
  /// Write a 32-byte HMAC with the 32-byte key using the reusable context; false reports unavailable/failed crypto.
  /// Calls must be serialized; no input buffers are retained.
  bool hmac_sha256(const uint8_t key[32], const uint8_t *data, size_t size, uint8_t out[32]) override;
  /// Encrypt/decrypt a positive multiple of 16 bytes without padding, allowing in-place buffers.
  /// False reports invalid length or mbedTLS failure; discard output on failure.
  bool aes128_ecb(bool encrypt, const uint8_t key[16], const uint8_t *data, size_t size, uint8_t *out) override;

 private:
  mbedtls_md_context_t hmac_{};
  bool hmac_ready_{};
};
}  // namespace esphome::meshcore
