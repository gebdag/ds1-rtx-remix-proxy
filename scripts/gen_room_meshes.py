"""Generate assets/sky/room_meshes.txt: the map meshes of shut-in areas, which the proxy can make glow.

Some DS1 areas are closed spaces the game lights by ambient alone: Quelaag's Domain, the Great Hollow, Bed of Chaos's room,
Duke's Archives. A path tracer finds nothing in them to light them with. The proxy's F4 option (game/room_glow.hpp) makes
their surfaces emit the game's own ambient. This table says which meshes; how bright comes from each draw's constants.

The test is per authored light region: every map piece has an MSB LightID, and the designers split a map into these
regions by what is meant to be lit together. A single mesh is no good for this: a wall sheltered under a Burg rampart
sees little sky too, while pieces in a closed cavern have odd faces that see a gap. Rays against the map's opaque geometry,
from each mesh's surface (world positions from its part transform). Sky model meshes, window glass, blended, additive, water
and alpha-tested cards do not block rays, so a window or an opening to the sky counts as open:
  - escape: share of cosine-weighted rays that leave the map;
  - reach: share of the surface a placed light reaches, unblocked (MSB Light events, assets/lights synthesized flames).
A region qualifies when its area-weighted escape is at most REGION_ESCAPE_MAX and its reach at most REGION_REACH_MAX.
Of a qualifying region, a mesh qualifies unless itself mostly open (MESH_ESCAPE_MAX). It must pass in every placement. A key
(index hash, count) shared with a mesh that fails is left out: the proxy recognises a draw by its indices alone, and short
strips recur across maps.

Measured over the captures (escape): the Great Hollow 0.005, Bed of Chaos 0.014, the Archives 0.014-0.016, Quelaag's room 0.061,
against Blighttown 0.12, the Burg 0.224, Firelink 0.315, Ash Lake's shore 0.80. Anor Londo's halls (0.057) pass here and are
left out at runtime by their low ambient (game/room_glow.hpp). See analysis/room_glow_report.md.

Not in the ray scene: objects (doors, lifts, fog walls); effect lights.

Table lines (one per face set, every LOD), the sky table format plus the scores:
  <FNV-1a 64 of the u16 index bytes, hex> <index count> <map>/<model>/mesh<i>/lod<j> <texture> <light id> <region escape> <region reach> <mesh escape>

Needs embreex (pip install embreex).

Usage: py -3.12 scripts/gen_room_meshes.py [--data <DATA\\unpacked>] [--bbformats <dir with bbformats>]
"""
import argparse
import glob
import hashlib
import math
import os
import struct
import sys
from pathlib import Path

import numpy as np

from gen_map_lights import param_prefix, read_bank, read_events
from gen_sky_meshes import diffuse_name, fnv1a64

HERE = os.path.dirname(os.path.abspath(__file__))
REGION_ESCAPE_MAX = 0.07
REGION_REACH_MAX = 0.03
MESH_ESCAPE_MAX = 0.15
RAYS = 600
REACH_SAMPLES = 200
PART_MAP_PIECE = 0


def _sjis(data, offset):
    return data[offset:data.index(b'\0', offset)].decode('shift-jis', errors='replace')


def _section(data, pos):
    _, _, count = struct.unpack_from('<iii', data, pos)
    offsets = struct.unpack_from(f'<{count - 1}i', data, pos + 12)
    return list(offsets), struct.unpack_from('<i', data, pos + 12 + 4 * (count - 1))[0]


def map_pieces(msb_path):
    """(model name, position, rotation in degrees, scale, light id) of each map piece part (MSB1 layout)."""
    data = open(msb_path, 'rb').read()
    model_offsets, pos = _section(data, 0)
    models = [_sjis(data, o + struct.unpack_from('<i', data, o)[0]) for o in model_offsets]
    _, pos = _section(data, pos)  # events
    _, pos = _section(data, pos)  # regions
    part_offsets, _ = _section(data, pos)
    out = []
    for o in part_offsets:
        _, kind, _, model = struct.unpack_from('<iIii', data, o)
        if kind == PART_MAP_PIECE and 0 <= model < len(models):
            entity = o + struct.unpack_from('<i', data, o + 0x58)[0]
            out.append((models[model], np.array(struct.unpack_from('<3f', data, o + 0x14), np.float64),
                        struct.unpack_from('<3f', data, o + 0x20), np.array(struct.unpack_from('<3f', data, o + 0x2C), np.float64),
                        struct.unpack_from('<b', data, entity + 4)[0]))
    return out


