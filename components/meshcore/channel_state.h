#pragma once

#include "esphome/components/meshcore_core/channel_codec.h"

namespace esphome::meshcore {

// One volatile, bounded preview per channel; never a persistent history or key.
class ChannelState {
 public:
  /// Record one accepted RX and copy a bounded raw UTF-8 preview; caller handles validation/duplicates.
  void received(const ::meshcore_lib::ChannelMessage &message) {
    ++this->received_;
    this->direction_ = Direction::RX;
    this->set_text_({message.text.data(), std::min(message.text_size, message.text.size())});
  }
  /// Record one successful physical TX and copy its preview; callers must not invoke this merely on enqueue.
  void sent(std::string_view text) {
    ++this->sent_;
    this->direction_ = Direction::TX;
    this->set_text_(text);
  }
  /// Return the volatile count of recorded RX events.
  uint32_t received_messages() const { return this->received_; }
  /// Return the volatile count of recorded successful TX events, not peer ACKs.
  uint32_t sent_messages() const { return this->sent_; }
  /// Report whether any RX or TX event has been recorded, even if its preview text is empty.
  bool has_message() const { return this->direction_ != Direction::NONE; }
  /// Return the static label "RX", "TX" or "--" for the most recent recorded event.
  const char *last_direction() const {
    return this->direction_ == Direction::TX ? "TX" : this->direction_ == Direction::RX ? "RX" : "--";
  }
  /// Borrow the owned NUL-terminated UTF-8 text until mutation/destruction.
  const char *last_text() const { return this->text_.data(); }

 private:
  void set_text_(std::string_view text) {
    const auto size = std::min(text.size(), this->text_.size() - 1);
    std::copy_n(text.begin(), size, this->text_.begin());
    this->text_[size] = 0;
  }
  enum class Direction : uint8_t { NONE, RX, TX };
  uint32_t received_{}, sent_{};
  std::array<char, ::meshcore_lib::MAX_RECEIVED_TEXT + 1> text_{};
  Direction direction_{Direction::NONE};
};

// Bounded message metadata for accepted sends. Only a matching successful radio
// completion changes the target preview; queueing/failure is not a sent message.
template<size_t Capacity> class ChannelTxState {
 public:
  static_assert(Capacity > 0, "ChannelTxState requires at least one transmit slot");
  /// Copy bounded text and retain target until completion; target must outlive this pending entry.
  /// False rejects a duplicate token or full storage; no TX counter is advanced here.
  bool remember(uint32_t token, ChannelState &target, const ::meshcore_lib::ChannelMessage &message) {
    for (const auto &pending : this->pending_)
      if (pending.target != nullptr && pending.token == token)
        return false;
    for (auto &pending : this->pending_) {
      if (pending.target != nullptr)
        continue;
      pending.token = token;
      pending.target = &target;
      pending.length = std::min(message.text_size, pending.text.size());
      std::copy_n(message.text.begin(), pending.length, pending.text.begin());
      return true;
    }
    return false;
  }
  /// Remove a matching entry, updating its target only for sent=true; true means the token was found.
  /// Unknown/stale tokens return false and do not change any preview.
  bool complete(uint32_t token, bool sent) {
    for (auto &pending : this->pending_) {
      if (pending.target == nullptr || pending.token != token)
        continue;
      if (sent)
        pending.target->sent({pending.text.data(), pending.length});
      pending = {};
      return true;
    }
    return false;
  }

 private:
  struct Pending {
    ChannelState *target{};
    uint32_t token{};
    size_t length{};
    std::array<char, ::meshcore_lib::MAX_RECEIVED_TEXT + 1> text{};
  };
  std::array<Pending, Capacity> pending_{};
};
}  // namespace esphome::meshcore
