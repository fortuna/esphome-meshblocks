# Design: reusable MeshCore services on ESPHome

Updated 2026-11-05. This is the implemented contract, not a proposed device
application. Configuration/actions are documented in the [manual](esphome-manual.md).

## Distribution and dependency direction

The project owns `meshcore_core` portable C++ services, `meshcore` ESPHome adapters,
and vendored Ed25519. It distributes no board or radio packages. All public
configuration is static YAML. Three headless flashable examples are maintained in `examples/`: a
static three-room server, a channel bot using sender names, and two clients on one
device demonstrating a PRG-triggered ping/pong with signed CHAT adverts and event logs. Their standard template
clock starts unset; no fixed test epoch or prototype adapters are imported. The
example helper generates private room keys and channel inputs without runtime
provisioning. The two-client example embeds public demo key pairs with `a11ce` and
`b0b` public-key prefixes and needs no secrets file. Device-specific application firmware, UI, USB command grammar, provisioning tools,
clock providers and power policies are external consumers. The library imports
none of those components and has no dependency on its parent workspace.

Consumers declare MeshCore in `external_components`. Board packages and the
radio driver can be selected directly from their own upstream sources. Local component
paths resolve relative to the top-level consumer YAML; nested package includes
resolve from their containing files. Git consumers select component, board and driver sources independently. The examples
follow the open SX126x PR through `github://pr#22197` with the default daily cache
refresh; the core and board remain pinned. `tests/test_external_consumer.py` exercises a relocated checkout and a
temporary pinned Git repository containing only components and notices, without
library packages/scripts/tests. This verifies
layout/loading. The examples and external prototype now consume the hosted
`fortuna/esphome-meshblocks` review branch (`review/initial-import`) directly.
ESPHome fetches the source; users need no library checkout. This mutable branch
is private during review and requires Git access; immutable reviewed SHAs remain
the deployment pinning option.

## Ownership

One `MeshCoreComponent` owns a `NodeRuntime`, channel decoder, shared scheduler,
TX metadata and room runtimes. ESPHome supplies its SX126x and read-only time
source. One execution context owns all mutable protocol/service state. No global
identity swapping, per-room radio owner, per-room threads or unbounded queues.

| Boundary | Responsibility |
| --- | --- |
| `NodeRuntime` | Full identity registration, all-candidate dispatch and shared receipt admission |
| `RoomService` | Access policy, sessions, retained posts, history delivery and synchronization |
| `ClientService` | Local/RF private messages, bounded receive peers, adverts and one pending send |
| `RoomProtocol` / channel codec | MeshCore wire formats through injected crypto/identity ports |
| `RoomRepository` | Per-room durable history and commit/failure contracts |
| `RadioScheduler` | Bounded FIFO, pacing, one active transmission, completion/deadline handling |
| `RoomRuntime` | ESPHome composition, native identity, NVS and service activation |
| `Room`, `Client`, `Channel` | Typed ESPHome handles bound to their owning node |

Portable code lives in `components/meshcore_core`, namespace `meshcore_lib`,
without SDK, board, RadioLib or filesystem headers. It receives clocks, frames,
crypto, storage and transport dependencies explicitly. Host tests exercise two
independent instances. ESPHome state stays in adapters. Follow the
[C++ style policy](cpp-style.md): private service state, explicit `this->`, no
protected data members, and narrow behavior interfaces.

## Radio and execution

External board packages supply pins, buses, oscillator/RF-switch and hardware
definitions. This project ships no board packages. The Heltec examples import
ESPHomeCollection directly at a pinned revision; internal headless tests declare
their ESP32/SX1262 reference wiring inline.
Consumers select the asynchronous SX126x driver independently and configure
MeshCore framing explicitly: LoRa, CRC, variable-length packets, sync 0x14/0x24
and RX startup. There is no bundled meshcore-lora application package.
Consumers explicitly choose frequency/modulation profile and transmit power.
The examples expose frequency, bandwidth, SF, CR and transmit power as `meshcore_*` substitutions;
their preamble is derived at configuration time (32 symbols through SF8, 16 above).
These are application defaults, not implicit profile selection by the component.
All identities share the same radio, queue and airtime. Endpoints never forward
transit traffic. Raw SX126x send actions are rejected alongside the mesh owner.

Accepted frames are copied into bounded jobs. Queue admission, physical TX and
peer acknowledgment are distinct. The scheduler retains buffers until completion
or cancellation; failure cannot report success. Room/client builds use the current
100 ms minimum TX gap and four-second deadline. These are bench limits, not a
production airtime-utilization policy. Configured `tx_queue_size` includes active
TX. Capacity follows YAML and is compile checked.

The mesh loop advances services and scheduler in the owner context, then suspends
until the next deadline, capped at one second for health/time checks. RX/admission
wake it; active TX retains polling. An optional explicitly injected `IdlePower`
port supports consumer policies. The library implements no sleep/wake hardware
policy. Missing that port leaves normal mesh behavior intact.

