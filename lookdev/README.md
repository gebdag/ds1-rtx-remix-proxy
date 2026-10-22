# DS1 lookdev

The game runs vanilla in its own window. Frames from it are replayed in a separate x64 viewer, where the
RTX side can be iterated on without restarting the game.

Status: phase 1–2 (frame capture + plain-D3D9 replay with a pixel-match check). Remix in the viewer, live
streaming and hot reload come next.

## Use

```
powershell -File lookdev.ps1          # writes DATA\remix-comp-proxy.lookdev.ini, starts DARKSOULS.exe -lookdev
python lookdev.py capture [name]      # or F9 in game: records the next frame
python lookdev.py replay [name]       # replays it, compares with the game's frame -> <name>_replay\compare.png
python lookdev.py view [name]         # viewer window: 1 replay, 2 vanilla, 3 difference
python lookdev.py info [name]         # what is in a capture
```

Captures live in `DATA\rtx_comp\lookdev\`.

## How it stays out of the way

- The proxy reads `remix-comp-proxy.lookdev.ini` only when started with `-lookdev`. A normal start from
  Steam is untouched.
- That INI is regenerated from the normal INI on every `lookdev.ps1` run, with these keys off: Remix, FFP,
  SkipPostFX, skinning, lights, sun. `[Lookdev] Enabled=1` is set.
- The capture module (`src\comp\modules\lookdev_capture.cpp`) is only registered with `[Lookdev] Enabled=1`.

## Capture format

See `src\shared\lookdev\capture_format.hpp`. Recording works like this:

- It hooks the **real** device below the proxy's wrapper, so the proxy's own draws are recorded too.
- At every draw it reads back the device's effective state and writes only what changed. State blocks
  therefore need no special handling.
- Resources are written at first use, with their contents at that moment. Writes during the frame are
  picked up through hooked buffer `Lock`/`Unlock` and texture `UnlockRect`.
- Render targets are read with `GetRenderTargetData`.
- Depth buffers and static `D3DPOOL_DEFAULT` textures cannot be read back. They are counted as
  "unreadable" in the capture log line.

## Build

- Proxy: `..\build.bat release --name DarkSouls` (deploys to the game).
- Viewer: `build_viewer.bat` (x64, output in `bin\`).
