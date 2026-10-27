"""Multi-scene regression for the Remix look: every capture in the suite rendered
through the proxy and RTX Remix, next to the frame the game presented.

    python lookdev.py suite add <capture>...     put captures in the suite (rtx_comp\\lookdev\\suite.txt)
    python lookdev.py suite remove <capture>...
    python lookdev.py suite list
    python lookdev.py suite run [--tag T] [--only a,b] [--set rtx.key=value]...
                                                 (--set overrides rtx.conf for the run, then restores it)
                                                 render them all -> rtx_comp\\lookdev\\suite\\<T>\\
    python lookdev.py suite compare <tagA> <tagB>
    python lookdev.py suite sheet <tag>... [--out file.png]   game | each run, one row per scene

A run writes, per capture, <name>.png (the Remix frame) and <name>_ab.png (game
above Remix), plus sheet.png (every scene, game | Remix) and metrics.json. The
metrics are mean sRGB colours in a 3x3 grid of each frame for the game and for
Remix, so a change can be checked against every scene at once instead of tuning
one area and breaking another.
"""
import argparse
import ctypes
import datetime
import json
import shutil
import subprocess
import threading
import time
from ctypes import wintypes
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageGrab

import lookdev

SUITE_FILE = lookdev.CAPTURE_DIR / "suite.txt"
SUITE_DIR = lookdev.CAPTURE_DIR / "suite"
HOST_TITLE = "ds1 lookdev"
GRID = 3

user32 = ctypes.windll.user32
user32.SetProcessDPIAware()


# ------------------------------------------------------------------ suite list

def suite_names() -> list[str]:
    if not SUITE_FILE.exists():
        return []
    return [line.strip() for line in SUITE_FILE.read_text(encoding="utf-8").splitlines() if line.strip() and not line.startswith("#")]


def save_names(names: list[str]) -> None:
    SUITE_FILE.write_text("# captures rendered by `lookdev.py suite run`, one per line\n" + "\n".join(names) + "\n", encoding="utf-8")


# ------------------------------------------------------------------ host control

def stop_host() -> None:
    """Closes the lookdev host cleanly and waits for its bridge to exit (it holds the runtime)."""
    lookdev.stop_host()


def start_host(source: list[str]) -> None:
    lookdev.require_deploy_switch()  # copies into GAME_DIR: not the live set's DATA without deploy.conf
    shutil.copy2(lookdev.HOST, lookdev.HOST_IN_GAME)
    shutil.copy2(lookdev.PROXY_BUILD, lookdev.PROXY_IN_GAME)
    lookdev.deploy_game_proxy()
    subprocess.Popen([str(lookdev.HOST_IN_GAME), *source, "--proxy", lookdev.PROXY_IN_GAME.name, "-lookdev-host", "-nobeep"],
                     cwd=str(lookdev.GAME_DIR), creationflags=subprocess.CREATE_NEW_CONSOLE | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
    lookdev.lower_host_priority()


def host_window() -> int:
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def enum(hwnd, _):
        n = user32.GetWindowTextLengthW(hwnd)
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        if user32.IsWindowVisible(hwnd) and HOST_TITLE in buf.value.lower():
            found.append(hwnd)
        return True

    user32.EnumWindows(enum, 0)
    return found[0] if found else 0


def print_window(hwnd: int, width: int, height: int) -> Image.Image | None:
    """The window's client area as DWM composed it (PrintWindow with
    PW_CLIENTONLY | PW_RENDERFULLCONTENT): what the host drew, whatever covers it
    on screen. None when the call fails or returns a black image."""
    gdi32 = ctypes.windll.gdi32
    gdi32.CreateCompatibleBitmap.restype = wintypes.HANDLE
    gdi32.CreateCompatibleDC.restype = wintypes.HANDLE
    gdi32.SelectObject.restype = wintypes.HANDLE
    user32.GetDC.restype = wintypes.HANDLE
    screen_dc = user32.GetDC(hwnd)
    mem_dc = gdi32.CreateCompatibleDC(wintypes.HANDLE(screen_dc))
    bitmap = gdi32.CreateCompatibleBitmap(wintypes.HANDLE(screen_dc), width, height)
    old = gdi32.SelectObject(wintypes.HANDLE(mem_dc), wintypes.HANDLE(bitmap))
    try:
        if not user32.PrintWindow(hwnd, wintypes.HANDLE(mem_dc), 3):
            return None

        class BITMAPINFOHEADER(ctypes.Structure):
            _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG), ("biHeight", wintypes.LONG),
                        ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                        ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
                        ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD),
                        ("biClrImportant", wintypes.DWORD)]

        info = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), width, -height, 1, 32, 0, 0, 0, 0, 0, 0)
        buffer = ctypes.create_string_buffer(width * height * 4)
        if not gdi32.GetDIBits(wintypes.HANDLE(mem_dc), wintypes.HANDLE(bitmap), 0, height, buffer,
                               ctypes.byref(info), 0):
            return None
        image = Image.frombuffer("RGB", (width, height), buffer.raw, "raw", "BGRX", 0, 1)
        return image if image.getextrema() != ((0, 0), (0, 0), (0, 0)) else None
    finally:
        gdi32.SelectObject(wintypes.HANDLE(mem_dc), wintypes.HANDLE(old))
        gdi32.DeleteObject(wintypes.HANDLE(bitmap))
        gdi32.DeleteDC(wintypes.HANDLE(mem_dc))
        user32.ReleaseDC(hwnd, wintypes.HANDLE(screen_dc))


