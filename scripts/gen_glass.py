"""Generate the stained- and leaded-glass split: assets/mesh_tables/glass_meshes.txt, assets/glass/*.dds and
the ds1_glass Remix mod.

DS1 paints its windows into one texture: the leaded came (and the window frame) at alpha >= ~248, the
glass at a lower alpha, its opacity in the game's blend (a * lit + (1 - a) * behind). Some meshes draw
it blended (the panes), some opaque (the rose window discs). Under Remix a blended pane was tagged as a
decal and vanished in front of the sky, and an opaque one was a painted wall: no light came through.

The proxy draws each listed glass mesh twice:
  - the came: the game's texture, blending off, alpha test >= the texture's cut. Opaque and path traced;
  - the glass: a derived copy of the texture with the came's alpha zeroed, 2 mm off the came, blended.
    The copy has its own Remix hash (XXH3 of mip 0), which the ds1_glass mod
    replaces with AperturePBR_Translucent: thin-walled, transmittance = lerp(1, rgb, a) of that same
    texture, so the game's own glass opacity and tint become the transmittance. Without the mod the
    glass pass renders as the game's blended glass.

Glass textures: map textures in BC3 with a came population (>= MIN_CAME of texels at alpha >= 248), a
glass population (>= MIN_GLASS in 1..235) and little in between. Glass meshes: map and object meshes using one, with
at least MIN_GLASS_AREA of their surface on glass texels (the furniture window's frame mesh shares the
pane texture but maps only onto the frame, so it stays a single opaque draw).

Glass with nothing modelled behind it. Many towers and facades carry windows over an interior that was
never built: its walls are only the back faces of the outer shell. Seen through transmissive glass, rays
passed through that culled shell to stray panes and polygons beyond, and the glass mirrored the sky where
the game shows dark painted panes (frame_20261028_195126). Rays from the pane along both normals tell them
apart: a side where at least VOID_SIDE of the rays first hit a back face VOID_MIN_DIST or farther out
(nearer first hits are the window's own jambs and frame, and count as built) is a void. Rooms, the chapel
and the open sky give front faces or misses. A void-backed mesh the game draws opaque is left out of the
table, so it stays the game's painted surface (no sun goes into a room that does not exist); one the
game blends keeps the came pass and a glass pass with the game's own blend (a second derived texture,
<name>_blend, that the mod does not replace).

Outer panes. Anor Londo builds many windows twice, in separate models: an outer pane facing the open air
and an inner one facing the room (m2310 / m3310 for the chapel, m2330 / m3370 for the hall). The game
draws glass single-sided, so each view sees only its own pane; the proxy keeps the glass pass
single-sided too (stage-4 tag 0xD4000002). The two panes of a window lie back to back: a pane's partner
is the glass mesh that a ray PAIR_DIST along its -normal meets from behind for the largest share of its
samples, at least PAIR_SHARE (one outer mesh can back several inner ones, so this is per pane, not
mutual). A pane with a partner is not void-backed, whatever its rays find: something is built behind it.
Of a pair, the pane whose front (its vertex normals: the winding agrees on every glass mesh) looks farther out is the
outer one: the larger share of front hemisphere rays escaping or first hitting beyond FAR_DIST (the
first FAR_SKIP are the window's own tracery). A pane without a partner is an outer pane when that share
reaches OUTER_FAR. The open sky alone does not tell: Anor Londo lies in a canyon, and the cliffs fill much
of an outer pane's hemisphere. Through transmissive glass an outer pane showed the dark room behind it
and mirrored the sky, where the game shows painted panes (frame_20261028_194513), so it gets the game's
blend glass pass. That holds for outer panes the game draws opaque too: Remix's shadow rays are not
culled, and a pane left fully opaque would shut all direct sun out of the room behind it, where the
blended glass pass lets (1 - a) of it through and the came casts the lattice. Inner panes keep the
translucent material.

Each mesh's rays are seeded from its own name, so a decision depends only on that mesh and its map.

Objects (obj/) have no placement here and are not tested.

Table lines (one per face set, every LOD), the sky table format plus the cut and the glass pass texture:
  <FNV-1a 64 of the u16 index bytes, hex> <index count> <map>/<model>/mesh<i>/lod<j> <texture> <cut> <derived>

Needs embreex (pip install embreex) for the ray tests.

Usage: py -3.12 scripts/gen_glass.py [--data <DATA\\unpacked>] [--bbformats <dir>]
"""
import os  # noqa: E402  (ds1paths: the local folders, see paths.example.ini)
import sys  # noqa: E402
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)))))
import ds1paths  # noqa: E402

