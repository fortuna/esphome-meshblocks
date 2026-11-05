#include "client_service.h"

namespace meshcore_lib {
bool parse_public_key(std::string_view text, PublicKey &out) {
  out = {};
  if (text.size() != 64)
    return false;
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  PublicKey key;
  for (size_t i = 0; i < key.size(); ++i) {
    const int high = hex(text[2 * i]), low = hex(text[2 * i + 1]);
    if (high < 0 || low < 0)
      return false;
    key[i] = (high << 4) | low;
  }
  if (key[0] == 0 || key[0] == 255)
    return false;
  out = key;
  return true;
}

bool ClientService::configure(const LocalIdentity &identity) {
  if (this->configured_ || identity.public_key[0] == 0 || identity.public_key[0] == 255)
    return false;
  this->identity_ = identity;
  this->configured_ = true;
  return true;
}
bool ClientService::advertisement(std::string_view name, uint32_t timestamp, bool flood, Frame &out,
                                  const FloodScope *scope) {
  return this->configured_ &&
         this->protocol_.advert(this->identity_, name, timestamp, out, flood, scope, AdvertRole::CHAT);
}
ChannelText ClientMessage::sender_public_key() const {
  static const char HEX[] = "0123456789abcdef";
  char text[65]{};
  for (size_t i = 0; i < this->sender.size(); ++i) {
    text[2 * i] = HEX[this->sender[i] >> 4];
    text[2 * i + 1] = HEX[this->sender[i] & 15];
  }
  return ChannelText(text);
}
void ClientService::set_state_(ClientSendState state) {
  this->state_ = state;
  if (this->listener_)
    this->listener_->on_client_send_state(state);
}
bool ClientService::add_peer(const PublicKey &key) {
  if (key[0] == 0 || key[0] == 255 || key == this->identity_.public_key)
    return false;
  for (size_t i = 0; i < this->peer_count_; ++i)
    if (this->peers_[i].key == key)
      return true;
  if (this->peer_count_ == this->peers_.size())
    return false;
  this->peers_[this->peer_count_++].key = key;
  return true;
}
bool ClientService::encode_message_(const PublicKey &peer, std::string_view text, uint32_t timestamp, uint8_t width,
                                    Frame &out, SharedSecret &secret, std::array<uint8_t, 4> &ack) {
  if (!this->configured_ || this->pending() || peer == this->identity_.public_key || peer[0] == 0 || peer[0] == 255 ||
      timestamp == 0 || text.empty() || text.size() > MAX_SENT_TEXT || text.find('\0') != text.npos || width < 1 ||
      width > 3)
    return false;
  if (!this->identities_.shared_secret(this->identity_, peer, secret))
    return false;
  std::array<uint8_t, 5 + MAX_SENT_TEXT> plain{};
  write_u32(plain.data(), timestamp);
  std::copy(text.begin(), text.end(), plain.begin() + 5);
  PacketPath path;
  path.descriptor = (width - 1) << 6;
  return this->protocol_.ack_token(plain.data(), 5 + text.size(), this->identity_.public_key, ack) &&
         this->protocol_.datagram(2, this->identity_, peer, secret, plain.data(), 5 + text.size(), true, path, out);
}
bool ClientService::send_client_message(const PublicKey &peer, std::string_view text, uint32_t timestamp, uint8_t width,
                                        uint64_t now_ms) {
  SharedSecret secret;
  std::array<uint8_t, 4> ack;
  Frame frame;
  uint32_t token;
  if (!this->encode_message_(peer, text, timestamp, width, frame, secret, ack) ||
      !this->transport_.ack_available(ack) || !this->transport_.send(frame, now_ms, token))
    return false;
  this->peer_ = peer;
  this->secret_ = secret;
  this->ack_ = ack;
  this->token_ = token;
  this->local_pending_ = false;
  this->set_state_(ClientSendState::QUEUED);
  return true;
}
bool ClientService::queue_local_message(const PublicKey &peer, std::string_view text, uint32_t timestamp, uint8_t width,
                                        Frame &out) {
  SharedSecret secret;
  std::array<uint8_t, 4> ack;
  if (!this->encode_message_(peer, text, timestamp, width, out, secret, ack))
    return false;
  this->local_pending_ = true;
  this->set_state_(ClientSendState::QUEUED);
  return true;
}
void ClientService::complete_local(bool success) {
  if (!this->local_pending_ || this->state_ != ClientSendState::QUEUED)
    return;
  this->local_pending_ = false;
  this->set_state_(success ? ClientSendState::DELIVERED_LOCALLY : ClientSendState::FAILED);
}
bool ClientService::decode_message_(const PacketView &packet, const PublicKey &sender, ClientMessage &out) {
  if (!this->configured_ || packet.type != 2 || packet.payload_size < 20 ||
      packet.payload[0] != this->public_key()[0] || packet.payload[1] != sender[0] ||
      (!packet.flood() && packet.path.size() != 0))
    return false;
  SharedSecret secret;
  Plaintext plain;
  if (!this->identities_.shared_secret(this->identity_, sender, secret) ||
      !this->protocol_.open_peer(packet, secret, plain) || plain.size < 6 || read_u32(plain.bytes.data()) == 0 ||
      (plain.bytes[4] >> 2) != 0)
    return false;
  size_t length = 0;
  while (5 + length < plain.size && plain.bytes[5 + length] != 0)
    ++length;
  if (length == 0 || length > MAX_SENT_TEXT)
    return false;
  // One optional extended retry byte follows the NUL terminator in stock messages.
  for (size_t i = 5 + length + 2; i < plain.size; ++i)
    if (plain.bytes[i] != 0)
      return false;
  out = {};
  out.sender = sender;
  out.timestamp = read_u32(plain.bytes.data());
  out.text = ChannelText(std::string_view(reinterpret_cast<const char *>(plain.bytes.data() + 5), length));
  return true;
}
bool ClientService::decode_local_message(const Frame &frame, ClientMessage &out) {
  PacketView packet;
  if (!parse_packet(frame.bytes.data(), frame.size, packet) || this->prepare_message(packet, out) != Match::ONE)
    return false;
  out.local = true;
  return true;
}
ClientService::Match ClientService::prepare_message(const PacketView &packet, ClientMessage &out) {
  bool found = false;
  ClientMessage candidate;
  for (size_t i = 0; i < this->peer_count_; ++i) {
    if (!this->decode_message_(packet, this->peers_[i].key, candidate))
      continue;
    if (found)
      return Match::AMBIGUOUS;
    found = true;
    out = candidate;
  }
  return found ? Match::ONE : Match::NONE;
}
bool ClientService::accept_message(const PacketView &packet, const ClientMessage &message, uint64_t now_ms) {
  Peer *peer = nullptr;
  for (size_t i = 0; i < this->peer_count_; ++i)
    if (this->peers_[i].key == message.sender)
      peer = &this->peers_[i];
  if (!peer || message.timestamp < peer->last_timestamp ||
      (message.timestamp == peer->last_timestamp && message.body() != peer->last_text.view()))
    return false;
  SharedSecret secret;
  Plaintext plain;
  std::array<uint8_t, 4> ack;
  Frame reply;
  uint32_t token;
  // Preserve the received attempt bits in the receipt hash, as stock clients do.
  if (!this->identities_.shared_secret(this->identity_, message.sender, secret) ||
      !this->protocol_.open_peer(packet, secret, plain) ||
      !this->protocol_.ack_token(plain.bytes.data(), 5 + message.body().size(), message.sender, ack))
    return false;
  const bool encoded = packet.flood() ? this->protocol_.path_response(this->identity_, message.sender, secret,
                                                                      packet.path, 3, ack.data(), ack.size(), reply)
                                      : this->protocol_.ack(ack.data(), true, PacketPath{}, reply);
  if (!encoded || !this->transport_.send(reply, now_ms, token))
    return false;
  if (message.timestamp == peer->last_timestamp)
    return true;  // Retry gets a receipt, not a second callback/reply.
  peer->last_timestamp = message.timestamp;
  peer->last_text = message.text;
  this->deliver_message(message);
  return true;
}
void ClientService::deliver_message(const ClientMessage &message) {
  if (this->listener_)
    this->listener_->on_client_message(message);
}
void ClientService::transmitted(uint32_t token, bool success, uint64_t now_ms) {
  if (this->local_pending_ || this->state_ != ClientSendState::QUEUED || token != this->token_)
    return;
  this->deadline_ = now_ms + ACK_TIMEOUT_MS;
  this->set_state_(success ? ClientSendState::SENT : ClientSendState::FAILED);
}
void ClientService::tick(uint64_t now_ms) {
  if (this->state_ == ClientSendState::SENT && now_ms >= this->deadline_)
    this->set_state_(ClientSendState::TIMED_OUT);
}
uint64_t ClientService::next_deadline() const {
  return this->state_ == ClientSendState::SENT ? this->deadline_ : UINT64_MAX;
}
bool ClientService::ack_matches(const uint8_t *ack, bool transmitted_only) const {
  return this->pending_radio() && (!transmitted_only || this->state_ == ClientSendState::SENT) &&
         std::equal(this->ack_.begin(), this->ack_.end(), ack);
}
bool ClientService::matches_path_ack(const PacketView &packet) {
  if (this->state_ != ClientSendState::SENT || packet.type != 8 || packet.payload_size < 20 ||
      packet.payload[0] != this->identity_.public_key[0] || packet.payload[1] != this->peer_[0])
    return false;
  Plaintext plain;
  if (!this->protocol_.open_peer(packet, this->secret_, plain))
    return false;
  PacketPath path;
  path.descriptor = plain.bytes[0];
  const size_t extra = 1 + path.size();
  return path.valid() && extra + 5 <= plain.size && plain.bytes[extra] == 3 &&
         this->ack_matches(plain.bytes.data() + extra + 1, true);
}
void ClientService::acknowledge() {
  if (this->state_ == ClientSendState::SENT)
    this->set_state_(ClientSendState::ACKNOWLEDGED);
}
}  // namespace meshcore_lib
