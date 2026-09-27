"""iNES parsing and BFS/orphan discovery in tools/nesrecomp.py."""
import contextlib
import io
import unittest

from helpers import (BEQ, FILL, JMP_ABS, JMP_IND, JSR, LDA_IMM, NOP, RTI, RTS,
                     RomBuilder, discover, nesrecomp, w)


def quiet(fn, *a, **kw):
    with contextlib.redirect_stdout(io.StringIO()):
        return fn(*a, **kw)


class TestParseInes(unittest.TestCase):
    def test_header_fields(self):
        rb = RomBuilder(mapper=0x47, prg_banks=4, chr_banks=2, battery=True)
        hdr, prg, chr_ = nesrecomp.parse_ines(rb.image())
        self.assertEqual(hdr.mapper, 0x47)          # low nibble flags6, high flags7
        self.assertEqual(hdr.prg_banks, 4)
        self.assertEqual(hdr.chr_banks, 2)
        self.assertTrue(hdr.has_battery)
        self.assertEqual(len(prg), 4 * 0x4000)
        self.assertEqual(len(chr_), 2 * 0x2000)

    def test_trainer_is_skipped(self):
        img = bytearray(RomBuilder().image())
        img[6] |= 0x04                               # trainer present
        img[16:16] = bytes([0xEE]) * 512
        hdr, prg, _ = nesrecomp.parse_ines(bytes(img))
        self.assertTrue(hdr.has_trainer)
        self.assertEqual(prg[0], FILL)               # not trainer bytes

    def test_rejects_non_ines(self):
        with self.assertRaises(ValueError):
            nesrecomp.parse_ines(b"NOPE" + bytes(32))


class TestBfs(unittest.TestCase):
    def test_vectors_are_seeds(self):
        rb = (RomBuilder()
              .put(0x8000, [NOP, RTS]).put(0x8100, [RTI]).put(0x8200, [RTI])
              .vectors(reset=0x8000, nmi=0x8100, irq=0x8200))
        dis = quiet(discover, rb)
        self.assertEqual({0x8000, 0x8100, 0x8200}, set(dis.functions))

    def test_decoding_stops_at_terminator(self):
        rb = RomBuilder().put(0x8000, [NOP, NOP, RTS, NOP]).vectors(0x8000)
        dis = quiet(discover, rb)
        self.assertEqual([pc for pc, _, _ in dis.functions[0x8000]],
                         [0x8000, 0x8001, 0x8002])

    def test_jsr_seeds_target_and_return_address(self):
        rb = (RomBuilder()
              .put(0x8000, [JSR, *w(0x9000), RTS])
              .put(0x9000, [RTS]).vectors(0x8000))
        dis = quiet(discover, rb)
        self.assertIn(0x9000, dis.functions)
        self.assertIn(0x8003, dis.functions)         # return address

    def test_inline_data_func_skips_return_address(self):
        # JSR print; .byte "data"; — bytes after the JSR are not code.
        rb = (RomBuilder()
              .put(0x8000, [JSR, *w(0x9000), 0x41, 0x42, 0x00])
              .put(0x9000, [RTS]).vectors(0x8000))
        dis = quiet(discover, rb, inline_data=[0x9000])
        self.assertIn(0x9000, dis.functions)
        self.assertNotIn(0x8003, dis.functions)

    def test_branch_target_is_seeded(self):
        rb = (RomBuilder()
              .put(0x8000, [BEQ, 0x10, RTS])            # -> $8012
              .put(0x8012, [LDA_IMM, 1, RTS]).vectors(0x8000))
        dis = quiet(discover, rb)
        self.assertIn(0x8012, dis.functions)

    def test_jmp_abs_target_is_seeded(self):
        rb = (RomBuilder().put(0x8000, [JMP_ABS, *w(0xA000)])
              .put(0xA000, [RTS]).vectors(0x8000))
        dis = quiet(discover, rb)
        self.assertIn(0xA000, dis.functions)

    def test_indirect_jmp_scans_table_until_non_prg_word(self):
        table = w(0xA000) + w(0xA010) + w(0x1234) + w(0xA020)
        rb = (RomBuilder()
              .put(0x8000, [JMP_IND, *w(0x9000)])
              .put(0x9000, table)
              .put(0xA000, [RTS]).put(0xA010, [RTS]).put(0xA020, [RTS])
              .vectors(0x8000))
        dis = quiet(discover, rb)
        self.assertIn(0xA000, dis.functions)
        self.assertIn(0xA010, dis.functions)
        self.assertNotIn(0xA020, dis.functions)      # after the $1234 stop word

    def test_indirect_jmp_through_ram_is_not_followed(self):
        rb = RomBuilder().put(0x8000, [JMP_IND, *w(0x0300)]).vectors(0x8000)
        dis = quiet(discover, rb)
        self.assertEqual({0x8000}, set(dis.functions))

    def test_data_region_is_not_entered(self):
        rb = (RomBuilder().put(0x8000, [JSR, *w(0x9000), RTS])
              .put(0x9000, [RTS]).vectors(0x8000))
        dis = quiet(discover, rb, data_regions=[(0x9000, 0x90FF)])
        self.assertNotIn(0x9000, dis.functions)

    def test_extra_func_seed(self):
        rb = RomBuilder().put(0x8000, [RTS]).put(0xB000, [RTS]).vectors(0x8000)
        dis = quiet(discover, rb, extra=[0xB000])
        self.assertIn(0xB000, dis.functions)

    def test_switchable_window_is_left_to_interpreter_mmc1(self):
        # MMC1: $8000-$BFFF is switchable -> never recompiled from the fixed bank.
        rb = (RomBuilder(mapper=1, prg_banks=4)
              .put(0xC000, [JSR, *w(0x8000), RTS]).vectors(0xC000))
        dis = quiet(discover, rb)
        self.assertNotIn(0x8000, dis.functions)
        self.assertEqual({}, dis.banked_functions)   # no per-bank path for MMC1
        self.assertIn(0xC000, dis.functions)


