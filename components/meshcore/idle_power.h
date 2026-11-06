#pragma once

namespace esphome::meshcore {
// Platform policy, called only from the ESPHome owning loop. Portable services
// supply deadlines and never see MCU sleep APIs.
class IdlePower {
 public:
  /// Allow policy destruction through this port; callers do not own the injected policy.
  virtual ~IdlePower() = default;
  /// Update the owner-loop busy state; false allows the platform policy to consider idle sleep.
  virtual void set_mesh_busy(bool busy) = 0;
  /// Notify local management activity so the policy can extend its awake window.
  virtual void management_activity() = 0;
  /// Emit platform power diagnostics on explicit request; this is not a current/energy measurement.
  virtual void log_status() = 0;
};
}  // namespace esphome::meshcore
