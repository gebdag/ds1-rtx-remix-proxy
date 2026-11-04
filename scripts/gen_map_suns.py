"""Writes assets/sky/map_suns.txt for the lookdev host, which has no game memory to read: the same one sun per map that the proxy reads from the game's banks
in the running game (game/map_sun.cpp), computed from the game's files by the same rule.

    py -3.12 gen_map_suns.py

The rule. The banks of an area name their rows by block ("B1基本" = block 1, basic).
Direction: the block's SHADOW_BANK row named basic or, without one, outdoors (屋外).
Strength: the LIGHT_BANK row of the same name, else the block's strongest map row along that direction (no character, sky dome or far view rows): its probe
cube EnvDif_<area>_<id> times its env colour, fitted as a sky gradient plus a lobe along the direction.
Below MIN_SUN, or small against its ambient (MIN_SUN_SHARE), a map has no sun of its own: outdoor suns measure 0.31 and up at an ambient about their own size;
enclosed places reach 0.31 at an ambient of 2.5, where such a fit finds nothing real.
"""
import math
import struct
import sys
from pathlib import Path

import numpy as np

UNPACKED = Path(r"<game dir>\DATA\unpacked")
OUT = Path(__file__).resolve().parent.parent / "assets" / "sky" / "map_suns.txt"
NORMALS = np.array([[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]], dtype=np.float64)
WEIGHTS = np.array([0.2126, 0.7152, 0.0722])
BASIC = "基本".encode("cp932")
OUTDOORS = "屋外".encode("cp932")
NOT_MAP_LIGHT = tuple(s.encode("cp932") for s in ("キャラ", "ｷｬﾗ", "天球", "遠景", "中景"))
MIN_SUN = 0.25
MIN_SUN_SHARE = 0.25


def read_param(name):
    b = (UNPACKED / "param" / "DrawParam" / f"{name}.param").read_bytes()
    strings = struct.unpack_from("<I", b, 0)[0]
    count = struct.unpack_from("<H", b, 0xA)[0]
    heads = [struct.unpack_from("<iII", b, 0x30 + i * 12) for i in range(count)]
    rows = {}
    for i, (rid, off, name_off) in enumerate(heads):
        end = heads[i + 1][1] if i + 1 < count else strings
        rows[rid] = (b[name_off:b.index(b"\0", name_off)] if name_off else b"", b[off:end])
    return rows


