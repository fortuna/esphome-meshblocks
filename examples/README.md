# Flashable Heltec V3 examples

These are complete, headless ESPHome applications using only this project's
components and standard ESPHome features. They do not import the external
prototype, UI, USB console, manual clock or power experiments. Board definitions
come directly from ESPHomeCollection at the revision pinned in each example.
Each example also selects the pinned SX126x external component directly.

| Example | Behavior |
| --- | --- |
| [Three rooms](heltec-v3-three-rooms.yaml) | Family and Building private rooms, Public open room; independent static identities, retained history and scheduled signed room advertisements |
| [Channel bot](heltec-v3-ping-bot.yaml) | Replies to `esphome-ping` on testing/private channels using `sender_name`, without a client identity |
| [Two clients](heltec-v3-two-clients.yaml) | Alice sends Bob a private `ping` on PRG; Bob responds `pong`, with both clients on one device and event logs and embedded demo keys |

## Configure and build

Use the [pinned ESPHome toolchain](../docs/esphome-manual.md#toolchain). From the
library root, copy the desired example into `local/` (a sibling of `examples/`,
so its relative component paths remain correct):

```sh
mkdir -p local
cp examples/heltec-v3-three-rooms.yaml local/rooms.yaml
cp examples/heltec-v3-ping-bot.yaml local/bot.yaml
cp examples/heltec-v3-two-clients.yaml local/clients.yaml
python examples/generate_secrets.py local/secrets.yaml
esphome config local/rooms.yaml
esphome compile local/rooms.yaml
```

The helper requires Python's `cryptography` package. It creates three independent
native private/public key pairs, two room passwords and a private channel key in
a new mode-0600 file. It refuses to replace files or follow an existing symlink.
For an existing device, reuse its original keys/passwords/storage IDs and merge
any missing values manually; do not regenerate identities to adopt an example.
Secrets and firmware contain private material and must stay out of version control.

The bot only needs the shared private channel key configured on its channel peers.
The two-client example needs no `secrets.yaml` or key-generation step. Its YAML
includes matching native private/public key pairs: Alice's public key starts with
`a11ce`, Bob's with `b0b`. The button action explicitly supplies `sender_private_key: ${alice_private_key}`
and `recipient_public_key: ${bob_public_key}`. Bob's reply supplies his private key
and `message.sender_public_key()` as the destination. Each receiver explicitly
lists the other client's public key in `peers`.
These are public demo credentials shared by everyone; use unique `!secret` keys
for a real deployment.

```sh
esphome compile local/bot.yaml
esphome compile local/clients.yaml
```

After reviewing board, RF profile and existing-device state, use ESPHome's normal
upload/run workflow for the chosen YAML and device. These sources target the
Heltec WiFi LoRa 32 V3. Their explicit NYC settings are 910.525 MHz, BW62.5 kHz,
SF7, CR4/5, preamble32 and 22 dBm; change them for your actual network/hardware.
Set the five substitutions at the top of each example (or override them from a
consumer package):

```yaml
substitutions:
  meshcore_frequency: 910.525MHz
  meshcore_bandwidth: 62_5kHz
  meshcore_sf: 7
  meshcore_cr: CR_4_5
  meshcore_pa_power: 22  # dBm
```

The radio derives `preamble_size` using
`${32 if (meshcore_sf | int) <= 8 else 16}`: 32 symbols through SF8 and 16 above it.
Frequency includes its unit; bandwidth and coding rate use SX126x's enum spellings.
Channel/advertisement scopes remain separate network choices. To override from the
command line, for example: `esphome -s meshcore_sf 9 config local/clients.yaml`.

Building the examples does not upload them. None of these examples needs Wi-Fi or HA.

The clock is a standard template source returning unset time, not a fixed test
epoch. Rooms use their build/restored ordering floor; sends use the timestamp
helper's estimates. Replace this input with an independent RTC/SNTP provider for
accurate UTC. Client advertisement timestamps do not persist across reboot; a
stale timestamp can make peers ignore a new advert. An unset source is not fresh UTC.

## Try the rooms

Flash the rooms configuration with your own keys/passwords. New static rooms
initialize on boot; no USB provisioning step is needed. Nearby advertisements are
scheduled after startup, then every 240 minutes; Public also floods every 47 hours
with scope `nyc`. Accept the room contacts on a stock companion/app and join using
the appropriate password; Public uses blank-password access.

The example uses storage IDs `family`, `building`, `public`. If updating a device
that previously used `personal`, retain that namespace explicitly instead of
assuming `family` migrates it. Keep NVS and identity material on ordinary updates.

## Try the channel bot

Send exactly `esphome-ping` on `#testing` or the private channel. The bot attempts
`@[sender] esphome-pong` on that same channel, with independent ten-second cooldowns.
Both sends use `sender_name: ESPHome Ping Bot` and inherit the channel's scope.
No client identity is required; the label is unverified channel text.

## Try the two clients

For a quick start, only copy and build the self-contained client example:

```sh
mkdir -p local
cp examples/heltec-v3-two-clients.yaml local/clients.yaml
esphome compile local/clients.yaml
```

Flash `clients.yaml` and watch the UART logs at 115200 baud. Startup identifies the
clients by ID/name; keys are in the YAML rather than the logs. Press PRG. The expected sequence is:

```text
PRG pressed: Alice sending ping to Bob
Client alice: DM queued
Client alice: DM delivered locally
Client bob: RX local: ping
Bob received ping; replying to its sender
Client bob: DM queued
Client bob: DM delivered locally
Client alice: RX local: pong
Alice received pong: exchange complete
```

This exchanges encrypted messages through a bounded local queue, one delivery per
loop tick. Decryption checks the receiver's configured `peers`, as for radio ingress;
there is no automatic key discovery or sender-key lookup from the local registry.
Removing Alice's key from Bob's peers makes local delivery fail. Loopback does not
transmit the ping/pong on the radio or claim RF receipts.
Only Bob replies to `ping`; Alice observes `pong`, so responses do not loop.
Private sends allow one pending message per client; rejected calls are logged.

To send from another device, add its full public key to Bob's existing `peers` list
(as a hex literal or `!secret remote_public_key`), then rebuild.
Accept Bob's nearby CHAT advert on that device, or use his public key from the YAML in its
contact setup, and send `ping`. Bob logs `RX radio` and attempts a private `pong` to
that full sender key. Configure Alice's peers similarly if desired. Matching RF
settings and knowledge of both full identities are required; remote contacts are
not learned automatically. Client private keys are never printed. Radio sends log
TX and receipt/timeout separately; receipt does not prove phone UI display.

Builds and host tests are separate from physical PRG and stock-peer RF acceptance,
which remain pending for this client example. No room membership, UI, USB command
interface or phone companion protocol is included.
