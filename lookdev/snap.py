"""Instant side-by-side of the running game and the live Remix host.

    python snap.py [--out DIR] [--wait-settled] [--tag NAME]

Grabs both windows with PrintWindow (no focus change, nothing covering them matters)
and writes <out>/game.png, <out>/remix.png and <out>/ab.png (game left, Remix right).
--wait-settled first waits until the host frame stops changing (denoiser/exposure
converged, shaders compiled), polling every 0.25 s, so a host restart or a setting
change is captured as soon as it is visible and no sooner.
"""
import argparse
import ctypes
import sys
import time
from ctypes import wintypes
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
import suite  # noqa: E402  (PrintWindow helpers)

user32 = ctypes.windll.user32
OUT = Path(r"<projects>\ds1-comp-proxy-nv\lookdev\snaps")


def find_window(match) -> int:
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def enum(hwnd, _):
        n = user32.GetWindowTextLengthW(hwnd)
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        if user32.IsWindowVisible(hwnd) and match(buf.value):
            found.append(hwnd)
        return True

    user32.EnumWindows(enum, 0)
    return found[0] if found else 0


def game_window() -> int:
    return find_window(lambda t: t.strip().upper() == "DARK SOULS")


def grab(hwnd) -> Image.Image | None:
    return suite.print_client(hwnd) if hwnd else None


SNAP_DIR = Path(r"<game dir>\DATA\rtx_comp\lookdev")


def grab_game(timeout: float = 5.0) -> Image.Image | None:
    """The game's own presented frame, from the proxy (lookdev_capture.cpp snap_*): it does not
    depend on the window being visible. Falls back to PrintWindow on an older proxy."""
    out = SNAP_DIR / "snap_game.bmp"
    req = SNAP_DIR / "snap.request"
    before = out.stat().st_mtime if out.exists() else 0.0
    req.write_text("snap")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if out.exists() and out.stat().st_mtime > before and not req.exists():
            img = Image.open(out)
            img.load()
            return img.convert("RGB")
        time.sleep(0.05)
    if req.exists():
        req.unlink()
    return grab(game_window())


def wait_settled(timeout: float, tol: float = 0.6, polls: int = 4) -> float:
    start = time.time()
    prev, same = None, 0
    while time.time() - start < timeout:
        img = grab(suite.host_window())
        if img is not None:
            cells = np.asarray(img.convert("RGB").resize((16, 9), Image.BOX), dtype=np.float32)
            if prev is not None and np.abs(cells - prev).mean() < tol:
                same += 1
                if same >= polls:
                    break
            else:
                same = 0
            prev = cells
        time.sleep(0.25)
    return time.time() - start


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(OUT))
    ap.add_argument("--tag", default="")
    ap.add_argument("--wait-settled", action="store_true")
    ap.add_argument("--timeout", type=float, default=120)
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--capture", help="game image = this capture's reference frame (host replaying the file)")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    if args.wait_settled:
        print(f"settled after {wait_settled(args.timeout):.1f} s")
    if args.capture:
        import look
        import lookdev
        g = look.reference_image(lookdev.resolve(args.capture)).convert("RGB").resize((1920, 1080), Image.LANCZOS)
    else:
        g = grab_game()
    r = grab(suite.host_window())
    if g is None or r is None:
        print(f"missing window: game={g is not None} host={r is not None}")
        return 1
    tag = f"_{args.tag}" if args.tag else ""
    g.save(out / f"game{tag}.png")
    r.save(out / f"remix{tag}.png")
    w = args.width // 2
    h = int(w * g.height / g.width)
    sheet = Image.new("RGB", (2 * w, h), (0, 0, 0))
    sheet.paste(g.convert("RGB").resize((w, h), Image.LANCZOS), (0, 0))
    sheet.paste(r.convert("RGB").resize((w, h), Image.LANCZOS), (w, 0))
    d = ImageDraw.Draw(sheet)
    d.text((6, 4), "game", fill=(255, 255, 0))
    d.text((w + 6, 4), f"remix {args.tag}", fill=(255, 255, 0))
    sheet.save(out / f"ab{tag}.png")
    ga = np.asarray(g.convert("RGB"), dtype=np.float32)
    ra = np.asarray(r.convert("RGB").resize(g.size), dtype=np.float32)
    print(f"game {g.size} mean {ga.reshape(-1,3).mean(0).round(1)}  remix mean {ra.reshape(-1,3).mean(0).round(1)}")
    print(out / f"ab{tag}.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
