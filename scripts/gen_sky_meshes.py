"""Generate assets/sky/sky_meshes.txt: the map meshes the proxy sends to Remix as sky.

Dark Souls draws its skies as ordinary map pieces. A sky model holds the dome, the far cloud layers
and the backdrop they are composited over (e.g. Firelink's m9000B2A10: a 5 km ground bowl, the cloud
sea blended over it, the dome). All of those must go into the Remix sky together; one left as world
geometry hides the layers composited over it in-game.

Rules (per FLVER in map/mNN_NN_NN_NN):
  - a "sky layer" mesh has a diffuse texture named mNN_*sky* or mNN_*cloud* (not *shadow*) and is
    at least 150 m across (smaller ones are sky patches inside buildings);
  - a model with a sky layer is a sky model; its other meshes 1 km across or more are its backdrop
    (ground bowls, distant ground, backdrop rings, far light shafts). Cliffs, trees and buildings in
    the same model are all smaller and stay world geometry;
  - in a map with a sky model, a mesh of any other model 1 km across or more whose top is above the
    top of the map's sky layers is its enclosure (Ash Lake's canopy and wall ring): seen only
    through the sky, and as world geometry it would shut the ambient sky out;
  - EXTRA_LAYERS: far blended layers checked against captures one by one (Ash Lake's mist).

Also writes assets/sky/shell_meshes.txt, same format: the blended shells (see blended_shells), which
the proxy draws as their own surfaces instead of decals.

Each listed mesh contributes one line per face set (every LOD the engine may draw):
  <FNV-1a 64 of the u16 index bytes, hex> <index count> <map>/<model>/mesh<i>/lod<j> <texture>
The engine's index buffers hold the face set indices verbatim (checked against a RenderDoc capture),
so the proxy recognises a draw by hashing the index range it draws.

Usage: py -3.12 scripts/gen_sky_meshes.py [--data <DATA\\unpacked>] [--bbformats <dir with bbformats>]
"""
import argparse
import glob
import os
import re
import sys

import numpy as np

SKY_NAME = re.compile(r'^m\d\d_.*(sky|cloud)')
# The proxy's atmospheric layers (texture_names.cpp): never decals, so never shells either.
ATMOSPHERIC_NAME = re.compile(r'^m\d\d(?=_)(?!.*shadow).*(sky|cloud|mist|fog|shaft|_bg_|back_ground)')
SHELL_ON_SURFACE = 0.05
SHELL_SAMPLES = 300
# Maps whose shells were checked against captures. Elsewhere the test also flags real decals (floor
# stains, blood, leaks, AO patches), grass cards and waterfalls: pieces placed off the map origin or
# floating a few cm over their surface. Those stay decals until a map is checked.
SHELL_MAPS = {
    'm13_02_00_00',  # Ash Lake (frame_20261027_000113, frame_20261027_094848)
}
MIN_SKY_LAYER = 150.0
MIN_BACKDROP = 1000.0

# Far layers no rule above catches, checked one by one against captures: they are blended over the
# sky and seen only far off, and as world geometry they misrender (a mist sheet ray traced as a dark
# translucent ceiling; fading trees taken for decals and dropped where nothing is behind them).
EXTRA_LAYERS = {
    # Ash Lake (frame_20261027_000113): the mist sheet ~150 m over the lake and the low mist bank.
    # (The background trees' fading upper halves, m0110B2A13 mesh1, are blended shells: in the sky
    # they met their world-geometry lower halves at a seam where the game's shading met Remix's.)
    ('m13_02_00_00', 'm0050B2A13', 0),
    ('m13_02_00_00', 'm2000B2A13', 0),
}


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def diffuse_name(material) -> str:
    for t in material.textures:
        if t.path:
            return os.path.basename(t.path.replace('\\', '/')).rsplit('.', 1)[0].lower()
    return ''


def positions(mesh):
    p = mesh.vertices['position']
    return p[0] if isinstance(p, list) else p


def extent(mesh) -> float:
    p = positions(mesh)
    return float(np.max(p.max(0) - p.min(0)))


def top(mesh) -> float:
    """Highest point (map pieces sit at the map origin, so this is the map's height)."""
    return float(positions(mesh)[:, 1].max())


def mesh_alpha(mesh):
    """Vertex colour alpha 0-1, None without vertex colours."""
    col = mesh.vertices.get('color') if hasattr(mesh.vertices, 'get') else None
    if col is None:
        return None
    c = np.asarray(col[0] if isinstance(col, list) else col, dtype=np.float64)
    return c[:, 3] if c.max() <= 1.0 else c[:, 3] / 255.0


