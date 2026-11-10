"""Generate assets/lights/m14_00_00_00.custom.txt: soft lights for Quelaag's Domain, placed against the map's own geometry.

The game lights this area by a flat ambient and a warm probe term masked by lightmaps, with no light behind either. A path
tracer finds nothing there to light it with. These are large Remix spheres hung in the open space so that the area's
surfaces each see enough of them, chosen greedily: from points in the open air, the one that adds most of the light still
missing, tested with rays against the map's opaque pieces, until nearly every surface has its share. Surfaces facing down
want more than those facing up: the game's probe is brightest on down-facing surfaces (0.84 against 0.21 up), so its
caves are bright overhead and dark underfoot.

Needs embreex (pip install embreex).

Usage: py -3.12 scripts/gen_domain_lights.py [--strength S] [--want E] [--deploy]
"""
import argparse
import os
import shutil
import sys

import numpy as np

from map_geometry import Scene, blocks_rays, cosine_hemisphere, first, map_pieces, rotation, surface_samples, table_sources

HERE = os.path.dirname(os.path.abspath(__file__))
MAP = 'm14_00_00_00'
# The Domain, from the captures: the cave, the ramp, the paved corridor, the rooms past it.
BOX_MIN = np.array([-135.0, -262.0, 55.0])
BOX_MAX = np.array([175.0, -150.0, 195.0])
TARGETS = 14000
CANDIDATES = 7000
REACH = 18.0
# A light at least this far from everything, and below ROOMY a dim one: a bright one close by burns a patch into the nearest surface.
CLEARANCE = 1.5
ROOMY = 4.0
DIM = 1.0 / 3.0
COLOUR = '255 238 160'
STEP = 1.5
RAYS_PER_CELL = 12
SEE = 35.0
# The way through, cave to bonfire chamber, in the order the captures walked it.
ROUTE = [(-49.5, -209.5, 135.1), (-29.4, -220.5, 122.8), (13.7, -237.0, 112.5), (34.2, -242.0, 127.3), (82.8, -239.0, 129.1), (99.5, -234.0, 129.9),
         (110.8, -236.0, 126.3), (115.4, -251.5, 134.0), (130.0, -251.5, 127.0), (129.6, -253.0, 114.0), (139.7, -252.5, 100.1)]
# Surfaces this close to it count this much more.
ROUTE_NEAR = 25.0
ROUTE_WEIGHT = 6.0
# Where the captures stood: a walk starts from each. Quelaag's room lies past these and is not meant.
SEEDS = [(130.0, -251.5, 127.0), (115.4, -251.5, 134.0), (129.6, -253.0, 114.0), (139.7, -252.5, 100.1), (-49.5, -209.5, 135.1), (-29.4, -220.5, 122.8),
         (13.7, -237.0, 112.5), (34.2, -242.0, 127.3), (82.8, -239.0, 129.1), (99.5, -234.0, 129.9), (110.8, -236.0, 126.3)]


def distance_to_route(points):
    """How far each point is from the walk through the area, the line through ROUTE."""
    best = np.full(len(points), np.inf)
    for a, b in zip(ROUTE[:-1], ROUTE[1:]):
        a, b = np.array(a), np.array(b)
        s = np.clip(((points - a) @ (b - a)) / max(((b - a) ** 2).sum(), 1e-9), 0.0, 1.0)
        best = np.minimum(best, np.linalg.norm(points - (a + s[:, None] * (b - a)), axis=1))
    return best


