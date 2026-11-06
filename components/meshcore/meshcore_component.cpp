#include "meshcore_component.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <new>
#include "esphome/core/hal.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome::meshcore {
static const char *const TAG = "meshcore";

void configure_send_only_channel(MeshCoreComponent *parent, Channel *channel, const std::vector<uint8_t> &key) {
  parent->configure_send_only_channel(channel, key);
}

void MeshCoreComponent::configure_send_only_channel(Channel *channel, const std::vector<uint8_t> &key) {
  ::meshcore_lib::ChannelKey decoded;
  if (channel == nullptr || !this->codec_.make_key(key.data(), key.size(), decoded)) {
    this->configuration_failed_ = true;
    return;
  }
  channel->configure(this, "send-only", decoded, {});
  this->has_send_only_channel_ = true;
}

void MeshCoreComponent::add_channel(Channel *channel, const std::string &id, const std::vector<uint8_t> &key,
                                    const std::vector<uint8_t> &scope_key) {
  ::meshcore_lib::ChannelKey decoded_key;
  ::meshcore_lib::FloodScope scope;
  if (channel == nullptr || this->channel_count_ >= this->channels_.size() ||
      !this->codec_.make_key(key.data(), key.size(), decoded_key) ||
      (!scope_key.empty() && !this->codec_.make_scope(scope_key.data(), scope_key.size(), scope))) {
    this->configuration_failed_ = true;
    return;
  }
  channel->configure(this, id, decoded_key, scope);
  this->channels_[this->channel_count_++] = channel;
}

bool MeshCoreComponent::send_client_message(::meshcore_lib::ClientService &sender,
                                            const ::meshcore_lib::PublicKey &recipient, std::string_view text,
                                            uint32_t timestamp) {
  if (!this->is_radio_ready())
    return false;
  if (this->node_.identity_available(recipient))
    return sender.send_client_message(recipient, text, timestamp, this->path_hash_size(), millis_64());
  const bool accepted =
      this->node_.send_local_client_message(sender, recipient, text, timestamp, this->path_hash_size());
  if (accepted)
    this->wake_mesh_();
  return accepted;
}
void MeshCoreComponent::add_client(Client *client, const std::string &id, const std::string &name,
                                   const std::vector<uint8_t> &key) {
  if (client == nullptr || this->client_count_ == CLIENT_CAPACITY || !client->configure(id, name, key) ||
      !client->register_with(this->node_)) {
    this->configuration_failed_ = true;
    return;
  }
  this->clients_[this->client_count_++] = client;
}

void MeshCoreComponent::add_room(const std::string &id, const std::string &name, ::meshcore_lib::RoomAccess access,
                                 const std::vector<uint8_t> &seed, const std::string &password) {
  if (this->room_count_ == this->rooms_.size()) {
    this->configuration_failed_ = true;
    return;
  }
  for (size_t i = 0; i < this->room_count_; ++i)
    if (this->rooms_[i]->id() == id) {
      this->configuration_failed_ = true;
      return;
    }
  // Composition-time allocation only. Allocation failure must not look like a
  // successfully configured room or dereference a null adapter.
  std::unique_ptr<RoomRuntime> room(new (std::nothrow) RoomRuntime(this->crypto_, *this, *this));
  if (!room) {
    this->configuration_failed_ = true;
    return;
  }
  room->set_config(id, name, access);
  if (!seed.empty() && !room->set_static_credentials(seed, password)) {
    this->configuration_failed_ = true;
    return;
  }
  this->rooms_[this->room_count_++] = std::move(room);
}
void MeshCoreComponent::setup() {
  const auto build_epoch = App.get_build_time();
  this->channel_timestamp_.seed(::meshcore_lib::EpochClock::valid(build_epoch) ? static_cast<uint32_t>(build_epoch) : 1,
                                0);
  if (this->configuration_failed_ || this->room_count_ != ROOM_CAPACITY || this->channel_count_ != CHANNEL_CAPACITY ||
      this->client_count_ != CLIENT_CAPACITY ||
      (this->channel_count_ == 0 && this->room_count_ == 0 && this->client_count_ == 0 &&
       !this->has_send_only_channel_) ||
      this->radio_ == nullptr || this->radio_->is_failed() || this->time_source_ == nullptr) {
    ESP_LOGE(TAG, "Room, client, channel or radio configuration/allocation failed");
    this->mark_failed();
    return;
  }
  if (!this->packet_radio_.attach(this->radio_)) {
    ESP_LOGE(TAG, "Could not claim exclusive radio transmitter");
    this->mark_failed();
    return;
  }
  this->last_channel_.reserve(24);
  for (size_t i = 0; i < this->room_count_; ++i) {
    auto &room = *this->rooms_[i];
    const auto initial_epoch = this->source_epoch_() ? this->source_epoch_()
                                                     : (::meshcore_lib::EpochClock::valid(build_epoch)
                                                            ? static_cast<uint32_t>(build_epoch)
                                                            : ::meshcore_lib::EpochClock::MIN_EPOCH);
    if (!room.setup(initial_epoch, this->node_) || (room.ready() && !this->node_.add_room(room.service()))) {
      ESP_LOGE(TAG, "Room %s storage/identity failed or duplicates another local identity", room.id().c_str());
      this->mark_failed();
      return;
    }
    if (room.ready())
      this->protocol_clock_.advance(room.restored_epoch(), millis_64());
  }
  if (this->room_count_ || this->client_count_)
    this->scheduler_.set_send_gap(100);
  this->ready_ = true;
  ESP_LOGI(TAG, "MeshCore ready; rooms=%u; no repeating", static_cast<unsigned>(this->room_count_));
}

void MeshCoreComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "  Time: read-only ESPHome time_id; protocol floor retained");
  ESP_LOGCONFIG(TAG, "  Rooms: %u; fixed room state: %u / %u bytes", static_cast<unsigned>(ROOM_CAPACITY),
                static_cast<unsigned>(ROOM_STATE_BYTES), static_cast<unsigned>(MESHCORE_ROOM_STATE_BUDGET));
  ESP_LOGCONFIG(TAG, "  Clients: %u; fixed clients/peers/local queue: %u bytes", static_cast<unsigned>(CLIENT_CAPACITY),
                static_cast<unsigned>(CLIENT_STATE_BYTES));
  for (size_t i = 0; i < this->client_count_; ++i)
    this->clients_[i]->dump_config();
  ESP_LOGCONFIG(TAG, "  TX queue: %u slots including active TX", static_cast<unsigned>(this->scheduler_.capacity()));
  ESP_LOGCONFIG(
      TAG, "  Fixed TX buffers: %u bytes (frames, previews and metadata)",
      static_cast<unsigned>(sizeof(this->tx_queue_) + sizeof(this->channel_tx_state_) + sizeof(this->tx_state_)));
  ESP_LOGCONFIG(TAG, "MeshCore channel experiment: %u channels; %u-byte outbound paths",
                static_cast<unsigned>(this->channel_count_), this->path_hash_size_);
  for (size_t i = 0; i < this->channel_count_; ++i)
    ESP_LOGCONFIG(TAG, "  Channel: %s; outbound scoped=%s", this->channels_[i]->id().c_str(),
                  this->channels_[i]->scope().valid ? "yes" : "no");
}

