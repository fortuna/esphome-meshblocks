# Library change log

Built, flashed and observed are separate states. Earlier full experiment/deployment
history was preserved in the workspace's external prototype under
`prototype/docs/history/LOG.md` during the 2026-11-04 extraction. This file now
tracks the self-contained reusable project. No historical device observation is
being claimed for a new build.

## 2026-11-06 — Room post automations and signal metadata

Rooms gain `on_post`, fired after a member post is stored (after the packet's
receive returns, so automations may post or send), with an owning `post` holding
the text and the carrying packet's RSSI/SNR; the author is not exposed.
`meshcore.add_room_post` stores a post under the room's identity. Channel
messages now carry `rssi`/`snr`. Callbacks are registered at composition time only.

Validation: host tests, config tests (on_post reply, 151-byte text and unknown
room rejected), C++ style and the external consumer compile pass. Not flashed by
the library.

## 2026-11-06 — Names use MeshCore's 31-byte limit

Room, client and channel sender names now accept up to 31 printable ASCII
characters, MeshCore v1.18's limit: advert app data is 32 bytes (one flags byte,
then the name when no location is sent), and stock firmware stores names in
`char[32]`. The former 24-character cap had no protocol basis. One
`MAX_NAME_SIZE` constant bounds validation, advert buffers and room names. Names
are not persisted, so stored room formats are unchanged.

Validation: host tests (including a 31-byte room advert and 32-byte rejection,
and 31/32-byte channel senders), config tests with 31/32-byte room and sender
names, C++ style checks and the external consumer compile pass. Not flashed by
the library; consumers observe it on their own devices.

## 2026-11-05 — Hosted review branch and clone-free consumers

Created `fortuna/esphome-meshblocks` privately with an empty `main` baseline and
initial standalone snapshot on `review/initial-import`, under draft PR #1.
The three example files now fetch Meshblocks from that branch. README feature
callouts and a complete button-to-channel beacon replace the sibling-checkout
setup; the manual and examples guide show direct sources and package imports.
Git access, branch refresh and optional immutable pins are explicit.

The example checker validates the README YAML alongside maintained examples and
supports `--published` for actual hosted sources/package imports. Default local
checks still exercise the worktree. The external prototype's source now uses Git;
its developer checks resolve ESPHome's cache rather than a sibling directory.
Validation: all schema/RF-profile cases pass. The three maintained examples and
README beacon validate and compile. `python tests/test_examples.py --published`
validates the actual hosted component source, all three remote package entry
points and the README beacon from a temporary application directory, with no
local component substitution. Demo key agreement, generation/file permissions,
no-overwrite/symlink guards and secret redaction pass. Isolated local and pinned
Git consumers also validate. The actual external prototype compiles with downloaded
components; its host, 11 composition, provisioning and format checks pass.
The beacon build uses 64,363 bytes linker RAM and 335,771 bytes flash. Hosted
validation resolved commit `c6072ed03624b266e11ba05c78225373d1a8f628`; subsequent
commits only extend package checks and record evidence, without component changes.
Firmware builds required normal SDK-cache access outside the workspace sandbox.
No flash, credential/storage-ID change, release or new RF evidence.

## Project renamed Meshblocks — 2026-11-05

The project is now Meshblocks ("MeshCore-compatible building blocks for ESPHome"),
and the checkout folder is `esphome-meshblocks/`. Several unrelated
`esphome-meshcore` repositories already exist on GitHub. The `meshcore:` YAML key,
the component names, C++ namespaces, NVS formats and storage IDs are unchanged, so
consumer configurations need only the new folder path. `scripts/cpp_style.py` now
replaces stale include symlinks left behind when a checkout moves. Config (211)
and isolated local/Git consumer checks, host C++ style and its self-test pass. No
firmware rebuild, flash or new hardware observation.

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

## 2026-11-04 — Remove the obsolete firmware directory

Deleted the remaining ignored firmware directory at the user's request. It
contained only the legacy secrets.yaml; its contents were not read or logged.
Application firmware and deployment inputs now live in the external prototype.
No maintained references to the old directory remain. No build inputs, prototype
files or device state changed; no rebuild or flash was performed.

## 2026-11-04 — Remove bundled board and radio packages

Deleted the entire packages directory. All three public examples now import the
Heltec V3 board directly from ESPHomeCollection and declare the SX126x external
component directly, retaining both immutable revisions. Internal Heltec fixtures
use the same direct board source; the headless fixture declares its ESP32 DevKit /
SX1262 reference wiring inline. The internal RF fixture owns its driver declaration.
Removed obsolete test-loader path rewriting and updated guidance, usage docs and
third-party provenance references. Protocol code and hardware settings are unchanged.

All 202 schema cases pass, including the headless wiring and RF overrides. All
three actual examples validate and compile with generated test credentials.
Isolated local and Git component-only consumers validate. No flash or new RF
observation; headless wiring remains a test target, not verified hardware support.

## 2026-11-04 — Parameterize example RF profiles