def enclosed(scene, p, front, rng, rays=12):
    """Share of cosine rays from each point that hit something: the far side of a room's shell looks out into nothing."""
    hits = np.zeros(len(p))
    for _ in range(rays):
        d = scene.first_hit(p + front * 0.03, cosine_hemisphere(front, rng))
        hits += np.isfinite(d)
    return hits / rays


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=r'<steam dir>\steamapps\common'
                                      r'\Dark Souls Prepare to Die Edition\DATA\unpacked')
    ap.add_argument('--bbformats', default=r'<other project>\tools')
    ap.add_argument('--strength', type=float, default=12.0, help='each light, in Remix units')
    ap.add_argument('--want', type=float, default=0.12, help='light a side-facing surface should get, strength * cos / d^2')
    ap.add_argument('--max-lights', type=int, default=110)
    ap.add_argument('--deploy', action='store_true', help='also into the game\'s rtx_comp\\lights')
    args = ap.parse_args()
    sys.path.insert(0, args.bbformats)
    from bbformats.flver import read_flver

    tables_dir = os.path.join(HERE, '..', 'assets', 'mesh_tables')
    no_block = table_sources(os.path.join(tables_dir, 'sky_meshes.txt')) | table_sources(os.path.join(tables_dir, 'glass_meshes.txt'))
    msb = os.path.join(args.data, 'map', 'MapStudio', f'{MAP}.msb')
    area = MAP[1:3]
    flvers, world_tris, blockers = {}, [], []
    for model, at, rot, scale, _light in map_pieces(msb):
        path = os.path.join(args.data, 'map', MAP, f'{model}A{area}.flver')
        if not os.path.exists(path):
            continue
        if model not in flvers:
            flvers[model] = read_flver(open(path, 'rb').read())
        flver = flvers[model]
        r = rotation(rot) * scale
        for i, mesh in enumerate(flver.meshes):
            fs = mesh.lod0()
            if fs is None or 'position' not in mesh.vertices:
                continue
            p = np.asarray(first(mesh.vertices['position']), np.float64)[:, :3]
            t = fs.triangles()
            t = t[(t < len(p)).all(1)]
            if not len(t):
                continue
            source = f'{MAP}/{model}A{area}/mesh{i}'
            if f'{source}/lod0' in no_block or not blocks_rays(str(flver.materials[mesh.material_index].mtd)):
                continue
            world = p @ r.T + at
            normals = np.asarray(first(mesh.vertices['normal']), np.float64)[:, :3] @ r.T if 'normal' in mesh.vertices else None
            world_tris.append(world[t])
            lo, hi = world.min(0), world.max(0)
            if (hi >= BOX_MIN).all() and (lo <= BOX_MAX).all():
                blockers.append((world, t, normals))
    scene = Scene(np.concatenate(world_tris))
    print(f'{len(world_tris)} opaque meshes, {len(blockers)} around the Domain')

    # The floors the player can walk: up-facing surface points joined from the captures' positions.
    rng = np.random.default_rng(14)
    ps, fs = [], []
    for w, tri, n in blockers:
        a = 0.5 * np.linalg.norm(np.cross(w[tri][:, 1] - w[tri][:, 0], w[tri][:, 2] - w[tri][:, 0]), axis=1).sum()
        s = surface_samples(w, tri, n, max(int(a * 6), 8), rng)
        if s is not None:
            ps.append(s[0])
            fs.append(s[1])
    ps, fs = np.concatenate(ps), np.concatenate(fs)
    keep = ((ps >= BOX_MIN) & (ps <= BOX_MAX)).all(1) & (fs[:, 1] > 0.7)
    floors = ps[keep]
    head = scene.first_hit(floors + np.array([0.0, 0.1, 0.0]), np.tile(np.array([0.0, 1.0, 0.0]), (len(floors), 1)))
    floors = floors[head > 1.8]
    cells = {}
    for q in floors:
        cells.setdefault((int(q[0] // STEP), int(q[2] // STEP), int(q[1] // 1.0)), []).append(q)
    nodes = {k: np.mean(v, axis=0) for k, v in cells.items()}
    keys = list(nodes)
    at = np.array([nodes[k] for k in keys])
    seen = set()
    stack = []
    for s in SEEDS:
        d = np.linalg.norm(at - np.array(s), axis=1)
        stack.append(keys[int(np.argmin(d))])
    by_xz = {}
    for k in keys:
        by_xz.setdefault(k[:2], []).append(k)
    while stack:
        k = stack.pop()
        if k in seen:
            continue
        seen.add(k)
        for dx in (-1, 0, 1):
            for dz in (-1, 0, 1):
                for o in by_xz.get((k[0] + dx, k[1] + dz), ()):
                    if o not in seen and abs(nodes[o][1] - nodes[k][1]) < 1.2:
                        stack.append(o)
    walk = np.array([nodes[k] for k in seen])
    print(f'{len(floors)} floor points, {len(walk)} walkable cells of {len(nodes)}; x {walk[:, 0].min():.0f} to {walk[:, 0].max():.0f}, y {walk[:, 1].min():.0f} to {walk[:, 1].max():.0f}')

    # What the player sees from those floors: rays from head height. Their hits are the surfaces to light, and points along
    # them are in the same open space.
    # Mostly from the cells near the way through.
    near_route = distance_to_route(walk) < ROUTE_NEAR
    order = np.concatenate([np.nonzero(near_route)[0][rng.permutation(near_route.sum())[:4000]], np.nonzero(~near_route)[0][rng.permutation((~near_route).sum())[:1000]]])
    origins = np.repeat(walk[order] + np.array([0.0, 1.5, 0.0]), RAYS_PER_CELL, axis=0)
    dirs = rng.normal(size=(len(origins), 3))
    dirs[:, 1] = np.abs(dirs[:, 1]) * np.where(rng.random(len(origins)) < 0.75, 1.0, -1.0)
    dirs /= np.linalg.norm(dirs, axis=1, keepdims=True)
    r = scene.scene.run(origins.astype(np.float32), dirs.astype(np.float32), output=True)
    dist = np.asarray(r['tfar']).astype(np.float64)
    hit = (np.asarray(r['primID']).astype(np.int64) >= 0) & (dist < SEE)
    ng = np.asarray(r['Ng']).astype(np.float64)[hit]
    ng /= np.maximum(np.linalg.norm(ng, axis=1, keepdims=True), 1e-9)
    ng[(ng * dirs[hit]).sum(1) > 0] *= -1
    surface = origins[hit] + dirs[hit] * dist[hit, None]
    pick = rng.choice(len(surface), min(TARGETS, len(surface)), replace=False)
    targets, normals = surface[pick], ng[pick]
    print(f'{len(surface)} seen surface points; y {surface[:, 1].min():.0f} to {surface[:, 1].max():.0f}')
    # What each surface wants: most facing down, least facing up.
    want = args.want * np.interp(normals[:, 1], [-1.0, 0.0, 1.0], [1.5, 1.0, 0.5])
    importance = np.where(distance_to_route(targets) < ROUTE_NEAR, ROUTE_WEIGHT, 1.0)

    # Candidates: points along those rays, clear on every side.
    long_enough = hit & (dist > 3.5)
    frac = rng.uniform(0.35, 0.8, long_enough.sum())
    cand = origins[long_enough] + dirs[long_enough] * (dist[long_enough] * frac)[:, None]
    # The spots near the way through first: tight tunnels would lose out to the big rooms.
    close = distance_to_route(cand) < ROUTE_NEAR
    cand = np.concatenate([cand[close][rng.permutation(close.sum())[:CANDIDATES * 3]], cand[~close][rng.permutation((~close).sum())[:CANDIDATES]]])
    around = rng.normal(size=(26, 3))
    around /= np.linalg.norm(around, axis=1, keepdims=True)
    lowest = np.full(len(cand), np.inf)
    for d in around:
        lowest = np.minimum(lowest, scene.first_hit(cand, np.tile(d, (len(cand), 1))))
    good = lowest >= CLEARANCE
    cand, lowest = cand[good][:CANDIDATES], lowest[good][:CANDIDATES]
    print(f'{len(cand)} candidate spots')

    # The light each candidate gives each target. Each spot once bright, where there is room, and once dim.
    bright = lowest >= ROOMY
    cand = np.concatenate([cand[bright], cand])
    scale = np.concatenate([np.ones(bright.sum()), np.full(len(lowest), DIM)])
    lowest = np.concatenate([lowest[bright], lowest])
    gives = []
    for c, c_scale in zip(cand, scale):
        to = c - targets
        d = np.linalg.norm(to, axis=1)
        idx = np.nonzero(d < REACH)[0]
        if not len(idx):
            gives.append((idx, np.zeros(0)))
            continue
        cosine = (to[idx] * normals[idx]).sum(1) / d[idx]
        idx, cosine = idx[cosine > 0.1], cosine[cosine > 0.1]
        if not len(idx):
            gives.append((idx, np.zeros(0)))
            continue
        direction = to[idx] / d[idx, None]
        o = targets[idx] + normals[idx] * 0.05
        hit = scene.first_hit(o, direction)
        seen = hit >= d[idx] - 1.0
        gives.append((idx[seen], args.strength * c_scale * cosine[seen] / np.maximum(d[idx][seen], 1.0) ** 2))
    print('visibility done')

    have = np.zeros(len(targets))
    chosen = []
    for _ in range(args.max_lights):
        best, gain = -1, 0.0
        for i, (idx, e) in enumerate(gives):
            if not len(idx):
                continue
            g = ((np.minimum(have[idx] + e, want[idx]) - np.minimum(have[idx], want[idx])) * importance[idx]).sum()
            # Light past twice a surface's share is a hot patch.
            over = np.maximum(have[idx] + e - 2.0 * want[idx], 0.0) - np.maximum(have[idx] - 2.0 * want[idx], 0.0)
            g -= 0.5 * (over * importance[idx]).sum()
            if g > gain:
                best, gain = i, g
        if best < 0 or gain < 0.002 * want.sum() / 50.0:
            break
        idx, e = gives[best]
        have[idx] += e
        chosen.append(best)
        # Not two in one place.
        dist = np.linalg.norm(cand - cand[best], axis=1)
        for j in np.nonzero(dist < 5.0)[0]:
            gives[j] = (np.zeros(0, int), np.zeros(0))
    covered = have >= want * 0.8
    print(f'{len(chosen)} lights ({sum(scale[i] == 1.0 for i in chosen)} bright); surfaces with at least 80% of their share: {covered.mean():.3f}; none at all: {(have == 0).mean():.3f}')
    # Around each place the captures stood: did it get its share?
    for s in SEEDS:
        near = np.linalg.norm(targets - np.array(s), axis=1) < 15.0
        lights_near = sum(np.linalg.norm(cand[i] - np.array(s)) < 20.0 for i in chosen)
        if near.any():
            print(f'  at ({s[0]:6.1f}, {s[1]:6.1f}, {s[2]:6.1f}): {near.sum():5d} surface points, {covered[near].mean():.2f} with their share, {(have[near] == 0).mean():.2f} none; {lights_near} lights within 20 m')

    lines = [f"# Custom lights for {MAP} (Quelaag's Domain). F4 > Lights > Custom lights edits and saves this file.",
             '# x y z  dwindleBegin dwindleEnd  r g b  intensityPct  name  [remix intensity exposure radius volumetric]',
             '#   [spot dx dy dz coneDeg softness]  [focus exponent]  [off]',
             "# With 'remix' a plain Remix sphere light; without, the game's falloff (dwindle begin/end, intensityPct).",
             '#',
             '# Generated by scripts/gen_domain_lights.py, placed against the map geometry. See there. Saving from the menu keeps your edits',
             '# until the script is run again.']
    for n, i in enumerate(sorted(chosen, key=lambda k: cand[k][0])):
        c = cand[i]
        radius = min(2.5, lowest[i] * 0.8)
        lines.append(f'{c[0]:.1f} {c[1]:.1f} {c[2]:.1f} 8 12 {COLOUR} 600 Domain_{n + 1:02d} remix {args.strength * scale[i]:g} 0.00 {radius:.3f} 1.00')
    text = '\n'.join(lines) + '\n'
    out = os.path.join(HERE, '..', 'assets', 'lights', f'{MAP}.custom.txt')
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    if args.deploy:
        shutil.copy2(out, os.path.join(os.path.dirname(args.data), 'rtx_comp', 'lights', f'{MAP}.custom.txt'))
    print('wrote', os.path.normpath(out))


if __name__ == '__main__':
    main()
