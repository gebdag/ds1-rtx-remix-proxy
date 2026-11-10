"""Generates assets/sky/ground_cards.txt: flat alpha-tested cards the artists lifted off the ground, and each vertex's height.

Leaves (Sen's Fortress), lily pads (Blighttown), bones (Catacombs), rubble (Undead Burg) and the like are _Edge (alpha-tested)
meshes lying 1-11 cm above the surface they cover, so the game's depth test never lets the ground through them. The game draws
them without shadows, so the gap never shows. Ray traced they are solid cutouts that float, their shadows sitting off them. The
proxy brings each vertex of a listed draw down to clearance + kept * its height ([Sky] GroundCardClearance, GroundCardHeightKept),
in the rigid expansion (game/sky_meshes.hpp ground_card_heights). They keep their shadows.

Measured from the game's files: every map piece placed by its MSB, each card vertex projected straight down onto the map's solid
triangles (the first one below it, within 1 m). A card is listed when 95% of its area faces up, its heights spread less than
12 cm, and its median height is 1-30 cm. Cards over water are left out (water is not ground here). Its placement must be upright
(no x/z rotation, scale 1: the model's y is the world's); a mesh placed more than once must sit at the same heights (within 1 cm)
in each place. A vertex with no ground under it takes its nearest measured neighbour's height. A face set whose index hash
another mesh shares, or that another table lists, is left out.

Line: <index FNV-1a 64> <index count> <source> <texture> <vertex count> <height mm> ...   (65535 = no lowering)

Run: python scripts/gen_ground_cards.py      (needs numpy; reads DATA/unpacked)
"""
import glob
import os
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, r"<other project>\tools")
from bbformats.flver import read_flver  # noqa: E402
from map_geometry import first, map_pieces, rotation  # noqa: E402

DATA = r"<game dir>\DATA\unpacked\map"
OUT = HERE.parent / "assets" / "sky" / "ground_cards.txt"
CELL = 1.0
REACH = 1.0
NONE = 0xFFFF


def stem(p):
    return str(p).replace("/", "\\").split("\\")[-1].rsplit(".", 1)[0]


def solid(mtd):
    m = mtd.lower()
    return not any(s in m for s in ("_alp", "_add", "water", "_edge", "ntoa", "_sky", "[sky]"))


class Ground:
    def __init__(self, tris):
        self.t = tris
        lo = tris[:, :, [0, 2]].min(1)
        hi = tris[:, :, [0, 2]].max(1)
        self.grid = defaultdict(list)
        for i in range(len(tris)):
            for gx in range(int(np.floor(lo[i, 0] / CELL)), int(np.floor(hi[i, 0] / CELL)) + 1):
                for gz in range(int(np.floor(lo[i, 1] / CELL)), int(np.floor(hi[i, 1] / CELL)) + 1):
                    self.grid[(gx, gz)].append(i)

    def below(self, p):
        """Height of p above the first solid surface below it (within REACH; a surface up to 1 cm above counts), or None."""
        cand = self.grid.get((int(np.floor(p[0] / CELL)), int(np.floor(p[2] / CELL))))
        if not cand:
            return None
        t = self.t[cand]
        a, b, c = t[:, 0], t[:, 1], t[:, 2]
        v0 = c[:, [0, 2]] - a[:, [0, 2]]
        v1 = b[:, [0, 2]] - a[:, [0, 2]]
        v2 = p[[0, 2]] - a[:, [0, 2]]
        d00 = (v0 * v0).sum(1)
        d01 = (v0 * v1).sum(1)
        d11 = (v1 * v1).sum(1)
        d20 = (v2 * v0).sum(1)
        d21 = (v2 * v1).sum(1)
        den = d00 * d11 - d01 * d01
        ok = np.abs(den) > 1e-12
        den = np.where(ok, den, 1.0)
        u = (d11 * d20 - d01 * d21) / den
        v = (d00 * d21 - d01 * d20) / den
        ok &= (u >= -1e-4) & (v >= -1e-4) & (u + v <= 1 + 1e-4)
        if not ok.any():
            return None
        y = a[:, 1] + u * (c[:, 1] - a[:, 1]) + v * (b[:, 1] - a[:, 1])
        gap = (p[1] - y)[ok]
        gap = gap[(gap <= REACH) & (gap >= -0.01)]
        return float(gap.min()) if len(gap) else None


