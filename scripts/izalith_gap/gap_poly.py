"""Straight edges for the floor fill (gap_floor.py): its cells become polygons, simplified by less than SIMPLIFY m (the 0.5 m
stair steps go, the shape stays), then triangulated. Needs shapely 2 (the default python here, not 3.12).

Usage: python gap_poly.py <holes_fill.npz> <out_tris.npz>"""
import sys

import numpy as np
import shapely
from shapely.geometry import box

SIMPLIFY = 0.75
f = np.load(sys.argv[1])
mask, x0, z0, cell = f["mask"], float(f["x0"]), float(f["z0"]), float(f["cell"])
shape = shapely.unary_union([box(x0 + x * cell, z0 + z * cell, x0 + (x + 1) * cell, z0 + (z + 1) * cell) for z, x in np.argwhere(mask)])
shape = shape.simplify(SIMPLIFY, preserve_topology=True)
polys = list(shape.geoms) if hasattr(shape, "geoms") else [shape]
tris = []
for p in polys:
    for t in shapely.constrained_delaunay_triangles(p).geoms:
        tris.append(np.array(t.exterior.coords)[:3])
print(f"{len(polys)} polygon(s), {sum(len(p.exterior.coords) - 1 for p in polys)} outline points, {len(tris)} triangles, "
      f"area {shape.area:.0f} m2 (cells {mask.sum() * cell * cell:.0f} m2)")
np.savez(sys.argv[2], tris=np.array(tris), level=float(f["level"]), walk=float(f["walk"]))
