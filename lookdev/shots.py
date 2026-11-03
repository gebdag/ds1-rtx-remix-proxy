"""Shoot many captures fast: one host, changing capture in place, so Remix and the mods load once (about 5 s a capture against 45 s).

    python shots.py <prefix> "settings" capture [capture ...]      saves snaps\\remix_<prefix><capture>.png and game_<prefix><capture>.png
    --fresh    restart the host first
    --keep     leave the host running

A capture with another back buffer size is refused by the host; it is shot after a restart. Prints each capture's frame mean, the game's,
and the error on a grid of cells over the part of the frame without the HUD.
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

import look
import lookdev
import shot

HERE = Path(__file__).parent
SNAPS = HERE / "snaps"
NEXT = lookdev.GAME_DIR / "lookdev_next_capture.txt"
LUMA = np.array([0.2126, 0.7152, 0.0722])


def stats(img, ref):
    a = np.asarray(img.convert("RGB").resize((480, 270), Image.BOX), np.float32)
    g = np.asarray(ref.convert("RGB").resize((480, 270), Image.BOX), np.float32)
    mask = np.ones((270, 480), bool)
    mask[:75, :] = False
    mask[75:, :150] = False
    mask[235:, 390:] = False
    la, lg = a @ LUMA, g @ LUMA
    cells = []
    for y in range(75, 270, 14):
        for x in range(150, 480, 14):
            c = mask[y:y + 14, x:x + 14]
            if c.sum() > 100:
                cells.append((la[y:y + 14, x:x + 14][c].mean(), lg[y:y + 14, x:x + 14][c].mean()))
    cells = np.array(cells)
    return float(la[mask].mean()), float(lg[mask].mean()), float(np.abs(cells[:, 0] - cells[:, 1]).mean()), float((la[mask] < 8).mean()), float((lg[mask] < 8).mean())


def start_fresh(path):
    source = path.name
    shot.start_host(source)
    end = time.time() + 120
    while not shot.showing(source) and time.time() < end:
        time.sleep(0.5)
    # The mods load after the first frames, once a host run.
    time.sleep(20)


def change_to(path):
    """True once the host shows it."""
    NEXT.write_text(str(path), encoding="utf-8")
    end = time.time() + 15
    while time.time() < end:
        if not NEXT.exists() and shot.showing(path.name):
            return True
        time.sleep(0.1)
    NEXT.unlink(missing_ok=True)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("settings")
    ap.add_argument("captures", nargs="+")
    ap.add_argument("--fresh", action="store_true")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if lookdev.running("DARKSOULS.exe"):
        sys.exit("DARKSOULS.exe is running: it owns the bridge")
    paths = [lookdev.resolve(c) for c in args.captures]
    start = time.time()
    running = lookdev.running(lookdev.HOST_IN_GAME.name) and not args.fresh
    if not running:
        start_fresh(paths[0])
    subprocess.run([sys.executable, "live.py", *args.settings.split()], cwd=HERE, capture_output=True, check=True)
    for i, path in enumerate(paths):
        t0 = time.time()
        if not (i == 0 and not running) and not change_to(path):
            print(path.stem, "needs a restart", flush=True)
            start_fresh(path)
            subprocess.run([sys.executable, "live.py", *args.settings.split()], cwd=HERE, capture_output=True, check=True)
        reference = look.reference_image(path)
        time.sleep(1.0)
        img = shot.wait_frame(reference, 60)
        img.save(SNAPS / f"remix_{args.prefix}{path.stem}.png")
        # A capture from the normal game has no game image.
        reference = reference or Image.new("RGB", img.size)
        reference.convert("RGB").save(SNAPS / f"game_{args.prefix}{path.stem}.png")
        s = stats(img, reference)
        print(f"{path.stem} ours {s[0]:5.1f} game {s[1]:5.1f} error {s[2]:5.1f} black {s[3]:.2f}/{s[4]:.2f}  ({time.time() - t0:.1f} s)", flush=True)
    if not args.keep:
        lookdev.stop_host()
    print(f"done in {time.time() - start:.0f} s", flush=True)


if __name__ == "__main__":
    main()
