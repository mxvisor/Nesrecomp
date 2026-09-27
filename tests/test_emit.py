"""C emission, per-bank recompilation and the CLI of tools/nesrecomp.py."""
import contextlib
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

from helpers import (BEQ, JSR, LDA_ABS, LDA_ABX, LDA_IMM, NOP, ROOT, RTI, RTS,
                     STA_ABS, STA_ABX, RomBuilder, discover, emit,
                     function_body, nesrecomp, w)


def quiet(fn, *a, **kw):
    with contextlib.redirect_stdout(io.StringIO()):
        return fn(*a, **kw)


class EmitCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)

    def gen(self, code, **kw):
        rb = RomBuilder().put(0x8000, code).vectors(0x8000)
        dis = quiet(discover, rb, **kw)
        full, dispatch = emit(dis, self.tmp)
        return full, dispatch, function_body(full, "func_8000")


class TestCycles(EmitCase):
    def test_page_cross_penalty_on_indexed_load(self):
        _, _, body = self.gen([LDA_ABX, 0xF0, 0x02, RTS])
        self.assertIn("g_cpu_cycles += 4;", body)
        self.assertIn("if ((0xF0 + cpu.X) > 0xFF) g_cpu_cycles++;", body)

    def test_no_page_cross_penalty_on_indexed_store(self):
        # STA abs,X always takes 5 cycles; the penalty is in the base count.
        _, _, body = self.gen([STA_ABX, 0xF0, 0x02, RTS])
        self.assertIn("g_cpu_cycles += 5;", body)
        self.assertNotIn("g_cpu_cycles++", body)

    def test_branch_taken_and_page_cross_penalties(self):
        _, _, body = self.gen([BEQ, 0x7F, RTS])      # $8002 + $7F = $8081
        self.assertIn("g_cpu_cycles += 1;", body)    # taken
        self.assertIn("(0x8002u ^ 0x8081u) & 0xFF00u", body)
        self.assertIn("cpu.PC = 0x8081; return;", body)

    def test_cycle_table_matches_interpreter_base_table(self):
        """OPTABLE base cycles must equal cpu_base_cycles[] in cpu_interp.c
        (which is verified identical to FCEUX CycTable)."""
        with open(os.path.join(ROOT, "src", "cpu_interp.c")) as f:
            src = f.read()
        m = re.search(r"cpu_base_cycles\[256\]\s*=\s*\{(.*?)\};", src, re.S)
        self.assertIsNotNone(m, "cpu_base_cycles[256] table not found")
        body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
        table = [int(x, 0) for x in re.findall(r"0x[0-9A-Fa-f]+|\d+", body)]
        self.assertEqual(len(table), 256)
        diffs = {f"${opc:02X} {op.mnemonic} {op.mode}": (op.cycles, table[opc])
                 for opc, op in nesrecomp.OPTABLE.items()
                 if op.mnemonic != "STP" and op.cycles != table[opc]}
        self.assertEqual(diffs, {}, "OPTABLE vs cpu_base_cycles (emitter, interp)")


class TestStructure(EmitCase):
    def test_pc_set_only_at_function_entry(self):
        _, _, body = self.gen([NOP, LDA_IMM, 1, NOP, RTS])
        self.assertEqual(body.count("cpu.PC = 0x8000;"), 1)
        self.assertEqual(len(re.findall(r"cpu\.PC = 0x80", body)), 1)

    def test_control_flow_yields(self):
        _, _, body = self.gen([JSR, *w(0x9000), RTS])
        self.assertIn("cpu.PC = 0x9000;", body)
        self.assertIn("return;", body)

    def test_tick_before_ppu_register_access(self):
        _, _, body = self.gen([LDA_ABS, *w(0x2002), RTS])
        self.assertIn("tick_ppu_apu();", body)

    def test_tick_before_mapper_write(self):
        _, _, body = self.gen([STA_ABS, *w(0x8000), RTS])
        self.assertIn("tick_ppu_apu();", body)

    def test_no_tick_for_plain_ram(self):
        _, _, body = self.gen([STA_ABS, *w(0x0300), LDA_ABS, *w(0x0300), RTS])
        self.assertNotIn("tick_ppu_apu();", body)

    def test_tick_when_indexed_range_reaches_io(self):
        # $1FF0,X can reach $2000-$20EF.
        _, _, body = self.gen([LDA_ABX, *w(0x1FF0), RTS])
        self.assertIn("tick_ppu_apu();", body)

    def test_every_opcode_has_an_emission(self):
        unhandled = []
        for opc, op in sorted(nesrecomp.OPTABLE.items()):
            operand = 0x0300 if op.size == 3 else 0x10
            lines = nesrecomp.emit_instruction(op, operand, 0x8000, set())
            if any("UNHANDLED" in s for s in lines):
                unhandled.append(f"${opc:02X} {op.mnemonic}")
        self.assertEqual(unhandled, [])


