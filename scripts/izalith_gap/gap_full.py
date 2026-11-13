"""Lost Izalith gap filler against the FULL map (every visible piece of m14_01 and m14_00, not what a capture drew).

Remix world = game world with x mirrored. Escaping rays (sky) from the capture camera's frustum and from viewpoints over the
walkway looking down; then for each level under the walkway, the sheet cells: where steep escaping rays cross, small holes
closed, kept only if nothing of the map is straight below and nothing within 0.4 m. Prints a table per level; with a level
given, saves that sheet's cells.
Usage: py -3.12 -P gap_full.py <cams_iz.npy> <out.npz> [level]"""
import os
import sys

import numpy as np

sys.path.insert(0, r"<projects>\ds1-comp-proxy-nv\scripts")
sys.path.insert(0, r"<other project>\tools")
from map_geometry import first, map_pieces, rotation  # noqa: E402
from bbformats.flver import read_flver  # noqa: E402
from embreex import rtcore_scene as rtcs  # noqa: E402
from embreex.mesh_construction import TriangleMesh  # noqa: E402

DATA = r"<game dir>\DATA\unpacked"
cams = np.load(sys.argv[1])
out = sys.argv[2]
want = float(sys.argv[3]) if len(sys.argv) > 3 else None
CELL = 0.5
rng = np.random.default_rng(1)

tris = []
for MAP in ("m14_01_00_00", "m14_00_00_00"):
    flvers = {}
    for model, at, rot, scale, _ in map_pieces(os.path.join(DATA, "map", "MapStudio", f"{MAP}.msb")):
        path = os.path.join(DATA, "map", MAP, f"{model}A14.flver")
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
            if len(t):
                tris.append((p @ r.T + at)[t])
G = np.concatenate(tris) * np.array([-1.0, 1.0, 1.0])  # Remix world
scene = rtcs.EmbreeScene()
TriangleMesh(scene, G.astype(np.float32))
print(len(G), "map triangles")


def cast(o, d):
    res = scene.run(o.astype(np.float32), d.astype(np.float32), output=1)
    t = np.asarray(res["tfar"]).astype(np.float64)
    t[np.asarray(res["primID"]) < 0] = np.inf
    return t


# 1. the capture camera's frustum (USD camera looks down -Z)
rays = []
for xf in cams:
    W, H = 320, 180
    gx, gy = np.meshgrid(((np.arange(W) + 0.5) / W - 0.5) * 1.6, -((np.arange(H) + 0.5) / H - 0.5) * 0.9)
    d = np.stack([gx, gy, -np.ones_like(gx)], -1).reshape(-1, 3) @ xf[:3, :3]
    d /= np.linalg.norm(d, axis=1, keepdims=True)
    rays.append(np.c_[np.repeat(xf[3:4, :3], len(d), 0), d])
center = cams[:, 3, :3].mean(0)
# 2. viewpoints over the walkway floor (found by casting down), 2/4/6 m up, looking down
gx, gz = np.meshgrid(np.arange(-24, 24.01, 2.0), np.arange(-24, 24.01, 2.0))
starts = np.stack([center[0] + gx.ravel(), np.full(gx.size, center[1] + 15.0), center[2] + gz.ravel()], -1)
t = cast(starts, np.tile([0, -1.0, 0], (len(starts), 1)))
ok = np.isfinite(t) & (t < 40)
floor = starts[ok] - np.array([0, 1.0, 0]) * t[ok, None]
walk = float(np.median(floor[:, 1]))
for f in floor:
    for h in (2.0, 4.0, 6.0):
        d = rng.normal(size=(500, 3))
        d[:, 1] = -np.abs(d[:, 1]) - 0.2
        d /= np.linalg.norm(d, axis=1, keepdims=True)
        rays.append(np.c_[np.repeat((f + [0, h, 0])[None], len(d), 0), d])
R = np.concatenate(rays)
esc = ~np.isfinite(cast(R[:, :3], R[:, 3:]))
E = R[esc & (R[:, 4] < -0.4)]
print(f"viewpoints {len(floor)}, walkway {walk:.2f}; rays {len(R)}, escaped {int(esc.sum())}, steep escaped {len(E)}")

D14 = np.array([[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]] +
               [[sx, sy, sz] for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)], np.float64)
D14 /= np.linalg.norm(D14, axis=1, keepdims=True)


def dil(g):
    o = g.copy()
    o[1:, :] |= g[:-1, :]; o[:-1, :] |= g[1:, :]; o[:, 1:] |= g[:, :-1]; o[:, :-1] |= g[:, 1:]
    return o


def sheet(level):
    tt = (level - E[:, 1]) / E[:, 4]
    p = (E[:, :3] + E[:, 3:] * tt[:, None])[tt > 0]
    if not len(p):
        return None
    x0, z0 = np.floor(p[:, 0].min()) - 2, np.floor(p[:, 2].min()) - 2
    nx = int((p[:, 0].max() + 2 - x0) / CELL) + 1
    nz = int((p[:, 2].max() + 2 - z0) / CELL) + 1
    g = np.zeros((nz, nx), bool)
    g[((p[:, 2] - z0) / CELL).astype(int), ((p[:, 0] - x0) / CELL).astype(int)] = True
    g = ~dil(~dil(dil(g)))  # close 1-cell holes
    jz, ix = np.nonzero(g)
    c = np.stack([x0 + (ix + 0.5) * CELL, np.full(len(ix), level), z0 + (jz + 0.5) * CELL], -1)
    below = np.isfinite(cast(c, np.tile([0, -1.0, 0], (len(c), 1))))
    near = np.zeros(len(c), bool)
    for d in D14:
        near |= cast(c, np.tile(d, (len(c), 1))) < 0.4
    keep = ~below & ~near
    out_g = np.zeros_like(g)
    out_g[jz[keep], ix[keep]] = True
    return dict(grid=out_g, x0=x0, z0=z0, kept=int(keep.sum()), below=int((below & ~near).sum()), near=int(near.sum()))


if want is None:
    print("level     kept  dropped(below)  dropped(near)   share kept")
    for level in np.arange(walk - 2.0, walk - 41.0, -2.0):
        s = sheet(level)
        if s:
            tot = s["kept"] + s["below"] + s["near"]
            print(f"{level:8.2f} {s['kept']:7d} {s['below']:12d} {s['near']:14d}   {s['kept'] / max(tot, 1) * 100:6.1f}%")
else:
    s = sheet(want)
    np.savez(out, grid=s["grid"], x0=s["x0"], z0=s["z0"], cell=CELL, level=want, walk=walk)
    print(f"level {want}: kept {s['kept']}, dropped below {s['below']}, near {s['near']}; saved {out}")