Added frequency, bandwidth, SF, CR and PA-power substitutions to all three public
examples, preserving the NYC default profile and 22 dBm power. Moved the network
explanation to the substitutions block. SF and power use numeric YAML values;
the preamble expression explicitly converts SF so CLI string overrides also work.
It selects 32 symbols through SF8 and 16 above it. Updated example usage, manual
and design documentation. The separate prototype uses the same parameters.

The 202-case schema suite passes. Nine focused profile cases pass against the
final five-parameter configuration, exercising each actual example's defaults and
package overrides at SF8/SF9, including frequency, bandwidth, CR and TX power.
All three example configurations compile with isolated credentials. Prototype
validation passes all 11 cases and its firmware compiles. No upload or hardware
observation was performed; build checks do not establish compatibility on a new RF
profile.

## 2026-11-04 — Remove the shared LoRa application package

Deleted packages/radio/meshcore-lora.yaml at the user's request. Updated its active
callers and usage docs to select board/driver dependencies separately and configure
framing explicitly. The prototype now imports ESPHomeCollection directly, with
only the external MeshCore component source coming from this repository.

Internal schema fixtures own their test framing. The isolated consumer regression
now copies only components and notices; neither local nor pinned Git consumption
requires library packages/scripts/tests. Both validate, and the Git consumer
compiles. All 193 library schema cases pass. Prototype config comparison confirms
unchanged settings/pins, and its firmware builds in its own cache. No upload or
new hardware observation; existing deployment evidence remains separate.

## 2026-11-04 — Explicit send keys and receive-key requirements

`meshcore.send_client_message` now takes `sender_private_key` and
`recipient_public_key`; the old sender_id/recipient_id fields are rejected.
The sender key is a static native literal or !secret; the destination accepts
literals/lambdas. `ClientMessage.sender_public_key()` replaces sender_id() so
reply destinations explicitly name the full public-key value they return.

Code generation collects private send keys across actions and creates one bounded
sender service per identity, reusing declared clients when present. A send action
requires no client-handle declaration, and a node can consist only of send actions.
Repeated keys share pending-send/ACK state rather than creating competing identities.
Room-key reuse is rejected. All sender services are composed before receive
callbacks, including forward references. These inferred services do not learn peers.

Loopback remains for the one-device Alice/Bob example, but its decoder no longer
receives the sender key from the registry. It uses the same explicitly configured
receive-peer matching as radio ingress. The example lists Alice's and Bob's public
keys as each other's receive peers. An unknown/ambiguous peer fails local delivery;
no automatic peer insertion or discovery occurs. Local completion still reports
DELIVERED_LOCALLY and consumes no radio airtime.

Validation: 193 schema cases and all nine sanitizer host executables pass,
including loopback failure without the sender's configured public key. All three
public examples compile. The isolated local/Git consumer validates and compiles
key-only send actions with no declared client; two actions using the same key
produce exactly one compiled sender service. The native vanity-key and no-secrets
example checks still pass. No firmware was flashed; physical/RF acceptance remains
separate from these checks.

## 2026-11-04 — Public vanity identities in the client example

Embedded native private/public demo pairs directly in the two-client YAML:
Alice starts with `a11ce855` and Bob with `b0b593ef`. The button uses Bob's public
key substitution; the reply still targets the received sender. This example now
builds without secrets.yaml. The keys are explicitly public shared demo credentials;
room/channel examples retain generated private inputs. The helper now generates
only three room identities plus passwords and the channel key.

Client private keys accept literal native hex as well as !secret, preserving
format/clamp checks and secret redaction. Room keys still require !secret.
Startup and receive logs no longer print keys; client names and message/delivery
events remain. Updated the manual, example instructions and capability status.

Both pairs were found by searching Ed25519 seed candidates and checked independently
with the vendored native-key derivation used by firmware. Example tests verify exact
public/private agreement and prefixes, then compile the two-client YAML after removing
the secrets file. All three example builds, 188 schema cases and isolated local/Git
consumer validation pass. The two-client build uses 68,475 bytes linker RAM and
401,723 bytes flash. No firmware was flashed.

## 2026-11-04 — Sender names, local clients and event logs

Channel sends now require `sender_name`, with no client identity dependency. Removed
the channel client's convenience overload and migrated public examples, regression
fixtures, external-consumer test and prototype channel actions. Private sends retain
`sender_id` and full-key `recipient_id`.

Added a third public example: two static clients on one V3. PRG sends Alice's ping
to Bob; Bob responds to the event's full sender key and Alice logs completion.
NodeRuntime borrows a bounded encrypted local queue (one slot per client), dispatches
one frame per tick, and completes as DELIVERED_LOCALLY without RF/ACK reservations.
Self, room, unregistered-sender and full-queue cases reject admission. Tests exercise
nonrecursive callbacks, exact full-key routing even with prefix collisions, and
independent runtimes.

Clients optionally accept up to eight explicit remote peer public keys. All-candidate
short-MAC checking precedes mutation; normal-text RF receives admit a receipt before
replay state/callbacks. Retries get receipts without duplicate application replies;
older timestamps and conflicting same-timestamp text are dropped. Remote peers,
replay state and callbacks are bounded; replay state resets at reboot. No dynamic
contact discovery, learned routes, room membership or companion protocol was added.
Pinned upstream normal/extended retry encoding was traced and recorded in design.

