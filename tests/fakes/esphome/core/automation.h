#pragma once
namespace esphome {
template<typename... Args> class Trigger {
 public:
  void trigger(Args...) {}
};
}  // namespace esphome
