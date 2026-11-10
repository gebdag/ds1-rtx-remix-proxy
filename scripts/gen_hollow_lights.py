"""Generate assets/lights/m13_02_00_00.custom.txt: the Great Hollow lit from above, all the way down.

The game lights the Hollow with one overhead light that never changes with height: the shadow light c175 and the scattering
direction c111 point straight down, the probe is 0.99 on its top face, 0.45 on its sides and 0 below, and the hemisphere is
c98 from above and 0 from below, at the top of the shaft and at the bottom alike. Only geometry near the camera shadows it.
A path tracer's light from above cannot do that: by 280 m down, 95% of the walkable surface has a root within 60 m overhead.

So a light at the top (the Remix sun already comes straight down from c175 too), then wide spots facing down, hung through
the shaft in the open air, each lighting the stretch below it. Chosen greedily from candidates above the surfaces, tested
with rays against the map's opaque pieces. Each surface wants light by its facing, as the game's probe gives it: all facing
up, about half sideways, none facing down.

The tunnel entrance at the top, a tube about 5 m across, is lit evenly in the game (largely by the player's head light, reach
18 m, which moves with the player). Its lights are plain spheres at fixed stops along the tube, their strengths solved together
so that the light on the tube's surfaces comes out even: strengths set from each light's own surroundings left it patchy.

Needs embreex (pip install embreex).

Usage: py -3.12 scripts/gen_hollow_lights.py [--strength S] [--want E] [--max-lights N] [--deploy]
"""
import argparse
import os
import shutil
import sys

import numpy as np

from map_geometry import Scene, blocks_rays, cosine_hemisphere, first, map_pieces, rotation, surface_samples, table_sources

HERE = os.path.dirname(os.path.abspath(__file__))
MAP = 'm13_02_00_00'
# The shaft, above the beach at its foot (Ash Lake is not meant).
BOX_MIN = np.array([-362.0, -398.0, 75.0])
BOX_MAX = np.array([-300.0, -200.0, 150.0])
# Where the captures stood, walking down from the tunnel: a walk over the floors starts from each.
SEEDS = [(-331.4, -207.0, 81.9), (-343.3, -209.7, 96.1), (-351.3, -220.5, 120.3), (-318.4, -237.7, 120.4), (-345.7, -260.5, 106.7),
         (-326.3, -261.3, 121.3), (-331.6, -271.0, 118.0), (-339.4, -287.6, 112.3), (-316.1, -314.0, 122.2), (-317.6, -342.8, 114.7),
         (-336.6, -343.9, 121.5), (-341.1, -359.0, 121.4), (-320.9, -370.4, 118.7), (-322.5, -392.2, 116.5),
         (-338.8, -207.8, 90.4), (-348.8, -219.5, 114.8), (-342.4, -224.6, 126.5), (-326.4, -227.7, 130.1), (-318.6, -236.0, 120.7)]
# The tunnel entrance, its way down to the ladder: the floor under the player in the captures (head light c112 + camera, less
# the head's height). The cameras ride up to 3.5 m above it. Past this the shaft's lights take over.
TUNNEL = [(-331.4, -207.0, 78.0), (-338.8, -207.8, 90.4), (-343.3, -210.0, 96.1), (-348.8, -219.5, 114.8), (-342.4, -224.6, 126.5),
          (-326.4, -227.7, 130.1)]
