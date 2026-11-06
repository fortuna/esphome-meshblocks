#include "../components/meshcore/room_runtime.h"
#include "openssl_crypto.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>

using namespace meshcore_lib;
using namespace esphome::meshcore;
namespace {
// Fake persistence survives runtime destruction. Commits and readback failures
// exercise the real adapter, not a second implementation of credential policy.
struct Storage {
  std::map<std::string, nvs_handle_t> handles;
  std::map<std::pair<nvs_handle_t, std::string>, std::vector<uint8_t>> data;
  size_t writes{};
  bool fail_commit{}, fail_read{}, fail_verification{};
} storage;
constexpr uint32_t EPOCH = 1793500000;
struct Transport final : RoomTransport {
  bool send(const Frame &, uint64_t, uint32_t &token) override {
    token = 1;
    return true;
  }
  bool ack_available(const std::array<uint8_t, 4> &) const override { return true; }
};
struct Status final : NodeStatusSource {
  NodeStatus snapshot() const override { return {}; }
};
struct Fixture {
  OpenSslCrypto crypto;
  Transport transport;
  Status status;
  std::array<RoomService *, 3> registry{};
  NodeRuntime node{this->registry};
  RoomRuntime room{this->crypto, this->transport, this->status};
  bool start(const std::string &id, const std::vector<uint8_t> &seed, const std::string &password = "chosen-pass123",
             RoomAccess access = RoomAccess::PRIVATE) {
    this->room.set_config(id, "Room", access);
    return this->room.set_static_credentials(seed, password) && this->room.setup(EPOCH, this->node) &&
           this->node.add_room(this->room.service());
  }
};
}  // namespace
int nvs_open(const char *name, int, nvs_handle_t *handle) {
  auto [entry, inserted] = storage.handles.emplace(name, storage.handles.size() + 1);
  (void) inserted;
  *handle = entry->second;
  return ESP_OK;
}
void nvs_close(nvs_handle_t) {}
int nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *size) {
  if (storage.fail_read)
    return 2;
  auto entry = storage.data.find({handle, key});
  if (entry == storage.data.end())
    return ESP_ERR_NVS_NOT_FOUND;
  if (*size < entry->second.size())
    return 2;
  *size = entry->second.size();
  std::memcpy(data, entry->second.data(), *size);
  return ESP_OK;
}
int nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  storage.data[{handle, key}] = {bytes, bytes + size};
  ++storage.writes;
  return ESP_OK;
}
int nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *value) {
  size_t size = sizeof(*value);
  return nvs_get_blob(handle, key, value, &size);
}
int nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value) {
  return nvs_set_blob(handle, key, &value, sizeof(value));
}
int nvs_commit(nvs_handle_t) {
  if (storage.fail_verification)
    storage.fail_read = true;
  return storage.fail_commit ? 2 : ESP_OK;
}

