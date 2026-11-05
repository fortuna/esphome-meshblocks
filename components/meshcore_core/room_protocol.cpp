#include "room_protocol.h"
#include <algorithm>

namespace meshcore_lib {
uint32_t read_u32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void write_u32(uint8_t *p, uint32_t value) {
  for (size_t i = 0; i < 4; ++i)
    p[i] = static_cast<uint8_t>(value >> (8 * i));
}
bool parse_packet(const uint8_t *data, size_t size, PacketView &out) {
  out = {};
  if (!data || size < 2 || size > MAX_FRAME_SIZE || (data[0] >> 6) != 0)
    return false;
  out.type = (data[0] >> 2) & 15;
  out.route = data[0] & 3;
  const size_t path_offset = out.route == 0 || out.route == 3 ? 5 : 1;
  if (size <= path_offset)
    return false;
  out.path.descriptor = data[path_offset];
  if (!out.path.valid())
    return false;
  const size_t payload_offset = path_offset + 1 + out.path.size();
  if (payload_offset >= size || size - payload_offset > MAX_PAYLOAD_SIZE)
    return false;
  std::copy_n(data + path_offset + 1, out.path.size(), out.path.bytes.begin());
  out.payload = data + payload_offset;
  out.payload_size = size - payload_offset;
  return true;
}
bool RoomProtocol::envelope_(uint8_t type, bool flood, const PacketPath &path, const uint8_t *payload, size_t size,
                             Frame &out) {
  out = {};
  if (type > 15 || !path.valid() || size == 0 || size > MAX_PAYLOAD_SIZE || 2 + path.size() + size > MAX_FRAME_SIZE)
    return false;
  out.bytes[0] = static_cast<uint8_t>((type << 2) | (flood ? 1 : 2));
  out.bytes[1] = path.descriptor;
  std::copy_n(path.bytes.begin(), path.size(), out.bytes.begin() + 2);
  std::copy_n(payload, size, out.bytes.begin() + 2 + path.size());
  out.size = 2 + path.size() + size;
  return true;
}
bool RoomProtocol::advert(const LocalIdentity &id, std::string_view name, uint32_t epoch, Frame &out, bool flood,
                          const FloodScope *scope, AdvertRole role) {
  out = {};
  if ((role != AdvertRole::CHAT && role != AdvertRole::ROOM) || name.empty() || name.size() > 24 || epoch == 0 ||
      name.find('\0') != std::string_view::npos || (scope != nullptr && (!flood || !scope->valid)))
    return false;
  std::array<uint8_t, 125> payload{};
  std::copy(id.public_key.begin(), id.public_key.end(), payload.begin());
  write_u32(payload.data() + 32, epoch);
  payload[100] = 0x80 | static_cast<uint8_t>(role);  // Role | ADV_NAME_MASK; no location.
  std::copy(name.begin(), name.end(), payload.begin() + 101);
  std::array<uint8_t, 61> message{};
  std::copy_n(payload.begin(), 36, message.begin());
  std::copy_n(payload.begin() + 100, name.size() + 1, message.begin() + 36);
  if (!this->identities_.sign(id, message.data(), 37 + name.size(), payload.data() + 36))
    return false;
  if (!this->envelope_(4, flood, PacketPath{}, payload.data(), 101 + name.size(), out))
    return false;
  if (scope != nullptr && !scope_flood_frame(this->crypto_, *scope, out)) {
    out = {};
    return false;
  }
  return true;
}
bool RoomProtocol::decrypt_(const SharedSecret &secret, const uint8_t *data, size_t size, Plaintext &out) {
  out = {};
  if (size < 18 || size - 2 > out.bytes.size() || (size - 2) % 16 != 0)
    return false;
  std::array<uint8_t, 32> mac{};
  if (!this->crypto_.hmac_sha256(secret.data(), data + 2, size - 2, mac.data()) ||
      ((mac[0] ^ data[0]) | (mac[1] ^ data[1])) != 0)
    return false;
  if (!this->crypto_.aes128_ecb(false, secret.data(), data + 2, size - 2, out.bytes.data()))
    return false;
  out.size = size - 2;
  return true;
}
bool RoomProtocol::open_anonymous(const PacketView &packet, const LocalIdentity &id, PublicKey &peer,
                                  SharedSecret &secret, Plaintext &out) {
  if (packet.type != 7 || packet.payload_size < 51 || packet.payload[0] != id.public_key[0])
    return false;
  std::copy_n(packet.payload + 1, peer.size(), peer.begin());
  if (!this->identities_.shared_secret(id, peer, secret))
    return false;
  return this->decrypt_(secret, packet.payload + 33, packet.payload_size - 33, out);
}
bool RoomProtocol::open_peer(const PacketView &packet, const SharedSecret &secret, Plaintext &out) {
  return packet.payload_size >= 20 && this->decrypt_(secret, packet.payload + 2, packet.payload_size - 2, out);
}
bool RoomProtocol::datagram(uint8_t type, const LocalIdentity &id, const PublicKey &peer, const SharedSecret &secret,
                            const uint8_t *data, size_t size, bool flood, const PacketPath &path, Frame &out) {
  out = {};
  if (size == 0 || size > MAX_CIPHERTEXT_SIZE || !data)
    return false;
  const size_t encrypted_size = (size + 15) / 16 * 16;
  std::array<uint8_t, MAX_CIPHERTEXT_SIZE> padded{};
  std::copy_n(data, size, padded.begin());
  std::array<uint8_t, MAX_PAYLOAD_SIZE> payload{};
  payload[0] = peer[0];
  payload[1] = id.public_key[0];
  if (!this->crypto_.aes128_ecb(true, secret.data(), padded.data(), encrypted_size, payload.data() + 4))
    return false;
  std::array<uint8_t, 32> mac{};
  if (!this->crypto_.hmac_sha256(secret.data(), payload.data() + 4, encrypted_size, mac.data()))
    return false;
  payload[2] = mac[0];
  payload[3] = mac[1];
  return this->envelope_(type, flood, path, payload.data(), 4 + encrypted_size, out);
}
bool RoomProtocol::path_response(const LocalIdentity &id, const PublicKey &peer, const SharedSecret &secret,
                                 const PacketPath &received_path, uint8_t extra_type, const uint8_t *extra,
                                 size_t extra_size, Frame &out) {
  out = {};
  if (!received_path.valid() || extra_size == 0 || received_path.size() + extra_size + 5 > 166)
    return false;
  std::array<uint8_t, MAX_CIPHERTEXT_SIZE> plain{};
  plain[0] = received_path.descriptor;
  std::copy_n(received_path.bytes.begin(), received_path.size(), plain.begin() + 1);
  plain[1 + received_path.size()] = extra_type;
  std::copy_n(extra, extra_size, plain.begin() + 2 + received_path.size());
  PacketPath outbound;
  outbound.descriptor = received_path.descriptor & 0xc0;
  return this->datagram(8, id, peer, secret, plain.data(), 2 + received_path.size() + extra_size, true, outbound, out);
}
bool RoomProtocol::ack(const uint8_t token[4], bool flood, const PacketPath &path, Frame &out, int unsynced) {
  std::array<uint8_t, 5> payload{};
  std::copy_n(token, 4, payload.begin());
  payload[4] = static_cast<uint8_t>(unsynced);
  return this->envelope_(3, flood, path, payload.data(), unsynced < 0 ? 4 : 5, out);
}
bool RoomProtocol::ack_token(const uint8_t *plain, size_t size, const PublicKey &peer, std::array<uint8_t, 4> &out) {
  if (size > MAX_CIPHERTEXT_SIZE)
    return false;
  std::array<uint8_t, MAX_CIPHERTEXT_SIZE + 32> input{};
  std::copy_n(plain, size, input.begin());
  std::copy(peer.begin(), peer.end(), input.begin() + size);
  std::array<uint8_t, 32> digest{};
  if (!this->crypto_.sha256(input.data(), size + peer.size(), digest.data()))
    return false;
  std::copy_n(digest.begin(), out.size(), out.begin());
  return true;
}
std::array<uint8_t, 56> RoomProtocol::status_payload(uint32_t tag, const NodeStatus &node, uint32_t posts,
                                                     uint32_t pushes) {
  std::array<uint8_t, 56> out{};
  auto u16 = [&](size_t offset, uint16_t value) {
    out[offset] = value;
    out[offset + 1] = value >> 8;
  };
  write_u32(out.data(), tag);
  u16(4, node.battery_mv.value_or(0));
  u16(6, node.tx_queue_length);
  u16(8, static_cast<uint16_t>(node.noise_floor.value_or(0)));
  u16(10, static_cast<uint16_t>(node.last_rssi));
  write_u32(out.data() + 12, node.packets_received);
  write_u32(out.data() + 16, node.packets_sent);
  write_u32(out.data() + 20, node.tx_airtime_seconds);
  write_u32(out.data() + 24, node.uptime_seconds);
  write_u32(out.data() + 28, node.sent_flood);
  write_u32(out.data() + 32, node.sent_direct);
  write_u32(out.data() + 36, node.received_flood);
  write_u32(out.data() + 40, node.received_direct);
  u16(44, node.error_flags.value_or(0));
  u16(46, static_cast<uint16_t>(node.last_snr_quarters));
  u16(48, node.direct_duplicates.value_or(0));
  u16(50, node.flood_duplicates.value_or(0));
  u16(52, static_cast<uint16_t>(posts));
  u16(54, static_cast<uint16_t>(pushes));
  return out;
}
}  // namespace meshcore_lib