import argparse
import glob
import os
import re
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GAME_DIR = ds1paths.GAME + r'\DATA'

# The alpha shape alone also fits mist, bushes and decals (opaque cores over soft fringes): only
# window and glass textures are candidates. Grilles, shutters and window sets are wood and iron.
GLASS_NAME = re.compile(r'(glass|window)(?!.*(grille|shutter|set))')
MIN_CAME = 0.05
MIN_GLASS = 0.15
MIN_GLASS_AREA = 0.2
DEFAULT_CUT = 240
# Glass whose texels ramp up into the lead (DXT ramp over ~225-249): cut nearer the lead.
CUTS = {
    'm15_chapel_glass_01': 245,
}
# Frosted, untinted glass keeps the game's dirty-white streaks as a diffuse layer; stained glass has
# none, so the light it lets through keeps each texel's colour.
DIFFUSE_LAYER = {
    'm15_furniture_window',
}
# 2 mm thin glass; Remix measures thin-walled transmittance over 1.0, so a thickness of 1.0 makes the
# texture colour the per-pass transmittance at normal incidence.
THIN_WALL_THICKNESS = 1.0
IOR = 1.5
# Void test (see the docstring): rays per side, their spread around the normal, how far they look.
VOID_SAMPLES = 480
VOID_CONE = 0.15
VOID_MIN_DIST = 2.0
VOID_MAX_DIST = 40.0
VOID_SIDE = 0.3
# Outer test (see the docstring): cosine-weighted rays over the front hemisphere.
FAR_SAMPLES = 384
FAR_SKIP = 1.5
FAR_DIST = 50.0
OUTER_FAR = 0.4
PAIR_SAMPLES = 96
PAIR_DIST = 1.0
PAIR_SHARE = 0.3


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


# ---- DDS (BC3) -------------------------------------------------------------------------------------

def read_dds(path):
    """(header bytes, width, height, mip count, fourcc, data bytes)."""
    raw = open(path, 'rb').read()
    if raw[:4] != b'DDS ':
        return None
    height, width = struct.unpack_from('<II', raw, 12)
    mips = max(1, struct.unpack_from('<I', raw, 28)[0])
    fourcc = raw[84:88]
    return raw[:128], width, height, mips, fourcc, raw[128:]


def bc3_levels(width, height, mips):
    """(offset, blocks wide, blocks high) of each mip level in the data."""
    out, off = [], 0
    for i in range(mips):
        w, h = max(1, width >> i), max(1, height >> i)
        bw, bh = (w + 3) // 4, (h + 3) // 4
        out.append((off, bw, bh))
        off += bw * bh * 16
    return out


