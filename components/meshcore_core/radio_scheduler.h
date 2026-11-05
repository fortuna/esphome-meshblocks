#pragma once

#include "channel_codec.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace meshcore_lib {

enum class RadioResult { PENDING, SENT, TIMEOUT, FAILED };

// A single owner calls this port from one execution context. start() copies the
// frame before returning. poll() consumes exactly one completion; cancel()
// terminates TX, restores RX, and discards any late completion before reuse.
class AsyncPacketRadio {
 public:
  /// Allow adapter destruction through the radio port; the scheduler borrows the adapter.
  virtual ~AsyncPacketRadio() = default;
  /// Attempt one nonblocking TX, copying the frame before returning; timeout_ms bounds the driver operation.
  /// True admits an operation whose terminal result must later be consumed through poll(), unless cancelled.
  virtual bool start(const Frame &frame, uint32_t timeout_ms) = 0;
  /// Consume a terminal TX result once, or return PENDING; completion alone does not prove peer delivery.
  virtual RadioResult poll() = 0;
  /// Terminate TX, restore receive mode and discard late completion; false means restoration was not confirmed.
  virtual bool cancel() = 0;
};

struct TransmitEvent {
  uint32_t token{};
  RadioResult result{RadioResult::PENDING};
  uint32_t elapsed_ms{};
  size_t bytes{};
};

struct TransmitJob {
  Frame frame;
  uint32_t token{};
  uint64_t not_before_ms{};
};

class RadioScheduler {
 public:
  static constexpr uint32_t TX_TIMEOUT_MS = 4000;
  static constexpr uint32_t SEND_GAP_MS = 5000;
  /// Borrow the sole radio adapter and exclusive FIFO storage; both must outlive this scheduler.
  /// Capacity includes active TX. No allocation/resizing, retries or per-room budgets occur.
  template<size_t N>
  RadioScheduler(AsyncPacketRadio &radio, std::array<TransmitJob, N> &storage)
      : radio_(radio), queue_(storage.data()), capacity_(N) {
    static_assert(N > 0, "RadioScheduler requires at least one transmit slot");
  }
  /// Prevent copying a scheduler that owns exclusive radio/queue use.
  RadioScheduler(const RadioScheduler &) = delete;
  /// Prevent assignment that would duplicate radio/queue ownership.
  RadioScheduler &operator=(const RadioScheduler &) = delete;
  /// Copy a valid frame into the bounded FIFO, or return false without admission when invalid/full.
  /// Tokens are caller-assigned; not_before_ms is an absolute monotonic-ms deadline, not a delay.
  bool enqueue(const Frame &frame, uint32_t token, uint64_t not_before_ms = 0);
  /// Change the inter-transmission gap in milliseconds only while the queue is empty; otherwise return false.
  bool set_send_gap(uint32_t gap_ms) {
    if (this->count_ != 0)
      return false;
    this->send_gap_ms_ = gap_ms;
    return true;
  }
  /// Make at most one TX-start/completion transition using monotonic milliseconds.
  /// Return true only with a terminal event; false includes empty/waiting/just-started states and resets event.
  bool tick(uint64_t now_ms, TransmitEvent &event);
  /// Return the fixed capacity supplied by composition, including the active transmission slot.
  size_t capacity() const { return this->capacity_; }
  /// Count copied jobs including any active transmission, up to capacity().
  size_t outstanding() const { return this->count_; }
  /// Report whether an admitted job is currently waiting for radio completion.
  bool active() const { return this->active_; }
  /// Return the next absolute monotonic-ms service time, UINT64_MAX when empty, or now_ms while TX needs polling.
  uint64_t next_deadline(uint64_t now_ms) const;

 private:
  bool finish_(uint64_t now_ms, RadioResult result, TransmitEvent &event);
  AsyncPacketRadio &radio_;
  TransmitJob *queue_;
  size_t capacity_;
  size_t head_{};
  size_t count_{};
  bool active_{};
  uint64_t started_at_{};
  uint64_t next_send_at_{};
  uint32_t send_gap_ms_{SEND_GAP_MS};
};
}  // namespace meshcore_lib
