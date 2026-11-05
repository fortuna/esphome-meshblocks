#pragma once
#include "ed25519_identity.h"
#include "nvs_room_store.h"
#include "../meshcore_core/node_runtime.h"
#include <vector>
namespace esphome::meshcore {
class EspRandom final : public ::meshcore_lib::RandomSource {
 public:
  /// Fill size bytes through ESP-IDF's random source; this adapter currently always reports success.
  bool fill(uint8_t *data, size_t size) override;
};
// ESPHome composition adapter; the RoomService below has no platform headers.
class RoomRuntime {
 public:
  /// Compose one room and its NVS/identity adapters; borrowed dependencies must outlive this runtime.
  RoomRuntime(::meshcore_lib::CryptoProvider &crypto, ::meshcore_lib::RoomTransport &transport,
              ::meshcore_lib::NodeStatusSource &status)
      : protocol_(crypto, this->identities_),
        service_(this->protocol_, transport, this->store_, this->random_, status) {}
  /// Copy the stable storage ID, advertised name and policy before static credentials/setup.
  /// Changing id selects a different namespace; it does not migrate stored state.
  void set_config(const std::string &id, const std::string &name, ::meshcore_lib::RoomAccess access) {
    this->id_ = id;
    this->name_ = name;
    this->access_ = access;
  }
  /// Return the configured policy as the static string "public" or "private".
  const char *access_name() const { return this->access_ == ::meshcore_lib::RoomAccess::PUBLIC ? "public" : "private"; }
  /// Report whether a nonempty stable ID has been configured, not whether credentials are ready.
  bool enabled() const { return !this->id_.empty(); }
  /// Report successful service activation; this says nothing about radio or UTC readiness.
  bool ready() const { return this->ready_; }
  /// Copy initial key material and password before setup; false for invalid lengths/content.
  /// seed accepts a 32-byte legacy seed or 64-byte native key; public access ignores password.
  /// Key derivation, collision checks and durable application occur during setup, not here.
  bool set_static_credentials(const std::vector<uint8_t> &seed, const std::string &password);
  /// Open/load storage once and activate valid credentials, applying static inputs when present.
  /// initial_epoch is ordering time in epoch seconds; node supplies existing-identity checks.
  /// False means storage/key failure; true may still leave a legacy room unprovisioned/not ready.
  /// Disabled rooms return true without opening storage.
  bool setup(uint32_t initial_epoch, const ::meshcore_lib::NodeRuntime &node);
  /// Provision a legacy room from 64 lowercase seed hex characters and a 12-15 byte password.
  /// Requires a valid 2024-2099 epoch; rejects static rooms, identity collisions and stored mismatches.
  /// Success confirms matching/persisted credentials and activation, without registering with node.
  bool provision(const char *id, const char *seed_hex, const char *password, uint32_t epoch,
                 const ::meshcore_lib::NodeRuntime &node);
  /// Durably change a ready legacy private room's password, retaining keys/history.
  /// Changed passwords clear sessions; unchanged values avoid writes. Static/public rooms reject this.
  bool change_password(const char *password);
  /// Persist a ready room's ordering floor in epoch seconds; false if unavailable or the store fails.
  bool checkpoint(uint32_t epoch) { return this->ready_ && this->store_.checkpoint(epoch); }
  /// Checkpoint an increasing per-room epoch and sign a direct/empty-path or optionally scoped flood advert.
  /// scope is borrowed for this call and rejected for direct mode or when invalid; no RF send occurs here.
  /// Reject unavailable/zero time, epoch exhaustion or storage/crypto failure. A successful call consumes the
  /// timestamp even if the caller exports the frame or cannot enqueue it. Existing identities/history are retained.
  bool create_advertisement(uint32_t epoch, bool flood, ::meshcore_lib::Frame &out,
                            const ::meshcore_lib::FloodScope *scope = nullptr);
  /// Return the activation-time ordering floor (advanced one second when possible), not fresh UTC.
  uint32_t restored_epoch() const { return this->restored_epoch_; }
  /// Borrow the stable ID; the returned string belongs to this runtime.
  const std::string &id() const { return this->id_; }
  /// Borrow the advertised name; renaming it alone does not change the storage binding.
  const std::string &name() const { return this->name_; }
  /// Borrow the owned service for owner-context dispatch/registration; ready() gates active use.
  ::meshcore_lib::RoomService &service() { return this->service_; }
  /// Borrow the owned service for read-only inspection; this reference does not transfer ownership.
  const ::meshcore_lib::RoomService &service() const { return this->service_; }

 private:
  bool activate_(const RoomCredentials &credentials);
  bool derive_identity_(const RoomCredentials &credentials, ::meshcore_lib::LocalIdentity &identity);
  bool apply_static_credentials_(RoomCredentials &credentials, StoreResult state, uint32_t initial_epoch,
                                 const ::meshcore_lib::NodeRuntime &node);
  Ed25519Identity identities_;
  EspRandom random_;
  NvsRoomStore store_;
  ::meshcore_lib::RoomProtocol protocol_;
  ::meshcore_lib::RoomService service_;
  std::string id_, name_;
  ::meshcore_lib::RoomAccess access_{::meshcore_lib::RoomAccess::PRIVATE};
  bool ready_{};
  bool static_credentials_{};
  RoomCredentials initial_credentials_{};
  uint32_t restored_epoch_{};
  uint32_t last_advert_epoch_{};
};
}  // namespace esphome::meshcore
