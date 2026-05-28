#!/usr/bin/env python3
"""Extract PRG and CHR from an iNES ROM and emit GAME_embedded_data.{h,c}."""

import sys
import os
import argparse

def arr_bytes(name, buf):
    lines = [f'const uint8_t {name}[{len(buf)}] = {{']
    for i in range(0, len(buf), 16):
        chunk = buf[i:i+16]
        hexes = ', '.join(f'0x{b:02x}' for b in chunk)
        lines.append(f'  {hexes},')
    lines.append('};')
    return '\n'.join(lines)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("rom", help="Path to iNES ROM")
    parser.add_argument("--out", default="generated", help="Output directory")
    parser.add_argument("--game", required=True, help="Game name (prefix)")
    args = parser.parse_args()

    with open(args.rom, "rb") as f:
        data = f.read()

    if data[:4] != b"NES\x1a":
        print("Not a valid iNES ROM")
        sys.exit(1)

    prg_banks   = data[4]
    chr_banks   = data[5]
    flags6      = data[6]
    flags7      = data[7]
    mapper_id   = (flags7 & 0xF0) | (flags6 >> 4)
    mirroring   = flags6 & 1
    has_trainer = (flags6 >> 2) & 1
    header_size = 16 + (512 if has_trainer else 0)

    prg_size = prg_banks * 16384
    chr_size = chr_banks * 8192

    prg = data[header_size:header_size + prg_size]
    chr_ = data[header_size + prg_size:header_size + prg_size + chr_size]

    prg = prg.ljust(prg_size, b'\x00')
    chr_ = chr_.ljust(chr_size, b'\x00')

    prefix = args.game

    # --- Header ---
    guard = f"{prefix.upper()}_EMBEDDED_DATA_H"
    header = f'''#ifndef {guard}
#define {guard}

#include <stdint.h>

#define EMBEDDED_PRG_SIZE    {prg_size}
#define EMBEDDED_CHR_SIZE    {chr_size}
#define EMBEDDED_MAPPER_ID   {mapper_id}
#define EMBEDDED_PRG_BANKS   {prg_banks}
#define EMBEDDED_CHR_BANKS   {chr_banks}
#define EMBEDDED_MIRRORING   {mirroring}

extern const uint8_t embedded_prg_rom[EMBEDDED_PRG_SIZE];
extern const uint8_t embedded_chr_rom[EMBEDDED_CHR_SIZE];

#endif
'''

    # --- Source ---
    src = f'''#include "{prefix}_embedded_data.h"

/* Auto-generated from {os.path.basename(args.rom)} */
/* PRG: {prg_banks} × 16KB, CHR: {chr_banks} × 8KB, Mapper: {mapper_id} */

{arr_bytes("embedded_prg_rom", prg)}

{arr_bytes("embedded_chr_rom", chr_)}
'''

    os.makedirs(args.out, exist_ok=True)

    h_path = os.path.join(args.out, f"{prefix}_embedded_data.h")
    c_path = os.path.join(args.out, f"{prefix}_embedded_data.c")

    with open(h_path, "w") as f:
        f.write(header)

    with open(c_path, "w") as f:
        f.write(src)

    print(f"Generated {h_path}")
    print(f"Generated {c_path}")
    print(f"  PRG: {prg_size} bytes, CHR: {chr_size} bytes, Mapper: {mapper_id}")

if __name__ == "__main__":
    main()
