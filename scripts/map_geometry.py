"""The map's geometry for the light generators: its pieces out of the MSB and FLVERs, and rays against them.

Needs embreex (pip install embreex).
"""
import math
import struct

import numpy as np


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