def rotation(degrees):
    """SoulsFormats' order: x, then z, then y on a vertex."""
    x, y, z = (math.radians(a) for a in degrees)
    rx = np.array([[1, 0, 0], [0, math.cos(x), -math.sin(x)], [0, math.sin(x), math.cos(x)]])
    ry = np.array([[math.cos(y), 0, math.sin(y)], [0, 1, 0], [-math.sin(y), 0, math.cos(y)]])
    rz = np.array([[math.cos(z), -math.sin(z), 0], [math.sin(z), math.cos(z), 0], [0, 0, 1]])
    return ry @ rz @ rx


def first(v):
    return v[0] if isinstance(v, list) else v


def blocks_rays(mtd):
    m = mtd.lower()
    return not any(s in m for s in ('_alp', '_add', 'water', '_edge', 'ntoa'))


def table_sources(path):
    out = set()
    with open(path, encoding='utf-8') as f:
        for line in f:
            if line.strip() and line[0] != '#':
                out.add(line.split()[2])
    return out


def cosine_hemisphere(normals, rng):
    u, v = rng.random(len(normals)), rng.random(len(normals))
    r, phi = np.sqrt(u), 2 * np.pi * v
    x, y, z = r * np.cos(phi), r * np.sin(phi), np.sqrt(1 - u)
    other = np.where(np.abs(normals[:, [0]]) > 0.9, [[0.0, 1.0, 0.0]], [[1.0, 0.0, 0.0]])
    t = np.cross(normals, other)
    t /= np.maximum(np.linalg.norm(t, axis=1, keepdims=True), 1e-12)
    return t * x[:, None] + np.cross(normals, t) * y[:, None] + normals * z[:, None]


def surface_samples(world, tris, normals, count, rng):
    """Area-weighted points on a mesh's faces, their fronts (from the vertex normals), the mesh's area."""
    w = world[tris]
    g = np.cross(w[:, 1] - w[:, 0], w[:, 2] - w[:, 0])
    area = 0.5 * np.linalg.norm(g, axis=1)
    if area.sum() <= 0:
        return None
    pick = rng.choice(len(tris), count, p=area / area.sum())
    u, v = rng.random(count), rng.random(count)
    flip = u + v > 1
    u[flip], v[flip] = 1 - u[flip], 1 - v[flip]
    p = w[pick, 0] + u[:, None] * (w[pick, 1] - w[pick, 0]) + v[:, None] * (w[pick, 2] - w[pick, 0])
    front = g[pick] / np.maximum(np.linalg.norm(g[pick], axis=1, keepdims=True), 1e-12)
    if normals is not None:
        front[(front * normals[tris[pick]].sum(1)).sum(1) < 0] *= -1
    return p, front, float(area.sum())


class Scene:
    def __init__(self, triangles):
        from embreex import rtcore_scene
        from embreex.mesh_construction import TriangleMesh
        self.scene = rtcore_scene.EmbreeScene()
        TriangleMesh(self.scene, triangles.astype(np.float32))

    def first_hit(self, origins, directions):
        r = self.scene.run(origins.astype(np.float32), directions.astype(np.float32), output=True)
        dist = np.asarray(r['tfar']).astype(np.float64)
        dist[np.asarray(r['primID']).astype(np.int64) < 0] = np.inf
        return dist


def light_reach(scene, p, front, lights):
    origins = p + front * 0.02
    reached = np.zeros(len(p), bool)
    for at, end in lights:
        v = at - origins
        dist = np.linalg.norm(v, axis=1)
        near = (dist < end) & ((v * front).sum(1) > 0)
        if near.any():
            hit = scene.first_hit(origins[near], v[near] / dist[near][:, None])
            reached[np.nonzero(near)[0][hit >= dist[near] - 0.05]] = True
    return float(reached.mean())


