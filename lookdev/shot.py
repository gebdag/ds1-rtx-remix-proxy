"""Reliable, quick Remix snapshots of a capture (or the live game), one per settings set.

    python shot.py <capture | --live> [--fresh] [--timeout S] tag="key=value key=value" ...

Each tag gets snaps\\remix_<tag>.png (and snaps\\game_<tag>.png: the capture's reference frame, or
the live game) and one line of means. Every step checks itself and stops with a message instead
of waiting blind:
  - the host: reused when it already shows this capture (or the live game); otherwise started, and
    a start that fails says why (lookdev.py host output);
  - ready: the host's title shows the source at a frame rate, its window answers, and its frame
    is a Remix frame, not the raster fallback Remix shows while it compiles shaders (that frame is
    the game's own image: it matches the capture's reference frame);
  - each settings set: applied through live.py, then snapped as soon as the frame stops changing.
Live settings stay in the host until it restarts: --fresh restarts it first, so earlier runs'
settings are gone. remix_live.txt is put back afterwards.
"""
import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lookdev  # noqa: E402
import suite  # noqa: E402
import snap  # noqa: E402

HERE = Path(__file__).resolve().parent
OUT = HERE / "snaps"
LIVE_FILE = lookdev.CAPTURE_DIR / "remix_live.txt"
# A frame this close to the game's own image (mean absolute difference, 0-255) is the raster fallback.
RASTER_MATCH = 3.0


class ShotError(RuntimeError):
    pass


def host_title() -> str:
    import ctypes
    user32 = ctypes.windll.user32
    hwnd = suite.host_window()
    if not hwnd:
        return ""
    buf = ctypes.create_unicode_buffer(user32.GetWindowTextLengthW(hwnd) + 1)
    user32.GetWindowTextW(hwnd, buf, len(buf))
    return buf.value


def showing(source: str) -> bool:
    title = host_title()
    return bool(re.search(r"[\d.]+ fps", title)) and (source in title if source != "live" else "live" in title)


def start_host(source: str) -> None:
    args = ["--live"] if source == "live" else [source]
    out = subprocess.run([sys.executable, "lookdev.py", "host", *args], cwd=HERE, capture_output=True, text=True)
    if out.returncode != 0 or "host started" not in out.stdout:
        raise ShotError(f"host did not start: {(out.stdout + out.stderr).strip()}")


def grab() -> Image.Image:
    hwnd = suite.host_window()
    if not hwnd:
        raise ShotError("no host window (the host exited)")
    img = suite.print_client(hwnd)
    if img is None:
        raise ShotError("host window gave no image")
    return img.convert("RGB")


def is_raster(img: Image.Image, reference: Image.Image | None) -> bool:
    if reference is None:
        return False
    a = np.asarray(img.resize((192, 108), Image.BOX), dtype=np.float32)
    b = np.asarray(reference.resize((192, 108), Image.BOX), dtype=np.float32)
    return float(np.abs(a - b).mean()) < RASTER_MATCH


def wait_frame(reference: Image.Image | None, timeout: float) -> Image.Image:
    """The host's frame once it is a Remix frame that stopped changing."""
    end = time.time() + timeout
    prev, same, raster_seen = None, 0, False
    while time.time() < end:
        img = grab()
        if is_raster(img, reference):
            raster_seen, prev, same = True, None, 0
        else:
            cells = np.asarray(img.resize((suite.SETTLE_GRID, suite.SETTLE_GRID), Image.BOX), dtype=np.float32)
            if prev is not None and np.abs(cells - prev).mean() < suite.SETTLE_TOLERANCE:
                same += 1
                if same >= 3:
                    return img
            else:
                same = 0
            prev = cells
        time.sleep(0.25)
    raise ShotError("still compiling shaders (raster frame)" if raster_seen else "frame did not settle")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", nargs="?", help="capture name or path (omit with --live)")
    ap.add_argument("shots", nargs="*", help='tag="key=value ..." (no settings: tag=)')
    ap.add_argument("--live", action="store_true", help="the host follows the running game")
    ap.add_argument("--fresh", action="store_true", help="restart the host first (clears earlier live settings)")
    ap.add_argument("--timeout", type=float, default=90.0, help="seconds for the host to be ready, per step")
    args = ap.parse_intermixed_args()
    if args.live and args.source:
        args.shots.insert(0, args.source)
    if not args.live and not args.source:
        ap.error("give a capture or --live")
    source = "live" if args.live else lookdev.resolve(args.source).name
    shots = args.shots or ["now="]

    t0 = time.time()
    if args.fresh or not showing(source):
        start_host(source)
        end = time.time() + args.timeout
        while not showing(source):
            if time.time() > end:
                raise ShotError(f"host never showed {source}: title {host_title()!r}")
            time.sleep(0.25)
    import look
    reference = None if source == "live" else look.reference_image(lookdev.resolve(source))
    print(f"host ready ({host_title()}) after {time.time() - t0:.1f} s", flush=True)

    backup = LIVE_FILE.read_bytes() if LIVE_FILE.exists() else None
    OUT.mkdir(exist_ok=True)
    try:
        for shot in shots:
            tag, _, settings = shot.partition("=")
            t1 = time.time()
            if settings.strip():
                subprocess.run([sys.executable, "live.py", *settings.split()], cwd=HERE, capture_output=True, check=True)
            img = wait_frame(reference, args.timeout)
            game = reference if reference is not None else snap.grab_game()
            img.save(OUT / f"remix_{tag}.png")
            if game is not None:
                game.save(OUT / f"game_{tag}.png")
            r = np.asarray(img, dtype=np.float32).reshape(-1, 3).mean(0).round(1)
            g = np.asarray(game.convert("RGB"), dtype=np.float32).reshape(-1, 3).mean(0).round(1) if game is not None else None
            print(f"{tag}: remix {r} game {g} ({time.time() - t1:.1f} s)", flush=True)
    finally:
        if backup is not None:
            LIVE_FILE.write_bytes(backup)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ShotError, suite.HostDown) as e:
        print(f"shot failed: {e}", file=sys.stderr)
        sys.exit(1)
