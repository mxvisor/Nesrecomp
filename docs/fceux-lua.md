# FCEUX 2.6.6 Lua API — Frame Capture & Exit (researched from source)

> Moved from `AGENTS.md`. Reference for writing FCEUX Lua dump scripts (`tools/fceux_dump.lua`, `tools/fceux_framehash.lua`).

Source: `/home/VisoR/projects/HOME/BATTLE_CITY_ADW/fceux-2.6.6/src/`

## Speed rule

**Always run FCEUX at maximum speed** when capturing sync/frame data. Add `emu.speedmode("nothrottle")` at the top of every Lua script. This is already in `tools/fceux_dump.lua`.

## Speed modes (`emu.speedmode`)

| Mode | Rendering | `gui.gdscreenshot()` | Lua hooks |
|------|-----------|----------------------|-----------|
| `"normal"` | every frame, throttled to 60fps | ✅ valid | every frame |
| `"nothrottle"` | every frame, no throttle (~3200%) | ✅ valid | every frame |
| `"turbo"` | frame-skip | ⚠️ may be stale | every frame |
| `"maximum"` | **SKIPPED** (stub only) | ❌ stale buffer | every frame |

**Rule:** Use `"nothrottle"` for fast capture. Never use `"maximum"` with `gui.gdscreenshot()`.

## Exiting FCEUX from Lua

- `os.exit(0)` — **works immediately**, os library is NOT sandboxed (`luaL_openlibs` loads everything). Best for clean process kill.
- `emu.exit()` — sets `exitScheduled = TRUE`, processed at next frame boundary (not immediate). Prefer `os.exit(0)` when speed matters.
- `--movielength N` CLI flag — calls `exit(0)` directly in `fceu.cpp` after N frames. Zero Lua overhead. Use this when you don't need per-frame hashes.

## `gui.gdscreenshot()` — GD2 format

Returns a Lua string of `11 + 256×240×4 = 245,771` bytes:
- Bytes 0–10: GD2 header (`FF FE`, width(2BE), height(2BE), truecolor(1), bgcolor(4))
- Bytes 11+: pixels as `[A][R][G][B]` per pixel, A=0 means opaque (7-bit alpha)

**Fast CRC32 idiom** — hash entire pixel block in one Lua pass (NOT per-pixel sub()):
```lua
local pixels = gd:sub(12)   -- everything after header (Lua 1-indexed: byte 12 = first pixel A)
local hash = crc32(pixels)  -- one pass over 245,771 bytes
```

Performance note: per-pixel `gd:sub(off, off+2)` in a loop = ~61K string allocs → GC pressure → ~100ms/frame. One `gd:sub(12)` + byte-loop CRC32 = ~5ms/frame.

## Frame hash format compatibility with `--dump-frames`

FCEUX GD2 pixel layout: `[A=0, R, G, B]` big-endian per pixel.
Our framebuf layout: `0xFFRRGGBB` uint32_t (little-endian: bytes B, G, R, A in memory).

**To get matching hashes**, runner.c must output pixels in the same byte order as GD2.
Options:
1. Hash `[0, R, G, B]` in both (add zero alpha byte before each pixel in runner.c)
2. Hash only `[R, G, B]` in both (skip alpha in Lua with per-pixel indexing — slow)
3. **Best:** use FCEUX palette in our emulator so colors match, then hash `[R, G, B]`

**Correct approach for palette-invariant hashes:**
1. Add `uint8_t indexbuf[SCREEN_W * SCREEN_H]` to ppu struct (filled alongside framebuf with `color & 0x3F`)
2. In runner.c `--dump-frames`: convert indexbuf → [0, R, G, B] using FCEUX's palette table (static const in runner.c), hash that
3. In Lua: `gd:sub(12)` already uses FCEUX palette → identical bytes for same NES index

Do NOT change `PALETTE[]` in ppu.c — it is used for display rendering, not hash comparison.

## Correct minimal Lua script template

```lua
local outfile = os.getenv("FRAME_HASH_OUT") or "/tmp/fceux_frames.txt"
local limit   = tonumber(os.getenv("FRAME_LIMIT") or "0") or 0
local f = assert(io.open(outfile, "w"))

emu.speedmode("nothrottle")

-- CRC32 (Lua 5.1, bit library)
local bit = require("bit")
local band, bxor, rshift = bit.band, bit.bxor, bit.rshift
local crc_tab = {}
for i = 0, 255 do
    local c = i
    for _ = 1, 8 do
        c = band(c,1)==1 and bxor(rshift(c,1), 0xEDB88320) or rshift(c,1)
    end
    crc_tab[i] = c
end
local function crc32(s)
    local crc = 0xFFFFFFFF
    for i = 1, #s do
        crc = bxor(rshift(crc,8), crc_tab[band(bxor(crc, s:byte(i)), 0xFF)])
    end
    return band(bxor(crc, 0xFFFFFFFF), 0xFFFFFFFF)
end

local frame, done = 0, false
emu.registerafter(function()
    if done then return end
    frame = frame + 1
    local gd = gui.gdscreenshot()
    if gd and #gd > 11 then
        f:write(string.format("%06d %08X\n", frame, crc32(gd:sub(12))))
    else
        f:write(string.format("%06d NOFRAME\n", frame))
    end
    if (limit > 0 and frame >= limit) or movie.mode() == "finished" then
        done = true
        f:close()
        os.exit(0)   -- immediate, os not sandboxed
    end
end)
```

