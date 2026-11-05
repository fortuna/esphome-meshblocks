#include "client.h"
#include "meshcore_component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::meshcore {
static const char *const TAG = "meshcore.client";

Client::Client(MeshCoreComponent *parent) : parent_(parent), service_(this->protocol_, this->identities_, *parent) {
  this->service_.set_listener(this);
}
bool Client::configure(const std::string &id, const std::string &name, const std::vector<uint8_t> &key) {
  ::meshcore_lib::LocalIdentity identity;
  if (id.empty() || name.empty() || name.size() > 24 || key.size() != 64 ||
      !this->identities_.import_private_key(key.data(), identity) || !this->service_.configure(identity))
    return false;
  this->id_ = id;
  this->name_ = name;
  return true;
}
::meshcore_lib::ChannelText Client::public_key_hex() const {
  ::meshcore_lib::ClientMessage message;
  message.sender = this->public_key();
  return message.sender_public_key();
}
void Client::add_peer(const std::vector<uint8_t> &key) {
  ::meshcore_lib::PublicKey peer{};
  if (key.size() == peer.size())
    std::copy(key.begin(), key.end(), peer.begin());
  if (key.size() != peer.size() || !this->service_.add_peer(peer)) {
    ESP_LOGE(TAG, "Client %s receive peer configuration failed", this->id_.c_str());
    this->parent_->mark_failed();
  }
}
void Client::dump_config() const { ESP_LOGCONFIG(TAG, "Client %s (%s)", this->id_.c_str(), this->name_.c_str()); }
void Client::on_client_send_state(::meshcore_lib::ClientSendState state) {
  using State = ::meshcore_lib::ClientSendState;
  switch (state) {
    case State::IDLE:
      break;
    case State::QUEUED:
      ESP_LOGI(TAG, "Client %s: DM queued", this->id_.c_str());
      break;
    case State::SENT:
      ESP_LOGI(TAG, "Client %s: DM transmitted; awaiting receipt", this->id_.c_str());
      break;
    case State::ACKNOWLEDGED:
      ESP_LOGI(TAG, "Client %s: DM receipt received", this->id_.c_str());
      break;
    case State::DELIVERED_LOCALLY:
      ESP_LOGI(TAG, "Client %s: DM delivered locally", this->id_.c_str());
      break;
    case State::FAILED:
      ESP_LOGW(TAG, "Client %s: DM delivery failed", this->id_.c_str());
      break;
    case State::TIMED_OUT:
      ESP_LOGW(TAG, "Client %s: DM receipt timed out", this->id_.c_str());
      break;
  }
}
void Client::on_client_message(const ::meshcore_lib::ClientMessage &message) {
  // Keep untrusted text from introducing terminal control sequences or forged log lines.
  std::array<char, ::meshcore_lib::MAX_SENT_TEXT + 1> printable{};
  for (size_t i = 0; i < message.body().size(); ++i) {
    const auto ch = static_cast<unsigned char>(message.body()[i]);
    printable[i] = ch < 32 || ch == 127 ? '?' : ch;
  }
  ESP_LOGI(TAG, "Client %s: RX %s: %s", this->id_.c_str(), message.local ? "local" : "radio", printable.data());
  this->message_callbacks_.call(message);
}
bool Client::register_with(::meshcore_lib::NodeRuntime &node) { return node.add_client(this->service_); }
bool Client::send_client_message(std::string_view recipient, std::string_view text, uint32_t timestamp) {
  ::meshcore_lib::PublicKey peer;
  const bool accepted = ::meshcore_lib::parse_public_key(recipient, peer) &&
                        this->parent_->send_client_message(this->service_, peer, text, timestamp);
  if (!accepted)
    ESP_LOGW(TAG,
             "Client %s DM rejected: invalid input, self/room recipient, unavailable radio, pending send or queue/ACK "
             "capacity",
             this->id_.c_str());

  return accepted;
}
bool Client::advertise(uint32_t timestamp, bool flood, const ::meshcore_lib::FloodScope *scope) {
  ::meshcore_lib::Frame frame;
  uint32_t token;
  const bool accepted = this->parent_->is_radio_ready() &&
                        this->service_.advertisement(this->name_, timestamp, flood, frame, scope) &&
                        this->parent_->send(frame, millis_64(), token);
  if (!accepted)
    ESP_LOGW(TAG, "Client %s advertisement rejected", this->id_.c_str());
  else
    ESP_LOGI(TAG, "Client %s: advertisement queued", this->id_.c_str());
  return accepted;
}
}  // namespace esphome::meshcore
