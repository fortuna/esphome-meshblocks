"""Validate a relocated local checkout and a pinned Git consumer with no prototype files.

The Git source is a temporary local repository, not a claim that this project is
published. --compile also builds that Git consumer. Only synthetic keys are used.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compile", action="store_true")
args = parser.parse_args()
env = dict(os.environ, ESPHOME_DATA_DIR=str(root / "local/.esphome"))


def run(command, cwd, log=None):
    result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True)
    if log:
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"Failed: {command}; details: {log}" if log else result.stderr)
    return result.stdout.strip()


with tempfile.TemporaryDirectory(prefix="meshcore-consumer-") as directory:
    base = Path(directory)
    dependency, consumer = base / "dependency", base / "consumer"
    dependency.mkdir()
    consumer.mkdir()
    # Whitelist the distributable inputs; no local/, firmware, UI, tests or parent workspace.
    for name in ("components", "third_party"):
        shutil.copytree(root / name, dependency / name, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    run(["git", "init", "-q"], dependency)
    run(["git", "add", "."], dependency)
    run(["git", "-c", "user.name=Consumer test", "-c", "user.email=test@example.invalid",
         "commit", "-qm", "Synthetic consumer fixture"], dependency)
    revision = run(["git", "rev-parse", "HEAD"], dependency)
    url = dependency.as_uri()
    key = bytearray(hashlib.sha512(bytes([7]) * 32).digest())
    key[0] &= 248
    key[31] = (key[31] & 63) | 64
    (consumer / "secrets.yaml").write_text(f'client_key: "{key.hex()}"\n')
    for mode in ("local", "git"):
        packages = """  board: github://fortuna/ESPHomeCollection/boards/heltec-wifi-lora-32-v3/board.yaml@71d63352dcb09a396d1cbfe2dc28c3210d8d436e"""
        source = "type: local\n      path: ../dependency/components" if mode == "local" else f"type: git\n      url: {url}\n      ref: {revision}"
        config = consumer / "node.yaml"
        config.write_text(f"""esphome:
  name: meshcore-external-consumer
  min_version: 2026.11.0-dev
packages:
{packages}
external_components:
  - source: github://pr#22197
    components: [sx126x]
  - source:
      {source}
    refresh: never
    components: [meshcore, meshcore_core, meshcore_ed25519]
logger:
time:
  - platform: template
    id: clock_source
    timezone: UTC0
    lambda: return 1800000000;
sx126x:
  - id: !extend lora_radio
    modulation: LORA
    crc_enable: true
    payload_length: 0
    sync_value: [0x14, 0x24]
    rx_start: true
    pa_power: 22
    frequency: 910525000
    bandwidth: 62_5kHz
    spreading_factor: 7
    coding_rate: CR_4_5
    preamble_size: 32
meshcore:
  id: node
  radio_id: lora_radio
  time_id: clock_source
  channels:
    - id: testing
      key: "#testing"
script:
  - id: send_test
    then:
      - meshcore.send_channel_message:
          channel_id: testing
          sender_name: Automation
          message: Consumer test
          timestamp: 1800000000
      - meshcore.send_channel_message:
          channel_key: "#send-only"
          sender_name: Automation
          message: Send only
          timestamp: 1800000000
      - meshcore.send_client_message:
          sender_private_key: !secret client_key
          recipient_public_key: "1212121212121212121212121212121212121212121212121212121212121212"
          message: Key-only sender
          timestamp: 1800000001
      - meshcore.send_client_message:
          sender_private_key: !secret client_key
          recipient_public_key: "1313131313131313131313131313131313131313131313131313131313131313"
          message: Same sender state
          timestamp: 1800000002
""")
        log = root / f"build/external-consumer-{mode}.log"
        run(["esphome", "config", str(config)], consumer, log)
        print(f"PASS: isolated {mode} dependency, direct upstream board/driver and component-only loading", flush=True)
        if mode == "git" and args.compile:
            run(["esphome", "compile", str(config)], consumer, root / "build/external-consumer-compile.log")
            defines = (root / "local/.esphome/build/meshcore-external-consumer/src/esphome/core/defines.h").read_text()
            assert "#define MESHCORE_CLIENT_COUNT 1" in defines, "Repeated send keys must share one sender service"
            print("PASS: key-only actions share one bounded sender service", flush=True)
            print("PASS: pinned Git consumer firmware compiled without prototype components", flush=True)
            source = config.read_text()
            source = source.replace('  channels:\n    - id: testing\n      key: "#testing"\n', '')
            start = source.index('      - meshcore.send_channel_message:\n          channel_id: testing')
            end = source.index('      - meshcore.send_channel_message:\n          channel_key:', start)
            source = source[:start] + source[end:]
            start = source.index('      - meshcore.send_client_message:')
            source = source[:start]
            config.write_text(source)
            run(["esphome", "compile", str(config)], consumer, root / "build/external-consumer-send-only-compile.log")
            defines = (root / "local/.esphome/build/meshcore-external-consumer/src/esphome/core/defines.h").read_text()
            assert "#define MESHCORE_CHANNEL_COUNT 0" in defines and "#define MESHCORE_CLIENT_COUNT 0" in defines
            print("PASS: channel-key-only firmware compiled without subscriptions", flush=True)
