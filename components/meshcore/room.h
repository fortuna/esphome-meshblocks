#pragma once

#include "esphome/components/meshcore_core/room_protocol.h"
#include "esphome/core/automation.h"
#include <string>
#include <vector>

namespace esphome::meshcore {
class MeshCoreComponent;

// Typed ESPHome handle; the parent retains the room runtime and durable state.
class Room {
 public:
  /// Bind a borrowed node and copy the stable storage ID during composition; the node must outlive this handle.
  Room(MeshCoreComponent *parent, const std::string &storage_id) : parent_(parent), storage_id_(storage_id) {}
  /// Queue one signed advertisement via the parent's room operation, without owning a separate radio or timer.
  /// True means admission only; invalid scope, unavailable room/time/radio or full queue returns false and logs.
  bool advertise(bool flood = false, const ::meshcore_lib::FloodScope *scope = nullptr);
  /// Borrow the stable storage/USB ID; it can differ from this handle's ESPHome ID.
  const std::string &storage_id() const { return this->storage_id_; }

 private:
  MeshCoreComponent *parent_;
  std::string storage_id_;
};

template<typename... Ts> class AdvertiseRoomAction : public Action<Ts...> {
 public:
  /// Borrow the room handle and copy mode/scope at composition; no separate node reference is needed.
  /// Empty scope_key selects unscoped; invalid nonempty input remains invalid and is rejected when played.
  AdvertiseRoomAction(Room *room, bool flood, const std::vector<uint8_t> &scope_key = {})
      : room_(room), flood_(flood), scoped_(!scope_key.empty()) {
    if (this->scoped_)
      ::meshcore_lib::make_flood_scope(scope_key.data(), scope_key.size(), this->scope_);
  }
  /// Attempt one enqueue in the owner context; the room logs rejection without scheduling or retrying.
  void play(const Ts &...) override { this->room_->advertise(this->flood_, this->scoped_ ? &this->scope_ : nullptr); }

 private:
  Room *room_;
  bool flood_;
  bool scoped_;
  ::meshcore_lib::FloodScope scope_{};
};
}  // namespace esphome::meshcore
