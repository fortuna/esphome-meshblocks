#pragma once
#include "room_protocol.h"

namespace meshcore_lib {
constexpr size_t ROOM_CLIENT_CAPACITY = 4;
constexpr size_t ROOM_HISTORY_CAPACITY = 8;
constexpr size_t ROOM_TEXT_CAPACITY = 150;
enum class RoomAccess { PRIVATE, PUBLIC };
struct RoomPost {
  PublicKey author{};
  uint32_t timestamp{};
  std::array<char, ROOM_TEXT_CAPACITY + 1> text{};
};
struct RoomLoginDiagnostics {
  uint32_t crypto_failures{}, password_failures{}, replays{}, capacity_failures{};
  uint32_t evictions{};
  uint32_t replies_sent{}, reply_failures{}, paths_received{}, keepalives{}, keepalive_no_path{};
  uint32_t status_requests{}, status_replies{};
};
// append() reports verified durable success before a post is ACKed. A failed or
// uncertain commit must not advance the visible state; adapters must stop writes
// until recovery if they cannot determine the durable outcome. Posts are copied.
class RoomRepository {
 public:
  /// Allow backend destruction through the repository port; the room does not own the injected store.
  virtual ~RoomRepository() = default;
  /// Commit a copied post and report verified durable success before it can be ACKed; false withholds success.
  virtual bool append(const RoomPost &post) = 0;
  /// Return the number of retained posts, not the lifetime number accepted.
  virtual size_t count() const = 0;
  /// Copy a post by oldest-first index; false means no valid post was returned.
  virtual bool get(size_t oldest_first_index, RoomPost &out) const = 0;
  /// Return the newest retained post timestamp in epoch seconds, or zero for empty history.
  virtual uint32_t latest_timestamp() const = 0;
};
class RoomTransport {
 public:
  /// Allow transport destruction through the port; the room borrows rather than owns this dependency.
  virtual ~RoomTransport() = default;
  // Copies the frame into the ONE device scheduler. False means no ownership
  // transfer. Token identifies actual TX completion, not queue admission.
  /// Queue a copied frame no earlier than not_before_ms (monotonic milliseconds); return its token on success.
  /// Admission is not physical transmission or peer delivery; false transfers no frame ownership.
  virtual bool send(const Frame &frame, uint64_t not_before_ms, uint32_t &token) = 0;
  // Check all rooms on this device, including queued pushes, before reserving a
  // short ACK token. Called in the same execution context as send/receive.
  /// Check device-wide pending/recent ACK reservations; false also covers reservation-capacity backpressure.
  virtual bool ack_available(const std::array<uint8_t, 4> &ack) const = 0;
};

class RoomService {
 public:
  /// Borrow protocol, transport, repository, randomness and status for the lifetime of this room.
  /// All calls and dependency use belong to one execution context.
  RoomService(RoomProtocol &protocol, RoomTransport &transport, RoomRepository &posts, RandomSource &random,
              NodeStatusSource &status)
      : protocol_(protocol), transport_(transport), posts_(posts), random_(random), status_(status) {}
  /// Copy identity/name/password and select access once; false for repeated configuration or invalid bounds.
  /// Accepts legacy passwords up to 32 bytes, loads the latest preview and neither creates keys nor sends traffic.
  bool configure(const LocalIdentity &identity, std::string_view name, std::string_view password,
                 RoomAccess access = RoomAccess::PRIVATE);
  /// Replace a configured private room password (12-15 bytes), clearing sessions only if it changes.
  /// Does not persist it; the application must arrange durable policy changes separately.
  bool set_password(std::string_view password);
  /// Build this configured room's signed direct/empty-path or optionally scoped flood advertisement.
  /// scope is borrowed for this call and only allowed with flood; no frame is queued or sent.
  bool advert(uint32_t epoch, Frame &out, bool flood = false, const FloodScope *scope = nullptr);
  /// Synchronously process a borrowed packet with epoch seconds and monotonic milliseconds.
  /// Use NodeRuntime for co-located identities so candidate matching and raw ACK ownership are coordinated.
  void receive(const PacketView &packet, uint64_t now_ms, uint32_t epoch);
  /// Service retries/session scans using monotonic milliseconds and epoch seconds.
  /// Return true only when a history push was queued; allow_push=false still services due ACK timeouts.
  bool tick(uint64_t now_ms, uint32_t epoch, bool allow_push = true);
  /// Return the next monotonic-ms service scan, or UINT64_MAX when unconfigured/without clients.
  uint64_t next_deadline(uint64_t now_ms) const;
  /// Apply an actual transport completion by globally unique token; successful pushes begin their ACK timeout here.
  void transmitted(uint32_t token, bool success, uint64_t now_ms);
  /// Durably append bounded text under this room's identity; false rejects input or an unsuccessful store commit.
  /// Timestamps use epoch seconds and advance past retained posts; delivery is scheduled later.
  bool add_system_post(std::string_view text, uint32_t epoch);
  /// Count allocated volatile member sessions; this is not a live-presence measurement.
  size_t clients() const;
  /// Return the current retained-history count supplied by this room's repository.
  size_t posts() const { return this->posts_.count(); }
  /// Count admitted logins since service construction, not distinct or currently connected users.
  uint32_t logins() const { return this->logins_; }
  /// Count newly accepted peer posts since construction; retries are not new posts.
  uint32_t received_posts() const { return this->received_posts_; }
  /// Count history pushes acknowledged by members since construction, not merely queued transmissions.
  uint32_t delivered_posts() const { return this->delivered_posts_; }
  /// Return this service's rejected-operation counter; some ignored/unsupported packets are not counted.
  uint32_t rejected() const { return this->rejected_; }
  /// Borrow room-local login/delivery diagnostics; counters are volatile and updated by owner-context calls.
  const RoomLoginDiagnostics &login_diagnostics() const { return this->login_diagnostics_; }
  /// Borrow a NUL-terminated latest-post preview; storage belongs to this service and contents can change.
  const char *last_text() const { return this->last_text_.data(); }
  /// Borrow the full public key of the latest post's author (the room's own key for system posts).
  const PublicKey &last_author() const { return this->last_author_; }
  /// Borrow the room's full public identity, meaningful after configure succeeds.
  const PublicKey &public_key() const { return this->identity_.public_key; }
  /// Borrow the copied NUL-terminated advertised name, empty before configuration.
  const char *name() const { return this->name_.data(); }