def alpha_palettes(a0, a1):
    """(n, 8) BC3 alpha palettes."""
    a0 = a0.astype(np.int32)
    a1 = a1.astype(np.int32)
    p = np.zeros((len(a0), 8), np.int32)
    p[:, 0], p[:, 1] = a0, a1
    seven = a0 > a1
    for i in range(1, 7):
        p[:, i + 1] = np.where(seven, ((7 - i) * a0 + i * a1 + 3) // 7, 0)
    for i in range(1, 5):
        p[:, i + 1] = np.where(seven, p[:, i + 1], ((5 - i) * a0 + i * a1 + 2) // 5)
    p[:, 6] = np.where(seven, p[:, 6], 0)
    p[:, 7] = np.where(seven, p[:, 7], 255)
    return p


def block_alphas(blocks):
    """(n, 16) alpha values of (n, 16) uint8 BC3 blocks."""
    bits = np.zeros(len(blocks), np.uint64)
    for i in range(6):
        bits |= blocks[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    idx = np.stack([(bits >> np.uint64(3 * t)) & np.uint64(7) for t in range(16)], 1).astype(np.int64)
    pal = alpha_palettes(blocks[:, 0], blocks[:, 1])
    return np.take_along_axis(pal, idx, 1)


def top_alpha(header, width, height, mips, data):
    """Mip 0 alpha as a (height, width) uint8 image."""
    off, bw, bh = bc3_levels(width, height, mips)[0]
    blocks = np.frombuffer(data, np.uint8, bw * bh * 16, off).reshape(-1, 16)
    a = block_alphas(blocks).reshape(bh, bw, 4, 4).transpose(0, 2, 1, 3).reshape(bh * 4, bw * 4)
    return a[:height, :width].astype(np.uint8)


def zero_came(data, width, height, mips, cut):
    """The BC3 data with every texel at alpha >= cut set to alpha 0 (all mips); colour untouched.

    A block holding came texels is re-encoded in the 6-value mode (a0 <= a1), whose code 6 is an exact
    0: its glass texels take the nearest of the palette spanned by their own range."""
    out = bytearray(data)
    for off, bw, bh in bc3_levels(width, height, mips):
        n = bw * bh
        blocks = np.frombuffer(bytes(out[off:off + n * 16]), np.uint8).reshape(n, 16).copy()
        alphas = block_alphas(blocks)
        came = alphas >= cut
        rows = np.nonzero(came.any(1))[0]
        for r in rows:
            a, c = alphas[r], came[r]
            g = a[~c]
            lo, hi = (int(g.min()), int(g.max())) if len(g) else (0, 0)
            pal = alpha_palettes(np.array([lo]), np.array([hi]))[0]
            codes = np.where(c, 6, np.abs(a[:, None] - pal[None, :7]).argmin(1))
            bits = 0
            for t in range(16):
                bits |= int(codes[t]) << (3 * t)
            blocks[r, 0], blocks[r, 1] = lo, hi
            blocks[r, 2:8] = np.frombuffer(bits.to_bytes(6, 'little'), np.uint8)
        out[off:off + n * 16] = blocks.tobytes()
    return bytes(out)


# ---- meshes -----------------------------------------------------------------------------------------

def diffuse_name(material) -> str:
    for t in material.textures:
        if t.path:
            return os.path.basename(t.path.replace('\\', '/')).rsplit('.', 1)[0].lower()
    return ''


def first(v):
    return v[0] if isinstance(v, list) else v


def glass_area(mesh, alpha, cut, rng):
    """Fraction of the mesh's LOD0 surface (by world area) that samples glass texels."""
    uvs = mesh.vertices.get('uv') if hasattr(mesh.vertices, 'get') else None
    if not uvs:
        return 0.0
    uv = np.asarray(first(uvs), np.float64)[:, :2]
    p = np.asarray(first(mesh.vertices['position']), np.float64)[:, :3]
    fs = mesh.face_sets[0]
    idx = np.asarray(fs.indices, np.int64)
    if fs.triangle_strip:
        if len(idx) < 3:
            return 0.0
        t = np.stack([idx[:-2], idx[1:-1], idx[2:]], 1)
        t = t[(t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])]
    else:
        t = idx[: len(idx) // 3 * 3].reshape(-1, 3)
    t = t[((t >= 0) & (t < len(p))).all(1)]
    if not len(t):
        return 0.0
    area = 0.5 * np.linalg.norm(np.cross(p[t[:, 1]] - p[t[:, 0]], p[t[:, 2]] - p[t[:, 0]]), axis=1)
    if area.sum() <= 0:
        return 0.0
    h, w = alpha.shape
    k = 32
    b = rng.random((len(t), k, 2))
    flip = b.sum(2) > 1
    b[flip] = 1 - b[flip]
    s = uv[t[:, 0]][:, None] * (1 - b.sum(2))[..., None] + uv[t[:, 1]][:, None] * b[..., :1] + uv[t[:, 2]][:, None] * b[..., 1:]
    x = (np.floor(s[..., 0] * w).astype(np.int64)) % w
    y = (np.floor(s[..., 1] * h).astype(np.int64)) % h
    glass = (alpha[y, x] < cut).mean(1)
    return float((glass * area).sum() / area.sum())


def mesh_triangles(mesh):
    """(positions, vertex normals, LOD0 triangle indices)."""
    pos = np.asarray(first(mesh.vertices['position']), np.float64)[:, :3]
    nrm = np.asarray(first(mesh.vertices['normal']), np.float64)[:, :3]
    fs = mesh.face_sets[0]
    idx = np.asarray(fs.indices, np.int64)
    if fs.triangle_strip:
        t = np.stack([idx[:-2], idx[1:-1], idx[2:]], 1) if len(idx) >= 3 else np.zeros((0, 3), np.int64)
        t = t[(t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])]
    else:
        t = idx[: len(idx) // 3 * 3].reshape(-1, 3)
    t = t[((t >= 0) & (t < len(pos))).all(1)]
    return pos, nrm, t


def raycast(origins, dirs, world, min_dist=0.05, max_dist=VOID_MAX_DIST):
    """Per ray: (distance of the first hit in (min_dist, max_dist), or inf; whether it is a back face).
    The ray starts min_dist out: anything nearer than that is passed through."""
    scene, nrm = world
    dirs = np.asarray(dirs, np.float64)
    o = (np.asarray(origins, np.float64) + dirs * min_dist).astype(np.float32)
    res = scene.run(o, dirs.astype(np.float32),
                    dists=np.full(len(o), max_dist - min_dist, np.float32), output=1)
    prim = res['primID']
    hit = prim >= 0
    out_t = np.where(hit, res['tfar'].astype(np.float64) + min_dist, np.inf)
    out_back = np.zeros(len(o), bool)
    out_back[hit] = (nrm[prim[hit]] * dirs[hit]).sum(1) > 0
    return out_t, out_back


def world_of(meshes):
    """Ray test geometry: the map's opaque, untested meshes (blended, additive and alpha-tested
    meshes are not what a window looks onto), as an Embree scene plus each triangle's vertex normal
    (which side is its front)."""
    from embreex import rtcore_scene
    from embreex.mesh_construction import TriangleMesh
    tris, nrms = [], []
    for mesh, mtd in meshes:
        if '_alp' in mtd or '_add' in mtd or '_edge' in mtd:
            continue
        pos, nrm, t = mesh_triangles(mesh)
        tris.append(pos[t])
        nrms.append(nrm[t].mean(1))
    scene = rtcore_scene.EmbreeScene()
    TriangleMesh(scene, np.concatenate(tris).astype(np.float32))
    return scene, np.concatenate(nrms)


def void_side(mesh, world, rng):
    """The larger of the two sides' fractions of rays that hit back faces (see the docstring)."""
    pos, nrm, t = mesh_triangles(mesh)
    tri = pos[t]
    area = 0.5 * np.linalg.norm(np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0]), axis=1)
    pick = rng.choice(len(t), VOID_SAMPLES, p=area / area.sum())
    b = rng.random((VOID_SAMPLES, 2))
    flip = b.sum(1) > 1
    b[flip] = 1 - b[flip]
    w = np.stack([1 - b.sum(1), b[:, 0], b[:, 1]], 1)
    pts = (tri[pick] * w[..., None]).sum(1)
    n = (nrm[t][pick] * w[..., None]).sum(1)
    n /= np.linalg.norm(n, axis=1, keepdims=True)
    worst = 0.0
    for side in (1.0, -1.0):
        d = side * n + rng.normal(0, VOID_CONE, n.shape)
        d /= np.linalg.norm(d, axis=1, keepdims=True)
        th, back = raycast(pts, d, world)
        worst = max(worst, float((np.isfinite(th) & (th >= VOID_MIN_DIST) & back).mean()))
    return worst


def pane_samples(mesh, rng, count):
    """Area-weighted points on the pane and their (interpolated) vertex normals."""
    pos, nrm, t = mesh_triangles(mesh)
    tri = pos[t]
    area = 0.5 * np.linalg.norm(np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0]), axis=1)
    pick = rng.choice(len(t), count, p=area / area.sum())
    b = rng.random((count, 2))
    flip = b.sum(1) > 1
    b[flip] = 1 - b[flip]
    w = np.stack([1 - b.sum(1), b[:, 0], b[:, 1]], 1)
    pts = (tri[pick] * w[..., None]).sum(1)
    n = (nrm[t][pick] * w[..., None]).sum(1)
    return pts, n / np.linalg.norm(n, axis=1, keepdims=True)


