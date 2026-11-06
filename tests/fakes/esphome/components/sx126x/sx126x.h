#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <algorithm>

// Only the fork driver's public adapter-facing contract is simulated here.
// The driver's SPI/IRQ/recovery tests live in fortuna/esphome PR #10.
namespace esphome::sx126x {
enum class SX126xError {
  NONE,
  TIMEOUT,
  INVALID_PARAMS,
  SX126X_ERROR_NOT_READY,
  SX126X_ERROR_BUSY_TIMEOUT,
  SX126X_ERROR_BUSY,
  SX126X_ERROR_CANCELLED
};
class SX126xTransmitListener {
 public:
  virtual void on_transmit_complete(SX126xError result) = 0;
};
class SX126x {
 public:
  unsigned loop_wakes{};
  void enable_loop_soon_any_context() { ++this->loop_wakes; }
  bool is_failed() const { return this->failed; }
  SX126xError start_transmit_async(SX126xTransmitListener *listener, const uint8_t *data, size_t length,
                                   uint32_t timeout_ms) {
    ++this->starts;
    if (this->start_result != SX126xError::NONE) {
      return this->start_result;
    }
    if (this->owner != nullptr) {
      return SX126xError::SX126X_ERROR_BUSY;
    }
    std::copy(data, data + length, this->payload.begin());
    this->size = length;
    this->deadline = timeout_ms;
    this->owner = listener;
    return SX126xError::NONE;
  }
  bool cancel_transmit_async(SX126xTransmitListener *listener) {
    ++this->cancels;
    if (this->owner == nullptr || this->owner != listener) {
      return false;
    }
    this->complete(this->cancel_result);
    return true;
  }
  void complete(SX126xError result) {
    auto *listener = this->owner;
    this->owner = nullptr;
    listener->on_transmit_complete(result);
  }
  bool failed{};
  SX126xError start_result{SX126xError::NONE};
  SX126xError cancel_result{SX126xError::SX126X_ERROR_CANCELLED};
  SX126xTransmitListener *owner{};
  std::array<uint8_t, 255> payload{};
  size_t size{};
  uint32_t deadline{};
  unsigned starts{};
  unsigned cancels{};
};
}  // namespace esphome::sx126x
