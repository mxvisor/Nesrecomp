#!/usr/bin/env python3
"""
ca65 assembly label parser for NESRecomp.

Parses a ca65 .asm / .s source file and returns the set of addresses
(>= $8000) that have a label.  Used by nesrecomp.py to seed BFS with
extra entry points from manual disassembly.

Usage (standalone):
    python asm_parser.py game.asm [game2.asm ...]

Imported:
    from asm_parser import parse_asm_labels
    labels: Set[int] = parse_asm_labels(path, optable)
"""

import os
import sys
from typing import Dict, List, Optional, Set, Tuple


def parse_asm_labels(path: str,
                     optable: dict,
                     *,
                     _insns: dict = None) -> Set[int]:
    """
    Parse a ca65 assembly source file and return all labeled addresses >= $8000.

    Parameters
    ----------
    path     : path to the .asm / .s file
    optable  : OPTABLE dict from nesrecomp (opcode -> Opcode dataclass with
               .mnemonic, .mode, .size fields).  Used to estimate instruction
               sizes so the PC counter stays accurate.

    Returns
    -------
    Set of integer addresses.
    """
    if not os.path.exists(path):
        print(f"[asm_parser] ASM file not found: {path}")
        return set()

    # Build mnemonic -> [(mode, size)] lookup
    if _insns is not None:
        insns = _insns
    else:
        insns: Dict[str, List[Tuple[str, int]]] = {}
        for op in optable.values():
            insns.setdefault(op.mnemonic, []).append((op.mode, op.size))

    labels: Set[int] = set()
    pc: int = 0
    in_code: bool = False
    code_org: Optional[int] = None

    with open(path) as f:
        for line in f:
            # Strip comments
            if ';' in line:
                line = line[:line.index(';')]
            line = line.strip()
            if not line:
                continue

            # .org directive
            if line.startswith('.org'):
                try:
                    val_str = line.split()[-1]
                    org = int(val_str.replace('$', ''), 16)
                except ValueError:
                    continue
                if org >= 0x8000:
                    code_org = org
                    pc = org
                    in_code = True
                continue

            # .segment directive
            if '.segment' in line:
                in_code = ('"CODE"' in line or '"PRG"' in line
                           or '"HEADER"' in line)
                if in_code and code_org is not None:
                    pc = code_org
                continue

            if not in_code:
                continue

            # Extract label
            label = None
            instr_part = line
            if ':' in line:
                parts = line.split(':', 1)
                lc = parts[0].strip()
                if lc and lc[0].isalpha():
                    label = lc
                instr_part = parts[1].strip() if len(parts) > 1 else ''

            if label:
                labels.add(pc)

            # Estimate byte size of this line
            instr_upper = instr_part.upper()

            if not instr_part:
                size = 0
            elif instr_upper.startswith('.BYTE') or instr_upper.startswith('.DB'):
                rest = instr_part.split(None, 1)[1] if ' ' in instr_part else ''
                if rest.startswith('"') or rest.startswith("'"):
                    quote = rest[0]
                    end = rest.find(quote, 1)
                    size = end if end > 0 else 1
                else:
                    size = rest.count(',') + 1 if rest else 1
            elif (instr_upper.startswith('.WORD') or instr_upper.startswith('.ADDR')
                  or instr_upper.startswith('.DW')):
                rest = instr_part.split(None, 1)[1] if ' ' in instr_part else ''
                size = (rest.count(',') + 1) * 2 if rest else 2
            elif instr_upper.startswith('.RES') or instr_upper.startswith('.DS'):
                try:
                    rest = instr_part.split(None, 1)[1] if ' ' in instr_part else ''
                    size = int(rest.split(',')[0])
                except (ValueError, IndexError):
                    size = 1
            else:
                mn = instr_part.split()[0].upper() if instr_part else ''
                size = 0
                if mn in insns:
                    op_part = instr_part[len(mn):].strip()
                    matched = False
                    for mode, sz in insns[mn]:
                        if mode == 'imp' and not op_part:
                            size = sz; matched = True; break
                        elif mode == 'imm' and op_part.startswith('#'):
                            size = sz; matched = True; break
                        elif mode == 'izx' and '(' in op_part and ',X' in op_part.upper():
                            size = 2; matched = True; break
                        elif mode == 'izy' and '(' in op_part and ',Y' in op_part.upper():
                            size = 2; matched = True; break
                        elif mode == 'ind' and '(' in op_part:
                            size = 3; matched = True; break
                        elif mode in ('zpx', 'abx') and ',X' in op_part.upper():
                            size = sz; matched = True; break
                        elif mode in ('zpy', 'aby') and ',Y' in op_part.upper():
                            size = sz; matched = True; break
                        elif mode == 'zp' and not op_part.startswith('#') and ',' not in op_part:
                            try:
                                val = int(op_part.replace('$', ''), 16)
                                size = 2 if val < 0x100 else 3
                            except Exception:
                                size = 3
                            matched = True; break
                        elif mode == 'abs' and not op_part.startswith('#') and ',' not in op_part:
                            size = 3; matched = True; break
                        elif mode == 'rel':
                            size = 2; matched = True; break
                    if not matched:
                        size = 3
                else:
                    size = 3  # unknown — assume 3-byte instruction

            pc += size

    print(f"[asm_parser] {len(labels)} labels >= $8000 from {path}")
    return labels