TUNNEL_NEAR = 7.0
TUNNEL_SPACING = 3.0
TUNNEL_TARGETS = 4000
# Most light a tunnel stop may give its nearest surface, as a share of what the surfaces want: more is a hot patch.
TUNNEL_HOT = 1.6
# Light wanted by normal y: down, sideways, up. The game's probe and hemisphere give 0.47 sideways before its lightmaps mask the
# probe; at 0.47 the walls and roots beside the first bonfire stood out bright where the game's are dark.
FACING = ([-1.0, 0.0, 1.0], [0.0, 0.25, 1.0])
# c98 + probe top * c86 in linear, corrected against the game by the frame colour over ten captures (red 1.09, blue 0.83).
COLOUR = '213 255 205'
TOP = (-331.0, -135.0, 118.0)
TARGETS = 16000
CANDIDATES = 8000
REACH = 34.0
SEE = 45.0
CLEARANCE = 1.5
ROOMY = 4.0
DIM = 1.0 / 3.0
STEP = 1.5
RAYS_PER_CELL = 10
CONE = 80.0
SOFTNESS = 0.3
SPACING = 5.0
# Strengths here are what a light of this radius would be given. Remix's unit is radiance: a sphere's output goes with its
# radius squared.
REFERENCE_RADIUS = 2.5
MIN_RADIUS = 0.5


def radiance(strength, radius):
    return strength * (REFERENCE_RADIUS / radius) ** 2


def distance_to_tunnel(points):
    best = np.full(len(points), np.inf)
    for a, b in zip(TUNNEL[:-1], TUNNEL[1:]):
        a, b = np.array(a), np.array(b)
        s = np.clip(((points - a) @ (b - a)) / max(((b - a) ** 2).sum(), 1e-9), 0.0, 1.0)
        best = np.minimum(best, np.linalg.norm(points - (a + s[:, None] * (b - a)), axis=1))
    return best


