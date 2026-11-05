"""Validate public examples with generated private inputs; optionally compile all three.

Uses a temporary consumer directory, never deployment secrets or a serial device.
"""
import argparse
import base64
import ctypes
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
from unittest.mock import patch

import yaml
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("example_secrets", root / "examples/generate_secrets.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compile", action="store_true")
args = parser.parse_args()

with patch.object(generator.secrets, "token_bytes", return_value=bytes([1]) * 32):
    private, public = generator.identity()
    assert public == Ed25519PrivateKey.from_private_bytes(bytes([1]) * 32).public_key().public_bytes_raw().hex()
    assert len(bytes.fromhex(private)) == 64

with tempfile.TemporaryDirectory(prefix="meshcore-examples-") as directory:
    consumer = Path(directory)
    # Check the literal example pairs with the same native-key implementation used by firmware.
    demo = yaml.load((root / "examples/heltec-v3-two-clients.yaml").read_text(), Loader=yaml.BaseLoader)
    library = consumer / "demo-ed25519.so"
    subprocess.run([os.environ.get("CC", "cc"), "-shared", "-fPIC", "-O2",
                    *map(str, sorted((root / "components/meshcore_ed25519").glob("*.c"))),
                    "-o", str(library)], check=True, capture_output=True)
    native_crypto = ctypes.CDLL(str(library))
    native_crypto.ed25519_derive_pub.argtypes = [ctypes.POINTER(ctypes.c_ubyte), ctypes.POINTER(ctypes.c_ubyte)]
    native_crypto.ed25519_derive_pub.restype = None
    for name, prefix in (("alice", "a11ce"), ("bob", "b0b")):
        private = bytes.fromhex(demo["substitutions"][f"{name}_private_key"])
        public = demo["substitutions"][f"{name}_public_key"]
        assert len(private) == 64 and len(public) == 64 and public.startswith(prefix)
        assert private[0] & 7 == 0 and private[31] & 0xc0 == 0x40
        derived = (ctypes.c_ubyte * 32)()
        native_crypto.ed25519_derive_pub(derived, (ctypes.c_ubyte * 64).from_buffer_copy(private))
        assert bytes(derived).hex() == public, f"{name} demo key pair mismatch"
    print("PASS: Alice/Bob vanity prefixes and native private/public key agreement", flush=True)

    secrets_file = consumer / "secrets.yaml"
    generator.generate(secrets_file)
    initial = secrets_file.read_bytes()
    assert secrets_file.stat().st_mode & 0o777 == 0o600
    for destination in (secrets_file, consumer / "symlink.yaml"):
        if destination != secrets_file:
            destination.symlink_to(secrets_file)
        try:
            generator.generate(destination)
            raise AssertionError("Must not replace existing files or follow symlinks")
        except FileExistsError:
            pass
    assert secrets_file.read_bytes() == initial
    values = yaml.safe_load(initial)
    keys = [value for key, value in values.items() if key.endswith("private_key")]
    assert len(keys) == len(set(keys)) == 3
    assert len(base64.b64decode(values["meshcore_private_channel_key"])) == 16
    for key in keys:
        native = bytes.fromhex(key)
        assert len(native) == 64 and native[0] & 7 == 0 and native[31] & 0xc0 == 0x40
    secrets_file.write_text(yaml.safe_dump(values))
    for name in ("heltec-v3-three-rooms", "heltec-v3-ping-bot", "heltec-v3-two-clients"):
        text = (root / f"examples/{name}.yaml").read_text()
        # Execute the published files from outside the repository with explicit source paths.
        text = text.replace("path: ../components", f"path: {root / 'components'}")
        config = consumer / f"{name}.yaml"
        config.write_text(text)
        if name == "heltec-v3-two-clients":
            # This example must work without any generated or user-provided secrets.
            secrets_file.unlink()
        for command in ("config", "compile") if args.compile else ("config",):
            result = subprocess.run(["esphome", command, str(config)], cwd=consumer, capture_output=True, text=True,
                                    env=dict(os.environ, ESPHOME_DATA_DIR=str(root / "local/.esphome")))
            log = root / f"build/example-{name}-{command}.log"
            log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text(result.stdout + result.stderr)
            assert result.returncode == 0, f"{name} {command} failed; see {log}"
            assert not any(key in result.stdout + result.stderr for key in keys), "Private key exposed in output"
            print(f"PASS: {name} {command}", flush=True)
    print("PASS: independent example keys, file permissions, overwrite/symlink protection and redaction", flush=True)
