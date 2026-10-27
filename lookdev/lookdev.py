"""DS1 lookdev helper.

  python lookdev.py capture [name]      ask the running game (started by lookdev.ps1) for a capture
  python lookdev.py replay [capture]    replay a capture in the viewer and compare with the game's frame
  python lookdev.py view [capture]      open the viewer window on a capture
  python lookdev.py host [capture]      render a capture through the proxy and RTX Remix
  python lookdev.py host --live         follow the running game (lookdev.ps1) through the proxy and RTX Remix
  python lookdev.py info [capture]      record statistics of a capture
  python lookdev.py list                captures, newest first
  python lookdev.py look [capture]...   the game's look values (exposure, haze, grade, lights) in captures (look.py)
  python lookdev.py suite ...           multi-scene regression: every suite capture in Remix next to the game (suite.py)

[capture] is a .ds1cap path or a name in the game's rtx_comp\\lookdev folder;
it defaults to the newest capture.
"""
import argparse
import collections
import datetime
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

GAME_DATA = Path(r"<game dir>\DATA")
# DS1_LOOKDEV_GAME_DIR points the host at another folder laid out like DATA (its own .trex, rtx.conf and
# proxy inis, with rtx_comp and rtx-remix\mods linked to DATA's), so the nv set can render captures
# without touching the live set in DATA.
GAME_DIR = Path(os.environ.get("DS1_LOOKDEV_GAME_DIR", str(GAME_DATA)))
CAPTURE_DIR = GAME_DIR / "rtx_comp" / "lookdev"
HERE = Path(__file__).resolve().parent
VIEWER = HERE / "bin" / "ds1_lookdev_viewer.exe"
HOST = HERE / "bin" / "ds1_lookdev_host.exe"
PROXY_BUILD = HERE.parent / "build" / "bin" / "release" / "d3d9.dll"
# The host runs from DATA (Remix resolves rtx.conf and mods from the client exe's
# folder) with its own copy of the proxy.
HOST_IN_GAME = GAME_DIR / "ds1_lookdev_host.exe"
HOST_WINDOW_CLASS = "DARK SOULS lookdev host"   # host\main.cpp create_window
PROXY_IN_GAME = GAME_DIR / "d3d9_lookdev.dll"
# The normal game's proxy. Every host start deploys the same build there (deploy_game_proxy),
# so what lookdev shows is what the game runs on its next launch.
PROXY_FOR_GAME = GAME_DIR / "d3d9.dll"
PROXY_PDB_BUILD = PROXY_BUILD.with_name("DarkSouls-comp-proxy.pdb")
# This is the stock-NVIDIA (nv) proxy tree. It shares DATA with the live ds1-comp-proxy /
# ds1-dxvk-nr set, so nothing here writes into DATA until the game is switched over on purpose:
# rename ..\deploy.conf.DISABLED to deploy.conf (the same switch build.bat reads).
DEPLOY_SWITCH = HERE.parent / "deploy.conf"


def require_deploy_switch():
    if GAME_DIR.resolve() != GAME_DATA.resolve():
        return  # a separate host folder (DS1_LOOKDEV_GAME_DIR) is not the live set's DATA
    if not DEPLOY_SWITCH.exists():
        sys.exit(f"nv tree: {DEPLOY_SWITCH} is missing, so DATA still belongs to the live set. "
                 "Rename deploy.conf.DISABLED to deploy.conf to switch the game to the nv set.")


def deploy_file(src, dst):
    """Copy src over dst unless they already match (size and time; copy2 keeps the time).

    A DLL a running process has loaded can't be overwritten but can be renamed: a locked dst moves
    aside as dst.old-<time>, the new file takes its name, and the next launch loads it. Earlier
    moved-aside copies are removed once nothing has them loaded."""
    require_deploy_switch()
    import shutil
    for old in dst.parent.glob(dst.name + ".old-*"):
        try:
            old.unlink()
        except OSError:
            pass
    s = src.stat()
    if dst.exists():
        d = dst.stat()
        if d.st_size == s.st_size and int(d.st_mtime) == int(s.st_mtime):
            return "current"
    try:
        shutil.copy2(src, dst)
        return "deployed"
    except PermissionError:
        dst.rename(dst.with_name(f"{dst.name}.old-{int(time.time())}"))
        shutil.copy2(src, dst)
        return "deployed; a running game keeps the old one until it restarts"


