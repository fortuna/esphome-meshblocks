#pragma once
#include "esphome/core/hal.h"
#include <vector>
namespace esphome::spi {
constexpr int BIT_ORDER_MSB_FIRST = 0, CLOCK_POLARITY_LOW = 0, CLOCK_PHASE_LEADING = 0, DATA_RATE_8MHZ = 0;
// Minimal command-level chip model. Only models commands needed for TX/RX
// transitions, not RF behavior. Tests compile the actual patched driver.
struct FakeChip {
  uint16_t irq{};
  uint32_t tx_timeout_ticks{};
  bool transmitting{};
  bool receiving{};
  unsigned starts{};
  uint8_t packet_length{};
  std::vector<uint8_t> command;
  std::vector<uint8_t> rx{0x15, 0x40, 0x42};
  std::vector<std::vector<uint8_t>> writes;
  void begin() { this->command.clear(); }
  uint8_t transfer(uint8_t byte) {
    this->command.push_back(byte);
    const size_t pos = this->command.size() - 1;
    if (pos == 0)
      return 0;
    if (this->command[0] == 0x12 && pos == 2)
      return this->irq >> 8;
    if (this->command[0] == 0x12 && pos == 3)
      return this->irq & 255;
    if (this->command[0] == 0x13 && pos == 2)
      return this->rx.size();
    if (this->command[0] == 0x14 && pos == 2)
      return 80;
    if (this->command[0] == 0x14 && pos == 3)
      return 40;
    if (this->command[0] == 0x1e && pos >= 3 && pos - 3 < this->rx.size())
      return this->rx[pos - 3];
    return 0;
  }
  void end() {
    if (this->command.empty())
      return;
    this->writes.push_back(this->command);
    if (this->command[0] == 0x02 && this->command.size() == 3)
      this->irq &= ~((this->command[1] << 8) | this->command[2]);
    if (this->command[0] == 0x80) {
      this->transmitting = false;
      this->receiving = false;
    }
    if (this->command[0] == 0x83) {
      ++this->starts;
      this->transmitting = true;
      this->receiving = false;
      this->tx_timeout_ticks = (this->command[1] << 16) | (this->command[2] << 8) | this->command[3];
    }
    if (this->command[0] == 0x82) {
      this->transmitting = false;
      this->receiving = true;
    }
    if (this->command[0] == 0x8c)
      this->packet_length = this->command[4];
  }
};
template<int, int, int, int> class SPIDevice {
 public:
  void set_test_chip(FakeChip *chip) { this->chip_ = chip; }
  void spi_setup() {}
  void enable() { this->chip_->begin(); }
  void disable() { this->chip_->end(); }
  uint8_t transfer_byte(uint8_t byte) { return this->chip_->transfer(byte); }
  void write_byte(uint8_t byte) { this->chip_->transfer(byte); }

 private:
  FakeChip *chip_{};
};
}  // namespace esphome::spi