def cube_faces(path):
    """Mean RGB of each face's top level, 0-1. DDS cube, A8R8G8B8 or DXT1."""
    b = path.read_bytes()
    height, width = struct.unpack_from("<II", b, 12)
    mips = max(struct.unpack_from("<I", b, 28)[0], 1)
    four = b[84:88]
    pos = 128
    out = []
    for _ in range(6):
        if four == b"DXT1":
            n = ((width + 3) // 4) * ((height + 3) // 4)
            blocks = np.frombuffer(b, np.uint8, n * 8, pos).reshape(-1, 8)
            c0 = blocks[:, 0].astype(np.uint32) | (blocks[:, 1].astype(np.uint32) << 8)
            c1 = blocks[:, 2].astype(np.uint32) | (blocks[:, 3].astype(np.uint32) << 8)
            rgb = lambda c: np.stack([((c >> 11) & 31) / 31.0, ((c >> 5) & 63) / 63.0, (c & 31) / 31.0], 1)
            a, d = rgb(c0), rgb(c1)
            codes = (blocks[:, 4:].astype(np.uint32) * np.array([1, 256, 65536, 16777216], dtype=np.uint32)).sum(1)
            sum_rgb = np.zeros(3)
            for t in range(16):
                k = (codes >> (2 * t)) & 3
                opaque = c0 > c1
                v = np.where((k == 0)[:, None], a, np.where((k == 1)[:, None], d,
                    np.where((k == 2)[:, None], np.where(opaque[:, None], (2 * a + d) / 3, (a + d) / 2),
                             np.where(opaque[:, None], (a + 2 * d) / 3, 0.0))))
                sum_rgb += v.sum(0)
            out.append(sum_rgb / (16 * n))
            step = 0
            w, h = width, height
            for _m in range(mips):
                step += ((w + 3) // 4) * ((h + 3) // 4) * 8
                w, h = max(w // 2, 1), max(h // 2, 1)
            pos += step
        else:
            bytes_per = struct.unpack_from("<I", b, 88)[0] // 8
            n = width * height
            px = np.frombuffer(b, np.uint8, n * bytes_per, pos).reshape(-1, bytes_per)
            out.append(px[:, [2, 1, 0]].mean(0) / 255.0)
            step = 0
            w, h = width, height
            for _m in range(mips):
                step += w * h * bytes_per
                w, h = max(w // 2, 1), max(h // 2, 1)
            pos += step
    return np.array(out)


def direction(rot_x, rot_y):
    x, y = math.radians(rot_x), math.radians(rot_y)
    return np.array([math.cos(x) * math.sin(y), -math.sin(x), math.cos(x) * math.cos(y)])


def fit(faces, d):
    lambert = np.clip(NORMALS @ -d, 0.0, None)
    t = NORMALS[:, 1] * 0.5 + 0.5
    a = np.stack([t, 1.0 - t, lambert], axis=1)
    coef, *_ = np.linalg.lstsq(a, faces, rcond=None)
    return np.clip(coef[0], 0, None), np.clip(coef[1], 0, None), np.clip(coef[2], 0, None)


def row_sun(area, s, d):
    cube = UNPACKED / "map" / area / f"EnvDif_{area}_{s[40]:03d}.dds"
    if not cube.exists():
        return None
    scale = np.array(s[32:35], float) / 255.0 * s[35] / 100.0
    up, down, lobe = fit(cube_faces(cube) * scale, d)
    hemi_up = np.array(s[18:21], float) / 255.0 * s[21] / 100.0
    hemi_down = np.array(s[22:25], float) / 255.0 * s[25] / 100.0
    base = np.zeros(3)
    for axis in range(3):
        v = -d[axis]
        face = (up + hemi_up if v >= 0 else down + hemi_down) if axis == 1 else 0.5 * (up + down + hemi_up + hemi_down)
        base += v * v * face
    return lobe, base


def map_sun(area, block):
    light, shadow = read_param(f"{area}_LightBank"), read_param(f"{area}_ShadowBank")
    tag = f"B{block}".encode()
    shadow_row = None
    for wanted in (tag + BASIC, tag + OUTDOORS) + ((BASIC,) if block == 0 else ()):
        for rid, (name, raw) in sorted(shadow.items()):
            if name == wanted and struct.unpack_from("<2h", raw) != (0, 0):
                shadow_row = (rid, name, raw)
                break
        if shadow_row:
            break
    if not shadow_row:
        return None
    d = direction(*struct.unpack_from("<2h", shadow_row[2]))
    best = None
    for rid, (name, raw) in sorted(light.items()):
        if not name.startswith(tag) or any(c in name for c in NOT_MAP_LIGHT):
            continue
        r = row_sun(area, struct.unpack_from("<46h", raw), d)
        if r is None:
            continue
        lum = float(r[0] @ WEIGHTS)
        if name == shadow_row[1]:
            best = (rid, name, r, lum)
            break
        if best is None or lum > best[3]:
            best = (rid, name, r, lum)
    if best is None or best[3] < MIN_SUN or best[3] < MIN_SUN_SHARE * float(best[2][1] @ WEIGHTS):
        return ("none", shadow_row, best)
    return (d, shadow_row, best)


def main():
    blocks = sorted(p.name for p in (UNPACKED / "map").glob("m[0-9][0-9]_[0-9][0-9]_00_00"))
    lines = []
    seen = set()
    for m in blocks:
        area, block = m[:3], int(m[4:6])
        if (area, block) in seen:
            continue
        seen.add((area, block))
        found = map_sun(area, block)
        key = f"{int(area[1:]):02x}{block:02x}"
        if not found:
            print(f"{area}_{block:02d}: no basic or outdoor shadow row: goes by the game's own values")
            continue
        if isinstance(found[0], str):
            rid = found[2][0] if found[2] else "-"
            lum = found[2][3] if found[2] else 0.0
            print(f"{area}_{block:02d}: no sun, strongest {lum:.3f} (light row {rid})")
            lines.append(f"{key} 0 -1 0 0 0 0 0 0 0")
            continue
        d, shadow_row, (rid, name, (lobe, base), lum) = found
        print(f"{area}_{block:02d}: shadow {shadow_row[0]} {shadow_row[1].decode('cp932')} light {rid} {name.decode('cp932')} dir {d.round(3)} lobe {lobe.round(3)} base {base.round(3)}")
        lines.append(f"{key} " + " ".join(f"{x:.5f}" for x in list(d) + list(lobe) + list(base)))
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("# One sun per map, for the lookdev host (scripts/gen_map_suns.py): map, direction of travel, lobe (gamma), ambient towards the sun (gamma)\n")
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
