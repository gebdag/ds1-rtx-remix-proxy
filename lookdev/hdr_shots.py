"""HDR10 output analysis on lookdev captures (runtime branch hdr10, rtx.hdr.*).

    python hdr_shots.py <prefix> capture [capture ...] [--look on|off|both] [--settings "k=v ..."] [--keep] [--fresh]

For each capture the host renders it twice, HDR off and HDR on, and the runtime dumps its final frame both times
(rtx.hdr.dumpFrame -> GAME_DIR\\Screenshots\\hdrFinal_*.dds, 16-bit float, gamma-encoded, 1.0 = SDR white, above 1
the headroom). The dumps go to hdr_out\\<prefix>\\ with the metrics:

  - unchanged:  mean |hdr - sdr| in 8-bit steps over the pixels the SDR frame did not clip (should be the path
                tracing noise floor: the two frames are different renders);
  - clipped:    share of the frame SDR clipped (any channel at >= 254/255) and what the HDR frame holds there
                (headroom above white, in stops, and nits at the given paper white);
  - peak:       the 99.9th percentile and the maximum luminance of the HDR frame in nits, after the present
                roll-off (shoulder, peak);
  - hue:        chroma kept in the clipped pixels: saturation (max - min) / max of the HDR frame against the SDR one.

Images: <cap>_sdr.png (the SDR frame), <cap>_hdr_view.png (the HDR frame mapped to SDR for viewing: nits / peak,
gamma 2.2), <cap>_headroom.png (where the HDR frame exceeds SDR white: grey = within SDR, colour = stops above),
<cap>_hdr_window.png (PrintWindow of the HDR host window: what DWM hands an SDR capture).

The host runs from DS1_LOOKDEV_GAME_DIR (default: the nv set's own host folder <projects>\\ds1-nv-lookdev),
so the game's DATA is never touched. Its .trex\\d3d9.dll must be the hdr10 build.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

os.environ.setdefault("DS1_LOOKDEV_GAME_DIR", r"<projects>\ds1-nv-lookdev")

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
import look  # noqa: E402
import lookdev  # noqa: E402
import shot  # noqa: E402
import shots  # noqa: E402
import suite  # noqa: E402

HERE = Path(__file__).resolve().parent
OUT_ROOT = HERE / "hdr_out"
SCREENSHOTS = lookdev.GAME_DIR / "Screenshots"
LUMA = np.array([0.2126, 0.7152, 0.0722], np.float32)


def live(*settings: str) -> None:
    subprocess.run([sys.executable, "live.py", *settings], cwd=HERE, capture_output=True, check=True)


def read_dds(path: Path) -> np.ndarray:
    """A DDS of R16G16B16A16_SFLOAT (fourCC 113 or DX10 / DXGI 10) as float32 HxWx4."""
    data = path.read_bytes()
    if data[:4] != b"DDS ":
        raise ValueError(f"{path}: not a DDS")
    height, width = struct.unpack_from("<II", data, 12)
    fourcc = data[84:88]
    offset = 128
    if fourcc == b"DX10":
        dxgi = struct.unpack_from("<I", data, 128)[0]
        if dxgi != 10:
            raise ValueError(f"{path}: DXGI format {dxgi}, expected 10 (R16G16B16A16_FLOAT)")
        offset = 148
    elif struct.unpack_from("<I", data, 84)[0] != 113:
        raise ValueError(f"{path}: pixel format {fourcc!r} is not A16B16G16R16F")
    count = width * height * 4
    pixels = np.frombuffer(data, np.float16, count, offset).astype(np.float32)
    return pixels.reshape(height, width, 4)


def dump_frame(timeout: float = 20.0) -> Path:
    """Asks the runtime for a final-frame dump and returns the new file."""
    SCREENSHOTS.mkdir(exist_ok=True)
    before = {p: p.stat().st_mtime for p in SCREENSHOTS.glob("hdrFinal_*.dds")}
    # The option is cleared by the runtime after the dump; a second True through the same API layer did not
    # register once, so it goes False first.
    live("rtx.hdr.dumpFrame=False")
    time.sleep(0.5)
    live("rtx.hdr.dumpFrame=True")
    end = time.time() + timeout
    while time.time() < end:
        for p in SCREENSHOTS.glob("hdrFinal_*.dds"):
            if p not in before or p.stat().st_mtime > before[p]:
                # The exporter writes on its own thread: wait for the size to stop changing.
                size = -1
                while p.stat().st_size != size:
                    size = p.stat().st_size
                    time.sleep(0.3)
                return p
        time.sleep(0.2)
    raise RuntimeError("no hdrFinal dump appeared (is the runtime the hdr10 build?)")


def wait_frames(reference, timeout: float, settle_hdr: bool) -> Image.Image | None:
    """The host's frame once settled. In HDR mode the PrintWindow image may be anything DWM hands an SDR
    capture, so a settle failure is not fatal there."""
    try:
        return shot.wait_frame(None if settle_hdr else reference, timeout)
    except shot.ShotError as e:
        if settle_hdr:
            print(f"  (HDR window did not settle for PrintWindow: {e})", flush=True)
            time.sleep(3.0)
            try:
                return shot.grab()
            except shot.ShotError:
                return None
        raise


def decode(v: np.ndarray, gamma: float) -> np.ndarray:
    v = np.maximum(v, 0.0)
    if gamma > 0:
        return v ** gamma
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def present_nits(frame_rgb: np.ndarray, paper_white: float, peak: float, shoulder: float, gamma: float) -> np.ndarray:
    """The present shader's mapping (dxvk_present_frag_hdr.frag) without the PQ encode: linear nits."""
    lin = decode(frame_rgb, gamma) * paper_white
    y = lin @ LUMA
    s = shoulder * peak
    rng = max(peak - s, 1e-3)
    rolled = np.where(y > s, s + rng * (1.0 - np.exp(-(y - s) / rng)), y)
    lin = lin * (rolled / np.maximum(y, 1e-5))[..., None]
    m = lin.max(axis=2)
    lin = lin * np.where(m > peak, peak / np.maximum(m, 1e-5), 1.0)[..., None]
    return lin


