# Using MeshCore in an ESPHome application

The library provides services and typed actions. Your application owns its board,
radio profile, clock source, automations and optional management/UI adapters.

## Toolchain

The validated core is ESPHome **2026.11.0-dev** at
`940f87056d692a6ee87c2aa3d526087842e20ee0`, using Python 3.13 and ESP-IDF.
From your application directory (no library checkout required):

```sh
python3.13 -m venv .venv
.venv/bin/python -m pip install "esphome @ git+https://github.com/esphome/esphome.git@940f87056d692a6ee87c2aa3d526087842e20ee0"
source .venv/bin/activate
python -m esphome version
```

The asynchronous SX126x driver is selected independently through
[ESPHome PR #22197](https://github.com/esphome/esphome/pull/22197), using
`github://pr#22197` in the examples' `external_components` declarations. This
follows the PR head with ESPHome's default one-day cache refresh. The head checked
on 2026-11-05 is `75a730821a9c5ea0efc3c5a8828e4687684d1311`; later PR updates
and stable core releases need separate validation. Build-time
downloads are required on a fresh cache; running mesh services need no Internet.

## Flashable starting points

The [three-room server](../examples/heltec-v3-three-rooms.yaml),
[channel bot](../examples/heltec-v3-ping-bot.yaml) and
[two clients](../examples/heltec-v3-two-clients.yaml) are complete Heltec V3 configurations.
See [the examples guide](../examples/README.md) for generating private build inputs,
choosing a peer, compiling and trying the room/channel/private-message behavior.
The examples use only the reusable library and standard ESPHome features. Their
clock starts unset and can be replaced with your own independent time source.

## External application layout

```text
my-node/
  node.yaml
  secrets.yaml           # private; do not commit
```

This is a complete headless channel configuration for a Heltec V3. The RF values
are an explicit NYC test profile, not inferred defaults for every deployment.
The [flashable examples](../examples/README.md) expose `meshcore_frequency`,
`meshcore_bandwidth`, `meshcore_sf`, `meshcore_cr` and `meshcore_pa_power` substitutions and calculate
the MeshCore preamble from SF. Match your network/hardware before transmitting. The template clock intentionally
starts unset; replace it with your application's independent UTC source.

```yaml
esphome:
  name: my-mesh-node
  min_version: 2026.11.0-dev
packages:
  board: github://fortuna/ESPHomeCollection/boards/heltec-wifi-lora-32-v3/board.yaml@71d63352dcb09a396d1cbfe2dc28c3210d8d436e
external_components:
  - source: github://pr#22197
    components: [sx126x]
  - source: github://fortuna/esphome-meshblocks@review/initial-import
    components: [meshcore, meshcore_core, meshcore_ed25519]
logger:
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
```

From your application directory, run `esphome config node.yaml` then
`esphome compile node.yaml`. Upload only the image for your device after preserving
its storage/credentials. These commands do not require the library's source tree
to be your working directory. No UI, console, USB clock or power component is
needed. Use `!extend` to override existing hardware/service list entries.

Local `external_components.source.path` is relative to the top-level YAML, even
when written inside a package. Keep that declaration in the consumer. Nested
`!include` paths in packages resolve from the including file. Font/icon files
belong to consumer UI packages; this library has none.

Board definitions are external to MeshCore; no board packages are bundled here.
Use a pinned upstream board package or declare your wiring in the application.

## Git dependencies

Meshblocks is hosted at [fortuna/esphome-meshblocks](https://github.com/fortuna/esphome-meshblocks).
Use the `review/initial-import` branch while the initial draft PR is under review:

```yaml
external_components:
  - source: github://fortuna/esphome-meshblocks@review/initial-import
    components: [meshcore, meshcore_core, meshcore_ed25519]
```

ESPHome downloads and caches the source automatically. Board and SX126x driver
sources remain independent. The review branch is mutable and follows ESPHome's
default one-day refresh; set `refresh: always` on the source to check for updates
on every validation/build. For reproducible deployments, replace the branch name
with a reviewed full commit SHA and set `refresh: never`.

The repository is currently private. The build machine needs a Git credential
helper with access to it; an authenticated GitHub CLI can configure one with
`gh auth setup-git`. Do not put tokens in YAML or source URLs. Once the repository
is public, anonymous downloads use the same source declaration.

You can also import a complete example as a package, with no local library files:

```yaml
packages:
  demo: github://fortuna/esphome-meshblocks/examples/heltec-v3-two-clients.yaml@review/initial-import
```

Example packages declare their own external components. Keep package and component
refs aligned when pinning an example. The isolated consumer test separately checks
relocated local sources and a temporary immutable Git dependency; hosted example
and prototype checks verify the real review branch.

## Static YAML rooms

Add `rooms` to the existing `meshcore` mapping, for example:

```yaml
meshcore:
  rooms:
    - id: personal
      name: Personal
      private_key: !secret personal_room_private_key
      password: !secret personal_room_password
    - id: public_room
      storage_id: public
      name: Public
      access: public
      private_key: !secret public_room_private_key
```

A native private key is **64 bytes / 128 hex characters**, the expanded MeshCore
format. It is not a 32-byte Ed25519 seed or a seed-plus-public-key concatenation.
Room `private_key` must use `!secret`; the public identity is derived. Private room
passwords use `!secret` and must have 12–15 printable non-space ASCII bytes.
`access: private` is the default. `access: public` rejects a password setting and
allows blank-password members within the same bounded session policy.

Provision keys through your own trusted consumer tooling or reuse the exact
existing native identity. New static rooms initialize at boot without a console.
Do not generate replacement keys for an existing device. Credentials are compiled
into private firmware despite `!secret` redaction in normal config output.
Keep both secrets and firmware private and preserve a secure backup.

Room `id` is a typed ESPHome handle. `storage_id` defaults to `id` and selects
`mc_<storage_id>` in NVS. Retain it explicitly when changing a handle. Names and
list order can change without rotating identity. `public` is a reserved C++ ID;
use handle `public_room` with `storage_id: public` for existing Public storage.
A conflicting stored identity stops startup instead of replacing it. Static
password changes apply on reboot; unchanged values avoid unnecessary writes.
Legacy rooms without `private_key` load their existing NVS credentials. Consumer
management adapters may use the narrow legacy operations; no USB console ships
with this library. V1 seed records remain readable; native-key MCR2 records require
compatible firmware on downgrade. Preserve the NVS partition on updates.

## Client identities and messages

```yaml
meshcore:
  clients:
    - id: house_bot
      name: House Bot
      private_key: !secret house_bot_client_private_key
```

Client keys use the same native format and accept literal hex or `!secret`. Use
`!secret` for private identities; the two-client example deliberately publishes
shared demo pairs. Room keys still require `!secret`. Name defaults to the ID. Each client must have
an independent full identity, including relative to rooms. This is a client
endpoint, not a phone-facing companion or a room member.

Use actions within an ESPHome script, interval or other automation:

```yaml
- meshcore.send_channel_message:
    channel_id: testing
    sender_name: House Bot
    message: Temperature is normal
    timestamp: !lambda return id(mesh_node).next_channel_timestamp();
- meshcore.send_client_message:
    sender_private_key: !secret house_bot_client_private_key
    recipient_public_key: !secret friend_public_key
    message: Hello
    timestamp: !lambda return id(mesh_node).next_channel_timestamp();
- meshcore.advertise_client:
    client_id: house_bot
    mode: flood
    scope: nyc
    timestamp: !lambda return id(mesh_node).next_channel_timestamp();
```

`sender_private_key` is a static native **64-byte private key / 128 hex characters**,
provided as a literal or `!secret`. `recipient_public_key` is the recipient's full
**32-byte public key / 64 hex characters** and accepts a lambda. `message` and
`timestamp` also accept lambdas. The old `sender_id` and `recipient_id` action
fields are rejected. No client handle or separate client declaration is required
to send: code generation creates one bounded sender service per key, reusing an
explicitly configured client with the same identity when present. Repeated actions
share that identity's pending-send/receipt state. Sender keys are static build
inputs; runtime private-key lambdas are not supported. An action cannot reuse a
room's identity. A node may consist solely of key-based send actions.

Remote messages use ordinary unscoped flood and require peers to know/accept the
sender. A registered local client receives through a bounded loopback queue, without
RF. Supply the destination's full public key in either case. Loopback uses the same
configured-peer decryption as radio ingress: it never obtains an unknown sender's
key from the local registry. Unknown or ambiguous sender keys fail local delivery.
Self and local room destinations are rejected. Room login/posting, route learning
and automatic retries are deferred.

To receive from another device, configure its full public key in the destination
client's `peers` list (at most eight). Loopback requires these explicit keys too.
Packets carry only a sender-key prefix; no automatic key discovery is implemented. Add an
`on_message` automation to a client; `message.body()` is a view of its owning text,
`message.sender_public_key()` returns an owning full hex public key suitable for a reply,
`message.timestamp` is the sender's timestamp, and `message.local` identifies local
delivery. The [two-client example](../examples/heltec-v3-two-clients.yaml) shows a
complete ping/pong. Unknown remote senders are ignored; no automatic contact
learning is implemented. Repeated radio messages get receipts but no repeated
callback. Older timestamps and different text at the same timestamp are dropped;
per-peer replay state is volatile.

Startup logs print each client's ID and name. Logs also report queued sends, local
delivery, physical radio TX, receipts, failures/timeouts and received text, without
printing keys. The two-client example lists both public/private demo pairs in YAML. Advertisements still need an explicit
automation; the example schedules nearby CHAT adverts for both clients.

Private text accepts 1–160 UTF-8 bytes. One DM may be pending per client. C++
`send_client_message()` returns queue admission; `send_state()` distinguishes
QUEUED, SENT, ACKNOWLEDGED, DELIVERED_LOCALLY, FAILED and TIMED_OUT. The 30-second receipt timeout
starts after physical TX; no state proves phone UI display. Reboot clears pending
state. Raw ACKs are not authenticated.

## Channel events and send actions

Channels choose `public: true` or `key`, which accepts a hashtag beginning with `#` or a 16- or 32-byte key encoded as base64 or a YAML byte list. Hashtags derive a 16-byte key from SHA-256. Use `!secret` for private keys. Optional `scope` is a bare
public region name such as `nyc`, or null/omitted for ordinary flood. It affects
outbound routing, not membership or inbound authorization.

`meshcore.send_channel_message` requires exactly one of `channel_id` or `channel_key`, plus templatable `sender_name`,
`message` and nonzero uint32 `timestamp`. It requires no configured client.
A `channel_key` accepts the same hashtag, base64 key or byte list as `channels.key` and sends unscoped without subscribing or retaining a channel preview. Use `channel_id` for a configured channel's scope and preview. The former `hashtag`, `name` and `sender_id` fields are rejected; migrate either to
`sender_name`. Formatted `name: message` must fit 160 UTF-8 bytes; names are 1–31
printable ASCII characters without colon. The label is unverified channel text. Timestamp values are
used exactly; the helper supplies within-boot estimates when source UTC is unset.

For a send without a subscription:

```yaml
- meshcore.send_channel_message:
    channel_key: "#testing"  # Or !secret holding a base64 key
    sender_name: Bot
    message: Hello
    timestamp: !lambda return id(mesh_node).next_channel_timestamp();
```

Each channel may declare `on_message`. It receives an owning bounded `message`
with `sender()` and `body()` views, timestamp/path/scope metadata. Copy views if
retaining them beyond their owner. Duplicate/self-echo filtering occurs first.
The library supplies events and send operations; ping responses, cooldowns and
application message rules belong in your YAML.

Messages also carry `rssi` and `snr`: the signal of the received packet's last hop.

## Room posts

Each room may declare `on_post`. It runs after the room has stored a member's post,
and receives an owning `post` with `body()`, `author` (the member's full public
key), `rssi` and `snr` (of the packet's last hop). MeshCore room posts carry no
author name. `meshcore.add_room_post` (`room_id`, templatable
`text` of 1-150 UTF-8 bytes) stores a post under the room's own identity and
delivers it to members; a rejection is logged, not retried.

```yaml
rooms:
  - id: bot
    name: Ping Bot
    access: public
    on_post:
      - if:
          condition:
            lambda: return post.body() == "ping";
          then:
            - meshcore.add_room_post:
                room_id: bot
                text: !lambda |-
                  char text[48];
                  snprintf(text, sizeof(text), "pong (RSSI %.1f, SNR %.1f)", post.rssi, post.snr);
                  return meshcore_lib::ChannelText(text);
```

## Advertisements and time

| Action | Reference fields |
| --- | --- |
| `meshcore.advertise_room` | `room_id` |
| `meshcore.advertise_client` | `client_id`, required `timestamp` |
| `meshcore.send_channel_message` | `channel_id`, `sender_name` |
| `meshcore.send_client_message` | `sender_private_key`, `recipient_public_key` |

Advertise actions default to `mode: direct` (zero-hop). Optional `scope` requires
`mode: flood`; omission is unscoped. Room timestamps use the room's durable
ordering floor. Client timestamps are caller-supplied and volatile. Schedule all
announcements explicitly with native ESPHome automations; there is no implicit
boot/periodic advertisement or retry. Sending a flood advert never repeats transit
traffic. Each typed handle already knows its node; actions need no meshcore ID.

`meshcore.time_id` selects a read-only ESPHome clock. Your independent source owns
synchronization/freshness. Protocol floors/holdover prevent history moving backward,
but do not establish accurate UTC. `checkpoint_time()` persists effective floors;
failure can occur after earlier rooms succeeded. Client advert timestamps have
no reboot-persistent uniqueness guarantee.

The old `meshcore.send_message` and `meshcore.client_send_message` names are rejected.
Use the destination-specific names above. Generic action `id`, old room `room`,
client `client_id`/`recipient` send fields and client channel destinations are
rejected. Advertisements still use `client_id`.

## Limits and troubleshooting

Room/client/channel counts follow merged YAML. Each room has four sessions and
eight durable posts. `room_state_budget`, `client_state_budget` (default 16384 bytes)
and `tx_queue_size` bound fixed state/admission; they are not total/free RAM
estimates. All identities share the queue and radio. A successful enqueue does
not prove transmission or delivery. Budget/load acceptance is per device.

- Missing component: declare its external source from your top-level YAML; do not
  assume a package registers components. Check local paths from that directory.
- Startup fails: inspect identity conflicts, storage errors, duplicate resources,
  capability validation and memory budgets. Do not erase NVS to bypass a conflict.
- No peer delivery: verify RF profile, shared channel key, sender contact acceptance
  for private messages, timestamp policy and queue/TX results separately.
- Unknown action/field: use the names above and typed IDs, not historical examples.
- No clock/UI/USB commands: those are consumer adapters, not bundled services.

The adapters currently require ESP32/ESP-IDF and the pinned SX126x driver.
Repeater/companion firmware and T096 are not implemented. Earlier V3 room/channel
RF evidence does not automatically validate new client identities or a new image.