Startup prints client IDs/names/full public keys, never private keys. Logs distinguish
queued, physical TX, receipts/timeouts, local delivery, receive body/source, failures
and advertisement admission. Received control characters are sanitized. The example
logs PRG, Bob's reply decision and Alice's completed exchange. Its README documents
how to configure another device's public key to receive and answer radio pings.

Validation: 188 schema cases, nine sanitizer host executables, three actual example
firmware builds, exclusive secret generation/redaction, and isolated local/Git
consumer loading/build pass. The prototype also compiles with the renamed channel
field. Full library semantic checks cover 54/54 owned files. These changes were not
flashed; physical button/log and stock-peer RF checks remain pending. ClientService
is 1,960 bytes in the host ABI; embedded linker totals are in STATUS and do not
measure heap/stack/airtime.

## 2026-11-04 — External consumer deployment

The user requested flashing the external V3 prototype. The exact consumer config
compiled and uploaded successfully with hash verification (`0xeb05548a`). Device
and compiled partition tables matched, preserving NVS. USB pre/post checks verified
unchanged signed room identities, names/policies and 8/8/8 retained-post counts.
Rooms/radio were ready with zero failures, UTC synchronization succeeded, and
reported light-sleep counts increased. Physical UI and fresh stock-peer RF checks
remain pending. Detailed private deployment evidence and logs belong to the consumer.

## 2026-11-04 — Restore supported flashable examples

- Clarified the extraction boundary: basic flashable examples belong in the
  reusable project; device-specific/exploratory adapters remain in the external
  prototype. Restored the three-room server and bot under examples/, removing
  duplicate internal copies so schema/build checks exercise the public YAML.
- The bot now declares a static client identity, uses sender_id for both channel
  pong paths, advertises that chat identity, and sends a private greeting to a
  configured full recipient public key on PRG. No private send happens at boot
  or in response to channel traffic. Private receive/room membership remain absent.
- Both examples use standard ESPHome time inputs that start unset, with documented
  ordering/timestamp estimates and reboot limitations; neither imports a fixed
  test epoch, UI, USB console, manual-clock component or prototype package.
- Added a small example-only generator for private native keys/passwords/channel
  keys. It creates new mode-0600 files exclusively, rejects replacement/symlinks,
  and leaves the remote bot recipient blank until explicitly selected.
- **Validation:** 184 schema cases pass, including missing client key/recipient
  and blank-recipient rejection. Both public examples validate and compile from
  a temporary consumer directory using generated inputs; generation/redaction
  checks pass. Three-room build: 72,315 bytes linker RAM / 401,511 bytes flash;
  bot: 64,907 / 404,203 bytes. No upload or new RF/client observation.

## 2026-11-04 — Extract the device application

- Removed application firmware, display assets/packages, serial console, manual
  clock, radio-probe/power experiments and host provisioning scripts from the
  reusable project. Moved their code/tests and dated research into the external
  prototype. Old starter examples became headless internal regression fixtures.
- Extracted page selection and display normalization into the consumer UI. Channel
  observations retain raw text and successful-TX semantics. Extracted UART room
  grammar into the consumer console, using narrow typed node operations; kept
  legacy NVS formats, identity checks and static provisioning semantics intact.
- Retained only reusable hardware/radio packages, explicit typed service/actions
  and portable/runtime adapters. Rewrote active design/manual/README around local
  and pinned Git consumers. The library has no parent/prototype dependency.
- Added an isolated consumer test: copies only distributable inputs, checks a
  relocated local checkout and a temporary pinned Git repository, and compiles the
  Git consumer. Nested board/radio packages and component loading work independently.
  This is layout/build verification, not hosted publication.
- **Validation:** 181 library schema cases; nine sanitizer host executables;
  isolated local/Git config checks and Git firmware build; external V3 firmware
  build; C++ semantic checks with 54/54 library files; checker regression tests.
  The application separately checks its UI/console/power/provisioning contracts.
  No upload or new hardware/RF observation.

## 2026-11-04 — Static clients and typed actions

Commit `4bd4030` introduced static client identities, outbound private/channel
messages and signed chat adverts. Full-key uniqueness, shared ACK admission,
30-second post-TX receipts and bounded state were host/build checked. The API uses
`send_channel_message` with `channel_id` and either `name` or `sender_id`, and
`send_client_message` with `sender_id` and full public key `recipient_id`.
Advertisements use `room_id`/`client_id`. Typed room handles retain optional
`storage_id`; Public uses `public_room` with storage `public`. The then-combined
project passed 191 schema cases, eleven reported host suites and V3 builds;
no client RF/flash acceptance was claimed.

Commit `88df77e` preceded that work with YAML-sized channel/TX capacities and ACL
documentation. Earlier room/channel protocol, storage and hardware evidence is
preserved in the archived prototype records and existing regression tests.
