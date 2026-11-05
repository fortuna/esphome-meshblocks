# Library status

Updated **2026-11-05**. ESPHome is the selected implementation approach.
[Design](docs/design.md) defines supported boundaries;
[manual](docs/esphome-manual.md) defines consumer configuration/actions.

## Hosted review branch and clone-free setup — 2026-11-05

The standalone repository is hosted privately at `fortuna/esphome-meshblocks`,
with draft PR #1 from `review/initial-import` to an empty `main` baseline.
The examples, manual and README import that review branch through ESPHome Git
sources; no library clone is required. The README calls out implemented features
and includes a complete PRG-to-channel beacon. Private Git access, mutable-branch
refresh and optional immutable pinning are documented. No release is claimed.

The external prototype now consumes the hosted branch and compiles successfully
without a sibling component path. All schema cases pass; the three maintained
examples and README beacon validate and compile. All three hosted package imports
and hosted components validate from an isolated application directory. Default
local and immutable Git consumer checks pass. See LOG for commands/evidence.
No flash or new RF observation is part of this change.

## Upstream SX126x PR source — 2026-11-05

The three examples, radio fixture, isolated consumer and usage snippets now use
`github://pr#22197` instead of the fork's immutable commit. Only `sx126x` is
imported; removing `refresh: never` enables ESPHome's default one-day refresh.
The open upstream PR head checked today is
`75a730821a9c5ea0efc3c5a8828e4687684d1311`, matching the previous pin.
All three examples and both isolated local/Git consumers validate using the PR
source. Documentation, guidance and provenance record the moving dependency.
No firmware rebuild, flash or new hardware observation.

## Compact external component imports — 2026-11-05

The pinned SX126x source uses `github://fortuna/esphome@<commit>`
shorthand. Revision `75a730821a9c5ea0efc3c5a8828e4687684d1311`,
the explicit component list and `refresh: never` are preserved.
All three examples and isolated local/Git consumers validate.
No firmware rebuild, flash or new hardware observation.

## Compact board imports — 2026-11-05

Board imports use the compact `github://fortuna/ESPHomeCollection/...@<commit>`
notation, preserving revision `71d63352dcb09a396d1cbfe2dc28c3210d8d436e`.
The shorthand uses ESPHome's default one-day cache refresh instead of explicit
`refresh: never`; the dependency remains pinned. All 211 schema checks pass; all three public
examples and isolated local/Git consumers validate. Maintained docs and test inputs use the
same shorthand. No firmware rebuild, flash or new RF observation.

## Send-only channel keys — 2026-11-04

Channel subscriptions now use `key` for hashtags, base64 keys, or byte lists;
`hashtag` is removed. `send_channel_message` accepts a `channel_key` in place
of `channel_id`, with no subscription. Direct-key sends are unscoped and use
the shared scheduler without an observable subscription preview. Existing
channel IDs retain their configured scopes and previews. Host tests, all example
firmware builds, the isolated Git consumer including a zero-subscription build,
and prototype host/schema tests and firmware build passed. No flash or new RF
observation was performed.

## Implemented

- Independent rooms, static native identities/passwords, private/public policy,
  durable keys/history and state-preserving legacy NVS loading.
- Client identities, private-message receipts/timeouts, signed chat adverts, local
  delivery and receive events from explicitly configured remote peers. The two-client example
  embeds public vanity demo keys; logs show events without key material. Room membership remains deferred.
- Shared-key channels, receive events, scopes and destination-specific typed send
  actions. Names on channel messages remain unverified.
- One bounded radio scheduler and full-identity/ACK ownership across services.
- ESP32/ESP-IDF/SX126x adapters with an independently pinned radio driver and explicit time
  source. Repeating, T096 and companion firmware are not implemented.

The obsolete local `firmware/` directory has also been removed. Application
firmware and deployment inputs belong to the external prototype; public examples
remain under `examples/`.

## No bundled packages — 2026-11-04

The packages directory is removed. Examples import the pinned ESPHomeCollection
board and SX126x driver directly. Internal fixtures declare their own dependencies
and reference wiring. No public board/radio wrapper remains.

All 202 schema cases pass, including the headless wiring and RF overrides. All
three examples validate and compile; isolated local and Git consumers validate
with only components/notices. No flash or new RF observation was performed.

## Configurable example RF profiles — 2026-11-04