def front_far(mesh, world, rng):
    """Share of the front hemisphere (vertex normal side) that escapes or first hits beyond FAR_DIST."""
    pts, n = pane_samples(mesh, rng, FAR_SAMPLES)
    r = rng.normal(size=n.shape)
    d = n + r / np.linalg.norm(r, axis=1, keepdims=True)
    d /= np.linalg.norm(d, axis=1, keepdims=True)
    th, _ = raycast(pts, d, world, FAR_SKIP, FAR_DIST)
    return float((~np.isfinite(th)).mean())


def back_to_back(mesh, other, rng):
    """Share of the pane's samples whose ray PAIR_DIST along -normal meets the other pane's back."""
    pts, n = pane_samples(mesh, rng, PAIR_SAMPLES)
    th, back = raycast(pts, -n, world_of([(other, '')]), 0.0, PAIR_DIST)
    return float((np.isfinite(th) & back).mean())


def blend_variant(derived, width, height, mips):
    """The derived texture with one all-came block's unused alpha endpoint changed: the same pixels
    (its texels use code 6, an exact 0), a different Remix hash."""
    out = bytearray(derived)
    off, bw, bh = bc3_levels(width, height, mips)[0]
    for k in range(bw * bh):
        blk = 128 + off + k * 16
        a0, a1 = out[blk], out[blk + 1]
        bits = int.from_bytes(out[blk + 2:blk + 8], 'little')
        if a0 <= a1 < 255 and all((bits >> (3 * t)) & 7 == 6 for t in range(16)):
            out[blk + 1] = a1 + 1
            return bytes(out)
    raise ValueError('no all-came block to vary')