 private:
  friend class NodeRuntime;
  enum class Match { NONE, ONE, AMBIGUOUS };
  struct Incoming {
    PublicKey peer{};
    SharedSecret secret{};
    Plaintext plain;
    size_t client_index{};
  };
  Match prepare_(const PacketView &packet, uint64_t now_ms, Incoming &out, bool rate_limit = true);
  void accept_(const PacketView &packet, const Incoming &incoming, uint64_t now_ms, uint32_t epoch);
  size_t ack_matches_(const uint8_t *data, bool transmitted_only) const;
  struct Client {
    bool used{}, path_known{};
    PublicKey key{};
    SharedSecret secret{};
    PacketPath path;
    uint32_t last_timestamp{}, last_post_timestamp{}, sync_since{};
    uint64_t last_activity_ms{};  // Local monotonic activity, never peer/UTC time.
    bool login_reply_pending{};
    uint32_t login_reply_token{};
    std::array<char, ROOM_TEXT_CAPACITY + 1> last_post{};
    bool push_pending{}, push_on_air{};
    std::array<uint8_t, 4> pending_ack{};
    uint32_t push_token{}, push_timestamp{};
    uint64_t ack_deadline{};
    uint8_t failures{};
  };
  bool post_(const PublicKey &author, std::string_view text, uint32_t epoch);
  void login_(const PacketView &packet, const Incoming &incoming, uint64_t now_ms, uint32_t epoch);
  void peer_(const PacketView &packet, Client &client, const Plaintext &plain, uint64_t now_ms, uint32_t epoch);
  void ack_(const uint8_t *data, size_t size, uint64_t now_ms);
  uint8_t unsynced_(const Client &client) const;
  PacketPath reply_path_(const Client &client, const PacketView &packet) const;
  RoomProtocol &protocol_;
  RoomTransport &transport_;
  RoomRepository &posts_;
  RandomSource &random_;
  NodeStatusSource &status_;
  LocalIdentity identity_{};
  RoomAccess access_{RoomAccess::PRIVATE};
  std::array<char, MAX_NAME_SIZE + 1> name_{};
  std::array<char, 33> password_{};
  std::array<Client, ROOM_CLIENT_CAPACITY> clients_{};
  std::array<char, ROOM_TEXT_CAPACITY + 1> last_text_{};
  PublicKey last_author_{};
  bool configured_{};
  size_t next_client_{};
  uint64_t next_push_{}, next_login_{};
  uint32_t logins_{}, received_posts_{}, delivered_posts_{}, rejected_{};
  uint32_t posts_since_boot_{}, pushes_since_boot_{};
  RoomLoginDiagnostics login_diagnostics_{};
};
}  // namespace meshcore_lib
