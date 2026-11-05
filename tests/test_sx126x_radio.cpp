#include "sx126x_radio.h"
#include <cassert>
#include <iostream>

using esphome::meshcore::Sx126xRadio;
using esphome::sx126x::SX126x;
using Error = esphome::sx126x::SX126xError;
using Result = meshcore_lib::RadioResult;

int main() {
  Sx126xRadio adapter;
  SX126x chip;
  meshcore_lib::Frame frame;
  frame.size = 3;
  frame.bytes[0] = 0x11;
  frame.bytes[1] = 0x22;
  frame.bytes[2] = 0x33;
  assert(!adapter.attach(nullptr));
  chip.failed = true;
  assert(!adapter.attach(&chip));
  assert(!adapter.start(frame, 4000));
  assert(!adapter.cancel());
  chip.failed = false;
  assert(adapter.attach(&chip));
  assert(adapter.poll() == Result::PENDING);

  auto invalid = frame;
  invalid.size = 0;
  assert(!adapter.start(invalid, 4000));
  invalid.size = invalid.bytes.size() + 1;
  assert(!adapter.start(invalid, 4000));
  assert(chip.starts == 0);

  for (const auto error : {Error::SX126X_ERROR_BUSY, Error::SX126X_ERROR_NOT_READY, Error::SX126X_ERROR_BUSY_TIMEOUT,
                           Error::INVALID_PARAMS}) {
    chip.start_result = error;
    assert(!adapter.start(frame, 4000));
    assert(adapter.poll() == Result::PENDING);
  }
  chip.start_result = Error::NONE;
  assert(adapter.start(frame, 1234));
  assert(chip.size == 3 && chip.deadline == 1234);
  frame.bytes[0] = 0x99;
  assert(chip.payload[0] == 0x11);  // The caller may reuse its frame after start.
  assert(!adapter.start(frame, 1234));
  assert(adapter.poll() == Result::PENDING);
  chip.complete(Error::NONE);
  assert(adapter.poll() == Result::SENT);
  assert(adapter.poll() == Result::PENDING);

  for (const auto error : {Error::TIMEOUT, Error::SX126X_ERROR_BUSY_TIMEOUT}) {
    assert(adapter.start(frame, 4000));
    chip.complete(error);
    assert(adapter.poll() == (error == Error::TIMEOUT ? Result::TIMEOUT : Result::FAILED));
    assert(adapter.poll() == Result::PENDING);
  }

  assert(adapter.start(frame, 4000));
  assert(adapter.cancel());
  assert(adapter.poll() == Result::PENDING);  // Cancellation consumes its synchronous callback.
  assert(!adapter.cancel());
  assert(adapter.start(frame, 4000));
  chip.cancel_result = Error::SX126X_ERROR_BUSY_TIMEOUT;
  assert(!adapter.cancel());  // Accepted cancellation with failed restoration is not success.
  assert(adapter.poll() == Result::PENDING);

  SX126x other_chip;
  Sx126xRadio other;
  assert(other.attach(&other_chip));
  assert(adapter.start(frame, 4000));
  assert(other.start(frame, 4000));
  other_chip.complete(Error::NONE);
  assert(other.poll() == Result::SENT);
  assert(adapter.poll() == Result::PENDING);
  chip.complete(Error::TIMEOUT);
  assert(adapter.poll() == Result::TIMEOUT);
  std::cout << "PASS: fork SX126x adapter payload lifetime, admission, completion mapping, cancellation restoration "
               "and isolation\n";
}
