#pragma once
#include "../meshcore_core/room_service.h"
#include "nvs.h"
#include <string>
namespace esphome::meshcore {
struct RoomCredentials {
  std::array<uint8_t, 32> seed{};
  std::array<uint8_t, 64> private_key{};
  bool has_private_key{};
  std::array<char, 33> password{};
  uint32_t epoch{};
};
enum class StoreResult { MISSING, OK, ERROR };
// One stable namespace per room. No global preferences or shared filenames.
class NvsRoomStore final : public ::meshcore_lib::RoomRepository {
 public:
  /// Close the owned NVS handle without deleting credentials, posts or the clock floor.
  ~NvsRoomStore() override;
  /// Open mc_<stable_id> once; false for empty/overlength IDs, an already open handle or SDK failure.
  bool open(const std::string &stable_id);
  /// Decode V1 seed or V2 key credentials; distinguish missing, structurally valid and invalid records.
  /// out is private material usable only on OK; the runtime checks cryptographic key validity separately.
  StoreResult load_credentials(RoomCredentials &out);
  /// Commit/readback-verify a missing credential record; existing exact matches are a no-write success.
  /// Reject mismatches/corruption. Caller validates key/password policy before invoking this method.
  bool provision(const RoomCredentials &credentials);
  /// Commit/readback-verify a 12-15 byte password while preserving the key representation and history.
  /// Unchanged values avoid writes; failure/uncertainty stops further writes on this instance.
  bool change_password(const char *password);
  /// Load and validate posts and clock floor; missing history is empty and a missing floor is zero.
  bool load_history();
  /// Persist a higher epoch-second floor and verify it; no write is made when it has not advanced.
  bool checkpoint(uint32_t epoch);
  /// Return the loaded/checkpointed floor, including the latest retained post timestamp.
  uint32_t clock_floor() const { return this->clock_floor_; }
  /// Durably append a newer valid post, evicting the oldest at capacity; update RAM only after verification.
  /// False means rejection or uncertain persistence, and the caller must withhold its message ACK.
  bool append(const ::meshcore_lib::RoomPost &post) override;
  /// Return the number of retained posts loaded or verified by this instance, at most ROOM_HISTORY_CAPACITY.
  size_t count() const override { return this->count_; }
  /// Copy the retained post at an oldest-first index; false when the index is out of range.
  bool get(size_t index, ::meshcore_lib::RoomPost &out) const override;
  /// Return the newest retained post's epoch timestamp, or zero when history is empty.
  uint32_t latest_timestamp() const override { return this->count_ ? this->posts_[this->count_ - 1].timestamp : 0; }

 private:
  bool write_blob_(const char *key, const uint8_t *data, size_t size);
  bool write_credentials_(const RoomCredentials &credentials);
  nvs_handle_t handle_{};
  bool opened_{};
  bool healthy_{true};  // An uncertain write requires reboot/reload before more writes.
  std::array<::meshcore_lib::RoomPost, ::meshcore_lib::ROOM_HISTORY_CAPACITY> posts_{};
  size_t count_{};
  uint32_t clock_floor_{};
};
}  // namespace esphome::meshcore