int main() {
  const std::vector<uint8_t> seed(32, 1), other_seed(32, 2);
  Ed25519Identity identities;
  LocalIdentity derived, imported, peer;
  assert(identities.derive(seed.data(), derived));
  const std::vector<uint8_t> raw_key(derived.private_key.begin(), derived.private_key.end());
  assert(identities.import_private_key(raw_key.data(), imported));
  assert(imported.public_key == derived.public_key && imported.private_key == derived.private_key);
  assert(identities.derive(other_seed.data(), peer));
  SharedSecret outgoing, incoming;
  assert(identities.shared_secret(imported, peer.public_key, outgoing));
  assert(identities.shared_secret(peer, imported.public_key, incoming) && incoming == outgoing);
  uint8_t signature[64];
  const uint8_t message[] = "imported identity";
  assert(identities.sign(imported, message, sizeof(message), signature));
  auto *verify_key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, imported.public_key.data(), 32);
  auto *context = EVP_MD_CTX_new();
  assert(verify_key && context && EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, verify_key) == 1);
  assert(EVP_DigestVerify(context, signature, sizeof(signature), message, sizeof(message)) == 1);
  EVP_MD_CTX_free(context);
  EVP_PKEY_free(verify_key);
  auto invalid = raw_key;
  invalid[0] |= 1;
  assert(!identities.import_private_key(invalid.data(), imported));
  {
    Fixture native;
    assert(native.start("native", raw_key));
    assert(native.room.service().public_key() == derived.public_key);
  }
  const size_t native_written = storage.writes;
  {
    Fixture native_reboot;
    assert(native_reboot.start("native", raw_key));
    assert(storage.writes == native_written);
    Fixture equivalent_seed;
    assert(equivalent_seed.start("native", seed));
    assert(storage.writes == native_written);  // Keep the v2 record; no conversion write.
    Fixture rotated;
    assert(rotated.start("native", raw_key, "rotated-pass123"));
    NvsRoomStore inspect;
    RoomCredentials credentials;
    assert(inspect.open("native") && inspect.load_credentials(credentials) == StoreResult::OK);
    assert(credentials.has_private_key && credentials.private_key == derived.private_key);
  }
  storage = {};
  PublicKey identity;
  {
    Fixture first;
    assert(first.start("personal", seed));
    assert(first.room.ready() && storage.writes == 1);
    identity = first.room.service().public_key();
    assert(first.room.service().add_system_post("retained", EPOCH + 10));
    assert(first.room.checkpoint(EPOCH + 20));
    assert(!first.room.change_password("usb-pass-12345"));
    assert(!first.room.provision("personal", std::string(64, '1').c_str(), "chosen-pass123", EPOCH, first.node));
  }
  const size_t written = storage.writes;
  {
    Fixture equivalent_key;
    assert(equivalent_key.start("personal", raw_key));
    assert(storage.writes == written);  // Adopts the v1 seed record without rewriting it.
  }
  {
    Fixture reboot;
    assert(reboot.start("personal", seed));
    assert(storage.writes == written && reboot.room.service().public_key() == identity);
    assert(reboot.room.service().posts() == 1 && reboot.room.restored_epoch() == EPOCH + 21);
  }
  {
    Fixture changed_password;
    assert(changed_password.start("personal", seed, "rotated-pass123"));
    assert(storage.writes == written + 1 && changed_password.room.service().posts() == 1);
    assert(changed_password.room.service().public_key() == identity);
    NvsRoomStore inspect;
    RoomCredentials current;
    assert(inspect.open("personal") && inspect.load_credentials(current) == StoreResult::OK);
    assert(std::strcmp(current.password.data(), "rotated-pass123") == 0);
  }
  const auto before = storage.data;
  {
    Fixture conflict;
    assert(!conflict.start("personal", other_seed));
    assert(!conflict.room.ready() && storage.data == before);
  }
  {
    Fixture public_room;
    assert(public_room.start("public", other_seed, "", RoomAccess::PUBLIC));
    assert(public_room.room.service().public_key() != identity);
    RoomRuntime duplicate(public_room.crypto, public_room.transport, public_room.status);
    duplicate.set_config("duplicate", "Duplicate", RoomAccess::PUBLIC);
    assert(duplicate.set_static_credentials(other_seed, ""));
    const size_t count = storage.writes;
    assert(!duplicate.setup(EPOCH, public_room.node) && storage.writes == count);
  }
  {
    Fixture public_to_private;
    assert(public_to_private.start("public", other_seed));
    Fixture private_to_public;
    const size_t count = storage.writes;
    assert(private_to_public.start("public", other_seed, "", RoomAccess::PUBLIC));
    assert(storage.writes == count);  // Public policy does not rewrite an unused password.
  }
  {
    Fixture legacy;
    legacy.room.set_config("personal", "Renamed", RoomAccess::PRIVATE);
    const size_t count = storage.writes;
    assert(legacy.room.setup(EPOCH, legacy.node) && legacy.room.ready());
    assert(std::strcmp(legacy.room.service().name(), "Renamed") == 0);
    assert(legacy.room.service().public_key() == identity && storage.writes == count);
  }
  {
    Fixture missing;
    missing.room.set_config("legacy", "Legacy", RoomAccess::PRIVATE);
    assert(missing.room.setup(EPOCH, missing.node) && !missing.room.ready());
  }
  {
    Fixture failed;
    storage.fail_commit = true;
    assert(!failed.start("failed", seed) && !failed.room.ready());
    const size_t count = storage.writes;
    assert(!failed.room.checkpoint(EPOCH + 1) && storage.writes == count);
    storage.fail_commit = false;
  }
  {
    Fixture failed_update;
    storage.fail_commit = true;
    assert(!failed_update.start("personal", seed, "another-pass12"));
    assert(!failed_update.room.ready());
    storage.fail_commit = false;
  }
  {
    Fixture corrupt;
    storage.data[{storage.handles.at("mc_personal"), "identity"}].resize(3);
    const size_t count = storage.writes;
    assert(!corrupt.start("personal", seed) && storage.writes == count);
  }
  {
    NvsRoomStore orphan;
    assert(orphan.open("orphan") && orphan.load_history() && orphan.checkpoint(EPOCH));
    Fixture rejected;
    const size_t count = storage.writes;
    assert(!rejected.start("orphan", seed) && storage.writes == count);
  }
  {
    Fixture read_error;
    storage.fail_read = true;
    const size_t count = storage.writes;
    assert(!read_error.start("public", other_seed) && storage.writes == count);
    storage.fail_read = false;
  }
  {
    Fixture verification_error;
    storage.fail_verification = true;
    assert(!verification_error.start("readback", seed) && !verification_error.room.ready());
    const size_t count = storage.writes;
    assert(!verification_error.room.change_password("another-pass12") && storage.writes == count);
    storage.fail_verification = storage.fail_read = false;
  }
  {
    Fixture advertisements;
    assert(advertisements.start("advert", seed));
    FloodScope scope, invalid_scope;
    std::array<uint8_t, 32> region_key{};
    const uint8_t region[] = "#nyc";
    assert(advertisements.crypto.sha256(region, sizeof(region) - 1, region_key.data()));
    assert(make_flood_scope(region_key.data(), 16, scope));
    Frame direct, flood;
    const auto writes_before_invalid_scope = storage.writes;
    assert(!advertisements.room.create_advertisement(EPOCH, false, direct, &scope));
    assert(!advertisements.room.create_advertisement(EPOCH, true, direct, &invalid_scope));
    assert(storage.writes == writes_before_invalid_scope);
    assert(!advertisements.room.create_advertisement(0, false, direct));
    assert(advertisements.room.create_advertisement(EPOCH, false, direct));
    assert(advertisements.room.create_advertisement(EPOCH, true, flood, &scope));
    PacketView direct_view, flood_view;
    assert(parse_packet(direct.bytes.data(), direct.size, direct_view));
    assert(parse_packet(flood.bytes.data(), flood.size, flood_view));
    assert(!direct_view.flood() && flood_view.route == 0);
    assert(read_u32(flood_view.payload + 32) == read_u32(direct_view.payload + 32) + 1);
    assert(
        std::equal(direct_view.payload, direct_view.payload + 32, advertisements.room.service().public_key().begin()));
    assert(std::equal(direct_view.payload, direct_view.payload + 32, flood_view.payload));
    // Independently verify the signature after changing the signed timestamp.
    std::vector<uint8_t> signed_data(flood_view.payload, flood_view.payload + 36);
    signed_data.insert(signed_data.end(), flood_view.payload + 100, flood_view.payload + flood_view.payload_size);
    auto *key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, flood_view.payload, 32);
    auto *ctx = EVP_MD_CTX_new();
    assert(key && ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1);
    assert(EVP_DigestVerify(ctx, flood_view.payload + 36, 64, signed_data.data(), signed_data.size()) == 1);
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    Fixture reboot;
    assert(reboot.start("advert", seed));
    assert(reboot.room.create_advertisement(EPOCH, false, direct));
    assert(parse_packet(direct.bytes.data(), direct.size, direct_view));
    assert(read_u32(direct_view.payload + 32) > read_u32(flood_view.payload + 32));
    storage.fail_commit = true;
    assert(!reboot.room.create_advertisement(EPOCH + 100, true, flood));
    storage.fail_commit = false;
  }
  {
    Fixture exhausted;
    assert(exhausted.start("exhaust", seed));
    Frame advert;
    assert(exhausted.room.create_advertisement(UINT32_MAX, true, advert));
    assert(!exhausted.room.create_advertisement(UINT32_MAX, false, advert));
    Fixture reboot;
    assert(reboot.start("exhaust", seed));
    assert(!reboot.room.create_advertisement(UINT32_MAX, false, advert));
  }
  std::cout << "PASS: static first boot, no-op reboot, password apply, identity/history retention, public/private, "
               "native import/signature/ECDH/v2 storage, duplicate/conflicting identities, legacy startup and "
               "failed/corrupt storage, signed direct/flood advert timestamps/reboot/exhaustion\n";
}
