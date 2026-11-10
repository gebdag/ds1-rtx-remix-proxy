"""Generate assets/lights/m17_00_00_00.custom.txt: Duke's Archives, the few large lights its rooms need.

The game bakes this area. Its lanterns are emissive meshes with no point light; their lights come from the meshes at runtime
(assets/lights/mesh_lights.txt, game/mesh_lights in the proxy). The rest of the area's level has no source in the game: a soft even
light, measured cool in the frames around the halls (red 0.66, blue 1.5 against green). One fill per room gives it.

Needs embreex (pip install embreex).

Usage: py -3.12 scripts/gen_archives_lights.py [--fill S] [--deploy]
"""
import argparse
import os
import shutil
import sys

import numpy as np

from map_geometry import Scene, blocks_rays, first, map_pieces, rotation, table_sources

HERE = os.path.dirname(os.path.abspath(__file__))
MAP = 'm17_00_00_00'
# name, position, radius, radiance, spot cone (0: a plain sphere). The game's floors are lit and its walls and vaults dark but for the
# lanterns, so every fill is a spot down onto the floor and lower walls. The upper tiers are left to their lanterns.
# The pillar between the halls, at (302, 547), is near black in the game and has a shiny material: each low spot stands on the far side
# of its hall, with the pillar outside its cone even at the soft edge.
ROOMS = [('Archives_tower_wall_n', (405.0, 250.0, 565.5), 2.0, 15.0, 60.0),
         ('Archives_tower_wall_e', (405.5, 250.0, 565.0), 2.0, 15.0, 60.0),
         ('Archives_tower_wall_s', (405.0, 250.0, 564.5), 2.0, 15.0, 60.0),
         ('Archives_tower_wall_w', (404.5, 250.0, 565.0), 2.0, 15.0, 60.0),
         ('Archives_gears', (382.0, 237.0, 541.0), 1.5, 2000.0, 20.0),
         ('Archives_elevator_room', (236.0, 211.0, 481.0), 2.5, 3.0, 60.0),
         ('Archives_stair_hall', (260.0, 283.0, 505.0), 2.0, 4.0, 50.0),
         ('Archives_hall_1', (278.5, 340.0, 523.5), 9.0, 85.0, 35.0),
         ('Archives_hall_2', (335.5, 340.0, 579.5), 9.0, 85.0, 35.0),
         # The high spots cannot reach under the balconies.
         ('Archives_hall_1_low', (278.0, 291.0, 523.0), 6.0, 3.0, 60.0),
         ('Archives_hall_2_low', (331.0, 291.0, 575.0), 6.0, 3.0, 60.0),
         # The rooms above. Overhead and dim: the game's reflections hold bright spots overhead (its cube for shiny materials) and these
         # stand for them, so tiles and crystal catch highlights from them as they do in the game.
         ('Archives_elevator_top', (304.0, 366.5, 549.0), 1.2, 1.6, 0.0),
         ('Archives_chest_room_1', (280.0, 410.0, 525.0), 2.0, 32.0, 0.0),
         ('Archives_chest_room_2', (300.0, 410.0, 535.0), 2.0, 32.0, 0.0),
         ('Archives_chest_room_3', (290.0, 410.0, 552.0), 2.0, 32.0, 0.0),
         # The narrow stairs between them, which no room light reaches.
         ('Archives_upper_stair', (312.5, 367.6, 519.5), 0.4, 6.0, 0.0),
         ('Archives_top_stair', (271.0, 393.5, 515.0), 0.4, 2.5, 0.0),
         ('Archives_stair_landing', (313.0, 368.0, 527.5), 0.5, 8.0, 0.0),
         # The jail tower: the dome's pale light down the shaft, a spot so the dome itself stays out of it, and a low one for the floors its
         # galleries shade. Blue as the area's ambient: the book walls are yellow, and any paler light turns them olive.
         ('Archives_jail_cells', (376.0, 249.0, 533.0), 1.0, 5.0, 0.0),
         ('Archives_tower_foot', (387.5, 236.5, 549.5), 2.5, 1.2, 0.0),
         # The hidden library below the halls.
         ('Archives_ladder_shaft', (317.0, 297.0, 595.0), 1.5, 6.0, 0.0),
         ('Archives_library_2', (305.0, 295.0, 590.0), 3.0, 10.0, 0.0),
         ('Archives_library_3', (303.0, 300.0, 610.0), 3.0, 10.0, 0.0),
         ('Archives_library_low', (301.0, 284.0, 616.0), 3.0, 1.5, 0.0),
         ('Archives_library_under', (301.4, 276.0, 613.5), 1.2, 2.5, 0.0),
         # The Crystal Cave, high up: its facets need something overhead to catch, as the rooms above do.
         ('Archives_cave_1', (104.0, 165.0, 857.0), 3.0, 6.0, 0.0),
         ('Archives_cave_2', (148.0, 165.0, 810.0), 3.0, 6.0, 0.0)]
