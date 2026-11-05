"""Build-sized channels and rooms; no repeater or companion firmware."""
import base64
import hashlib
from io import StringIO
from pathlib import Path
import re

import yaml

import esphome.codegen as cg
from esphome import automation
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import sx126x, time as time_
from esphome.const import CONF_ID, CONF_TRIGGER_ID
from esphome.core import CORE, EsphomeError, Lambda
from esphome.yaml_util import is_secret, parse_yaml

DEPENDENCIES = ["sx126x", "esp32"]
AUTO_LOAD = ["meshcore_core", "meshcore_ed25519"]
meshcore_ns = cg.esphome_ns.namespace("meshcore")
MeshCoreComponent = meshcore_ns.class_("MeshCoreComponent", cg.Component, sx126x.SX126xListener)
Channel = meshcore_ns.class_("Channel")
Room = meshcore_ns.class_("Room")
Client = meshcore_ns.class_("Client")
SendClientMessageAction = meshcore_ns.class_("SendClientMessageAction", automation.Action)
AdvertiseClientAction = meshcore_ns.class_("AdvertiseClientAction", automation.Action)
ClientMessage = cg.global_ns.namespace("meshcore_lib").struct("ClientMessage")
ClientMessageTrigger = meshcore_ns.class_("ClientMessageTrigger", automation.Trigger.template(ClientMessage))
ChannelMessage = cg.global_ns.namespace("meshcore_lib").struct("ChannelMessage")
ChannelText = cg.global_ns.namespace("meshcore_lib").class_("ChannelText")
ChannelMessageTrigger = meshcore_ns.class_("ChannelMessageTrigger", automation.Trigger.template(ChannelMessage))
SendChannelMessageAction = meshcore_ns.class_("SendChannelMessageAction", automation.Action)
AdvertiseRoomAction = meshcore_ns.class_("AdvertiseRoomAction", automation.Action)
RoomAccess = cg.global_ns.namespace("meshcore_lib").enum("RoomAccess", is_class=True)
ROOM_ACCESS = {"private": RoomAccess.PRIVATE, "public": RoomAccess.PUBLIC}


def channel_name(value):
    value = cv.string_strict(value)
    if not re.fullmatch(r"[a-z][a-z0-9_]{0,23}", value):
        raise cv.Invalid("Use a channel name of 1-24 lowercase letters, digits, or underscores")
    return value


def hashtag(value):
    value = cv.string_strict(value)
    if not value.startswith("#") or len(value.encode()) < 2 or len(value.encode()) > 31 or any(ord(c) < 33 for c in value):
        raise cv.Invalid("Use a hashtag including #, 2-31 UTF-8 bytes, with no whitespace/control characters")
    return value


def channel_key(value):
    if isinstance(value, list):
        key = cv.ensure_list(cv.int_range(min=0, max=255))(value)
        if len(key) not in (16, 32) or not any(key):
            raise cv.Invalid("Channel key must contain 16 or 32 bytes and not be all zero")
        return key
    value = cv.string_strict(value)
    if not is_secret(value):
        _restore_secret_reference(value)
    if value.startswith("#"):
        return hashtag(value)
    try:
        key = base64.b64decode(value, validate=True)
    except ValueError as err:
        raise cv.Invalid("Channel key must be base64") from err
    if len(key) not in (16, 32):
        raise cv.Invalid("Channel key must decode to 16 or 32 bytes")
    if not any(key):
        raise cv.Invalid("An all-zero channel key is not allowed")
    return value


def sender_name(value):
    value = cv.string_strict(value)
    if not 1 <= len(value) <= 24 or any(not 32 <= ord(c) <= 126 or c == ':' for c in value):
        raise cv.Invalid("Sender name must be 1-24 printable ASCII characters without colon")
    return value


def scope_name(value):
    value = cv.string_strict(value)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,29}", value):
        raise cv.Invalid("Use a bare region name such as nyc (1-30 letters, digits, underscores or hyphens), or null")
    return value


def resolve_scope_key(scope):
    return hashlib.sha256(("#" + scope).encode()).digest()[:16] if scope is not None else b""


CHANNEL_SCHEMA = cv.All(cv.Schema({
    cv.Required(CONF_ID): cv.All(channel_name, cv.declare_id(Channel)),
    cv.Optional("public"): cv.one_of(True),
    cv.Optional("key"): channel_key,
    cv.Optional("scope"): cv.Any(None, scope_name),
    cv.Optional("on_message"): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ChannelMessageTrigger),
    }),
}), cv.has_exactly_one_key("public", "key"))


