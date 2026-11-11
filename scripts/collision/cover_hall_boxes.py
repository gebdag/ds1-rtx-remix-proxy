"""Covers each Duke's Archives library hall with a few turned boxes (area_overrides `box` + `yaw`): the hall's own navmesh floor
(all levels) on a grid in the frame turned by YAW, no box touching a cell where another room's floor lies within the boxes'
heights. Greedy: each step takes the rectangle of allowed cells holding the most still-uncovered floor.
Navmesh vertices are part-local: world = local + (19, 63, -119)."""
import glob
import os

import numpy as np

OFF = np.array([19.0, 63.0, -119.0])
YAW, CELL, Y0, Y1 = 45.0, 1.0, 276.0, 357.0
GROUPS = {"hall 1": ["n0001B0", "n0002B0", "n0003B0", "n0004B0", "n0006B0", "n0007B0", "n0008B0", "n0009B0", "n0124B0", "n0124B1", "n0005B0"],
          "hall 2": ["n0011B0", "n0012B0", "n0013B0", "n0014B0", "n0016B0", "n0017B0", "n0018B0", "n0019B0", "n0125B0", "n0125B1"]}


def load(path):
    v, f = [], []
    for line in open(path):
        if line.startswith("v "):
            v.append(list(map(float, line.split()[1:4])))
        elif line.startswith("f "):
            f.append([int(x) - 1 for x in line.split()[1:4]])
    return np.array(v) + OFF, np.array(f, int)


def to_frame(p):
    r = np.radians(YAW)
    c, s = np.cos(r), np.sin(r)
    return np.stack([p[..., 0] * c + p[..., 2] * s, -p[..., 0] * s + p[..., 2] * c], -1)


def from_frame(q):
    r = np.radians(YAW)
    c, s = np.cos(r), np.sin(r)
    return np.array([q[0] * c - q[1] * s, q[0] * s + q[1] * c])


nav = {os.path.basename(p)[:7]: load(p) for p in glob.glob(os.path.join(os.path.dirname(os.path.abspath(__file__)), "out", "col_m17", "n*.obj"))}
nav = {k: v for k, v in nav.items() if len(v[0])}


def raster(names, origin, shape, ylim=None):
    """Cells whose centre lies in any triangle of these navmeshes (in the turned frame); ylim: only triangles within it."""
    grid = np.zeros(shape, bool)
    gx, gz = np.meshgrid(origin[0] + (np.arange(shape[0]) + 0.5) * CELL, origin[1] + (np.arange(shape[1]) + 0.5) * CELL, indexing="ij")
    pts = np.stack([gx, gz], -1)
    for n in names:
        v, f = nav[n]
        for tri in f:
            t3 = v[tri]
            if ylim and (t3[:, 1].max() < ylim[0] or t3[:, 1].min() > ylim[1]):
                continue
            a, b, c = to_frame(t3)
            lo, hi = np.minimum(np.minimum(a, b), c), np.maximum(np.maximum(a, b), c)
            i0, i1 = np.clip(((lo - origin) / CELL).astype(int) - 1, 0, None), np.minimum(((hi - origin) / CELL).astype(int) + 2, shape)
            if i0[0] >= i1[0] or i0[1] >= i1[1]:
                continue
            p = pts[i0[0]:i1[0], i0[1]:i1[1]]
            def side(u, w):
                return (w[0] - u[0]) * (p[..., 1] - u[1]) - (w[1] - u[1]) * (p[..., 0] - u[0])
            d1, d2, d3 = side(a, b), side(b, c), side(c, a)
            inside = ~(((d1 < 0) | (d2 < 0) | (d3 < 0)) & ((d1 > 0) | (d2 > 0) | (d3 > 0)))
            grid[i0[0]:i1[0], i0[1]:i1[1]] |= inside
    return grid


for g, names in GROUPS.items():
    allv = np.concatenate([to_frame(nav[n][0]) for n in names])
    origin = allv.min(0) - 3
    shape = tuple(((allv.max(0) + 3 - origin) / CELL).astype(int) + 1)
    own = raster(names, origin, shape)
    others = [n for n in nav if n not in sum(GROUPS.values(), [])] + [n for k, ns in GROUPS.items() if k != g for n in ns]
    forbidden = raster(others, origin, shape, (Y0, Y1)) & ~own
    left = own.copy()
    boxes = []
    while left.sum() > 0.01 * own.sum() and len(boxes) < 8:
        F = np.pad(forbidden.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
        L = np.pad(left.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
        best = (0, None)
        nx, nz = shape
        for x0 in range(nx):
            for x1 in range(x0 + 1, nx + 1):
                frow = F[x1, 1:] - F[x0, 1:] - F[x1, :-1] + F[x0, :-1]   # forbidden per z column in [x0, x1)
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
        if not best[1] or best[0] < 0.02 * own.sum():
            break
        x0, x1, z0, z1 = best[1]
        left[x0:x1, z0:z1] = False
        boxes.append(best[1])
    print(f"{g}: {len(boxes)} boxes cover {100 * (1 - left.sum() / own.sum()):.1f}% of {own.sum()} floor cells; forbidden cells {forbidden.sum()}")
    for k, (x0, x1, z0, z1) in enumerate(boxes):
        q0, q1 = origin + np.array([x0, z0]) * CELL, origin + np.array([x1, z1]) * CELL
        c = from_frame((q0 + q1) / 2)
        h = (q1 - q0) / 2
        print(f"  [{k}] box = {c[0] - h[0]:.2f} {Y0:.1f} {c[1] - h[1]:.2f} {c[0] + h[0]:.2f} {Y1:.1f} {c[1] + h[1]:.2f}   yaw = {YAW:g}   ({2 * h[0]:.0f} x {2 * h[1]:.0f} m)")
