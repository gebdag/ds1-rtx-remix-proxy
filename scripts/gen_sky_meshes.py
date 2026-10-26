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
  - EXTRA_LAYERS: far blended layers checked against captures one by one (Ash Lake's mist and its
    fading background trees).

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
MIN_SKY_LAYER = 150.0
MIN_BACKDROP = 1000.0

# Far layers no rule above catches, checked one by one against captures: they are blended over the
# sky and seen only far off, and as world geometry they misrender (a mist sheet ray traced as a dark
# translucent ceiling; fading trees taken for decals and dropped where nothing is behind them).
EXTRA_LAYERS = {
    # Ash Lake (frame_20261027_000113): the mist sheet ~150 m over the lake, the low mist bank, and
    # the background giant trees that fade into the mist (vertex alpha mean 0.39, never nearer than
    # 394 m from the beach).
    ('m13_02_00_00', 'm0050B2A13', 0),
    ('m13_02_00_00', 'm2000B2A13', 0),
    ('m13_02_00_00', 'm0110B2A13', 1),
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
    models = 0
    enclosures = 0

    def add(map_id, model, i, mesh, name):
        for j, fs in enumerate(mesh.face_sets):
            idx = np.asarray(fs.indices, dtype='<u2')
            lines.append(f'{fnv1a64(idx.tobytes()):016x} {len(idx)} {map_id}/{model}/mesh{i}/lod{j} {name}')

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
        if sky_top is None:
            continue
        for model, flver, names, sizes, layers in flvers:
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

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# Dark Souls sky model meshes: <index FNV-1a 64> <index count> <source> <diffuse>\n')
        f.write('# Generated by scripts/gen_sky_meshes.py; do not edit by hand.\n')
        f.writelines(line + '\n' for line in lines)
    print(f'{models} sky models, {enclosures} enclosure meshes, {len(lines)} face sets -> {os.path.abspath(args.out)}')


if __name__ == '__main__':
    main()
