#!/usr/bin/env python3
"""Generate Mesen replay + screenshots from FM2.

Usage:
    # Step 1: Convert FM2 → replay files
    python3 tools/replay/fm2_replay.py fm2/Mario.fm2 /tmp/mario

    # Step 2: Run in Mesen headless (requires AllowIoOsAccess=true in settings.json)
    ./Mesen/Mesen --testRunner --doNotSaveSettings /tmp/mario.lua rom/Mario.nes

    # Step 3: Convert raw screenshots to PNG
    python3 tools/replay/fm2_replay.py --convert-png /tmp/mario_shots/

Dependencies: Python 3, Pillow (pip install Pillow)
Mesen settings: Debug → AllowIoOsAccess = true (already set in Mesen/settings.json)
"""

import argparse, os, struct, sys


def parse_fm2_buttons(s):
    v = 0
    if len(s) >= 8:
        if s[0] != '.': v |= 0x01
        if s[1] != '.': v |= 0x02
        if s[2] != '.': v |= 0x04
        if s[3] != '.': v |= 0x08
        if s[4] != '.': v |= 0x10
        if s[5] != '.': v |= 0x20
        if s[6] != '.': v |= 0x40
        if s[7] != '.': v |= 0x80
    return v


def raw_to_png(raw_path, png_path=None):
    """Convert raw ARGB → PNG."""
    try:
        from PIL import Image
    except ImportError:
        print("Need Pillow: pip install Pillow", file=sys.stderr)
        sys.exit(1)
    base = os.path.splitext(raw_path)[0]
    info_path = base + '.txt'
    if not png_path:
        png_path = base + '.png'
    with open(info_path) as f:
        w, h, _ = f.read().strip().split()
        w, h = int(w), int(h)
    with open(raw_path, 'rb') as f:
        raw = f.read()
    img = Image.new('RGBA', (w, h))
    for y in range(h):
        for x in range(w):
            off = (y * w + x) * 4
            img.putpixel((x, y), (raw[off + 2], raw[off + 1], raw[off], raw[off + 3]))
    img.save(png_path)
    print(f"  {png_path} ({w}x{h})")


def convert(fm2_path, output_prefix, shot_interval=60):
    with open(fm2_path, 'r', encoding='utf-8', errors='replace') as f:
        data = f.read()

    frames = []
    for l in data.split('\n'):
        if not l or l[0] != '|':
            continue
        parts = l.split('|')
        if len(parts) < 3:
            continue
        skip = int(parts[1]) if parts[1].isdigit() else 0
        p1 = parts[2] if len(parts) > 2 else '........'
        p2 = parts[3] if len(parts) > 3 else '........'
        c0 = parse_fm2_buttons(p1)
        c1 = parse_fm2_buttons(p2)
        for _ in range(skip + 1):
            frames.append(struct.pack('BB', c0, c1))

    total = len(frames)
    bin_abs = os.path.abspath(output_prefix + '.bin')
    shot_dir = os.path.abspath(output_prefix + '_shots')

    with open(output_prefix + '.bin', 'wb') as f:
        for frame in frames:
            f.write(frame)

    lua = f'''-- Mesen replay: {os.path.basename(fm2_path)}
local BIN = "{bin_abs}"
local SHOT_DIR = "{shot_dir}"
local SHOT_INTERVAL = {shot_interval}
local TOTAL = {total}

os.execute("mkdir -p " .. SHOT_DIR)

local f = io.open(BIN, "rb")
if not f then emu.stop(1) end
local raw = f:read("*all")
f:close()

local frame = 0
local off = 0

local function on_input()
    if frame >= TOTAL then emu.stop(0) return end
    local c0 = string.byte(raw, off + 1)
    local c1 = string.byte(raw, off + 2)
    local inp0, inp1 = {{}}, {{}}
    if c0 & 0x01 ~= 0 then inp0.right  = true end
    if c0 & 0x02 ~= 0 then inp0.left   = true end
    if c0 & 0x04 ~= 0 then inp0.down   = true end
    if c0 & 0x08 ~= 0 then inp0.up     = true end
    if c0 & 0x10 ~= 0 then inp0.start  = true end
    if c0 & 0x20 ~= 0 then inp0.select = true end
    if c0 & 0x40 ~= 0 then inp0.b      = true end
    if c0 & 0x80 ~= 0 then inp0.a      = true end
    if c1 & 0x01 ~= 0 then inp1.right  = true end
    if c1 & 0x02 ~= 0 then inp1.left   = true end
    if c1 & 0x04 ~= 0 then inp1.down   = true end
    if c1 & 0x08 ~= 0 then inp1.up     = true end
    if c1 & 0x10 ~= 0 then inp1.start  = true end
    if c1 & 0x20 ~= 0 then inp1.select = true end
    if c1 & 0x40 ~= 0 then inp1.b      = true end
    if c1 & 0x80 ~= 0 then inp1.a      = true end
    emu.setInput(inp0, 0)
    emu.setInput(inp1, 1)
    off = off + 2
    frame = frame + 1
    if frame % SHOT_INTERVAL == 0 then
        local buf = emu.getScreenBuffer()
        local sz = emu.getScreenSize()
        if buf and #buf > 0 then
            local raw_data = {{}}
            for i = 1, #buf do
                local v = buf[i]
                raw_data[#raw_data + 1] = string.char(v & 0xFF)
                raw_data[#raw_data + 1] = string.char((v >> 8) & 0xFF)
                raw_data[#raw_data + 1] = string.char((v >> 16) & 0xFF)
                raw_data[#raw_data + 1] = string.char((v >> 24) & 0xFF)
            end
            local fname = string.format(SHOT_DIR .. "/frame_%04d", frame)
            local f_raw = io.open(fname .. ".raw", "wb")
            if f_raw then f_raw:write(table.concat(raw_data)); f_raw:close() end
            local f_info = io.open(fname .. ".txt", "w")
            if f_info then f_info:write(tostring(sz.width) .. " " .. tostring(sz.height) .. " " .. tostring(#buf)); f_info:close() end
        end
    end
end
emu.addEventCallback(on_input, emu.eventType.inputPolled)
'''

    with open(output_prefix + '.lua', 'w') as f:
        f.write(lua)

    print(f'frames: {total}')
    print(f'bin:    {output_prefix}.bin')
    print(f'script: {output_prefix}.lua')
    print(f'shots:  {shot_dir}/')
    print(f'run:    ./Mesen/Mesen --testRunner --doNotSaveSettings {output_prefix}.lua <rom.nes>')


def main():
    parser = argparse.ArgumentParser(description='FM2 → Mesen replay + screenshots')
    parser.add_argument('input', help='.fm2 file')
    parser.add_argument('output_prefix', nargs='?', default=None, help='Output prefix')
    parser.add_argument('--shot-interval', type=int, default=60, help='Screenshot every N frames')
    parser.add_argument('--convert-png', metavar='DIR', help='Convert all .raw in DIR to PNG')
    args = parser.parse_args()

    if args.convert_png:
        import glob
        raws = sorted(glob.glob(os.path.join(args.convert_png, '*.raw')))
        if not raws:
            print(f'no .raw files in {args.convert_png}')
            sys.exit(1)
        for r in raws:
            raw_to_png(r)
        print(f'converted {len(raws)} files')
        return

    if not args.input:
        parser.print_help()
        sys.exit(1)

    prefix = args.output_prefix or os.path.splitext(args.input)[0] + '_replay'
    convert(args.input, prefix, args.shot_interval)


if __name__ == '__main__':
    main()
