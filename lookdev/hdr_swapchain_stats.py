"""Nits of a presented swap chain image dumped by the hdr10 runtime (rtx.hdr.dumpFrame -> Screenshots\\hdrSwapchain_*.dds).

    python hdr_swapchain_stats.py <dds> [--paper 200]

A2B10G10R10 (DXGI 24) is PQ-encoded BT.2020 with HDR10: decoded to nits, luminance stats and the share above paper
white are printed (what an HDR screenshot's MaxCLL sees). An 8-bit or 16-bit float dump is reported as SDR values.
"""
import struct
import sys
from pathlib import Path

import numpy as np

LUMA_2020 = np.array([0.2627, 0.6780, 0.0593], np.float32)


def pq_to_nits(e):
    m1, m2, c1, c2, c3 = 0.1593017578125, 78.84375, 0.8359375, 18.8515625, 18.6875
    p = np.power(np.clip(e, 0, 1), 1.0 / m2)
    return 10000.0 * np.power(np.maximum(p - c1, 0) / (c2 - c3 * p), 1.0 / m1)


def read(path: Path):
    data = path.read_bytes()
    height, width = struct.unpack_from("<II", data, 12)
    fourcc = data[84:88]
    dxgi = None
    offset = 128
    if fourcc == b"DX10":
        dxgi = struct.unpack_from("<I", data, 128)[0]
        offset = 148
    masks = struct.unpack_from("<4I", data, 92)
    # gli writes its own 'GLI1' four-CC with the bit masks: R10G10B10A2 is masks (0x3ff, 0xffc00, 0x3ff00000, 0xc0000000)
    # in some channel order; the runtime's swap chain is A2B10G10R10 (R in the low bits).
    if dxgi == 24 or (fourcc == b"GLI1" and 0xC0000000 in masks and 0x3FF in masks):
        raw = np.frombuffer(data, np.uint32, width * height, offset).reshape(height, width)
        r = (raw & 0x3FF).astype(np.float32) / 1023.0
        g = ((raw >> 10) & 0x3FF).astype(np.float32) / 1023.0
        b = ((raw >> 20) & 0x3FF).astype(np.float32) / 1023.0
        return "rgb10a2", np.stack([r, g, b], axis=2)
    if dxgi == 10 or struct.unpack_from("<I", data, 84)[0] == 113:
        px = np.frombuffer(data, np.float16, width * height * 4, offset).astype(np.float32).reshape(height, width, 4)
        return "rgba16f", px[..., :3]
    if dxgi in (28, 29, 87, 91) or struct.unpack_from("<I", data, 88)[0] == 32:
        px = np.frombuffer(data, np.uint8, width * height * 4, offset).reshape(height, width, 4).astype(np.float32) / 255.0
        return "rgba8", px[..., :3]
    raise ValueError(f"{path}: unknown pixel format fourcc={fourcc!r} dxgi={dxgi}")


def main() -> int:
    paper = 200.0
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if "--paper" in sys.argv:
        paper = float(sys.argv[sys.argv.index("--paper") + 1])
    kind, rgb = read(Path(args[0]))
    if kind == "rgb10a2":
        nits = pq_to_nits(rgb)
        y = nits @ LUMA_2020
        print(f"{kind} {rgb.shape[1]}x{rgb.shape[0]} PQ: luminance nits p50 {np.percentile(y, 50):.0f} p99 {np.percentile(y, 99):.0f} "
              f"p99.9 {np.percentile(y, 99.9):.0f} max {y.max():.0f} (MaxCLL-like: max channel {nits.max():.0f}); "
              f"share above paper white ({paper:.0f}) {(y > paper * 1.02).mean() * 100:.2f}%")
    else:
        print(f"{kind} {rgb.shape[1]}x{rgb.shape[0]}: max {rgb.max():.3f} p99.9 {np.percentile(rgb, 99.9):.3f} share>0.99 {(rgb.max(axis=2) > 0.99).mean() * 100:.2f}%")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
