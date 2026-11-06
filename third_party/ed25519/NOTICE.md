# Ed25519 provenance

Copied from `meshcore-dev/MeshCore` revision
`81dbae592ffad40b4212bc0b37e82b38da08f4bc`, directory `lib/ed25519/`.
Original files and Git blob hashes are recorded in `upstream.json`; the original
orlp license is retained in `LICENSE`. Checked 2026-10-31.

This component includes key generation, Ed25519 signing, and the upstream
Ed25519-to-Montgomery key exchange used by MeshCore. It excludes `seed.c`
(host-specific entropy), `add_scalar.c` (unused), and `verify.c` (unused here and
disabled in the inspected upstream Identity wrapper). Host-side contact
verification uses Python cryptography's independent Ed25519 implementation.

Local modifications:

- Flat ESPHome component registration in `__init__.py`.
- `fe.c` and `sc.c`: replace signed carry left shifts with multiplication by
  `INT64_C(1) << bits`; negative signed shifts triggered UndefinedBehaviorSanitizer.
- `fe.c`: cast serialization limbs to `uint32_t` before left shifts, defining
  truncation instead of overflowing signed integers.
- `ge.c`: replace a potentially negative signed left shift by multiplication by 2.

No algorithm or wire format is intentionally changed. Tests run the actual
vendored files under ASan/UBSan and compare 64 deterministic key/signature cases
with OpenSSL, plus independent Python public-key, shared-secret, login, and
advertisement fixtures. This is compatibility evidence, not a cryptographic audit.
