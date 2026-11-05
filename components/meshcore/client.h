#pragma once

#include "channel.h"
#include "esphome/components/meshcore_core/node_runtime.h"
#include "ed25519_identity.h"
#include "mbedtls_crypto.h"
#include "esphome/components/meshcore_core/client_service.h"

namespace esphome::meshcore {

class Client : public ::meshcore_lib::ClientMessageListener {
 public:
  /// Borrow the node for this handle's lifetime; all operations run in its owning execution context.
  explicit Client(MeshCoreComponent *parent);
  /// Import a static private key and copy labels once during composition; no NVS records are created.
  bool configure(const std::string &id, const std::string &name, const std::vector<uint8_t> &key);
  /// Borrow the configured local handle/name; neither is a cryptographic identity.
  const std::string &id() const { return this->id_; }
  const std::string &name() const { return this->name_; }
  /// Register the owned portable service with the node's identity/receipt dispatcher during composition.
  bool register_with(::meshcore_lib::NodeRuntime &node);
  /// Borrow the full public identity, safe for contact exchange.
  const ::meshcore_lib::PublicKey &public_key() const { return this->service_.public_key(); }
  /// Return the full public key as an owning hexadecimal value for contact exchange and actions.
  ::meshcore_lib::ChannelText public_key_hex() const;
  /// Add a full remote receive peer during composition; malformed/capacity errors fail node setup.
  void add_peer(const std::vector<uint8_t> &key);
  /// Log the client ID and name without key material.
  void dump_config() const;
  /// Register an owning message callback during composition, invoked serially in the node context.
  void add_on_message_callback(std::function<void(::meshcore_lib::ClientMessage)> callback) {
    this->message_callbacks_.add(std::move(callback));
  }
  void on_client_message(const ::meshcore_lib::ClientMessage &message) override;
  void on_client_send_state(::meshcore_lib::ClientSendState state) override;
  /// Attempt an encrypted DM to a full hex public key; true is admission, not delivery.
  bool send_client_message(std::string_view recipient, std::string_view text, uint32_t timestamp);
  /// Queue a signed chat advertisement using exact caller time; scope is supported only for flood mode.
  bool advertise(uint32_t timestamp, bool flood, const ::meshcore_lib::FloodScope *scope = nullptr);
  /// Read the last admitted DM's state; rejected calls leave this state unchanged.
  ::meshcore_lib::ClientSendState send_state() const { return this->service_.send_state(); }

 private:
  MeshCoreComponent *parent_;
  MbedTlsCrypto crypto_;
  Ed25519Identity identities_;
  ::meshcore_lib::RoomProtocol protocol_{this->crypto_, this->identities_};
  ::meshcore_lib::ClientService service_;
  std::string id_, name_;
  CallbackManager<void(::meshcore_lib::ClientMessage)> message_callbacks_;
};

class ClientMessageTrigger : public Trigger<::meshcore_lib::ClientMessage> {
 public:
  /// Borrow the client for the automation's lifetime; callback arguments own their bounded text.
  explicit ClientMessageTrigger(Client *client) {
    client->add_on_message_callback([this](::meshcore_lib::ClientMessage message) { this->trigger(message); });
  }
};

template<typename... Ts> class SendClientMessageAction : public Action<Ts...> {
 public:
  /// Borrow the configured sending client for this action's lifetime.
  explicit SendClientMessageAction(Client *sender) : sender_(sender) {}
  /// Store the recipient's hex public key or a lambda returning an owning bounded value.
  template<typename V> void set_recipient_public_key(V value) { this->recipient_public_key_ = value; }
  /// Store message text; send-time bounds reject invalid lambda results.
  template<typename V> void set_message(V value) { this->message_ = value; }
  /// Store caller-selected epoch seconds; no implicit clock policy is applied.
  template<typename V> void set_timestamp(V value) { this->timestamp_ = value; }
  /// Evaluate bounded arguments and attempt admission once, without retrying.
  void play(const Ts &...x) override {
    const auto text = this->message_.value(x...);
    const auto timestamp = this->timestamp_.value(x...);
    const auto peer = this->recipient_public_key_.value(x...);
    this->sender_->send_client_message(peer.view(), text.view(), timestamp);
  }

 private:
  Client *sender_;
  TemplatableStorage<::meshcore_lib::ChannelText, Ts...> recipient_public_key_{}, message_{};
  TemplatableStorage<uint32_t, Ts...> timestamp_{};
};

template<typename... Ts> class AdvertiseClientAction : public Action<Ts...> {
 public:
  /// Borrow the client, copying mode and optional scope during composition.
  AdvertiseClientAction(Client *client, bool flood, const std::vector<uint8_t> &scope_key)
      : client_(client), flood_(flood), scoped_(!scope_key.empty()) {
    if (this->scoped_)
      ::meshcore_lib::make_flood_scope(scope_key.data(), scope_key.size(), this->scope_);
  }
  /// Store a caller-selected advertisement timestamp in epoch seconds.
  template<typename V> void set_timestamp(V value) { this->timestamp_ = value; }
  /// Attempt one queue admission; the client logs rejection.
  void play(const Ts &...x) override {
    this->client_->advertise(this->timestamp_.value(x...), this->flood_, this->scoped_ ? &this->scope_ : nullptr);
  }

 private:
  Client *client_;
  bool flood_, scoped_;
  ::meshcore_lib::FloodScope scope_{};
  TemplatableStorage<uint32_t, Ts...> timestamp_{};
};
}  // namespace esphome::meshcore
