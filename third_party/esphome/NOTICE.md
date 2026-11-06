# ESPHome SX126x driver provenance

## Development style tooling, 2026-11-03

`.clang-format` is copied from `fortuna/esphome` commit
[`9b0ea38449bd188d4e2a30e4b4100b43a3048b9d`](https://github.com/fortuna/esphome/commit/9b0ea38449bd188d4e2a30e4b4100b43a3048b9d).
The ESP32 Clang-analysis flags in `scripts/cpp_style.py` follow that revision's
`script/clang-tidy`. Both non-runtime sources fall under the upstream MIT terms
and Copyright (c) 2019 ESPHome notice in the accompanying `LICENSE`. This
tooling provenance does not change the firmware dependency pin below. The local
style policy overrides the upstream preference for protected fields.

## Current upstream PR dependency, 2026-11-05

The SX126x external component follows `github://pr#22197` with ESPHome's default
one-day cache refresh. Its head checked on 2026-11-05 is `fortuna/esphome` commit
[`75a730821a9c5ea0efc3c5a8828e4687684d1311`](https://github.com/fortuna/esphome/commit/75a730821a9c5ea0efc3c5a8828e4687684d1311),
from branch `refactor/sx126x-radio-lifecycle`, reviewed in
[PR #10](https://github.com/fortuna/esphome/pull/10) and upstream
[PR #22197](https://github.com/esphome/esphome/pull/22197). The commit is based on
upstream `dev` `940f87056d692a6ee87c2aa3d526087842e20ee0` and reports
ESPHome `2026.11.0-dev`. The PR source lives in
each example's `external_components` declaration; `requirements.txt` was removed on 2026-11-03.
The selected core is official upstream ESPHome at
`940f87056d692a6ee87c2aa3d526087842e20ee0`, installed in the dev venv.
It supplies the power components directly; the standard V3/headless builds
pass without local compatibility patches.
A local release-based candidate, `a7b95c60177475a5e1a4fe36243b95e6e72469cf`,
is based directly on official ESPHome 2026.10.1
`4ea26ed65fefaf6ec9d1347285253801db096ee5`. It is not yet pushed or selected by
those declarations. See the contribution record for its host/build evidence and
the historical V3 power-management release-compatibility checks.

No driver source is vendored here anymore. Firmware and examples share the
external component selected independently by the consuming application; the MeshCore
adapter uses its per-operation listener API and checks the synchronous
cancellation completion for restoration failure. The fork owns driver tests;
this repository owns adapter/portable-service tests. This remains a fork review
contribution, not an accepted upstream ESPHome release.

The accompanying upstream `LICENSE` is retained: C++ runtime files are GPLv3,
Python and other files are MIT. Hardware evidence and remaining limits are in
[the contribution record](../../docs/sx126x-upstream.md).

## Local external power-component trial, 2026-11-03

The 2026.10.2 venv trial uses local copies of only `power_management` and
`esp32_power_management` from upstream commit
`1c8808339aab2e77d5f4d6225cf93aee54015bcc` (PR #12325). The copy is ignored under
`local/external-power-2026.10.2/`, with the original license and provenance.
Two Python compatibility changes define the Zigbee domain locally and use the
literal ESP32 configuration key; C++ is unchanged. This local build experiment
has not been published/integrated/flashed. It does not change the shared SX126x
pin or imply a full-fork installation. See the dated build record.

## Retired local patch, 2026-10-31

The former `components/sx126x/` was based on ESPHome 2026.9.1, commit
[`0b2b16e664ecfc0add37f3435db1c2140b8a687e`](https://github.com/esphome/esphome/tree/0b2b16e664ecfc0add37f3435db1c2140b8a687e/esphome/components/sx126x).
The five original files exactly matched the locally installed ESPHome baseline.
Their original SHA-256 hashes are recorded in `upstream.json`.

The accompanying upstream `LICENSE` applies: C++ runtime files are GPLv3,
Python and other files are MIT. Preserve these notices and the full license.
This is a local patch, not an upstream ESPHome release or accepted contribution.

Changes dated 2026-10-31 affect `sx126x.h` and `sx126x.cpp`:

- Exclusive asynchronous transmitter registration; legacy blocking send rejects
  calls when that owner is registered.
- Start TX without waiting for airtime; TX-done/timeout IRQs complete in the
  component loop after restoring the receive payload limit and receive mode.
- Drain an already-completed RX packet before TX and clear stale IRQs.
- Hardware timeout in 15.625-us ticks, plus the portable scheduler's deadline.
- Keep the driver loop enabled during TX to tolerate a missed interrupt edge.
- Cancel TX safely, suppress late completion events, and propagate BUSY failures.
- Reserve the RX buffer once and stop issuing SPI commands after BUSY failure.

The vendor [SX126x driver](https://github.com/Lora-net/LoRaMac-node/blob/master/src/radio/sx126x/sx126x.c)
and Semtech's SX1261/2 data sheet describe the SetTx timeout and separate TX-done
and timeout IRQs. No vendor code from that driver is copied here.

That local override and its original chip-simulation test were removed when the
fork dependency was adopted. `upstream.json` preserves the original baseline
hashes as historical provenance; they do not describe the current fork revision.
