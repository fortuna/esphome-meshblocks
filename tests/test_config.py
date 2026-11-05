"""Exercise schema boundaries with ESPHome's actual configuration validator."""
from pathlib import Path
import subprocess
import os
import tempfile
import hashlib

root = Path(__file__).resolve().parent.parent
fixtures = root / "tests/fixtures"


def fixture_source(name):
    # Cases use one relative-path spelling; execution expands it below.
    return (fixtures / name).read_text().replace(
        "path: ../../components", "path: ../components")


original = fixture_source("channel.yaml")
room = fixture_source("legacy-room.yaml")
def expanded_key_fixture(byte):
    expanded = bytearray(hashlib.sha512(bytes([byte]) * 32).digest())
    expanded[0] &= 248
    expanded[31] = (expanded[31] & 63) | 64
    return expanded.hex()


expanded_fixture = expanded_key_fixture(1)
static_room = room.replace("      name: personal", "      name: personal\n      private_key: !secret personal_room_private_key")
def channel_config(count):
    return original.replace('key: "#testing"', 'key: "#testing"' + ''.join(
        f'\n    - id: ch{i}\n      key: "#ch{i}"' for i in range(count - 1)))


client_config = original.replace("  channels:\n", """  clients:
    - id: automation_client
      private_key: !secret personal_room_private_key
    - id: second_client
      name: Second
      private_key: !secret building_room_private_key
  channels:
""")
client_action = """
interval:
  - interval: 10min
    then:
      - meshcore.send_channel_message:
          sender_name: Automation
          channel_id: testing
          message: Hello
          timestamp: !lambda return id(mesh_node).next_channel_timestamp();
"""
client_dm = client_action.replace("sender_name: Automation", "sender_private_key: !secret personal_room_private_key").replace("meshcore.send_channel_message:", "meshcore.send_client_message:").replace("channel_id: testing", 'recipient_public_key: "' + "12" * 32 + '"')
client_advert = client_action.replace("sender_name: Automation", "sender_id: automation_client").replace("meshcore.send_channel_message:", "meshcore.advertise_client:").replace("sender_id:", "client_id:").replace(
    "          channel_id: testing\n          message: Hello\n", "          mode: flood\n          scope: nyc\n")

