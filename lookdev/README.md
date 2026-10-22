# DS1 lookdev

The game runs vanilla in its own window while a separate host renders its frames
through the proxy and RTX Remix. The RTX side can then be changed and checked
without restarting the game.

## Use

```
powershell -File lookdev.ps1          # writes DATA\remix-comp-proxy.lookdev.ini, starts DARKSOULS.exe -lookdev
python lookdev.py host --live         # Remix window that follows the running game frame by frame
python lookdev.py capture [name]      # or F9 in game: records the next frame to a .ds1cap
python lookdev.py host [name]         # Remix window replaying one captured frame
python lookdev.py replay [name]       # plain-D3D9 replay, compared with the game's frame -> <name>_replay\compare.png
python lookdev.py view [name]         # plain-D3D9 viewer window: 1 replay, 2 vanilla, 3 difference
python lookdev.py info [name]         # what is in a capture
```

Captures are stored in `DATA\rtx_comp\lookdev\`. In the host window, F4 opens the
proxy menu and Alt+X opens the Remix menu, as in the game.

### Iterating on the proxy

1. Build it with `..\build.bat release --name DarkSouls --no-deploy`. This leaves the
   running game's `d3d9.dll` alone.
2. Run `lookdev.py host --live` again. It copies the new build to
   `DATA\d3d9_lookdev.dll`, restarts the host, and the game resends everything.

Changes to `rtx.conf` and Remix settings work live through Alt+X.

## Parts

| Piece | What it is |
|---|---|
| `src\comp\modules\lookdev_capture.cpp` (proxy, game side) | One-frame capture (F9) and the live stream. It hooks the real device below the proxy's wrapper and reads back the effective state at every draw. |
| `src\shared\lookdev\capture_format.hpp` | The record format shared by files and the stream, and the stream's shared-memory layout. |
| `host\main.cpp` → `ds1_lookdev_host.exe` (x86) | Loads a copy of the proxy in host mode with `-lookdev-host`, creates the device through it, and replays a file or the stream into Remix. |
| `viewer\main.cpp` → `ds1_lookdev_viewer.exe` (x64) | Plain-D3D9 replay with a pixel comparison, to show that a capture is complete. |
| `common\replay.hpp` | The replay, shared by both tools. |
| `src\comp\game\host_feed.cpp` (proxy) | Game state the host has no memory to read: lights, the camera override, the player position, the map ID and texture names. All of it comes from the capture. |

### How lookdev mode stays out of the way

- The proxy reads `remix-comp-proxy.lookdev.ini` only when started with `-lookdev`.
  A normal start from Steam is untouched.
- In lookdev mode the device wrapper passes draws and render targets straight to the
  device.
- The lookdev INI is regenerated from the normal one on every `lookdev.ps1` run, with
  these off: Remix, FFP, skinning, lights and sun. `[Lookdev] Enabled=1` is set.

### How the host stays out of the way

- It runs from `DATA`, because Remix resolves `rtx.conf` and the mods folder from the
  client exe's folder.
- It uses its own proxy copy, `d3d9_lookdev.dll`.
- It loads the system `d3d9.dll` first. Otherwise `d3dx9_43`'s import would bind to
  the game's proxy in `DATA`.
- In host mode the proxy skips every game patch, hook and memory read (`main.cpp`,
  `render_toggles`).
- It owns the Remix bridge, so it cannot run beside the normal (Remix) game.

### Live stream

- The host creates a 128 MB shared-memory ring (`Local\ds1_lookdev_stream`). The game
  polls for it every 30 frames and, once found, streams every frame into it.
- The first frame sends everything in use. After that only state changes, new
  resources, buffer and texture writes, and resources whose address was reused
  (`REC_DESTROY`) are sent.
- While the host is behind, the game waits for ring space, and the host applies state
  without drawing until it has caught up.
- Restarting the host makes the game resend everything.

## Known gaps

- Depth buffers and static `D3DPOOL_DEFAULT` textures cannot be read back. The capture
  log line counts them as "unreadable".
- Writes made through a surface `LockRect` are not hooked. `UpdateSurface` sources are
  re-read instead.
- The host keeps every resource the game frees until its address is reused, so its
  memory grows over a long session.

## Build

- Proxy: `..\build.bat release --name DarkSouls`. It deploys to the game; add
  `--no-deploy` to build for the host only.
- Tools: `build_lookdev.bat` builds the x64 viewer and the x86 host into `bin\`.