def analyse(sdr: np.ndarray, hdr: np.ndarray, paper_white: float, peak: float, shoulder: float, gamma: float) -> dict:
    s = np.clip(sdr[..., :3], 0.0, None)
    h = np.clip(hdr[..., :3], 0.0, None)
    # What the SDR frame clipped: a channel at the top step of 8 bits.
    clipped = (s.max(axis=2) >= 254.0 / 255.0)
    # The region the SDR frame kept: compare the two frames there, in 8-bit steps.
    inside = ~clipped & (h.max(axis=2) <= 0.99)
    diff8 = np.abs(h - s).mean(axis=2) * 255.0
    unchanged = float(diff8[inside].mean()) if inside.any() else float("nan")
    unchanged_p99 = float(np.percentile(diff8[inside], 99)) if inside.any() else float("nan")
    # Headroom the HDR frame holds where SDR clipped, in stops above white (display linear).
    h_lin = decode(h, gamma)
    h_max = h_lin.max(axis=2)
    stops = np.log2(np.maximum(h_max, 1e-6))
    above = h_max > 1.0
    nits = present_nits(h, paper_white, peak, shoulder, gamma)
    y_nits = nits @ LUMA
    # Chroma kept in the clipped pixels.
    def sat(c):
        mx = c.max(axis=2)
        mn = c.min(axis=2)
        return np.where(mx > 1e-4, (mx - mn) / np.maximum(mx, 1e-4), 0.0)
    sat_s = sat(s)
    sat_h = sat(h_lin ** (1.0 / 2.2))
    return {
        "frame": [int(s.shape[1]), int(s.shape[0])],
        "unchanged_mean_8bit": unchanged,
        "unchanged_p99_8bit": unchanged_p99,
        "inside_share": float(inside.mean()),
        "sdr_clipped_share": float(clipped.mean()),
        "hdr_above_white_share": float(above.mean()),
        "clipped_headroom_stops_mean": float(stops[clipped].mean()) if clipped.any() else 0.0,
        "clipped_headroom_stops_p95": float(np.percentile(stops[clipped], 95)) if clipped.any() else 0.0,
        "headroom_stops_max": float(stops.max()),
        "nits_p50": float(np.percentile(y_nits, 50)),
        "nits_p99": float(np.percentile(y_nits, 99)),
        "nits_p999": float(np.percentile(y_nits, 99.9)),
        "nits_max": float(y_nits.max()),
        "nits_above_paper_share": float((y_nits > paper_white * 1.02).mean()),
        "clipped_saturation_sdr": float(sat_s[clipped].mean()) if clipped.any() else 0.0,
        "clipped_saturation_hdr": float(sat_h[clipped].mean()) if clipped.any() else 0.0,
        "paper_white": paper_white,
        "peak": peak,
        "shoulder": shoulder,
        "decode_gamma": gamma,
    }