# ---------------------------------------------------------------------------
# Standalone entry point
# ---------------------------------------------------------------------------

def merge_into_cfg(cfg_path: str, new_addrs: Set[int]) -> None:
    """
    Merge new_addrs into an existing cfg file as extra_func entries.
    Existing entries and all other directives are preserved.
    Only addresses not already present are appended.
    """
    existing: Set[int] = set()
    lines: list = []

    if os.path.exists(cfg_path):
        with open(cfg_path) as f:
            for line in f:
                lines.append(line)
                s = line.strip()
                if s.startswith('extra_func') and '=' in s:
                    try:
                        existing.add(int(s.split('=', 1)[1].strip(), 16))
                    except ValueError:
                        pass

    to_add = sorted(new_addrs - existing)
    if not to_add:
        print(f"[asm_parser] {cfg_path}: nothing new to add")
        return

    os.makedirs(os.path.dirname(cfg_path) if os.path.dirname(cfg_path) else '.', exist_ok=True)
    with open(cfg_path, 'a') as f:
        if lines and not lines[-1].endswith('\n'):
            f.write('\n')
        f.write("# asm_parser.py\n")
        for a in to_add:
            f.write(f"extra_func = {a:04X}\n")
    print(f"[asm_parser] added {len(to_add)} new entries to {cfg_path}")


if __name__ == '__main__':
    import argparse as _ap
    p = _ap.ArgumentParser(description="ca65 label parser — extract PRG entry points")
    p.add_argument("asm", nargs='+', help="ca65 .asm / .s source files")
    p.add_argument("--cfg", default=None,
                   help="Merge discovered labels as extra_func entries into this cfg file")
    args = p.parse_args()

    # Minimal stub optable so the module works standalone (sizes may be off)
    class _Op:
        def __init__(self, mn, mode, size):
            self.mnemonic = mn; self.mode = mode; self.size = size

    _STUB: dict = {}
    for _mn in ('LDA','STA','LDX','STX','LDY','STY','ADC','SBC','AND','ORA',
                'EOR','CMP','CPX','CPY','BIT','ASL','LSR','ROL','ROR',
                'INC','DEC','INX','DEX','INY','DEY','TAX','TXA','TAY','TYA',
                'TSX','TXS','PHA','PLA','PHP','PLP','JMP','JSR','RTS','RTI',
                'BEQ','BNE','BCC','BCS','BMI','BPL','BVC','BVS',
                'CLC','SEC','CLI','SEI','CLD','SED','CLV','NOP','BRK'):
        _STUB.setdefault(_mn, []).append(('imp', 1))

    all_addrs: Set[int] = set()
    for path in args.asm:
        all_addrs |= parse_asm_labels(path, {}, _insns=_STUB)

    if args.cfg:
        merge_into_cfg(args.cfg, all_addrs)
    else:
        for a in sorted(all_addrs):
            print(f"  ${a:04X}")
