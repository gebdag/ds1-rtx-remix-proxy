"""Generate assets/mesh_tables/thin_wall_meshes.txt: the map meshes the proxy tags as thin walls, which are only the meshes in
CONFIRMED below, each a wall seen black in a capture and lit by its entry. The rule further down finds candidates; it
does not decide what is listed (`--candidates` prints them).

Some DS1 map models build a wall from two single-sided faces a few centimetres apart whose fronts face
each other. The game culls whichever face is seen from behind; ray traced, that face
still shadows the other one from every light, and the wall goes black. The two faces are often in different meshes of
one model: Firelink's m10_02 m0100B2A10, where mesh0 (440 triangles) lies 1-11 cm in front of mesh1's
stone wall (frame_20261101_023844). Alone, mesh0 pairs nothing and mesh1 3%; against their model, 55% and
12%. Both must be tagged: with only one, the wall stays black from the other side.

Rule (per mesh against every face of its model, lod0, fronts from the vertex normals):
a face pairs when a face of the model with an anti-parallel front (cos <= -0.95) lies 0.01-0.5 m in front
of it with overlapping bounds; a mesh qualifies when its paired faces cover >= 10% of its area and >= 1 m^2.
Face sets the game already draws without culling are skipped.

Each listed mesh contributes one line per face set (every LOD), as scripts/gen_sky_meshes.py:
  <FNV-1a 64 of the u16 index bytes, hex> <index count> <map>/<model>/mesh<i>/lod<j> <texture>

Usage: py -3.12 scripts/gen_thin_walls.py [--data <DATA\\unpacked>] [--bbformats <dir with bbformats>]
         [--map <id part>] [--out <file>] [--candidates]
  default: write the CONFIRMED meshes' face sets to the table (every CONFIRMED mesh must be found)
  --candidates: print every mesh the rule finds, marking the confirmed ones; writes nothing
"""
import os  # noqa: E402  (ds1paths: the local folders, see paths.example.ini)
import sys  # noqa: E402
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)))))
import ds1paths  # noqa: E402

import argparse
import glob
import os
import sys

import numpy as np

from gen_sky_meshes import diffuse_name, fnv1a64

GAP_MAX, GAP_MIN, FACING, MIN_SHARE, MIN_AREA = 0.5, 0.01, 0.95, 0.1, 1.0

# The only meshes the table lists: each one a wall seen black in a capture and lit by its entry, (map, model, mesh): proof.
# The rule below finds thousands of meshes on every map (3489 in 2026-11-01's table); listed unchecked, they let light
# through walls wherever anything stood within 0.5 m of them (the Undead Asylum's first corridor, frame_20261107_015039),
# and none but these had a confirmed fix (audit 2026-11-07). A new entry needs its own capture, black before and lit
# after, and a check of the captures around it. `--candidates` prints the rule's hits to look at; it writes nothing.
CONFIRMED = {
    ('m10_02_00_00', 'm0100B2A10', 0): 'Firelink tower wall, frame_20261101_023844 and 133737 (mesh0 1-11 cm in front of mesh1)',
    ('m10_02_00_00', 'm0100B2A10', 1): 'Firelink tower wall, frame_20261101_023844 and 133737 (mesh1, the stone wall)',
    ('m14_00_00_00', 'm0300B0A14', 2): 'Blighttown cave wall m14_b_wall_01, frame_20261031_171553 (two faces 0.27 m apart)',
}
CHUNK = 2048


def mesh_faces(mesh):
    """lod0's faces whose geometric normal agrees with the vertex normals: centre, front, bounds, area."""
    fs = mesh.lod0()
    if fs is None or 'normal' not in mesh.vertices:
        return None
    p = mesh.vertices['position']
    p = np.asarray(p[0] if isinstance(p, list) else p, np.float64)[:, :3]
    nr = mesh.vertices['normal']
    nr = np.asarray(nr[0] if isinstance(nr, list) else nr, np.float64)[:, :3]
    t = fs.triangles()
    t = t[(t < len(p)).all(1)]
    w = p[t]
    vn = nr[t].sum(1)
    g = np.cross(w[:, 1] - w[:, 0], w[:, 2] - w[:, 0])
    glen = np.linalg.norm(g, axis=1)
    nlen = np.linalg.norm(vn, axis=1)
    area = 0.5 * glen
    ok = (area > 1e-4) & (nlen > 1e-3)
    gn = g / np.maximum(glen, 1e-20)[:, None]
    along = (gn * vn).sum(1) / np.maximum(nlen, 1e-20)
    keep = ok & (np.abs(along) >= FACING)
    front = np.where(along[:, None] > 0, gn, -gn)[keep]
    return dict(centre=w.mean(1)[keep], front=front, lo=w.min(1)[keep], hi=w.max(1)[keep], area=area[keep],
                total=area[ok].sum(), cull=fs.cull_backfaces)


def concat(faces):
    faces = [f for f in faces if f is not None and len(f['area'])]
    if not faces:
        return None
    return {k: np.concatenate([f[k] for f in faces]) for k in ('centre', 'front', 'lo', 'hi', 'area')}