def scan(map_name):
    """(model, mesh index, texture, heights in m per vertex (nan = none), upright) of each candidate card placement."""
    msb = os.path.join(DATA, "MapStudio", f"{map_name}.msb")
    area = map_name[1:3]
    flvers, solids, cards = {}, [], []
    for model, at, rot, scale, _light in map_pieces(msb):
        path = os.path.join(DATA, map_name, f"{model}A{area}.flver")
        if not os.path.exists(path):
            continue
        if model not in flvers:
            flvers[model] = read_flver(open(path, "rb").read())
        flver = flvers[model]
        r = rotation(rot) * scale
        upright = abs(rot[0]) < 0.01 and abs(rot[2]) < 0.01 and np.allclose(scale, 1.0, atol=1e-3)
        for i, mesh in enumerate(flver.meshes):
            fs = mesh.lod0()
            if fs is None or "position" not in mesh.vertices:
                continue
            p = np.asarray(first(mesh.vertices["position"]), np.float64)[:, :3]
            t = fs.triangles()
            t = t[(t < len(p)).all(1)]
            if not len(t):
                continue
            world = p @ r.T + at
            mat = flver.materials[mesh.material_index]
            mtd = stem(mat.mtd)
            if solid(mtd):
                solids.append(world[t])
            elif "_edge" in mtd.lower():
                tex = next((stem(x.path) for x in mat.textures if x.path), "")
                if "water" in tex.lower():
                    continue
                tri = world[t]
                n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
                area_ = np.linalg.norm(n, axis=1)
                nz = area_ > 1e-9
                up = float((area_[nz] * (np.abs(n[nz, 1]) / area_[nz] > 0.8)).sum() / max(area_[nz].sum(), 1e-9))
                if up >= 0.95:
                    cards.append((model, i, tex, world, upright))
    if not solids:
        return []
    ground = Ground(np.concatenate(solids))
    out = []
    for model, i, tex, world, upright in cards:
        h = np.array([np.nan if g is None else g for g in (ground.below(v) for v in world)])
        out.append((model, i, tex, h, world, upright))
    return out


def fill_missing(h, world):
    """A vertex with no ground under it takes its nearest measured vertex's height."""
    have = ~np.isnan(h)
    if have.all() or not have.any():
        return h
    out = h.copy()
    src = world[have]
    for k in np.where(~have)[0]:
        out[k] = h[have][np.argmin(((src - world[k]) ** 2).sum(1))]
    return out


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for c in data:
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    maps = sorted(stem(p) for p in glob.glob(os.path.join(DATA, "MapStudio", "m*.msb")))
    cards, bad = {}, set()
    for m in maps:
        for model, i, tex, h, world, upright in scan(m):
            key = (m, model, i)
            measured = h[~np.isnan(h)]
            if len(measured) < max(3, 0.3 * len(h)):
                continue
            p5, p50, p90 = np.percentile(measured * 100, [5, 50, 90])
            if p90 - p5 >= 12 or not (1.0 <= p50 < 30):
                continue
            if not upright:
                print("placed tilted or scaled, left out:", m, model, "mesh", i, tex)
                bad.add(key)
                continue
            h = fill_missing(h, world)
            if key in cards and np.nanmax(np.abs(cards[key][1] - h)) > 0.01:
                print("placed at other heights, left out:", m, model, "mesh", i, tex)
                bad.add(key)
                continue
            cards[key] = (tex, h, p50)
        print(m, len(cards), "cards so far", flush=True)
    for key in bad:
        cards.pop(key, None)

    owners = defaultdict(set)
    sets = {}
    for map_dir in sorted(glob.glob(os.path.join(DATA, "m*_*"))):
        map_name = os.path.basename(map_dir)
        for path in sorted(glob.glob(os.path.join(map_dir, "*.flver"))):
            model = os.path.basename(path).rsplit(".", 1)[0]
            flver = read_flver(open(path, "rb").read(), decode_vertices=False)
            for i, mesh in enumerate(flver.meshes):
                for j, fs in enumerate(mesh.face_sets):
                    idx = np.asarray(fs.indices, dtype="<u2")
                    h = fnv1a64(idx.tobytes())
                    owners[(h, len(idx))].add((map_name, model[:-3] if model[-3] == "A" else model, i))
                    sets.setdefault((map_name, model, i), []).append((j, h, len(idx)))
    taken = set()
    for table in OUT.parent.glob("*.txt"):
        if table != OUT:
            taken |= {line.split()[0] for line in table.read_text(encoding="utf-8").splitlines() if line and line[0] != "#"}

    lines = ["# Flat alpha-tested cards lifted off the ground, each vertex's height above it (game/sky_meshes.hpp ground_card_heights). "
             "Generated by scripts/gen_ground_cards.py: <index FNV-1a 64> <index count> <source> <texture> <vertex count> <height mm> ..."]
    left_out = 0
    for (map_name, model, i), (tex, h, p50) in sorted(cards.items()):
        full = f"{model}A{map_name[1:3]}"
        mm = " ".join(str(NONE if np.isnan(x) else int(np.clip(round(x * 1000), 0, NONE - 1))) for x in h)
        for j, hsh, count in sets.get((map_name, full, i), []):
            key = f"{hsh:016x}"
            if key in taken or owners[(hsh, count)] != {(map_name, model, i)}:
                left_out += 1
                print("shared hash, left out:", map_name, full, "mesh", i, "lod", j, sorted(owners[(hsh, count)])[:3])
                continue
            lines.append(f"{key} {count} {map_name}/{full}/mesh{i}/lod{j} {tex} {len(h)} {mm}")
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(len(lines) - 1, "face sets written to", OUT, "-", left_out, "left out")


if __name__ == "__main__":
    main()