def resolve_channel_key(channel):
    if "public" in channel:
        return bytes.fromhex("8b3387e9c5cdea6ac9e5edbaa115cd72")
    return resolve_key(channel["key"])


def resolve_key(value):
    if isinstance(value, list):
        return bytes(value)
    return hashlib.sha256(value.encode()).digest()[:16] if value.startswith("#") else base64.b64decode(value)


def unique_channels(config):
    names = [str(channel[CONF_ID]) for channel in config["channels"]]
    if len(set(names)) != len(names):
        raise cv.Invalid("Channel IDs must be unique")
    keys = [resolve_channel_key(channel) for channel in config["channels"]]
    if len(set(keys)) != len(keys):
        raise cv.Invalid("Channel keys must be unique; configure one subscription per key")
    room_ids = [str(room[CONF_ID]) for room in config["rooms"]]
    storage_ids = [room["storage_id"] for room in config["rooms"]]
    if len(set(storage_ids)) != len(storage_ids):
        raise cv.Invalid("Room storage IDs must be unique")
    if len(set(room_ids)) != len(room_ids):
        raise cv.Invalid("Room handle IDs must be unique")
    identities = [bytes.fromhex(room["private_key"])[:32]
                  for room in [*config["rooms"], *config["clients"]] if "private_key" in room]
    if len(set(identities)) != len(identities):
        raise cv.Invalid("Each room and client must have a different identity")
    client_ids = [str(client[CONF_ID]) for client in config["clients"]]
    if len(set(client_ids)) != len(client_ids):
        raise cv.Invalid("Client IDs must be unique")
    if set(client_ids) & set(room_ids):
        raise cv.Invalid("Client and room IDs must be distinct")
    if config["rooms"] and config["path_hash_size"] != 2:
        raise cv.Invalid("The first room experiment requires path_hash_size: 2; other outbound room widths are not implemented")
    return config


def room_id(value):
    value = channel_name(value)
    if len(value) > 12:
        raise cv.Invalid("Room stable ids must fit 12 characters for the storage namespace")
    return value


def _restore_secret_reference(value):
    """Recover ESPHome's secret marker lost while loading remote packages.

    ESPHome 2026.10.1 loads remote YAML with clear_secrets=True. The original
    scalar still carries its source range. Re-read only a literal !secret node
    through ESPHome's loader, preserving its resolution and output redaction.
    Never accept a plain string or modify ESPHome's private secret registry.
    """
    source = getattr(value, "esp_range", None)
    if source is None:
        return False
    start, end = source.start_mark, source.end_mark
    if start.document != end.document:
        return False
    try:
        lines = Path(start.document).read_text(encoding="utf-8").splitlines(keepends=True)
        lines = lines[start.line:end.line + 1]
        lines[-1] = lines[-1][:end.column]
        lines[0] = lines[0][start.column:]
        fragment = "".join(lines)
        node = yaml.compose(fragment, Loader=yaml.SafeLoader)
        if not isinstance(node, yaml.ScalarNode) or node.tag != "!secret":
            return False
        resolved = parse_yaml(Path(start.document), StringIO(fragment))
        return resolved == value and bool(is_secret(value))
    except (OSError, UnicodeError, IndexError, yaml.YAMLError, EsphomeError):
        return False


def room_password(value):
    value = cv.string_strict(value)
    if not is_secret(value) and not _restore_secret_reference(value):
        raise cv.Invalid("Room passwords must use !secret")
    if not 12 <= len(value) <= 15 or any(not 33 <= ord(c) <= 126 for c in value):
        raise cv.Invalid("Room passwords must be 12-15 printable ASCII characters without spaces")
    return value


def room_access(config):
    if config["access"] == "private" and "password" not in config:
        raise cv.Invalid("Private rooms require password: !secret <name>; use access: public for password-free joining")
    if config["access"] == "public" and "password" in config:
        raise cv.Invalid("Public rooms must not specify a password")
    return config


