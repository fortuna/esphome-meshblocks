# Reusable MeshCore components for ESPHome

This repository must be usable without its parent workspace or the prototype.
Read docs/design.md before implementation and keep it aligned with architecture,
configuration, ownership and persistence. Keep docs/esphome-manual.md aligned with
supported configuration/actions. STATUS.md records current work; LOG.md records
material changes and validation. Distinguish built, flashed and peer-observed.

The deliberate public scope is portable MeshCore services, ESP32/ESP-IDF/SX126x
adapters, static YAML configuration, typed actions, durable room state, plus three
basic flashable examples: rooms, a channel bot and two local clients.
Keep examples self-contained and validate/build their actual YAML. They illustrate
static provisioning, advertisements, channel replies and local/remote client messaging
using standard ESPHome features. The small examples key generator only prepares
private room/channel build inputs. The two-client example intentionally embeds
public demo credentials; client literals are supported, while real identities
should use !secret. Device-specific application firmware, UI/assets, USB console
and provisioning workflows, manual clock and experimental sleep policy belong to
external consumers. Research is not
a public API. Do not reintroduce dependencies on prototype or parent paths.

Preserve the deployed NVS formats and storage_id semantics. Static native keys and
passwords initialize new rooms; existing legacy records still load. Narrow typed
legacy operations remain for consumers, without USB grammar in the library. Never
rotate, replace or log credentials implicitly. The runtime must work without USB,
UI, Wi-Fi, Home Assistant or a host. Keep secrets/build artifacts out of Git.

One owner controls the radio, shared queue and bounded runtime. No global identity
swapping, per-room threads or hidden mutable protocol state. Full public identities
key state; prefixes are routing hints. Keep wire/crypto compatibility and tests.
No SDK headers in meshcore_core. Classes own private state, no protected fields,
explicit this-> for non-static access. Follow docs/cpp-style.md and narrow behavior
interfaces. Read-only observers do not expose mutable service internals.

No board or radio packages are distributed. Consumers import board definitions
directly from their upstream source or define wiring themselves, select the
SX126x external component independently and own radio framing explicitly.
The removed meshcore-lora application package must not return.
Consumers explicitly select RF profile, time source, service configuration and
application behavior. Declare list components with stable IDs and use !extend.
Do not hide a checkout-relative external_components source in a shared package:
that path is relative to the consumer's top-level YAML. Git users select their
board, driver and component sources independently; the examples follow the open
SX126x PR as described below. Publication remains a separate action.

Use the pinned ESPHome 2026.11.0-dev core at
940f87056d692a6ee87c2aa3d526087842e20ee0 and independent SX126x source
`github://pr#22197` declared in the examples. This follows the open upstream PR
with the default daily refresh; its checked head on 2026-11-05 is
75a730821a9c5ea0efc3c5a8828e4687684d1311. Verified
commands are in CONTRIBUTING.md.
Run relevant host/schema tests, the isolated consumer test and an actual firmware
build for adapter changes. Run python3 scripts/cpp_style.py; use --firmware-build
for complete platform coverage. Compilation is not RF compatibility or a flash.
