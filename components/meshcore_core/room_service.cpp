#include "room_service.h"
#include <algorithm>
#include <cstring>

namespace meshcore_lib {
namespace {
size_t bounded_text(const uint8_t *data, size_t size) {
  size_t n = 0;
  while (n < size && data[n] != 0)
    ++n;
  return n;
}
}  // namespace
bool RoomService::configure(const LocalIdentity &identity, std::string_view name, std::string_view password,
                            RoomAccess access) {
  // Load old records up to 32 bytes for explicit USB repair. New provisioning and
  // policy updates cap passwords at the stock companion's 15-character limit.
  if (this->configured_ || name.empty() || name.size() > MAX_NAME_SIZE || password.size() < 12 ||
      password.size() > 32 || name.find('\0') != std::string_view::npos ||
      password.find('\0') != std::string_view::npos || (access != RoomAccess::PRIVATE && access != RoomAccess::PUBLIC))
    return false;
  this->identity_ = identity;
  this->access_ = access;
  std::copy(name.begin(), name.end(), this->name_.begin());
  std::copy(password.begin(), password.end(), this->password_.begin());
  RoomPost latest;
  if (this->posts_.count() && this->posts_.get(this->posts_.count() - 1, latest)) {
    this->last_text_ = latest.text;
    this->last_author_ = latest.author;
  }
  this->configured_ = true;
  return true;
}
bool RoomService::advert(uint32_t epoch, Frame &out, bool flood, const FloodScope *scope) {
  return this->configured_ && this->protocol_.advert(this->identity_, this->name_.data(), epoch, out, flood, scope);
}
bool RoomService::set_password(std::string_view password) {
  if (!this->configured_ || this->access_ != RoomAccess::PRIVATE || password.size() < 12 || password.size() > 15 ||
      password.find('\0') != std::string_view::npos)
    return false;
  if (password == std::string_view(this->password_.data()))
    return true;
  this->password_.fill(0);
  std::copy(password.begin(), password.end(), this->password_.begin());
  this->clients_ = {};  // Local policy change revokes all existing member sessions.
  return true;
}
size_t RoomService::clients() const {
  size_t n = 0;
  for (const auto &client : this->clients_)
    n += client.used;
  return n;
}
bool RoomService::post_(const PublicKey &author, std::string_view text, uint32_t epoch) {
  if (!this->configured_ || epoch == 0 || text.empty() || text.size() > ROOM_TEXT_CAPACITY ||
      text.find('\0') != std::string_view::npos || this->posts_.latest_timestamp() == UINT32_MAX)
    return false;
  RoomPost post;
  post.author = author;
  post.timestamp = std::max(epoch, this->posts_.latest_timestamp() + 1);
  std::copy(text.begin(), text.end(), post.text.begin());
  if (!this->posts_.append(post))
    return false;
  ++this->posts_since_boot_;
  this->last_text_ = post.text;
  this->last_author_ = post.author;
  return true;
}
bool RoomService::add_system_post(std::string_view text, uint32_t epoch) {
  return this->post_(this->identity_.public_key, text, epoch);
}
PacketPath RoomService::reply_path_(const Client &client, const PacketView &packet) const {
  if (client.path_known)
    return client.path;
  PacketPath result;
  result.descriptor = packet.path.descriptor & 0xc0;
  return result;
}
void RoomService::login_(const PacketView &packet, const Incoming &incoming, uint64_t now_ms, uint32_t epoch) {
  const auto &peer = incoming.peer;
  const auto &secret = incoming.secret;
  const auto &plain = incoming.plain;
  const size_t length = bounded_text(plain.bytes.data() + 8, plain.size - 8);
  // Open rooms use the stock blank-password login, retaining identity crypto,
  // replay checks, session limits and the normal read/write member response.
  const size_t expected = this->access_ == RoomAccess::PUBLIC ? 0 : std::strlen(this->password_.data());
  uint8_t difference = static_cast<uint8_t>(length != expected);
  for (size_t i = 0; i < expected; ++i)
    difference |= static_cast<uint8_t>(this->password_[i]) ^ (i < plain.size - 8 ? plain.bytes[8 + i] : 0);
  if (difference || peer == this->identity_.public_key) {
    ++this->login_diagnostics_.password_failures;
    ++this->rejected_;
    return;
  }
  Client *slot = nullptr;
  for (auto &client : this->clients_)
    if (client.used && client.key == peer) {
      slot = &client;
      break;
    }
  const bool returning = slot != nullptr;
  const uint32_t timestamp = read_u32(plain.bytes.data());
  if (timestamp == 0 || (slot && timestamp <= slot->last_timestamp)) {
    ++this->login_diagnostics_.replays;
    ++this->rejected_;
    return;
  }
  if (!slot)
    for (auto &client : this->clients_)
      if (!client.used) {
        slot = &client;
        break;
      }
  // Interim policy: only a valid new login at capacity replaces the least
  // recently active member. Equal times choose the first slot deterministically.
  // Do not mutate the victim until the login response has been admitted.
  if (!slot)
    slot = &*std::min_element(this->clients_.begin(), this->clients_.end(),
                              [](const Client &a, const Client &b) { return a.last_activity_ms < b.last_activity_ms; });
  const bool evicting = slot->used && !returning;
  Client candidate;
  candidate.used = true;
  candidate.key = peer;
  candidate.secret = secret;
  candidate.last_timestamp = timestamp;
  candidate.sync_since = read_u32(plain.bytes.data() + 4);
  candidate.last_activity_ms = now_ms;
  if (!packet.flood() && returning) {
    candidate.path_known = slot->path_known;
    candidate.path = slot->path;
  }
  std::array<uint8_t, 13> response{};
  write_u32(response.data(), epoch);
  response[7] = 2;  // Read/write member. No remote administrator role in this slice.
  response[12] = 1;
  if (!this->random_.fill(response.data() + 8, 4))
    return;
  Frame frame;
  bool encoded = packet.flood()
                     ? this->protocol_.path_response(this->identity_, peer, secret, packet.path, 1, response.data(),
                                                     response.size(), frame)
                     : this->protocol_.datagram(1, this->identity_, peer, secret, response.data(), response.size(),
                                                !candidate.path_known, this->reply_path_(candidate, packet), frame);
  uint32_t token;
  if (!encoded || !this->transport_.send(frame, now_ms + 300, token))
    return;
  candidate.login_reply_pending = true;
  candidate.login_reply_token = token;
  *slot = candidate;  // Commit session only after reply admission.
  if (evicting)
    ++this->login_diagnostics_.evictions;
  ++this->logins_;
  this->next_push_ = now_ms + 2000;
}
void RoomService::ack_(const uint8_t *data, size_t size, uint64_t now_ms) {
  if (size < 4)
    return;
  Client *match = nullptr;
  for (auto &client : this->clients_) {
    if (client.used && client.push_pending && client.push_on_air &&
        std::equal(client.pending_ack.begin(), client.pending_ack.end(), data)) {
      if (match)
        return;  // Ambiguous short tokens must not complete multiple jobs.
      match = &client;
    }
  }
  if (match) {
    match->sync_since = match->push_timestamp;
    match->push_pending = false;
    match->failures = 0;
    match->last_activity_ms = now_ms;
    ++this->delivered_posts_;
  }
}
uint8_t RoomService::unsynced_(const Client &client) const {
  uint8_t n = 0;
  for (size_t i = 0; i < this->posts_.count(); ++i) {
    RoomPost post;
    if (this->posts_.get(i, post) && post.timestamp > client.sync_since && post.author != client.key)
      ++n;
  }
  return n;
}
size_t RoomService::ack_matches_(const uint8_t *data, bool transmitted_only) const {
  size_t matches = 0;
  for (const auto &client : this->clients_)
    if (client.used && client.push_pending && (!transmitted_only || client.push_on_air) &&
        std::equal(client.pending_ack.begin(), client.pending_ack.end(), data))
      ++matches;
  return matches;
}
RoomService::Match RoomService::prepare_(const PacketView &packet, uint64_t now_ms, Incoming &out, bool rate_limit) {
  out = {};
  if (!this->configured_ || (!packet.flood() && packet.path.size() != 0) || packet.payload_size == 0 ||
      packet.payload[0] != this->identity_.public_key[0])
    return Match::NONE;
  if (packet.type == 7) {
    if (rate_limit && now_ms < this->next_login_)
      return Match::NONE;
    if (rate_limit)
      this->next_login_ = now_ms + 250;
    if (!this->protocol_.open_anonymous(packet, this->identity_, out.peer, out.secret, out.plain) ||
        out.plain.size < 16) {
      ++this->login_diagnostics_.crypto_failures;
      ++this->rejected_;
      return Match::NONE;
    }
    return Match::ONE;
  }
  if (!(packet.type == 0 || packet.type == 2 || packet.type == 8) || packet.payload_size < 20)
    return Match::NONE;
  bool found = false;
  Plaintext plain;
  for (size_t i = 0; i < this->clients_.size(); ++i) {
    const auto &client = this->clients_[i];
    if (client.used && client.key[0] == packet.payload[1] && this->protocol_.open_peer(packet, client.secret, plain)) {
      if (found) {
        ++this->rejected_;
        return Match::AMBIGUOUS;
      }
      found = true;
      out.client_index = i;
      out.plain = plain;
    }
  }
  return found ? Match::ONE : Match::NONE;
}
void RoomService::accept_(const PacketView &packet, const Incoming &incoming, uint64_t now_ms, uint32_t epoch) {
  if (packet.type == 7)
    this->login_(packet, incoming, now_ms, epoch);
  else
    this->peer_(packet, this->clients_[incoming.client_index], incoming.plain, now_ms, epoch);
}
void RoomService::receive(const PacketView &packet, uint64_t now_ms, uint32_t epoch) {
  if (!this->configured_ || epoch == 0 || (!packet.flood() && packet.path.size() != 0))
    return;
  if (packet.type == 3) {
    this->ack_(packet.payload, packet.payload_size, now_ms);
    return;
  }
  Incoming incoming;
  if (this->prepare_(packet, now_ms, incoming) == Match::ONE)
    this->accept_(packet, incoming, now_ms, epoch);
}
void RoomService::peer_(const PacketView &packet, Client &client, const Plaintext &plain, uint64_t now_ms,
                        uint32_t epoch) {
  if (packet.type == 8) {
    PacketPath path;
    path.descriptor = plain.bytes[0];
    if (!path.valid() || 2 + path.size() > plain.size) {
      ++this->rejected_;
      return;
    }
    std::copy_n(plain.bytes.begin() + 1, path.size(), path.bytes.begin());
    client.path = path;
    client.path_known = true;
    ++this->login_diagnostics_.paths_received;
    client.last_activity_ms = now_ms;
    if ((plain.bytes[1 + path.size()] & 15) == 3)
      this->ack_(plain.bytes.data() + 2 + path.size(), plain.size - 2 - path.size(), now_ms);
    return;
  }
  if (plain.size < 5)
    return;
  const uint32_t timestamp = read_u32(plain.bytes.data());
  if (timestamp == 0 || timestamp < client.last_timestamp) {
    ++this->rejected_;
    return;
  }
  Frame reply;
  uint32_t token;
  if (packet.type == 2) {
    if ((plain.bytes[4] >> 2) != static_cast<uint8_t>(TextType::PLAIN)) {
      ++this->rejected_;
      return;
    }  // CLI/admin commands unsupported.
    const size_t length = bounded_text(plain.bytes.data() + 5, plain.size - 5);
    if (length == 0 || length > ROOM_TEXT_CAPACITY) {
      ++this->rejected_;
      return;
    }
    const std::string_view text(reinterpret_cast<const char *>(plain.bytes.data() + 5), length);
    const bool retry = timestamp == client.last_post_timestamp;
    if (retry && text != std::string_view(client.last_post.data())) {
      ++this->rejected_;
      return;
    }
    if (!retry && timestamp <= client.last_timestamp) {
      ++this->rejected_;
      return;
    }
    std::array<uint8_t, 4> ack;
    if (!this->protocol_.ack_token(plain.bytes.data(), 5 + length, client.key, ack))
      return;
    if (!retry) {
      if (!this->post_(client.key, text, epoch)) {
        ++this->rejected_;
        return;
      }
      client.last_post.fill(0);
      std::copy(text.begin(), text.end(), client.last_post.begin());
      client.last_post_timestamp = timestamp;
      ++this->received_posts_;
    }
    client.last_timestamp = timestamp;
    client.last_activity_ms = now_ms;
    client.failures = 0;
    if (this->protocol_.ack(ack.data(), !client.path_known, this->reply_path_(client, packet), reply))
      this->transport_.send(reply, now_ms + 200, token);  // Retries can request the ACK if queue was full.
  } else if (packet.type == 0 && plain.bytes[4] == 1) {
    // Stock clients may request room status as part of completing their join.
    // This is a read-only member request, not a device administration command.
    ++this->login_diagnostics_.status_requests;
    client.last_timestamp = timestamp;
    client.last_activity_ms = now_ms;
    client.failures = 0;
    const auto data = this->protocol_.status_payload(timestamp, this->status_.snapshot(), this->posts_since_boot_,
                                                     this->pushes_since_boot_);
    const bool encoded = packet.flood() ? this->protocol_.path_response(this->identity_, client.key, client.secret,
                                                                        packet.path, 1, data.data(), data.size(), reply)
                                        : this->protocol_.datagram(1, this->identity_, client.key, client.secret,
                                                                   data.data(), data.size(), !client.path_known,
                                                                   this->reply_path_(client, packet), reply);
    if (encoded && this->transport_.send(reply, now_ms + 300, token))
      ++this->login_diagnostics_.status_replies;
  } else if (packet.type == 0 && plain.bytes[4] == 2 && !packet.flood() && plain.size >= 9) {
    ++this->login_diagnostics_.keepalives;
    if (!client.path_known)
      ++this->login_diagnostics_.keepalive_no_path;
    client.last_timestamp = timestamp;
    client.last_activity_ms = now_ms;
    client.failures = 0;
    const uint32_t since = read_u32(plain.bytes.data() + 5);
    if (since != 0)
      client.sync_since = since;
    client.push_pending = false;
    std::array<uint8_t, 4> ack;
    if (client.path_known && this->protocol_.ack_token(plain.bytes.data(), 9, client.key, ack) &&
        this->protocol_.ack(ack.data(), false, client.path, reply, this->unsynced_(client)))
      this->transport_.send(reply, now_ms + 300, token);
  }
}
void RoomService::transmitted(uint32_t token, bool success, uint64_t now_ms) {
  for (auto &client : this->clients_)
    if (client.used && client.login_reply_pending && client.login_reply_token == token) {
      client.login_reply_pending = false;
      if (success)
        ++this->login_diagnostics_.replies_sent;
      else
        ++this->login_diagnostics_.reply_failures;
    }
  for (auto &client : this->clients_)
    if (client.used && client.push_pending && client.push_token == token) {
      if (!success) {
        client.push_pending = false;
        ++client.failures;
      } else {
        client.push_on_air = true;
        client.ack_deadline = now_ms + (client.path_known ? 4000 + 2000 * ((client.path.descriptor & 63) + 1) : 12000);
      }
    }
}
uint64_t RoomService::next_deadline(uint64_t now_ms) const {
  if (!this->configured_ || this->clients() == 0)
    return UINT64_MAX;
  return std::max(now_ms, this->next_push_);
}
bool RoomService::tick(uint64_t now_ms, uint32_t epoch, bool allow_push) {
  if (!this->configured_ || epoch == 0 || now_ms < this->next_push_)
    return false;
  this->next_push_ = now_ms + 150;
  for (auto &client : this->clients_)
    if (client.push_pending && client.push_on_air && now_ms >= client.ack_deadline) {
      client.push_pending = false;
      ++client.failures;
    }
  if (!allow_push)
    return false;
  auto &client = this->clients_[this->next_client_];
  this->next_client_ = (this->next_client_ + 1) % this->clients_.size();
  if (!client.used || client.push_pending || client.failures >= 3)
    return false;
  for (size_t i = 0; i < this->posts_.count(); ++i) {
    RoomPost post;
    if (!this->posts_.get(i, post) || post.author == client.key || post.timestamp <= client.sync_since ||
        uint64_t(epoch) < uint64_t(post.timestamp) + 6)
      continue;
    std::array<uint8_t, MAX_CIPHERTEXT_SIZE> plain{};
    write_u32(plain.data(), post.timestamp);
    uint8_t attempt;
    if (!this->random_.fill(&attempt, 1))
      return false;
    plain[4] = (static_cast<uint8_t>(TextType::SIGNED_PLAIN) << 2) | (attempt & 3);
    std::copy_n(post.author.begin(), 4, plain.begin() + 5);
    const size_t length = std::strlen(post.text.data());
    std::copy_n(post.text.begin(), length, plain.begin() + 9);
    std::array<uint8_t, 4> ack;
    if (!this->protocol_.ack_token(plain.data(), 9 + length, client.key, ack))
      return false;
    for (const auto &other : this->clients_)
      if (other.push_pending && other.pending_ack == ack)
        return false;  // Retry with a fresh attempt later.
    if (!this->transport_.ack_available(ack))
      return false;
    Frame frame;
    uint32_t token;
    PacketPath path = client.path_known ? client.path : PacketPath{};
    if (!this->protocol_.datagram(2, this->identity_, client.key, client.secret, plain.data(), 9 + length,
                                  !client.path_known, path, frame) ||
        !this->transport_.send(frame, now_ms, token))
      return false;
    client.push_pending = true;
    client.push_on_air = false;
    client.push_token = token;
    client.push_timestamp = post.timestamp;
    client.pending_ack = ack;
    ++this->pushes_since_boot_;
    this->next_push_ = now_ms + 1200;
    return true;
  }
  return false;
}
}  // namespace meshcore_lib