def native_private_key(value):
    value = cv.string_strict(value)
    if not re.fullmatch(r"[0-9a-fA-F]{128}", value):
        raise cv.Invalid("MeshCore private identity keys must contain 64 bytes encoded as 128 hex characters")
    key = bytes.fromhex(value)
    if key[0] & 7 or key[31] & 0xc0 != 0x40:
        raise cv.Invalid("Use a clamped MeshCore private key, not seed || public key")
    return value


def room_private_key(value):
    value = cv.string_strict(value)
    if not is_secret(value) and not _restore_secret_reference(value):
        raise cv.Invalid("Private room identity keys must use !secret")
    return native_private_key(value)


def client_private_key(value):
    value = cv.string_strict(value)
    # Keep !secret redaction across remote package merges; public demo keys may be literals.
    if not is_secret(value):
        _restore_secret_reference(value)
    return native_private_key(value)


def room_storage(config):
    config["storage_id"] = room_id(config.get("storage_id", str(config[CONF_ID])))
    return config


ROOM_SCHEMA = cv.All(cv.Schema({
    cv.Required(CONF_ID): cv.All(channel_name, cv.declare_id(Room)),
    cv.Optional("storage_id"): room_id,
    cv.Required("name"): sender_name,
    cv.Optional("access", default="private"): cv.enum(ROOM_ACCESS, lower=True),
    cv.Optional("password"): room_password,
    cv.Optional("private_key"): room_private_key,
}), room_access, room_storage)


def recipient_key(value):
    value = cv.string_strict(value)
    if not re.fullmatch(r"[0-9a-fA-F]{64}", value) or value[:2].lower() in ("00", "ff"):
        raise cv.Invalid("Recipient must be a full 32-byte public key (64 hex characters), without reserved prefix")
    return value


CLIENT_SCHEMA = cv.Schema({
    cv.Required(CONF_ID): cv.All(channel_name, cv.declare_id(Client)),
    cv.Optional("name"): sender_name,
    cv.Required("private_key"): client_private_key,
    cv.Optional("peers", default=[]): cv.All(cv.ensure_list(recipient_key), cv.Length(max=8)),
    cv.Optional("on_message"): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ClientMessageTrigger),
    }),
})


CONFIG_SCHEMA = cv.All(cv.Schema({
    cv.GenerateID(): cv.declare_id(MeshCoreComponent),
    cv.Required("radio_id"): cv.use_id(sx126x.SX126x),
    cv.Required("time_id"): cv.use_id(time_.RealTimeClock),
    cv.Optional("schema_version", default=1): cv.one_of(1, int=True),
    cv.Optional("client_state_budget", default=16384): cv.int_range(min=0, max=1048576),
    cv.Optional("room_state_budget", default=65536): cv.int_range(min=0, max=1048576),
    cv.Optional("tx_queue_size", default=4): cv.int_range(min=1),
    cv.Optional("path_hash_size", default=2): cv.int_range(min=1, max=3),
    cv.Optional("channels", default=[]): cv.ensure_list(CHANNEL_SCHEMA),
    cv.Optional("clients", default=[]): cv.ensure_list(CLIENT_SCHEMA),
    cv.Optional("rooms", default=[]): cv.ensure_list(ROOM_SCHEMA),
}).extend(cv.COMPONENT_SCHEMA), cv.require_esphome_version(2026, 10, 1), unique_channels)


def client_senders(config, full):
    """One static sender service per private scalar, including keys used only by actions."""
    senders = list(config["clients"])
    scalars = {bytes.fromhex(sender["private_key"])[:32] for sender in senders}

    def collect(value):
        if isinstance(value, dict):
            for key, child in value.items():
                if key == "meshcore.send_client_message":
                    private_key = child["sender_private_key"]
                    scalar = bytes.fromhex(private_key)[:32]
                    if scalar not in scalars:
                        senders.append({CONF_ID: child["sender"], "private_key": private_key})
                        scalars.add(scalar)
                collect(child)
        elif isinstance(value, list):
            for child in value:
                collect(child)
    collect(full)
    return senders