FILL_COLOUR = '130 180 255'
# The rooms above are lit a paler blue than the halls (PS c98 0.84 0.86 1.0 there).
ROOM_COLOUR = {'Archives_elevator_top': '215 220 255', 'Archives_chest_room_1': '215 220 255',
               'Archives_chest_room_2': '215 220 255', 'Archives_chest_room_3': '215 220 255',
               'Archives_upper_stair': '215 220 255', 'Archives_top_stair': '215 220 255',
               'Archives_stair_landing': '215 220 255',
               # The hue of the cave's own probe (as gen_emission.py).
               'Archives_cave_1': '125 196 255', 'Archives_cave_2': '125 196 255',
               'Archives_tower_dome': '130 180 255', 'Archives_tower_low': '130 180 255', 'Archives_jail_cells': '215 220 255',
               'Archives_tower_foot': '215 220 255', 'Archives_ladder_shaft': '225 225 240', 'Archives_library_2': '225 225 240',
               'Archives_library_3': '225 225 240', 'Archives_library_low': '225 225 240', 'Archives_library_under': '225 225 240',
               'Archives_gears': '255 250 240', 'Archives_tower_wall_n': '255 225 180', 'Archives_tower_wall_e': '255 225 180', 'Archives_tower_wall_s': '255 225 180', 'Archives_tower_wall_w': '255 225 180'}
# A spot that does not point straight down: name -> direction. The gears at the tower's foot, (404, 231, 565), lit from high on the far
# wall. The game has them in a pool of light; from straight above that pool would catch the player as well.
ROOM_DIRECTION = {'Archives_tower_wall_n': (0.0, 0.0, 1.0), 'Archives_tower_wall_e': (1.0, 0.0, 0.0), 'Archives_tower_wall_s': (0.0, 0.0, -1.0), 'Archives_tower_wall_w': (-1.0, 0.0, 0.0),
                  'Archives_gears': (0.693, -0.17, 0.701)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=r'<steam dir>\steamapps\common'
                                      r'\Dark Souls Prepare to Die Edition\DATA\unpacked')
    ap.add_argument('--bbformats', default=r'<other project>\tools')
    ap.add_argument('--fill', type=float, default=1.0, help='scales the room lights')
    ap.add_argument('--deploy', action='store_true', help='also into the game\'s rtx_comp\\lights')
    args = ap.parse_args()
    sys.path.insert(0, args.bbformats)
    from bbformats.flver import read_flver

    tables_dir = os.path.join(HERE, '..', 'assets', 'mesh_tables')
    no_block = table_sources(os.path.join(tables_dir, 'sky_meshes.txt')) | table_sources(os.path.join(tables_dir, 'glass_meshes.txt'))
    area = MAP[1:3]
    flvers, tris = {}, []
    for model, at, rot, scale, _light in map_pieces(os.path.join(args.data, 'map', 'MapStudio', f'{MAP}.msb')):
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
            world = p @ r.T + at
            material = flver.materials[mesh.material_index]
            if f'{MAP}/{model}A{area}/mesh{i}/lod0' in no_block or not blocks_rays(str(material.mtd)):
                continue
            tris.append(world[t])
    scene = Scene(np.concatenate(tris))

    around = np.array([[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]], float)
    for name, at, radius, _radiance, _cone in ROOMS:
        d = scene.first_hit(np.tile(np.array(at), (6, 1)), around)
        print(f'{name} {at}: radius {radius}, surfaces at +x -x +y -y +z -z {np.round(np.where(np.isfinite(d), d, -1), 1).tolist()}')

    lines = [f'# Custom lights for {MAP} (Duke\'s Archives). F4 > Lights > Custom lights edits and saves this file.',
             '# x y z  dwindleBegin dwindleEnd  r g b  intensityPct  name  [remix intensity exposure radius volumetric]',
             '#   [spot dx dy dz coneDeg softness]  [focus exponent]  [off]',
             "# With 'remix' a plain Remix sphere light; without, the game's falloff (dwindle begin/end, intensityPct).",
             '#',
             '# Generated by scripts/gen_archives_lights.py. See there. Saving from the menu keeps your edits until the script is run again.']
    for name, at, radius, radiance, cone in ROOMS:
        direction = ROOM_DIRECTION.get(name, (0.0, -1.0, 0.0))
        shape = f' spot {direction[0]:.4f} {direction[1]:.4f} {direction[2]:.4f} {cone:.1f} 0.40' if cone else ''
        lines.append(f'{at[0]:.1f} {at[1]:.1f} {at[2]:.1f} 8 12 {ROOM_COLOUR.get(name, FILL_COLOUR)} 600 {name} remix {args.fill * radiance:g} 0.00 {radius:.3f} 1.00{shape}')
    out = os.path.join(HERE, '..', 'assets', 'lights', f'{MAP}.custom.txt')
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    if args.deploy:
        shutil.copy2(out, os.path.join(os.path.dirname(args.data), 'rtx_comp', 'lights', f'{MAP}.custom.txt'))
    print('wrote', os.path.normpath(out), len(lines) - 6, 'lights')


if __name__ == '__main__':
    main()