# ---- mod --------------------------------------------------------------------------------------------

def mod_usda(materials):
    out = ['#usda 1.0', '(', '    customLayerData = {',
           '        string lightspeed_game_name = "DarkSouls1"',
           '        string lightspeed_layer_type = "replacement"',
           '        string lightspeed_mod_name = "ds1_glass"',
           '        string lightspeed_mod_notes = """Translucent window glass for the proxy\'s glass pass '
           '(its derived glass textures: the game texture with the came alpha zeroed). Transmittance = '
           'lerp(1, rgb, a) of the game texture. Built by scripts/gen_glass.py."""',
           '        string lightspeed_mod_version = "1.0"', '    }', '    upAxis = "Y"', ')', '',
           'over "RootNode"', '{', '    over "Looks"', '    {']
    for name, h, diffuse in materials:
        out += [f'        over "mat_{h:016X}"  # {name}', '        {', '            over "Shader"', '            {',
                '                uniform token info:implementationSource = "sourceAsset"',
                '                uniform asset info:mdl:sourceAsset = @AperturePBR_Translucent.mdl@',
                '                uniform token info:mdl:sourceAsset:subIdentifier = "AperturePBR_Translucent"',
                f'                asset inputs:transmittance_texture = @./{name}.dds@ (',
                '                    colorSpace = "raw"', '                )',
                '                color3f inputs:transmittance_color = (1, 1, 1)',
                f'                float inputs:ior_constant = {IOR}',
                '                bool inputs:thin_walled = 1',
                f'                float inputs:thin_wall_thickness = {THIN_WALL_THICKNESS}',
                f'                bool inputs:use_diffuse_layer = {1 if diffuse else 0}',
                '            }', '        }']
    out += ['    }', '}', '']
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=os.path.join(GAME_DIR, 'unpacked'))
    ap.add_argument('--bbformats', default=ds1paths.SCRIPTS)
    ap.add_argument('--mod', default=os.path.join(GAME_DIR, 'rtx-remix', 'mods', 'ds1_glass'))
    args = ap.parse_args()

    sys.path.insert(0, args.bbformats)
    sys.path.insert(0, ds1paths.PBR_TOOLS)
    from bbformats.flver import read_flver
    import xxhash

    rng = np.random.default_rng(0)
    textures = {}   # name -> (dds tuple, alpha, cut) or None

    def texture(area, name):
        key = (area, name)
        if key in textures:
            return textures[key]
        textures[key] = None
        path = os.path.join(args.data, 'map', area, name + '.dds')
        dds = read_dds(path) if os.path.exists(path) else None
        if not dds or dds[4] != b'DXT5':
            # Glass without an alpha channel (Duke's Archives' DXT1 chapel glass) has nothing to
            # split on: it stays the game's painted surface.
            print(f'not glass {area}/{name}: {dds[4].decode(errors="replace") if dds else "no DDS"}, no alpha to split')
            return None
        alpha = top_alpha(dds[0], dds[1], dds[2], dds[3], dds[5])
        came = (alpha >= 248).mean()
        glass = ((alpha >= 1) & (alpha <= 235)).mean()
        between = ((alpha > 235) & (alpha < 248)).mean()
        if came < MIN_CAME or glass < MIN_GLASS:
            print(f'not glass {area}/{name}: came {came:.0%}, glass {glass:.0%}')
            return None
        cut = CUTS.get(name, DEFAULT_CUT)
        print(f'glass texture {area}/{name}: came {came:.0%}, glass {glass:.0%}, between {between:.1%}, cut {cut}')
        textures[key] = (path, dds, alpha, cut)
        return textures[key]

    lines = []
    used = {}       # texture -> its glass tuple
    blended = set()  # textures that also need the plain-blend variant
    # Map pieces (one map's pieces share its origin, so they are tested against each other), and
    # objects (Anor Londo's o5966/o5999 use the chapel glass); a map texture lives with its area's
    # TPFs (map/mNN) whoever draws it.
    groups = [(os.path.basename(d), sorted(glob.glob(os.path.join(d, '*.flver'))))
              for d in sorted(glob.glob(os.path.join(args.data, 'map', 'm*_*')))]
    groups.append(('obj', sorted(glob.glob(os.path.join(args.data, 'obj', '*', '*.flver')))))
    for map_id, paths in groups:
        meshes, glass = [], []
        for path in paths:
            flver = read_flver(open(path, 'rb').read())
            model = os.path.basename(path).rsplit('.', 1)[0]
            for i, mesh in enumerate(flver.meshes):
                material = flver.materials[mesh.material_index]
                mtd = os.path.basename(material.mtd)
                name = diffuse_name(material)
                area = re.match(r'm\d\d(?=_)', name)
                tex = texture(area.group(0), name) if area and GLASS_NAME.search(name) else None
                keep = False
                if tex:
                    frac = glass_area(mesh, tex[2], tex[3], rng)
                    keep = frac >= MIN_GLASS_AREA
                    print(f'  {"glass " if keep else "frame "} {map_id}/{model}/mesh{i} {name} {mtd}: '
                          f'{frac:.0%} on glass')
                if keep:
                    glass.append((model, i, mesh, mtd, name, tex))
                else:
                    meshes.append((mesh, mtd.lower()))
        world = world_of(meshes) if glass and map_id != 'obj' else None
        seeds = [fnv1a64(f'{map_id}/{g[0]}/mesh{g[1]}'.encode()) for g in glass]
        void = [void_side(g[2], world, np.random.default_rng(sd)) if world else 0.0 for g, sd in zip(glass, seeds)]
        far = [front_far(g[2], world, np.random.default_rng(sd ^ 1)) if world else 0.0 for g, sd in zip(glass, seeds)]
        partner = [None] * len(glass)
        for a in range(len(glass)) if world else []:
            shares = [back_to_back(glass[a][2], glass[b][2], np.random.default_rng(seeds[a] ^ 2)) if b != a else 0.0
                      for b in range(len(glass))]
            best = int(np.argmax(shares))
            if shares[best] >= PAIR_SHARE:
                partner[a] = best
        for k, (model, i, mesh, mtd, name, tex) in enumerate(glass):
            derived = name
            if world is not None:
                where = f'{map_id}/{model}/mesh{i}'
                p = partner[k]
                if p is None and void[k] >= VOID_SIDE and '_alp' not in mtd.lower():
                    print(f'  void-backed ({void[k]:.0%}) {where}: drawn opaque as the game does')
                    continue
                if p is None and void[k] >= VOID_SIDE:
                    why = f'void-backed ({void[k]:.0%})'
                elif p is not None:
                    other = f'{glass[p][0]}/mesh{glass[p][1]}'
                    why = (f'outer pane of a pair (far {far[k]:.0%} vs {other} {far[p]:.0%})' if far[k] > far[p] else None)
                else:
                    why = f'outer pane (far {far[k]:.0%})' if far[k] >= OUTER_FAR else None
                if why:
                    print(f'  {why} {where}: the game\'s blend')
                    derived = name + '_blend'
                    blended.add(name)
                else:
                    pair = f', inner of a pair with {glass[p][0]}/mesh{glass[p][1]}' if p is not None else ''
                    print(f'  inner pane {where} (void {void[k]:.0%}, far {far[k]:.0%}{pair}): translucent')
            used[name] = tex
            for j, fs in enumerate(mesh.face_sets):
                idx = np.asarray(fs.indices, dtype='<u2')
                lines.append(f'{fnv1a64(idx.tobytes()):016x} {len(idx)} {map_id}/{model}/mesh{i}/lod{j} '
                             f'{name} {tex[3]} {derived}')

    tex_out = os.path.join(ROOT, 'assets', 'glass')
    os.makedirs(tex_out, exist_ok=True)
    os.makedirs(args.mod, exist_ok=True)
    materials = []
    for name, (path, dds, alpha, cut) in sorted(used.items()):
        header, width, height, mips, fourcc, data = dds
        derived = header + zero_came(data, width, height, mips, cut)
        for d in (tex_out, args.mod):
            with open(os.path.join(d, name + '.dds'), 'wb') as f:
                f.write(derived)
        top = bc3_levels(width, height, mips)[0]
        h = xxhash.xxh3_64_intdigest(derived[128:128 + top[1] * top[2] * 16])
        materials.append((name, h, name in DIFFUSE_LAYER))
        print(f'derived {name}: Remix hash {h:016X}')
        if name in blended:
            with open(os.path.join(tex_out, name + '_blend.dds'), 'wb') as f:
                f.write(blend_variant(derived, width, height, mips))
            print(f'derived {name}_blend (game blend, not in the mod)')

    table = os.path.join(ROOT, 'assets', 'mesh_tables', 'glass_meshes.txt')
    with open(table, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# Dark Souls window glass meshes: <index FNV-1a 64> <index count> <source> <diffuse> <alpha cut> '
                '<glass pass texture>\n')
        f.write('# Generated by scripts/gen_glass.py; do not edit by hand.\n')
        f.writelines(line + '\n' for line in lines)
    with open(os.path.join(args.mod, 'mod.usda'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(mod_usda(materials))
    print(f'{len(used)} glass textures, {len(lines)} face sets -> {table}; mod -> {args.mod}')


if __name__ == '__main__':
    main()
