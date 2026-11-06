#pragma once
#include "hal.h"
namespace esphome {
namespace setup_priority {
constexpr float PROCESSOR = 400;
}
class Component {
 public:
  bool failed{}, loop_enabled{true};
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return 600; }
  void mark_failed() { this->failed = true; }
  bool is_failed() const { return this->failed; }
  void disable_loop() { this->loop_enabled = false; }
  void enable_loop() { this->loop_enabled = true; }
  void enable_loop_soon_any_context() { this->loop_enabled = true; }
};
}  // namespace esphome