class HostDown(RuntimeError):
    """The host or its Remix bridge crashed or stopped responding."""


def print_client(hwnd: int, timeout: float = 15.0) -> Image.Image | None:
    """print_window of the whole client area, on a worker thread: PrintWindow waits on the
    window's thread, and a crash dialog would block it forever.

    Raises:
        HostDown: the window did not answer within `timeout` seconds.
    """
    rect = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    result: list = []
    worker = threading.Thread(target=lambda: result.append(print_window(hwnd, rect.right, rect.bottom)), daemon=True)
    worker.start()
    worker.join(timeout)
    if worker.is_alive():
        raise HostDown(f"host window did not answer PrintWindow within {timeout:.0f} s")
    return result[0] if result else None


SETTLE_GRID = 12          # the frame is compared as a SETTLE_GRID x SETTLE_GRID grid of mean colours
SETTLE_TOLERANCE = 1.0    # mean absolute change (sRGB 0-255) below which two polls count as the same
SETTLE_POLLS = 3          # consecutive unchanged polls, one second apart, that make a frame settled


def wait_until_rendered(started: float, timeout: float) -> None:
    """Returns once the host shows a settled frame: textures streamed in, the denoiser converged
    and exposure adapted, i.e. SETTLE_POLLS polls in a row within SETTLE_TOLERANCE of each other.
    Also returns when the host dies (the caller checks host_health) or after `timeout` seconds."""
    previous, unchanged = None, 0
    while time.time() - started < timeout:
        time.sleep(1.0)
        if host_health(started) is not None:
            return
        hwnd = host_window()
        image = print_client(hwnd) if hwnd else None
        if image is None:
            previous, unchanged = None, 0
            continue
        cells = np.asarray(image.convert("RGB").resize((SETTLE_GRID, SETTLE_GRID), Image.BOX), dtype=np.float32)
        if previous is not None and np.abs(cells - previous).mean() < SETTLE_TOLERANCE:
            unchanged += 1
            if unchanged >= SETTLE_POLLS:
                return
        else:
            unchanged = 0
        previous = cells


SENTRY_REPORTS = lookdev.GAME_DIR / "rtx-remix" / "logs" / ".sentry-native" / "reports"


def host_health(since: float) -> str | None:
    """None while the host and its bridge run; otherwise what went wrong, with the
    runtime's crash dump if one was written after `since`."""
    problems = []
    if not lookdev.running(lookdev.HOST_IN_GAME.name):
        problems.append("host exited")
    if not lookdev.running("NvRemixBridge.exe"):
        problems.append("Remix bridge exited (runtime crash)")
    hwnd = host_window()
    if hwnd and user32.IsHungAppWindow(hwnd):
        problems.append("host window not responding")
    if not problems:
        return None
    dumps = sorted((d for d in SENTRY_REPORTS.glob("*.dmp") if d.stat().st_mtime >= since),
                   key=lambda d: d.stat().st_mtime) if SENTRY_REPORTS.exists() else []
    if dumps:
        problems.append(f"dump {dumps[-1]}")
    return "; ".join(problems)


