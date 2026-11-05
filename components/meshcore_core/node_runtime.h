#pragma once
#include "room_service.h"
#include "client_service.h"

namespace meshcore_lib {
/// Composition-owned local delivery slot; queued frames never enter the radio scheduler.
struct LocalClientDelivery {
  ClientService *sender{}, *recipient{};
  Frame frame;
};
// One execution context, bounded room/client registries, one decoded ingress frame.
// Composition owns services/stores; this runtime never owns or polls a radio.
class NodeRuntime {
 public:
  /// Borrow exclusive fixed registry storage; the array and registered services must outlive this runtime.
  /// No allocation/resizing occurs; a zero-length array is valid for a channel-only node.
  template<size_t N>
  explicit NodeRuntime(std::array<RoomService *, N> &storage) : rooms_(storage.data()), capacity_(N) {}
  /// Borrow separate exclusive room/client registries; all backing arrays/services must outlive this runtime.
  template<size_t R, size_t C>
  NodeRuntime(std::array<RoomService *, R> &rooms, std::array<ClientService *, C> &clients)
      : rooms_(rooms.data()), capacity_(R), clients_(clients.data()), client_capacity_(C) {}
  /// Borrow bounded local delivery storage as well as registries; one frame is dispatched per tick.
  template<size_t R, size_t C, size_t Q>
  NodeRuntime(std::array<RoomService *, R> &rooms, std::array<ClientService *, C> &clients,
              std::array<LocalClientDelivery, Q> &queue)
      : rooms_(rooms.data()),
        capacity_(R),
        clients_(clients.data()),
        client_capacity_(C),
        local_queue_(queue.data()),
        local_capacity_(Q) {}
  /// Admit an encrypted message between distinct registered clients, using full keys and no RF airtime.
  bool send_local_client_message(ClientService &sender, const PublicKey &recipient, std::string_view text,
                                 uint32_t timestamp, uint8_t width);
  /// Register a configured client, rejecting duplicate full identities across all local services.
  bool add_client(ClientService &client);
  /// Return the number of registered client identities.
  size_t clients() const { return this->client_count_; }
  /// Prevent copying an owner that borrows exclusive registry storage and coordinates ACK reservations.
  NodeRuntime(const NodeRuntime &) = delete;
  /// Prevent assignment that would duplicate registry/ACK ownership.
  NodeRuntime &operator=(const NodeRuntime &) = delete;
  // Interim bench throttle inherited from the first room prototype, not an airtime-utilization calculation.
  static constexpr uint64_t PUSH_INTERVAL_MS = 1200;
  static constexpr uint64_t ACK_REUSE_GUARD_MS = 180000;
  static constexpr size_t RECENT_ACK_CAPACITY = 256;
  /// Register a configured borrowed service; re-registering the same instance is a no-op success.
  /// Reject capacity exhaustion or an identity already owned by another service; no radio is created.
  bool add_room(RoomService &room);
  /// Check full-key uniqueness among rooms and clients, optionally excluding one service; not an authorization check.
  bool identity_available(const PublicKey &identity, const RoomService *except = nullptr) const;
  /// Authenticate encrypted candidates and dispatch one room/client match; raw ACKs use token matching without
  /// authentication. now_ms is monotonic milliseconds; epoch is protocol epoch seconds. Ambiguous packet/ACK matches
  /// are dropped.
  void receive(const PacketView &packet, uint64_t now_ms, uint32_t epoch);
  /// Advance rooms and client receipt timeouts in the owner context with monotonic-ms/epoch-second inputs and shared
  /// history-push pacing.
  void tick(uint64_t now_ms, uint32_t epoch);
  /// Return the earliest room scan or client receipt deadline in monotonic milliseconds, or UINT64_MAX.
  uint64_t next_deadline(uint64_t now_ms, uint32_t epoch) const;
  /// Distribute an actual TX completion by device-wide unique token; success is not a member delivery ACK.
  void transmitted(uint32_t token, bool success, uint64_t now_ms);
  /// Check pending/recent tokens and free reservation capacity using the last receive/tick time.
  /// False applies backpressure rather than reusing a live four-byte ACK reservation.
  bool ack_available(const std::array<uint8_t, 4> &ack) const;
  /// Return the count of registered active services, which may be smaller than the compiled capacity.
  size_t rooms() const { return this->count_; }
  /// Return the fixed number of slots in the borrowed registry; it never grows at runtime.
  size_t capacity() const { return this->capacity_; }
  /// Count authenticated packet matches dropped because more than one identity/session could own them.
  uint32_t ambiguous_packets() const { return this->ambiguous_packets_; }
  /// Count raw ACKs dropped because multiple transmitted operations matched their short token.
  uint32_t ambiguous_acks() const { return this->ambiguous_acks_; }

 private:
  RoomService **const rooms_;
  const size_t capacity_;
  ClientService **clients_{};
  size_t client_capacity_{}, client_count_{};
  LocalClientDelivery *local_queue_{};
  size_t local_capacity_{}, local_head_{}, local_count_{};
  void deliver_local_();
  struct RecentAck {
    std::array<uint8_t, 4> token{};
    uint64_t until{};
  };
  std::array<RecentAck, RECENT_ACK_CAPACITY> recent_acks_{};
  void remember_ack_(const std::array<uint8_t, 4> &ack, uint64_t now_ms, bool renew = false);
  void remember_pending_acks_(uint64_t now_ms);
  size_t count_{}, next_room_{};
  uint64_t next_push_{}, next_login_{}, now_{};
  uint32_t ambiguous_packets_{}, ambiguous_acks_{};
};
}  // namespace meshcore_lib