def placed_lights(map_id, msb_path, light_dir):
    out = []
    _, events = read_events(Path(msb_path))
    bank = read_bank(param_prefix(map_id))
    for _, row, at in events:
        v = bank.get(row)
        if v and v[5] > 0:
            out.append((np.array(at, np.float64), float(v[1])))
    synth = os.path.join(light_dir, f'{map_id}.txt')
    if os.path.exists(synth):
        with open(synth, encoding='utf-8') as f:
            for line in f:
                if line.strip() and line[0] != '#':
                    t = line.split()
                    out.append((np.array([float(t[0]), float(t[1]), float(t[2])]), float(t[4])))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=r'<steam dir>\steamapps\common'
                                      r'\Dark Souls Prepare to Die Edition\DATA\unpacked')
    ap.add_argument('--bbformats', default=r'<other project>\tools')
    ap.add_argument('--out', default=os.path.join(HERE, '..', 'assets', 'sky', 'room_meshes.txt'))
    args = ap.parse_args()
    sys.path.insert(0, args.bbformats)
    from bbformats.flver import read_flver

    sky_dir = os.path.join(HERE, '..', 'assets', 'sky')
    no_block = table_sources(os.path.join(sky_dir, 'sky_meshes.txt')) | table_sources(os.path.join(sky_dir, 'glass_meshes.txt'))

    qualifies = {}  # key -> every placement of every mesh with that key passed
    lines = {}
    for msb_path in sorted(glob.glob(os.path.join(args.data, 'map', 'MapStudio', 'm*.msb'))):
        map_id = os.path.basename(msb_path)[:-4]
        area = map_id[1:3]
        flvers = {}
        meshes = []
        for model, at, rot, scale, light_id in map_pieces(msb_path):
            path = os.path.join(args.data, 'map', map_id, f'{model}A{area}.flver')
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
                material = flver.materials[mesh.material_index]
                source = f'{map_id}/{model}A{area}/mesh{i}'
                normals = None
                if 'normal' in mesh.vertices:
                    normals = np.asarray(first(mesh.vertices['normal']), np.float64)[:, :3] @ r.T
                meshes.append(dict(source=source, light=light_id, mesh=mesh, name=diffuse_name(material), world=p @ r.T + at, tris=t, normals=normals,
                                   blocker=f'{source}/lod0' not in no_block and blocks_rays(str(material.mtd))))
        blockers = [m for m in meshes if m['blocker']]
        if not blockers:
            print(f'{map_id}: no map pieces')
            continue
        scene = Scene(np.concatenate([m['world'][m['tris']] for m in blockers]))
        lights = placed_lights(map_id, msb_path, os.path.join(HERE, '..', 'assets', 'lights'))

        regions = {}
        for m in blockers:
            # Seeded from its own name: a decision depends only on the mesh and its map.
            rng = np.random.default_rng(int.from_bytes(hashlib.sha1(m['source'].encode()).digest()[:8], 'little'))
            s = surface_samples(m['world'], m['tris'], m['normals'], RAYS, rng)
            m['escape'] = None
            if s is None:
                continue
            p, front, mesh_area = s
            dist = scene.first_hit(p + front * 0.02, cosine_hemisphere(front, rng))
            m['escape'] = float((~np.isfinite(dist)).mean())
            reach = light_reach(scene, p[:REACH_SAMPLES], front[:REACH_SAMPLES], lights)
            r = regions.setdefault(m['light'], [0.0, 0.0, 0.0])
            r[0] += mesh_area
            r[1] += m['escape'] * mesh_area
            r[2] += reach * mesh_area
        shut = {k: (r[1] / r[0], r[2] / r[0]) for k, r in regions.items() if r[0] > 0}

        passed = 0
        for m in meshes:
            esc, reach = shut.get(m['light'], (1.0, 1.0))
            ok = m['blocker'] and m.get('escape') is not None and esc <= REGION_ESCAPE_MAX and reach <= REGION_REACH_MAX and m['escape'] <= MESH_ESCAPE_MAX
            for j, fs in enumerate(m['mesh'].face_sets):
                key = (fnv1a64(np.asarray(fs.indices, dtype='<u2').tobytes()), len(fs.indices))
                qualifies[key] = qualifies.get(key, True) and ok
                if ok:
                    lines[key] = f'{key[0]:016x} {key[1]} {m["source"]}/lod{j} {m["name"]} {m["light"]} {esc:.3f} {reach:.3f} {m["escape"]:.3f}'
            passed += ok
        kept = sorted(k for k, (e, h) in shut.items() if e <= REGION_ESCAPE_MAX and h <= REGION_REACH_MAX)
        print(f'{map_id}: {passed} of {len(meshes)} meshes; regions {kept}, of {sorted(shut)}; {len(lights)} placed lights')

    kept = sorted(line for key, line in lines.items() if qualifies[key])
    with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# DS1 meshes of shut-in areas: <index FNV-1a 64> <index count> <source> <diffuse> <light id> <region escape> <region reach> <mesh escape>\n')
        f.write('# Generated by scripts/gen_room_meshes.py; do not edit by hand.\n')
        f.write('\n'.join(kept) + '\n')
    print(f'{len(kept)} face sets ({len(lines) - len(kept)} shared with a mesh that fails) -> {os.path.normpath(args.out)}')


if __name__ == '__main__':
    main()