void MeshCoreComponent::on_packet(const std::vector<uint8_t> &packet, float rssi, float snr) {
  if (!this->is_radio_ready())
    return;
  this->wake_mesh_();
  ++this->received_packets_;
  this->radio_last_rssi_ = rssi;
  this->radio_last_snr_ = snr;
  ESP_LOGD(TAG, "RX packet #%lu: %u bytes RSSI %.1f SNR %.1f", static_cast<unsigned long>(this->received_packets_),
           static_cast<unsigned>(packet.size()), rssi, snr);
  ::meshcore_lib::PacketView view;
  if (::meshcore_lib::parse_packet(packet.data(), packet.size(), view)) {
    if (view.flood())
      ++this->received_flood_;
    else
      ++this->received_direct_;
    if ((this->node_.rooms() || this->node_.clients()) && view.type != 5) {
      ESP_LOGD(TAG, "MESH RX type=%u route=%s hops=%u", view.type, view.flood() ? "flood" : "direct",
               view.path.descriptor & 63);
      this->node_.receive(view, millis_64(), this->current_epoch_());
      // Post automations run after the room has finished with the packet, so they may post or send.
      for (size_t i = 0; i < this->room_count_; ++i)
        this->rooms_[i]->dispatch_posts(rssi, snr);
      return;
    }
  }
  for (size_t i = 0; i < this->channel_count_; ++i) {
    ::meshcore_lib::ChannelMessage message;
    auto result = this->codec_.decode(packet.data(), packet.size(), this->channels_[i]->key(), message);
    if (result == ::meshcore_lib::DecodeResult::WRONG_CHANNEL || result == ::meshcore_lib::DecodeResult::BAD_MAC)
      continue;
    if (result != ::meshcore_lib::DecodeResult::OK)
      return;
    if (this->recent_.contains(message.fingerprint)) {
      ++this->duplicates_;
      return;
    }
    this->recent_.remember(message.fingerprint);
    ++this->received_messages_;
    this->last_rssi_ = rssi;
    this->last_snr_ = snr;
    message.rssi = rssi;
    message.snr = snr;
    this->last_channel_ = this->channels_[i]->id();
    this->last_message_timestamp_ = message.timestamp;
    this->last_hops_ = message.hops;
    this->last_hash_size_ = message.path_hash_size;
    this->last_scoped_ = message.scoped;
    this->last_transport_codes_ = message.transport_codes;
    // Preserve UTF-8 in serial logs while neutralizing terminal control bytes.
    auto log_text = message.text;
    for (size_t j = 0; j < message.text_size; ++j) {
      const auto ch = static_cast<uint8_t>(message.text[j]);
      if (ch < 32 || ch == 127)
        log_text[j] = ' ';
    }
    this->last_text_ = message.text;
    this->last_text_[message.text_size] = 0;
    this->last_log_text_ = log_text;
    ESP_LOGI(TAG, "MESSAGE [%s] ts=%lu hops=%u hash_bytes=%u RSSI=%.1f SNR=%.1f text=%s",
             this->channels_[i]->id().c_str(), static_cast<unsigned long>(message.timestamp), message.hops,
             message.path_hash_size, rssi, snr, log_text.data());
    ESP_LOGI(TAG, "MESSAGE route scoped=%s transport=%04x/%04x", message.scoped ? "yes" : "no",
             message.transport_codes[0], message.transport_codes[1]);
    this->channels_[i]->on_message(message);
    return;
  }
}

bool MeshCoreComponent::checkpoint_time() {
  // Persist protocol ordering state; never set or restore the input time source.
  if (!this->ready_ || this->is_failed())
    return false;
  this->wake_mesh_();
  const auto epoch = this->current_epoch_();
  if (!epoch)
    return false;
  for (size_t i = 0; i < this->room_count_; ++i)
    if (this->rooms_[i]->ready() && !this->rooms_[i]->checkpoint(epoch))
      return false;
  return true;
}

bool MeshCoreComponent::clock_ready() const { return this->source_epoch_() != 0; }

bool Channel::send_channel_message(std::string_view name, std::string_view message, uint32_t timestamp) {
  return this->parent_ != nullptr && this->parent_->send_channel_message(*this, name, message, timestamp);
}

uint32_t MeshCoreComponent::next_channel_timestamp() {
  const auto source = this->source_epoch_();
  const auto value = this->channel_timestamp_.next(source, this->current_epoch_(), millis_64());
  using Quality = ::meshcore_lib::ChannelTimestamp::Quality;
  if (this->channel_timestamp_.quality() == Quality::EXHAUSTED) {
    ESP_LOGE(TAG, "Channel timestamp helper exhausted uint32 range");
  } else if (this->channel_timestamp_.quality() == Quality::ESTIMATED) {
    ++this->estimated_channel_timestamps_;
    ESP_LOGW(TAG, "Channel timestamp estimated: %lu; UTC source unavailable", static_cast<unsigned long>(value));
  } else if (this->channel_timestamp_.quality() == Quality::ADJUSTED) {
    ++this->adjusted_channel_timestamps_;
    ESP_LOGW(TAG, "Channel timestamp advanced for uniqueness: %lu; source=%lu", static_cast<unsigned long>(value),
             static_cast<unsigned long>(source));
  }
  return value;
}