## Identities, dispatch and isolation

Rooms and clients own independent full identities. Static inputs use native
64-byte expanded MeshCore private keys; prefixes are routing hints, never
identity or authorization. Config validation rejects duplicate configured scalars;
runtime registration rejects equal full public keys across roles, including
stored legacy rooms. Prefix collisions remain legal. Decryption checks all
candidates; ambiguous authentication/receipt ownership is rejected before mutation.

Each room has four volatile sessions and eight durable posts. A valid new member
at capacity may evict the least active session only after reply admission. Invalid
logins and failed admission do not evict. Sessions key by full peer identity and
retain their own paths, replay state, ACK and history cursors. Reboots clear
sessions, not keys/history. Shared ACK guards prevent room/client receipts from
being attributed to another pending message.

## Client messages

`clients[]` declares stable typed IDs, optional names (default ID) and native
keys. Client keys accept literals for public demos or `!secret` for private inputs. Clients send and receive ordinary encrypted text and signed CHAT advertisements.
Channel sends use an independent sender name. Room membership/login/posting,
automatic contact discovery, learned routes, automatic retries and companion phone
interfaces are not implemented.

Each client optionally configures up to eight full remote `peers` public keys.
Receive dispatch checks every matching peer and local identity before mutation;
ambiguous short-MAC matches are dropped. Normal text and stock retry attempt bits
are accepted. After receipt admission, `on_message` delivers an owning bounded
`ClientMessage` with body, full sender key, timestamp and local/RF flag. Each remote
peer retains its last timestamp/text in RAM: exact retries receive another receipt
without another callback, older timestamps or changed text at the same timestamp
are dropped. Reboot clears this replay state. Flood ingress returns an encrypted
PATH receipt; direct ingress receives a flooded raw ACK because no return route is
learned. This is not a complete companion contact database.

