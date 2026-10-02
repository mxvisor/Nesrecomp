#!/usr/bin/env python3
"""Write a tiny test program as an iNES file (used by CI smoke builds).

    python3 tests/make_smoke_rom.py rom/CiSmoke.nes            # NROM
    python3 tests/make_smoke_rom.py rom/CiSmokeUnrom.nes --unrom

NROM: RESET enables NMI and spins incrementing $10; NMI increments $11. Enough
to drive the whole pipeline (embed -> discover -> compile) and run the binary
headless in every backend, without a commercial ROM.

UNROM (4 x 16 KB): the fixed bank selects bank 1 and jumps to $9000 through a
pointer built in RAM (JMP ($0000)) — static BFS cannot see that target, so it
is only found by learn mode, which must record it bank-qualified as
"extra_func = 1:9000". Banks 0 and 2 hold different code at $9000.
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

U_RESET, U_LOOP, U_NMI, U_BANK1 = 0xC000, 0xC00A, 0xC040, 0xC0FF
unrom_fixed = [
    0x78, 0xD8, 0xA2, 0xFF, 0x9A,            # SEI; CLD; LDX #$FF; TXS
    0xA9, 0x80, 0x8D, *w(0x2000),            # LDA #$80; STA $2000
    # U_LOOP ($C00A):
    0xA9, 0x01, 0x8D, *w(U_BANK1),           # LDA #1; STA $C0FF (holds $01: no bus conflict)
    0xA9, 0x00, 0x85, 0x00,                  # LDA #<$9000; STA $00
    0xA9, 0x90, 0x85, 0x01,                  # LDA #>$9000; STA $01
    0x6C, *w(0x0000),                        # JMP ($0000)  -> $9000 in bank 1
]
unrom_bank_code = {                          # $9000 in each switchable bank
    0: [0xE6, 0x12, 0x4C, *w(U_LOOP)],       # INC $12; JMP U_LOOP  (never selected)
    1: [0xE6, 0x10, 0x4C, *w(U_LOOP)],       # INC $10; JMP U_LOOP
    2: [0xE6, 0x13, 0x4C, *w(U_LOOP)],       # INC $13; JMP U_LOOP  (never selected)
}


def build_nrom():
    rb = RomBuilder()
    # $FF padding like most real carts. Do NOT use a 1-byte terminator (the
    # test default $02, or $00 = BRK): the orphan phase then turns every
    # padding byte into a function, one per pass — quadratic (see STATUS.md).
    rb.prg[:] = bytes([0xFF]) * len(rb.prg)
    return rb.put(RESET, program).put(NMI, nmi).vectors(RESET, nmi=NMI)


def build_unrom():
    rb = RomBuilder(mapper=2, prg_banks=4)
    rb.prg[:] = bytes([0xFF]) * len(rb.prg)
    rb.put(U_RESET, unrom_fixed).put(U_NMI, nmi).put(U_BANK1, [0x01])
    for bank, code in unrom_bank_code.items():
        rb.put_bank(bank, 0x9000, code)
    return rb.vectors(U_RESET, nmi=U_NMI)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    unrom = "--unrom" in sys.argv[1:]
    out = args[0] if args else ("rom/CiSmokeUnrom.nes" if unrom else "rom/CiSmoke.nes")
    rb = build_unrom() if unrom else build_nrom()
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "wb") as f:
        f.write(rb.image())
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
