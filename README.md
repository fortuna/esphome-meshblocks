# Meshblocks

MeshCore-compatible building blocks for ESPHome. Compose room servers, channels
and client identities into your own firmware, in YAML, alongside any other
ESPHome component. However many blocks a node has, it owns one radio and one
shared bounded transmit queue.

The project is named Meshblocks, but its YAML still uses MeshCore names: the
configuration key is `meshcore:` and the [external components](components/) are
`meshcore`, `meshcore_core` and `meshcore_ed25519`.

This project supplies the protocol/services and ESPHome adapters. Your application
supplies the board, radio profile, clock and automations. Three basic flashable
examples illustrate those choices; device-specific UI, USB management and power
experiments remain external.

## Flashable examples

- [Three-room server](examples/heltec-v3-three-rooms.yaml): independent static room
  identities, private/public access and scheduled advertisements.
- [Channel bot](examples/heltec-v3-ping-bot.yaml): channel pongs using `sender_name`.
- [Two clients](examples/heltec-v3-two-clients.yaml): PRG-triggered Alice/Bob ping/pong
  on one device, embedded vanity demo keys, signed client adverts and event logs.

All are complete Heltec V3 YAML configurations with no prototype dependency.
[Setup, private key generation, building and usage](examples/README.md).

## Use from another directory

With this checkout beside your application:

```yaml
# my-node/node.yaml
packages:
  board: github://fortuna/ESPHomeCollection/boards/heltec-wifi-lora-32-v3/board.yaml@71d63352dcb09a396d1cbfe2dc28c3210d8d436e
external_components:
  - source: github://pr#22197
    components: [sx126x]
  - source:
      type: local
      path: ../esphome-meshblocks/components
    components: [meshcore, meshcore_core, meshcore_ed25519]
```

The SX126x source follows [open ESPHome PR #22197](https://github.com/esphome/esphome/pull/22197)
with the default one-day cache refresh. Its head checked on 2026-11-05 is
`75a730821a9c5ea0efc3c5a8828e4687684d1311`; future PR updates need revalidation.

Add an explicit RF profile, ESPHome time source and `meshcore:` services as shown
in the [manual](docs/esphome-manual.md). Component paths resolve from your top-level
YAML; package `!include` paths resolve from their containing file. Importing a
package alone does not load this project's external components. Both relocated
local and pinned Git consumption are checked by [the consumer test](tests/test_external_consumer.py).
The repository is not published yet; no hosted URL or release is promised.

## Supported scope

- Independent rooms with private/public access, durable identity/history and
  signed advertisements. Configure native keys/passwords with `!secret`.
- Shared-key channels with receive events and `meshcore.send_channel_message`.
- Static client identities with `meshcore.send_client_message` and
  `meshcore.advertise_client`, local delivery and `on_message` for local/configured
  remote peers. Room membership is deferred; RF/app acceptance is pending.
- Typed `room_id`, `channel_id`, `client_id` advertisement references and
  `sender_private_key`/`recipient_public_key` client-message arguments; channel sends use `sender_name`.

Current adapters require ESP32, ESP-IDF and SX126x. The Heltec V3 has earlier
room/channel hardware evidence; builds alone do not qualify other boards.
Repeating and companion firmware are not implemented. Normal operation does not
require Wi-Fi, Home Assistant, a phone, USB or an Internet connection.

Private keys/passwords are embedded in firmware when supplied statically. Preserve
room `storage_id` and credentials on updates. See the manual for formats and limits.

[Manual](docs/esphome-manual.md) · [Design](docs/design.md) ·
[Contributing](CONTRIBUTING.md) · [Status](STATUS.md) · [History](LOG.md)
