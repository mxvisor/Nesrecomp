"""Shared helpers for the tools/nesrecomp.py test suite.

Tests build tiny synthetic iNES images in memory — no commercial ROMs needed.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

import nesrecomp  # noqa: E402  (path set up above)

# Filler byte for unused PRG space. Every opcode decodes (OPTABLE covers all
# 256), so pick STP ($02): a 1-byte terminator keeps stray linear decoding short.
FILL = 0x02

# Opcodes used to hand-assemble test programs.
LDA_IMM, LDA_ABS, LDA_ABX, STA_ABS, STA_ABX = 0xA9, 0xAD, 0xBD, 0x8D, 0x9D
JSR, JMP_ABS, JMP_IND, RTS, RTI, NOP = 0x20, 0x4C, 0x6C, 0x60, 0x40, 0xEA
BEQ, BNE = 0xF0, 0xD0


def lo(v):
    return v & 0xFF


def hi(v):
    return (v >> 8) & 0xFF


def w(v):
    """Little-endian 16-bit word as a list of bytes."""
    return [lo(v), hi(v)]


class RomBuilder:
    """Assemble an iNES image by poking bytes at CPU or bank addresses.

    NROM (mapper 0) with 2 PRG banks maps PRG linearly at $8000-$FFFF.
    For banked mappers use put_bank(): UNROM banks are 16 KB at $8000
    (last bank fixed at $C000); AxROM banks are 32 KB at $8000.
    """

    def __init__(self, mapper=0, prg_banks=2, chr_banks=1, battery=False):
        self.mapper = mapper
        self.prg_banks = prg_banks
        self.chr_banks = chr_banks
        self.battery = battery
        self.prg = bytearray([FILL] * (prg_banks * 0x4000))

    # -- placement ---------------------------------------------------------
    def put(self, addr, data):
        """Write bytes at a CPU address in the default (NROM/fixed) mapping."""
        if self.mapper in (1, 2):
            if addr < 0xC000:
                raise ValueError("switchable window: use put_bank()")
            off = (self.prg_banks - 1) * 0x4000 + (addr - 0xC000)
        elif self.mapper == 7:
            off = addr - 0x8000                      # bank 0
        else:
            off = (addr - 0x8000) % len(self.prg)
        self.prg[off:off + len(data)] = bytes(data)
        return self

    def put_bank(self, bank, addr, data):
        if self.mapper == 2:
            off = bank * 0x4000 + (addr - 0x8000)
        elif self.mapper == 7:
            off = bank * 0x8000 + (addr - 0x8000)
        else:
            raise ValueError("put_bank supports mappers 2 and 7")
        self.prg[off:off + len(data)] = bytes(data)
        return self

    def vectors(self, reset, nmi=None, irq=None, bank=None):
        nmi = reset if nmi is None else nmi
        irq = reset if irq is None else irq
        data = w(nmi) + w(reset) + w(irq)
        if bank is None:
            return self.put(0xFFFA, data)
        return self.put_bank(bank, 0xFFFA, data)

    # -- output --------------------------------------------------------------
    def image(self):
        flags6 = ((self.mapper & 0x0F) << 4) | (0x02 if self.battery else 0)
        flags7 = self.mapper & 0xF0
        header = bytes([0x4E, 0x45, 0x53, 0x1A, self.prg_banks, self.chr_banks,
                        flags6, flags7]) + bytes(8)
        return header + bytes(self.prg) + bytes(self.chr_banks * 0x2000)

    def disassembler(self, orphan_window=0):
        hdr, prg, _ = nesrecomp.parse_ines(self.image())
        dis = nesrecomp.Disassembler(prg, hdr.mapper, hdr.prg_banks)
        dis.orphan_window = orphan_window
        return dis


def discover(rb, orphan_window=0, extra=(), inline_data=(), data_regions=()):
    """Run discovery from the ROM's vectors; return the Disassembler."""
    dis = rb.disassembler(orphan_window)
    for a in extra:
        dis.extra_funcs.add(a)
    for a in inline_data:
        dis.inline_data_funcs.add(a)
    for start, end in data_regions:
        dis.data_regions.add(range(start, end + 1))
    seeds = [dis.read_vector(v) for v in (0xFFFC, 0xFFFA, 0xFFFE)]
    dis.discover(seeds)
    return dis


def emit(dis, tmpdir, game="T"):
    """Emit C for a Disassembler; return (full_c, dispatch_c) as strings."""
    import contextlib
    import io
    with contextlib.redirect_stdout(io.StringIO()):
        nesrecomp.CEmitter(dis, {}).emit_full(tmpdir, game)
    with open(os.path.join(tmpdir, f"{game}_full.c")) as f:
        full = f.read()
    with open(os.path.join(tmpdir, f"{game}_dispatch.c")) as f:
        dispatch = f.read()
    return full, dispatch


def function_body(full_c, name):
    """Return the text of `void name(void) { ... }` from generated C."""
    start = full_c.index(f"void {name}(void) {{")
    end = full_c.index("\n}\n", start)
    return full_c[start:end + 2]
