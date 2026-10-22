"""DS1 lookdev helper.

  python lookdev.py capture [name]      ask the running game (started by lookdev.ps1) for a capture
  python lookdev.py replay [capture]    replay a capture in the viewer and compare with the game's frame
  python lookdev.py view [capture]      open the viewer window on a capture
  python lookdev.py host [capture]      render a capture through the proxy and RTX Remix
  python lookdev.py host --live         follow the running game (lookdev.ps1) through the proxy and RTX Remix
  python lookdev.py info [capture]      record statistics of a capture
  python lookdev.py list                captures, newest first

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

GAME_DIR = Path(r"<game dir>\DATA")
CAPTURE_DIR = GAME_DIR / "rtx_comp" / "lookdev"
HERE = Path(__file__).resolve().parent
VIEWER = HERE / "bin" / "ds1_lookdev_viewer.exe"
HOST = HERE / "bin" / "ds1_lookdev_host.exe"
PROXY_BUILD = HERE.parent / "build" / "bin" / "release" / "d3d9.dll"
# The host runs from DATA (Remix resolves rtx.conf and mods from the client exe's
# folder) with its own copy of the proxy, so a running game's d3d9.dll is never touched.
HOST_IN_GAME = GAME_DIR / "ds1_lookdev_host.exe"
PROXY_IN_GAME = GAME_DIR / "d3d9_lookdev.dll"

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


def cmd_host(args):
    cap = None if args.live else resolve(args.capture)
    for exe in (HOST, PROXY_BUILD):
        if not exe.exists():
            sys.exit(f"missing {exe}: run build_lookdev.bat and ..\\build.bat release --name DarkSouls --no-deploy")
    if running(HOST_IN_GAME.name):
        subprocess.run(["taskkill", "/IM", HOST_IN_GAME.name, "/F"], capture_output=True)
        time.sleep(1.0)
    if running("NvRemixBridge.exe"):
        sys.exit("NvRemixBridge.exe is running: the normal game (or another Remix client) owns the bridge. Close it first.")
    import shutil
    shutil.copy2(HOST, HOST_IN_GAME)
    shutil.copy2(PROXY_BUILD, PROXY_IN_GAME)
    print(f"proxy {PROXY_BUILD} ({datetime.datetime.fromtimestamp(PROXY_BUILD.stat().st_mtime):%H:%M:%S}) -> {PROXY_IN_GAME.name}")
    source = ["--live"] if args.live else [str(cap)]
    subprocess.Popen([str(HOST_IN_GAME), *source, "--proxy", PROXY_IN_GAME.name, "-lookdev-host", "-nobeep"],
                     cwd=str(GAME_DIR), creationflags=subprocess.CREATE_NEW_CONSOLE)
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
    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