def grab_host(out: Path) -> None:
    """Screenshots the host window's client area once it is really the foreground
    window (a grab of whatever covers it would silently measure the wrong image).

    Raises:
        HostDown: the window hung while being grabbed.
        RuntimeError: no host window, or it never came to the front.
    """
    hwnd = host_window()
    if not hwnd:
        raise RuntimeError("no lookdev host window")
    for _ in range(10):
        user32.ShowWindow(hwnd, 9)
        # An Alt tap lets this process take the foreground from whoever has it.
        user32.keybd_event(0x12, 0, 0, 0)
        user32.keybd_event(0x12, 0, 2, 0)
        user32.SetForegroundWindow(hwnd)
        time.sleep(0.5)
        if user32.GetForegroundWindow() == hwnd:
            break
    else:
        raise RuntimeError("the lookdev host window did not come to the front")
    # Topmost while grabbed, so the taskbar does not cover the bottom of the frame.
    swp = 0x0001 | 0x0002 | 0x0010  # SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE
    user32.SetWindowPos(hwnd, -1, 0, 0, 0, 0, swp)
    try:
        time.sleep(1.0)
        rect = wintypes.RECT()
        user32.GetClientRect(hwnd, ctypes.byref(rect))
        origin = wintypes.POINT(0, 0)
        user32.ClientToScreen(hwnd, ctypes.byref(origin))
        image = print_client(hwnd)
        if image is None:
            image = ImageGrab.grab((origin.x, origin.y, origin.x + rect.right, origin.y + rect.bottom), all_screens=True)
        image.save(out)
    finally:
        user32.SetWindowPos(hwnd, -2, 0, 0, 0, 0, swp)


# ------------------------------------------------------------------ images and metrics

def vanilla_image(name: str) -> Path:
    """The frame the game presented for a capture, replaying it once if needed."""
    path = lookdev.CAPTURE_DIR / f"{name}_replay" / "vanilla.png"
    if not path.exists():
        lookdev.cmd_replay(argparse.Namespace(capture=name))
    return path