def spot_shape(to_light):
    """A downward spot's strength towards a surface, from the unit vector surface to light."""
    angle = np.degrees(np.arccos(np.clip(to_light[:, 1], -1.0, 1.0)))
    t = np.clip((CONE - angle) / (CONE * SOFTNESS), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=r'<steam dir>\steamapps\common'
                                      r'\Dark Souls Prepare to Die Edition\DATA\unpacked')
    ap.add_argument('--bbformats', default=r'<other project>\tools')
    ap.add_argument('--strength', type=float, default=56.0, help='each light, in Remix units')
    ap.add_argument('--want', type=float, default=0.48, help='light an up-facing surface should get, strength * cos / d^2')
    ap.add_argument('--top', type=float, default=800.0, help='the light at the top of the shaft')
    ap.add_argument('--tunnel', type=float, default=0.8, help='light on the tunnel floor, strength / d^2')
    ap.add_argument('--max-lights', type=int, default=140)
    ap.add_argument('--deploy', action='store_true', help='also into the game\'s rtx_comp\\lights')
    args = ap.parse_args()
    sys.path.insert(0, args.bbformats)
    from bbformats.flver import read_flver

    tables_dir = os.path.join(HERE, '..', 'assets', 'mesh_tables')
    no_block = table_sources(os.path.join(tables_dir, 'sky_meshes.txt')) | table_sources(os.path.join(tables_dir, 'glass_meshes.txt'))
    msb = os.path.join(args.data, 'map', 'MapStudio', f'{MAP}.msb')
    area = MAP[1:3]
    flvers, world_tris, near, shell_tris = {}, [], [], []
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
            if f'{source}/lod0' in no_block:
                continue
            world = p @ r.T + at
            if (world.max(0) >= BOX_MIN).all() and (world.min(0) <= BOX_MAX).all():
                shell_tris.append(world[t])
            if not blocks_rays(str(flver.materials[mesh.material_index].mtd)):
                continue
            normals = np.asarray(first(mesh.vertices['normal']), np.float64)[:, :3] @ r.T if 'normal' in mesh.vertices else None
            world_tris.append(world[t])
            lo, hi = world.min(0), world.max(0)
            if (hi >= BOX_MIN).all() and (lo <= BOX_MAX).all() and normals is not None:
                near.append((world, t, normals))
    scene = Scene(np.concatenate(world_tris))
    # What the tunnel's surfaces are to the eye: the opaque pieces and the alpha-tested shell on them.
    tube_scene = Scene(np.concatenate(shell_tris))
    print(f'{len(world_tris)} opaque meshes, {len(near)} around the shaft')

    # The floors the player can walk, joined from where the captures stood.
    rng = np.random.default_rng(13)
    ps, fs = [], []
    for w, tri, n in near:
        a = 0.5 * np.linalg.norm(np.cross(w[tri][:, 1] - w[tri][:, 0], w[tri][:, 2] - w[tri][:, 0]), axis=1).sum()
        s = surface_samples(w, tri, n, max(int(min(a, 8000.0) * 6), 8), rng)
        if s is not None:
            ps.append(s[0])
            fs.append(s[1])
    ps, fs = np.concatenate(ps), np.concatenate(fs)
    keep = ((ps >= BOX_MIN) & (ps <= BOX_MAX)).all(1) & (fs[:, 1] > 0.5)
    floors = ps[keep]
    up = np.tile(np.array([0.0, 1.0, 0.0]), (len(floors), 1))
    head = scene.first_hit(floors + np.array([0.0, 0.1, 0.0]), up)
    floors = floors[head > 1.8]
    cells = {}
    for q in floors:
        cells.setdefault((int(q[0] // STEP), int(q[2] // STEP), int(q[1] // 1.0)), []).append(q)
    nodes = {k: np.mean(v, axis=0) for k, v in cells.items()}
    keys = list(nodes)
    at = np.array([nodes[k] for k in keys])
    by_xz = {}
    for k in keys:
        by_xz.setdefault(k[:2], []).append(k)
    stack = [keys[int(np.argmin(np.linalg.norm(at - np.array(s), axis=1)))] for s in SEEDS]
    seen = set()
    while stack:
        k = stack.pop()
        if k in seen:
            continue
        seen.add(k)
        for dx in (-1, 0, 1):
            for dz in (-1, 0, 1):
                for o in by_xz.get((k[0] + dx, k[1] + dz), ()):
                    if o not in seen and abs(nodes[o][1] - nodes[k][1]) < 1.4:
                        stack.append(o)
    walk = np.array([nodes[k] for k in seen])
    print(f'{len(floors)} floor points, {len(walk)} walkable cells of {len(nodes)}')

    # The surfaces to light: what the player sees from those floors, within the shaft.
    origins = np.repeat(walk + np.array([0.0, 1.5, 0.0]), RAYS_PER_CELL, axis=0)
    dirs = rng.normal(size=(len(origins), 3))
    dirs /= np.linalg.norm(dirs, axis=1, keepdims=True)
    r = scene.scene.run(origins.astype(np.float32), dirs.astype(np.float32), output=True)
    dist = np.asarray(r['tfar']).astype(np.float64)
    hit = (np.asarray(r['primID']).astype(np.int64) >= 0) & (dist < SEE)
    ng = np.asarray(r['Ng']).astype(np.float64)[hit]
    ng /= np.maximum(np.linalg.norm(ng, axis=1, keepdims=True), 1e-9)
    ng[(ng * dirs[hit]).sum(1) > 0] *= -1
    surface = origins[hit] + dirs[hit] * dist[hit, None]
    inside = ((surface >= BOX_MIN) & (surface <= BOX_MAX)).all(1)
    surface, ng = surface[inside], ng[inside]
    pick = rng.choice(len(surface), min(TARGETS, len(surface)), replace=False)
    targets, normals = surface[pick], ng[pick]
    in_tunnel = distance_to_tunnel(targets) < TUNNEL_NEAR
    want = args.want * np.interp(normals[:, 1], *FACING)
    # The tunnel has its own lights below.
    want[in_tunnel] = 0.0
    print(f'{len(surface)} seen surface points, {len(targets)} targets, {in_tunnel.sum()} in the tunnel')

    # Candidates over the shaft's up-facing surfaces.
    facing_up = np.nonzero((normals[:, 1] > 0.3) & ~in_tunnel)[0]
    base = targets[rng.choice(facing_up, CANDIDATES * 3)]
    roof = scene.first_hit(base + np.array([0.0, 0.1, 0.0]), np.tile(np.array([0.0, 1.0, 0.0]), (len(base), 1)))
    rise = np.minimum(rng.uniform(5.0, 22.0, len(base)), 0.75 * roof)
    cand = base + np.c_[np.zeros(len(base)), rise, np.zeros(len(base))]
    around = rng.normal(size=(26, 3))
    around /= np.linalg.norm(around, axis=1, keepdims=True)

    def clearance(points):
        lowest = np.full(len(points), np.inf)
        for d in around:
            lowest = np.minimum(lowest, scene.first_hit(points, np.tile(d, (len(points), 1))))
        return lowest

    lowest = clearance(cand)
    good = (lowest >= CLEARANCE) & (distance_to_tunnel(cand) >= TUNNEL_NEAR)
    order = rng.permutation(np.nonzero(good)[0])[:CANDIDATES]
    cand, lowest = cand[order], lowest[order]
    print(f'{len(cand)} candidates')

    bright = lowest >= ROOMY
    cand = np.concatenate([cand[bright], cand])
    scale = np.concatenate([np.ones(bright.sum()), np.full(len(lowest), DIM)])
    lowest = np.concatenate([lowest[bright], lowest])

    # Along the tunnel: stops at the middle of the tube, between its walls and halfway from floor to roof.
    def tube_clearance(points):
        lowest = np.full(len(points), np.inf)
        for d in around:
            lowest = np.minimum(lowest, tube_scene.first_hit(points, np.tile(d, (len(points), 1))))
        return lowest

    def tube_middle(q):
        for _ in range(3):
            hits = []
            for angle in np.arange(0.0, 2.0 * np.pi, np.pi / 4.0):
                d = np.array([np.cos(angle), 0.0, np.sin(angle)])
                hits.append(q + d * min(tube_scene.first_hit(q[None], d[None])[0], 6.0))
            q = np.array([np.mean(hits, axis=0)[0], q[1], np.mean(hits, axis=0)[2]])
        return q

    tube = []
    tunnel_walk = walk[distance_to_tunnel(walk) < 4.0]
    for a, b in zip(TUNNEL[:-1], TUNNEL[1:]):
        a, b = np.array(a), np.array(b)
        for s in np.arange(0.0, np.linalg.norm(b - a), TUNNEL_SPACING):
            stop = a + (b - a) / np.linalg.norm(b - a) * s
            floor = tunnel_walk[int(np.argmin(np.linalg.norm(tunnel_walk - stop, axis=1)))]
            if tube and min(np.linalg.norm(floor - q[0]) for q in tube) < 0.6 * TUNNEL_SPACING:
                continue
            ceiling = tube_scene.first_hit((floor + np.array([0.0, 0.1, 0.0]))[None], np.array([[0.0, 1.0, 0.0]]))[0]
            c = tube_middle(floor + np.array([0.0, min(0.55 * ceiling, 2.2), 0.0]))
            nearest = tube_clearance(c[None])[0]
            if nearest >= 0.5:
                tube.append((floor, c, max(min(0.8 * nearest, 0.8), MIN_RADIUS), nearest))
    print(f'{len(tube)} stops along the tunnel, nearest surface {min(q[3] for q in tube):.1f} to {max(q[3] for q in tube):.1f} m')

    # The tube's surfaces: what the player sees from its floor.
    origin_t = np.repeat(tunnel_walk + np.array([0.0, 1.5, 0.0]), 40, axis=0)
    dir_t = rng.normal(size=(len(origin_t), 3))
    dir_t /= np.linalg.norm(dir_t, axis=1, keepdims=True)
    r = tube_scene.scene.run(origin_t.astype(np.float32), dir_t.astype(np.float32), output=True)
    dt = np.asarray(r['tfar']).astype(np.float64)
    ok = (np.asarray(r['primID']).astype(np.int64) >= 0) & (dt < 12.0)
    ntube = np.asarray(r['Ng']).astype(np.float64)[ok]
    ntube /= np.maximum(np.linalg.norm(ntube, axis=1, keepdims=True), 1e-9)
    ntube[(ntube * dir_t[ok]).sum(1) > 0] *= -1
    ptube = origin_t[ok] + dir_t[ok] * dt[ok, None]
    close = distance_to_tunnel(ptube) < TUNNEL_NEAR
    ptube, ntube = ptube[close], ntube[close]
    pick = rng.choice(len(ptube), min(TUNNEL_TARGETS, len(ptube)), replace=False)
    ptube, ntube = ptube[pick], ntube[pick]

    # The light each stop gives each surface at strength 1, then the strengths that bring every surface nearest the same light.
    gain = np.zeros((len(ptube), len(tube)))
    for j, (_, c, _, _) in enumerate(tube):
        to = c - ptube
        d = np.linalg.norm(to, axis=1)
        direction = to / d[:, None]
        cosine = np.maximum((direction * ntube).sum(1), 0.0)
        seen = scene.first_hit(ptube + ntube * 0.05, direction) >= d - 0.5
        gain[:, j] = cosine * seen / np.maximum(d, 1.0) ** 2
    reached = gain.sum(1) > 0
    a_t = gain[reached]
    hottest = np.array([args.tunnel * TUNNEL_HOT * max(q[3], 1.0) ** 2 for q in tube])
    strengths = np.minimum(np.full(len(tube), args.tunnel * 4.0), hottest)
    step = 1.0 / np.linalg.norm(a_t, 2) ** 2
    for _ in range(4000):
        strengths = np.clip(strengths - step * a_t.T @ (a_t @ strengths - args.tunnel), 0.0, hottest)
    e_t = a_t @ strengths
    print(f'tunnel: {reached.mean():.2f} of {len(ptube)} surfaces reached; light p10 {np.percentile(e_t, 10):.2f} median {np.median(e_t):.2f} p90 {np.percentile(e_t, 90):.2f}, wanted {args.tunnel:g}')

    def reaches(c, spot, strength, radius):
        """(target indices, light) from a light at c."""
        to = c - targets
        d = np.linalg.norm(to, axis=1)
        idx = np.nonzero(d < radius)[0]
        if not len(idx):
            return np.zeros(0, int), np.zeros(0)
        direction = to[idx] / d[idx, None]
        cosine = (direction * normals[idx]).sum(1)
        e = strength * cosine / np.maximum(d[idx], 1.0) ** 2
        if spot:
            e *= spot_shape(direction)
        front = e > 1e-4
        idx, e, direction, length = idx[front], e[front], direction[front], d[idx][front]
        if not len(idx):
            return idx, e
        seen = scene.first_hit(targets[idx] + normals[idx] * 0.05, direction) >= length - 1.0
        return idx[seen], e[seen]

    gives = [reaches(c, True, args.strength * k, REACH) for c, k in zip(cand, scale)]
    print('visibility done')

    have = np.zeros(len(targets))
    idx, e = reaches(np.array(TOP), True, args.top, 400.0)
    have[idx] += e
    print(f'the top light reaches {len(idx)} targets')
    chosen = []
    total = want.sum()
    for _ in range(args.max_lights):
        best, gain = -1, 0.0
        for i, (idx, e) in enumerate(gives):
            if not len(idx):
                continue
            g = (np.minimum(have[idx] + e, want[idx]) - np.minimum(have[idx], want[idx])).sum()
            # Light past twice a surface's share is a hot patch.
            over_share = np.maximum(have[idx] + e - 2.0 * want[idx], 0.0) - np.maximum(have[idx] - 2.0 * want[idx], 0.0)
            g -= 0.5 * over_share.sum()
            if g > gain:
                best, gain = i, g
        if best < 0 or gain < 0.002 * total / 50.0:
            break
        idx, e = gives[best]
        have[idx] += e
        chosen.append(best)
        # Not two in one place.
        for j in np.nonzero(np.linalg.norm(cand - cand[best], axis=1) < SPACING)[0]:
            gives[j] = (np.zeros(0, int), np.zeros(0))
    wanting = want > 0.02 * args.want
    covered = have >= want * 0.8
    print(f'{len(chosen)} spots ({sum(scale[i] == 1.0 for i in chosen)} bright); '
          f'surfaces with at least 80% of their share: {covered[wanting].mean():.2f}')
    for y in range(-200, -400, -20):
        k = (targets[:, 1] <= y) & (targets[:, 1] > y - 20) & wanting
        lights_here = sum((cand[i][1] <= y) and (cand[i][1] > y - 20) for i in chosen)
        if k.any():
            print(f'  y {y:5d}: {k.sum():5d} targets, {covered[k].mean():.2f} covered, median share {np.median(have[k] / want[k]):.2f}, {lights_here} lights')
    lines = [f'# Custom lights for {MAP} (the Great Hollow). F4 > Lights > Custom lights edits and saves this file.',
             '# x y z  dwindleBegin dwindleEnd  r g b  intensityPct  name  [remix intensity exposure radius volumetric]',
             '#   [spot dx dy dz coneDeg softness]  [focus exponent]  [off]',
             "# With 'remix' a plain Remix sphere light; without, the game's falloff (dwindle begin/end, intensityPct).",
             '#',
             '# Generated by scripts/gen_hollow_lights.py, placed against the map geometry. See there. Saving from the menu keeps your edits',
             '# until the script is run again.',
             f'{TOP[0]:.1f} {TOP[1]:.1f} {TOP[2]:.1f} 8 12 {COLOUR} 600 Hollow_top remix {radiance(args.top, 10.0):.2f} 0.00 10.000 1.00 spot 0.0000 -1.0000 0.0000 {CONE:.1f} {SOFTNESS:.2f}']
    for n, i in enumerate(sorted(chosen, key=lambda k: -cand[k][1])):
        c = cand[i]
        radius = max(min(REFERENCE_RADIUS, lowest[i] * 0.8), MIN_RADIUS)
        lines.append(f'{c[0]:.1f} {c[1]:.1f} {c[2]:.1f} 8 12 {COLOUR} 600 Hollow_{n + 1:03d} remix {radiance(args.strength * scale[i], radius):.2f} 0.00 {radius:.3f} 1.00'
                     f' spot 0.0000 -1.0000 0.0000 {CONE:.1f} {SOFTNESS:.2f}')
    kept = [(c, radius, s) for (_, c, radius, _), s in zip(tube, strengths) if s > 0.02]
    for n, (c, radius, s) in enumerate(kept):
        lines.append(f'{c[0]:.1f} {c[1]:.1f} {c[2]:.1f} 8 12 {COLOUR} 600 Hollow_tunnel_{n + 1:02d} remix {radiance(s, radius):.2f} 0.00 {radius:.3f} 1.00')
    print(f'{len(kept)} of {len(tube)} tunnel stops have a light')
    text = '\n'.join(lines) + '\n'
    out = os.path.join(HERE, '..', 'assets', 'lights', f'{MAP}.custom.txt')
    # Ash Lake shares the map: its hand-placed lights at the end of the file stay.
    keep = ''
    if os.path.exists(out):
        previous = open(out, encoding='utf-8').read()
        at = previous.find("# Ash Lake, the Everlasting Dragon's end")
        keep = previous[at:] if at >= 0 else ''
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text if text.endswith('\n') else text + '\n')
        f.write(keep)
    if args.deploy:
        shutil.copy2(out, os.path.join(os.path.dirname(args.data), 'rtx_comp', 'lights', f'{MAP}.custom.txt'))
    print('wrote', os.path.normpath(out))


if __name__ == '__main__':
    main()
