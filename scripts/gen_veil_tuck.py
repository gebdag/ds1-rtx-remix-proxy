"""Write the 'tuck' lists of rtx_comp\\mesh_tables\\veil_meshes.txt (game/mesh_tables.hpp veil_tuck).

A veil's solid edge (vertex alpha 1) can meet a wall edge to edge: the Kiln doorway's ring m0010B0A18 mesh17 and its wall
mesh5 share that outline, two meshes, and rays slipped through the seam to the white passage behind (a bright line round the
wall). For each listed veil in TUCK, every solid vertex next to a fading one gets the unit direction away from its fading
neighbours, along the ring: the proxy moves it [Meshes] VeilTuck metres that way, under the wall. Other vertices: 0 0 0.

    python scripts/gen_veil_tuck.py [--data <DATA\\unpacked>]   (rewrites assets/mesh_tables/veil_meshes.txt in place)
"""
import argparse
import os
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, r"<other project>\tools")
import gen_sky_meshes as g  # noqa: E402
from bbformats.flver import read_flver  # noqa: E402

TABLE = HERE.parent / "assets" / "mesh_tables" / "veil_meshes.txt"
DATA = r"<game dir>\DATA\unpacked"
# map/model/mesh of the veils to tuck
TUCK = {("m18_00_00_00", "m0010B0A18", 17)}
SOLID = 0.99


def tuck_dirs(mesh):
    p = np.asarray(g.positions(mesh), dtype=np.float64)[:, :3]
    a = g.mesh_alpha(mesh)
    dirs = np.zeros((len(p), 3))
    if a is None:
        return dirs
    away = np.zeros((len(p), 3))
    count = np.zeros(len(p))
    for fs in mesh.face_sets:
        t = g.strip_triangles(fs)
        t = t[((t >= 0) & (t < len(p))).all(1)]
        for i, j in ((0, 1), (1, 2), (2, 0), (1, 0), (2, 1), (0, 2)):
            v, n = t[:, i], t[:, j]
            pick = (a[v] > SOLID) & (a[n] <= SOLID)
            d = p[v[pick]] - p[n[pick]]
            d /= np.linalg.norm(d, axis=1, keepdims=True) + 1e-12
            np.add.at(away, v[pick], d)
            np.add.at(count, v[pick], 1)
    has = count > 0
    norm = np.linalg.norm(away[has], axis=1, keepdims=True)
    dirs[has] = np.where(norm > 1e-6, away[has] / np.maximum(norm, 1e-12), 0.0)
    return dirs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=DATA)
    args = ap.parse_args()
    raw = TABLE.read_bytes().decode("utf-8")
    nl = "\r\n" if "\r\n" in raw else "\n"
    out = []
    cache = {}
    for line in raw.splitlines():
        words = line.split()
        if not words or line.startswith("#") or len(words) < 3:
            out.append(line)
            continue
        source = words[2]
        parts = source.split("/")
        map_id, model, mesh_index = parts[0], parts[1], int(parts[2].replace("mesh", ""))
        base = " ".join(words[:3])
        if (map_id, model, mesh_index) not in TUCK:
            out.append(base)
            continue
        key = (map_id, model, mesh_index)
        if key not in cache:
            flver = read_flver(open(os.path.join(args.data, "map", map_id, model + ".flver"), "rb").read())
            cache[key] = tuck_dirs(flver.meshes[mesh_index])
        d = np.round(cache[key] * 1000).astype(int)
        moved = int((np.abs(d).sum(1) > 0).sum())
        print(f"{source}: {moved} of {len(d)} vertices tucked")
        out.append(base + " tuck " + str(len(d)) + " " + " ".join(f"{x} {y} {z}" for x, y, z in d))
    TABLE.write_bytes((nl.join(out) + nl).encode("utf-8"))


if __name__ == "__main__":
    main()
