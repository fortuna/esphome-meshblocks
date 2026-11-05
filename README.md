# Meshblocks

**Bring MeshCore messaging to your ESPHome devices.**

Turn a button press into a LoRa message, build a channel bot, or host several
independent room servers on one radio. Meshblocks connects ESPHome automations
to MeshCore-compatible rooms, channels and clients through YAML.

## Features

- **Independent room servers.** Give each room its own identity, private/public
  access policy, member sessions and signed advertisements.
- **History that survives a reboot.** Room identities and message history persist
  on the device; stable storage IDs preserve them across ordinary firmware updates.
- **Channel automations.** React to incoming messages and send templated replies
  on shared-key channels. Send-only actions don't need a receive subscription.
- **Encrypted client messages.** Send private messages, receive from configured
  peers, advertise CHAT identities and observe TX, receipts and timeouts. Clients
  on the same device can exchange messages through the local queue without RF.
- **One radio, several services.** Rooms, channels and clients share one bounded
  scheduler and transmit queue, with full-identity ownership and collision checks.
- **Composable ESPHome building blocks.** Compose typed actions with buttons, sensors and
  automations. Fetch components directly from Git; normal mesh operation needs no
  Wi-Fi, Home Assistant, phone or deployment controller.

## Composable by design

A room server, a sensor beacon and a channel bot can live in the same ESPHome
configuration. Start with one service, then add rooms, channels or client
identities as your application grows.

- **Connect behavior in YAML.** Wire buttons, sensors and timers to message sends;
  use incoming messages to trigger scripts or update a display. Share configuration
  through ESPHome packages and customize existing components with `!extend`.
- **Keep control of your application.** Choose your board, radio profile and clock
  independently. Add UI, integrations and application policies through actions,
  events and narrow management APIs, without changing the protocol core.
- **Extend at clear boundaries.** Portable C++ services are separate from ESPHome
  and hardware adapters, with explicit dependencies for cryptography, storage and
  packet transport. New platform integrations can build on those boundaries;
  current hardware support is listed [below](#current-scope).
- **Grow within explicit limits.** Service capacities and the shared transmit queue
  are configured at build time. Adding services shares the device's memory and
  airtime budget; higher capacity needs measurement on the target hardware.

See the [design](docs/design.md) for ownership and extension boundaries, and the
[manual](docs/esphome-manual.md) for configuration, events and actions.

## Try it: a button that talks over the mesh

Save this as `beacon.yaml`. On a **Heltec WiFi LoRa 32 V3**, pressing PRG sends
`Checking in from the trail!` to `#testing`. ESPHome fetches the board, radio driver
and Meshblocks automatically. No library clone or identity-key generation is
needed for this channel example.

The RF values below are an explicit **NYC / MeshNY example**. Match frequency,
modulation, power and scope to your network and hardware before transmitting.

```yaml
esphome:
  name: trail-beacon
  min_version: 2026.11.0-dev

packages:
  board: github://fortuna/ESPHomeCollection/boards/heltec-wifi-lora-32-v3/board.yaml@71d63352dcb09a396d1cbfe2dc28c3210d8d436e

external_components:
  - source: github://pr#22197
    components: [sx126x]
  - source: github://fortuna/esphome-meshblocks@review/initial-import
    components: [meshcore, meshcore_core, meshcore_ed25519]

logger:
  baud_rate: 115200
  hardware_uart: UART0

# Starts unset; Meshblocks supplies within-boot ordering estimates.
# Use an independent RTC/SNTP source when you need accurate UTC.
time:
  - platform: template
    id: mesh_time
    timezone: UTC0
    lambda: return {};

sx126x:
  - id: !extend lora_radio
    modulation: LORA
    crc_enable: true
    payload_length: 0
    sync_value: [0x14, 0x24]
    rx_start: true
    frequency: 910.525MHz
    bandwidth: 62_5kHz
    spreading_factor: 7
    coding_rate: CR_4_5
    preamble_size: 32
    pa_power: 22

meshcore:
  id: mesh_node
  radio_id: lora_radio
  time_id: mesh_time
  path_hash_size: 2
  channels:
    - id: testing
      key: "#testing"
      scope: nyc

binary_sensor:
  - id: !extend boot_button
    on_press:
      - meshcore.send_channel_message:
          channel_id: testing
          sender_name: Trail Beacon
          message: Checking in from the trail!
          timestamp: !lambda return id(mesh_node).next_channel_timestamp();
```

Use the [validated ESPHome toolchain](docs/esphome-manual.md#toolchain), then run:

```sh
esphome config beacon.yaml
esphome compile beacon.yaml
```

Upload with ESPHome for your device, then watch `#testing` on a peer with matching
RF settings and channel key. Channel sender names are unverified labels. The
beacon needs Internet access for build-time downloads, then runs independently.

> **Review branch:** imports use `review/initial-import`, which can change and
> uses ESPHome's default one-day refresh. The repository is private during review;
> your build environment needs GitHub access. See [Git access and revision pinning](docs/esphome-manual.md#git-dependencies).
> The SX126x driver follows [ESPHome PR #22197](https://github.com/esphome/esphome/pull/22197).

## More starting points

| Example | What it does |
| --- | --- |
| [Three-room server](examples/heltec-v3-three-rooms.yaml) | Family and Building private rooms plus an open Public room, each with its own identity, retained history and signed advertisements |
| [Channel bot](examples/heltec-v3-ping-bot.yaml) | Answers `esphome-ping` on testing/private channels with a scoped reply |
| [Two clients](examples/heltec-v3-two-clients.yaml) | Alice sends Bob an encrypted ping on PRG; Bob replies through the local queue, with public demo keys and event logs |

Import these configurations directly as Git packages. For example, save this as
`clients.yaml` and compile it; no secrets file is needed:

```yaml
packages:
  demo: github://fortuna/esphome-meshblocks/examples/heltec-v3-two-clients.yaml@review/initial-import
```

[The examples guide](examples/README.md) covers RF overrides, private room inputs,
and expected radio/log behavior. Replace public demo credentials with unique
`!secret` values for real client deployments.

## Current scope

Meshblocks uses `meshcore:` in YAML and supplies the `meshcore`, `meshcore_core`
and `meshcore_ed25519` external components. Your application selects the board,
radio profile, time source and behavior. Current adapters require ESP32, ESP-IDF
and SX126x; Heltec V3 has earlier room/channel hardware evidence.

**Still ahead:** repeater and companion firmware, client room membership, automatic
contact discovery, and stock-peer RF acceptance for client messaging. Builds and
host tests establish different evidence from physical radio exchanges.

Private static credentials are embedded in firmware. Keep secrets/build artifacts
private and preserve room credentials, `storage_id` and NVS on updates.

## About the author

I'm [Vinicius (Vini) Fortuna](https://github.com/fortuna), co-creator of
[Outline](https://getoutline.org/). I build software around small, focused
components and clear interfaces so it can be reused, extended and scaled as needs
change. Meshblocks brings that approach to embedded mesh networking: compose the
services you need, keep application behavior separate, and make resource limits
explicit.

[Manual](docs/esphome-manual.md) · [Design](docs/design.md) ·
[Contributing](CONTRIBUTING.md) · [Status](STATUS.md) · [History](LOG.md)
