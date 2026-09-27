# 0002 — GPL FCEUX oracle stays out of the tree

- Status: Accepted
- Date: 2026-07-23 (recorded retroactively 2026-09-27)

## Context
The vendored FCEUX sources (`x6502_vendor.c`, `x6502_ops.inc`, `ppu_vendor.c`,
`apu_vendor.c`) are GPL, but are valuable as a cycle-exact differential oracle.

## Decision
Keep them in a gitignored `nogpl/` directory. The Makefile auto-detects them
(`VENDOR_SRCS`); when present, the `INTERP=1` build gets `-DHAVE_VENDOR` and
`--interp=fceux_vendor`. When absent, the build is GPL-free and the flag exits with an error.

## Rejected alternatives
Committing the vendored files to `src/` — would make the repository GPL.

## Consequences
Never commit anything from `nogpl/`. The non-GPL `--interp=fceux` must reach
parity by re-implementation, verified against the local oracle.
See [`../investigations/fceux-backend.md`](../investigations/fceux-backend.md).