def deploy_game_proxy():
    """The proxy build -> the game's d3d9.dll (and its PDB, for crash dumps)."""
    state = deploy_file(PROXY_BUILD, PROXY_FOR_GAME)
    if PROXY_PDB_BUILD.exists():
        deploy_file(PROXY_PDB_BUILD, GAME_DIR / PROXY_PDB_BUILD.name)
    stamp = datetime.datetime.fromtimestamp(PROXY_BUILD.stat().st_mtime)
    print(f"game proxy {PROXY_FOR_GAME.name} ({stamp:%H:%M:%S}): {state}")

MAGIC = 0x43315344
HEADER_SIZE = 156

# capture_format.hpp record_type
RECORD_NAMES = {
    1: "TEXTURE", 2: "CUBE_TEXTURE", 3: "VOLUME_TEXTURE", 4: "SURFACE", 5: "SURFACE_LEVEL", 6: "BACKBUFFER",
    7: "VERTEX_BUFFER", 8: "INDEX_BUFFER", 9: "VERTEX_SHADER", 10: "PIXEL_SHADER", 11: "VERTEX_DECL", 12: "DESTROY",
    32: "TEXTURE_DATA", 33: "SURFACE_DATA", 34: "BUFFER_DATA",
    64: "RENDER_STATE", 65: "SAMPLER_STATE", 66: "TEXTURE_STAGE_STATE", 67: "SET_TEXTURE", 68: "SET_VERTEX_SHADER",
    69: "SET_PIXEL_SHADER", 70: "SET_VERTEX_DECL", 71: "SET_FVF", 72: "VS_CONST_F", 73: "PS_CONST_F", 74: "VS_CONST_I",
    75: "PS_CONST_I", 76: "VS_CONST_B", 77: "PS_CONST_B", 78: "STREAM_SOURCE", 79: "STREAM_FREQ", 80: "INDICES",
    81: "RENDER_TARGET", 82: "DEPTH_STENCIL", 83: "VIEWPORT", 84: "SCISSOR", 85: "CLIP_PLANE", 86: "TRANSFORM",
    87: "MATERIAL", 88: "LIGHT", 89: "LIGHT_ENABLE",
    96: "DRAW", 97: "DRAW_INDEXED", 98: "DRAW_UP", 99: "DRAW_INDEXED_UP", 100: "CLEAR", 101: "STRETCH_RECT",
    102: "COLOR_FILL", 103: "UPDATE_SURFACE", 104: "UPDATE_TEXTURE", 105: "BEGIN_SCENE", 106: "END_SCENE", 107: "PRESENT",
    128: "REFERENCE_IMAGE", 129: "NOTE", 130: "GAME_FRAME", 131: "GAME_LIGHTS", 132: "TEXTURE_NAME",
    140: "STREAM_BEGIN",
}


def captures():
    if not CAPTURE_DIR.exists():
        return []
    return sorted(CAPTURE_DIR.glob("*.ds1cap"), key=lambda p: p.stat().st_mtime, reverse=True)


def resolve(arg):
    if not arg:
        found = captures()
        if not found:
            sys.exit(f"no captures in {CAPTURE_DIR}")
        return found[0]
    p = Path(arg)
    if p.exists():
        return p
    p = CAPTURE_DIR / (arg if arg.endswith(".ds1cap") else arg + ".ds1cap")
    if not p.exists():
        sys.exit(f"capture not found: {arg}")
    return p


def cmd_capture(args):
    name = args.name or datetime.datetime.now().strftime("frame_%Y%m%d_%H%M%S")
    CAPTURE_DIR.mkdir(parents=True, exist_ok=True)
    target = CAPTURE_DIR / f"{name}.ds1cap"
    if target.exists():
        target.unlink()
    (CAPTURE_DIR / "capture.request").write_text(name + "\n")
    print(f"requested {name}, waiting for the game...")
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        if target.exists():
            print(f"{target} ({target.stat().st_size / 2**20:.1f} MB)")
            return 0
        time.sleep(0.25)
    sys.exit("timed out: is the game running in lookdev mode (lookdev.ps1) and not paused in a menu?")


