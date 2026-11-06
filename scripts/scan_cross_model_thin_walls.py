"""Find thin walls whose two faces are in different map pieces: candidates for CROSS_MODEL in gen_thin_walls.py.

gen_thin_walls.py pairs faces within one model. This places every map piece of every map in world space from
its MSB (map_geometry.map_pieces) and applies the same rule across placements: a face pairs when a face with an
anti-parallel front (cos <= -0.95) lies 0.01-0.5 m in front of it with overlapping bounds; a mesh qualifies when
its paired faces (within its own model or against any other placed piece) cover >= 10% of its area and >= 1 m^2,
and it is listed when it is not in thin_wall_meshes.txt already, i.e. when the cross-model pairs made it qualify.

The table is per mesh, not per placement: a model placed several times may pair in only some placements, so the
output counts the placements that qualify.

Spatial pruning: faces sorted along a Morton curve, cut into chunks with bounds (a one-level BVH); a chunk is
tested only against the chunks whose expanded bounds overlap it, and the facing test runs before the others.

Usage: py -3.12 scripts/scan_cross_model_thin_walls.py [--map m18_01] [--out <dir>]
"""
import argparse
import csv
import glob
import os
import sys
import time
from collections import defaultdict

import numpy as np

from gen_sky_meshes import diffuse_name
from gen_thin_walls import CROSS_MODEL, FACING, GAP_MAX, GAP_MIN, MIN_AREA, MIN_SHARE
from map_geometry import blocks_rays, first, map_pieces, rotation, table_sources

CHUNK = 128
DUP_GAP = 0.01
DUP_FLAG = 0.1
COVIS = True  # pair only pieces that can be drawn in one frame (draw groups)
NL, TAB = chr(10), chr(9)
BINS = 96


def fibonacci_sphere(k):
    i = np.arange(k) + 0.5
    z = 1 - 2 * i / k
    r = np.sqrt(1 - z * z)
    phi = np.pi * (1 + 5 ** 0.5) * i
    return np.stack([r * np.cos(phi), r * np.sin(phi), z], 1)


def world_faces(mesh, r, at):
    """lod0's faces in world space whose geometric normal agrees with the vertex normals (gen_thin_walls.mesh_faces)."""
    fs = mesh.lod0()
    if fs is None or 'normal' not in mesh.vertices or 'position' not in mesh.vertices:
        return None
    p = np.asarray(first(mesh.vertices['position']), np.float64)[:, :3] @ r.T + at
    nr = np.asarray(first(mesh.vertices['normal']), np.float64)[:, :3] @ np.linalg.inv(r)
    t = fs.triangles()
    t = t[(t < len(p)).all(1)]
    if not len(t):
        return None
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
    return dict(centre=w.mean(1)[keep], front=front, lo=w.min(1)[keep], hi=w.max(1)[keep], area=area[keep], tri=w[keep],
                total=float(area[ok].sum()), cull=bool(fs.cull_backfaces))


def draw_groups(msb_path):
    """Per map piece (map_pieces' order) its draw groups, and every collision part's display groups (MSB1 part
    header: draw groups at 0x38, display groups at 0x48, 4 x u32 each; collision = part type 5)."""
    import struct
    from map_geometry import PART_MAP_PIECE, _section
    data = open(msb_path, 'rb').read()
    model_offsets, pos = _section(data, 0)
    _, pos = _section(data, pos)
    _, pos = _section(data, pos)
    part_offsets, _ = _section(data, pos)
    pieces, cols = [], []
    for o in part_offsets:
        _, kind, _, model = struct.unpack_from('<iIii', data, o)
        draw = sum(v << (32 * k) for k, v in enumerate(struct.unpack_from('<4I', data, o + 0x38)))
        disp = sum(v << (32 * k) for k, v in enumerate(struct.unpack_from('<4I', data, o + 0x48)))
        if kind == PART_MAP_PIECE and 0 <= model < len(model_offsets):
            pieces.append(draw)
        elif kind == 5:
            cols.append(disp)
    return pieces, sorted(set(cols))


