#include "radio_scheduler.h"
#include <cassert>
#include <iostream>
#include <vector>

using namespace meshcore_lib;
class FakeRadio final : public AsyncPacketRadio {
 public:
  bool start_ok{true}, cancel_ok{true}, in_tx{};
  unsigned cancels{};
  RadioResult next{RadioResult::PENDING};
  std::vector<Frame> sent;
  bool start(const Frame &frame, uint32_t timeout) override {
    assert(!this->in_tx && timeout == 4000);
    if (!this->start_ok)
      return false;
    this->sent.push_back(frame);
    this->in_tx = true;
    return true;
  }
  RadioResult poll() override {
    auto result = this->next;
    this->next = RadioResult::PENDING;
    if (result != RadioResult::PENDING)
      this->in_tx = false;
    return result;
  }
  bool cancel() override {
    ++this->cancels;
    this->in_tx = false;
    this->next = RadioResult::PENDING;
    return this->cancel_ok;
  }
};

template<size_t Capacity> void check_capacity() {
  FakeRadio radio;
  std::array<TransmitJob, Capacity> storage{};
  RadioScheduler scheduler(radio, storage);
  assert(scheduler.capacity() == Capacity && scheduler.set_send_gap(0));
  Frame frame;
  frame.size = 1;
  TransmitEvent event;
  // Repeated full/drain cycles wrap the ring, preserving owned bytes and FIFO
  // tokens across uint32 rollover for capacities that do not divide 2^32.
  uint32_t token = UINT32_MAX - 1;
  for (size_t cycle = 0; cycle < 3; ++cycle) {
    const uint32_t first = token;
    for (size_t i = 0; i < Capacity; ++i) {
      frame.bytes[0] = static_cast<uint8_t>(i);
      assert(scheduler.enqueue(frame, token++));
    }
    assert(!scheduler.enqueue(frame, token) && scheduler.outstanding() == Capacity);
    for (size_t i = 0; i < Capacity; ++i) {
      assert(!scheduler.tick(0, event) && scheduler.active());
      if (i == 0)
        assert(!scheduler.enqueue(frame, token));  // Active TX still occupies a slot.
      assert(radio.sent.back().bytes[0] == i);
      radio.next = RadioResult::SENT;
      assert(scheduler.tick(0, event) && event.token == first + static_cast<uint32_t>(i));
      assert(scheduler.outstanding() == Capacity - i - 1);
      // Refill only on the next cycle; a smaller count must allow admission.
      if (i + 1 < Capacity)
        assert(scheduler.outstanding() < scheduler.capacity());
    }
  }
}

int main() {
  check_capacity<1>();
  check_capacity<3>();
  check_capacity<8>();
  FakeRadio radio, independent;
  std::array<TransmitJob, 4> storage{}, other_storage{};
  RadioScheduler queue(radio, storage), other(independent, other_storage);
  Frame frame;
  frame.size = 37;
  frame.bytes[0] = 42;
  TransmitEvent event;
  assert(!queue.tick(0, event));
  assert(queue.next_deadline(0) == UINT64_MAX);
  for (uint32_t i = 1; i <= 4; ++i)
    assert(queue.enqueue(frame, i));
  assert(!queue.enqueue(frame, 5) && queue.outstanding() == 4);
  frame.bytes[0] = 99;  // Queue retains an owned copy, not borrowed callback data.
  assert(!queue.tick(0, event) && queue.active());
  assert(radio.sent.size() == 1 && radio.sent[0].bytes[0] == 42);
  assert(!queue.tick(100, event) && radio.sent.size() == 1);
  assert(queue.next_deadline(100) == 100);  // Active TX keeps the owner polling.
  assert(!other.tick(100, event) && independent.sent.empty());
  radio.next = RadioResult::SENT;
  assert(queue.tick(216, event) && event.token == 1 && event.elapsed_ms == 216 && event.bytes == 37);
  assert(queue.outstanding() == 3 && !queue.active());
  assert(queue.next_deadline(216) == 5216);  // Shared pacing survives idle sleep.
  assert(!queue.tick(5215, event) && radio.sent.size() == 1);
  assert(!queue.tick(5216, event) && radio.sent.size() == 2);
  assert(!queue.tick(9215, event));
  assert(queue.tick(9216, event) && event.token == 2 && event.result == RadioResult::TIMEOUT);
  assert(radio.cancels == 1 && !radio.in_tx);
  assert(!queue.tick(14216, event) && queue.active());
  radio.next = RadioResult::TIMEOUT;  // Hardware timeout restores RX through port.
  assert(queue.tick(14300, event) && event.token == 3 && event.result == RadioResult::TIMEOUT);
  assert(!queue.tick(19300, event));
  radio.next = RadioResult::SENT;
  assert(queue.tick(19500, event) && event.token == 4);
  assert(queue.outstanding() == 0 && !queue.tick(20000, event));

  radio.start_ok = false;
  assert(queue.enqueue(frame, 5));
  assert(queue.tick(25000, event) && event.result == RadioResult::FAILED && event.token == 5);
  assert(radio.cancels == 2 && queue.outstanding() == 0);
  radio.start_ok = true;
  radio.cancel_ok = false;
  assert(queue.enqueue(frame, 6));
  assert(!queue.tick(30000, event));
  assert(queue.tick(34000, event) && event.result == RadioResult::FAILED && event.token == 6);
  assert(queue.outstanding() == 0);

  // 64-bit monotonic time works across the 32-bit millisecond rollover.
  FakeRadio rollover_radio;
  std::array<TransmitJob, 4> rollover_storage{};
  RadioScheduler rollover(rollover_radio, rollover_storage);
  assert(rollover.enqueue(frame, 0));
  assert(!rollover.tick(0xfffffff0ULL, event));
  rollover_radio.next = RadioResult::SENT;
  assert(rollover.tick(0x100000100ULL, event) && event.elapsed_ms == 272);
  FakeRadio room_radio;
  std::array<TransmitJob, 4> room_storage{};
  RadioScheduler room_queue(room_radio, room_storage);
  room_queue.set_send_gap(100);
  assert(room_queue.enqueue(frame, 10, 300));
  assert(room_queue.enqueue(frame, 11, 0));
  assert(room_queue.next_deadline(0) == 300);  // FIFO head's reply delay wins.
  assert(!room_queue.tick(299, event) && room_radio.sent.empty());
  assert(!room_queue.tick(300, event) && room_radio.sent.size() == 1);
  room_radio.next = RadioResult::SENT;
  assert(room_queue.tick(500, event) && event.token == 10);
  assert(room_queue.next_deadline(500) == 600);
  assert(room_queue.next_deadline(700) == 700);  // Overdue work runs immediately.
  assert(!room_queue.tick(599, event) && room_radio.sent.size() == 1);
  assert(!room_queue.tick(600, event) && room_radio.sent.size() == 2);
  frame.size = 0;
  assert(!queue.enqueue(frame, 7));
  frame.size = 256;
  assert(!queue.enqueue(frame, 7));
  std::cout << "PASS: bounded FIFO, owned buffers, deadlines, recovery, failure, two instances, clock rollover\n";
}
