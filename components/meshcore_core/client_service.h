#pragma once

#include "room_service.h"

namespace meshcore_lib {

/// Parse exactly 64 hexadecimal characters as a full public key; reject reserved prefixes, never truncate.
bool parse_public_key(std::string_view text, PublicKey &out);

enum class ClientSendState { IDLE, QUEUED, SENT, ACKNOWLEDGED, DELIVERED_LOCALLY, FAILED, TIMED_OUT };
struct ClientMessage {
  PublicKey sender{};
  uint32_t timestamp{};
  ChannelText text;
  bool local{};
  std::string_view body() const { return this->text.view(); }
  ChannelText sender_public_key() const;
};
class ClientMessageListener {
 public:
  virtual ~ClientMessageListener() = default;
  virtual void on_client_message(const ClientMessage &message) = 0;
  virtual void on_client_send_state(ClientSendState state) = 0;
};

// Chat endpoint with bounded explicit peers; not a companion interface or room member.
class ClientService {
 public:
  static constexpr uint64_t ACK_TIMEOUT_MS = 30000;
  static constexpr size_t MAX_PEERS = 8;
  enum class Match { NONE, ONE, AMBIGUOUS };
  /// Add a full remote peer key during composition; duplicate keys are idempotent, capacity is fixed.
  bool add_peer(const PublicKey &key);
  /// Borrow an optional owner-context listener; it must outlive the service.
  void set_listener(ClientMessageListener *listener) { this->listener_ = listener; }
  /// Authenticate all configured peer candidates without mutation; reject unsupported text envelopes.
  Match prepare_message(const PacketView &packet, ClientMessage &out);
  /// Send a receipt and deliver a uniquely authenticated RF message once; retain per-peer replay state.
  bool accept_message(const PacketView &packet, const ClientMessage &message, uint64_t now_ms);
  /// Prepare an encrypted local send after the owning node has reserved queue space; no RF/ACK reservation.
  bool queue_local_message(const PublicKey &peer, std::string_view text, uint32_t timestamp, uint8_t width, Frame &out);
  /// Decode a loopback envelope using explicit receive peers, exactly as for radio ingress.
  bool decode_local_message(const Frame &frame, ClientMessage &out);
  /// Complete one queued local operation; this never claims an RF acknowledgment.
  void complete_local(bool success);
  /// Deliver an owning message to the listener in the node context, never recursively through send().
  void deliver_message(const ClientMessage &message);
  bool pending_radio() const { return this->pending() && !this->local_pending_; }
  /// Borrow serially used protocol, identity crypto and shared node transport for this service's lifetime.
  ClientService(RoomProtocol &protocol, IdentityCrypto &identities, RoomTransport &transport)
      : protocol_(protocol), identities_(identities), transport_(transport) {}
  ClientService(const ClientService &) = delete;
  ClientService &operator=(const ClientService &) = delete;
  /// Copy a validated identity once; registry uniqueness must be checked before activating the node.
  bool configure(const LocalIdentity &identity);
  /// Borrow the full public identity; private key material is never exposed.
  const PublicKey &public_key() const { return this->identity_.public_key; }
  /// Encode a signed chat advertisement; callers own timestamp selection and queue admission.
  bool advertisement(std::string_view name, uint32_t timestamp, bool flood, Frame &out,
                     const FloodScope *scope = nullptr);
  /// Admit one encrypted plain-text flood to a full peer identity; true means queued only.
  /// One pending direct message per identity, no automatic retry; views are copied before returning.
  bool send_client_message(const PublicKey &peer, std::string_view text, uint32_t timestamp, uint8_t path_hash_size,
                           uint64_t now_ms);
  /// Apply the shared scheduler's actual completion; only successful TX starts the ACK timeout.
  void transmitted(uint32_t token, bool success, uint64_t now_ms);
  /// Expire the outstanding receipt at its monotonic deadline; no retransmission occurs.
  void tick(uint64_t now_ms);
  /// Return the ACK deadline or UINT64_MAX when no timer is needed.
  uint64_t next_deadline() const;
  /// Read the most recently admitted direct-message operation's volatile state.
  ClientSendState send_state() const { return this->state_; }
  /// Report whether this client is holding a queued or transmitted message.
  bool pending() const { return this->state_ == ClientSendState::QUEUED || this->state_ == ClientSendState::SENT; }
  /// Borrow the pending receipt token; meaningful only while pending().
  const std::array<uint8_t, 4> &pending_ack() const { return this->ack_; }
  /// Check a raw short ACK, optionally requiring actual TX; raw receipts are not authenticated.
  bool ack_matches(const uint8_t *ack, bool transmitted_only) const;
  /// Authenticate a PATH receipt against the pending full peer before any state change.
  bool matches_path_ack(const PacketView &packet);
  /// Complete a uniquely dispatched receipt after actual TX; NodeRuntime owns ambiguity checks.
  void acknowledge();
  /// Report whether composition supplied an identity.
  bool configured() const { return this->configured_; }

 private:
  bool encode_message_(const PublicKey &peer, std::string_view text, uint32_t timestamp, uint8_t width, Frame &out,
                       SharedSecret &secret, std::array<uint8_t, 4> &ack);
  bool decode_message_(const PacketView &packet, const PublicKey &sender, ClientMessage &out);
  void set_state_(ClientSendState state);
  struct Peer {
    PublicKey key{};
    uint32_t last_timestamp{};
    ChannelText last_text;
  };
  std::array<Peer, MAX_PEERS> peers_{};
  size_t peer_count_{};
  ClientMessageListener *listener_{};
  bool local_pending_{};
  RoomProtocol &protocol_;
  IdentityCrypto &identities_;
  RoomTransport &transport_;
  LocalIdentity identity_{};
  PublicKey peer_{};
  SharedSecret secret_{};
  std::array<uint8_t, 4> ack_{};
  uint32_t token_{};
  uint64_t deadline_{};
  ClientSendState state_{ClientSendState::IDLE};
  bool configured_{};
};
}  // namespace meshcore_lib
