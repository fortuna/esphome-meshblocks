#pragma once
#include "esphome/components/meshcore_core/radio_scheduler.h"
#include "esphome/components/sx126x/sx126x.h"

namespace esphome::meshcore {
// Adapts chip events to the portable scheduler. No worker or second SPI owner.
class Sx126xRadio final : public ::meshcore_lib::AsyncPacketRadio, public sx126x::SX126xTransmitListener {
 public:
  /// Bind a borrowed initialized driver for owner-context use; false for nullptr or a failed driver.
  bool attach(sx126x::SX126x *radio);
  /// Start asynchronous TX with a millisecond timeout; the driver copies the frame before returning.
  /// False means admission failed; normal ESPHome driver processing delivers completion callbacks.
  bool start(const ::meshcore_lib::Frame &frame, uint32_t timeout_ms) override;
  /// Consume the recorded completion once, otherwise return PENDING; this method performs no SPI polling.
  ::meshcore_lib::RadioResult poll() override;
  /// Synchronously cancel and consume completion; true requires confirmed CANCELLED status with RX restored.
  bool cancel() override;
  /// Record a driver completion for poll(); this callback does not dispatch or mutate room state.
  void on_transmit_complete(sx126x::SX126xError result) override;

 private:
  sx126x::SX126x *radio_{};
  ::meshcore_lib::RadioResult result_{::meshcore_lib::RadioResult::PENDING};
  sx126x::SX126xError completion_{sx126x::SX126xError::NONE};
};
}  // namespace esphome::meshcore
