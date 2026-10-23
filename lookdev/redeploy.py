"""Reload the live lookdev host with the latest builds and snapshot it next to the game:

    python redeploy.py [tag]

Stops the host cleanly (Remix keeps its shader caches), copies the runtime from
ds1-dxvk-nr\\_output into DATA\\.trex when it is newer, starts `host --live` (which copies the
proxy build), waits until the streamed map is up and the frame has settled, and runs snap.py.
The game is not touched: it streams without waiting on the host and resyncs by itself.
"""
import ctypes
import shutil
import subprocess
import sys
import time
from pathlib import Path

import lookdev
import snap
import suite

RUNTIME_BUILD = Path(r"<projects>\ds1-dxvk-nr\_output\d3d9.dll")
RUNTIME_PDB = Path(r"<projects>\ds1-dxvk-nr\_Comp64Release\src\d3d9\d3d9.pdb")
TREX = lookdev.GAME_DIR / ".trex"
BRIDGE_LOG = lookdev.GAME_DIR / "rtx-remix" / "logs" / "bridge64.log"


def host_title() -> str:
    h = suite.host_window()
    if not h:
        return ""
    buf = ctypes.create_unicode_buffer(512)
    ctypes.windll.user32.GetWindowTextW(h, buf, 512)
    return buf.value


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--capture=")]
    capture = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--capture=")), None)
    tag = args[0] if args else "redeploy"
    t0 = time.time()
    lookdev.stop_host()
    deployed = TREX / "d3d9.dll"
    if RUNTIME_BUILD.stat().st_mtime > deployed.stat().st_mtime:
        shutil.copy2(RUNTIME_BUILD, deployed)
        if RUNTIME_PDB.exists():
            shutil.copy2(RUNTIME_PDB, TREX / "d3d9.pdb")
        print(f"runtime {time.strftime('%H:%M:%S', time.localtime(RUNTIME_BUILD.stat().st_mtime))} deployed")
    subprocess.run([sys.executable, str(Path(__file__).with_name("lookdev.py")), "host",
                    *([capture] if capture else ["--live"])], check=True)
    deadline = time.time() + 180
    while time.time() < deadline:
        title = host_title()
        if (capture and title) or ("live, session" in title and "map 00000000" not in title and " map " in title):
            break
        if not lookdev.running(lookdev.HOST_IN_GAME.name):
            print("host exited")
            return 1
        time.sleep(0.5)
    # The window shows the plain D3D9 replay until the Remix device is up and rendering.
    while time.time() < deadline and not (BRIDGE_LOG.exists() and BRIDGE_LOG.stat().st_mtime > t0
                                          and "Server side D3D9 Device created" in BRIDGE_LOG.read_text(errors="ignore")):
        time.sleep(0.5)
    time.sleep(3.0)
    print(f"host streaming after {time.time() - t0:.0f} s: {host_title()[:100]}")
    return subprocess.run([sys.executable, str(Path(__file__).with_name("snap.py")), "--wait-settled",
                           "--timeout", "60", "--tag", tag, *(["--capture", capture] if capture else [])]).returncode


if __name__ == "__main__":
    sys.exit(main())
