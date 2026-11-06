#include "room_runtime.h"
#include "esp_random.h"
#include "../meshcore_core/epoch_clock.h"
#include <algorithm>
#include <cstring>
namespace esphome::meshcore {
bool EspRandom::fill(uint8_t *data, size_t size) {
  esp_fill_random(data, size);
  return true;
}
bool RoomRuntime::derive_identity_(const RoomCredentials &credentials, ::meshcore_lib::LocalIdentity &identity) {
  return credentials.has_private_key ? this->identities_.import_private_key(credentials.private_key.data(), identity)
                                     : this->identities_.derive(credentials.seed.data(), identity);
}
bool RoomRuntime::activate_(const RoomCredentials &credentials) {
  ::meshcore_lib::LocalIdentity identity;
  if (!this->derive_identity_(credentials, identity))
    return false;
  this->ready_ = this->service_.configure(identity, this->name_, credentials.password.data(), this->access_);
  this->restored_epoch_ = std::max(credentials.epoch, this->store_.clock_floor());
  if (this->restored_epoch_ < UINT32_MAX)
    ++this->restored_epoch_;
  return this->ready_;
}
bool RoomRuntime::set_static_credentials(const std::vector<uint8_t> &seed, const std::string &password) {
  if ((seed.size() != this->initial_credentials_.seed.size() &&
       seed.size() != this->initial_credentials_.private_key.size()) ||
      std::all_of(seed.begin(), seed.end(), [](uint8_t byte) { return byte == 0; }))
    return false;
  const std::string effective_password =
      this->access_ == ::meshcore_lib::RoomAccess::PUBLIC ? "unused-public" : password;
  if (effective_password.size() < 12 || effective_password.size() > 15 ||
      std::any_of(effective_password.begin(), effective_password.end(), [](char c) { return c < 33 || c > 126; }))
    return false;
  this->initial_credentials_.has_private_key = seed.size() == this->initial_credentials_.private_key.size();
  if (this->initial_credentials_.has_private_key)
    std::copy(seed.begin(), seed.end(), this->initial_credentials_.private_key.begin());
  else
    std::copy(seed.begin(), seed.end(), this->initial_credentials_.seed.begin());
  this->initial_credentials_.password.fill(0);
  std::copy(effective_password.begin(), effective_password.end(), this->initial_credentials_.password.begin());
  this->static_credentials_ = true;
  return true;
}
bool RoomRuntime::apply_static_credentials_(RoomCredentials &credentials, StoreResult state, uint32_t initial_epoch,
                                            const ::meshcore_lib::NodeRuntime &node) {
  ::meshcore_lib::LocalIdentity identity;
  if (!::meshcore_lib::EpochClock::valid(initial_epoch) ||
      !this->derive_identity_(this->initial_credentials_, identity) ||
      !node.identity_available(identity.public_key, &this->service_))
    return false;
  if (state == StoreResult::MISSING) {
    // Never bind a fresh identity to orphaned retained history or clock data.
    if (this->store_.count() || this->store_.clock_floor())
      return false;
    credentials = this->initial_credentials_;
    credentials.epoch = initial_epoch;
    return this->store_.provision(credentials);
  }
  // Different compiled key material must not silently replace a stored identity.
  ::meshcore_lib::LocalIdentity stored_identity;
  if (!this->derive_identity_(credentials, stored_identity) || stored_identity.private_key != identity.private_key)
    return false;
  if (this->access_ == ::meshcore_lib::RoomAccess::PRIVATE &&
      credentials.password != this->initial_credentials_.password) {
    if (!this->store_.change_password(this->initial_credentials_.password.data()))
      return false;
    credentials.password = this->initial_credentials_.password;
  }
  return true;
}
bool RoomRuntime::setup(uint32_t initial_epoch, const ::meshcore_lib::NodeRuntime &node) {
  if (!this->enabled())
    return true;
  if (!this->store_.open(this->id_) || !this->store_.load_history())
    return false;
  RoomCredentials credentials;
  const auto state = this->store_.load_credentials(credentials);
  if (state == StoreResult::ERROR)
    return false;
  if (this->static_credentials_)
    return this->apply_static_credentials_(credentials, state, initial_epoch, node) && this->activate_(credentials);
  if (state == StoreResult::MISSING)
    return true;
  return state == StoreResult::OK && this->activate_(credentials);
}
bool RoomRuntime::provision(const char *id, const char *seed_hex, const char *password, uint32_t epoch,
                            const ::meshcore_lib::NodeRuntime &node) {
  if (this->static_credentials_ || !this->enabled() || this->id_ != id || std::strlen(seed_hex) != 64 ||
      epoch < 1704067200UL || epoch > 4102444799UL || std::strlen(password) < 12 || std::strlen(password) > 15)
    return false;
  RoomCredentials credentials;
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    return -1;
  };
  for (size_t i = 0; i < credentials.seed.size(); ++i) {
    int hi = hex(seed_hex[2 * i]), lo = hex(seed_hex[2 * i + 1]);
    if (hi < 0 || lo < 0)
      return false;
    credentials.seed[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  for (size_t i = 0; password[i]; ++i)
    if (password[i] < 33 || password[i] > 126)
      return false;
  std::copy_n(password, std::strlen(password), credentials.password.begin());
  credentials.epoch = epoch;
  ::meshcore_lib::LocalIdentity check;
  if (!this->identities_.derive(credentials.seed.data(), check) ||
      !node.identity_available(check.public_key, &this->service_) || !this->store_.provision(credentials))
    return false;
  return this->ready_ || this->activate_(credentials);
}
bool RoomRuntime::change_password(const char *password) {
  if (this->static_credentials_ || !this->ready_ || this->access_ != ::meshcore_lib::RoomAccess::PRIVATE ||
      std::strlen(password) < 12 || std::strlen(password) > 15)
    return false;
  for (size_t i = 0; password[i]; ++i)
    if (password[i] < 33 || password[i] > 126)
      return false;
  return this->store_.change_password(password) && this->service_.set_password(password);
}
bool RoomRuntime::create_advertisement(uint32_t epoch, bool flood, ::meshcore_lib::Frame &out,
                                       const ::meshcore_lib::FloodScope *scope) {
  if (!this->ready_ || epoch == 0 || this->last_advert_epoch_ == UINT32_MAX || this->restored_epoch_ == UINT32_MAX ||
      (scope != nullptr && (!flood || !scope->valid)))
    return false;
  epoch = std::max({epoch, this->restored_epoch_, this->store_.clock_floor()});
  if (epoch <= this->last_advert_epoch_)
    epoch = this->last_advert_epoch_ + 1;
  if (!this->checkpoint(epoch) || !this->service_.advert(epoch, out, flood, scope))
    return false;
  this->last_advert_epoch_ = epoch;
  return true;
}
void RoomRuntime::dispatch_posts(float rssi, float snr) {
  const uint32_t received = this->service_.received_posts();
  if (received == this->dispatched_posts_)
    return;
  // One packet carries at most one post, and last_text() is the post just stored.
  this->dispatched_posts_ = received;
  RoomMessage message;
  const std::string_view text(this->service_.last_text());
  message.text_size = std::min(text.size(), ::meshcore_lib::ROOM_TEXT_CAPACITY);
  std::copy_n(text.begin(), message.text_size, message.text.begin());
  message.rssi = rssi;
  message.snr = snr;
  for (auto &callback : this->post_callbacks_)
    callback(message);
}
}  // namespace esphome::meshcore
