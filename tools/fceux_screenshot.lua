-- fceux_screenshot.lua  (Lua 5.1, FCEUX 2.6+)
-- Capture a PNG screenshot at a specific frame during FM2 playback.
--
-- Env:
--   SCREENSHOT_FRAME  N     (default 1) — frame number to capture
--   SCREENSHOT_OUT    path  (default /tmp/fceux_frame.png)
--
-- Usage (via tools/fceux_screenshot.sh or directly):
--   SCREENSHOT_FRAME=116 SCREENSHOT_OUT=/tmp/ref_116.png \
--     fceux --sound 0 --loadlua tools/fceux_screenshot.lua \
--           --playmov fm2/Felix.fm2 rom/Felix.nes

local target = tonumber(os.getenv("SCREENSHOT_FRAME") or "1") or 1
local outpath = os.getenv("SCREENSHOT_OUT") or "/tmp/fceux_frame.png"

emu.speedmode("nothrottle")

local frame, done = 0, false
emu.registerafter(function()
    if done then return end
    frame = frame + 1
    if frame == target then
        done = true
        -- gui.savescreenshot saves current frame as PNG to FCEUX screenshot dir.
        -- We use emu.screenshot(path) if available, otherwise fall back to GD2 dump.
        local ok = false
        if type(emu.screenshot) == "function" then
            emu.screenshot(outpath)
            ok = true
        end
        if not ok then
            -- Fallback: dump raw GD2 pixels; caller converts via Python
            local gd = gui.gdscreenshot()
            if gd and #gd > 11 then
                local f = assert(io.open(outpath .. ".gd2", "wb"))
                f:write(gd)
                f:close()
            end
        end
        io.stderr:write(string.format("[screenshot] frame %d → %s\n", frame, outpath))
        os.exit(0)
    end
end)