cases = [
    ("key-only send without declared clients", original + client_dm, True),
    ("send-only node", original.replace('  channels:\n    - id: testing\n      key: "#testing"\n', '') + client_dm, True),
    ("empty node rejected", original.replace('  channels:\n    - id: testing\n      key: "#testing"\n', ''), False),
    ("send key cannot reuse room", static_room + client_dm, False),
    ("send private key is static", original + client_dm.replace("!secret personal_room_private_key", "!lambda return std::string();"), False),
    ("channel requires no client", original + client_action, True),
    ("channel rejects sender identity", original + client_action.replace("sender_name: Automation", "sender_id: automation_client"), False),
    ("channel rejects old name", original + client_action.replace("sender_name:", "name:"), False),
    ("client receive peers", client_config.replace("      name: Second", '      name: Second\n      peers: ["' + "12" * 32 + '"]'), True),
    ("client rejects short peer", client_config.replace("      name: Second", '      name: Second\n      peers: ["1234"]'), False),
    ("client bounds peer count", client_config.replace("      name: Second", '      name: Second\n      peers: [' + ', '.join('"' + f'{i:02x}' * 32 + '"' for i in range(1,10)) + ']'), False),
    ("client receive automation", client_config.replace("      name: Second", '      name: Second\n      on_message:\n        - logger.log: "Received DM"'), True),
    ("old channel action rejected", original + client_action.replace("meshcore.send_channel_message:", "meshcore.send_message:"), False),
    ("old client action rejected", client_config + client_dm.replace("meshcore.send_client_message:", "meshcore.client_send_message:"), False),
    ("channel sender and name conflict", client_config + client_action.replace("          message:", "          name: Explicit\n          message:"), False),
    ("channel sender required", client_config + client_action.replace("          sender_name: Automation\n", ""), False),
    ("channel old client field rejected", client_config + client_action.replace("sender_name:", "client_id:"), False),
    ("DM sender required", client_config + client_dm.replace("          sender_private_key: !secret personal_room_private_key\n", ""), False),
    ("DM recipient required", client_config + client_dm.replace('          recipient_public_key: "' + "12" * 32 + '"\n', ""), False),
    ("DM sender type checked", client_config + client_dm.replace("!secret personal_room_private_key", "testing"), False),
    ("DM sender existence checked", client_config + client_dm.replace("!secret personal_room_private_key", "!secret absent_key"), False),
    ("DM old client field rejected", client_config + client_dm.replace("sender_private_key:", "client_id:"), False),
    ("DM old recipient field rejected", client_config + client_dm.replace("recipient_public_key:", "recipient:"), False),
    ("client send rejects generic ID", client_config + client_action.replace("sender_name:", "id:"), False),
    ("client send rejects old channel field", client_config + client_action.replace("channel_id:", "channel:"), False),
    ("client advert rejects generic ID", client_config + client_advert.replace("client_id:", "id:"), False),
    ("client advert rejects wrong resource type", client_config + client_advert.replace("client_id: automation_client", "client_id: testing"), False),
    ("room explicit storage ID", room.replace("id: personal", "id: personal_handle\n      storage_id: personal"), True),
    ("room handle rename with retained storage", room.replace("id: personal", "id: renamed\n      storage_id: personal"), True),
    ("room long handle with short storage", room.replace("id: personal", "id: personal_room_handle\n      storage_id: personal"), True),
    ("room storage namespace limit", room.replace("id: personal", "id: personal\n      storage_id: namespace_too_long"), False),
    ("room reserved handle rejected", room.replace("id: personal", "id: public"), False),
    ("room reserved storage allowed", room.replace("id: personal", "id: public_room\n      storage_id: public"), True),
    ("duplicate room storage rejected", room.replace("      password: !secret personal_room_password", "      password: !secret personal_room_password\n    - id: other_room\n      storage_id: personal\n      name: Other\n      access: public"), False),
    ("room and channel handle conflict", original.replace("  channels:", "  rooms:\n    - id: testing\n      name: Room\n      access: public\n  channels:"), False),

    ("client budget", client_config.replace("meshcore:\n", "meshcore:\n  client_state_budget: 8192\n"), True),
    ("negative client budget", client_config.replace("meshcore:\n", "meshcore:\n  client_state_budget: -1\n"), False),
    ("two static clients with default name", client_config, True),
    ("client channel action", client_config + client_action, True),
    ("client DM action", client_config + client_dm, True),
    ("client templated recipient", client_config + client_dm.replace('recipient_public_key: "' + "12" * 32 + '"',
        'recipient_public_key: !lambda return "' + "12" * 32 + '";'), True),
    ("client advertisement", client_config + client_advert, True),
    ("client direct scoped advert rejected", client_config + client_advert.replace("mode: flood", "mode: direct"), False),
    ("client direct advert", client_config + client_advert.replace("mode: flood", "mode: direct").replace("          scope: nyc\n", ""), True),
    ("client only", client_config.replace('  channels:\n    - id: testing\n      key: "#testing"\n', ''), True),
    ("client requires private key", client_config.replace("      private_key: !secret personal_room_private_key\n", ""), False),
    ("client accepts literal demo key", client_config.replace("!secret personal_room_private_key", '"' + expanded_key_fixture(5) + '"'), True),
    ("duplicate client keys", client_config.replace("!secret building_room_private_key", "!secret personal_room_private_key"), False),
    ("duplicate client scalar", client_config.replace("!secret building_room_private_key", "!secret same_scalar_key"), False),
    ("duplicate client IDs", client_config.replace("id: second_client", "id: automation_client"), False),
    ("client and channel ID conflict", client_config.replace("id: automation_client", "id: testing"), False),
    ("client and room identity conflict", static_room.replace("  rooms:", "  clients:\n    - id: automation_client\n      private_key: !secret personal_room_private_key\n  rooms:"), False),
    ("client and room ID conflict", room.replace("  rooms:", "  clients:\n    - id: personal\n      private_key: !secret personal_room_private_key\n  rooms:"), False),
    ("client invalid private key", client_config.replace("!secret personal_room_private_key", "!secret unclamped_key"), False),
    ("client unknown action ID", client_config + client_dm.replace("sender_private_key:", "sender_id:"), False),
    ("client wrong action ID type", client_config + client_dm.replace("recipient_public_key:", "recipient_id:"), False),
    ("client missing destination", client_config + client_action.replace("          channel_id: testing\n", ""), False),
    ("client room destination deferred", client_config + client_action.replace("channel_id: testing", "room_id: personal"), False),
    ("client two destinations", client_config + client_dm.replace("          message:", "          channel_id: testing\n          message:"), False),
    ("client short recipient rejected", client_config + client_dm.replace("12" * 32, "1234"), False),
    ("client reserved recipient rejected", client_config + client_dm.replace("12" * 32, "00" * 32), False),
    ("client recipient rejects nonhex", client_config + client_dm.replace("12" * 32, "xy" * 32), False),
    ("client timestamp required", client_config + client_dm.replace("          timestamp: !lambda return id(mesh_node).next_channel_timestamp();\n", ""), False),
    ("client invalid timestamp", client_config + client_dm.replace("!lambda return id(mesh_node).next_channel_timestamp();", "0"), False),
    ("client name limits", client_config.replace("name: Second", 'name: "Bad:Name"'), False),

    ("TX queue minimum", original.replace("meshcore:\n", "meshcore:\n  tx_queue_size: 1\n"), True),
    ("TX queue non-power-of-two", original.replace("meshcore:\n", "meshcore:\n  tx_queue_size: 3\n"), True),
    ("TX queue enlarged", original.replace("meshcore:\n", "meshcore:\n  tx_queue_size: 8\n"), True),
    ("TX queue zero rejected", original.replace("meshcore:\n", "meshcore:\n  tx_queue_size: 0\n"), False),
    ("TX queue negative rejected", original.replace("meshcore:\n", "meshcore:\n  tx_queue_size: -1\n"), False),
    ("TX queue fractional rejected", original.replace("meshcore:\n", "meshcore:\n  tx_queue_size: 1.5\n"), False),
    ("meshcore rejects writable clock", original.replace("  time_id: mesh_time", "  time_id: mesh_time\n  clock_id: usb_clock"), False),
    ("missing time source", original.replace("  clock: !include time.yaml\n", ""), False),
    ("unknown time ID", original.replace("time_id: mesh_time", "time_id: missing_clock"), False),
    ("wrong time ID type", original.replace("time_id: mesh_time", "time_id: lora_radio"), False),
    ("personal room, implicit schema", room, True),
    ("public room", room.replace("      password: !secret personal_room_password", "      access: public"), True),
    ("explicit private", room.replace("name: personal", "name: personal\n      access: private"), True),
    ("private requires password", room.replace("      password: !secret personal_room_password\n", ""), False),
    ("public rejects password", room.replace("name: personal", "name: personal\n      access: public"), False),
    ("old open policy rejected", room.replace("name: personal", "name: personal\n      access: open"), False),
    ("old password policy rejected", room.replace("name: personal", "name: personal\n      access: password"), False),
    ("empty password", room.replace("!secret personal_room_password", "!secret empty_password"), False),
    ("short password", room.replace("!secret personal_room_password", "!secret short_password"), False),
    ("long password", room.replace("!secret personal_room_password", "!secret long_password"), False),
    ("password whitespace", room.replace("!secret personal_room_password", "!secret spaced_password"), False),
    ("password number", room.replace("!secret personal_room_password", "!secret numeric_password"), False),
    ("password null", room.replace("!secret personal_room_password", "!secret null_password"), False),
    ("inline password rejected", room.replace("!secret personal_room_password", '"do-not-embed-me"'), False),
    ("missing secret", room.replace("!secret personal_room_password", "!secret absent_password"), False),
    ("unknown room access", room.replace("name: personal", "name: personal\n      access: guessed"), False),
    ("three named rooms with stable IDs", (root / "examples/heltec-v3-three-rooms.yaml").read_text(), True),
    ("standalone ping bot", (root / "examples/heltec-v3-ping-bot.yaml").read_text(), True),
    ("ping bot requires private channel key", (root / "examples/heltec-v3-ping-bot.yaml").read_text().replace(
        "!secret meshcore_private_channel_key", "!secret absent_channel_key"), False),
    ("static room", static_room, True),
    ("private key requires secret", static_room.replace("!secret personal_room_private_key", '"' + expanded_fixture + '"'), False),
    ("private key rejects seed length", static_room.replace("!secret personal_room_private_key", "!secret seed_length_key"), False),
    ("private key clamp", static_room.replace("!secret personal_room_private_key", "!secret unclamped_key"), False),
    ("private key hex", static_room.replace("!secret personal_room_private_key", "!secret invalid_key"), False),
    ("private key uppercase hex", static_room.replace("!secret personal_room_private_key", "!secret uppercase_key"), True),
    ("public key is derived", static_room.replace("      name: personal", "      name: personal\n      public_key: " + '"' + "01" * 32 + '"'), False),
    ("private key rejects wrong type", static_room.replace("!secret personal_room_private_key", "!secret numeric_password"), False),
    ("old identity_seed field rejected", static_room.replace("private_key:", "identity_seed:"), False),
    ("old identity_key field rejected", static_room.replace("private_key:", "identity_key:"), False),
    ("static public room", static_room.replace("      password: !secret personal_room_password", "      access: public"), True),
    ("duplicate room identities rejected", static_room.replace("      password: !secret personal_room_password", "      password: !secret personal_room_password\n    - id: building\n      name: Building\n      private_key: !secret personal_room_private_key\n      password: !secret building_room_password"), False),
    ("same scalar different nonce rejected", static_room.replace("      password: !secret personal_room_password", "      password: !secret personal_room_password\n    - id: building\n      name: Building\n      private_key: !secret same_scalar_key\n      password: !secret building_room_password"), False),
    ("hardware-only V3 composition", "esphome:\n  name: board-only-check\npackages:\n  board: github://fortuna/ESPHomeCollection/boards/heltec-wifi-lora-32-v3/board.yaml@71d63352dcb09a396d1cbfe2dc28c3210d8d436e\n  network: !include radio-profile.yaml\n", True),
    ("explicit schema", room.replace("meshcore:\n", "meshcore:\n  schema_version: 1\n"), True),
    ("negative room budget", room.replace("meshcore:\n", "meshcore:\n  room_state_budget: -1\n"), False),
    ("room budget", room.replace("meshcore:\n", "meshcore:\n  room_state_budget: 32768\n"), True),
    ("eight rooms", room.replace("      password: !secret personal_room_password", "      password: !secret personal_room_password" + "".join(f"\n    - id: room{i}\n      name: room{i}\n      access: public" for i in range(7))), True),
    ("ESP-IDF required without console", room.replace(
        "    advanced:\n      loop_task_stack_size: 16384", "    type: arduino"), False),
    ("headless ESP32 SX1262", fixture_source("headless-rooms.yaml"), True),
    ("unknown room schema", room.replace("meshcore:\n", "meshcore:\n  schema_version: 2\n"), False),
    ("second room", room.replace("      password: !secret personal_room_password", "      password: !secret personal_room_password\n    - id: building\n      name: building\n      password: !secret building_room_password"), True),
    ("third room", room.replace("      password: !secret personal_room_password", "      password: !secret personal_room_password\n    - id: building\n      name: building\n      password: !secret building_room_password\n    - id: third\n      name: third\n      access: public"), True),
    ("duplicate room stable id", room.replace("name: personal", "name: personal\n    - id: personal\n      name: different"), False),
    ("two-room package composition", 'packages:\n  base: !include legacy-room.yaml\nmeshcore:\n  rooms:\n    - id: building\n      name: Building\n      password: !secret building_room_password\n' , True),
    ("stable id length", room.replace("id: personal", "id: personal_room_too_long"), False),
    ("room cannot repeat", room.replace("name: personal", "name: personal\n      repeat: true"), False),
    ("room outbound path width", room.replace("path_hash_size: 2", "path_hash_size: 3"), False),
    ("hashtag", original, True),
    ("public", original.replace('key: "#testing"', 'public: true'), True),
    ("private", original.replace('key: "#testing"', 'key: "AQIDBAUGBwgJCgsMDQ4PEA=="'), True),
    ("reserved path width", original.replace("path_hash_size: 2", "path_hash_size: 4"), False),
    ("NYC scope", original.replace('key: "#testing"', 'key: "#testing"\n      scope: nyc'), True),
    ("explicit no scope", original.replace('key: "#testing"', 'key: "#testing"\n      scope: null'), True),
    ("scope with hashtag", original.replace('key: "#testing"', 'key: "#testing"\n      scope: "#nyc"'), False),
    ("empty scope", original.replace('key: "#testing"', 'key: "#testing"\n      scope: ""'), False),
    ("long scope", original.replace('key: "#testing"', 'key: "#testing"\n      scope: ' + 'x' * 31), False),
    ("scope whitespace", original.replace('key: "#testing"', 'key: "#testing"\n      scope: "new york"'), False),
    ("scope wrong type", original.replace('key: "#testing"', 'key: "#testing"\n      scope: true'), False),
    ("scoped example", original.replace('key: "#testing"', 'key: "#testing"\n      scope: nyc'), True),
    ("empty key", original.replace('key: "#testing"', 'key: "AAAAAAAAAAAAAAAAAAAAAA=="'), False),
    ("two key sources", original.replace('key: "#testing"', 'key: "#testing"\n      public: true'), False),
    ("disabled public", original.replace('key: "#testing"', 'public: false'), False),
    ("duplicate channel ID", original.replace('key: "#testing"', 'key: "#testing"\n    - id: testing\n      key: "#second"'), False),
    ("RX recovery required", original + "\nsx126x:\n  - id: !extend lora_radio\n    rx_start: false\n", False),
    ("variable length required", original + "\nsx126x:\n  - id: !extend lora_radio\n    payload_length: 37\n", False),
    ("raw action bypasses owner", original + "\ninterval:\n  - interval: 10s\n    then:\n      - sx126x.send_packet:\n          id: lora_radio\n          data: [1, 2]\n", False),
]
advertisement_action = """
script:
  - id: announce
    then:
      - meshcore.advertise_room:
          room_id: personal
          mode: flood
"""
scoped_advertisement_action = advertisement_action.replace("          mode: flood", "          mode: flood\n          scope: nyc")
advertisement_intervals = """
interval:
  - id: personal_nearby
    interval: 240min
    startup_delay: 2s
    then:
      - meshcore.advertise_room:
          room_id: personal
          mode: direct
  - id: personal_flood
    interval: 47h
    startup_delay: 47h
    then:
      - meshcore.advertise_room:
          room_id: personal
          mode: flood
"""
cases += [
    ("native interval room announcements", room + advertisement_intervals, True),
    ("per-room advertisement settings removed", room.replace("name: personal", "name: personal\n      advertisements: []"), False),
    ("per-room advertisement schedules removed", room.replace("name: personal", "name: personal\n      advertisements:\n        - id: nearby\n          mode: direct\n          interval: 240min"), False),
    ("room action rejects generic node ID", room + advertisement_action.replace("room_id: personal", "id: mesh_node\n          room: personal"), False),
    ("room action uses handle not storage ID", room.replace("id: personal", "id: room_handle\n      storage_id: personal") + advertisement_action.replace("room_id: personal", "room_id: room_handle"), True),
    ("room action rejects storage-only reference", room.replace("id: personal", "id: room_handle\n      storage_id: personal") + advertisement_action, False),
    ("room advertisement action", room + advertisement_action, True),
    ("scoped room advertisement", room + scoped_advertisement_action, True),
    ("scoped room advertisement case-sensitive name", room + scoped_advertisement_action.replace("scope: nyc", "scope: NYC"), True),
    ("scoped room advertisement null", room + scoped_advertisement_action.replace("scope: nyc", "scope: null"), True),
    ("direct room advertisement null scope", room + scoped_advertisement_action.replace("mode: flood", "mode: direct").replace("scope: nyc", "scope: null"), True),
    ("direct room advertisement rejects scope", room + scoped_advertisement_action.replace("mode: flood", "mode: direct"), False),
    ("default direct advertisement rejects scope", room + scoped_advertisement_action.replace("          mode: flood\n", ""), False),
    ("advertisement scope rejects hashtag", room + scoped_advertisement_action.replace("scope: nyc", 'scope: "#nyc"'), False),
    ("advertisement scope rejects empty", room + scoped_advertisement_action.replace("scope: nyc", 'scope: ""'), False),
    ("advertisement scope rejects long name", room + scoped_advertisement_action.replace("scope: nyc", "scope: " + "x" * 31), False),
    ("advertisement scope rejects whitespace", room + scoped_advertisement_action.replace("scope: nyc", "scope: new york"), False),
    ("advertisement scope rejects wrong type", room + scoped_advertisement_action.replace("scope: nyc", "scope: true"), False),
    ("advertisement scope rejects raw keys", room + scoped_advertisement_action.replace("scope: nyc", "scope_key: AQIDBAUGBwgJCgsMDQ4PEA=="), False),
    ("native interval scoped advertisement", room + advertisement_intervals.replace("          mode: flood", "          mode: flood\n          scope: nyc"), True),
    ("room advertisement default direct action", room + advertisement_action.replace("          mode: flood\n", ""), True),
    ("room advertisement unknown room", room + advertisement_action.replace("room_id: personal", "room_id: missing"), False),
    ("room advertisement bad mode", room + advertisement_action.replace("mode: flood", "mode: guessed"), False),
    ("room advertisement wrong resource type", room + advertisement_action.replace("room_id: personal", "room_id: lora_radio"), False),
    ("channel-only advertisement rejected", original + advertisement_action, False),
    ("native interval unknown room", room + advertisement_intervals.replace("room_id: personal", "room_id: missing"), False),
    ("native interval package extension", "packages:\n  base: !include advertisement-intervals.yaml\ninterval:\n  - id: !extend personal_nearby\n    interval: 8h\n", True),
]