class TestOrphanPhase(unittest.TestCase):
    # Place code at the top of the address space: every byte decodes as some
    # opcode, so the orphan chain only ends at $10000.
    def rom(self):
        return (RomBuilder()
                .put(0xFFE0, [RTS])                   # reached from RESET
                .put(0xFFE1, [LDA_IMM, 7, RTS])       # adjacent, never referenced
                .vectors(0xFFE0))

    def test_adjacent_function_found_with_window(self):
        dis = quiet(discover, self.rom(), orphan_window=3)
        self.assertIn(0xFFE1, dis.functions)

    def test_no_orphans_with_zero_window(self):
        dis = quiet(discover, self.rom(), orphan_window=0)
        self.assertNotIn(0xFFE1, dis.functions)


class TestParseCfg(unittest.TestCase):
    def test_all_directives(self):
        import os
        import tempfile
        text = ("# comment\n"
                "extra_func = 8123\n"
                "extra_func = 2:A000\n"
                "data_region = 9000,90FF\n"
                "inline_data_func = C5D0\n"
                "jump_table = E000,4\n")
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "g.cfg")
            with open(p, "w") as f:
                f.write(text)
            cfg = nesrecomp.parse_cfg(p)
        self.assertEqual(cfg["extra_func"], [0x8123])
        self.assertEqual(cfg["banked_extra_func"], [(2, 0xA000)])
        self.assertEqual(cfg["data_region"], [(0x9000, 0x90FF)])
        self.assertEqual(cfg["inline_data_func"], [0xC5D0])
        self.assertEqual(cfg["jump_table"], [(0xE000, 4)])

    def test_missing_file_gives_empty_config(self):
        cfg = nesrecomp.parse_cfg("/nonexistent/x.cfg")
        self.assertEqual(cfg["extra_func"], [])


if __name__ == "__main__":
    unittest.main()