bool MeshCoreComponent::send_channel_message(Channel &channel, std::string_view name, std::string_view message,
                                             uint32_t timestamp) {
  if (!this->is_radio_ready()) {
    this->send_status_ = "Radio unavailable";
    ESP_LOGW(TAG, "SEND rejected: radio unavailable");
    return false;
  }
  if (timestamp == 0) {
    this->send_status_ = "Invalid timestamp";
    ESP_LOGW(TAG, "SEND rejected: caller must supply a nonzero timestamp");
    return false;
  }
  if (!channel.belongs_to(this)) {
    ESP_LOGW(TAG, "SEND rejected: channel does not belong to this node");
    return false;
  }
  if (this->scheduler_.outstanding() == this->scheduler_.capacity()) {
    ESP_LOGW(TAG, "SEND rejected: TX queue full");
    return false;
  }
  ::meshcore_lib::Frame frame;
  const auto *scope = channel.scope().valid ? &channel.scope() : nullptr;
  if (!this->codec_.encode(channel.key(), this->path_hash_size_, timestamp, name, message, frame, scope)) {
    ESP_LOGW(TAG, "SEND rejected: invalid name/message or more than 160 UTF-8 bytes including sender prefix");
    return false;
  }
  ::meshcore_lib::ChannelMessage check;
  if (this->codec_.decode(frame.bytes.data(), frame.size, channel.key(), check) != ::meshcore_lib::DecodeResult::OK) {
    ESP_LOGE(TAG, "SEND rejected: local encode verification failed");
    return false;
  }
  if (!channel.track_send(this->channel_tx_state_, this->next_tx_token_, check)) {
    ESP_LOGW(TAG, "SEND rejected: channel TX metadata full");
    return false;
  }
  TransmitMetadata metadata{true, true, check.fingerprint};
  if (!this->tx_state_.remember(this->next_tx_token_, metadata)) {
    this->channel_tx_state_.complete(this->next_tx_token_, false);
    return false;
  }
  if (!this->scheduler_.enqueue(frame, this->next_tx_token_)) {
    this->channel_tx_state_.complete(this->next_tx_token_, false);
    this->tx_state_.take(this->next_tx_token_, metadata);
    return false;
  }
  ++this->next_tx_token_;
  this->send_status_ = "TX queued";
  this->wake_mesh_();
  ESP_LOGI(TAG, "SEND queued id=%lu [%s] ts=%lu bytes=%u scoped=%s",
           static_cast<unsigned long>(this->next_tx_token_ - 1), channel.id().c_str(),
           static_cast<unsigned long>(timestamp), static_cast<unsigned>(frame.size), scope == nullptr ? "no" : "yes");
  return true;
}

void MeshCoreComponent::loop() {
  const uint64_t now = millis_64();
  const uint32_t gap = this->last_loop_at_ == 0 ? 0 : static_cast<uint32_t>(now - this->last_loop_at_);
  this->max_loop_gap_ms_ = std::max(this->max_loop_gap_ms_, gap);
  this->last_loop_at_ = now;
  if (!this->is_radio_ready()) {
    if (this->idle_power_)
      this->idle_power_->set_mesh_busy(true);
    return;
  }
  if (this->idle_power_)
    this->idle_power_->set_mesh_busy(true);
  this->node_.tick(now, this->current_epoch_());
  const bool was_active = this->scheduler_.active();
  if (was_active) {
    ++this->tx_loop_iterations_;
    this->max_tx_loop_gap_ms_ = std::max(this->max_tx_loop_gap_ms_, gap);
  }
  ::meshcore_lib::TransmitEvent event;
  const uint32_t start = micros();
  const bool completed = this->scheduler_.tick(now, event);
  this->max_radio_service_us_ = std::max(this->max_radio_service_us_, micros() - start);
  if (!was_active && this->scheduler_.active()) {
    this->tx_loop_iterations_ = 0;
    this->send_status_ = "Transmitting...";
  }
  if (!completed) {
    this->schedule_next_tick_();
    return;
  }
  this->channel_tx_state_.complete(event.token, event.result == ::meshcore_lib::RadioResult::SENT);
  this->node_.transmitted(event.token, event.result == ::meshcore_lib::RadioResult::SENT, now);
  TransmitMetadata metadata;
  this->tx_state_.take(event.token, metadata);
  if (event.result == ::meshcore_lib::RadioResult::SENT) {
    ++this->sent_messages_;
    if (metadata.flood)
      ++this->sent_flood_;
    else
      ++this->sent_direct_;
    this->tx_airtime_ms_ += event.elapsed_ms;
    if (metadata.channel)
      this->recent_.remember(metadata.fingerprint);
    this->send_status_ = "TX done (unconfirmed)";
    ESP_LOGI(TAG,
             "TX complete: id=%lu bytes=%u elapsed_ms=%lu service_max_us=%lu loop_ticks=%lu; peer delivery unconfirmed",
             static_cast<unsigned long>(event.token), static_cast<unsigned>(event.bytes),
             static_cast<unsigned long>(event.elapsed_ms), static_cast<unsigned long>(this->max_radio_service_us_),
             static_cast<unsigned long>(this->tx_loop_iterations_));
  } else {
    ++this->failed_transmissions_;
    this->send_status_ = "TX failed";
    ESP_LOGW(TAG, "TX failed: id=%lu code=%u elapsed_ms=%lu", static_cast<unsigned long>(event.token),
             static_cast<unsigned>(event.result), static_cast<unsigned long>(event.elapsed_ms));
  }
  this->schedule_next_tick_();
}