def bmp_to_png(bmp, png):
    from PIL import Image
    Image.open(bmp).convert("RGB").save(png)


def side_by_side(out_dir):
    from PIL import Image, ImageDraw
    names = [("vanilla", "game (vanilla)"), ("replay", "viewer replay"), ("diff", "difference x16")]
    imgs = [(label, Image.open(out_dir / f"{n}.bmp").convert("RGB")) for n, label in names if (out_dir / f"{n}.bmp").exists()]
    if not imgs:
        return None
    w, h = imgs[0][1].size
    scale = min(1.0, 960 / w)
    tw, th = int(w * scale), int(h * scale)
    sheet = Image.new("RGB", (tw * len(imgs), th + 24), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for i, (label, im) in enumerate(imgs):
        sheet.paste(im.resize((tw, th)), (i * tw, 24))
        draw.text((i * tw + 8, 6), label, fill=(230, 230, 230))
    path = out_dir / "compare.png"
    sheet.save(path)
    return path


def cmd_replay(args):
    cap = resolve(args.capture)
    if not VIEWER.exists():
        sys.exit(f"viewer not built: run {HERE / 'build_lookdev.bat'}")
    out_dir = cap.parent / (cap.stem + "_replay")
    proc = subprocess.run([str(VIEWER), str(cap), "--compare", "--out", str(out_dir)], capture_output=True, text=True)
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    for n in ("vanilla", "replay", "diff"):
        if (out_dir / f"{n}.bmp").exists():
            bmp_to_png(out_dir / f"{n}.bmp", out_dir / f"{n}.png")
    sheet = side_by_side(out_dir)
    if sheet:
        print(f"side by side: {sheet}")
    return proc.returncode


def running(image):
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {image}", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    return image.lower() in out.lower()


def pids(image):
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {image}", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    return [int(row.split('","')[1]) for row in out.splitlines() if row.lower().startswith(f'"{image.lower()}"')]


def lower_host_priority(timeout=30.0):
    """The host and its bridge run below the game's priority. The bridge raises itself to High, and
    after a runtime change it compiles shaders on most cores for minutes: at High that starved the
    game until DS1 kicked it to the title screen for low framerate (2026-10-23). Waits for the bridge
    to come up (it starts with the host's device)."""
    import ctypes
    kernel32 = ctypes.windll.kernel32
    BELOW_NORMAL_PRIORITY_CLASS, PROCESS_SET_INFORMATION = 0x4000, 0x0200
    lowered = set()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for image in (HOST_IN_GAME.name, "NvRemixBridge.exe"):
            for pid in pids(image):
                handle = kernel32.OpenProcess(PROCESS_SET_INFORMATION, False, pid)
                if handle:
                    if kernel32.SetPriorityClass(handle, BELOW_NORMAL_PRIORITY_CLASS):
                        lowered.add(image)
                    kernel32.CloseHandle(handle)
        if len(lowered) == 2:
            return True
        time.sleep(0.25)
    print(f"could not lower priority of: {sorted({HOST_IN_GAME.name, 'NvRemixBridge.exe'} - lowered)}")
    return False


def _wait_gone(image, timeout):
    deadline = time.monotonic() + timeout
    while running(image):
        if time.monotonic() > deadline:
            return False
        time.sleep(0.5)
    return True


def stop_host(timeout=45.0):
    """Closes the host the way its window's close button does, so the device is released and the
    bridge shuts Remix down cleanly: a killed Remix never writes its shader and pipeline caches,
    and the next start compiles every shader again. Kills only what is still running after
    `timeout` seconds (a hung host)."""
    import ctypes
    hwnd = ctypes.windll.user32.FindWindowW(HOST_WINDOW_CLASS, None)
    if hwnd:
        ctypes.windll.user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
    for image in (HOST_IN_GAME.name, "NvRemixBridge.exe"):
        if not _wait_gone(image, timeout):
            print(f"{image} did not exit within {timeout:.0f} s; killing it")
            subprocess.run(["taskkill", "/IM", image, "/F"], capture_output=True)
            _wait_gone(image, 5.0)


def cmd_host(args):
    cap = None if args.live else resolve(args.capture)
    for exe in (HOST, PROXY_BUILD):
        if not exe.exists():
            sys.exit(f"missing {exe}: run build_lookdev.bat and ..\\build.bat release --name DarkSouls --no-deploy")
    if running(HOST_IN_GAME.name):
        stop_host()
    if running("NvRemixBridge.exe"):
        sys.exit("NvRemixBridge.exe is running: the normal game (or another Remix client) owns the bridge. Close it first.")
    require_deploy_switch()
    import shutil
    shutil.copy2(HOST, HOST_IN_GAME)
    shutil.copy2(PROXY_BUILD, PROXY_IN_GAME)
    print(f"proxy {PROXY_BUILD} ({datetime.datetime.fromtimestamp(PROXY_BUILD.stat().st_mtime):%H:%M:%S}) -> {PROXY_IN_GAME.name}")
    deploy_game_proxy()
    source = ["--live"] if args.live else [str(cap)]
    subprocess.Popen([str(HOST_IN_GAME), *source, "--proxy", PROXY_IN_GAME.name, "-lookdev-host", "-nobeep"],
                     cwd=str(GAME_DIR), creationflags=subprocess.CREATE_NEW_CONSOLE | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
    lower_host_priority()
    print("host started, following the game live" if args.live else f"host started on {cap.name}")
    return 0


def cmd_view(args):
    cap = resolve(args.capture)
    subprocess.Popen([str(VIEWER), str(cap)], creationflags=subprocess.CREATE_NEW_CONSOLE)
    return 0


def cmd_info(args):
    cap = resolve(args.capture)
    data = cap.read_bytes()
    magic, version, header_size = struct.unpack_from("<III", data, 0)
    if magic != MAGIC:
        sys.exit("not a lookdev capture")
    fields = struct.unpack_from("<21IQ64s", data, 0)
    bb_w, bb_h, bb_fmt = fields[6], fields[7], fields[8]
    ini = fields[22].split(b"\0")[0].decode()
    print(f"{cap.name}: v{version}, frame {fields[21]}, back buffer {bb_w}x{bb_h} fmt {bb_fmt}, "
          f"INI {ini}, {len(data) / 2**20:.1f} MB")
    counts = collections.Counter()
    sizes = collections.Counter()
    pos = header_size
    while pos + 8 <= len(data):
        rtype, size = struct.unpack_from("<II", data, pos)
        pos += 8 + size
        name = RECORD_NAMES.get(rtype, f"type{rtype}")
        counts[name] += 1
        sizes[name] += size
    for name, n in sorted(counts.items(), key=lambda kv: -sizes[kv[0]]):
        print(f"  {name:22} {n:8}  {sizes[name] / 2**20:10.2f} MB")
    return 0


def cmd_list(_args):
    for p in captures():
        t = datetime.datetime.fromtimestamp(p.stat().st_mtime)
        print(f"{t:%Y-%m-%d %H:%M:%S}  {p.stat().st_size / 2**20:8.1f} MB  {p.name}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("capture"); p.add_argument("name", nargs="?"); p.add_argument("--timeout", type=float, default=60); p.set_defaults(fn=cmd_capture)
    p = sub.add_parser("replay"); p.add_argument("capture", nargs="?"); p.set_defaults(fn=cmd_replay)
    p = sub.add_parser("view"); p.add_argument("capture", nargs="?"); p.set_defaults(fn=cmd_view)
    p = sub.add_parser("host"); p.add_argument("capture", nargs="?"); p.add_argument("--live", action="store_true"); p.set_defaults(fn=cmd_host)
    p = sub.add_parser("info"); p.add_argument("capture", nargs="?"); p.set_defaults(fn=cmd_info)
    p = sub.add_parser("list"); p.set_defaults(fn=cmd_list)
    import suite
    suite.add_parser(sub)
    import look
    look.add_parser(sub)
    import probe_fit
    probe_fit.add_parser(sub)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
