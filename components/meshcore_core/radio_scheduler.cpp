#include "radio_scheduler.h"
#include <algorithm>

namespace meshcore_lib {
uint64_t RadioScheduler::next_deadline(uint64_t now_ms) const {
  if (this->count_ == 0)
    return UINT64_MAX;
  if (this->active_)
    return now_ms;
  return std::max(now_ms, std::max(this->next_send_at_, this->queue_[this->head_].not_before_ms));
}
bool RadioScheduler::enqueue(const Frame &frame, uint32_t token, uint64_t not_before_ms) {
  if (frame.size == 0 || frame.size > frame.bytes.size() || this->count_ == this->capacity_)
    return false;
  this->queue_[(this->head_ + this->count_) % this->capacity_] = {frame, token, not_before_ms};
  ++this->count_;
  return true;
}

bool RadioScheduler::tick(uint64_t now_ms, TransmitEvent &event) {
  event = {};
  if (this->count_ == 0)
    return false;
  if (this->active_) {
    auto result = this->radio_.poll();
    if (result == RadioResult::PENDING && now_ms - this->started_at_ >= TX_TIMEOUT_MS) {
      result = this->radio_.cancel() ? RadioResult::TIMEOUT : RadioResult::FAILED;
    }
    if (result != RadioResult::PENDING)
      return this->finish_(now_ms, result, event);
    return false;
  }
  if (now_ms < this->next_send_at_ || now_ms < this->queue_[this->head_].not_before_ms)
    return false;
  this->started_at_ = now_ms;
  if (!this->radio_.start(this->queue_[this->head_].frame, TX_TIMEOUT_MS)) {
    // Restore a known state even if a partial start changed radio mode.
    this->radio_.cancel();
    return this->finish_(now_ms, RadioResult::FAILED, event);
  }
  this->active_ = true;
  return false;
}

bool RadioScheduler::finish_(uint64_t now_ms, RadioResult result, TransmitEvent &event) {
  const auto &job = this->queue_[this->head_];
  event = {job.token, result, static_cast<uint32_t>(now_ms - this->started_at_), job.frame.size};
  this->head_ = (this->head_ + 1) % this->capacity_;
  --this->count_;
  this->active_ = false;
  this->next_send_at_ = now_ms + this->send_gap_ms_;
  return true;
}
}  // namespace meshcore_lib