void MeshCoreComponent::wake_mesh_() {
  this->cancel_timeout("mesh_tick");
  this->enable_loop();
}

void MeshCoreComponent::schedule_next_tick_() {
  // Preserve TX polling/restoration; idle work uses the existing ESPHome
  // scheduler in the same execution context, with one outstanding timer.
  if (this->idle_power_)
    this->idle_power_->set_mesh_busy(this->scheduler_.active());
  if (this->scheduler_.active()) {
    this->enable_loop();
    return;
  }
  const auto now = millis_64();
  uint64_t next = std::min(this->node_.next_deadline(now, this->current_epoch_()), this->scheduler_.next_deadline(now));
  // A one-second health/time-source check also covers externally updated UTC.
  const uint32_t wait = next <= now ? 1 : static_cast<uint32_t>(std::min<uint64_t>(next - now, 1000));
  this->disable_loop();
  this->set_timeout("mesh_tick", wait, [this]() { this->loop(); });
}

void MeshCoreComponent::log_status() {
  if (this->idle_power_)
    this->idle_power_->log_status();
  ESP_LOGI(TAG, "CHANNEL_TIME helper_last=%lu estimated=%lu adjusted=%lu",
           static_cast<unsigned long>(this->channel_timestamp_.last()),
           static_cast<unsigned long>(this->estimated_channel_timestamps_),
           static_cast<unsigned long>(this->adjusted_channel_timestamps_));
  ESP_LOGI(TAG, "CLOCK input=%lu effective=%lu", static_cast<unsigned long>(this->source_epoch_()),
           static_cast<unsigned long>(this->current_epoch_()));
  ESP_LOGI(TAG, "STATUS ready=%s clock=%s packets=%lu messages=%lu duplicates=%lu tx=%lu max_loop_gap_ms=%lu",
           this->is_radio_ready() ? "yes" : "no", this->clock_ready() ? "yes" : "no",
           static_cast<unsigned long>(this->received_packets_), static_cast<unsigned long>(this->received_messages_),
           static_cast<unsigned long>(this->duplicates_), static_cast<unsigned long>(this->sent_messages_),
           static_cast<unsigned long>(this->max_loop_gap_ms_));
  ESP_LOGI(TAG, "RADIO active=%s queued=%u failures=%lu service_max_us=%lu tx_loop_gap_max_ms=%lu",
           this->scheduler_.active() ? "yes" : "no", static_cast<unsigned>(this->scheduler_.outstanding()),
           static_cast<unsigned long>(this->failed_transmissions_),
           static_cast<unsigned long>(this->max_radio_service_us_),
           static_cast<unsigned long>(this->max_tx_loop_gap_ms_));
  if (this->received_messages_ != 0) {
    ESP_LOGI(TAG, "LAST MESSAGE [%s] ts=%lu hops=%u hash_bytes=%u RSSI=%.1f SNR=%.1f text=%s",
             this->last_channel_.c_str(), static_cast<unsigned long>(this->last_message_timestamp_), this->last_hops_,
             this->last_hash_size_, this->last_rssi_, this->last_snr_, this->last_log_text_.data());
    ESP_LOGI(TAG, "LAST MESSAGE route scoped=%s transport=%04x/%04x", this->last_scoped_ ? "yes" : "no",
             this->last_transport_codes_[0], this->last_transport_codes_[1]);
  }
  for (size_t i = 0; i < this->channel_count_; ++i) {
    const auto &display = this->channels_[i]->state();
    ESP_LOGI(TAG, "CHANNEL id=%s rx=%lu tx=%lu last=%s", this->channels_[i]->id().c_str(),
             static_cast<unsigned long>(display.received_messages()),
             static_cast<unsigned long>(display.sent_messages()), display.last_direction());
  }
  for (size_t i = 0; i < this->room_count_; ++i) {
    const auto &room = *this->rooms_[i];
    const auto &service = room.service();
    ESP_LOGI(TAG,
             "ROOM status id=%s ready=%s clients=%u posts=%u logins=%lu received_posts=%lu delivered=%lu rejected=%lu "
             "evictions=%lu",
             room.id().c_str(), room.ready() ? "yes" : "no", static_cast<unsigned>(service.clients()),
             static_cast<unsigned>(service.posts()), static_cast<unsigned long>(service.logins()),
             static_cast<unsigned long>(service.received_posts()),
             static_cast<unsigned long>(service.delivered_posts()), static_cast<unsigned long>(service.rejected()),
             static_cast<unsigned long>(service.login_diagnostics().evictions));
    const auto &diagnostics = service.login_diagnostics();
    ESP_LOGI(TAG, "ROOM login_failures id=%s crypto=%lu password=%lu replay=%lu capacity=%lu", room.id().c_str(),
             static_cast<unsigned long>(diagnostics.crypto_failures),
             static_cast<unsigned long>(diagnostics.password_failures), static_cast<unsigned long>(diagnostics.replays),
             static_cast<unsigned long>(diagnostics.capacity_failures));
    ESP_LOGI(TAG, "ROOM join id=%s replies_sent=%lu reply_failures=%lu paths=%lu keepalives=%lu keepalive_no_path=%lu",
             room.id().c_str(), static_cast<unsigned long>(diagnostics.replies_sent),
             static_cast<unsigned long>(diagnostics.reply_failures),
             static_cast<unsigned long>(diagnostics.paths_received), static_cast<unsigned long>(diagnostics.keepalives),
             static_cast<unsigned long>(diagnostics.keepalive_no_path));
    ESP_LOGI(TAG, "ROOM status_requests id=%s received=%lu replies_queued=%lu", room.id().c_str(),
             static_cast<unsigned long>(diagnostics.status_requests),
             static_cast<unsigned long>(diagnostics.status_replies));
  }
  if (this->room_count_) {
    ESP_LOGI(TAG, "NODE rooms=%u ambiguous_packets=%lu ambiguous_acks=%lu", static_cast<unsigned>(this->node_.rooms()),
             static_cast<unsigned long>(this->node_.ambiguous_packets()),
             static_cast<unsigned long>(this->node_.ambiguous_acks()));
    ESP_LOGI(TAG, "ROOM resources heap_min=%u stack_watermark=%u",
             static_cast<unsigned>(esp_get_minimum_free_heap_size()),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  }
  ESP_LOGI(TAG, "STATUS end");
}

uint32_t MeshCoreComponent::source_epoch_() const {
  if (!this->time_source_)
    return 0;
  const auto epoch = this->time_source_->timestamp_now();
  return epoch >= 0 && ::meshcore_lib::EpochClock::valid(static_cast<uint64_t>(epoch)) ? static_cast<uint32_t>(epoch)
                                                                                       : 0;
}
uint32_t MeshCoreComponent::current_epoch_() {
  const auto now = millis_64();
  this->protocol_clock_.advance(this->source_epoch_(), now);
  return this->protocol_clock_.now(now);
}
::meshcore_lib::NodeStatus MeshCoreComponent::snapshot() const {
  ::meshcore_lib::NodeStatus status;
  status.tx_queue_length = this->scheduler_.outstanding();
  status.last_rssi = static_cast<int16_t>(this->radio_last_rssi_);
  status.last_snr_quarters = static_cast<int16_t>(this->radio_last_snr_ * 4);
  status.packets_received = this->received_packets_;
  status.packets_sent = this->sent_messages_;
  status.tx_airtime_seconds = static_cast<uint32_t>(this->tx_airtime_ms_ / 1000);
  status.uptime_seconds = static_cast<uint32_t>(millis_64() / 1000);
  status.sent_flood = this->sent_flood_;
  status.sent_direct = this->sent_direct_;
  status.received_flood = this->received_flood_;
  status.received_direct = this->received_direct_;
  // Battery, noise-floor, upstream error flags and physical duplicate counters
  // are unavailable; keep their optional measurements empty, never fabricate them.
  return status;
}
bool MeshCoreComponent::send(const ::meshcore_lib::Frame &frame, uint64_t not_before_ms, uint32_t &token) {
  if (!this->is_radio_ready())
    return false;
  const auto route = frame.bytes[0] & 3;
  TransmitMetadata metadata{false, route == 0 || route == 1, {}};
  if (!this->tx_state_.remember(this->next_tx_token_, metadata))
    return false;
  if (!this->scheduler_.enqueue(frame, this->next_tx_token_, not_before_ms)) {
    this->tx_state_.take(this->next_tx_token_, metadata);
    return false;
  }
  this->wake_mesh_();
  token = this->next_tx_token_++;
  ESP_LOGD(TAG, "MESH TX queued id=%lu type=%u route=%u bytes=%u", static_cast<unsigned long>(token),
           (frame.bytes[0] >> 2) & 15, frame.bytes[0] & 3, static_cast<unsigned>(frame.size));
  return true;
}
bool Room::advertise(bool flood, const ::meshcore_lib::FloodScope *scope) {
  return this->parent_->advertise_room(this->storage_id_, flood, scope);
}
void Room::add_on_post_callback(std::function<void(const RoomMessage &)> callback) {
  this->parent_->add_room_post_callback(this->storage_id_, std::move(callback));
}
bool Room::post(std::string_view text) {
  for (size_t index = 0; index < this->parent_->room_count(); ++index)
    if (this->parent_->room_at(index)->id() == this->storage_id_ && this->parent_->add_room_post(index, text))
      return true;
  ESP_LOGW(TAG, "ROOM post rejected room=%s", this->storage_id_.c_str());
  return false;
}
void MeshCoreComponent::add_room_post_callback(const std::string &id,
                                               std::function<void(const RoomMessage &)> callback) {
  for (size_t index = 0; index < this->room_count_; ++index)
    if (this->rooms_[index]->id() == id) {
      this->rooms_[index]->add_on_post_callback(std::move(callback));
      return;
    }
  this->configuration_failed_ = true;
}
bool MeshCoreComponent::advertise_room(const std::string &id, bool flood, const ::meshcore_lib::FloodScope *scope) {
  if (scope == nullptr || (flood && scope->valid))
    for (size_t index = 0; index < this->room_count_; ++index)
      if (this->rooms_[index]->id() == id && this->advertise_room_(index, flood, scope))
        return true;
  ESP_LOGW(TAG, "ROOM advert rejected room=%s mode=%s scoped=%s", id.c_str(), flood ? "flood" : "direct",
           scope != nullptr ? "yes" : "no");
  return false;
}
bool MeshCoreComponent::advertise_room_(size_t index, bool flood, const ::meshcore_lib::FloodScope *scope) {
  auto &room = *this->rooms_[index];
  const uint32_t epoch = this->current_epoch_();
  ::meshcore_lib::Frame frame;
  uint32_t token;
  if (!this->is_radio_ready() || !room.ready() || epoch == 0 ||
      this->scheduler_.outstanding() == this->scheduler_.capacity() ||
      !room.create_advertisement(epoch, flood, frame, scope) || !this->send(frame, millis_64(), token))
    return false;
  ESP_LOGI(TAG, "ROOM advert queued id=%lu room=%s mode=%s scoped=%s", static_cast<unsigned long>(token),
           room.id().c_str(), flood ? "flood" : "direct", scope != nullptr ? "yes" : "no");
  return true;
}
bool MeshCoreComponent::provision_room(size_t index, const char *seed_hex, const char *password, uint32_t epoch) {
  if (index >= this->room_count_)
    return false;
  auto &room = *this->rooms_[index];
  if (!room.provision(room.id().c_str(), seed_hex, password, epoch, this->node_) ||
      !this->node_.add_room(room.service()))
    return false;
  this->protocol_clock_.advance(room.restored_epoch(), millis_64());
  this->wake_mesh_();
  return true;
}
bool MeshCoreComponent::change_room_password(size_t index, const char *password) {
  return index < this->room_count_ && this->scheduler_.outstanding() == 0 &&
         this->rooms_[index]->change_password(password);
}
bool MeshCoreComponent::add_room_post(size_t index, std::string_view text) {
  if (index >= this->room_count_ || !this->rooms_[index]->ready())
    return false;
  const bool accepted = this->rooms_[index]->service().add_system_post(text, this->current_epoch_());
  if (accepted)
    this->wake_mesh_();
  return accepted;
}
bool MeshCoreComponent::export_room_advertisement(size_t index, ::meshcore_lib::Frame &out) {
  return index < this->room_count_ && this->rooms_[index]->ready() &&
         this->rooms_[index]->create_advertisement(this->current_epoch_(), false, out);
}
}  // namespace esphome::meshcore
