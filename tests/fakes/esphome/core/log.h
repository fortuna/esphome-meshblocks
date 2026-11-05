#pragma once
#include "hal.h"
namespace esphome {
template<typename... Args> void test_log(Args...) {}
}  // namespace esphome
#define ESP_LOGE(...) ::esphome::test_log(__VA_ARGS__)
#define ESP_LOGW(...) ::esphome::test_log(__VA_ARGS__)
#define ESP_LOGI(...) ::esphome::test_log(__VA_ARGS__)
#define ESP_LOGCONFIG(...) ::esphome::test_log(__VA_ARGS__)
#define LOG_PIN(label, pin) ::esphome::test_log(label, pin)