# Channel resource/action validation, including real package merges and lambdas.
action = """
interval:
  - interval: 1h
    then:
      - meshcore.send_channel_message:
          channel_id: testing
          sender_name: Probe
          timestamp: !lambda return id(mesh_node).next_channel_timestamp();
          message: hello
"""
handler = """      on_message:
        - delay: 1ms
        - meshcore.send_channel_message:
            channel_id: testing
            sender_name: !lambda return message.sender();
            timestamp: !lambda return message.timestamp;
            message: !lambda return message.body();
"""
cases += [
    ("channel send rejects generic ID", original + action.replace("channel_id:", "id:"), False),
    ("channel send action", original + action, True),
    ("send-only hashtag", original.replace('  channels:\n    - id: testing\n      key: "#testing"\n', '') + action.replace('channel_id: testing', 'channel_key: "#other"'), True),
    ("channel byte-list key", original.replace('key: "#testing"', "key: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]"), True),
    ("send-only byte-list key", original + action.replace("channel_id: testing", "channel_key: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]"), True),
    ("send-only invalid byte-list key", original + action.replace("channel_id: testing", "channel_key: [1, 2, 3]"), False),
    ("send-only binary key", original + action.replace('channel_id: testing', 'channel_key: "AQIDBAUGBwgJCgsMDQ4PEA=="'), True),
    ("send-only secret", original + action.replace('channel_id: testing', 'channel_key: !secret test_channel_key'), True),
    ("send rejects both destinations", original + action.replace('channel_id: testing', 'channel_id: testing\n          channel_key: "#other"'), False),
    ("send rejects invalid key", original + action.replace('channel_id: testing', 'channel_key: "#"'), False),
    ("old hashtag field rejected", original.replace('key: "#testing"', 'hashtag: "#testing"'), False),
    ("action requires caller timestamp", original + action.replace("          timestamp: !lambda return id(mesh_node).next_channel_timestamp();\n", ""), False),
    ("action explicit timestamp", original + action.replace("!lambda return id(mesh_node).next_channel_timestamp();", "1800000000"), True),
    ("action zero timestamp rejected", original + action.replace("!lambda return id(mesh_node).next_channel_timestamp();", "0"), False),
    ("action timestamp overflow rejected", original + action.replace("!lambda return id(mesh_node).next_channel_timestamp();", "4294967296"), False),
    ("action negative timestamp rejected", original + action.replace("!lambda return id(mesh_node).next_channel_timestamp();", "-1"), False),
    ("action max timestamp", original + action.replace("!lambda return id(mesh_node).next_channel_timestamp();", "4294967295"), True),
    ("channel owning event with delayed templates", original.replace('      key: "#testing"', '      key: "#testing"\n' + handler.rstrip()), True),
    ("channel package extension", 'esphome:\n  name: channel-extend-check\npackages:\n  base: !include channel.yaml\nmeshcore:\n  channels:\n    - id: !extend testing\n      scope: nyc\n', True),
    ("channel missing ID", original.replace("- id: testing", "- name: testing"), False),
    ("channel duplicate key", original.replace('key: "#testing"', 'key: "#testing"\n    - id: another\n      key: "#testing"'), False),
    ("five channels", channel_config(5), True),
    ("eight channels with last-channel action", channel_config(8) + action.replace("id: testing", "id: ch6"), True),
    ("eight channels reject duplicate final key", channel_config(8).replace('key: "#ch6"', 'key: "#testing"'), False),
    ("eight channels reject duplicate final ID", channel_config(8).replace("id: ch6", "id: testing"), False),
    ("channel package addition beyond four", 'esphome:\n  name: channel-add-check\npackages:\n  base: !include channel.yaml\nmeshcore:\n  channels:\n' + ''.join(f'    - id: ch{i}\n      key: "#ch{i}"\n' for i in range(7)), True),
    ("channel secret redaction", original.replace('key: "#testing"', 'key: !secret test_channel_key'), True),
    ("action wrong resource type", original + action.replace("id: testing", "id: mesh_node"), False),
    ("action unknown channel", original + action.replace("id: testing", "id: missing_channel"), False),
    ("action needs sender", original + action.replace("          sender_name: Probe\n", ""), False),
    ("action invalid sender", original + action.replace("sender_name: Probe", 'sender_name: "bad:name"'), False),
    ("action empty message", original + action.replace("message: hello", 'message: ""'), False),
    ("action total byte limit", original + action.replace("message: hello", "message: " + 'x' * 154), False),
    ("action UTF-8 limit", original + action.replace("message: hello", "message: " + 'é' * 80), False),
]


