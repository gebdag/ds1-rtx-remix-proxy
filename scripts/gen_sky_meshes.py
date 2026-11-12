"""Generate assets/mesh_tables/sky_meshes.txt: the map meshes the proxy sends to Remix as sky.

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
  - EXTRA_LAYERS: far layers checked against captures one by one (Ash Lake's mist, the Archives' far ground).
    CAMERA_ABOVE holds one such mesh in the sky only while the camera is higher than that (the proxy reads
    `above <y>` at the end of its lines): sky draws behind all world geometry, which is wrong near it.

Also writes assets/mesh_tables/shell_meshes.txt, same format: the blended shells (see blended_shells), which
the proxy draws as their own surfaces instead of decals. Outside SHELL_MAPS only solid shells count (see
solid_shell): terrain pieces the game draws blended but paints opaque, fading only at their vertex edges.
In GAP_SKIRT_MAPS the gap skirts (see gap_skirts) are listed too, their lines ending in ' gap'.

And assets/mesh_tables/skirt_backing.txt: in GAP_SKIRT_MAPS, the skirts part of whose faces hang over open air (see
skirt_backings) and that a capture confirmed (BACKING_CONFIRMED). They stay decals; the proxy draws their hanging faces once more as a solid cutout, so the decal has a
surface there. One line per face set: <index FNV-1a 64> <index count> <source> <texture> <n> <n indices of a triangle list>.

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
# Solid shells, everywhere (solid_shell): a blended terrain piece off any surface whose texture is opaque
# and whose body is: the game fades it out at its edges only. As a decal it was pasted onto whatever lay
# behind it and dropped where nothing did (m16 m2750B0A16 mesh15, frame_20261029_082729: the Valley of
# Drakes hillside went flat and dark, with a white hole). Real decals carry their shape in the texture's
# alpha and edge-fade strips lie on their surface, so neither qualifies.
SOLID_ON_SURFACE = 0.3        # fewer of its visible vertices than this lie on an opaque surface
# ... and fewer than this within SOLID_NEAR of one: overlays a few centimetres over their own model's
# ground (m10_01 m3170B1A10, m18_00 m0010B0A18: 53-96% within 20 cm) are blends, not terrain; the
# pieces off any surface have at most 34% there.
SOLID_NEAR = 0.2
SOLID_NEAR_SHARE = 0.4
SOLID_TEXTURE_OPAQUE = 0.95   # texels at alpha >= 250 (the diffuse)
SOLID_BODY = 0.3              # vertices with alpha >= 0.9
# Plain map materials only: waterfalls, crystals and glass are blended by design.
SOLID_MTD = re.compile(r'^(m\[|m_4stone|a12_ground)')
# Gap skirts (gap_skirts), only in maps checked against captures: a plain blended map piece whose vertices lie on surfaces
# but whose faces span a trench. New Londo's walkway and rock skirts (m1200B0A16 mesh4, m1210B0A16 mesh6) fade from
# alpha 1 at a walkway's top edge to 0 on the floor beyond a trench 0.5-0.9 m deep. As decals they landed on the trench
# floor, past the runtime's 0.5 m decal lift (geometry_resolver kDs1SkirtMaxLift), and left a dark gap
# (frame_20261111_105331, frame_20261111_105412, frame_20261111_082245). Listed as shells marked 'gap', the proxy cuts
# them at [Meshes] GapSkirtAlphaRef so the faded part over the trench stays solid.
GAP_SKIRT_MAPS = {
    'm16_00_00_00',  # New Londo Ruins
}
GAP_DEPTH = 0.5          # the runtime's decal lift: a decal further than this over its surface is not lit at its own place
# ... for at least this share of the points inside its visible faces. New Londo: 62-63% for the two skirts (2532 points each),
# then 26% and less. Two more reach 50% on too few points (m0100B0A16 mesh13: 2, GAP_MIN_POINTS) or lie off any surface
# (m0200B0A16 mesh4, a cliff shell candidate).
GAP_SHARE = 0.4
GAP_POINTS = 6           # sample points per visible face
GAP_MIN_POINTS = 100     # sample points with a surface below them, at least: a share of a few faces says little
# Backed skirts (skirt_backings), same maps: a skirt whose vertices lie on surfaces like a gap skirt's, but whose faces
# span open air, not a trench: nothing lies within the ground's reach (1 m) below them. As a decal it was dropped there
# and showed the sky through the seam between the Valley of Drakes' bank and its cliff (m2750B0A16 mesh4,
# frame_20261112_014436, _014452, _014500). Cut whole like a gap skirt, its other half, which lies on the grass and fades
# into it, became a hard-edged sheet of rock. So it stays a decal and only its hanging faces are drawn again, solid.
BACKING_OPEN_SHARE = 0.25  # of its face points over open air. m2750B0A16 mesh4: 31%, its far copy m0100B0A16 mesh6: 30%;
#                            the next plain skirt has 13% (m2600B0A16 mesh8)
BACKING_MAX_OPEN = 0.5     # ... and no more than this: a piece mostly in the open is no skirt lying on the ground but a
#                            shell (solid_shell's to judge). m2750B0A16 mesh12, 81% in the open, fades over most of its body and
#                            is in no capture: it stays a decal
BACKING_POINTS = 24        # sample points per face for that share
# Only the skirts seen wrong in a capture and right with their backing are listed; any other the rule finds is printed as a
# candidate and stays a decal until it is seen. m0100B0A16 mesh6 (30% in the open) is such a candidate: a coarser copy of
# part of the same bank, drawn in none of the captures.
BACKING_CONFIRMED = {
    ('m16_00_00_00', 'm2750B0A16', 4),   # frame_20261112_014436, _014452, _014500
}
# Skirts drawn as their own surface, one by one from captures: a plain skirt whose vertices lie on a steep surface, so neither
# the shell nor the skirt rules take it, but whose visible part reaches past that surface's curved edge, where the next
# surface along the view lies metres behind. As a decal it took that surface's light there: a pale strip along the edge.
SKIRTS_AS_SURFACES = {
    ('m14_00_00_00', 'm0050B0A14', 10),  # Blighttown, a mound's foot over the swamp: frame_20261112_142920
}
BACKING_STEP = 0.05        # m between the points that decide which faces hang: a face with one such point hanging is backed.
#                            The seam is a slit a few centimetres wide across faces up to 5 m long; 24 points a face missed it
#                            in places (frame_20261112_014500 kept two slivers)
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
    # Tomb of the Giants, the view of Ash Lake (frame_20261104_094601): the forest, its ground, the lake and the mist, all a kilometre or more
    # across and 160 m or more from the path. As world geometry they sealed the lookout from the ambient sky and went black, the mist with them.
    ('m13_01_00_00', 'm0010B1A13', 0),
    ('m13_01_00_00', 'm0010B1A13', 1),
    ('m13_01_00_00', 'm0020B1A13', 0),
    ('m13_01_00_00', 'm0110B1A13', 0),
    ('m13_01_00_00', 'm0030B1A13', 0),
    ('m13_01_00_00', 'm0050B1A13', 0),
    ('m13_01_00_00', 'm0100B1A13', 0),
    ('m13_01_00_00', 'm0100B1A13', 1),
    ('m13_01_00_00', 'm0120B1A13', 0),
    # Undead Burg (frame_20261104_133638): the far land under the cloud sea, 1.6-5 km across. As world geometry it stood in front of the clouds.
    ('m10_01_00_00', 'm8000B1A10', 0),
    ('m10_01_00_00', 'm2372B1A10', 1),
    # Duke's Archives (frame_20261103_163307): the far ground ring, then the land below the castle (mountains,
    # forest, lower walls). The game draws them before its cloud bowl and blends the clouds over them; as world
    # geometry they stood in front of the clouds. Their models have no sky layer.
    ('m17_00_00_00', 'm8000B0A17', 0),
    ('m17_00_00_00', 'm2300B0A17', 1),
    ('m17_00_00_00', 'm2300B0A17', 2),
    ('m17_00_00_00', 'm2300B0A17', 3),
}
CAMERA_ABOVE = {
    # The mountains' mesh also holds the ground around the castle's foot, up to y 267. From 425 standing places over
    # the map, rays that met these meshes with world geometry behind them: from below 267 up to 46% of directions
    # (y 130-160 and 220-250); from above it 2.6% at one place, 0.2% or less elsewhere. Below, they stay world geometry.
    ('m17_00_00_00', 'm2300B0A17', 1): 267.0,
    ('m17_00_00_00', 'm2300B0A17', 2): 267.0,
    ('m17_00_00_00', 'm2300B0A17', 3): 267.0,
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


def texture_opaque_share(data_dir, name):
    """Share of the diffuse's top-level texels at alpha >= 250 (map textures, DXT1/DXT5), None if unknown."""
    import gen_glass as G
    path = os.path.join(data_dir, 'map', name[:3], name + '.dds')
    dds = G.read_dds(path) if os.path.exists(path) else None
    if not dds:
        return None
    header, width, height, mips, fourcc, data = dds
    if fourcc == b'DXT5':
        return float((G.top_alpha(header, width, height, mips, data) >= 250).mean())
    if fourcc == b'DXT1':
        # Punch-through texels: blocks in 3-colour mode (c0 <= c1) using index 3.
        bw, bh = (width + 3) // 4, (height + 3) // 4
        blocks = np.frombuffer(data, np.uint8, bw * bh * 8).reshape(-1, 8)
        c0 = blocks[:, 0].astype(np.uint16) | (blocks[:, 1].astype(np.uint16) << 8)
        c1 = blocks[:, 2].astype(np.uint16) | (blocks[:, 3].astype(np.uint16) << 8)
        bits = (blocks[:, 4].astype(np.uint32) | (blocks[:, 5].astype(np.uint32) << 8)
                | (blocks[:, 6].astype(np.uint32) << 16) | (blocks[:, 7].astype(np.uint32) << 24))
        codes = np.stack([(bits >> (2 * t)) & 3 for t in range(16)], 1)
        clear = (c0 <= c1)[:, None] & (codes == 3)
        return float(1.0 - clear.mean())
    return None


