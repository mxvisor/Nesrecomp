"""Differential test: code emitted by tools/nesrecomp.py vs src/cpu_interp.c.

For every opcode in OPTABLE (a few operand variants each) the emitter produces
a one-instruction C function; tests/c/cpu_diff.c runs it and cpu_interp_step()
from the same random CPU/RAM states and compares registers, flags, RAM, cycles
and control-flow PC. The interpreter is the reference: it is what the demo-sync
work (tools/verify_all.sh) has validated against FCEUX.

Needs gcc; skipped otherwise. No SDL, no ROMs.
"""
import contextlib
import io
import os
import random
import shutil
import subprocess
import tempfile
import unittest

from helpers import ROOT, nesrecomp

VARIANTS = 4          # operand variants per opcode
ITERATIONS = 64       # random CPU/RAM states per variant
SLOT = 4              # bytes reserved per case in PRG
BRK_VECTOR = 0x9ABC

# Opcodes deliberately excluded from the comparison.
SKIP = {
    # STP/JAM halts real hardware; the emitter instead re-dispatches at the same
    # PC (bank-switch trick used by Battletoads), the interpreter skips 1 byte.
    "STP",
}

# Known differences, excluded from the default run: {opcode: reason}.
# Every entry must say why. When the interpreter is fixed, delete the entry.
KNOWN = {}

SRC = ["memory.c", "cpu_interp.c", "ppu.c", "apu.c", "apu_fceux.c", "mapper.c"]


def operand_for(op, rng, variant):
    m = op.mode
    if m == "izy" and variant == 1:
        return 0xFF                                  # pointer at $FF: wrap test
    if m in ("imm", "zp", "zpx", "zpy", "izx", "izy", "rel"):
        return rng.randrange(0x100)
    if m == "abs":
        if op.mnemonic in ("JMP", "JSR"):
            return rng.randrange(0x8000, 0x10000)
        return rng.randrange(0x0200, 0x0800)
    if m in ("abx", "aby"):
        return (rng.randrange(0x02, 0x07) << 8) | rng.randrange(0x100)
    if m == "ind":                                   # JMP ($nnnn) pointer in RAM
        return 0x02FF if variant == 0 else rng.randrange(0x0200, 0x0800)
    return 0


def build(tmp):
    rng = random.Random(1234)
    prg = bytearray([0xEA] * 0x8000)
    prg[0x7FFE:0x8000] = bytes([BRK_VECTOR & 0xFF, BRK_VECTOR >> 8])
    dis = nesrecomp.Disassembler(bytes(prg), 0, 2)
    cases = []
    pc = 0x8000
    for opc, op in sorted(nesrecomp.OPTABLE.items()):
        if op.mnemonic in SKIP or opc in KNOWN:
            continue
        for v in range(VARIANTS):
            operand = operand_for(op, rng, v)
            raw = [opc, operand & 0xFF, operand >> 8][:op.size]
            prg[pc - 0x8000:pc - 0x8000 + len(raw)] = bytes(raw)
            dis.functions[pc] = [(pc, op, operand)]
            cases.append((opc, op, pc))
            pc += SLOT
    assert pc < 0xFF00, "too many cases for the PRG image"

    with contextlib.redirect_stdout(io.StringIO()):
        nesrecomp.CEmitter(dis, {}).emit_full(tmp, "T")

    cf = {"JMP", "JSR", "RTS", "RTI", "BRK"}
    lines = ['#include "runner.h"']
    lines += [f"void func_{pc:04X}(void);" for _, _, pc in cases]
    lines.append("const uint8_t test_prg[32768] = {")
    for i in range(0, len(prg), 32):
        lines.append("  " + ",".join(str(b) for b in prg[i:i + 32]) + ",")
    lines.append("};")
    lines.append("typedef struct { uint8_t opcode; uint16_t pc; void (*fn)(void);"
                 " uint8_t indirect_zp, control_flow, branch, size; const char *name; }"
                 " diff_case_t;")
    lines.append("const diff_case_t test_cases[] = {")
    for opc, op, pc in cases:
        branch = op.mnemonic in nesrecomp.BRANCH_OPS
        lines.append(
            f'  {{0x{opc:02X}, 0x{pc:04X}, func_{pc:04X}, {("", "izx", "izy").index(op.mode) if op.mode in ("izx", "izy") else 0},'
            f' {int(branch or op.mnemonic in cf)}, {int(branch)}, {op.size},'
            f' "{op.mnemonic} {op.mode}"}},')
    lines.append("};")
    lines.append(f"const int test_case_count = {len(cases)};")
    with open(os.path.join(tmp, "cases.c"), "w") as f:
        f.write("\n".join(lines) + "\n")
    return len(cases)


@unittest.skipUnless(shutil.which("gcc"), "gcc not available")
class TestRecompiledVsInterpreter(unittest.TestCase):
    exe = None

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.n = build(cls.tmp)
        exe = os.path.join(cls.tmp, "cpu_diff")
        srcs = [os.path.join(ROOT, "src", s) for s in SRC]
        srcs += [os.path.join(cls.tmp, "T_full.c"), os.path.join(cls.tmp, "cases.c"),
                 os.path.join(ROOT, "tests", "c", "cpu_diff.c")]
        cls.cc = subprocess.run(
            ["gcc", "-O1", "-w", "-I", os.path.join(ROOT, "src", "include"),
             '-DGAME_NAME="T"', *srcs, "-o", exe, "-lm"],
            capture_output=True, text=True)
        if cls.cc.returncode == 0:
            cls.exe = exe

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_harness(self, *args):
        self.assertIsNotNone(self.exe, "build failed:\n" + self.cc.stderr[-4000:])
        return subprocess.run([self.exe, *args], capture_output=True, text=True)

    def test_every_opcode_matches_interpreter(self):
        run = self.run_harness(str(ITERATIONS))
        self.assertIn(f"cases={self.n}", run.stdout)
        self.assertEqual(run.returncode, 0,
                         "recompiled code differs from cpu_interp_step():\n" + run.stdout)

    def test_zero_page_pointer_wraps(self):
        """(zp,X) / (zp),Y with the pointer at $FF must fetch the high byte
        from $00 (hardware, FCEUX `GetIX`/`GetIY`, and the emitter's
        izx_addr/izy_addr). Regression test for cpu_interp.c rd16_zp()."""
        run = self.run_harness("4", "wrap")
        self.assertIn("(wrap mode)", run.stdout)
        self.assertEqual(run.returncode, 0, run.stdout)


if __name__ == "__main__":
    unittest.main()
