# DS1 lookdev

The game runs vanilla in its own window while a separate host renders its frames
through the proxy and RTX Remix. The RTX side can then be changed and checked
without restarting the game.

## Use

```
powershell -File lookdev.ps1          # writes DATA\rtx_comp\settings\remix-comp-proxy.lookdev.ini, starts DARKSOULS.exe -lookdev
python lookdev.py host --live         # Remix window that follows the running game frame by frame
python lookdev.py capture [name]      # or F9 in game: records the next frame to a .ds1cap
python lookdev.py host [name]         # Remix window replaying one captured frame
python lookdev.py replay [name]       # plain-D3D9 replay, compared with the game's frame -> <name>_replay\compare.png
python lookdev.py view [name]         # plain-D3D9 viewer window: 1 replay, 2 vanilla, 3 difference
python lookdev.py info [name]         # what is in a capture
```

The same, double-clickable from this folder:

| File | Does |
|---|---|
| `lookdev_start_game.cmd` | `lookdev.ps1`: the game in lookdev mode |
| `lookdev_live.cmd` | `host --live`: the Remix window following the game (run again to restart it) |
| `lookdev_capture.cmd` | `capture`: asks for a name (Enter = timestamp) |
| `lookdev_replay.cmd` | `host <name>`: lists captures, asks which to replay in Remix (Enter = newest) |
| `lookdev_view.cmd` | `view <name>`: same, in the plain D3D9 viewer |

Each also takes the name as an argument.

Captures are stored in `DATA\rtx_comp\lookdev\`. In the host window, F4 opens the
proxy menu and Alt+X opens the Remix menu, as in the game.

F8 in the game freezes its input: keyboard, mouse and gamepad stop reaching the
game, so a camera lined up for a capture or the live host stays put while you use
the desktop. A pill at the top of the game window shows the state; F8 again (game
window focused) releases it. The pill never appears in captures or in the host.
`[Lookdev] FreezeInputKey` (a virtual-key code) changes the key.

`host [name] --drop i,j,...` leaves those draws (numbered as `analyze.py draws`
numbers them) out of the replay, to find which draw produces something.

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

The game must never slow down because of the host. Everything below follows from that.

- The host creates a 128 MB shared-memory ring (`Local\ds1_lookdev_stream`). The game
  polls for it every 30 frames and, once found, streams every frame into it.
- The game never waits for the host. It writes each frame into the space the host has
  already read and publishes it at its Present, whole frames only.
- **Full and no-draw frames.** The game sends a full frame (state and draws) only when
  the host has at most one published frame left to process (`frames_consumed`).
  Otherwise it sends a no-draw frame (`PRESENT_NO_DRAW`), which carries only what
  changes resources: creations, contents, the game's buffer writes, `UpdateSurface` and
  `UpdateTexture`, and `REC_DESTROY`. A no-draw frame skips the per-draw state
  read-back entirely, so it costs the game almost nothing. The next full frame carries
  the net state changes. The game therefore records full frames at the rate the host
  draws them.
- **Host skipping.** The host draws only the newest full, complete frame it has. It
  passes over the others, including older full frames, no-draw frames and incomplete
  frames. For those it creates resources and uploads texel contents, but it keeps
  state, constants, bindings and buffer writes in the replayer's shadow. The shadow is
  flushed as net changes before the next frame it draws. At each frame boundary it
  takes everything published, which frees the ring for the game.
- **Overflow and back-off.** A frame that does not fit in the ring is dropped
  unpublished, and the session ends. The game keeps running. The next session (a
  resync) starts only once the host is alive, has read everything, and has stayed
  caught up for a back-off time. The back-off is 2 s and doubles with each consecutive
  overflow, up to 60 s. A session that lasts 3 minutes resets it, and so does a new
  host. A hung host costs the game nothing: the stream stops until the host catches
  up or is restarted.
- **No GPU read-backs.** The stream never reads anything back from the GPU. Render
  targets, the back buffer and every other default-pool object are described but get
  no contents. The host fills them by replaying frames, and dynamic buffers get the
  game's own writes. File captures still read everything back.
- **Pacing.** The other contents, CPU-side copies of managed and system-memory
  objects, are paced by `[Lookdev] StreamBudgetKB` per frame (default 2048). If nothing
  else has gone that frame, one larger object may still go. An object over budget is
  described at once, and its contents follow at a later use. A resync (about 60 MB)
  takes about 30 full frames.
- **Copy sources.** The source of an `UpdateSurface` or `UpdateTexture` (system memory)
  is always sent in full at once.
- **Incomplete frames.** `PRESENT_INCOMPLETE` marks a frame that used an object whose
  contents are still to come. The host does not draw it, and its title shows
  "syncing". The game's log reports "Live stream in sync after N frames".
- **Cost log.** Every 10 s the game logs the recorder's own time per frame to
  `rtx_comp\console.log`: min / avg / max ms for full and no-draw frames, and MB/s.
  The line starts with "Live stream cost".
- Restarting the host makes the game resend everything.
- Both processes are 32-bit, so the shared 64-bit positions are read with interlocked
  operations. Plain 64-bit loads can tear.

## Known gaps

- Depth buffers and static `D3DPOOL_DEFAULT` textures cannot be read back. The capture
  log line counts them as "unreadable".
- Writes made through a surface `LockRect` are not hooked. `UpdateSurface` sources are
  re-read instead.
- The host keeps every resource the game frees until its address is reused, so its
  memory grows over a long session.
- Live stream: render targets and other default-pool objects start empty on the host.
  A frame the host does not draw also skips `StretchRect` and `ColorFill`, and a
  no-draw frame has none to skip. A target the game fills only once, in such a frame,
  stays wrong on the host until the game draws it again.

## Build

- Proxy: `..\build.bat release --name DarkSouls`. It deploys to the game; add
  `--no-deploy` to build for the host only.
- Tools: `build_lookdev.bat` builds the x64 viewer and the x86 host into `bin\`.
