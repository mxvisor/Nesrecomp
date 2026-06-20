#!/usr/bin/env python3
"""Convert FCEUX FM2 movie files to Mesen .mmo (ZIP) format.

Usage:
    python tools/fm2_to_mesen.py input.fm2 -o output.mmo
    python tools/fm2_to_mesen.py input.fm2                         # auto output.mmo
    python tools/fm2_to_mesen.py input.fm2 --game "Game.nes"       # with ROM filename
    python tools/fm2_to_mesen.py input.fm2 --sha1 DEADBEEF...      # with ROM SHA-1
    python tools/fm2_to_mesen.py input.fm2 --author "Me"           # set author

FM2 fields per line: |skip|P1|P2|
    P1/P2: 8 chars  R L D U Start Select B A   ('.' = off, letter = on)
    skip:  additional frames to repeat (0 = 1 frame)

Mesen .mmo: ZIP with:
    GameSettings.txt  - header + serialized emulator config
    Input.txt         - one line per frame: |UDLRSsBA|UDLRSsBA|
    MovieInfo.txt     - optional (author + description)
"""

import argparse, os, struct, sys, zipfile
from datetime import datetime


FM2_TO_MESEN_BTN = {
    # fm2_pos -> (mesen_char, mesen_pos)
    0: 'R',  # Right
    1: 'L',  # Left
    2: 'D',  # Down
    3: 'U',  # Up
    4: 's',  # Start (lowercase in Mesen)
    5: 'S',  # Select (uppercase in Mesen)
    6: 'B',  # B
    7: 'A',  # A
}


def parse_fm2_buttons(s):
    """Parse 8-char FM2 button string into a set of button chars."""
    pressed = set()
    if len(s) < 8:
        return pressed
    for i, ch in enumerate(s[:8]):
        if ch != '.' and i in FM2_TO_MESEN_BTN:
            pressed.add(FM2_TO_MESEN_BTN[i])
    return pressed


def buttons_to_mesen_str(pressed):
    """Convert set of Mesen button chars to a pipe-ready 8-char string.
    Mesen order: U D L R S s B A
    """
    mesen_order = ['U', 'D', 'L', 'R', 'S', 's', 'B', 'A']
    return ''.join(ch if ch in pressed else '.' for ch in mesen_order)


def fm2_to_input_lines(data):
    """Parse FM2 content, yield (frame_idx, input_line) for each frame."""
    lines = [l.strip() for l in data.split('\n') if l.strip() and l[0] == '|']
    frame = 0
    for line in lines:
        parts = line.split('|')
        # parts[0] is empty (leading |), parts[1] = skip, parts[2] = P1, parts[3] = P2
        if len(parts) < 3:
            continue
        skip = int(parts[1]) if parts[1].isdigit() else 0
        p1 = parts[2] if len(parts) > 2 else '........'
        p2 = parts[3] if len(parts) > 3 else '........'
        repeat = skip + 1
        for _ in range(repeat):
            p1_pressed = parse_fm2_buttons(p1)
            p2_pressed = parse_fm2_buttons(p2)
            yield frame, buttons_to_mesen_str(p1_pressed), buttons_to_mesen_str(p2_pressed)
            frame += 1


def make_game_settings(rom_name="Unknown.nes", sha1=""):
    """Generate GameSettings.txt content."""
    lines = [
        "MesenVersion 2.99",
        "MovieFormatVersion 2",
        f"GameFile {rom_name}",
        f"SHA1 {sha1}" if sha1 else ";SHA1 <not provided>",
        "",
        "; === Auto-converted from FM2 ===",
        "; No original emulator settings available — using defaults",
        "<SerializedEmuSettings>",
        "  Version 8",
        "  <Controllers>",
        "    <NesController Type=\"Standard\"/>",
        "    <NesController Type=\"Standard\"/>",
        "  </Controllers>",
        "  <NesConfig>",
        "    <P1Keys></P1Keys>",
        "    <P2Keys></P2Keys>",
        "  </NesConfig>",
        "</SerializedEmuSettings>",
    ]
    return '\n'.join(lines) + '\n'


def make_input_text(input_lines):
    """Generate Input.txt content from list of (p1_str, p2_str) tuples."""
    lines = []
    for p1, p2 in input_lines:
        lines.append(f"|{p1}|{p2}|")
    return '\n'.join(lines) + '\n' if lines else '\n'


def convert(fm2_path, output_path, rom_name="Unknown.nes", sha1="", author="", description=""):
    # Parse FM2
    with open(fm2_path, 'r', encoding='utf-8', errors='replace') as f:
        data = f.read()

    frames = list(fm2_to_input_lines(data))
    if not frames:
        print(f"[error] no input frames found in {fm2_path}", file=sys.stderr)
        sys.exit(1)

    print(f"[info] parsed {len(frames)} frames from {fm2_path}", file=sys.stderr)

    # Build ZIP
    with zipfile.ZipFile(output_path, 'w', zipfile.ZIP_DEFLATED) as zf:
        # GameSettings.txt
        settings = make_game_settings(rom_name, sha1)
        zf.writestr('GameSettings.txt', settings)

        # Input.txt
        inp = make_input_text([(p1, p2) for _, p1, p2 in frames])
        zf.writestr('Input.txt', inp)

        # MovieInfo.txt (optional)
        if author or description:
            info = f"Author {author}\nDescription\n{description}\n" if description else f"Author {author}\n"
            zf.writestr('MovieInfo.txt', info)

    print(f"[ok] wrote {output_path} ({os.path.getsize(output_path)} bytes)", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description="Convert FM2 to Mesen .mmo")
    parser.add_argument("input", help="Input .fm2 file or directory")
    parser.add_argument("-o", "--output", help="Output .mmo file (default: input basename + .mmo)")
    parser.add_argument("--game", default="Unknown.nes", help="ROM filename (GameFile)")
    parser.add_argument("--sha1", default="", help="ROM SHA-1 hash")
    parser.add_argument("--author", default="", help="Author name for MovieInfo.txt")
    parser.add_argument("--description", default="", help="Description for MovieInfo.txt")
    args = parser.parse_args()

    # Handle directory: convert all .fm2 files
    if os.path.isdir(args.input):
        outdir = args.input
        if args.output:
            outdir = args.output
            os.makedirs(outdir, exist_ok=True)
        for entry in sorted(os.listdir(args.input)):
            if entry.lower().endswith('.fm2'):
                fpath = os.path.join(args.input, entry)
                basename = entry[:-4] + '.mmo'
                outpath = os.path.join(outdir, basename)
                convert(fpath, outpath, args.game, args.sha1, args.author, args.description)
        return

    # Single file
    if not os.path.exists(args.input):
        print(f"[error] file not found: {args.input}", file=sys.stderr)
        sys.exit(1)

    outpath = args.output
    if not outpath:
        base = args.input
        if base.endswith('.fm2'):
            base = base[:-4]
        outpath = base + '.mmo'

    convert(args.input, outpath, args.game, args.sha1, args.author, args.description)


if __name__ == '__main__':
    main()
