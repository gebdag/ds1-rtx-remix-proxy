"""Finds the real holes in the floor around the Bed of Chaos walkway from the map data (every piece of m14_01 and m14_00, MSB +
FLVERs) and reports, for each hole, the height of the floor around it.

A 0.5 m grid over the area; from just above the walkway, a ray straight down per cell. A cell where the ray meets nothing at
all is a hole (the game shows the sky there). Holes are grouped into connected pieces; for each piece, the heights the rays
meet in the cells bordering it give the floor level around it.

Usage: py -3.12 -P gap_floor.py <out.npz>"""
import os
import sys
from collections import Counter, deque

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pit_view import load_map  # noqa: E402
from embreex import rtcore_scene as rtcs  # noqa: E402
from embreex.mesh_construction import TriangleMesh  # noqa: E402

WALK = -358.74          # the walkway (gap_full.py)
X0, X1, Z0, Z1 = -580.0, -390.0, 290.0, 460.0   # Remix world: the pit and its surroundings
CELL = 0.5

G, owner = load_map()
scene = rtcs.EmbreeScene()
TriangleMesh(scene, G.astype(np.float32))

nx, nz = int((X1 - X0) / CELL), int((Z1 - Z0) / CELL)
ix, jz = np.meshgrid(np.arange(nx), np.arange(nz))
o = np.stack([X0 + (ix.ravel() + 0.5) * CELL, np.full(ix.size, WALK + 4.0), Z0 + (jz.ravel() + 0.5) * CELL], -1)
res = scene.run(o.astype(np.float32), np.tile([0, -1.0, 0], (len(o), 1)).astype(np.float32), output=1)
prim = np.asarray(res["primID"]).reshape(nz, nx)
h = (WALK + 4.0 - np.asarray(res["tfar"])).reshape(nz, nx)
hole = prim < 0
h[hole] = np.nan
print(f"grid {nx}x{nz} at {CELL} m: holes {int(hole.sum())} cells")

# Connected holes (4-neighbour); the border of the grid is open space outside the area, so pieces touching it are left out.
label = np.full(hole.shape, -1)
pieces = []
for start in zip(*np.nonzero(hole)):
    if label[start] >= 0:
        continue
    q, cells, touches_edge = deque([start]), [], False
    label[start] = len(pieces)
    while q:
        z, x = q.popleft()
        cells.append((z, x))
        if z in (0, nz - 1) or x in (0, nx - 1):
            touches_edge = True
        for dz, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            a, b = z + dz, x + dx
            if 0 <= a < nz and 0 <= b < nx and hole[a, b] and label[a, b] < 0:
                label[a, b] = len(pieces)
                q.append((a, b))
    pieces.append((cells, touches_edge))

rows = []
for k, (cells, edge) in enumerate(pieces):
    cz = np.array([c[0] for c in cells]); cx = np.array([c[1] for c in cells])
    border = set()
    for z, x in cells:
        for dz, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            a, b = z + dz, x + dx
            if 0 <= a < nz and 0 <= b < nx and not hole[a, b]:
                border.add((a, b))
    bh = np.array([h[a, b] for a, b in border])
    owners = Counter(owner[prim[a, b]].split(":")[2] for a, b in border)
    rows.append((len(cells), k, edge, X0 + cx.min() * CELL, X0 + (cx.max() + 1) * CELL, Z0 + cz.min() * CELL, Z0 + (cz.max() + 1) * CELL,
                 np.percentile(bh, [10, 50, 90]) if len(bh) else [np.nan] * 3, owners.most_common(3)))
rows.sort(reverse=True)
print("holes (largest first): cells, x range, z range, floor height around it p10/p50/p90, pieces around it")
for n, k, edge, xa, xb, za, zb, hp, ow in rows[:12]:
    print(f"  #{k:3d} {n:6d} cells {'(open to the edge) ' if edge else ''}x {xa:7.1f}..{xb:7.1f} z {za:6.1f}..{zb:6.1f}  "
          f"floor {hp[0]:7.2f} / {hp[1]:7.2f} / {hp[2]:7.2f}  {ow}")
np.savez(sys.argv[1], hole=hole, h=h, label=label, x0=X0, z0=Z0, cell=CELL)


# ---- the fill: holes in the upper floor (left and right of the walkway) ----
# A hole is upper floor when it is large and every cell around it meets the map high up (nothing at the arena's -535): those
# are the two mirror-image holes bordered by m3000B1 (-383.35) and m8001B1 (-383.61). The openings down into the arena border
# m3163B1 at -535 and stay open. The floor goes 5 cm under the lowest floor around the hole, grown by GROW cells so its edges
# tuck under the pieces around it; growth never reaches another hole or a cell over the arena.
GROW = 6   # 3 m: under the walls' base ledge (m3000B1), which the game does not draw from the walkway
fill = np.zeros(hole.shape, bool)
levels = []
for n, k, edge, xa, xb, za, zb, hp, ow in rows:
    if n < 1000 or edge:
        continue
    cells = label == k
    ring = (np.roll(cells, 1, 0) | np.roll(cells, -1, 0) | np.roll(cells, 1, 1) | np.roll(cells, -1, 1)) & ~hole
    if np.nanmin(h[ring]) < WALK - 40.0:
        print(f"  hole #{k}: borders the arena (lowest neighbour {np.nanmin(h[ring]):.2f}), left open")
        continue
    level = float(np.nanmin(h[ring])) - 0.05
    levels.append(level)
    grown = cells.copy()
    for _ in range(GROW):
        grown |= np.roll(grown, 1, 0) | np.roll(grown, -1, 0) | np.roll(grown, 1, 1) | np.roll(grown, -1, 1)
    ok = cells | (~hole & (h > WALK - 40.0))
    fill |= grown & ok
    print(f"  hole #{k}: filled at {level:.2f} ({int(cells.sum())} cells, {int((grown & ok).sum())} with the overlap)")
level = min(levels)
rects = []
left = fill.copy()
for zz in range(nz):
    xx = 0
    while xx < nx:
        if not left[zz, xx]:
            xx += 1
            continue
        x1 = xx
        while x1 < nx and left[zz, x1]:
            x1 += 1
        z1 = zz + 1
        while z1 < nz and left[z1, xx:x1].all():
            z1 += 1
        left[zz:z1, xx:x1] = False
        rects.append((X0 + xx * CELL, Z0 + zz * CELL, X0 + x1 * CELL, Z0 + z1 * CELL))
        xx = x1
np.savez(sys.argv[1].replace(".npz", "_fill.npz"), rects=np.array(rects), mask=fill, x0=X0, z0=Z0, cell=CELL, level=level, walk=WALK)
print(f"fill: {len(rects)} rectangles at {level:.2f}")