def covisible(draws, cols):
    """[i, j]: some collision's display groups meet both pieces' draw groups, so both can be drawn in one frame."""
    bits = np.array([[bool(c & d) for c in cols] for d in draws], bool)
    return (bits.astype(np.int32) @ bits.T.astype(np.int32)) > 0


def morton(c):
    q = ((c - c.min(0)) / max(float((c.max(0) - c.min(0)).max()), 1e-6) * 1023).astype(np.uint64)

    def spread(v):
        v = (v | (v << np.uint64(16))) & np.uint64(0x030000FF)
        v = (v | (v << np.uint64(8))) & np.uint64(0x0300F00F)
        v = (v | (v << np.uint64(4))) & np.uint64(0x030C30C3)
        v = (v | (v << np.uint64(2))) & np.uint64(0x09249249)
        return v
    return spread(q[:, 0]) | (spread(q[:, 1]) << np.uint64(1)) | (spread(q[:, 2]) << np.uint64(2))


def scan_map(map_id, data_dir, read_flver, flver_cache):
    msb = os.path.join(data_dir, 'map', 'MapStudio', f'{map_id}.msb')
    area = map_id[1:3]
    folders = [map_id] + sorted(os.path.basename(d) for d in glob.glob(os.path.join(data_dir, 'map', f'm{area}_*'))
                                if os.path.basename(d) != map_id)
    pieces = map_pieces(msb)
    draws, cols = draw_groups(msb)
    assert len(draws) == len(pieces)
    covis = covisible(draws, cols) if cols else np.ones((len(pieces), len(pieces)), bool)
    np.fill_diagonal(covis, True)
    inst = []  # (flver folder, model, mesh index, placement index, total, cull, material)
    arrays = defaultdict(list)
    missing = set()
    for pi, (model, at, rot, scale, _light) in enumerate(pieces):
        name = f'{model}A{area}'
        folder = next((f for f in folders if os.path.exists(os.path.join(data_dir, 'map', f, name + '.flver'))), None)
        if folder is None:
            missing.add(model)
            continue
        key = (folder, name)
        if key not in flver_cache:
            flver_cache[key] = read_flver(open(os.path.join(data_dir, 'map', folder, name + '.flver'), 'rb').read())
        flver = flver_cache[key]
        r = rotation(rot) * scale
        for mi, mesh in enumerate(flver.meshes):
            f = world_faces(mesh, r, at)
            if f is None or not len(f['area']):
                continue
            ii = len(inst)
            inst.append((folder, name, mi, pi, f['total'], f['cull'], flver.materials[mesh.material_index]))
            for k in ('centre', 'front', 'lo', 'hi', 'area'):
                arrays[k].append(f[k])
            arrays['inst'].append(np.full(len(f['area']), ii, np.int32))
    if not inst:
        return [], 0, missing, 0
    A = {k: np.concatenate(v) for k, v in arrays.items()}
    # Direction bins: a face meets only faces of the bins whose centres can be anti-parallel to its own within FACING.
    dirs = fibonacci_sphere(BINS)
    fbin = np.concatenate([np.argmax(A['front'][s:s + 65536] @ dirs.T, axis=1) for s in range(0, len(A['area']), 65536)])
    cosb = np.clip((A['front'] * dirs[fbin]).sum(1), -1, 1)
    rad = np.zeros(BINS)
    np.maximum.at(rad, fbin, np.arccos(cosb))
    reach = np.cos(np.minimum(np.arccos(FACING) + rad[:, None] + rad[None] + 1e-6, np.pi))
    compat = (dirs @ -dirs.T) >= reach
    same = (dirs @ dirs.T) >= reach  # for the coplanar-duplicate check
    order = np.lexsort((morton(A['centre']), fbin))
    A = {k: v[order] for k, v in A.items()}
    fbin = fbin[order]
    part = np.array([i[3] for i in inst], np.int32)[A['inst']]
    n = len(A['area'])
    edges = np.r_[0, np.nonzero(np.diff(fbin))[0] + 1, n]
    starts = np.concatenate([np.arange(edges[k], edges[k + 1], CHUNK) for k in range(len(edges) - 1)])
    ends = np.r_[starts[1:], n]
    cbin = fbin[starts]
    clo = np.minimum.reduceat(A['lo'], starts, axis=0)
    chi = np.maximum.reduceat(A['hi'], starts, axis=0)
    target = np.array([i[5] for i in inst], bool)[A['inst']]  # culled face sets only
    # cross pairs only between ray-blocking materials (map_geometry.blocks_rays: no alpha, additive, water, edge, NtoA)
    blk = np.array([blocks_rays(str(i[6].mtd)) for i in inst], bool)[A['inst']]

    hit_all = np.zeros(n, bool)
    hit_x = np.zeros(n, bool)
    hit_w = np.zeros(n, bool)
    nvis = 0
    dup_part = np.full(n, -1, np.int64)
    partner = np.full(n, -1, np.int64)
    gap_x = np.full(n, np.nan)
    for ci, (s, t) in enumerate(zip(starts, ends)):
        if not target[s:t].any():
            continue
        near = np.nonzero((compat[cbin[ci], cbin] | same[cbin[ci], cbin]) & np.all((clo <= chi[ci] + GAP_MAX) & (clo[ci] <= chi + GAP_MAX), axis=1))[0]
        if not len(near):
            continue
        bidx = np.concatenate([np.arange(starts[j], ends[j]) for j in near])
        # per face against the chunk's bounds, then per pair: facing and bounds as one matrix, then the gap
        bidx = bidx[np.all((A['lo'][bidx] <= chi[ci] + GAP_MAX) & (clo[ci] <= A['hi'][bidx] + GAP_MAX), axis=1)]
        if not len(bidx):
            continue
        fa, ca, la, ha = A['front'][s:t], A['centre'][s:t], A['lo'][s:t], A['hi'][s:t]
        bl, bh = A['lo'][bidx], A['hi'][bidx]
        dots = fa @ A['front'][bidx].T
        m = dots <= -FACING
        d = dots >= FACING
        for k in range(3):
            m &= (la[:, k, None] <= bh[None, :, k] + GAP_MAX) & (bl[None, :, k] <= ha[:, k, None] + GAP_MAX)
            d &= (la[:, k, None] <= bh[None, :, k] + DUP_GAP) & (bl[None, :, k] <= ha[:, k, None] + DUP_GAP)
        # coplanar same-facing faces of another piece: the piece is a duplicate (an alternative state or a LOD shell)
        da, db = np.nonzero(d)
        if len(da):
            db = bidx[db]
            dg = np.abs(np.einsum('ik,ik->i', fa[da], A['centre'][db] - ca[da]))
            k = (dg <= DUP_GAP) & (part[da + s] != part[db])
            dup_part[da[k] + s] = part[db[k]]
        ia, jb = np.nonzero(m)
        if not len(ia):
            continue
        jb = bidx[jb]
        gap = np.einsum('ik,ik->i', fa[ia], A['centre'][jb] - ca[ia])
        ok = (gap >= GAP_MIN) & (gap <= GAP_MAX)
        nvis += int((ok & ~covis[part[ia + s], part[jb]]).sum())
        ok &= covis[part[ia + s], part[jb]] | (not COVIS)
        ia, jb, gap = ia[ok], jb[ok], gap[ok]
        if not len(ia):
            continue
        ga = ia + s
        hit_all[ga] = True
        x = part[ga] != part[jb]
        hit_w[ga[~x]] = True
        hit_all[ga] = False
        hit_all[ga[~x]] = True
        x &= blk[ga] & blk[jb]
        hit_all[ga[x]] = True
        ga, jb, gap = ga[x], jb[x], gap[x]
        if not len(ga):
            continue
        hit_x[ga] = True
        o = np.lexsort((gap, ga))  # per face, its nearest cross-placement partner
        ga, jb, gap = ga[o], jb[o], gap[o]
        firsts = np.r_[True, ga[1:] != ga[:-1]]
        partner[ga[firsts]] = jb[firsts]
        gap_x[ga[firsts]] = gap[firsts]
    hit_all &= target
    hit_x &= target
    hit_w &= target

    ninst = len(inst)
    pa_all = np.bincount(A['inst'], weights=A['area'] * hit_all, minlength=ninst)
    pa_x = np.bincount(A['inst'], weights=A['area'] * hit_x, minlength=ninst)
    pa_w = np.bincount(A['inst'], weights=A['area'] * hit_w, minlength=ninst)
    hx = np.nonzero(hit_x)[0]
    pair_a, pair_b = A['inst'][hx].astype(np.int64), A['inst'][partner[hx]].astype(np.int64)
    key = pair_a * ninst + pair_b
    uk, inv = np.unique(key, return_inverse=True)
    karea = np.bincount(inv, weights=A['area'][hx])
    o = np.argsort(inv, kind='stable')
    gs = gap_x[hx][o]
    cut = np.r_[0, np.cumsum(np.bincount(inv, minlength=len(uk)))]
    kgap = np.array([np.median(gs[cut[k]:cut[k + 1]]) for k in range(len(uk))])
    pairs = defaultdict(list)
    for k, ar, gp in zip(uk, karea, kgap):
        pairs[int(k // ninst)].append((int(k % ninst), float(ar), float(gp)))
    rows = []
    for ii, plist in pairs.items():
        folder, name, mi, pi, total, cull, mat = inst[ii]
        best, barea, bgap = max(plist, key=lambda p: p[1])
        pf, pn, pm, ppi, *_ = inst[best]
        rows.append(dict(map=map_id, folder=folder, model=name, mesh=mi, placement=pi, total=total,
                         paired=float(pa_all[ii]), cross=float(pa_x[ii]), within=float(pa_w[ii]), diffuse=diffuse_name(mat), mtd=str(mat.mtd),
                         partner=f'{pn}/mesh{pm}', partner_key=(pf, pn, pm), partner_placement=ppi,
                         partner_diffuse=diffuse_name(inst[best][6]), partner_mtd=str(inst[best][6].mtd),
                         partner_area=barea, gap=bgap, n_partners=len(plist),
                         dup=float(A['area'][(A['inst'] == ii) & (dup_part >= 0)].sum()),
                         dup_partner=float(A['area'][(A['inst'] == ii) & (dup_part == ppi)].sum()),
                         pairs={(inst[b][0], inst[b][1], inst[b][2], inst[b][3]): ar for b, ar, _ in plist},
                         qualifies=bool(pa_all[ii] >= MIN_AREA and pa_all[ii] >= MIN_SHARE * total)))
    placements = defaultdict(int)
    for folder, name, mi, pi, *_ in inst:
        placements[(folder, name, mi)] += 1
    for r in rows:
        r['placements'] = placements[(r['folder'], r['model'], r['mesh'])]
    return rows, n, missing, nvis


def coverage(map_id, keys, data_dir, read_flver, rng):
    """Second stage, for listed mesh placements: the share of the mesh's area whose NEAREST ray-blocking surface
    0.01-0.5 m in front is an anti-parallel face of another piece (sampled, rays along the face fronts).

    The face rule counts a whole face as paired when any partner face meets it, so a 20 m terrain triangle under one
    rock counts in full; this measures how much of the mesh a partner actually covers.
    keys: {(folder, model, mesh, placement)}; returns {key: (covered cross area, covered any-piece area, samples)}."""
    from embreex import rtcore_scene
    from embreex.mesh_construction import TriangleMesh
    msb = os.path.join(data_dir, 'map', 'MapStudio', f'{map_id}.msb')
    area = map_id[1:3]
    folders = [map_id] + sorted(os.path.basename(d) for d in glob.glob(os.path.join(data_dir, 'map', f'm{area}_*'))
                                if os.path.basename(d) != map_id)
    pieces = map_pieces(msb)
    draws, cols = draw_groups(msb)
    covis = covisible(draws, cols) if cols else np.ones((len(pieces), len(pieces)), bool)
    np.fill_diagonal(covis, True)
    cache = {}
    tris, fronts, parts, own = [], [], [], {}
    for pi, (model, at, rot, scale, _light) in enumerate(pieces):
        name = f'{model}A{area}'
        folder = next((f for f in folders if os.path.exists(os.path.join(data_dir, 'map', f, name + '.flver'))), None)
        if folder is None:
            continue
        if (folder, name) not in cache:
            cache[(folder, name)] = read_flver(open(os.path.join(data_dir, 'map', folder, name + '.flver'), 'rb').read())
        flver = cache[(folder, name)]
        r = rotation(rot) * scale
        for mi, mesh in enumerate(flver.meshes):
            f = world_faces(mesh, r, at)
            if f is None or not len(f['area']):
                continue
            if (folder, name, mi, pi) in keys:
                own[(folder, name, mi, pi)] = f
            if not blocks_rays(str(flver.materials[mesh.material_index].mtd)):
                continue
            tris.append(f['tri'])
            fronts.append(f['front'])
            parts.append(np.full(len(f['area']), pi, np.int32))
    T, F, P = np.concatenate(tris), np.concatenate(fronts), np.concatenate(parts)
    lo, hi = T.min(1), T.max(1)
    out = {}
    for key, f in own.items():
        pi = key[3]
        blo, bhi = f['lo'].min(0) - GAP_MAX, f['hi'].max(0) + GAP_MAX
        sel = np.nonzero(np.all((lo <= bhi) & (hi >= blo), axis=1) & covis[pi, P])[0]
        if not len(sel):
            out[key] = (0.0, 0.0, 0)
            continue
        count = int(np.clip(32 * f['area'].sum(), 4000, 400000))
        pick = rng.choice(len(f['area']), count, p=f['area'] / f['area'].sum())
        u, v = rng.random(count), rng.random(count)
        flip = u + v > 1
        u[flip], v[flip] = 1 - u[flip], 1 - v[flip]
        w = f['tri'][pick]
        p = w[:, 0] + u[:, None] * (w[:, 1] - w[:, 0]) + v[:, None] * (w[:, 2] - w[:, 0])
        d = f['front'][pick]
        scene = rtcore_scene.EmbreeScene()
        TriangleMesh(scene, T[sel].astype(np.float32))
        res = scene.run((p + d * GAP_MIN).astype(np.float32), d.astype(np.float32), output=True)
        prim = np.asarray(res['primID']).astype(np.int64)
        dist = np.asarray(res['tfar']).astype(np.float64)
        hit = (prim >= 0) & (dist <= GAP_MAX - GAP_MIN)
        g = sel[np.where(hit, prim, 0)]
        facing = hit & ((F[g] * d).sum(1) <= -FACING)
        cross = facing & (P[g] != pi)
        a = f['area'].sum() / count
        out[key] = (float(cross.sum() * a), float(facing.sum() * a), count)
    return out


def verify(args, read_flver):
    """Adds sampled covered areas to the listed meshes in candidates.tsv and rewrites cross_model.py from them."""
    path = os.path.join(args.out, 'candidates.tsv')
    rows = list(csv.DictReader(open(path), delimiter=TAB))
    listed = [r for r in rows if r['in_table'] == 'False' and float(r['cross']) >= MIN_AREA]
    listed += [r for r in rows if (r['folder'], r['model'], int(r['mesh'])) in CROSS_MODEL]
    rng = np.random.default_rng(1)
    t0 = time.time()
    want = defaultdict(set)  # map -> listed meshes; every placement of each is measured
    for r in listed:
        for m in r['maps'].split(','):
            want[m].add((r['folder'], r['model'], int(r['mesh'])))
    cov = defaultdict(list)
    for map_id in sorted(want):
        pieces = map_pieces(os.path.join(args.data, 'map', 'MapStudio', f'{map_id}.msb'))
        keys = {(fo, mo, me, pi) for fo, mo, me in want[map_id] for pi, pc in enumerate(pieces)
                if f'{pc[0]}A{map_id[1:3]}' == mo}
        t1 = time.time()
        for key, val in coverage(map_id, keys, args.data, read_flver, rng).items():
            cov[key[:3]].append((map_id, key[3], val))
        print(f'{map_id}: coverage of {len(keys)} mesh placements, {time.time() - t1:.0f} s', flush=True)
    for r in rows:
        r['cov_cross'] = r['cov_any'] = r['cov_share'] = r['cov_qual'] = ''
        c = cov.get((r['folder'], r['model'], int(r['mesh'])))
        if not c:
            continue
        total = float(r['total'])
        best = max(c, key=lambda x: x[2][0])
        ok = [x for x in c if x[2][0] >= MIN_AREA and x[2][0] >= MIN_SHARE * total]
        r['cov_cross'], r['cov_any'] = f'{best[2][0]:.3f}', f'{best[2][1]:.3f}'
        r['cov_share'] = f'{best[2][0] / total:.3f}'
        r['cov_qual'] = f'{len({(m, p) for m, p, _ in ok})}/{len({(m, p) for m, p, _ in c})}'
    cols = list(rows[0].keys())
    with open(path, 'w', newline=NL) as f:
        f.write(TAB.join(cols) + NL)
        for r in rows:
            f.write(TAB.join(r[c] for c in cols) + NL)

    def covered(r):
        return r['cov_cross'] != '' and float(r['cov_cross']) >= MIN_AREA and float(r['cov_cross']) >= MIN_SHARE * float(r['total'])

    def dup(r):
        return float(r['dup_partner_share']) >= DUP_FLAG
    fresh = [r for r in rows if r['in_table'] == 'False']
    new = [r for r in fresh if covered(r) and not dup(r)]
    band = [r for r in fresh if not covered(r) and r['cov_cross'] != '' and float(r['cov_cross']) >= MIN_AREA]
    partners = [r for r in band if not dup(r)]
    review = [r for r in fresh if covered(r) and dup(r)] + [r for r in band if dup(r)]
    for lst in (new, partners, review):
        lst.sort(key=lambda r: (r['folder'], -float(r['cov_cross'])))

    def line(r, why):
        return (f"    ({r['folder']!r}, {r['model']!r}, {r['mesh']}),  # {r['diffuse']}: {float(r['cov_cross']):.0f} of "
                f"{float(r['total']):.0f} m^2 covered ({100 * float(r['cov_share']):.0f}%), "
                f"{r['cov_qual']} placements, {why}" + NL)
    with open(os.path.join(args.out, 'cross_model.py'), 'w', newline=NL) as f:
        f.write('# From scan_cross_model_thin_walls.py --verify. Covered: sampled area of the mesh whose nearest blocking '
                'surface 0.01-0.5 m in front is an anti-parallel face of another piece; listed at >= 10% and >= 1 m^2.' + NL)
        f.write('CROSS_MODEL = {' + NL)
        for r in new:
            f.write(line(r, f"vs {r['partner']} {100 * float(r['gap']):.1f} cm"))
        f.write('}' + NL + NL + '# Bands: >= 1 m^2 covered but under 10% of the mesh (the known Asylum pair is of this '
                'kind: 1.5 and 3 m^2 of 912 and 378 m^2).' + NL + 'CROSS_MODEL_BANDS = {' + NL)
        for r in partners:
            f.write(line(r, f"vs {r['partner']} {100 * float(r['gap']):.1f} cm"))
        f.write('}' + NL + NL + f'# Review: >= {100 * DUP_FLAG:.0f}% of the mesh is coplanar (< 1 cm, same facing) with '
                'its partner piece; the two may be alternatives never drawn together.' + NL + 'CROSS_MODEL_REVIEW = {' + NL)
        for r in review:
            f.write(line(r, f"vs {r['partner']} {100 * float(r['gap']):.1f} cm, "
                            f"{100 * float(r['dup_partner_share']):.0f}% coplanar"))
        f.write('}' + NL)
    for r in rows:
        if (r['folder'], r['model'], int(r['mesh'])) in CROSS_MODEL:
            print(f"known: {r['folder']} {r['model']} mesh{r['mesh']}: covered {r['cov_cross']} of {float(r['total']):.0f} "
                  f"m^2 ({r['cov_share']}), face-rule share {r['share']}")
    per = defaultdict(lambda: [0, 0, 0])
    for k, lst in enumerate((new, partners, review)):
        for r in lst:
            per[r['folder']][k] += 1
    for k in sorted(per):
        print(f'{k}: {per[k][0]} covered, {per[k][1]} bands, {per[k][2]} to review')
    print(f'{len(new)} covered, {len(partners)} bands, {len(review)} to review, {time.time() - t0:.0f} s')


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=r'<steam dir>\steamapps\common'
                                      r'\Dark Souls Prepare to Die Edition\DATA\unpacked')
    ap.add_argument('--bbformats', default=r'<other project>\tools')
    ap.add_argument('--map', default='')
    ap.add_argument('--ignore-draw-groups', action='store_true')
    ap.add_argument('--out', required=True)
    ap.add_argument('--verify', action='store_true', help='second stage only: coverage of the listed meshes in '
                                                          '<out>/candidates.tsv, rewrites cross_model.py')
    args = ap.parse_args()
    global COVIS
    COVIS = not args.ignore_draw_groups
    sys.path.insert(0, args.bbformats)
    from bbformats.flver import read_flver
    if args.verify:
        verify(args, read_flver)
        return

    os.makedirs(args.out, exist_ok=True)
    table = table_sources(os.path.join(here, '..', 'assets', 'sky', 'thin_wall_meshes.txt'))
    t0 = time.time()
    rows = []
    for msb in sorted(glob.glob(os.path.join(args.data, 'map', 'MapStudio', 'm*.msb'))):
        map_id = os.path.basename(msb)[:-4]
        if args.map not in map_id:
            continue
        t1 = time.time()
        r, n, missing, nvis = scan_map(map_id, args.data, read_flver, {})
        rows += r
        print(f'{map_id}: {n} faces, {len(r)} mesh placements with cross pairs, '
              f'{sum(x["qualifies"] for x in r)} qualifying, {time.time() - t1:.0f} s, {nvis} face pairs dropped as never drawn together'
              + (f', missing {sorted(missing)}' if missing else ''), flush=True)

    # Per mesh (the table's unit): aggregate placements.
    mesh = defaultdict(list)
    for r in rows:
        mesh[(r['folder'], r['model'], r['mesh'])].append(r)
    # A qualifying mesh's partners need the tag too (each face hides the other from one side): meshes whose faces pair
    # >= MIN_AREA onto a qualifying placement.
    qual_place = {(r['map'], r['folder'], r['model'], r['mesh'], r['placement']) for r in rows if r['qualifies']}
    partner_of = defaultdict(set)
    for r in rows:
        for (pf, pm, pmi, ppi), ar in r['pairs'].items():
            if ar >= MIN_AREA and (r['map'], pf, pm, pmi, ppi) in qual_place:
                partner_of[(r['folder'], r['model'], r['mesh'])].add(f'{pm}/mesh{pmi}')
    out = []
    for key, rs in mesh.items():
        q = [r for r in rs if r['qualifies']]
        best = max(q or rs, key=lambda r: r['paired'] / r['total'])
        in_table = f'{key[0]}/{key[1]}/mesh{key[2]}/lod0' in table
        out.append(dict(best, n_qual=len({(r['map'], r['placement']) for r in q}),
                        n_cross=len({(r['map'], r['placement']) for r in rs}),
                        placements=sum(r['placements'] for r in {r['map']: r for r in rs}.values()),
                        in_table=in_table, maps=','.join(sorted({r['map'] for r in rs})),
                        qualifies=bool(q), within_qualifies=best['within'] >= MIN_AREA and
                        best['within'] >= MIN_SHARE * best['total'],
                        partner_of=','.join(sorted(partner_of.get(key, ())))))
    for r in out:
        r['share'] = r['paired'] / r['total']
        r['cross_share'] = r['cross'] / r['total']
        r['within_share'] = r['within'] / r['total']
        r['dup_share'] = r['dup'] / r['total']
        r['dup_partner_share'] = r['dup_partner'] / r['total']
    out.sort(key=lambda r: (r['folder'], not r['qualifies'], -r['cross']))
    cols = ['folder', 'model', 'mesh', 'diffuse', 'mtd', 'total', 'paired', 'share', 'cross', 'cross_share',
            'within_share', 'qualifies', 'within_qualifies', 'in_table', 'placements', 'n_cross', 'n_qual', 'partner',
            'partner_diffuse', 'partner_mtd', 'partner_area', 'gap', 'n_partners', 'dup_share', 'dup_partner_share', 'partner_of', 'maps']
    with open(os.path.join(args.out, 'candidates.tsv'), 'w', newline=NL) as f:
        f.write(TAB.join(cols) + NL)
        for r in out:
            f.write(TAB.join(f'{r[c]:.3f}' if isinstance(r[c], float) else str(r[c]) for c in cols) + NL)
    def dup(r):
        return r['dup_partner_share'] >= DUP_FLAG
    new = [r for r in out if r['qualifies'] and not r['in_table'] and not dup(r)]
    partners = [r for r in out if not r['qualifies'] and not r['in_table'] and r['partner_of'] and not dup(r)]
    review = [r for r in out if not r['in_table'] and (r['qualifies'] or r['partner_of']) and dup(r)]
    odd = [r for r in out if r['within_qualifies'] and not r['in_table']]

    def line(r, why):
        return (f"    ({r['folder']!r}, {r['model']!r}, {r['mesh']}),  # {r['diffuse']} {100 * r['share']:.0f}% of "
                f"{r['total']:.0f} m^2 ({100 * r['cross_share']:.0f}% cross), {r['n_qual']}/{r['placements']} placements"
                f" qualify, {why}" + (f", {100 * r['dup_share']:.0f}% coplanar with another piece" if r['dup_share'] >= 0.05 else '') + NL)
    with open(os.path.join(args.out, 'cross_model.py'), 'w', newline=NL) as f:
        f.write('CROSS_MODEL = {' + NL)
        for r in new:
            f.write(line(r, f"vs {r['partner']} {100 * r['gap']:.1f} cm"))
        f.write('}' + NL + NL + '# Partners of the above that fail the share rule themselves (>= 1 m^2 paired onto a '
                'qualifying placement):' + NL + 'CROSS_MODEL_PARTNERS = {' + NL)
        for r in partners:
            f.write(line(r, f"partner of {r['partner_of']}"))
        f.write('}' + NL + NL + f'# Review: >= {100 * DUP_FLAG:.0f}% of the mesh lies coplanar (same facing, < 1 cm) with its '
                'partner piece, so the two pieces may be alternatives never shown together:' + NL +
                'CROSS_MODEL_REVIEW = {' + NL)
        for r in review:
            f.write(line(r, f"vs {r['partner']} {100 * r['gap']:.1f} cm, {100 * r['dup_partner_share']:.0f}% coplanar "
                            f"with it" + ('' if r['qualifies'] else f", partner of {r['partner_of']}")))
        f.write('}' + NL)
    for r in out:
        if (r['folder'], r['model'], r['mesh']) in CROSS_MODEL:
            print(f"known: {r['folder']} {r['model']} mesh{r['mesh']} share {100 * r['share']:.0f}% cross "
                  f"{100 * r['cross_share']:.0f}% of {r['total']:.1f} m^2 vs {r['partner']} gap {100 * r['gap']:.1f} cm "
                  f"qualifies={r['qualifies']} partner_of={r['partner_of']}")
    for r in odd:
        print(f"WARNING within-model rule passes but not in table: {r['folder']}/{r['model']}/mesh{r['mesh']}")
    per_map = defaultdict(lambda: [0, 0, 0])
    for k, lst in enumerate((new, partners, review)):
        for r in lst:
            per_map[r['folder']][k] += 1
    for k in sorted(per_map):
        print(f'{k}: {per_map[k][0]} qualifying, {per_map[k][1]} partners, {per_map[k][2]} to review')
    print(f'{len(new)} new qualifying meshes, {len(partners)} partners, {len(review)} to review, '
          f'{len(out)} meshes with any cross pair, '
          f'{time.time() - t0:.0f} s')

if __name__ == '__main__':
    main()
