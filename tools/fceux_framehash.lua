-- fceux_framehash.lua  (Lua 5.1, FCEUX 2.6+)
-- Env:  FRAME_HASH_OUT=/tmp/nes_frames_FCEUX.txt
--       FRAME_LIMIT=600   (0 = full movie)
-- Runs at nothrottle speed, hashes gui.gdscreenshot() GD2 pixel block per frame.
-- Exits via os.exit(0) (not sandboxed in FCEUX).

-- Writes raw pixel frames to FRAME_PIPE (a named pipe / fifo).
-- hash_frames.py reads from the other end simultaneously — no disk usage.
-- Format: [4-byte LE frame number] [245760 bytes ARGB pixels] per frame.
local pipepath = os.getenv("FRAME_PIPE") or "/tmp/fceux_frames.pipe"
local limit    = tonumber(os.getenv("FRAME_LIMIT") or "0") or 0
local f        = assert(io.open(pipepath, "wb"))

emu.speedmode("maximum")

local frame, done = 0, false
emu.registerafter(function()
    if done then return end
    frame = frame + 1
    local gd = gui.gdscreenshot()
    local fn = frame
    f:write(string.char(fn%256, math.floor(fn/256)%256, math.floor(fn/65536)%256, 0))
    f:write(gd:sub(12))
    if (limit > 0 and frame >= limit) or movie.mode() == "finished" then
        done = true
        f:close()
        io.stderr:write(string.format("[fceux_framehash] done: %d frames\n", frame))
        os.exit(0)
    end
end)
