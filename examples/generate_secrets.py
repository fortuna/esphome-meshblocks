#!/usr/bin/env python3
"""Create private build inputs for the flashable examples; never replace a file."""
import argparse
import base64
import hashlib
import os
from pathlib import Path
import secrets

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


def identity():
    while True:
        seed = secrets.token_bytes(32)
        public = Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes_raw()
        if public[0] not in (0, 255):
            expanded = bytearray(hashlib.sha512(seed).digest())
            expanded[0] &= 248
            expanded[31] = (expanded[31] & 63) | 64
            return expanded.hex(), public.hex()


def generate(path):
    values = {}
    for name in ("family_room", "building_room", "public_room"):
        private, public = identity()
        values[f"{name}_private_key"] = private
        values[f"{name}_public_key"] = public
    for name in ("family_room_password", "building_room_password"):
        values[name] = secrets.token_urlsafe(10)
    values["meshcore_private_channel_key"] = base64.b64encode(secrets.token_bytes(16)).decode()
    path.parent.mkdir(parents=True, exist_ok=True)
    with os.fdopen(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w") as output:
        output.write("# Private build inputs. Keep a secure backup; do not commit or distribute.\n")
        for name, value in values.items():
            output.write(f'{name}: "{value}"\n')
        output.flush()
        os.fsync(output.fileno())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="new secrets.yaml next to your example YAML")
    args = parser.parse_args()
    try:
        generate(args.output)
    except FileExistsError:
        parser.error("Output exists; preserve its identities and merge missing values manually")
    print(f"Created {args.output}; secret values were not printed.")


if __name__ == "__main__":
    main()
