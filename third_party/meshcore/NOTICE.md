# MeshCore protocol provenance

The channel codec and room protocol/service adapt wire behavior from MeshCore v1.18.0, commit
`81dbae592ffad40b4212bc0b37e82b38da08f4bc`, under the accompanying MIT license:

- [Packet.h and Packet.cpp](https://github.com/meshcore-dev/MeshCore/blob/81dbae592ffad40b4212bc0b37e82b38da08f4bc/src/Packet.cpp): header, path encoding, payload fingerprint.
- [Utils.cpp](https://github.com/meshcore-dev/MeshCore/blob/81dbae592ffad40b4212bc0b37e82b38da08f4bc/src/Utils.cpp): AES-128 ECB, zero padding, two-byte HMAC-SHA256 using a 32-byte padded secret.
- [Mesh.cpp](https://github.com/meshcore-dev/MeshCore/blob/81dbae592ffad40b4212bc0b37e82b38da08f4bc/src/Mesh.cpp): group payload envelope.
- [BaseChatMesh.cpp](https://github.com/meshcore-dev/MeshCore/blob/81dbae592ffad40b4212bc0b37e82b38da08f4bc/src/helpers/BaseChatMesh.cpp): channel hash, timestamp, text flags, sender prefix and text limits.
- [Companion protocol](https://github.com/meshcore-dev/MeshCore/blob/81dbae592ffad40b4212bc0b37e82b38da08f4bc/docs/companion_protocol.md): hashtag key derivation and Public key.
- [TransportKeyStore.cpp](https://github.com/meshcore-dev/MeshCore/blob/81dbae592ffad40b4212bc0b37e82b38da08f4bc/src/helpers/TransportKeyStore.cpp): named scope keys, packet-dependent transport HMAC, and reserved-code remapping.
- [Companion MyMesh.cpp](https://github.com/meshcore-dev/MeshCore/blob/81dbae592ffad40b4212bc0b37e82b38da08f4bc/examples/companion_radio/MyMesh.cpp): scoped send envelope and receive behavior.
- [Room MyMesh.cpp](https://github.com/meshcore-dev/MeshCore/blob/81dbae592ffad40b4212bc0b37e82b38da08f4bc/examples/simple_room_server/MyMesh.cpp): room login, permissions, post ACKs, keepalives, history synchronization, and the GET_STATUS request/tag plus 52-byte room `ServerStats` layout.
- [TxtDataHelpers.h](https://github.com/meshcore-dev/MeshCore/blob/81dbae592ffad40b4212bc0b37e82b38da08f4bc/src/helpers/TxtDataHelpers.h): text subtype wire values, especially `SIGNED_PLAIN=2` for room pushes. The stock BaseChatMesh receiver displays/ACKs that subtype; `CLI_DATA=1` is handled as command output without a message ACK.
- [Identity.cpp](https://github.com/meshcore-dev/MeshCore/blob/81dbae592ffad40b4212bc0b37e82b38da08f4bc/src/Identity.cpp): identity/signing/key-agreement behavior. Vendored primitives have a separate [Ed25519 notice](../ed25519/NOTICE.md).

Room advertisement routing was checked on 2026-11-04 at the same revision:
`Mesh.cpp::createAdvert` signs identity/timestamp/application data independently
of routing; `sendZeroHop` chooses direct routing with an empty path and
`sendFlood` chooses ordinary flooding with an empty path of the selected hash
width. `Packet.cpp::calculatePacketHash` excludes route/path, so the runtime
uses increasing per-room advertisement timestamps for close direct/flood sends.
The pinned `Mesh.cpp` and `Packet.cpp` sources are linked above. This source
trace and host vectors are separate from RF/stock-client acceptance.

Scoped room announcements were checked on 2026-11-04 at the same revision:
the room server's `sendSelfAdvertisement` calls `sendFloodScoped` for flood mode.
That method computes the packet transport code and passes a zero return code to
`Mesh::sendFlood`. The scoped route inserts both codes before the path while
retaining the signed advertisement payload. Our shared transport wrapper uses
payload type 4 for advertisements and type 5 for channel text; independent Python
HMAC/signature fixtures cover the advertisement form. See the linked room-server,
Mesh, Packet and TransportKeyStore sources above. Region scoping is a routing
hint; this addition does not implement incoming region discovery or reply policy.

Native private-key import/export was checked on 2026-11-03 at the same revision:
`LocalIdentity` derives its public key from a 64-byte expanded private key;
[CommonCLI.cpp](https://github.com/meshcore-dev/MeshCore/blob/81dbae592ffad40b4212bc0b37e82b38da08f4bc/src/helpers/CommonCLI.cpp)
exports/imports that form as hex. It is distinct from a 32-byte seed and from
seed-plus-public formats. The adapter reuses the vendored derivation, signing and
ECDH functions; schema/import checks enforce clamping, and runtime rejects reserved
public prefixes. The `private_key` field, legacy seed storage and native-key records are
documented in the [manual](../../docs/esphome-manual.md#static-yaml-rooms).

The extracted boundary uses bounded value types, validates lengths before
reading, explicit little-endian timestamps, and injected crypto primitives.
It does not import the radio-owning Mesh class or application globals. The channel codec accepts
V1 ordinary/scoped flood group text; direct routes and other payload versions/types
are not interpreted. Named public scopes select outbound flooding, not access.
Received transport codes are unauthenticated routing metadata; the channel
payload MAC does not cover them. Fingerprints exclude both paths and transport
codes, preserving duplicate suppression across routes/scopes.
It does not strengthen MeshCore's inherited short MAC or authenticate sender
names. Hashtag/Public keys are public, and message text should be treated as public.

Cryptography is performed by ESP-IDF's mbedTLS on the device and OpenSSL in host
tests. No AES, SHA, or HMAC primitive is implemented here. The mbedTLS adapter
targets the project's pinned ESPHome 2026.9.1 / ESP-IDF 5.5.5 baseline.

The room adapter additionally interprets anonymous login, peer text/request/path,
and ACK packets. It advertises with its own Ed25519 identity, separates full-key
sessions from prefix routing, and owns bounded history/sync state through narrow
ports. It has no transit forwarding, admin CLI, or durable ACL in this slice.
See [room scope and evidence](../../docs/room-test.md). Room payloads retain
upstream AES-ECB/short-MAC behavior; this does not claim stronger cryptographic
guarantees or end-to-end author signatures on relayed room posts.

The status-response compatibility trace was checked on 2026-11-01 against
MeshCore One v1.6.2 (`ecbc8bfddc0bd8449179746c08cf67be1a5f8ed6`). No MC1 source
code is included here; it is a client behavior reference. See
[join compatibility](../../docs/join-compatibility.md) for exact source links,
wire fields, unavailable measurement handling, and retest evidence.
