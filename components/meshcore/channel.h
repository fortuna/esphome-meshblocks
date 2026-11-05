#pragma once

#include <vector>

#include "esphome/components/meshcore_core/channel_codec.h"
#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"
#include "channel_state.h"

namespace esphome::meshcore {
class MeshCoreComponent;
class Channel;
void configure_send_only_channel(MeshCoreComponent *parent, Channel *channel, const std::vector<uint8_t> &key);

// ESPHome resource handle; no radio ownership or independent scheduling loop.
class Channel {
 public:
  /// Copy this channel's ID/key/scope and bind a borrowed parent before use; the parent must outlive the channel.
  void configure(MeshCoreComponent *parent, const std::string &id, const ::meshcore_lib::ChannelKey &key,
                 const ::meshcore_lib::FloodScope &scope) {
    this->parent_ = parent;
    this->id_ = id;
    this->key_ = key;
    this->scope_ = scope;
  }
  /// Borrow the stable local configuration handle; it is not an authenticated on-air identity.
  const std::string &id() const { return this->id_; }
  bool belongs_to(const MeshCoreComponent *parent) const { return this->parent_ == parent; }
  /// Borrow the owned shared key for synchronous codec use; this contains private material for private channels.
  const ::meshcore_lib::ChannelKey &key() const { return this->key_; }
  /// Borrow the outbound scope; an invalid scope means unscoped flood and never acts as a receive ACL.
  const ::meshcore_lib::FloodScope &scope() const { return this->scope_; }
  /// Attempt one send through the parent's shared queue using the exact nonzero timestamp in epoch seconds.
  /// Text views are borrowed only during this call; true means admission, not TX completion or peer delivery.
  bool send_channel_message(std::string_view name, std::string_view message, uint32_t timestamp);
  /// Store a callback invoked synchronously for decoded RX; copy the borrowed message before retaining it.
  void add_on_message_callback(std::function<void(const ::meshcore_lib::ChannelMessage &)> callback) {
    this->callbacks_.add(std::move(callback));
  }
  /// Borrow this channel's volatile RX/TX counters and preview; it is neither retained history nor a delivery receipt.
  const ChannelState &state() const { return this->state_; }
  /// Copy pending message metadata for token; target state must outlive pending completion.
  /// False rejects duplicate tokens or full preview storage without recording a sent message.
  template<size_t Capacity>
  bool track_send(ChannelTxState<Capacity> &pending, uint32_t token, const ::meshcore_lib::ChannelMessage &message) {
    return pending.remember(token, this->state_, message);
  }
  /// Update the RX preview and invoke callbacks; the parent must first decode and suppress duplicate packets.
  void on_message(const ::meshcore_lib::ChannelMessage &message) {
    this->state_.received(message);
    this->callbacks_.call(message);
  }

 private:
  MeshCoreComponent *parent_{};
  std::string id_;
  ::meshcore_lib::ChannelKey key_;
  ::meshcore_lib::FloodScope scope_;
  CallbackManager<void(const ::meshcore_lib::ChannelMessage &)> callbacks_;
  ChannelState state_;
};

class ChannelMessageTrigger : public Trigger<::meshcore_lib::ChannelMessage> {
 public:
  /// Keep this trigger alive while its borrowed channel can dispatch; automation arguments own text across delays.
  explicit ChannelMessageTrigger(Channel *channel) {
    // Trigger arguments are owning values, including across ESPHome delays.
    channel->add_on_message_callback([this](const ::meshcore_lib::ChannelMessage &message) { this->trigger(message); });
  }
};

template<typename... Ts> class SendChannelMessageAction : public Action<Ts...> {
 public:
  /// Borrow the channel; the action supplies only an unverified sender label.
  explicit SendChannelMessageAction(Channel *channel) : channel_(channel) {}
  SendChannelMessageAction(MeshCoreComponent *parent, const std::vector<uint8_t> &key) : channel_(&this->send_only_) {
    configure_send_only_channel(parent, this->channel_, key);
  }
  /// Store the literal/templated sender label evaluated when play() runs; send-time limits still apply.
  template<typename V> void set_sender_name(V value) { this->sender_name_ = value; }
  /// Store the literal/templated message evaluated into owning bounded text when play() runs.
  template<typename V> void set_message(V value) { this->message_ = value; }
  /// Store the caller's literal/templated nonzero epoch seconds; the send operation does not choose a clock policy.
  template<typename V> void set_timestamp(V value) { this->timestamp_ = value; }
  /// Evaluate owned arguments and attempt one enqueue; rejected sends are logged by the parent, without automatic
  /// retry.
  void play(const Ts &...x) override {
    const auto message = this->message_.value(x...);
    const auto timestamp = this->timestamp_.value(x...);
    const auto sender_name = this->sender_name_.value(x...);
    this->channel_->send_channel_message(sender_name.view(), message.view(), timestamp);
  }

 private:
  // TEMPLATABLE_VALUE declares protected fields; keep the same storage and
  // code-generation setters while retaining ownership inside this class.
  TemplatableStorage<::meshcore_lib::ChannelText, Ts...> sender_name_{};
  TemplatableStorage<::meshcore_lib::ChannelText, Ts...> message_{};
  TemplatableStorage<uint32_t, Ts...> timestamp_{};
  Channel send_only_{};
  Channel *channel_;
};
}  // namespace esphome::meshcore
