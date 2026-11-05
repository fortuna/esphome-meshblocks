# Contributing

Read AGENTS.md and docs/design.md. This checkout must remain self-contained.

| Directory | Responsibility |
| --- | --- |
| components/meshcore_core | Portable protocol, client/room services and scheduling |
| components/meshcore | ESPHome composition, identity, storage and radio adapters |
| components/meshcore_ed25519 | Pinned cryptography and license notices |
| examples | Flashable room/channel-bot/two-client YAML and an exclusive private-build-input generator |
| tests | Internal regression fixtures, host checks and isolated consumer test |
| scripts | C++ style tooling |
| docs | Supported configuration, design and style contracts |

Device-specific firmware, UI, serial/provisioning workflows, power policies and research
belong to external consumers. Internal YAML fixtures are regression inputs with
synthetic secrets, not device configurations to flash.

Use the pinned [toolchain](docs/esphome-manual.md#toolchain). Host C++ tests need
Clang and OpenSSL (openssl@3 on Homebrew or OPENSSL_PREFIX). Formatting/semantic
checks use LLVM 21.1.8 with matching Python bindings; see docs/cpp-style.md.
Run from this repository with the ESPHome environment activated:

```sh
sh tests/run_host_tests.sh
python tests/test_config.py
python tests/test_examples.py --compile
python tests/test_external_consumer.py --compile
python3 scripts/cpp_style.py
python3 tests/test_cpp_style.py
python3 scripts/cpp_style.py --firmware-build local/.esphome/build/meshcore-external-consumer
```

The consumer test copies only components and third-party notices into a
temporary dependency, validates local and Git sources, and optionally compiles
the pinned Git version. It cannot access prototype components. It writes ignored
logs under build/ and uses the normal dependency cache under local/.esphome.
The temporary Git source is not a published release.

For changes involving room composition, also build the appropriate synthetic
static-room fixture; for application changes, build the external application.
Keep credentials and firmware out of source control. Update STATUS, LOG, design
and manual together when contracts change. Preserve NVS formats and identities.
Record actual test coverage and keep build evidence separate from flash/RF claims.
