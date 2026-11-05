#include "channel_state.h"
#include "transmit_state.h"
#include <cassert>
#include <cstring>
#include <iostream>

template<size_t Capacity> void check_transmit_capacity() {
  using namespace esphome::meshcore;
  ChannelTxState<Capacity> previews;
  TransmitState<Capacity> states;
  std::array<ChannelState, Capacity> targets;
  meshcore_lib::ChannelMessage message;
  message.text[0] = 'x';
  message.text_size = 1;
  const uint32_t first = UINT32_MAX - 1;
  for (size_t i = 0; i < Capacity; ++i) {
    TransmitMetadata metadata{i % 2 == 0, i % 3 == 0, {static_cast<uint8_t>(i)}};
    const uint32_t token = first + static_cast<uint32_t>(i);
    assert(states.remember(token, metadata));
    assert(!states.remember(token, metadata));
    assert(previews.remember(token, targets[i], message));
  }
  assert(!states.remember(first + static_cast<uint32_t>(Capacity), {}));
  assert(!previews.remember(first + static_cast<uint32_t>(Capacity), targets[0], message));
  // Exact token matching survives rollover with a three-slot queue: UINT32_MAX
  // and zero have the same token % 3, but belong to distinct transmissions.
  for (size_t i = Capacity; i-- > 0;) {
    const uint32_t token = first + static_cast<uint32_t>(i);
    TransmitMetadata result;
    assert(states.take(token, result));
    assert(result.channel == (i % 2 == 0) && result.flood == (i % 3 == 0));
    assert(result.fingerprint[0] == i);
    assert(!states.take(token, result));
    assert(previews.complete(token, i % 2 == 0));
    assert(targets[i].sent_messages() == (i % 2 == 0 ? 1 : 0));
  }
  assert(states.remember(42, {}) && previews.remember(42, targets[0], message));
  TransmitMetadata result;
  assert(states.take(42, result) && previews.complete(42, false));
}

int main() {
  check_transmit_capacity<1>();
  check_transmit_capacity<3>();
  check_transmit_capacity<8>();
  using namespace esphome::meshcore;
  ChannelState testing, private_chat;
  meshcore_lib::ChannelMessage message;
  const char *first = "Peer: testing message";
  std::strcpy(message.text.data(), first);
  message.text_size = std::strlen(first);
  testing.received(message);
  assert(testing.received_messages() == 1);
  assert(private_chat.received_messages() == 0 && private_chat.last_text()[0] == 0);
  const char *second = "Private peer: secret message";
  std::strcpy(message.text.data(), second);
  message.text_size = std::strlen(second);
  private_chat.received(message);
  message = {};
  assert(std::strcmp(testing.last_text(), first) == 0);
  assert(std::strcmp(private_chat.last_text(), second) == 0);
  // Diagnostics retain raw bytes in bounded owned text. Rendering belongs to the consumer.
  message.text.fill('\xff');
  message.text[0] = '\n';
  message.text_size = message.text.size();
  private_chat.received(message);
  assert(private_chat.received_messages() == 2 && testing.received_messages() == 1);
  assert(std::strlen(private_chat.last_text()) == meshcore_lib::MAX_RECEIVED_TEXT);
  assert(private_chat.last_text()[0] == '\n');
  ChannelState rebooted;
  assert(rebooted.received_messages() == 0 && rebooted.last_text()[0] == 0);

  auto text_message = [](const char *text) {
    meshcore_lib::ChannelMessage result;
    result.text_size = std::strlen(text);
    std::copy_n(text, result.text_size, result.text.begin());
    return result;
  };
  ChannelState public_ui, private_ui;
  ChannelTxState<4> pending;
  auto first_tx = text_message("Node: esphome-pong");
  assert(pending.remember(7, public_ui, first_tx));
  assert(!pending.remember(7, private_ui, first_tx));
  assert(pending.remember(8, private_ui, text_message("Private: failed")));
  assert(pending.remember(UINT32_MAX, public_ui, text_message("Node: second")));
  assert(pending.remember(0, private_ui, text_message("Private: timeout")));
  assert(!pending.remember(1, private_ui, first_tx));
  first_tx = {};  // Pending previews own bytes, not borrowed send buffers.
  assert(!public_ui.has_message() && !private_ui.has_message());
  assert(public_ui.sent_messages() == 0);
  assert(!pending.complete(999, true));  // Room/unknown completions do not affect channel UI.
  assert(pending.complete(8, false) && !private_ui.has_message());
  public_ui.received(text_message("Peer: esphome-ping"));
  assert(std::strcmp(public_ui.last_direction(), "RX") == 0);
  assert(pending.complete(7, true));
  assert(std::strcmp(public_ui.last_text(), "Node: esphome-pong") == 0);
  assert(std::strcmp(public_ui.last_direction(), "TX") == 0);
  assert(public_ui.received_messages() == 1 && public_ui.sent_messages() == 1);
  assert(!pending.complete(7, true) && public_ui.sent_messages() == 1);
  assert(pending.complete(UINT32_MAX, true));
  assert(public_ui.sent_messages() == 2 && std::strcmp(public_ui.last_text(), "Node: second") == 0);
  assert(pending.complete(0, false) && private_ui.sent_messages() == 0);
  // Freed entries are reusable; only the intended channel gets its TX preview.
  assert(pending.remember(1, private_ui, text_message("Private: esphome-pong")));
  assert(pending.complete(1, true));
  assert(std::strcmp(private_ui.last_text(), "Private: esphome-pong") == 0);
  assert(private_ui.sent_messages() == 1 && private_ui.received_messages() == 0);
  private_ui.received(text_message("Peer: later message"));
  assert(std::strcmp(private_ui.last_direction(), "RX") == 0);
  assert(std::strcmp(public_ui.last_text(), "Node: second") == 0);
  std::cout << "PASS: isolated RX/TX state, queued/failure/stale completion handling, owned "
               "buffers, token wrap, raw text and reset\n";
}
