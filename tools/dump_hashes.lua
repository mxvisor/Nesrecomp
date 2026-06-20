-- dump_hashes.lua  (Lua 5.1 — FCEUX 2.6.x)
-- Env: FM2=<absolute path to .fm2>
--      OUT=<absolute path to output .hashes>  (optional; default = FM2 with .hashes ext)
--
-- Output format: "NNNNNN HHHHHHHH\n" per frame — same as Nesrecomp --dump-frames
-- Hash: CRC32 of GD2 pixel data (256*240*4 bytes, [0x00,R,G,B]), skipping 11-byte header.
--
-- NOTE: movie.play() in FCEUX Qt 2.6 requires absolute paths.

-- ── Portable 32-bit helpers (no bitop required) ───────────────────────────────
local function rshift32(a, n) return math.floor(a / (2^n)) % 0x100000000 end
local function bnot32(a)      return (0xFFFFFFFF - a % 0x100000000) % 0x100000000 end

local function bxor32(a, b)
    local r, p = 0, 1
    a = a % 0x100000000; b = b % 0x100000000
    for _ = 1, 32 do
        if (a % 2) ~= (b % 2) then r = r + p end
        a = math.floor(a / 2); b = math.floor(b / 2); p = p * 2
    end
    return r
end

local function band32(a, b)
    local r, p = 0, 1
    a = a % 0x100000000; b = b % 0x100000000
    for _ = 1, 32 do
        if (a % 2 == 1) and (b % 2 == 1) then r = r + p end
        a = math.floor(a / 2); b = math.floor(b / 2); p = p * 2
    end
    return r
end

-- ── CRC32 table ───────────────────────────────────────────────────────────────
local crc_table = {}
for i = 0, 255 do
    local c = i
    for _ = 0, 7 do
        if c % 2 == 1 then
            c = bxor32(0xEDB88320, rshift32(c, 1))
        else
            c = rshift32(c, 1)
        end
    end
    crc_table[i] = c
end

local function crc32(data)
    local crc = 0xFFFFFFFF
    for i = 1, #data do
        local b = data:byte(i)
        crc = bxor32(crc_table[band32(bxor32(crc, b), 0xFF)], rshift32(crc, 8))
    end
    return bnot32(crc)
end

-- ── Config ────────────────────────────────────────────────────────────────────
local fm2_path = os.getenv("FM2")
local out_path = os.getenv("OUT")
if not fm2_path then
    print("[dump_hashes] ERROR: FM2 env var not set")
    emu.exit(); return
end
if not out_path then
    out_path = fm2_path:gsub("%.[^%.]+$", ".hashes")
end

-- ── State ─────────────────────────────────────────────────────────────────────
local outfile = nil
local frame   = 0
local started = false
local done    = false

-- ── FCEUX hooks ───────────────────────────────────────────────────────────────
-- emu.speedmode("maximum")  -- disabled: may suppress registerafter in Qt FCEUX

emu.registerbefore(function()
    if done then return end

    -- Load movie on first tick (ROM must be loaded first)
    if not started then
        started = true
        local ok, err = pcall(function() movie.play(fm2_path) end)
        if not ok then
            print("[dump_hashes] movie.play failed: " .. tostring(err))
            emu.exit(); done = true; return
        end
        -- Open output file
        outfile = io.open(out_path, "w")
        if not outfile then
            print("[dump_hashes] ERROR: cannot open " .. out_path)
            emu.exit(); done = true; return
        end
        print("[dump_hashes] " .. fm2_path .. " → " .. out_path)
        return  -- skip hash on frame 0 (movie not yet active this tick)
    end

    -- Detect movie end
    if not movie.active() and frame > 0 then
        if outfile then outfile:close(); outfile = nil end
        print(string.format("[dump_hashes] done: %d frames → %s", frame, out_path))
        done = true
        emu.exit()
    end
end)

emu.registerafter(function()
    if done or not outfile then return end

    local gd     = gui.gdscreenshot()
    local pixels = string.sub(gd, 12)     -- skip 11-byte GD2 header
    local h      = crc32(pixels)

    frame = frame + 1
    outfile:write(string.format("%06d %08X\n", frame, h))
end)

print("[dump_hashes] loaded. FM2=" .. fm2_path)
