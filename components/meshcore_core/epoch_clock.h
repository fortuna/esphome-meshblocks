#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace meshcore_lib {

// Epoch anchor advanced by an injected monotonic clock. No platform/global state.
class EpochClock {
 public:
  static constexpr uint32_t MIN_EPOCH = 1704067200UL;  // 2024
  static constexpr uint32_t MAX_EPOCH = 4102444799UL;  // end of 2099
  /// Check whether an epoch-second sample is within the accepted 2024-2099 input range.
  static bool valid(uint64_t epoch) { return epoch >= MIN_EPOCH && epoch <= MAX_EPOCH; }

  /// Return epoch seconds advanced by monotonic milliseconds; zero means unseeded, UINT32_MAX is saturation.
  uint32_t now(uint64_t now_ms) const {
    if (!this->epoch_)
      return 0;
    const uint64_t elapsed = now_ms >= this->anchor_ms_ ? (now_ms - this->anchor_ms_) / 1000 : 0;
    return static_cast<uint32_t>(
        std::min<uint64_t>(uint64_t(this->epoch_) + elapsed, std::numeric_limits<uint32_t>::max()));
  }

  /// Accept only a forward advance at now_ms; repeated/older samples do not reset fractional elapsed time.
  /// Caller validates epoch inputs; this method neither writes persistent storage nor synchronizes a UTC source.
  void advance(uint32_t epoch, uint64_t now_ms) {
    if (epoch > this->now(now_ms)) {
      this->epoch_ = epoch;
      this->anchor_ms_ = now_ms;
    }
  }

 private:
  uint32_t epoch_{};
  uint64_t anchor_ms_{};
};
}  // namespace meshcore_lib
