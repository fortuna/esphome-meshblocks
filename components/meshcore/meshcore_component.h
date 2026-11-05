#pragma once

#include <array>
#include <string>
#include <vector>
#include <memory>
#include "esphome/components/meshcore_core/channel_codec.h"
#include "esphome/components/meshcore_core/epoch_clock.h"
#include "esphome/components/meshcore_core/channel_timestamp.h"
#include "esphome/components/time/real_time_clock.h"
#include "mbedtls_crypto.h"
#include "sx126x_radio.h"
#include "room_runtime.h"
#include "room.h"
#include "channel.h"
#include "client.h"
#include "transmit_state.h"
#include "idle_power.h"
#include "esphome/components/sx126x/sx126x.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"

namespace esphome::meshcore {

class MeshCoreComponent final : public Component,
                                public sx126x::SX126xListener,
                                public ::meshcore_lib::RoomTransport,
                                public ::meshcore_lib::NodeStatusSource {
 public:
  static constexpr size_t ROOM_CAPACITY = MESHCORE_ROOM_COUNT;
  static constexpr size_t CLIENT_CAPACITY = MESHCORE_CLIENT_COUNT;
  static constexpr size_t CHANNEL_CAPACITY = MESHCORE_CHANNEL_COUNT;
  static constexpr size_t CLIENT_STATE_BYTES =
      CLIENT_CAPACITY * (sizeof(Client) + sizeof(Client *) + sizeof(::meshcore_lib::ClientService *) +
                         sizeof(::meshcore_lib::LocalClientDelivery));
  static_assert(CLIENT_STATE_BYTES <= MESHCORE_CLIENT_STATE_BUDGET,
                "Configured clients exceed client_state_budget; review the device RAM budget");
  static constexpr size_t TX_QUEUE_SIZE = MESHCORE_TX_QUEUE_SIZE;
  // Fixed room objects and registry; excludes allocator, string and NVS
  // SDK overhead. A build-time resource guard, not a total/free-heap guarantee.
  static constexpr size_t ROOM_STATE_BYTES =
      ROOM_CAPACITY * (sizeof(Room) + sizeof(Room *) + sizeof(RoomRuntime) + sizeof(std::unique_ptr<RoomRuntime>) +
                       sizeof(::meshcore_lib::RoomService *));
  static_assert(ROOM_STATE_BYTES <= MESHCORE_ROOM_STATE_BUDGET,
                "Configured rooms exceed room_state_budget; reduce rooms or review the device RAM budget");
  /// Bind the borrowed SX126x driver before setup; this component supplies its sole application TX scheduler.
  void set_radio(sx126x::SX126x *radio) { this->radio_ = radio; }
  /// Bind a borrowed read-only ESPHome epoch source before setup; restored room floors do not set this source.
  void set_time_source(time::RealTimeClock *clock) { this->time_source_ = clock; }
  /// Bind an optional borrowed owner-context power policy; nullptr leaves mesh operation independent of sleep control.
  void set_idle_power(IdlePower *power) { this->idle_power_ = power; }
  /// Notify the optional power policy of management activity without directly sleeping or changing radio state.
  void management_activity() {
    if (this->idle_power_)
      this->idle_power_->management_activity();
  }
  /// Set the validated outbound hash width before use (1-3 bytes; current room configurations require two).
  void set_path_hash_size(uint8_t width) { this->path_hash_size_ = width; }
  /// Read the configured outbound path hash width for client messages.
  uint8_t path_hash_size() const { return this->path_hash_size_; }
  /// Configure a borrowed channel handle with copied key/scope bytes before setup; an empty scope is unscoped.
  /// The handle must outlive this component. Invalid inputs or exhausted slots cause setup to fail.
  void configure_send_only_channel(Channel *channel, const std::vector<uint8_t> &key);
  void add_channel(Channel *channel, const std::string &id, const std::vector<uint8_t> &key,
                   const std::vector<uint8_t> &scope_key);
  /// Configure/register a borrowed static client; duplicate or invalid identities fail node setup.
  void add_client(Client *client, const std::string &id, const std::string &name, const std::vector<uint8_t> &key);
  /// Admit a DM through the local client queue or shared radio transport; true is not delivery.
  bool send_client_message(::meshcore_lib::ClientService &sender, const ::meshcore_lib::PublicKey &recipient,
                           std::string_view text, uint32_t timestamp);
  /// Run setup immediately after the radio's processor-priority initialization.
  float get_setup_priority() const override { return setup_priority::PROCESSOR - 1.0f; }
  /// Validate composition, attach the radio, load/apply room credentials and register unique room identities.
  /// Storage/key/configuration failures mark the component failed; no radio traffic is served after failed setup.
  void setup() override;
  /// Advance rooms and the single TX scheduler in the ESPHome owning context; defer idle work by
  /// deadlines.
  void loop() override;
  /// Log non-secret resource counts and configured channel/clock bindings.
  void dump_config() override;
  /// Synchronously process borrowed RX bytes with RSSI in dBm and SNR in dB; retained text/frames are copied.
  /// Routes room traffic through NodeRuntime and decoded, deduplicated text through channel callbacks; never forwards
  /// transit.
  void on_packet(const std::vector<uint8_t> &packet, float rssi, float snr) override;
  /// Persist the effective ordering floor in every ready room; false reports unavailable time/state or a failed write.
  /// Does not set the UTC source and is not atomic across rooms; earlier checkpoints may already be durable on failure.
  bool checkpoint_time();
  /// Encode/copy a message for a channel owned by this node, using timestamp exactly; true means queue admission only.
  /// Reject invalid text/time, unavailable radio or full TX/preview storage; no automatic retry or peer ACK is
  /// provided.
  bool send_channel_message(Channel &channel, std::string_view name, std::string_view message, uint32_t timestamp);
  /// Select a caller-policy timestamp from source UTC or estimates with within-boot uniqueness; zero means exhaustion.
  /// Logs degraded/adjusted selections without setting UTC or writing flash; send_channel_message() can bypass this
  /// helper.
  uint32_t next_channel_timestamp();
  /// Log counters, readiness, time quality and room/channel diagnostics without exporting credentials.
  /// This may include bounded platform power diagnostics and is intended for explicit management requests.
  void log_status();
  /// Allocate/configure one owned room during composition; invalid inputs or capacity/allocation failure reject setup.
  /// Empty seed retains legacy USB provisioning; nonempty input accepts a 32-byte legacy seed or 64-byte native key.
  void add_room(const std::string &id, const std::string &name, ::meshcore_lib::RoomAccess access,
                const std::vector<uint8_t> &seed = {}, const std::string &password = "");
  /// Enqueue one signed room advertisement by stable room ID; timing belongs to the caller's ESPHome automations.
  /// Direct uses an empty path; flood optionally accepts a borrowed regional scope and never enables repeating.
  /// False is logged for invalid/direct scope, unknown/not-ready rooms, unavailable time/storage/radio or queue
  /// rejection. True confirms admission only, with no automatic retry, TX-completion guarantee or peer delivery
  /// receipt.
  bool advertise_room(const std::string &id, bool flood = false, const ::meshcore_lib::FloodScope *scope = nullptr);
  /// Copy a room/protocol frame into the shared queue no earlier than not_before_ms; return a device-wide TX token.
  /// True admits a frame but does not confirm transmission or delivery.
  bool send(const ::meshcore_lib::Frame &frame, uint64_t not_before_ms, uint32_t &token) override;
  /// Query device-wide pending/recent ACK reservations; false also represents bounded reservation backpressure.
  bool ack_available(const std::array<uint8_t, 4> &ack) const override { return this->node_.ack_available(ack); }
  /// Copy shared radio/uptime/queue observations without polling hardware; unavailable optional measurements remain
  /// unset.
  ::meshcore_lib::NodeStatus snapshot() const override;
  /// Borrow a configured resource for read-only observation; nullptr for an out-of-range index.
  const RoomRuntime *room_at(size_t index) const {
    return index < this->room_count_ ? this->rooms_[index].get() : nullptr;
  }
  Channel *channel_at(size_t index) const { return index < this->channel_count_ ? this->channels_[index] : nullptr; }
  size_t room_count() const { return this->room_count_; }
  size_t channel_count() const { return this->channel_count_; }
  /// Return selected-source epoch seconds, not a restored protocol ordering floor.
  uint32_t source_timestamp() const { return this->source_epoch_(); }
  /// Legacy identity import for an explicit storage target; rejects static rooms and identity conflicts.
  /// On success registers the activated room and advances the protocol floor. No input is logged.
  bool provision_room(size_t index, const char *seed_hex, const char *password, uint32_t epoch);
  /// Change a legacy room password only while TX is drained; static/public rooms reject changes.
  bool change_room_password(size_t index, const char *password);
  /// Add a local post through the existing room service using protocol time.
  bool add_room_post(size_t index, const char *text);
  /// Export a signed room advertisement without transmitting; consumes a durable advert timestamp.
  bool export_room_advertisement(size_t index, ::meshcore_lib::Frame &out);
  /// Require successful component setup and a present, nonfailed radio; does not prove RF reception by a peer.
  bool is_radio_ready() const {
    return this->ready_ && !this->is_failed() && this->radio_ != nullptr && !this->radio_->is_failed();
  }
  /// Report whether the selected source supplies an in-range epoch; source freshness is the time provider's
  /// responsibility.
  bool clock_ready() const;
  /// Count physical RX callbacks processed since boot, including traffic not decoded as channel text.
  uint32_t received_packets() const { return this->received_packets_; }
  /// Count newly decoded channel messages after duplicate filtering, across all channels; excludes room posts.
  uint32_t received_messages() const { return this->received_messages_; }
  /// Count successful physical TX frames across all services, including room adverts/ACKs as well as channel messages.
  uint32_t sent_messages() const { return this->sent_messages_; }
  /// Return the last accepted channel message's RSSI in dBm, initialized to zero before any such message.
  float last_rssi() const { return this->last_rssi_; }
  /// Return the last accepted channel message's SNR in dB, initialized to zero before any such message.
  float last_snr() const { return this->last_snr_; }
  /// Borrow the aggregate latest received channel text in raw UTF-8; successful local sends do not replace this value.
  const char *last_text() const { return this->last_text_.data(); }
  /// Borrow the local ID of the latest accepted RX channel, empty before the first decoded channel message.
  const char *last_channel() const { return this->last_channel_.c_str(); }
  /// Borrow the most recent aggregate send/readiness status label; it is not a per-peer delivery result.
  const char *send_status() const { return this->send_status_; }

 private:
  void wake_mesh_();
  void schedule_next_tick_();
  IdlePower *idle_power_{};
  MbedTlsCrypto crypto_;
  std::array<::meshcore_lib::RoomService *, ROOM_CAPACITY> room_registry_{};
  std::array<::meshcore_lib::ClientService *, CLIENT_CAPACITY> client_registry_{};
  std::array<::meshcore_lib::LocalClientDelivery, CLIENT_CAPACITY> local_deliveries_{};
  ::meshcore_lib::NodeRuntime node_{this->room_registry_, this->client_registry_, this->local_deliveries_};
  std::array<Client *, CLIENT_CAPACITY> clients_{};
  size_t client_count_{};
  std::array<std::unique_ptr<RoomRuntime>, ROOM_CAPACITY> rooms_{};
  size_t room_count_{};
  bool has_send_only_channel_{false};
  ChannelTxState<TX_QUEUE_SIZE> channel_tx_state_;
  ::meshcore_lib::ChannelTextCodec codec_{this->crypto_};
  ::meshcore_lib::RecentMessages recent_;
  Sx126xRadio packet_radio_;
  std::array<::meshcore_lib::TransmitJob, TX_QUEUE_SIZE> tx_queue_{};
  ::meshcore_lib::RadioScheduler scheduler_{this->packet_radio_, this->tx_queue_};
  std::array<Channel *, CHANNEL_CAPACITY> channels_{};
  size_t channel_count_{};
  sx126x::SX126x *radio_{};
  uint8_t path_hash_size_{2};
  bool ready_{};
  bool configuration_failed_{};
  uint32_t received_packets_{};
  uint32_t received_messages_{};
  uint32_t duplicates_{};
  uint32_t sent_messages_{};
  uint32_t sent_flood_{}, sent_direct_{}, received_flood_{}, received_direct_{};
  uint64_t tx_airtime_ms_{};
  float radio_last_rssi_{}, radio_last_snr_{};
  float last_rssi_{};
  float last_snr_{};
  std::array<char, ::meshcore_lib::MAX_RECEIVED_TEXT + 1> last_text_{};
  std::array<char, ::meshcore_lib::MAX_RECEIVED_TEXT + 1> last_log_text_{};
  uint32_t last_message_timestamp_{};
  uint8_t last_hops_{};
  uint8_t last_hash_size_{};
  bool last_scoped_{};
  std::array<uint16_t, 2> last_transport_codes_{};
  std::string last_channel_;
  time::RealTimeClock *time_source_{};
  // Protocol floor/holdover is distinct from the selected UTC source. A source
  // reset or backward correction must not rewind room history or adverts.
  ::meshcore_lib::EpochClock protocol_clock_;
  ::meshcore_lib::ChannelTimestamp channel_timestamp_;
  uint32_t estimated_channel_timestamps_{}, adjusted_channel_timestamps_{};
  uint64_t last_loop_at_{};
  uint32_t max_loop_gap_ms_{};
  uint32_t next_tx_token_{};
  TransmitState<TX_QUEUE_SIZE> tx_state_;
  uint32_t source_epoch_() const;
  uint32_t current_epoch_();
  bool advertise_room_(size_t index, bool flood, const ::meshcore_lib::FloodScope *scope);
  uint32_t failed_transmissions_{};
  uint32_t max_radio_service_us_{};
  uint32_t tx_loop_iterations_{};
  uint32_t max_tx_loop_gap_ms_{};
  const char *send_status_{"Idle"};
};

}  // namespace esphome::meshcore
