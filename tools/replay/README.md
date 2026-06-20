# tools/replay — FM2 → Mesen replay & screenshots

## Files

| File | Purpose |
|------|---------|
| `fm2_replay.py` | Convert FM2 → binary replay + Mesen Lua script |
| `fm2_to_mesen.py` | Convert FM2 → .mmo (Mesen movie format) |

## Prerequisites

1. **Mesen** at `./Mesen/Mesen` with IO access enabled:
   - Already set: `Mesen/settings.json` → `"AllowIoOsAccess": true`
   - If reconfiguring: Edit → Preferences → Debug → Allow IO/OS access

2. **Python 3 + Pillow** (for raw → PNG conversion):
   ```bash
   pip install Pillow
   ```

## Quick start — Mario replay + screenshots

```bash
# 1. Convert FM2 → replay files
python3 tools/replay/fm2_replay.py fm2/Mario.fm2 /tmp/mario

# 2. Run headless (supply the correct ROM path)
./Mesen/Mesen --testRunner --doNotSaveSettings /tmp/mario.lua rom/Mario.nes

# 3. Convert raw → PNG
python3 tools/replay/fm2_replay.py --convert-png /tmp/mario_shots/
```

Screenshots land in `/tmp/mario_shots/frame_0060.png`, `/tmp/mario_shots/frame_0120.png`, etc.

## Options

```bash
# Custom screenshot interval (every 300 frames = ~5s)
python3 tools/replay/fm2_replay.py fm2/Mario.fm2 /tmp/mario --shot-interval 300

# Convert FM2 to .mmo (Mesen movie format)
python3 tools/replay/fm2_to_mesen.py fm2/Mario.fm2 -o Mario.mmo
```

## How it works

```
FM2 → fm2_replay.py → .bin (raw input bytes) + .lua (Mesen script)
                                           ↓
                              Mesen --testRunner → reads .bin
                              plays back inputs, saves .raw frames
                                           ↓
                              fm2_replay.py --convert-png → .png
```

The Lua script uses `emu.setInput()` per frame via `inputPolled` callback
and `emu.getScreenBuffer()` to capture frames. No Mesen movie API needed.
