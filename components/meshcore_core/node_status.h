#pragma once
#include <cstdint>
#include <optional>

namespace meshcore_lib {
// Device-wide observations supplied by the platform owner. Missing measurements
// are explicit here; the legacy wire format has no validity bits and uses zero.
struct NodeStatus {
  std::optional<uint16_t> battery_mv;
  std::optional<int16_t> noise_floor;
  uint16_t tx_queue_length{};
  int16_t last_rssi{}, last_snr_quarters{};
  uint32_t packets_received{}, packets_sent{}, tx_airtime_seconds{}, uptime_seconds{};
  uint32_t sent_flood{}, sent_direct{}, received_flood{}, received_direct{};
  std::optional<uint16_t> error_flags, direct_duplicates, flood_duplicates;
};
class NodeStatusSource {
 public:
  /// Allow adapter destruction through the status port; consumers borrow the provider.
  virtual ~NodeStatusSource() = default;
  /// Copy bounded device observations without polling hardware, allocating or transferring ownership.
  /// Unavailable measurements must remain unset rather than be invented as measured zeroes.
  virtual NodeStatus snapshot() const = 0;
};
}  // namespace meshcore_lib
