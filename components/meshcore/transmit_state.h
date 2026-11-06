#pragma once

#include "esphome/components/meshcore_core/channel_codec.h"

namespace esphome::meshcore {

struct TransmitMetadata {
  bool channel{}, flood{};
  std::array<uint8_t, 8> fingerprint{};
};

// Match exact tokens rather than token % capacity: arbitrary capacities must
// remain correct across uint32 token rollover, including mixed room/channel TX.
template<size_t Capacity> class TransmitState {
 public:
  static_assert(Capacity > 0, "TransmitState requires at least one transmit slot");
  /// Copy metadata before queue admission; reject full storage or a duplicate live token.
  bool remember(uint32_t token, const TransmitMetadata &metadata) {
    for (const auto &entry : this->entries_)
      if (entry.used && entry.token == token)
        return false;
    for (auto &entry : this->entries_) {
      if (entry.used)
        continue;
      entry = {true, token, metadata};
      return true;
    }
    return false;
  }
  /// Consume metadata for a completed/rejected send; unknown tokens leave output unchanged.
  bool take(uint32_t token, TransmitMetadata &out) {
    for (auto &entry : this->entries_) {
      if (!entry.used || entry.token != token)
        continue;
      out = entry.metadata;
      entry = {};
      return true;
    }
    return false;
  }

 private:
  struct Entry {
    bool used{};
    uint32_t token{};
    TransmitMetadata metadata;
  };
  std::array<Entry, Capacity> entries_{};
};
}  // namespace esphome::meshcore
