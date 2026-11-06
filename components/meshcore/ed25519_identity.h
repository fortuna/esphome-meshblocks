#pragma once
#include "../meshcore_core/room_protocol.h"
namespace esphome::meshcore {
class Ed25519Identity final : public ::meshcore_lib::IdentityCrypto {
 public:
  /// Derive both keys from a 32-byte seed; false for reserved 00/ff public-key prefixes.
  /// Output contains private material and is usable only on success; the seed is not retained.
  bool derive(const uint8_t seed[32], ::meshcore_lib::LocalIdentity &out) override;
  /// Import a clamped 64-byte MeshCore private key and derive its public key.
  /// Rejects invalid clamping or reserved prefixes; use out only on success.
  /// This imports the native expanded key, not a seed or seed-plus-public-key value.
  bool import_private_key(const uint8_t key[64], ::meshcore_lib::LocalIdentity &out);
  /// Write a 64-byte Ed25519 signature over exactly size bytes using an already valid identity.
  /// The primitive does not validate id; input buffers are borrowed for this call only.
  bool sign(const ::meshcore_lib::LocalIdentity &id, const uint8_t *data, size_t size, uint8_t signature[64]) override;
  /// Derive the 32-byte shared secret for id and peer; false if the result is all zero.
  /// Neither identity is retained, and out is private material usable only on success.
  bool shared_secret(const ::meshcore_lib::LocalIdentity &id, const ::meshcore_lib::PublicKey &peer,
                     ::meshcore_lib::SharedSecret &out) override;
};
}  // namespace esphome::meshcore
