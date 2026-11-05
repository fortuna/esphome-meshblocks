#include "epoch_clock.h"
#include "channel_timestamp.h"
#include <cassert>
#include <cstdint>
#include <iostream>

using meshcore_lib::EpochClock;

int main() {
  constexpr uint32_t epoch = 1800000000;
  EpochClock source, protocol, unrelated;
  assert(source.now(5000) == 0);
  assert(!EpochClock::valid(0));
  assert(!EpochClock::valid(uint64_t(UINT32_MAX) + epoch));
  assert(EpochClock::valid(EpochClock::MIN_EPOCH));
  assert(EpochClock::valid(EpochClock::MAX_EPOCH));
  assert(!EpochClock::valid(EpochClock::MAX_EPOCH + 1ULL));

  source.advance(epoch, 5000);
  protocol.advance(epoch + 20, 5000);  // Durable room floor ahead of source.
  for (uint64_t ms = 5000; ms < 8000; ms += 17) {
    source.advance(epoch, ms);  // Repeated/equal values must not freeze the clock.
    protocol.advance(source.now(ms), ms);
  }
  assert(source.now(8000) == epoch + 3);
  assert(protocol.now(8000) == epoch + 23);
  assert(unrelated.now(8000) == 0);

  protocol.advance(epoch + 100, 8500);  // A real forward correction.
  assert(protocol.now(9500) == epoch + 101);
  protocol.advance(epoch + 1, 9600);           // Source goes backward.
  protocol.advance(0, 9700);                   // Source becomes unavailable.
  assert(protocol.now(10500) == epoch + 102);  // Holdover continues.

  EpochClock after_reboot;
  after_reboot.advance(protocol.now(10500) + 1, 0);
  assert(after_reboot.now(2000) == epoch + 105);
  // Holdover measures only uptime, never inventing elapsed time during power loss.
  assert(source.now(5000 + (1ULL << 32)) == epoch + (1ULL << 32) / 1000);

  EpochClock overflow;
  overflow.advance(UINT32_MAX - 1, 0);
  assert(overflow.now(3000) == UINT32_MAX);  // Never wraps back to 1970.
  assert(overflow.now(UINT64_MAX) == UINT32_MAX);
  meshcore_lib::ChannelTimestamp channel, independent;
  using Quality = meshcore_lib::ChannelTimestamp::Quality;
  channel.seed(epoch, 0);  // Build timestamp is only an estimate, not UTC sync.
  assert(channel.next(0, 0, 5000) == epoch + 5);
  assert(channel.quality() == Quality::ESTIMATED);
  assert(channel.next(0, epoch + 100, 6000) == epoch + 100);
  assert(channel.next(0, 0, 7000) == epoch + 101);
  assert(channel.next(epoch + 200, 0, 8000) == epoch + 200);
  assert(channel.quality() == Quality::SOURCE);
  assert(channel.next(epoch + 200, 0, 8001) == epoch + 201);
  assert(channel.quality() == Quality::ADJUSTED);
  assert(channel.next(epoch + 50, 0, 9000) == epoch + 202);
  assert(channel.quality() == Quality::ADJUSTED);  // Explicitly report backward correction.
  assert(channel.next(0, 0, 86409000) == epoch + 50 + 86400);
  assert(channel.quality() == Quality::ESTIMATED);  // Expired source keeps holdover.
  meshcore_lib::ChannelTimestamp corrected;
  corrected.seed(epoch + 5000, 0);  // Bad build clock or stored floor.
  assert(corrected.next(epoch, epoch + 9000, 1000) == epoch);
  assert(corrected.next(0, epoch + 9000, 3000) == epoch + 2);
  independent.seed(epoch, 0);
  assert(independent.next(0, 0, 1000) == epoch + 1);
  assert(independent.next(0, 0, 0x100000000ULL) == epoch + 0x100000000ULL / 1000);
  meshcore_lib::ChannelTimestamp exhausted;
  exhausted.seed(UINT32_MAX, 0);
  assert(exhausted.next(0, 0, 0) == UINT32_MAX);
  assert(exhausted.next(0, 0, 1) == 0 && exhausted.quality() == Quality::EXHAUSTED);
  assert(exhausted.last() == UINT32_MAX);  // Never wrap to a plausible but wrong epoch.
  std::cout << "Channel timestamps: boot estimate, restored floor, source/holdover, uniqueness, correction, isolation, "
               "rollover and exhaustion passed\n";
  std::cout
      << "Epoch clock: source loss/backward steps, reboot floors, fractional uptime, isolation and overflow passed\n";
}