All three examples expose `meshcore_frequency`, `meshcore_bandwidth`,
`meshcore_sf`, `meshcore_cr` and `meshcore_pa_power` substitutions. NYC defaults
are explained in the substitutions block; SF and power are numeric. Preamble
length is calculated as 32 symbols through SF8, 16 above it. Defaults are unchanged.

The 202-case schema suite passes; nine focused profile cases also pass against
the final five-parameter configuration, checking default values and package
overrides at SF8/SF9. All three actual example configurations compile with isolated
build credentials. The external prototype's 11 schema cases and actual firmware
build also pass. No flash or new RF observation.

## Component-only consumers — 2026-11-04

Removed `packages/radio/meshcore-lora.yaml`. Applications select their board and
radio driver independently and declare MeshCore framing explicitly. The prototype
imports ESPHomeCollection directly and consumes only MeshCore's component source.
Internal regression fixtures own their RF/framing setup; public usage docs no
longer reference the deleted package.

**193 library schema cases pass.** The isolated local and Git consumers validate,
and the Git consumer builds from a dependency containing only components and
notices—no packages, scripts or library test fixtures. The external prototype
also builds with its own cache, with unchanged resolved settings/dependency pins.
No flash or new RF observation was performed for this change.

## External consumer boundary

The reusable tree contains components, examples, supported docs and
tests. Device-specific firmware, UI/assets, USB/host provisioning tools, manual clock,
power experiments and research have moved to a separate external prototype.
They are not dependencies of this repository. Three basic flashable examples remain
public: a static three-room server, a channel bot using `sender_name` without a
client, and two clients exchanging a PRG-triggered local ping/pong. The latter
needs no secrets file: public demo keys start with `a11ce` (Alice) and `b0b` (Bob). Their actual YAML is covered by schema/build checks; they do not
import prototype adapters. The old root proposal manifests remain deleted. The hosted review branch is available; no release is claimed.

UI page state and USB command parsing no longer live in MeshCoreComponent.
Consumers use bounded resource observations and typed legacy management operations.
Channel observations retain raw text; consumers own text normalization/rendering.
Existing storage formats, namespaces, keys, radio ownership and queue semantics
are preserved. See [design](docs/design.md#room-storage-and-configuration).

## Verification

- **193 library schema cases** pass after removing application policy cases.
- **Nine sanitizer host executables** pass, covering clocks, channel state/codec,
  scheduling, radio adapter, rooms, runtime isolation, persistence and clients.
  Client coverage includes deferred local ping/pong without RF, full-key prefix
  collisions, configured-peer RX, retry/replay suppression and queue backpressure.
  Loopback requires explicit receive keys; it cannot discover a sender from the
  local registry. Send actions take private/public keys and may create a bounded
  sender service without a declared client handle.
- **Isolated local and pinned Git consumers** validate; the Git consumer compiles
  with only distributable components/notices and no prototype components.
- All three public V3 examples validate and compile with temporary generated inputs.
  Three rooms: 72,331 bytes linker RAM / 402,851 bytes flash; channel bot:
  64,491 / 338,295 bytes; two clients: 68,475 / 402,107 bytes. These are build totals, not heap/stack or RF measurements.
  Native demo-key derivation/prefix checks, a client build without secrets, key
  generation, private file permissions, no-overwrite/symlink behavior and secret
  output redaction pass. The examples require no prototype files.
- The external V3 application compiles; full library C++ semantic coverage is
  **54/54 owned files**, and the checker regression tests pass.

The moved prototype separately owns UI/console/power and provisioning checks.
The external V3 consumer was subsequently flashed on **2026-11-04** (config hash
`0xeb05548a`). USB pre/post checks retained all three signed identities and 8/8/8
post counts; rooms/radio were ready, time sync succeeded and sleep activity was
observed. This is consumer deployment evidence, not a new stock-peer RF exchange
or client-app acceptance. Earlier
V3 room/channel hardware observations remain historical evidence in the workspace
prototype; new client identities still need stock-peer RF acceptance.

## Remaining work

- Complete review and publish a stable revision; verify anonymous access if the
  currently private repository is made public.
- Complete physical OLED/PRG and fresh stock-peer checks for the flashed external
  application; USB identity/history/readiness and sleep checks passed.
- Verify physical PRG/event logs and remote client send/receive/receipts on actual peers and shared load.
- Measure device heap/stack/airtime limits before raising capacity expectations.
- Define future repeater/management/runtime configuration contracts separately;
  exploratory application behavior is not part of the published library API.
