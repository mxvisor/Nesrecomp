#!/usr/bin/env python3
"""Write a tiny NROM test program as an iNES file (used by CI smoke builds).

    python3 tests/make_smoke_rom.py rom/CiSmoke.nes

RESET enables NMI and spins incrementing $10; NMI increments $11. Enough to
drive the whole pipeline (embed -> discover -> compile) and run the binary
headless in every backend, without a commercial ROM.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from helpers import RomBuilder, w  # noqa: E402

RESET, LOOP, NMI = 0x8000, 0x800A, 0x8020

program = [
    0x78,              # SEI
    0xD8,              # CLD
    0xA2, 0xFF,        # LDX #$FF
    0x9A,              # TXS
    0xA9, 0x80,        # LDA #$80
    0x8D, *w(0x2000),  # STA $2000   ; NMI on
    # LOOP:
    0xE6, 0x10,        # INC $10
    0x4C, *w(LOOP),    # JMP LOOP
]
nmi = [
    0xE6, 0x11,        # INC $11
    0x40,              # RTI
]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "rom/CiSmoke.nes"
    rb = RomBuilder()
    # $FF padding like most real carts. Do NOT use a 1-byte terminator (the
    # test default $02, or $00 = BRK): the orphan phase then turns every
    # padding byte into a function, one per pass — quadratic (see STATUS.md).
    rb.prg[:] = bytes([0xFF]) * len(rb.prg)
    rb.put(RESET, program).put(NMI, nmi).vectors(RESET, nmi=NMI)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "wb") as f:
        f.write(rb.image())
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