class TestDispatch(EmitCase):
    def test_dispatch_table(self):
        full, dispatch, _ = self.gen([JSR, *w(0x9000), RTS])
        self.assertIn("case 0x8000: func_8000(); return;", dispatch)
        self.assertIn("case 0x9000: func_9000(); return;", dispatch)
        self.assertRegex(dispatch, r"default:\s*runner_miss\(addr\);\s*cpu_interp_step\(\);")
        self.assertIn("void nes_entry_reset(void) { func_8000(); }", dispatch)

    def test_undiscovered_vector_reports_instead_of_calling(self):
        rb = RomBuilder().put(0x8000, [RTS]).vectors(0x8000, nmi=0x8100, irq=0x8200)
        dis = quiet(discover, rb)
        del dis.functions[0x8100]                    # simulate a missed vector
        _, dispatch = emit(dis, self.tmp)
        self.assertIn('vector $8100 not found', dispatch)


class TestBanked(EmitCase):
    def test_unrom_banks_recompiled_per_bank(self):
        rb = RomBuilder(mapper=2, prg_banks=4)
        rb.put(0xC000, [JSR, *w(0x8000), RTS]).vectors(0xC000)
        for bank in range(3):                        # banks 0-2 switchable, 3 fixed
            rb.put_bank(bank, 0x8000, [LDA_IMM, bank, RTS])
        dis = quiet(discover, rb)
        self.assertEqual(set(dis.banked_functions), {0, 1, 2})
        full, dispatch = emit(dis, self.tmp)
        for bank in range(3):
            body = function_body(full, f"func_b{bank}_8000")
            self.assertIn(f"cpu.A = 0x{bank:02X};", body)
            self.assertIn(f"case {bank}: func_b{bank}_8000(); return;", dispatch)
        self.assertIn("switch (mapper_get_prg_bank(0))", dispatch)
        self.assertIn("default: runner_miss(addr); cpu_interp_step(); return;", dispatch)

    def test_axrom_each_bank_seeded_from_its_own_vectors(self):
        rb = RomBuilder(mapper=7, prg_banks=4)       # 2 x 32 KB banks
        rb.put_bank(0, 0x8000, [RTS]).vectors(0x8000, bank=0)
        rb.put_bank(1, 0x9000, [RTI]).vectors(0x9000, bank=1)
        dis = quiet(discover, rb)
        self.assertIn(0x8000, dis.banked_functions[0])
        self.assertIn(0x9000, dis.banked_functions[1])
        self.assertNotIn(0x9000, dis.banked_functions[0])


class TestCli(unittest.TestCase):
    """End-to-end: run tools/nesrecomp.py as the Makefile does."""

    def test_cli_with_cfg(self):
        rb = (RomBuilder()
              .put(0x8000, [RTS])
              .put(0xE000, [*w(0xA000), *w(0xA010)])  # jump table from cfg
              .put(0xA000, [RTS]).put(0xA010, [RTS]).put(0xB000, [RTS])
              .vectors(0x8000))
        with tempfile.TemporaryDirectory() as d:
            rom = os.path.join(d, "Game.nes")
            cfg = os.path.join(d, "Game.cfg")
            with open(rom, "wb") as f:
                f.write(rb.image())
            with open(cfg, "w") as f:
                f.write("extra_func = B000\njump_table = E000,2\n")
            out = os.path.join(d, "gen")
            r = subprocess.run(
                [sys.executable, os.path.join(ROOT, "tools", "nesrecomp.py"), rom,
                 "--out", out, "--game", "Game", "--cfg", cfg, "--orphan-window", "0"],
                capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            with open(os.path.join(out, "Game_dispatch.c")) as f:
                dispatch = f.read()
        for addr in (0x8000, 0xA000, 0xA010, 0xB000):
            self.assertIn(f"case 0x{addr:04X}: func_{addr:04X}(); return;", dispatch)


if __name__ == "__main__":
    unittest.main()
