#pragma once
#include "channel_codec.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
class OpenSslCrypto final : public meshcore_lib::CryptoProvider {
 public:
  bool sha256(const uint8_t *data, size_t size, uint8_t out[32]) override { return SHA256(data, size, out) != nullptr; }
  bool hmac_sha256(const uint8_t key[32], const uint8_t *data, size_t size, uint8_t out[32]) override {
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
