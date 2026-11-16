"""Covers each Duke's Archives library hall with a few turned boxes (area_overrides `box` + `yaw`) for the sun-off sections.

Grid in the frame turned by YAW (CELL m):
  own        the hall's navmesh floor (all levels), minus floor open to the sky (a ray straight up meets none of the map's
             visible pieces: a balcony off hall 1 is outdoors)
  forbidden  any other floor within the boxes' heights: other rooms' navmeshes, and walkable collision (|normal.y| > 0.7) that
             is not within one cell of the hall's own floor (some rooms have collision but no navmesh); and the open-sky floor
Greedy: each step takes the rectangle of non-forbidden cells holding the most still-uncovered own floor. Then every box grows
GROW m on all sides, so boxes that meet (hall 1 and hall 2's walkways) overlap instead of leaving a strip between them.

Navmesh and collision vertices are part-local: world = local + (19, 63, -119) (every navmesh and collision part of m17_00_00_00
sits there). Inputs: out/col_m17/*.obj from colexport.cs. Run: py -3.12 -P cover_hall_boxes.py
"""
import os  # noqa: E402  (ds1paths: the local folders, see paths.example.ini)
import sys  # noqa: E402
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")))
import ds1paths  # noqa: E402

import glob
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, ds1paths.SCRIPTS)
from map_geometry import blocks_rays, first, map_pieces, rotation  # noqa: E402

DATA = ds1paths.GAME + r"\DATA\unpacked"
COL = os.path.join(HERE, "out", "col_m17")
OFF = np.array([19.0, 63.0, -119.0])
YAW, CELL, Y0, Y1, GROW = 45.0, 1.0, 276.0, 357.0, 0.4
GROUPS = {"hall 1": ["n0001B0", "n0002B0", "n0003B0", "n0004B0", "n0006B0", "n0007B0", "n0008B0", "n0009B0", "n0124B0", "n0124B1", "n0005B0"],
          "hall 2": ["n0011B0", "n0012B0", "n0013B0", "n0014B0", "n0016B0", "n0017B0", "n0018B0", "n0019B0", "n0125B0", "n0125B1"]}


def load(path):
    v, f = [], []
    for line in open(path):
        if line.startswith("v "):
            v.append(list(map(float, line.split()[1:4])))
        elif line.startswith("f "):
            f.append([int(x) - 1 for x in line.split()[1:4]])
    return np.array(v).reshape(-1, 3) + OFF, np.array(f, int).reshape(-1, 3)


def to_frame(p):
    r = np.radians(YAW)
    c, s = np.cos(r), np.sin(r)
    return np.stack([p[..., 0] * c + p[..., 2] * s, -p[..., 0] * s + p[..., 2] * c], -1)


def from_frame(q):
    r = np.radians(YAW)
    c, s = np.cos(r), np.sin(r)
    return np.array([q[0] * c - q[1] * s, q[0] * s + q[1] * c])


def visual_scene():
    from bbformats.flver import read_flver
    from embreex import rtcore_scene as rtcs
    from embreex.mesh_construction import TriangleMesh
    tris, flvers = [], {}
    for model, at, rot, scale, _ in map_pieces(os.path.join(DATA, "map", "MapStudio", "m17_00_00_00.msb")):
        path = os.path.join(DATA, "map", "m17_00_00_00", f"{model}A17.flver")
        if not os.path.exists(path):
            continue
        if model not in flvers:
            flvers[model] = read_flver(open(path, "rb").read())
        r = rotation(rot) * scale
        for mesh in flvers[model].meshes:
            fs = mesh.lod0()
            if fs is None or "position" not in mesh.vertices:
                continue
            p = np.asarray(first(mesh.vertices["position"]), np.float64)[:, :3]
            t = fs.triangles()
            t = t[(t < len(p)).all(1)]
            if len(t) and blocks_rays(str(flvers[model].materials[mesh.material_index].mtd)):
                tris.append((p @ r.T + at)[t])
    scene = rtcs.EmbreeScene()
    TriangleMesh(scene, np.concatenate(tris).astype(np.float32))
    return scene


def cells_of(tris, origin, shape):
    """Per triangle (n,3,3 world), the grid cells whose centre lies in it (turned frame): list of (ix, iz) arrays."""
    out = []
    gx = origin[0] + (np.arange(shape[0]) + 0.5) * CELL
    gz = origin[1] + (np.arange(shape[1]) + 0.5) * CELL
    for t3 in tris:
        a, b, c = to_frame(t3)
        lo, hi = np.minimum(np.minimum(a, b), c), np.maximum(np.maximum(a, b), c)
        i0 = np.clip(((lo - origin) / CELL).astype(int) - 1, 0, None)
        i1 = np.minimum(((hi - origin) / CELL).astype(int) + 2, shape)
        if i0[0] >= i1[0] or i0[1] >= i1[1]:
            out.append((np.array([], int), np.array([], int)))
            continue
        px, pz = np.meshgrid(gx[i0[0]:i1[0]], gz[i0[1]:i1[1]], indexing="ij")

        def side(u, w):
            return (w[0] - u[0]) * (pz - u[1]) - (w[1] - u[1]) * (px - u[0])
        d1, d2, d3 = side(a, b), side(b, c), side(c, a)
        inside = ~(((d1 < 0) | (d2 < 0) | (d3 < 0)) & ((d1 > 0) | (d2 > 0) | (d3 > 0)))
        ix, iz = np.nonzero(inside)
        out.append((ix + i0[0], iz + i0[1]))
    return out


