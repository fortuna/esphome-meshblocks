#pragma once

#include "epoch_clock.h"

namespace meshcore_lib {

// Optional caller policy, separate from encoding/sending. No clock ownership or
// persistence APIs: inputs are samples supplied by the platform adapter.
class ChannelTimestamp {
 public:
  enum class Quality { SOURCE, ESTIMATED, ADJUSTED, EXHAUSTED };
  /// Advance the initial estimate with epoch seconds at a monotonic-ms anchor; does not reset prior selections.
  void seed(uint32_t epoch, uint64_t now_ms) { this->estimate_.advance(epoch, now_ms); }
  /// Choose a nonzero timestamp from supplied UTC, holdover or initial estimate; source=0 means unavailable.
  /// protocol_floor is epoch seconds and now_ms is monotonic milliseconds. Selections increase within this instance;
  /// return zero on uint32 exhaustion without changing last(). This never synchronizes a clock or persists state.
  uint32_t next(uint32_t source, uint32_t protocol_floor, uint64_t now_ms) {
    if (source) {
      // A real source supersedes the build/restored estimate, even if the build
      // machine or stored floor was ahead. Keep its holdover after source loss.
      if (!this->had_source_ || source < this->estimate_.now(now_ms))
        this->estimate_ = {};
      this->estimate_.advance(source, now_ms);
      this->had_source_ = true;
    } else if (!this->had_source_) {
      this->estimate_.advance(protocol_floor, now_ms);
    }
    uint32_t value = source ? source : this->estimate_.now(now_ms);
    this->quality_ = source ? Quality::SOURCE : Quality::ESTIMATED;
    // Preserve within-boot uniqueness even during same-second sends or backward
    // corrections. A caller needing exact timestamps can bypass this policy.
    if (value <= this->last_) {
      if (this->last_ == UINT32_MAX) {
        this->quality_ = Quality::EXHAUSTED;
        return 0;
      }
      value = this->last_ + 1;
      if (source)
        this->quality_ = Quality::ADJUSTED;
    }
    return this->last_ = value;
  }
  /// Return the last successfully selected timestamp, or zero before the first selection.
  uint32_t last() const { return this->last_; }
  /// Describe the most recent selection attempt: source, estimate, adjusted source or exhausted counter.
  Quality quality() const { return this->quality_; }

 private:
  EpochClock estimate_;
  uint32_t last_{};
  bool had_source_{};
  Quality quality_{Quality::ESTIMATED};
};
}  // namespace meshcore_lib
