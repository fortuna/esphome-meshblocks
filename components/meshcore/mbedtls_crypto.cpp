#include "mbedtls_crypto.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

namespace esphome::meshcore {
MbedTlsCrypto::MbedTlsCrypto() {
  mbedtls_md_init(&this->hmac_);
  const auto *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  this->hmac_ready_ = info != nullptr && mbedtls_md_setup(&this->hmac_, info, 1) == 0;
}

MbedTlsCrypto::~MbedTlsCrypto() { mbedtls_md_free(&this->hmac_); }

bool MbedTlsCrypto::sha256(const uint8_t *data, size_t size, uint8_t out[32]) {
  return mbedtls_sha256(data, size, out, 0) == 0;
}

bool MbedTlsCrypto::hmac_sha256(const uint8_t key[32], const uint8_t *data, size_t size, uint8_t out[32]) {
  // Reuse the context allocated at construction, rather than allocate per packet.
  return this->hmac_ready_ && mbedtls_md_hmac_starts(&this->hmac_, key, 32) == 0 &&
         mbedtls_md_hmac_update(&this->hmac_, data, size) == 0 && mbedtls_md_hmac_finish(&this->hmac_, out) == 0;
}

bool MbedTlsCrypto::aes128_ecb(bool encrypt, const uint8_t key[16], const uint8_t *data, size_t size, uint8_t *out) {
  if (size == 0 || size % 16 != 0)
    return false;
  mbedtls_aes_context context;
  mbedtls_aes_init(&context);
  int result = encrypt ? mbedtls_aes_setkey_enc(&context, key, 128) : mbedtls_aes_setkey_dec(&context, key, 128);
  for (size_t i = 0; result == 0 && i < size; i += 16)
    result = mbedtls_aes_crypt_ecb(&context, encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, data + i, out + i);
  mbedtls_aes_free(&context);
  return result == 0;
}
}  // namespace esphome::meshcore