def validate_radio_ownership(config):
    full = fv.full_config.get()
    senders = client_senders(config, full)
    def has_channel_send(value):
        if isinstance(value, dict):
            return "meshcore.send_channel_message" in value or any(has_channel_send(v) for v in value.values())
        if isinstance(value, list):
            return any(has_channel_send(v) for v in value)
        return False
    if not config["channels"] and not config["rooms"] and not senders and not has_channel_send(full):
        raise cv.Invalid("Configure a channel, room, client or send action")
    room_keys = {bytes.fromhex(room["private_key"])[:32] for room in config["rooms"] if "private_key" in room}
    if any(bytes.fromhex(sender["private_key"])[:32] in room_keys for sender in senders):
        raise cv.Invalid("Client send keys must not reuse a room identity")
    if full.get("esp32", {}).get("framework", {}).get("type") != "esp-idf":
        raise cv.Invalid("MeshCore's current storage/crypto adapters require ESP32 with ESP-IDF")
    radio = next(item for item in full["sx126x"] if item["id"] == config["radio_id"])
    if radio["modulation"] != "LORA" or radio["payload_length"] != 0 or not radio["rx_start"]:
        raise cv.Invalid("MeshCore requires LoRa, variable payload length, and rx_start: true")
    # A custom lambda can always bypass ownership; supported YAML actions must
    # not change modes or transmit behind the scheduler's back.
    def check(value):
        if isinstance(value, dict):
            for key, child in value.items():
                if isinstance(key, str) and key.startswith("sx126x."):
                    raise cv.Invalid("Raw sx126x actions cannot share the MeshCore-owned radio")
                check(child)
        elif isinstance(value, list):
            for child in value:
                check(child)
    check(full)
    return config


FINAL_VALIDATE_SCHEMA = validate_radio_ownership


async def to_code(config):
    # Single component per firmware. Capacity is fixed by the composed YAML,
    # including package merges, and is used only by the ESPHome adapter.
    senders = client_senders(config, CORE.config)
    cg.add_define("MESHCORE_CLIENT_COUNT", len(senders))
    cg.add_define("MESHCORE_CLIENT_STATE_BUDGET", config["client_state_budget"])
    cg.add_define("MESHCORE_ROOM_COUNT", len(config["rooms"]))
    cg.add_define("MESHCORE_CHANNEL_COUNT", len(config["channels"]))
    cg.add_define("MESHCORE_TX_QUEUE_SIZE", config["tx_queue_size"])
    cg.add_define("MESHCORE_ROOM_STATE_BUDGET", config["room_state_budget"])
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    radio = await cg.get_variable(config["radio_id"])
    cg.add(var.set_radio(radio))
    clock = await cg.get_variable(config["time_id"])
    cg.add(var.set_time_source(clock))
    cg.add(radio.register_listener(var))
    cg.add(var.set_path_hash_size(config["path_hash_size"]))
    for client in senders:
        resource = cg.new_Pvariable(client[CONF_ID], var)
        cg.add(var.add_client(resource, str(client[CONF_ID]), client.get("name", str(client[CONF_ID])),
                              list(bytes.fromhex(client["private_key"]))))
    for client in senders:
        resource = await cg.get_variable(client[CONF_ID])
        for peer in client.get("peers", []):
            cg.add(resource.add_peer(list(bytes.fromhex(peer))))
        for conf in client.get("on_message", []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], resource)
            await automation.build_automation(trigger, [(ClientMessage, "message")], conf)
    for room in config["rooms"]:
        cg.new_Pvariable(room[CONF_ID], var, room["storage_id"])
        if "private_key" in room:
            identity = room["private_key"]
            cg.add(var.add_room(room["storage_id"], room["name"], room["access"],
                                list(bytes.fromhex(identity)), room.get("password", "")))
        else:
            # Retain the separately provisioned path for existing deployments.
            cg.add(var.add_room(room["storage_id"], room["name"], room["access"]))
    for channel in config["channels"]:
        key = resolve_channel_key(channel)
        scope = channel.get("scope")
        scope_key = resolve_scope_key(scope)
        resource = cg.new_Pvariable(channel[CONF_ID])
        cg.add(var.add_channel(resource, str(channel[CONF_ID]), list(key), list(scope_key)))
        for conf in channel.get("on_message", []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], resource)
            await automation.build_automation(trigger, [(ChannelMessage, "message")], conf)


def message_text(value):
    value = cv.string_strict(value)
    if not 1 <= len(value.encode()) <= 160 or "\0" in value:
        raise cv.Invalid("Message must be 1-160 UTF-8 bytes without NUL; sender prefix also counts at send time")
    return value