def grid_means(img: Image.Image) -> list[list[list[float]]]:
    """Mean sRGB colour (0-255) of each cell of a GRID x GRID split of the frame."""
    a = np.asarray(img.convert("RGB"), dtype=np.float32)
    h, w = a.shape[:2]
    return [[a[r * h // GRID:(r + 1) * h // GRID, c * w // GRID:(c + 1) * w // GRID].reshape(-1, 3).mean(0).round(1).tolist()
             for c in range(GRID)] for r in range(GRID)]


def scene_error(vanilla: list, remix: list) -> float:
    """Mean absolute sRGB difference over the grid cells and channels."""
    return float(np.abs(np.asarray(vanilla) - np.asarray(remix)).mean())


def ab_sheet(vanilla: Image.Image, remix: Image.Image, label: str, width: int = 960) -> Image.Image:
    h = int(width * vanilla.height / vanilla.width)
    out = Image.new("RGB", (width, 2 * h + 10), (20, 20, 20))
    out.paste(vanilla.convert("RGB").resize((width, h)), (0, 0))
    out.paste(remix.convert("RGB").resize((width, h)), (0, h + 10))
    draw = ImageDraw.Draw(out)
    draw.text((10, 8), f"{label}: game", fill=(255, 255, 0))
    draw.text((10, h + 18), f"{label}: Remix", fill=(255, 255, 0))
    return out


def contact_sheet(rows: list[tuple[str, Image.Image, Image.Image]], width: int = 640) -> Image.Image:
    h = int(width * 9 / 16)
    out = Image.new("RGB", (2 * width + 10, len(rows) * (h + 10)), (20, 20, 20))
    draw = ImageDraw.Draw(out)
    for i, (name, vanilla, remix) in enumerate(rows):
        y = i * (h + 10)
        out.paste(vanilla.convert("RGB").resize((width, h)), (0, y))
        out.paste(remix.convert("RGB").resize((width, h)), (width + 10, y))
        draw.text((8, y + 6), f"{name}: game", fill=(255, 255, 0))
        draw.text((width + 18, y + 6), f"{name}: Remix", fill=(255, 255, 0))
    return out


# ------------------------------------------------------------------ rtx.conf overrides

RTX_CONF = lookdev.GAME_DIR / "rtx.conf"


def apply_overrides(overrides: list[str]) -> bytes | None:
    """Writes `key = value` overrides into rtx.conf. Returns the original bytes to restore."""
    if not overrides:
        return None
    original = RTX_CONF.read_bytes()
    lines = original.decode("utf-8").splitlines()
    for item in overrides:
        key, value = (s.strip() for s in item.split("=", 1))
        lines = [line for line in lines if line.split("=", 1)[0].strip() != key]
        lines.append(f"{key} = {value}")
    RTX_CONF.write_bytes(("\n".join(lines) + "\n").encode("utf-8"))
    return original


# ------------------------------------------------------------------ commands

def cmd_run(args) -> int:
    names = suite_names()
    if args.only:
        wanted = args.only.split(",")
        names = [n for n in names if n in wanted]
    if not names:
        print(f"the suite is empty: python lookdev.py suite add <capture>... ({SUITE_FILE})")
        return 1
    tag = args.tag or datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = SUITE_DIR / tag
    out_dir.mkdir(parents=True, exist_ok=True)
    was_live = lookdev.running(lookdev.HOST_IN_GAME.name)

    metrics, rows, crashed = {}, [], {}
    lookdev.require_deploy_switch()  # rewrites GAME_DIR's rtx.conf for the run
    restore = apply_overrides(args.set)
    (out_dir / "settings.txt").write_text("\n".join(args.set) + "\n", encoding="utf-8")
    try:
        for name in names:
            cap = lookdev.resolve(name)
            vanilla_path = vanilla_image(cap.stem)
            stop_host()
            started = time.time()
            start_host([str(cap)])
            wait_until_rendered(started, args.wait)
            settled = time.time() - started
            remix_path = out_dir / f"{cap.stem}.png"
            problem = host_health(started)
            if problem is None:
                try:
                    grab_host(remix_path)
                except HostDown as e:
                    problem = host_health(started) or str(e)
            if problem is not None:
                crashed[cap.stem] = problem
                (out_dir / f"{cap.stem}.crash.txt").write_text(problem + "\n", encoding="utf-8")
                # The runtime's crash handler uploads and deletes its dumps on the next launch.
                for dump in SENTRY_REPORTS.glob("*.dmp") if SENTRY_REPORTS.exists() else []:
                    if dump.stat().st_mtime >= started:
                        shutil.copy2(dump, out_dir / f"{cap.stem}_{dump.name}")
                print(f"{cap.stem:24s} CRASHED: {problem}", flush=True)
                continue
            vanilla, remix = Image.open(vanilla_path), Image.open(remix_path)
            remix = remix.resize(vanilla.size)
            ab_sheet(vanilla, remix, cap.stem).save(out_dir / f"{cap.stem}_ab.png")
            v, r = grid_means(vanilla), grid_means(remix)
            metrics[cap.stem] = {"game": v, "remix": r, "error": round(scene_error(v, r), 2)}
            rows.append((cap.stem, vanilla, remix))
            print(f"{cap.stem:24s} error {metrics[cap.stem]['error']:6.2f}  (settled after {settled:.0f} s)", flush=True)
    finally:
        if restore is not None:
            RTX_CONF.write_bytes(restore)
        stop_host()
        if was_live and not args.no_live:
            start_host(["--live"])

    (out_dir / "metrics.json").write_text(json.dumps(metrics, indent=1), encoding="utf-8")
    if crashed:
        (out_dir / "crashed.json").write_text(json.dumps(crashed, indent=1), encoding="utf-8")
    if rows:
        contact_sheet(rows).save(out_dir / "sheet.png")
    mean = sum(m["error"] for m in metrics.values()) / len(metrics) if metrics else float("nan")
    print(f"mean error {mean:.2f} over {len(metrics)} scenes -> {out_dir}")
    for name, problem in crashed.items():
        print(f"CRASHED {name}: {problem}")
    return 1 if crashed else 0


def cmd_compare(args) -> int:
    a = json.loads((SUITE_DIR / args.a / "metrics.json").read_text(encoding="utf-8"))
    b = json.loads((SUITE_DIR / args.b / "metrics.json").read_text(encoding="utf-8"))
    print(f"{'scene':24s} {args.a:>14s} {args.b:>14s}   change")
    for name in sorted(set(a) | set(b)):
        ea, eb = a.get(name, {}).get("error"), b.get(name, {}).get("error")
        change = "" if ea is None or eb is None else f"{eb - ea:+.2f} {'better' if eb < ea else 'worse' if eb > ea else ''}"
        print(f"{name:24s} {ea if ea is not None else '-':>14} {eb if eb is not None else '-':>14}   {change}")
    return 0


def cmd_sheet(args) -> int:
    """One row per scene that every run rendered: the game's frame, then each run's."""
    runs = [SUITE_DIR / tag for tag in args.tags]
    names = [n for n in suite_names() if all((run / f"{n}.png").exists() for run in runs)]
    if not names:
        print("no scene was rendered by every run")
        return 1
    width = args.width
    height = int(width * 9 / 16)
    cols = 1 + len(runs)
    out = Image.new("RGB", (cols * (width + 6), len(names) * (height + 6)), (18, 18, 18))
    draw = ImageDraw.Draw(out)
    for row, name in enumerate(names):
        y = row * (height + 6)
        images = [(Image.open(vanilla_image(name)), f"{name}: game")]
        images += [(Image.open(run / f"{name}.png"), f"{name}: {run.name}") for run in runs]
        for col, (image, label) in enumerate(images):
            x = col * (width + 6)
            out.paste(image.convert("RGB").resize((width, height)), (x, y))
            draw.rectangle((x, y, x + 8 + 7 * len(label), y + 16), fill=(0, 0, 0))
            draw.text((x + 4, y + 3), label, fill=(255, 230, 0))
    target = Path(args.out) if args.out else SUITE_DIR / f"sheet_{'_'.join(args.tags)}.png"
    out.save(target)
    print(target)
    return 0


def cmd_add(args) -> int:
    names = suite_names()
    for n in args.captures:
        stem = lookdev.resolve(n).stem
        if stem not in names:
            names.append(stem)
    save_names(names)
    return cmd_list(args)


def cmd_remove(args) -> int:
    save_names([n for n in suite_names() if n not in set(args.captures)])
    return cmd_list(args)


def cmd_list(_args) -> int:
    names = suite_names()
    print(f"{len(names)} capture(s) in {SUITE_FILE}:")
    for n in names:
        print(f"  {n}")
    return 0


def add_parser(sub) -> None:
    p = sub.add_parser("suite", help="multi-scene regression (see suite.py)")
    s = p.add_subparsers(dest="suite_cmd", required=True)
    q = s.add_parser("run"); q.add_argument("--tag"); q.add_argument("--wait", type=float, default=90.0,
                   help="longest wait for a settled frame, seconds (it is usually there much sooner)")
    q.add_argument("--only", help="comma-separated captures to render instead of the whole suite")
    q.add_argument("--set", action="append", default=[], help="rtx.conf override for this run, key=value (repeatable)")
    q.add_argument("--no-live", action="store_true", help="do not restart the live host afterwards"); q.set_defaults(fn=cmd_run)
    q = s.add_parser("compare"); q.add_argument("a"); q.add_argument("b"); q.set_defaults(fn=cmd_compare)
    q = s.add_parser("sheet"); q.add_argument("tags", nargs="+"); q.add_argument("--out"); q.add_argument("--width", type=int, default=640)
    q.set_defaults(fn=cmd_sheet)
    q = s.add_parser("add"); q.add_argument("captures", nargs="+"); q.set_defaults(fn=cmd_add)
    q = s.add_parser("remove"); q.add_argument("captures", nargs="+"); q.set_defaults(fn=cmd_remove)
    q = s.add_parser("list"); q.set_defaults(fn=cmd_list)
