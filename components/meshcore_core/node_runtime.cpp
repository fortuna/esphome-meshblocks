#include "node_runtime.h"
#include <algorithm>

namespace meshcore_lib {
bool NodeRuntime::identity_available(const PublicKey &identity, const RoomService *except) const {
  for (size_t i = 0; i < this->count_; ++i)
    if (this->rooms_[i] != except && this->rooms_[i]->public_key() == identity)
      return false;
  for (size_t i = 0; i < this->client_count_; ++i)
    if (this->clients_[i]->public_key() == identity)
      return false;
  return true;
}
bool NodeRuntime::add_client(ClientService &client) {
  for (size_t i = 0; i < this->client_count_; ++i)
    if (this->clients_[i] == &client)
      return true;
  if (!client.configured() || this->client_count_ == this->client_capacity_ ||
      !this->identity_available(client.public_key()))
    return false;
  this->clients_[this->client_count_++] = &client;
  return true;
}
bool NodeRuntime::send_local_client_message(ClientService &sender, const PublicKey &recipient, std::string_view text,
                                            uint32_t timestamp, uint8_t width) {
  ClientService *target = nullptr;
  bool registered = false;
  for (size_t i = 0; i < this->client_count_; ++i) {
    registered |= this->clients_[i] == &sender;
    if (this->clients_[i]->public_key() == recipient)
      target = this->clients_[i];
  }
  if (!registered || !target || target == &sender || this->local_count_ == this->local_capacity_)
    return false;
  auto &job = this->local_queue_[(this->local_head_ + this->local_count_) % this->local_capacity_];
  // Reserve before notifying the sender's listener; failed encoding releases the slot.
  ++this->local_count_;
  job.sender = &sender;
  job.recipient = target;
  if (!sender.queue_local_message(recipient, text, timestamp, width, job.frame)) {
    --this->local_count_;
    return false;
  }
  return true;
}
void NodeRuntime::deliver_local_() {
  if (!this->local_count_)
    return;
  const auto job = this->local_queue_[this->local_head_];
  this->local_head_ = (this->local_head_ + 1) % this->local_capacity_;
  --this->local_count_;
  ClientMessage message;
  const bool valid = job.recipient->decode_local_message(job.frame, message);
  job.sender->complete_local(valid);
  if (valid)
    job.recipient->deliver_message(message);
}
bool NodeRuntime::add_room(RoomService &room) {
  for (size_t i = 0; i < this->count_; ++i)
    if (this->rooms_[i] == &room)
      return true;
  if (!room.configured_ || this->count_ == this->capacity_ || !this->identity_available(room.public_key()))
    return false;
  this->rooms_[this->count_++] = &room;
  return true;
}
bool NodeRuntime::ack_available(const std::array<uint8_t, 4> &ack) const {
  for (size_t i = 0; i < this->count_; ++i)
    if (this->rooms_[i]->ack_matches_(ack.data(), false))
      return false;
  for (size_t i = 0; i < this->client_count_; ++i)
    if (this->clients_[i]->ack_matches(ack.data(), false))
      return false;
  size_t space = 0;
  for (const auto &recent : this->recent_acks_) {
    if (recent.until <= this->now_)
      ++space;
    else if (recent.token == ack)
      return false;
  }
  for (size_t i = 0; i < this->client_count_; ++i) {
    const auto &client = *this->clients_[i];
    if (!client.pending_radio())
      continue;
    bool recorded = false;
    for (const auto &recent : this->recent_acks_)
      recorded |= recent.until > this->now_ && recent.token == client.pending_ack();
    if (!recorded) {
      if (space == 0)
        return false;
      --space;
    }
  }
  return space != 0;  // Backpressure instead of evicting a live reservation.
}
void NodeRuntime::remember_ack_(const std::array<uint8_t, 4> &ack, uint64_t now_ms, bool renew) {
  RecentAck *slot = nullptr;
  for (auto &recent : this->recent_acks_) {
    if (recent.until > now_ms && recent.token == ack) {
      if (renew)
        recent.until = now_ms + ACK_REUSE_GUARD_MS;
      return;
    }
    if (!slot && recent.until <= now_ms)
      slot = &recent;
  }
  if (slot)
    *slot = {ack, now_ms + ACK_REUSE_GUARD_MS};
}
void NodeRuntime::remember_pending_acks_(uint64_t now_ms) {
  for (size_t i = 0; i < this->count_; ++i)
    for (const auto &client : this->rooms_[i]->clients_)
      if (client.push_pending)
        this->remember_ack_(client.pending_ack, now_ms);
  for (size_t i = 0; i < this->client_count_; ++i)
    if (this->clients_[i]->pending_radio())
      this->remember_ack_(this->clients_[i]->pending_ack(), now_ms, true);
}
void NodeRuntime::receive(const PacketView &packet, uint64_t now_ms, uint32_t epoch) {
  this->now_ = now_ms;
  this->remember_pending_acks_(now_ms);
  for (size_t i = 0; i < this->client_count_; ++i)
    this->clients_[i]->tick(now_ms);
  if ((!packet.flood() && packet.path.size() != 0))
    return;
  RoomService *owner = nullptr;
  ClientService *client_owner = nullptr;
  if (packet.type == 3) {
    if (packet.payload_size < 4)
      return;
    size_t matches = 0;
    for (size_t i = 0; i < this->count_; ++i) {
      const size_t n = this->rooms_[i]->ack_matches_(packet.payload, true);
      if (n)
        owner = this->rooms_[i];
      matches += n;
    }
    for (size_t i = 0; i < this->client_count_; ++i) {
      if (this->clients_[i]->ack_matches(packet.payload, true)) {
        client_owner = this->clients_[i];
        ++matches;
      }
    }
    if (matches > 1) {
      ++this->ambiguous_acks_;
      return;
    }
    if (client_owner)
      client_owner->acknowledge();
    if (owner)
      owner->ack_(packet.payload, packet.payload_size, now_ms);
    return;
  }
  RoomService::Incoming selected, candidate;
  ClientMessage client_message, client_candidate;
  if (packet.type == 7) {
    bool local_prefix = false;
    for (size_t i = 0; i < this->count_; ++i)
      local_prefix |= packet.payload_size != 0 && packet.payload[0] == this->rooms_[i]->public_key()[0];
    if (!local_prefix || now_ms < this->next_login_)
      return;
    this->next_login_ = now_ms + 250;
  }
  for (size_t i = 0; i < this->client_count_; ++i) {
    const auto match = this->clients_[i]->prepare_message(packet, client_candidate);
    const bool receipt = this->clients_[i]->matches_path_ack(packet);
    if (match == ClientService::Match::AMBIGUOUS || ((receipt || match == ClientService::Match::ONE) && client_owner)) {
      ++this->ambiguous_packets_;
      return;
    }
    if (receipt || match == ClientService::Match::ONE) {
      client_owner = this->clients_[i];
      if (match == ClientService::Match::ONE)
        client_message = client_candidate;
    }
  }
  // Authenticate every prefix candidate before changing any room's membership,
  // paths, history or sync state. The inherited short MAC can be ambiguous.
  for (size_t i = 0; i < this->count_; ++i) {
    const auto match = this->rooms_[i]->prepare_(packet, now_ms, candidate, false);
    if (match == RoomService::Match::AMBIGUOUS || (match == RoomService::Match::ONE && (owner || client_owner))) {
      ++this->ambiguous_packets_;
      return;
    }
    if (match == RoomService::Match::ONE) {
      owner = this->rooms_[i];
      selected = candidate;
    }
  }
  if (client_owner) {
    if (packet.type == 2)
      client_owner->accept_message(packet, client_message, now_ms);
    else
      client_owner->acknowledge();
  }
  if (owner && epoch != 0)
    owner->accept_(packet, selected, now_ms, epoch);
}
uint64_t NodeRuntime::next_deadline(uint64_t now_ms, uint32_t epoch) const {
  uint64_t next = this->local_count_ ? now_ms : UINT64_MAX;
  if (epoch != 0)
    for (size_t i = 0; i < this->count_; ++i)
      next = std::min(next, this->rooms_[i]->next_deadline(now_ms));
  for (size_t i = 0; i < this->client_count_; ++i)
    next = std::min(next, this->clients_[i]->next_deadline());
  return next;
}
void NodeRuntime::tick(uint64_t now_ms, uint32_t epoch) {
  this->now_ = now_ms;
  this->remember_pending_acks_(now_ms);
  for (size_t i = 0; i < this->client_count_; ++i)
    this->clients_[i]->tick(now_ms);
  this->deliver_local_();
  if (!this->count_)
    return;
  const size_t start = this->next_room_;
  bool allowed = now_ms >= this->next_push_;
  for (size_t n = 0; n < this->count_; ++n) {
    const size_t i = (start + n) % this->count_;
    if (this->rooms_[i]->tick(now_ms, epoch, allowed)) {
      this->remember_pending_acks_(now_ms);
      allowed = false;
      this->next_push_ = now_ms + PUSH_INTERVAL_MS;
      this->next_room_ = (i + 1) % this->count_;
    }
  }
}
void NodeRuntime::transmitted(uint32_t token, bool success, uint64_t now_ms) {
  this->now_ = now_ms;
  this->remember_pending_acks_(now_ms);
  for (size_t i = 0; i < this->client_count_; ++i)
    this->clients_[i]->transmitted(token, success, now_ms);
  // Device transport tokens are globally unique across all outstanding frames.
  for (size_t i = 0; i < this->count_; ++i)
    this->rooms_[i]->transmitted(token, success, now_ms);
}
}  // namespace meshcore_lib