def greedy(own, forbidden):
    left = own.copy()
    boxes = []
    nx, nz = own.shape
    while left.sum() > 0.005 * own.sum() and len(boxes) < 8:
        F = np.pad(forbidden.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
        L = np.pad(left.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
        best = (0, None)
        for x0 in range(nx):
            for x1 in range(x0 + 1, nx + 1):
                frow = F[x1, 1:] - F[x0, 1:] - F[x1, :-1] + F[x0, :-1]
                lrow = L[x1, 1:] - L[x0, 1:] - L[x1, :-1] + L[x0, :-1]
                z = 0
                while z < nz:
                    if frow[z]:
                        z += 1
                        continue
                    z1 = z
                    while z1 < nz and not frow[z1]:
                        z1 += 1
                    s = lrow[z:z1].sum()
                    if s > best[0]:
                        best = (s, (x0, x1, z, z1))
                    z = z1
        if not best[1] or best[0] < 0.01 * own.sum():
            break
        x0, x1, z0, z1 = best[1]
        left[x0:x1, z0:z1] = False
        boxes.append(best[1])
    return boxes, left


def main():
    nav = {os.path.basename(p)[:7]: load(p) for p in glob.glob(os.path.join(COL, "n*.obj"))}
    nav = {k: v for k, v in nav.items() if len(v[1])}
    col = [load(p) for p in glob.glob(os.path.join(COL, "h*.obj"))]
    col_tris = np.concatenate([v[f] for v, f in col if len(f)])
    n = np.cross(col_tris[:, 1] - col_tris[:, 0], col_tris[:, 2] - col_tris[:, 0])
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
    ys = col_tris[:, :, 1]
    col_floor = col_tris[(np.abs(n[:, 1]) > 0.7) & (ys.max(1) >= Y0) & (ys.min(1) <= Y1)]
    scene = visual_scene()
    results = {}
    for g, names in GROUPS.items():
        allv = np.concatenate([to_frame(nav[k][0]) for k in names])
        origin = allv.min(0) - 6
        shape = tuple(((allv.max(0) + 6 - origin) / CELL).astype(int) + 1)
        own = np.zeros(shape, bool)
        sky = np.zeros(shape, bool)
        for k in names:
            v, f = nav[k]
            tris = v[f]
            cen = tris.mean(1) + np.array([0, 0.3, 0])
            r = scene.run(cen.astype(np.float32), np.tile(np.array([[0, 1, 0]], np.float32), (len(cen), 1)), output=1)
            open_sky = (np.asarray(r["primID"]) < 0) | (np.asarray(r["tfar"]) > 300)
            for (ix, iz), o in zip(cells_of(tris, origin, shape), open_sky):
                (sky if o else own)[ix, iz] = True
        own &= ~sky
        near_own = own.copy()
        near_own[1:] |= own[:-1]
        near_own[:-1] |= own[1:]
        near_own[:, 1:] |= own[:, :-1]
        near_own[:, :-1] |= own[:, 1:]
        forbidden = sky.copy()
        for k, (v, f) in nav.items():
            if k in names:
                continue
            tris = v[f]
            keep = (tris[:, :, 1].max(1) >= Y0) & (tris[:, :, 1].min(1) <= Y1)
            for ix, iz in cells_of(tris[keep], origin, shape):
                forbidden[ix, iz] = True
        for ix, iz in cells_of(col_floor, origin, shape):
            m = ~near_own[ix, iz]
            forbidden[ix[m], iz[m]] = True
        forbidden &= ~own
        boxes, left = greedy(own, forbidden)
        print(f"{g}: {len(boxes)} boxes cover {100 * (1 - left.sum() / own.sum()):.1f}% of {own.sum()} own floor cells; "
              f"open-sky cells left out {sky.sum()}; forbidden cells {forbidden.sum()}")
        results[g] = []
        for x0, x1, z0, z1 in boxes:
            q0 = origin + np.array([x0, z0]) * CELL - GROW
            q1 = origin + np.array([x1, z1]) * CELL + GROW
            c = from_frame((q0 + q1) / 2)
            h = (q1 - q0) / 2
            results[g].append((c[0] - h[0], Y0, c[1] - h[1], c[0] + h[0], Y1, c[1] + h[1]))
            print(f"  box = {c[0] - h[0]:.2f} {Y0:.1f} {c[1] - h[1]:.2f} {c[0] + h[0]:.2f} {Y1:.1f} {c[1] + h[1]:.2f}   yaw = {YAW:g}   ({2 * h[0]:.1f} x {2 * h[1]:.1f} m)")
    return results


if __name__ == "__main__":
    main()