def mesh_triangles(mesh):
    """The first face set's triangles as (n, 3, 3) positions."""
    p = np.asarray(positions(mesh), dtype=np.float64)[:, :3]
    fs = mesh.face_sets[0]
    idx = np.asarray(fs.indices, dtype=np.int64)
    if fs.triangle_strip:
        if len(idx) < 3:
            return np.zeros((0, 3, 3))
        t = np.stack([idx[:-2], idx[1:-1], idx[2:]], 1)
        t = t[(t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])]
    else:
        t = idx[: len(idx) // 3 * 3].reshape(-1, 3)
    t = t[((t >= 0) & (t < len(p))).all(1)]
    return p[t]


def point_triangles_distance(q, a, b, c):
    """Distance from point q (3,) to each triangle (a, b, c) (n, 3) (closest point, Ericson 5.1.5)."""
    ab, ac, aq = b - a, c - a, q - a
    d1, d2 = (ab * aq).sum(1), (ac * aq).sum(1)
    bq, cq = q - b, q - c
    d3, d4 = (ab * bq).sum(1), (ac * bq).sum(1)
    d5, d6 = (ab * cq).sum(1), (ac * cq).sum(1)
    va, vb, vc = d3 * d6 - d5 * d4, d5 * d2 - d1 * d6, d1 * d4 - d3 * d2

    def safe(x):
        return np.where(np.abs(x) < 1e-20, 1e-20, x)

    denom = safe(va + vb + vc)
    closest = a + ab * (vb / denom)[:, None] + ac * (vc / denom)[:, None]
    m = (va <= 0) & (d4 - d3 >= 0) & (d5 - d6 >= 0)
    closest[m] = (b + (c - b) * ((d4 - d3) / safe((d4 - d3) + (d5 - d6)))[:, None])[m]
    m = (vb <= 0) & (d2 >= 0) & (d6 <= 0)
    closest[m] = (a + ac * (d2 / safe(d2 - d6))[:, None])[m]
    m = (vc <= 0) & (d1 >= 0) & (d3 <= 0)
    closest[m] = (a + ab * (d1 / safe(d1 - d3))[:, None])[m]
    closest[(d6 >= 0) & (d5 <= d6)] = c[(d6 >= 0) & (d5 <= d6)]
    closest[(d3 >= 0) & (d4 <= d3)] = b[(d3 >= 0) & (d4 <= d3)]
    closest[(d1 <= 0) & (d2 <= 0)] = a[(d1 <= 0) & (d2 <= 0)]
    return np.linalg.norm(q - closest, axis=1)


def blended_shells(map_id, flvers, in_sky):
    """(model, mesh index, mesh, diffuse, fraction on a surface) of the map's blended shells.

    The game draws straight-alpha map meshes (MTD *_Alp) in two roles. Most are edge-fade strips lying
    on an opaque surface (bark over bark where two tree parts meet, a floor blend), which the proxy
    tags as decals for Remix to blend onto that surface. A shell lies off any surface: a giant tree's
    upper half fading into the mist, a sleeve standing 5-50 cm off a trunk. As a decal a shell took
    the lighting of whatever was behind it, or vanished where only sky was. A mesh is a shell when
    fewer than half of its visible vertices (alpha > 0.25, up to SHELL_SAMPLES) lie within
    SHELL_ON_SURFACE of an opaque triangle of the map (all map pieces at the map origin).
    """
    opaque, blended = [], []
    for model, flver, names, sizes, layers in flvers:
        for i, mesh in enumerate(flver.meshes):
            mtd = os.path.basename(flver.materials[mesh.material_index].mtd).lower()
            if '_alp' in mtd:
                if (map_id, model, i) not in in_sky and not ATMOSPHERIC_NAME.search(names[i]):
                    blended.append((model, i, mesh, names[i]))
            elif '_add' not in mtd:
                opaque.append(mesh_triangles(mesh))
    if not blended or not opaque:
        return []
    tris = np.concatenate(opaque)
    lo, hi = tris.min(1), tris.max(1)
    shells = []
    rng = np.random.default_rng(0)
    for model, i, mesh, name in blended:
        p = np.asarray(positions(mesh), dtype=np.float64)[:, :3]
        a = mesh_alpha(mesh)
        visible = p[a > 0.25] if a is not None else p
        if len(visible) == 0:
            continue
        sample = visible[rng.choice(len(visible), min(SHELL_SAMPLES, len(visible)), replace=False)]
        near = ((hi >= sample.min(0) - SHELL_ON_SURFACE) & (lo <= sample.max(0) + SHELL_ON_SURFACE)).all(1)
        t = tris[near]
        on = 0
        for q in sample:
            if len(t) and point_triangles_distance(q, t[:, 0], t[:, 1], t[:, 2]).min() < SHELL_ON_SURFACE:
                on += 1
        on_surface = on / len(sample)
        if on_surface < 0.5:
            shells.append((model, i, mesh, name, on_surface))
    return shells


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=r'<steam dir>\steamapps\common'
                                      r'\Dark Souls Prepare to Die Edition\DATA\unpacked')
    ap.add_argument('--bbformats', default=r'<other project>\tools')
    ap.add_argument('--out', default=os.path.join(here, '..', 'assets', 'sky', 'sky_meshes.txt'))
    args = ap.parse_args()

    sys.path.insert(0, args.bbformats)
    from bbformats.flver import read_flver

    lines = []
    shell_lines = []
    models = 0
    enclosures = 0
    in_sky = set()

    def add(map_id, model, i, mesh, name, out=lines):
        if out is lines:
            in_sky.add((map_id, model, i))
        for j, fs in enumerate(mesh.face_sets):
            idx = np.asarray(fs.indices, dtype='<u2')
            out.append(f'{fnv1a64(idx.tobytes()):016x} {len(idx)} {map_id}/{model}/mesh{i}/lod{j} {name}')

    for map_dir in sorted(glob.glob(os.path.join(args.data, 'map', 'm*_*'))):
        map_id = os.path.basename(map_dir)
        flvers = []
        for path in sorted(glob.glob(os.path.join(map_dir, '*.flver'))):
            flver = read_flver(open(path, 'rb').read())
            names = [diffuse_name(flver.materials[m.material_index]) for m in flver.meshes]
            sizes = [extent(m) for m in flver.meshes]
            layers = {i for i, n in enumerate(names)
                      if SKY_NAME.search(n) and 'shadow' not in n and sizes[i] >= MIN_SKY_LAYER}
            flvers.append((os.path.basename(path).rsplit('.', 1)[0], flver, names, sizes, layers))

        sky_top = None
        for model, flver, names, sizes, layers in flvers:
            if not layers:
                continue
            models += 1
            for i, mesh in enumerate(flver.meshes):
                if i in layers:
                    sky_top = max(sky_top if sky_top is not None else -np.inf, top(mesh))
                if i in layers or sizes[i] >= MIN_BACKDROP:
                    add(map_id, model, i, mesh, names[i])

        # The map's enclosure: meshes of other models a kilometre or more across that rise above the
        # top of its sky layers can only be seen through them (Ash Lake's tree canopy and wall ring,
        # 270-720 m over the lake and hidden behind its mist and sky). As world geometry they seal
        # the area in, so no bounce ray reaches the game's ambient sky and the map goes black.
        for model, flver, names, sizes, layers in flvers if sky_top is not None else []:
            if layers:
                continue
            for i, mesh in enumerate(flver.meshes):
                if (map_id, model, i) in EXTRA_LAYERS:
                    print(f'listed layer {map_id}/{model}/mesh{i} {names[i]}')
                    add(map_id, model, i, mesh, names[i])
                elif sizes[i] >= MIN_BACKDROP and top(mesh) > sky_top:
                    enclosures += 1
                    print(f'enclosure {map_id}/{model}/mesh{i} {names[i]}: {sizes[i]:.0f} m across, '
                          f'top {top(mesh):.0f} > sky top {sky_top:.0f}')
                    add(map_id, model, i, mesh, names[i])

        # Blended shells: straight-alpha meshes that do not lie on an opaque surface of the map.
        for model, i, mesh, name, on_surface in blended_shells(map_id, flvers, in_sky) if map_id in SHELL_MAPS else []:
            print(f'shell {map_id}/{model}/mesh{i} {name}: {on_surface:.0%} on a surface')
            add(map_id, model, i, mesh, name, shell_lines)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# Dark Souls sky model meshes: <index FNV-1a 64> <index count> <source> <diffuse>\n')
        f.write('# Generated by scripts/gen_sky_meshes.py; do not edit by hand.\n')
        f.writelines(line + '\n' for line in lines)
    print(f'{models} sky models, {enclosures} enclosure meshes, {len(lines)} face sets -> {os.path.abspath(args.out)}')

    shells_out = os.path.join(os.path.dirname(os.path.abspath(args.out)), 'shell_meshes.txt')
    with open(shells_out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# Dark Souls blended shells (alpha-faded meshes off any surface): '
                '<index FNV-1a 64> <index count> <source> <diffuse>\n')
        f.write('# Generated by scripts/gen_sky_meshes.py; do not edit by hand.\n')
        f.writelines(line + '\n' for line in shell_lines)
    print(f'{len(shell_lines)} blended shell face sets -> {shells_out}')


if __name__ == '__main__':
    main()
