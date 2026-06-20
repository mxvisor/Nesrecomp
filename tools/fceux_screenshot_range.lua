-- fceux_screenshot_range.lua  (Lua 5.1, FCEUX 2.6+)
-- Save raw GD2 screenshots for frames 1..FRAME_LIMIT into SCREENSHOT_DIR.
-- Files: frame_000001.gd2, frame_000002.gd2, ...
-- Convert to PNG afterwards with tools/gd2_to_png.py.
--
-- Env:
--   FRAME_LIMIT      N    (required) — capture frames 1..N
--   SCREENSHOT_DIR   path (default ./tmp/screenshots/fceux_GAME)

local limit  = tonumber(os.getenv("FRAME_LIMIT") or "0") or 0
local outdir = os.getenv("SCREENSHOT_DIR") or "./tmp/screenshots"

emu.speedmode("maximum")

local frame, done = 0, false
emu.registerafter(function()
    if done then return end
    frame = frame + 1
    local gd = gui.gdscreenshot()
    if gd and #gd > 11 then
        local path = string.format("%s/frame_%06d.gd2", outdir, frame)
        local f = assert(io.open(path, "wb"))
        f:write(gd)
        f:close()
    end
    if limit > 0 and frame >= limit then
        done = true
        io.stderr:write(string.format("[screenshot_range] done: %d frames → %s\n", frame, outdir))
        os.exit(0)
    end
end)