# Exercise package overrides and the MeshCore preamble transition on actual examples.
profile_cases = {}
for example in sorted((root / "examples").glob("heltec-v3-*.yaml")):
    for sf in (7, 8, 9):
        label = f"{example.stem} RF substitutions SF{sf}"
        profile_cases[label] = sf
        overrides = "" if sf == 7 else (
            f'substitutions:\n  meshcore_frequency: 915MHz\n  meshcore_bandwidth: 125_0kHz\n'
            f'  meshcore_sf: "{sf}"\n  meshcore_cr: CR_4_6\n  meshcore_pa_power: 17\n')
        cases.append((label, f'packages:\n  example: !include {example.name}\n' + overrides, True))


def fixture_content(text):
    return text.replace("path: ../../components", "path: ../components").replace("path: ../components", f"path: {root / 'components'}").replace(
        "source: github://fortuna/esphome-meshblocks@review/initial-import",
        f"source:\n      type: local\n      path: {root / 'components'}")


def find_actions(value, name):
    if isinstance(value, dict):
        if name in value:
            yield value[name]
        for child in value.values():
            yield from find_actions(child, name)
    elif isinstance(value, list):
        for child in value:
            yield from find_actions(child, name)


with tempfile.TemporaryDirectory(prefix="meshcore-schema-") as directory:
    fixture = Path(directory)
    for source in [*fixtures.glob("*.yaml"), *(root / "examples").glob("heltec-v3-*.yaml")]:
        if "secrets" not in source.name:
            (fixture / source.name).write_text(fixture_content(source.read_text()))
    (fixture / "advertisement-intervals.yaml").write_text(fixture_content(room + advertisement_intervals))
    (fixture / "secrets.yaml").write_text("""personal_room_password: personal-pass01
family_room_password: family-pass01
building_room_password: building-pass01
workshop_room_password: workshop-pass01
empty_password: ""
short_password: short
long_password: this-is-too-long
spaced_password: "contains space"
numeric_password: 123456789012
null_password: null
test_channel_key: "AQIDBAUGBwgJCgsMDQ4PEA=="
meshcore_private_channel_key: "AQIDBAUGBwgJCgsMDQ4PEA=="
seed_length_key: "0101010101010101010101010101010101010101010101010101010101010101"
invalid_key: "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"
""" + f'personal_room_private_key: "{expanded_fixture}"\n'
    + f'family_room_private_key: "{expanded_fixture}"\n'
    + f'building_room_private_key: "{expanded_key_fixture(2)}"\n'
    + f'public_room_private_key: "{expanded_key_fixture(3)}"\n'
    + f'bot_client_private_key: "{expanded_key_fixture(4)}"\n'
    + 'bot_recipient_public_key: "' + '12' * 32 + '"\n'
    + f'uppercase_key: "{expanded_fixture.upper()}"\n'
    + f'same_scalar_key: "{expanded_fixture[:64] + "00" * 32}"\n'
    + 'unclamped_key: "' + "01" * 64 + '"\n')
    for label, content, valid in cases:
        with tempfile.NamedTemporaryFile(mode="w", suffix=".yaml", prefix=".schema-test-", dir=fixture) as config:
            config.write(fixture_content(content))
            config.flush()
            result = subprocess.run(["esphome", "config", config.name], capture_output=True, text=True,
                                    env=dict(os.environ, ESPHOME_DATA_DIR=str(root / "local/.esphome")))
            if (result.returncode == 0) != valid:
                raise AssertionError(f"{label}: unexpected result\n{result.stdout}\n{result.stderr}")
            if label in profile_cases:
                import yaml
                radio = yaml.load(result.stdout, Loader=yaml.BaseLoader)["sx126x"][0]
                sf = profile_cases[label]
                assert int(radio["spreading_factor"]) == sf, label
                assert int(radio["preamble_size"]) == (32 if sf <= 8 else 16), label
                assert float(radio["frequency"]) == (910525000 if sf == 7 else 915000000), label
                assert radio["bandwidth"] == ("62_5kHz" if sf == 7 else "125_0kHz"), label
                assert radio["coding_rate"] == ("CR_4_5" if sf == 7 else "CR_4_6"), label
                assert int(radio["pa_power"]) == (22 if sf == 7 else 17), label
            if label == "ESP-IDF required without console":
                assert "current storage/crypto adapters require ESP32 with ESP-IDF" in result.stdout + result.stderr
            if valid:
                # Remote board loading must not lose !secret output redaction.
                for password in ("personal-pass01", "family-pass01", "building-pass01", "workshop-pass01", "AQIDBAUGBwgJCgsMDQ4PEA==" if label in ("channel secret redaction", "standalone ping bot") else "not-a-secret-fixture"):
                    assert password not in result.stdout + result.stderr, label
                for private_key in (expanded_fixture, expanded_key_fixture(2), expanded_key_fixture(3), expanded_key_fixture(4), expanded_fixture.upper()):
                    assert private_key not in result.stdout + result.stderr, label
            if label in ("five channels", "eight channels with last-channel action", "channel package addition beyond four"):
                import yaml
                merged = yaml.load(result.stdout, Loader=yaml.BaseLoader)
                expected = ["testing"] + [f"ch{i}" for i in range(4 if label == "five channels" else 7)]
                assert [channel["id"] for channel in merged["meshcore"]["channels"]] == expected
            if label in ("room explicit storage ID", "room handle rename with retained storage", "room long handle with short storage", "room reserved storage allowed"):
                import yaml
                merged = yaml.load(result.stdout, Loader=yaml.BaseLoader)
                actual = merged["meshcore"]["rooms"][0]
                assert actual["id"] != actual["storage_id"]
                assert actual["storage_id"] == ("public" if label == "room reserved storage allowed" else "personal")
            if label == "three named rooms with stable IDs":
                import yaml
                # ESPHome emits !lambda scalars outside this block. BaseLoader keeps
                # tags inert while letting us check the merged room values as text.
                merged = yaml.load(result.stdout, Loader=yaml.BaseLoader)
                actual = merged["meshcore"]["rooms"]
                assert [(item["id"], item["name"], item["access"]) for item in actual] == [
                    ("family", "Family", "private"),
                    ("building", "Building", "private"),
                    ("public_room", "Public", "public"),
                ]
                assert [item["storage_id"] for item in actual] == ["family", "building", "public"]
                import esphome.config_validation as cv
                intervals = {item["id"]: item for item in merged["interval"] if "id" in item}
                for name in ("family_nearby", "building_nearby", "public_nearby"):
                    assert cv.positive_time_period_milliseconds(intervals[name]["interval"]).total_milliseconds == 240 * 60000
                    assert list(find_actions(intervals[name], "meshcore.advertise_room"))[0]["mode"] == "direct"
                assert cv.positive_time_period_milliseconds(intervals["public_flood"]["interval"]).total_milliseconds == 47 * 3600000
                assert cv.positive_time_period_milliseconds(intervals["public_flood"]["startup_delay"]).total_milliseconds == 47 * 3600000
                flood = list(find_actions(intervals["public_flood"], "meshcore.advertise_room"))[0]
                assert flood["room_id"] == "public_room" and flood["scope"] == "nyc"
            if label == "native interval package extension":
                import yaml
                import esphome.config_validation as cv
                actual = yaml.load(result.stdout, Loader=yaml.BaseLoader)
                intervals = {item["id"]: item for item in actual["interval"] if "id" in item}
                assert cv.positive_time_period_milliseconds(intervals["personal_nearby"]["interval"]).total_milliseconds == 8 * 3600000
                assert cv.positive_time_period_milliseconds(intervals["personal_flood"]["interval"]).total_milliseconds == 47 * 3600000
                assert list(find_actions(intervals["personal_nearby"], "meshcore.advertise_room"))[0]["mode"] == "direct"
                assert list(find_actions(intervals["personal_flood"], "meshcore.advertise_room"))[0]["mode"] == "flood"
            if label == "standalone ping bot":
                import yaml
                actual = yaml.load(result.stdout, Loader=yaml.BaseLoader)
                channels = actual["meshcore"]["channels"]
                assert [channel["id"] for channel in channels] == ["testing", "private_chat"]
                assert not actual["meshcore"].get("rooms")
                assert not actual["meshcore"]["clients"]
                assert actual["time"][0]["platform"] == "template"
                assert "1800000000" not in str(actual["time"])
                scripts = {script["id"]: script for script in actual["script"]}
                # Check the merged receive -> script -> send routing. Private
                # input must never invoke the public responder or share its cooldown.
                for channel in channels:
                    script_id = channel["id"] + "_pong"
                    assert [action["id"] for action in find_actions(channel, "script.execute")] == [script_id]
                    assert scripts[script_id]["mode"] == "single"
                    assert list(find_actions(scripts[script_id], "meshcore.send_channel_message"))[0]["sender_name"] == "ESPHome Ping Bot"
                    assert [action["channel_id"] for action in find_actions(scripts[script_id], "meshcore.send_channel_message")] == [channel["id"]]
            print(f"PASS: {label}")
