-- fceux_dump.lua
-- Dumps per-frame: framecount, lag(0/1), lagcount, djb2(RAM $0000-$07FF)
-- Usage: fceux -playmovie demo.fm2 -lua fceux_dump.lua game.nes
-- Output file set via OUT variable below.

local OUT = "/tmp/fceux_sync.txt"
local MAX_FRAMES = 0   -- 0 = unlimited (до конца демки)

emu.speedmode("nothrottle")

local f = assert(io.open(OUT, "w"))
local frame_n = 0

local function djb2(s)
    local h = 5381
    for i = 1, #s do
        h = (h * 33 + s:byte(i)) % 4294967296
    end
    return h
end

local function onAfter()
    frame_n = frame_n + 1
    local ram = memory.readbyterange(0, 0x800)
    local lag = emu.lagged() and 1 or 0
    local lagc = emu.lagcount()
    f:write(string.format("%u %d %u %08X\n", frame_n, lag, lagc, djb2(ram)))

    local done = (MAX_FRAMES > 0 and frame_n >= MAX_FRAMES)
               or (movie.playing and not movie.playing())
    if done then
        f:close()
        os.exit(0)   -- immediate; emu.exit() only schedules and hangs in GUI mode
    end
end

emu.registerafter(onAfter)
