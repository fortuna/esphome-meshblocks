#pragma once
#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#define IRAM_ATTR
#define TRUEFALSE(x) ((x) ? "true" : "false")
namespace esphome {
inline uint32_t test_ms{};
inline uint64_t test_ms64{};
inline uint64_t millis_64() { return test_ms64; }
inline uint32_t millis() { return test_ms++; }
inline void delayMicroseconds(uint32_t us) { test_ms += us / 1000; }
inline const char *format_hex_to(char *out, const uint8_t *, size_t) {
  out[0] = 0;
  return out;
}
namespace gpio {
constexpr int INTERRUPT_RISING_EDGE = 1;
}
class GPIOPin {
 public:
  bool value{};
  std::function<bool()> read;
  virtual bool is_internal() const { return false; }
  virtual void setup() {}
  virtual bool digital_read() { return this->read ? this->read() : this->value; }
  virtual void digital_write(bool v) { this->value = v; }
};
class InternalGPIOPin : public GPIOPin {
 public:
  bool is_internal() const override { return true; }
  template<typename T> void attach_interrupt(void (*)(T *), T *, int) {}
};
}  // namespace esphome
