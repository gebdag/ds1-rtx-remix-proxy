"""Report map textures whose meshes show hard-edged facets: candidates for rtx.smoothNormalsTextures.

The game lights most of its map with baked lightmaps, so a low-poly mesh whose normals are split at sharp creases
(each patch of faces with its own normals) looks smooth in the game; lit per pixel by the sun in Remix, every patch
shows (the Undead Burg's cliffs, frame_20261107_025500). Remix's smooth normals (rtx.smoothNormalsTextures, by diffuse
texture hash) average the normals over each position.

Per map mesh (lod0, map pieces only): its area, and the length of its *hard* edges: edges two triangles share by
position whose vertex normals differ between the two (split normals) and whose faces meet at an angle (not a flat
seam). Per diffuse texture: total area, hard-edge length per square metre, and the mean angle at those edges.

This writes a report, never the rtx.conf list: each texture still has to be checked in a capture before it is added.

Usage: py -3.12 scripts/scan_hard_edges.py [--data <DATA\\unpacked>] [--bbformats <dir>] [--map <id part>] [--top N]
"""
import argparse
import collections
import glob
import os
import sys

import numpy as np

from gen_sky_meshes import diffuse_name

SPLIT_DEG = 5.0      # vertex normals of the two triangles at an edge differ by more than this: the edge is split
FOLD_DEG = 10.0      # and the faces meet at more than this: a crease, not a flat seam
MAX_FOLD_DEG = 170.0 # and less than this: beyond it the two faces are back to back (double-sided), not a crease


def mesh_hard_edges(mesh):
    fs = mesh.lod0()
    if fs is None or 'normal' not in mesh.vertices:
        return None
    p = mesh.vertices['position']
    p = np.asarray(p[0] if isinstance(p, list) else p, np.float64)[:, :3]
    nr = mesh.vertices['normal']
    nr = np.asarray(nr[0] if isinstance(nr, list) else nr, np.float64)[:, :3]
    nr = nr / np.maximum(np.linalg.norm(nr, axis=1), 1e-12)[:, None]
    t = fs.triangles()
    t = t[(t < len(p)).all(1)]
    if not len(t):
        return None
    w = p[t]
    g = np.cross(w[:, 1] - w[:, 0], w[:, 2] - w[:, 0])
    glen = np.linalg.norm(g, axis=1)
    area = 0.5 * glen
    keep = area > 1e-5
    t, w, g, glen, area = t[keep], w[keep], g[keep], glen[keep], area[keep]
    gn = g / glen[:, None]
    # positions to keys (1 mm), so split vertices at one point share a key
    key = np.round(p * 1000.0).astype(np.int64)
    kid = {}
    pk = np.array([kid.setdefault(tuple(k), len(kid)) for k in key], np.int64)
    edges = {}
    for f in range(len(t)):
        for a, b in ((0, 1), (1, 2), (2, 0)):
            va, vb = t[f, a], t[f, b]
            ka, kb = pk[va], pk[vb]
            edges.setdefault((ka, kb) if ka < kb else (kb, ka), []).append((f, va, vb))
    cos_split = np.cos(np.radians(SPLIT_DEG))
    hard_len = 0.0
    hard_angles = []
    for (ka, _kb), sides in edges.items():
        # only edges two triangles share: three or more is a fin or overlapping cards, not a surface
        if len(sides) != 2:
            continue
        (f, va, vb), (f2, wa, wb) = sides
        # the same two points in the other triangle (in either order)
        if pk[va] != ka:
            va, vb = vb, va
        if pk[wa] != ka:
            wa, wb = wb, wa
        split = np.dot(nr[va], nr[wa]) < cos_split or np.dot(nr[vb], nr[wb]) < cos_split
        fold = np.degrees(np.arccos(np.clip(np.dot(gn[f], gn[f2]), -1.0, 1.0)))
        # back-to-back faces (double-sided leaves, cards) fold by ~180 degrees: not a crease
        if split and FOLD_DEG < fold < MAX_FOLD_DEG:
            hard_len += float(np.linalg.norm(p[va] - p[vb]))
            hard_angles.append(fold)
    return area.sum(), hard_len, hard_angles


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=r'<steam dir>\steamapps\common'
                                      r'\Dark Souls Prepare to Die Edition\DATA\unpacked')
    ap.add_argument('--bbformats', default=r'<other project>\tools')
    ap.add_argument('--map', default='')
    ap.add_argument('--top', type=int, default=80)
    args = ap.parse_args()
    sys.path.insert(0, args.bbformats)
    from bbformats.flver import read_flver

    per = collections.defaultdict(lambda: dict(area=0.0, hard=0.0, angles=[], meshes=0, maps=set()))
    for map_dir in sorted(glob.glob(os.path.join(args.data, 'map', 'm*_*'))):
        map_id = os.path.basename(map_dir)
        if args.map not in map_id:
            continue
        for path in sorted(glob.glob(os.path.join(map_dir, '*.flver'))):
            flver = read_flver(open(path, 'rb').read())
            for mesh in flver.meshes:
                r = mesh_hard_edges(mesh)
                if r is None:
                    continue
                name = diffuse_name(flver.materials[mesh.material_index])
                d = per[name]
                d['area'] += r[0]
                d['hard'] += r[1]
                d['angles'] += r[2]
                d['meshes'] += 1
                d['maps'].add(map_id[:5])
        print(f'{map_id} done', file=sys.stderr, flush=True)

    rows = []
    for name, d in per.items():
        if d['area'] < 50.0 or not d['angles']:
            continue
        density = d['hard'] / d['area']  # metres of hard edge per square metre
        # ranked by total hard-edge length (area x density, capped): big distant meshes rank high, so check the maps column
        rows.append((d['area'] * min(density, 2.0), name, d['area'], density, float(np.mean(d['angles'])), d['meshes'],
                     ','.join(sorted(d['maps']))))
    rows.sort(reverse=True)
    print(f"{'texture':32} {'area m2':>9} {'hard m/m2':>9} {'mean deg':>8} {'meshes':>6}  maps")
    for score, name, area, density, ang, n, maps in rows[:args.top]:
        print(f'{name:32} {area:9.0f} {density:9.3f} {ang:8.1f} {n:6d}  {maps}')


if __name__ == '__main__':
    main()
