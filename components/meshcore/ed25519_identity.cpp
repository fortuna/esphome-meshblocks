#include "ed25519_identity.h"
#define ED25519_NO_SEED 1
#include "../meshcore_ed25519/ed_25519.h"
#include <algorithm>
namespace esphome::meshcore {
bool Ed25519Identity::derive(const uint8_t seed[32], ::meshcore_lib::LocalIdentity &out) {
  ed25519_create_keypair(out.public_key.data(), out.private_key.data(), seed);
  return out.public_key[0] != 0 && out.public_key[0] != 255;
}
bool Ed25519Identity::import_private_key(const uint8_t key[64], ::meshcore_lib::LocalIdentity &out) {
  // MeshCore stores the clamped SHA-512 expansion, not seed || public key.
  if ((key[0] & 7) != 0 || (key[31] & 0xc0) != 0x40)
    return false;
  std::copy_n(key, out.private_key.size(), out.private_key.begin());
  ed25519_derive_pub(out.public_key.data(), key);
  return out.public_key[0] != 0 && out.public_key[0] != 255;
}
bool Ed25519Identity::sign(const ::meshcore_lib::LocalIdentity &id, const uint8_t *data, size_t size,
                           uint8_t signature[64]) {
  ed25519_sign(signature, data, size, id.public_key.data(), id.private_key.data());
  return true;
}
bool Ed25519Identity::shared_secret(const ::meshcore_lib::LocalIdentity &id, const ::meshcore_lib::PublicKey &peer,
                                    ::meshcore_lib::SharedSecret &out) {
  ed25519_key_exchange(out.data(), peer.data(), id.private_key.data());
  return std::any_of(out.begin(), out.end(), [](uint8_t byte) { return byte != 0; });
}
}  // namespace esphome::meshcore