def send_channel_message_length(config):
    if not isinstance(config["sender_name"], Lambda) and not isinstance(config["message"], Lambda):
        if len(config["sender_name"].encode()) + 2 + len(config["message"].encode()) > 160:
            raise cv.Invalid("Message and sender_name prefix must fit 160 UTF-8 bytes")
    return config


@automation.register_action("meshcore.send_channel_message", SendChannelMessageAction, cv.All(cv.Schema({
    cv.GenerateID("meshcore_id"): cv.use_id(MeshCoreComponent),
    cv.Optional("channel_id"): cv.use_id(Channel),
    cv.Optional("channel_key"): channel_key,
    cv.Required("sender_name"): cv.templatable(sender_name),
    cv.Required("message"): cv.templatable(message_text),
    cv.Required("timestamp"): cv.templatable(cv.int_range(min=1, max=0xFFFFFFFF)),
}), cv.has_exactly_one_key("channel_id", "channel_key"), send_channel_message_length), synchronous=True)
async def send_channel_message_to_code(config, action_id, template_arg, args):
    if "channel_id" in config:
        channel = await cg.get_variable(config["channel_id"])
        var = cg.new_Pvariable(action_id, template_arg, channel)
    else:
        parent = await cg.get_variable(config["meshcore_id"])
        var = cg.new_Pvariable(action_id, template_arg, parent, list(resolve_key(config["channel_key"])))
    for field in ("sender_name", "message"):
        cg.add(getattr(var, f"set_{field}")(await cg.templatable(config[field], args, ChannelText)))
    cg.add(var.set_timestamp(await cg.templatable(config["timestamp"], args, cg.uint32)))
    return var


def advertisement_scope(config):
    if config.get("scope") is not None and config["mode"] != "flood":
        raise cv.Invalid("Advertisement scope requires mode: flood")
    return config


@automation.register_action("meshcore.advertise_room", AdvertiseRoomAction, cv.All(cv.Schema({
    cv.Required("room_id"): cv.use_id(Room),
    cv.Optional("mode", default="direct"): cv.one_of("direct", "flood", lower=True),
    cv.Optional("scope"): cv.Any(None, scope_name),
}), advertisement_scope), synchronous=True)
async def advertise_room_to_code(config, action_id, template_arg, args):
    room = await cg.get_variable(config["room_id"])
    return cg.new_Pvariable(action_id, template_arg, room, config["mode"] == "flood",
                            list(resolve_scope_key(config.get("scope"))))


@automation.register_action("meshcore.send_client_message", SendClientMessageAction, cv.Schema({
    cv.GenerateID("sender"): cv.declare_id(Client),
    cv.Required("sender_private_key"): client_private_key,
    cv.Required("recipient_public_key"): cv.templatable(recipient_key),
    cv.Required("message"): cv.templatable(message_text),
    cv.Required("timestamp"): cv.templatable(cv.int_range(min=1, max=0xFFFFFFFF)),
}), synchronous=True)
async def send_client_message_to_code(config, action_id, template_arg, args):
    scalar = bytes.fromhex(config["sender_private_key"])[:32]
    sender_config = next(sender for sender in client_senders(CORE.config["meshcore"], CORE.config)
                         if bytes.fromhex(sender["private_key"])[:32] == scalar)
    sender = await cg.get_variable(sender_config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, sender)
    for field in ("message", "recipient_public_key"):
        value = await cg.templatable(config[field], args, ChannelText)
        cg.add(getattr(var, f"set_{field}")(value))
    cg.add(var.set_timestamp(await cg.templatable(config["timestamp"], args, cg.uint32)))
    return var


@automation.register_action("meshcore.advertise_client", AdvertiseClientAction, cv.All(cv.Schema({
    cv.Required("client_id"): cv.use_id(Client),
    cv.Required("timestamp"): cv.templatable(cv.int_range(min=1, max=0xFFFFFFFF)),
    cv.Optional("mode", default="direct"): cv.one_of("direct", "flood", lower=True),
    cv.Optional("scope"): cv.Any(None, scope_name),
}), advertisement_scope), synchronous=True)
async def advertise_client_to_code(config, action_id, template_arg, args):
    client = await cg.get_variable(config["client_id"])
    var = cg.new_Pvariable(action_id, template_arg, client, config["mode"] == "flood",
                           list(resolve_scope_key(config.get("scope"))))
    cg.add(var.set_timestamp(await cg.templatable(config["timestamp"], args, cg.uint32)))
    return var
