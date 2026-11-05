#include "sx126x_radio.h"

namespace esphome::meshcore {
bool Sx126xRadio::attach(sx126x::SX126x *radio) {
  if (radio == nullptr || radio->is_failed()) {
    return false;
  }
  this->radio_ = radio;
  return true;
}

bool Sx126xRadio::start(const ::meshcore_lib::Frame &frame, uint32_t timeout_ms) {
  if (this->radio_ == nullptr || frame.size == 0 || frame.size > frame.bytes.size()) {
    return false;
  }
  this->result_ = ::meshcore_lib::RadioResult::PENDING;
  // The driver copies the frame into its FIFO before returning.
  return this->radio_->start_transmit_async(this, frame.bytes.data(), frame.size, timeout_ms) ==
         sx126x::SX126xError::NONE;
}

::meshcore_lib::RadioResult Sx126xRadio::poll() {
  auto result = this->result_;
  this->result_ = ::meshcore_lib::RadioResult::PENDING;
  return result;
}

bool Sx126xRadio::cancel() {
  this->completion_ = sx126x::SX126xError::NONE;
  const bool accepted = this->radio_ != nullptr && this->radio_->cancel_transmit_async(this);
  // Cancellation invokes our callback synchronously. Acceptance alone does not
  // mean restoration succeeded; the portable port promises restored RX.
  const bool restored = accepted && this->completion_ == sx126x::SX126xError::SX126X_ERROR_CANCELLED;
  this->result_ = ::meshcore_lib::RadioResult::PENDING;
  return restored;
}

void Sx126xRadio::on_transmit_complete(sx126x::SX126xError result) {
  using Result = ::meshcore_lib::RadioResult;
  this->completion_ = result;
  this->result_ = result == sx126x::SX126xError::NONE      ? Result::SENT
                  : result == sx126x::SX126xError::TIMEOUT ? Result::TIMEOUT
                                                           : Result::FAILED;
}
}  // namespace esphome::meshcore