def paired_area(a, b):
    """Area of a's faces with a facing partner among b's."""
    if a is None or b is None or not len(a['area']) or not len(b['area']):
        return 0.0
    order = np.argsort(a['centre'][:, 0])  # spatially compact chunks, so the prefilter below bites
    hit = np.zeros(len(order), bool)
    for s in range(0, len(order), CHUNK):
        e = order[s:s + CHUNK]
        lo, hi = a['lo'][e], a['hi'][e]
        sel = np.all((b['lo'] <= hi.max(0) + GAP_MAX) & (lo.min(0) <= b['hi'] + GAP_MAX), axis=1)
        if not sel.any():
            continue
        bf, bc, bl, bh = b['front'][sel], b['centre'][sel], b['lo'][sel], b['hi'][sel]
        fa, ca = a['front'][e], a['centre'][e]
        m = (fa @ bf.T) <= -FACING
        if not m.any():
            continue
        gap = np.einsum('ik,ijk->ij', fa, bc[None] - ca[:, None])
        m &= (gap >= GAP_MIN) & (gap <= GAP_MAX)
        if not m.any():
            continue
        m &= np.all((lo[:, None] <= bh[None] + GAP_MAX) & (bl[None] <= hi[:, None] + GAP_MAX), axis=2)
        hit[e] = m.any(1)
    return float(a['area'][hit].sum())


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=ds1paths.GAME + r'\DATA\unpacked')
    ap.add_argument('--bbformats', default=ds1paths.SCRIPTS)
    ap.add_argument('--out', default=None, help='default assets/mesh_tables/thin_wall_meshes.txt; with --map, none unless given')
    ap.add_argument('--map', default='', help='only maps whose id contains this (for testing)')
    ap.add_argument('--candidates', action='store_true',
                    help='print every mesh the rule finds (to be checked in the game one by one); writes nothing')
    args = ap.parse_args()

    sys.path.insert(0, args.bbformats)
    from bbformats.flver import read_flver

    lines = []
    found = set()
    flagged = self_only = 0
    for map_dir in sorted(glob.glob(os.path.join(args.data, 'map', 'm*_*'))):
        map_id = os.path.basename(map_dir)
        if args.map not in map_id:
            continue
        map_count = 0
        for path in sorted(glob.glob(os.path.join(map_dir, '*.flver'))):
            flver = read_flver(open(path, 'rb').read())
            model = os.path.basename(path).rsplit('.', 1)[0]
            faces = [mesh_faces(m) for m in flver.meshes]
            whole = concat(faces)
            for i, (mesh, f) in enumerate(zip(flver.meshes, faces)):
                if f is None or f['total'] <= 0 or not f['cull']:
                    continue
                confirmed = (map_id, model, i) in CONFIRMED
                if not confirmed and not args.candidates:
                    continue
                share = paired_area(f, whole)
                # Candidates are what the rule finds; the table is what was confirmed, whatever the rule says.
                if args.candidates and (share < MIN_AREA or share < MIN_SHARE * f['total']):
                    continue
                if confirmed:
                    found.add((map_id, model, i))
                own = paired_area(f, f)
                self_only += own >= MIN_AREA and own >= MIN_SHARE * f['total']
                flagged += 1
                map_count += 1
                name = diffuse_name(flver.materials[mesh.material_index])
                print(f'{map_id}/{model}/mesh{i}: {100 * share / f["total"]:.0f}% of {f["total"]:.0f} m^2 paired '
                      f'({100 * own / f["total"]:.0f}% within itself) {name}{" [confirmed]" if confirmed else ""}', flush=True)
                for j, fs in enumerate(mesh.face_sets):
                    idx = np.asarray(fs.indices, dtype='<u2')
                    lines.append(f'{fnv1a64(idx.tobytes()):016x} {len(idx)} {map_id}/{model}/mesh{i}/lod{j} {name}')
        print(f'{map_id}: {map_count} thin-wall meshes', flush=True)

    print(f'{flagged} thin-wall meshes ({self_only} pair within themselves), {len(lines)} face sets')
    if args.candidates:
        return
    # A confirmed mesh the run did not emit (a mistyped key, a missing model, a mesh without normals or drawn uncull)
    # would leave its wall untagged without a word: stop instead.
    missing = sorted(k for k in CONFIRMED if (not args.map or args.map in k[0]) and k not in found)
    if missing:
        sys.exit(f'CONFIRMED meshes not found: {missing}; the table was not written')
    if args.out is None:
        if args.map:
            return
        args.out = os.path.join(here, '..', 'assets', 'mesh_tables', 'thin_wall_meshes.txt')
    with open(args.out, 'w', newline='\n') as f:
        f.write('# Dark Souls thin-wall map meshes (single-sided faces facing each other across a few cm), each confirmed '
                'in a capture (CONFIRMED in scripts/gen_thin_walls.py): <index FNV-1a 64> <index count> <source> <diffuse>\n')
        f.write('# Generated by scripts/gen_thin_walls.py; do not edit by hand.\n')
        f.writelines(line + '\n' for line in lines)


if __name__ == '__main__':
    main()
