#include "nvs_room_store.h"
#include <algorithm>
#include <cstring>
namespace esphome::meshcore {
namespace {
constexpr size_t CREDENTIAL_BYTES = 73;               // magic/version(4), epoch(4), seed(32), password(33).
constexpr size_t PRIVATE_KEY_CREDENTIAL_BYTES = 105;  // v2: private key(64) in place of seed(32).
constexpr size_t POST_BYTES = 32 + 4 + ::meshcore_lib::ROOM_TEXT_CAPACITY + 1;
constexpr size_t HISTORY_BYTES = 8 + ::meshcore_lib::ROOM_HISTORY_CAPACITY * POST_BYTES;
}  // namespace
NvsRoomStore::~NvsRoomStore() {
  if (this->opened_)
    nvs_close(this->handle_);
}
bool NvsRoomStore::open(const std::string &stable_id) {
  if (this->opened_ || stable_id.empty() || stable_id.size() > 12)
    return false;
  this->opened_ = nvs_open(("mc_" + stable_id).c_str(), NVS_READWRITE, &this->handle_) == ESP_OK;
  return this->opened_;
}
bool NvsRoomStore::write_blob_(const char *key, const uint8_t *data, size_t size) {
  if (!this->opened_ || !this->healthy_ || size > HISTORY_BYTES)
    return false;
  if (nvs_set_blob(this->handle_, key, data, size) != ESP_OK || nvs_commit(this->handle_) != ESP_OK) {
    this->healthy_ = false;
    return false;
  }
  std::array<uint8_t, HISTORY_BYTES> readback{};
  size_t length = readback.size();
  this->healthy_ = nvs_get_blob(this->handle_, key, readback.data(), &length) == ESP_OK && length == size &&
                   std::equal(data, data + size, readback.begin());
  return this->healthy_;
}
StoreResult NvsRoomStore::load_credentials(RoomCredentials &out) {
  out = {};
  if (!this->opened_)
    return StoreResult::ERROR;
  std::array<uint8_t, PRIVATE_KEY_CREDENTIAL_BYTES> data{};
  size_t size = data.size();
  const auto error = nvs_get_blob(this->handle_, "identity", data.data(), &size);
  if (error == ESP_ERR_NVS_NOT_FOUND)
    return StoreResult::MISSING;
  const bool seed_record = size == CREDENTIAL_BYTES && ::meshcore_lib::read_u32(data.data()) == 0x3152434d;
  const bool key_record = size == PRIVATE_KEY_CREDENTIAL_BYTES && ::meshcore_lib::read_u32(data.data()) == 0x3252434d;
  if (error != ESP_OK || (!seed_record && !key_record) || data[size - 1] != 0)
    return StoreResult::ERROR;
  out.epoch = ::meshcore_lib::read_u32(data.data() + 4);
  out.has_private_key = key_record;
  if (key_record)
    std::copy_n(data.begin() + 8, 64, out.private_key.begin());
  else
    std::copy_n(data.begin() + 8, 32, out.seed.begin());
  std::copy_n(data.begin() + (key_record ? 72 : 40), 33, out.password.begin());
  const size_t length = std::strlen(out.password.data());
  if (length < 12 || length > 32 || out.epoch < 1704067200UL)
    return StoreResult::ERROR;
  return StoreResult::OK;
}
bool NvsRoomStore::provision(const RoomCredentials &credentials) {
  RoomCredentials current;
  const auto state = this->load_credentials(current);
  if (state == StoreResult::ERROR)
    return false;
  if (state == StoreResult::OK)
    return current.has_private_key == credentials.has_private_key && current.seed == credentials.seed &&
           current.private_key == credentials.private_key && current.password == credentials.password;
  return this->write_credentials_(credentials);
}
bool NvsRoomStore::change_password(const char *password) {
  const size_t length = std::strlen(password);
  if (length < 12 || length > 15 || !this->healthy_)
    return false;
  RoomCredentials current;
  if (this->load_credentials(current) != StoreResult::OK)
    return false;
  if (std::strcmp(current.password.data(), password) == 0)
    return true;
  current.password.fill(0);
  std::copy_n(password, length, current.password.begin());
  return this->write_credentials_(current);
}
bool NvsRoomStore::write_credentials_(const RoomCredentials &credentials) {
  std::array<uint8_t, PRIVATE_KEY_CREDENTIAL_BYTES> data{};
  ::meshcore_lib::write_u32(data.data(), credentials.has_private_key ? 0x3252434d : 0x3152434d);
  ::meshcore_lib::write_u32(data.data() + 4, credentials.epoch);
  if (credentials.has_private_key)
    std::copy(credentials.private_key.begin(), credentials.private_key.end(), data.begin() + 8);
  else
    std::copy(credentials.seed.begin(), credentials.seed.end(), data.begin() + 8);
  std::copy(credentials.password.begin(), credentials.password.end(),
            data.begin() + (credentials.has_private_key ? 72 : 40));
  return this->write_blob_("identity", data.data(),
                           credentials.has_private_key ? PRIVATE_KEY_CREDENTIAL_BYTES : CREDENTIAL_BYTES);
}
bool NvsRoomStore::load_history() {
  this->count_ = 0;
  std::array<uint8_t, HISTORY_BYTES> data{};
  size_t size = data.size();
  const auto error = nvs_get_blob(this->handle_, "history", data.data(), &size);
  if (error != ESP_ERR_NVS_NOT_FOUND) {
    if (error != ESP_OK || size != data.size() || ::meshcore_lib::read_u32(data.data()) != 0x3148434d)
      return false;
    const auto count = ::meshcore_lib::read_u32(data.data() + 4);
    if (count > this->posts_.size())
      return false;
    for (size_t i = 0; i < count; ++i) {
      const uint8_t *p = data.data() + 8 + i * POST_BYTES;
      std::copy_n(p, 32, this->posts_[i].author.begin());
      this->posts_[i].timestamp = ::meshcore_lib::read_u32(p + 32);
      std::copy_n(p + 36, this->posts_[i].text.size(), this->posts_[i].text.begin());
      if (this->posts_[i].text.back() != 0 || this->posts_[i].text[0] == 0 || this->posts_[i].timestamp == 0 ||
          (i && this->posts_[i].timestamp <= this->posts_[i - 1].timestamp))
        return false;
    }
    this->count_ = count;
  }
  auto clock_error = nvs_get_u32(this->handle_, "clock", &this->clock_floor_);
  if (clock_error == ESP_ERR_NVS_NOT_FOUND)
    this->clock_floor_ = 0;
  else if (clock_error != ESP_OK)
    return false;
  this->clock_floor_ = std::max(this->clock_floor_, this->latest_timestamp());
  return true;
}
bool NvsRoomStore::checkpoint(uint32_t epoch) {
  if (!this->opened_ || !this->healthy_)
    return false;
  if (epoch <= this->clock_floor_)
    return true;
  if (nvs_set_u32(this->handle_, "clock", epoch) != ESP_OK || nvs_commit(this->handle_) != ESP_OK) {
    this->healthy_ = false;
    return false;
  }
  uint32_t readback;
  if (nvs_get_u32(this->handle_, "clock", &readback) != ESP_OK || readback != epoch) {
    this->healthy_ = false;
    return false;
  }
  this->clock_floor_ = epoch;
  return true;
}
bool NvsRoomStore::append(const ::meshcore_lib::RoomPost &post) {
  if (post.timestamp <= this->latest_timestamp() || post.text.back() != 0 || post.text[0] == 0)
    return false;
  std::array<uint8_t, HISTORY_BYTES> data{};
  const size_t count = std::min(this->count_ + 1, this->posts_.size());
  const size_t skip = this->count_ == this->posts_.size() ? 1 : 0;
  ::meshcore_lib::write_u32(data.data(), 0x3148434d);
  ::meshcore_lib::write_u32(data.data() + 4, count);
  for (size_t i = 0; i < count; ++i) {
    const auto &entry = i + 1 == count ? post : this->posts_[i + skip];
    uint8_t *p = data.data() + 8 + i * POST_BYTES;
    std::copy(entry.author.begin(), entry.author.end(), p);
    ::meshcore_lib::write_u32(p + 32, entry.timestamp);
    std::copy(entry.text.begin(), entry.text.end(), p + 36);
  }
  if (!this->write_blob_("history", data.data(), data.size()))
    return false;
  for (size_t i = 0; i + 1 < count; ++i)
    if (skip)
      this->posts_[i] = this->posts_[i + skip];
  this->posts_[count - 1] = post;
  this->count_ = count;
  this->clock_floor_ = std::max(this->clock_floor_, post.timestamp);
  return true;
}
bool NvsRoomStore::get(size_t index, ::meshcore_lib::RoomPost &out) const {
  if (index >= this->count_)
    return false;
  out = this->posts_[index];
  return true;
}
}  // namespace esphome::meshcore