def to_png(frame_rgb: np.ndarray) -> Image.Image:
    return Image.fromarray((np.clip(frame_rgb, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8), "RGB")


def view_png(hdr_rgb: np.ndarray, paper_white: float, peak: float, shoulder: float, gamma: float) -> Image.Image:
    """The HDR frame mapped for an SDR viewer: nits / peak, so the peak is white, encoded gamma 2.2."""
    nits = present_nits(hdr_rgb, paper_white, peak, shoulder, gamma)
    return to_png((nits / peak) ** (1.0 / 2.2))


def headroom_png(sdr_rgb: np.ndarray, hdr_rgb: np.ndarray, gamma: float) -> Image.Image:
    """Grey SDR frame; where the HDR frame exceeds SDR white, a colour by stops above (0 blue -> 2 red)."""
    grey = np.clip(sdr_rgb, 0, 1) @ LUMA
    base = np.stack([grey, grey, grey], axis=2) * 0.6
    h_max = decode(np.clip(hdr_rgb, 0, None), gamma).max(axis=2)
    stops = np.clip(np.log2(np.maximum(h_max, 1e-6)), 0.0, 2.0) / 2.0
    above = h_max > 1.0
    colour = np.stack([stops, 1.0 - np.abs(stops - 0.5) * 2.0, 1.0 - stops], axis=2)
    out = np.where(above[..., None], colour, base)
    return to_png(out)


def sheet(images: list[tuple[str, Image.Image]], width: int = 1920) -> Image.Image:
    w = width // len(images)
    h = int(w * images[0][1].height / images[0][1].width)
    canvas = Image.new("RGB", (w * len(images), h), (0, 0, 0))
    d = ImageDraw.Draw(canvas)
    for i, (label, img) in enumerate(images):
        canvas.paste(img.resize((w, h), Image.LANCZOS), (i * w, 0))
        d.text((i * w + 6, 4), label, fill=(255, 255, 0))
    return canvas


def run_capture(path: Path, out: Path, look_on: bool, settings: list[str], first: bool, hdr_settings: dict) -> dict:
    tag = f"{path.stem}_{'look' if look_on else 'stock'}"
    reference = look.reference_image(path)
    base = [f"rtx.ds1Look.enable={'True' if look_on else 'False'}", "rtx.hdr.enable=False", *settings]
    live(*base)
    time.sleep(1.0)
    wait_frames(reference, 90, False)
    sdr_dump = dump_frame()
    sdr_path = out / f"{tag}_sdr.dds"
    sdr_dump.replace(sdr_path)
    sdr_window = shot.grab()

    live(*base[:1], "rtx.hdr.enable=True", *settings)
    # The back buffers change at the next present, the swap chain the one after; then the frame settles again.
    time.sleep(2.0)
    hdr_window = wait_frames(reference, 60, True)
    hdr_dump = dump_frame()
    hdr_path = out / f"{tag}_hdr.dds"
    hdr_dump.replace(hdr_path)
    live(*base)
    # A second SDR dump after switching back: the SDR-to-SDR difference is the noise and drift floor
    # (path tracing noise, the game's meter still adapting) the HDR-to-SDR difference is judged against.
    time.sleep(2.0)
    wait_frames(reference, 60, False)
    sdr2_dump = dump_frame()
    sdr2_path = out / f"{tag}_sdr2.dds"
    sdr2_dump.replace(sdr2_path)

    sdr = read_dds(sdr_path)
    hdr = read_dds(hdr_path)
    sdr2 = read_dds(sdr2_path)
    metrics = analyse(sdr, hdr, **hdr_settings)
    drift = np.abs(sdr2[..., :3] - sdr[..., :3]).mean(axis=2) * 255.0
    metrics["floor_mean_8bit"] = float(drift.mean())
    metrics["floor_p99_8bit"] = float(np.percentile(drift, 99))
    sdr2_path.unlink()
    metrics["capture"] = path.stem
    metrics["look"] = look_on
    sdr_img = to_png(sdr[..., :3])
    view = view_png(hdr[..., :3], **hdr_settings)
    head = headroom_png(sdr[..., :3], hdr[..., :3], hdr_settings["gamma"])
    sdr_img.save(out / f"{tag}_sdr.png")
    view.save(out / f"{tag}_hdr_view.png")
    head.save(out / f"{tag}_headroom.png")
    sdr_window.save(out / f"{tag}_sdr_window.png")
    if hdr_window is not None:
        hdr_window.save(out / f"{tag}_hdr_window.png")
    sheet([("SDR frame", sdr_img), ("HDR frame, peak = white", view), ("headroom above SDR white", head)]).save(out / f"{tag}_sheet.png")
    return metrics


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prefix")
    ap.add_argument("captures", nargs="+")
    ap.add_argument("--look", choices=["on", "off", "both"], default="on", help="the game's image pipeline on (default), off (stock Remix tonemapper), or both")
    ap.add_argument("--settings", default="", help='extra live settings "k=v k=v" applied to every shot')
    ap.add_argument("--paper", type=float, default=200.0, help="paper white nits for the analysis mapping (also set in the runtime)")
    ap.add_argument("--peak", type=float, default=1000.0)
    ap.add_argument("--shoulder", type=float, default=0.75)
    ap.add_argument("--srgb", action="store_true", help="analyse with the sRGB piecewise decode (rtx.hdr.srgbDecode)")
    ap.add_argument("--fresh", action="store_true", help="restart the host first")
    ap.add_argument("--keep", action="store_true", help="leave the host running")
    args = ap.parse_args()

    if lookdev.running("DARKSOULS.exe"):
        sys.exit("DARKSOULS.exe is running: it owns the bridge")
    out = OUT_ROOT / args.prefix
    out.mkdir(parents=True, exist_ok=True)
    paths = [lookdev.resolve(c) for c in args.captures]
    hdr_settings = {"paper_white": args.paper, "peak": args.peak, "shoulder": args.shoulder, "gamma": 0.0 if args.srgb else 2.2}
    runtime_settings = [f"rtx.hdr.paperWhiteNits={args.paper}", f"rtx.hdr.peakNits={args.peak}", f"rtx.hdr.shoulderStart={args.shoulder}",
                        f"rtx.hdr.srgbDecode={'True' if args.srgb else 'False'}", *args.settings.split()]
    looks = [True, False] if args.look == "both" else [args.look == "on"]

    start = time.time()
    running = lookdev.running(lookdev.HOST_IN_GAME.name) and not args.fresh
    if not running:
        shots.start_fresh(paths[0])
    results = []
    for i, path in enumerate(paths):
        if not (i == 0 and not running) and not shots.change_to(path):
            print(path.stem, "needs a restart", flush=True)
            shots.start_fresh(path)
        for look_on in looks:
            t0 = time.time()
            m = run_capture(path, out, look_on, runtime_settings, i == 0, hdr_settings)
            results.append(m)
            print(f"{path.stem} {'look' if look_on else 'stock'}: unchanged {m['unchanged_mean_8bit']:.2f}/255 (p99 {m['unchanged_p99_8bit']:.1f}) over {m['inside_share']*100:.0f}%, "
                  f"floor {m['floor_mean_8bit']:.2f} (p99 {m['floor_p99_8bit']:.1f}) | "
                  f"SDR clipped {m['sdr_clipped_share']*100:.1f}% -> HDR above white {m['hdr_above_white_share']*100:.1f}%, "
                  f"+{m['clipped_headroom_stops_mean']:.2f} stops there (p95 {m['clipped_headroom_stops_p95']:.2f}, max {m['headroom_stops_max']:.2f}) | "
                  f"nits p99 {m['nits_p99']:.0f} p99.9 {m['nits_p999']:.0f} max {m['nits_max']:.0f} | "
                  f"clipped sat SDR {m['clipped_saturation_sdr']:.2f} HDR {m['clipped_saturation_hdr']:.2f}  ({time.time() - t0:.0f} s)", flush=True)
            (out / "metrics.json").write_text(json.dumps(results, indent=1), encoding="utf-8")
    if not args.keep:
        lookdev.stop_host()
    print(f"done in {time.time() - start:.0f} s -> {out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