def body_share(mesh):
    """Share of the mesh's vertices with alpha >= 0.9. Per vertex: a piece whose alpha steps 0/1 has
    no triangle averaging 0.9 when each touches a faded vertex (the m10_01_cliff_01 pieces)."""
    a = mesh_alpha(mesh)
    return 1.0 if a is None else float((np.asarray(a) >= 0.9).mean())


def solid_shell(map_id, mtd, name, mesh, on_surface, near_share, data_dir):
    """Whether a blended shell outside SHELL_MAPS is a solid terrain piece (see SOLID_*). Its texture must
    be one of its own area's: a border piece dressed in a neighbouring area's textures can lie on that
    area's ground, which this map's geometry does not show."""
    if (on_surface >= SOLID_ON_SURFACE or near_share >= SOLID_NEAR_SHARE or name[:3] != map_id[:3]
            or not SOLID_MTD.search(mtd.lower())):
        return False
    opaque = texture_opaque_share(data_dir, name)
    return opaque is not None and opaque >= SOLID_TEXTURE_OPAQUE and body_share(mesh) >= SOLID_BODY


def blended_shells(map_id, flvers, in_sky):
    """(model, mesh index, mesh, diffuse, fraction on a surface) of the map's blended shells.

    The game draws straight-alpha map meshes (MTD *_Alp) in two roles. Most are edge-fade strips lying
    on an opaque surface (bark over bark where two tree parts meet, a floor blend), which the proxy
    tags as decals for Remix to blend onto that surface. A shell lies off any surface: a giant tree's
    upper half fading into the mist, a sleeve standing 5-50 cm off a trunk. As a decal a shell took
    the lighting of whatever was behind it, or vanished where only sky was. A mesh is a shell when
    fewer than half of its visible vertices (alpha > 0.25, up to SHELL_SAMPLES) lie within
    SHELL_ON_SURFACE of an opaque triangle of the map (all map pieces at the map origin). Also returns
    the share within SOLID_NEAR, for solid_shell.
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
        reach = max(SHELL_ON_SURFACE, SOLID_NEAR)
        near = ((hi >= sample.min(0) - reach) & (lo <= sample.max(0) + reach)).all(1)
        t = tris[near]
        on = close = 0
        for q in sample:
            d = point_triangles_distance(q, t[:, 0], t[:, 1], t[:, 2]).min() if len(t) else np.inf
            on += d < SHELL_ON_SURFACE
            close += d < SOLID_NEAR
        on_surface = on / len(sample)
        if on_surface < 0.5:
            shells.append((model, i, mesh, name, on_surface, close / len(sample)))
    return shells


def gap_skirts(map_id, flvers, in_sky, skip):
    """(model, mesh index, mesh, diffuse, share over GAP_DEPTH) of the map's gap skirts (see GAP_SKIRT_MAPS): plain
    straight-alpha map meshes (SOLID_MTD, the map's own textures) not already listed (skip), at least GAP_SHARE of whose
    sample points inside faces with a visible vertex (alpha > 0.25) lie more than GAP_DEPTH straight above the first
    opaque surface below them (within 1 m; points over nothing are not counted)."""
    from gen_ground_cards import Ground
    opaque, cands = [], []
    for model, flver, names, sizes, layers in flvers:
        for i, mesh in enumerate(flver.meshes):
            mtd = os.path.basename(flver.materials[mesh.material_index].mtd).lower()
            if '_alp' in mtd:
                if ((map_id, model, i) not in in_sky and (model, i) not in skip and not ATMOSPHERIC_NAME.search(names[i])
                        and SOLID_MTD.search(mtd) and names[i][:3] == map_id[:3]):
                    cands.append((model, i, mesh, names[i]))
            elif '_add' not in mtd and 'water' not in mtd:
                opaque.append(mesh_triangles(mesh))
    if not cands or not opaque:
        return []
    ground = Ground(np.concatenate(opaque))
    rng = np.random.default_rng(0)
    out = []
    for model, i, mesh, name in cands:
        p = np.asarray(positions(mesh), dtype=np.float64)[:, :3]
        a = mesh_alpha(mesh)
        fs = mesh.face_sets[0]
        idx = np.asarray(fs.indices, dtype=np.int64)
        if fs.triangle_strip:
            if len(idx) < 3:
                continue
            t = np.stack([idx[:-2], idx[1:-1], idx[2:]], 1)
            t = t[(t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])]
        else:
            t = idx[: len(idx) // 3 * 3].reshape(-1, 3)
        t = t[((t >= 0) & (t < len(p))).all(1)]
        if a is not None:
            t = t[a[t].max(1) > 0.25]
        if not len(t):
            continue
        w = rng.dirichlet((1, 1, 1), size=(len(t), GAP_POINTS))
        q = (w[..., None] * p[t][:, None, :, :]).sum(2).reshape(-1, 3)
        down = np.array([np.nan if (d := ground.below(v)) is None else d for v in q])
        down = down[~np.isnan(down)]
        if len(down) >= GAP_MIN_POINTS and (share := float((down > GAP_DEPTH).mean())) >= GAP_SHARE:
            out.append((model, i, mesh, name, share, len(down)))
    return out


def strip_triangles(fs):
    """A face set's triangles as index triples in the winding Direct3D draws them (a strip's odd triangles are turned)."""
    idx = np.asarray(fs.indices, dtype=np.int64)
    if not fs.triangle_strip:
        return idx[: len(idx) // 3 * 3].reshape(-1, 3)
    if len(idx) < 3:
        return np.zeros((0, 3), dtype=np.int64)
    t = np.stack([idx[:-2], idx[1:-1], idx[2:]], 1)
    t[1::2] = t[1::2][:, [1, 0, 2]]
    return t[(t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])]


def skirt_backings(map_id, flvers, in_sky, skip):
    """(model, mesh index, mesh, diffuse, share over open air, [hanging triangles per face set]) of the map's backed skirts
    (see BACKING_OPEN_SHARE): gap skirt candidates (plain straight-alpha map meshes with the map's own textures, not in
    skip) between BACKING_OPEN_SHARE and BACKING_MAX_OPEN of whose points inside visible faces have no opaque surface within
    the ground's reach below them. A face hangs when a point of its BACKING_STEP lattice has no surface within GAP_DEPTH below it, the runtime's
    decal lift."""
    from gen_ground_cards import Ground
    opaque, cands = [], []
    for model, flver, names, sizes, layers in flvers:
        for i, mesh in enumerate(flver.meshes):
            mtd = os.path.basename(flver.materials[mesh.material_index].mtd).lower()
            if '_alp' in mtd:
                if ((map_id, model, i) not in in_sky and (model, i) not in skip and not ATMOSPHERIC_NAME.search(names[i])
                        and SOLID_MTD.search(mtd) and names[i][:3] == map_id[:3]):
                    cands.append((model, i, mesh, names[i]))
            elif '_add' not in mtd and 'water' not in mtd:
                opaque.append(mesh_triangles(mesh))
    if not cands or not opaque:
        return []
    ground = Ground(np.concatenate(opaque))
    out = []
    for model, i, mesh, name in cands:
        p = np.asarray(positions(mesh), dtype=np.float64)[:, :3]
        a = mesh_alpha(mesh)
        if a is None:
            continue
        def faces(fs):
            t = strip_triangles(fs)
            t = t[((t >= 0) & (t < len(p))).all(1)]
            return t[a[t].max(1) > 0.0]   # a face at alpha 0 throughout draws nothing

        t = faces(mesh.face_sets[0])
        t = t[a[t].max(1) > 0.25]
        # As many visible faces as a gap skirt needs (GAP_MIN_POINTS at GAP_POINTS a face): m0200B0A16 mesh3, six faces, is none.
        if len(t) * GAP_POINTS < GAP_MIN_POINTS:
            continue
        w = np.random.default_rng(0).dirichlet((1, 1, 1), size=(len(t), BACKING_POINTS))
        q = (w[..., None] * p[t][:, None, :, :]).sum(2).reshape(-1, 3)
        share = float(np.mean([ground.below(v) is None for v in q]))
        if share < BACKING_OPEN_SHARE or share > BACKING_MAX_OPEN:
            continue
        sets = []
        for fs in mesh.face_sets:
            t = faces(fs)
            hang = np.zeros(len(t), dtype=bool)
            for f, (va, vb, vc) in enumerate(p[t]):
                n = max(1, int(np.ceil(max(np.linalg.norm(vb - va), np.linalg.norm(vc - va), np.linalg.norm(vc - vb)) / BACKING_STEP)))
                for iu in range(n + 1):
                    for iv in range(n + 1 - iu):
                        d = ground.below(va + (vb - va) * (iu / n) + (vc - va) * (iv / n))
                        if d is None or d > GAP_DEPTH:
                            hang[f] = True
                            break
                    if hang[f]:
                        break
            sets.append(t[hang])
        out.append((model, i, mesh, name, share, sets))
    return out


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=r'<steam dir>\steamapps\common'
                                      r'\Dark Souls Prepare to Die Edition\DATA\unpacked')
    ap.add_argument('--bbformats', default=r'<other project>\tools')
    ap.add_argument('--out', default=os.path.join(here, '..', 'assets', 'mesh_tables', 'sky_meshes.txt'))
    args = ap.parse_args()

    sys.path.insert(0, args.bbformats)
    sys.path.insert(0, here)
    from bbformats.flver import read_flver

    lines = []
    shell_lines = []
    models = 0
    enclosures = 0
    in_sky = set()

    # Every map face set's key (index hash, count) -> its sources: the proxy matches a draw by its
    # indices alone, so a solid shell whose key another mesh shares would turn that mesh into a cutout
    # too (m12_01 m1780B1A12 mesh11 lod0 = m12_00 m1501B0A12 mesh22 lod0, a Darkroot edge strip).
    keys = {}
    solid = []
    backing_lines = []

    def add(map_id, model, i, mesh, name, out=lines):
        if out is lines:
            in_sky.add((map_id, model, i))
        for j, fs in enumerate(mesh.face_sets):
            idx = np.asarray(fs.indices, dtype='<u2')
            above = CAMERA_ABOVE.get((map_id, model, i))
            suffix = f' above {above:g}' if above is not None else ''
            out.append(f'{fnv1a64(idx.tobytes()):016x} {len(idx)} {map_id}/{model}/mesh{i}/lod{j} {name}{suffix}')

    for map_dir in sorted(glob.glob(os.path.join(args.data, 'map', 'm*_*'))):
        map_id = os.path.basename(map_dir)
        flvers = []
        for path in sorted(glob.glob(os.path.join(map_dir, '*.flver'))):
            flver = read_flver(open(path, 'rb').read())
            model_name = os.path.basename(path).rsplit('.', 1)[0]
            for i, mesh in enumerate(flver.meshes):
                for fs in mesh.face_sets:
                    idx = np.asarray(fs.indices, dtype='<u2')
                    keys.setdefault((fnv1a64(idx.tobytes()), len(idx)), set()).add(f'{map_id}/{model_name}/mesh{i}')
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
        shelled = set()      # (model, mesh index) listed here
        off_surface = set()  # (model, mesh index) of the meshes whose visible vertices mostly lie off any surface
        for model, i, mesh, name, on_surface, near_share in blended_shells(map_id, flvers, in_sky):
            off_surface.add((model, i))
            if map_id in SHELL_MAPS:
                print(f'shell {map_id}/{model}/mesh{i} {name}: {on_surface:.0%} on a surface')
                add(map_id, model, i, mesh, name, shell_lines)
                shelled.add((model, i))
                continue
            mtd = next(os.path.basename(f.materials[f.meshes[i].material_index].mtd)
                       for m, f, *_ in flvers if m == model)
            if solid_shell(map_id, mtd, name, mesh, on_surface, near_share, args.data):
                print(f'solid shell {map_id}/{model}/mesh{i} {name} ({mtd}): {on_surface:.0%} on a surface, '
                      f'{near_share:.0%} within {SOLID_NEAR:g} m')
                add(map_id, model, i, mesh, name, solid)
                shelled.add((model, i))

        for model, flver, names, sizes, layers in flvers:
            for i, mesh in enumerate(flver.meshes):
                if (map_id, model, i) in SKIRTS_AS_SURFACES and (model, i) not in shelled:
                    print(f'skirt as surface {map_id}/{model}/mesh{i} {names[i]}')
                    add(map_id, model, i, mesh, names[i], solid)
                    shelled.add((model, i))

        # Gap skirts: listed as shells marked 'gap' (checked for shared keys with the solid shells). A skirt's vertices lie on
        # surfaces, so the meshes off them (shells, listed or not) are not candidates.
        if map_id in GAP_SKIRT_MAPS:
            for model, i, mesh, name, share, counted in gap_skirts(map_id, flvers, in_sky, shelled | off_surface):
                print(f'gap skirt {map_id}/{model}/mesh{i} {name}: {share:.0%} of its {counted} face points over {GAP_DEPTH:g} m')
                start = len(solid)
                add(map_id, model, i, mesh, name, solid)
                solid[start:] = [l + ' gap' for l in solid[start:]]
            gaps = {(l.split()[2].split('/')[1], int(l.split()[2].split('/')[2][4:])) for l in solid if l.endswith(' gap')}
            for model, i, mesh, name, share, sets in skirt_backings(map_id, flvers, in_sky, shelled | off_surface | gaps):
                if (map_id, model, i) not in BACKING_CONFIRMED:
                    print(f'backed skirt candidate, not listed (in no capture): {map_id}/{model}/mesh{i} {name}: '
                          f'{share:.0%} of its face points over open air, {len(sets[0])} of its faces hang')
                    continue
                print(f'backed skirt {map_id}/{model}/mesh{i} {name}: {share:.0%} of its face points over open air, '
                      f'{len(sets[0])} of its faces hang')
                for j, (fs, tris) in enumerate(zip(mesh.face_sets, sets)):
                    if not len(tris):
                        continue
                    idx = np.asarray(fs.indices, dtype='<u2')
                    backing_lines.append(f'{fnv1a64(idx.tobytes()):016x} {len(idx)} {map_id}/{model}/mesh{i}/lod{j} {name} '
                                         f'{tris.size} ' + ' '.join(str(int(v)) for v in tris.reshape(-1)))

    for line in solid:
        key, count, source = line.split()[:3]
        owner = source.rsplit('/', 1)[0]
        others = keys.get((int(key, 16), int(count)), set()) - {owner}
        if others:
            what = 'gap skirt' if line.endswith(' gap') else 'solid shell'
            print(f'{what} {source} dropped: its indices are also {", ".join(sorted(others))}')
        else:
            shell_lines.append(line)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# Dark Souls sky model meshes: <index FNV-1a 64> <index count> <source> <diffuse>\n')
        f.write('# Generated by scripts/gen_sky_meshes.py; do not edit by hand.\n')
        f.writelines(line + '\n' for line in lines)
    print(f'{models} sky models, {enclosures} enclosure meshes, {len(lines)} face sets -> {os.path.abspath(args.out)}')

    shells_out = os.path.join(os.path.dirname(os.path.abspath(args.out)), 'shell_meshes.txt')
    with open(shells_out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# Dark Souls blended shells (alpha-faded meshes off any surface): '
                '<index FNV-1a 64> <index count> <source> <diffuse> [gap]\n')
        f.write('# Generated by scripts/gen_sky_meshes.py; do not edit by hand.\n')
        f.writelines(line + '\n' for line in shell_lines)
    print(f'{len(shell_lines)} blended shell face sets -> {shells_out}')

    # Backed skirts: a face set another mesh shares, or one listed as a shell or sky mesh, is left out (the proxy knows a
    # draw by its indices alone). Two face sets of one mesh with the same indices give one line.
    listed = {tuple(l.split()[:2]) for l in shell_lines + lines}
    kept, seen = [], set()
    for line in backing_lines:
        key, count, source = line.split()[:3]
        others = keys.get((int(key, 16), int(count)), set()) - {source.rsplit('/', 1)[0]}
        if others or (key, count) in listed:
            print(f'backed skirt {source} dropped: its indices are also {", ".join(sorted(others)) or "a listed shell or sky mesh"}')
        elif (key, count) not in seen:
            seen.add((key, count))
            kept.append(line)
    backing_out = os.path.join(os.path.dirname(os.path.abspath(args.out)), 'skirt_backing.txt')
    with open(backing_out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# Dark Souls backed skirts (blended skirts whose faces hang over open air): <index FNV-1a 64> <index count> '
                '<source> <diffuse> <n> <n indices: the hanging faces as a triangle list>\n')
        f.write('# Generated by scripts/gen_sky_meshes.py; do not edit by hand.\n')
        f.writelines(line + '\n' for line in kept)
    print(f'{len(kept)} backed skirt face sets -> {backing_out}')


if __name__ == '__main__':
    main()