The protocol trace is the pinned upstream [BaseChatMesh.cpp](https://github.com/meshcore-dev/MeshCore/blob/81dbae592ffad40b4212bc0b37e82b38da08f4bc/src/helpers/BaseChatMesh.cpp)
(`onPeerDataRecv`, `composeMsgPacket`; checked 2026-11-04). It uses full contact
keys for decryption, normal-text flag/attempt bits and a four-byte receipt hash.
Our receipt payload uses the compatible four-byte form; it does not add upstream's
optional retry/random suffix.

DMs contain 1–160 UTF-8 bytes, normal text/attempt-zero flags and no name prefix.
They use existing ECDH and padded AES/HMAC, currently by ordinary unscoped flood.
Self and local room destinations are rejected. Registered local client destinations
use a separate composition-owned queue, one slot per client. It carries the encrypted
envelope and sender/recipient references for completion/routing, dispatching at most
one message per tick. Decryption receives only the envelope and checks the recipient's
explicit peer keys through the same decoder as radio ingress. The registry never
supplies an unknown sender key to the decoder; missing/ambiguous receive peers fail
the local send. Responses enqueue for a later tick, without recursive callbacks,
radio transmissions or RF receipt reservations. Successful local decode completes
the send as DELIVERED_LOCALLY before invoking the recipient callback. A full queue
or pending sender rejects admission. Remote peers must already know the sender's
full public key, for example by accepting its signed advertisement; receiving from
them here also requires their key in `peers`.
A room key is not a room login; room destinations are outside this client slice.

The send action takes a static `sender_private_key` and a templatable
`recipient_public_key`; no YAML client ID is used to obtain either key. Code generation
collects send keys across automations and creates one bounded service per identity,
reusing declared clients with the same private scalar. This also supports send-only
nodes without `clients` declarations. All services are configured before receive
automations are generated, so forward references cannot deadlock composition.
Implicit sender services have no receive peers or adverts unless the user explicitly
declares that client. Their memory is included in the normal client budget.

Each client admits one pending DM. State transitions are QUEUED → SENT on physical
success, then ACKNOWLEDGED on a matching raw ACK or authenticated PATH receipt.
TX failure is FAILED; the 30-second timer starts at actual TX and expires to
TIMED_OUT. Rejection leaves the preceding operation state unchanged. Timeouts
are not proof of non-delivery; receipts are not proof of phone UI display.

The shared recent-ACK guard has 256 entries and retains tokens for 180 seconds
since the last pending-state observation. Queued messages cannot accept receipts;
full guards reject admission. Raw ACKs remain unauthenticated 32-bit tokens and
encrypted receipts retain the inherited short MAC. No stronger guarantee is claimed.
Client keys/state have no additional NVS store; pending state resets at reboot.
Startup logs include client IDs and names; startup and receive logs omit key material.
Client logs report admission, physical TX, receipts, local delivery, failures and
received messages. Received text is bounded and control characters are sanitized;
applications choose their response policy through `on_message`.

## Channels and observations

A channel owns its shared key/scope and typed handle. Inbound packets match by
channel key, with duplicate suppression; outbound scope is not a receive ACL.
`on_message` receives an owning bounded `ChannelMessage`, safe across delayed
automations. Its sender prefix is unverified. Channel messages use only the shared
key; choosing a client name does not sign or authenticate authorship.

`send_channel_message` selects a subscribed `channel_id` or a send-only `channel_key` (hashtag, base64 binary key or byte list), and a templatable `sender_name`; no client identity is required. Send-only messages use unscoped flooding and do not create a receive subscription or observable channel preview.
Formatted `name: message` must fit 160 UTF-8 bytes; names have 1–24 printable ASCII
bytes without colon. Caller timestamps are used exactly, never silently clamped.
Templated values own bounded storage; overlength values are rejected, not truncated.

`ChannelState` retains bounded raw text, RX count and successful physical TX count.
`ChannelTxState` copies pending metadata and updates state only after the matching
successful completion. These observations contain no page selection, font folding
or rendering rules. Consumer UI performs its own text normalization. Node resource
lookups return bounded handles/read-only room runtimes; no mutable room service is
exposed to consumers. Borrowed observations are valid only while their owner lives.

## Room storage and configuration

`schema_version` is 1. Declaration `id` creates a typed ESPHome handle. Room
`storage_id` defaults to `id` and is a 1–12-character namespace key. Handle IDs
accept 1–24 lowercase letters/digits/underscores and ESPHome reserved-ID rules.
Duplicate storage IDs fail validation. `public` is reserved in C++, so a Public
room uses handle `public_room` and, when preserving existing storage,
`storage_id: public`. Names and list order do not change identity/storage.

`access: private` (default) requires a 12–15-byte secret password. Public rooms
reject a configured password and permit blank-password member access; protocol
crypto and session bounds remain. Static rooms use a `!secret` native private key.
On first boot, validated credentials initialize only absent state. Matching keys
are retained; a changed static password is durably applied and sessions cleared.
Conflicting identities, corrupt storage or failed writes stop startup without
silently replacing existing state. Commits are per room, not atomic across the list.

V1 seed credentials retain the 73-byte MCR1 record. Imported native keys use the
105-byte MCR2 record; a matching static key can adopt a V1 record without migration.
History and `mc_<storage_id>` namespaces are unchanged. Old firmware fails closed
on MCR2, so downgrades need explicit compatible recovery. Ordinary rebuilds and
updates must preserve secrets, storage IDs and the NVS partition.

Legacy rooms without static keys still load existing NVS state. Narrow typed
`provision_room`, `change_room_password`, `add_room_post` and
`export_room_advertisement` operations remain for existing consumer adapters.
They validate bounds/readiness and preserve collision/storage checks; password
changes require a drained TX queue. Static rooms reject legacy credential mutation.
USB parsing, UART ownership, command logging, backup formats and UI selection are
consumer responsibilities. No generic runtime provisioning protocol is promised.

## Time and advertisements

`meshcore.time_id` is a required read-only ESPHome `RealTimeClock`. The consumer
owns synchronization/freshness; it may use any compatible source. The source must
not call back into its MeshCore consumer. Portable services receive epoch seconds
and monotonic milliseconds explicitly. The forward-only protocol floor/holdover is
separate from source UTC. Restored state or build-time estimates do not mean fresh
UTC. `checkpoint_time()` persists effective room floors without setting the source;
a multi-room checkpoint can partially succeed and reports failure.

Room advertisements use the room's signed role/key/name and durable increasing
timestamp. Exporting an advertisement also consumes/checkpoints that timestamp.
Client advertisements use role CHAT and caller-supplied timestamp with no persistent
counter. Callers own timing/cadence. Direct is zero-hop; public scope names are
allowed only with flood mode. Sending an endpoint's advert never enables repeating.
`next_channel_timestamp()` offers within-boot increasing estimates; client reboot/
backward-time advertisement uniqueness is not guaranteed.

## Resources and evidence

Merged YAML fixes room/client/channel capacities and bounded queue sizes. The room
budget includes runtime, typed handle and registry storage; the default client
budget is 16384 bytes and includes objects, peer tables, registry pointers and local delivery slots. Budgets exclude
string/allocator overhead, stack, SDK state and other components. Codegen handle
backing storage contributes to linker RAM; RoomRuntime allocation occurs during
composition. Packet paths allocate no unbounded/per-message service state.

Host tests cover wire vectors, identity/receipt collisions, isolation, queue
backpressure, timestamps, failures, static initialization and state-preserving
updates. The pinned protocol baseline is MeshCore v1.18.0 at
`81dbae592ffad40b4212bc0b37e82b38da08f4bc`; notices live in third_party/.
Earlier V3 room/channel exchanges were observed with stock clients. The extracted
consumer layout and new client messaging require their own hardware acceptance.
See STATUS and LOG for actual current checks. Repeating, T096 adapters, runtime
reconciliation and companion firmware remain outside the implemented scope.
