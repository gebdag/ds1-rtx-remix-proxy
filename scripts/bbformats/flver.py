"""FLVER2 model reader (Bloodborne uses version 0x20013 / 0x20014)."""
import struct
from dataclasses import dataclass, field

import numpy as np

from .containers import dcx_decompress, utf16z

SEMANTICS = {0: 'position', 1: 'bone_weights', 2: 'bone_indices', 3: 'normal',
             5: 'uv', 6: 'tangent', 7: 'bitangent', 10: 'color'}

TYPE_SIZE = {0x01: 8, 0x02: 12, 0x03: 16, 0x10: 4, 0x11: 4, 0x12: 4, 0x13: 4,
             0x15: 4, 0x16: 8, 0x18: 8, 0x1A: 8, 0x2E: 8, 0x2F: 4, 0xF0: 1}

FACESET_LOD1 = 0x01000000
FACESET_LOD2 = 0x02000000
FACESET_MOTION_BLUR = 0x80000000


@dataclass
class Dummy:
    position: tuple
    forward: tuple
    upward: tuple
    reference_id: int
    parent_bone: int
    attach_bone: int


@dataclass
class Texture:
    type: str
    path: str
    scale: tuple
    raw: bytes


@dataclass
class Material:
    name: str
    mtd: str
    flags: int
    gx_offset: int
    textures: list = field(default_factory=list)


@dataclass
class Bone:
    name: str
    parent: int
    child: int
    next_sibling: int
    prev_sibling: int
    translation: tuple
    rotation: tuple  # euler radians
    scale: tuple
    bbox_min: tuple
    bbox_max: tuple


@dataclass
class Member:
    unk00: int
    struct_offset: int
    type: int
    semantic: int
    index: int


@dataclass
class VertexBuffer:
    buffer_index: int
    layout_index: int
    vertex_size: int
    vertex_count: int
    offset: int


@dataclass
class FaceSet:
    flags: int
    triangle_strip: bool
    cull_backfaces: bool
    index_size: int
    indices: np.ndarray

    def triangles(self):
        """(N,3) triangle list; strips are unrolled, degenerates/restarts dropped."""
        idx = self.indices.astype(np.int64)
        restart = 0xFFFF if self.index_size == 16 else 0xFFFFFFFF
        if not self.triangle_strip:
            tri = idx[: len(idx) // 3 * 3].reshape(-1, 3)
            return tri[(tri != restart).all(1)]
        if len(idx) < 3:
            return np.zeros((0, 3), np.int64)
        a, b, c = idx[:-2], idx[1:-1], idx[2:]
        flip = (np.arange(len(a)) & 1).astype(bool)
        tri = np.stack([np.where(flip, c, a), b, np.where(flip, a, c)], 1)
        ok = (a != b) & (b != c) & (a != c) & (tri != restart).all(1)
        return tri[ok]


@dataclass
class Mesh:
    dynamic: bool
    material_index: int
    default_bone: int
    bone_indices: list
    bbox_min: tuple
    bbox_max: tuple
    face_sets: list
    vertex_buffers: list
    vertices: dict = field(default_factory=dict)  # semantic name -> ndarray / list of ndarray

    @property
    def vertex_count(self):
        return self.vertex_buffers[0].vertex_count if self.vertex_buffers else 0

    def lod0(self):
        for fs in self.face_sets:
            if fs.flags == 0:
                return fs
        return self.face_sets[0] if self.face_sets else None


@dataclass
class Flver:
    version: int
    bbox_min: tuple
    bbox_max: tuple
    header_vertex_index_size: int
    unicode: bool
    dummies: list
    materials: list
    bones: list
    meshes: list
    layouts: list
    warnings: list


def _vec(b, off, n, e):
    return struct.unpack_from(f'{e}{n}f', b, off)


def _decode_member(chunk, m, uv_factor, e):
    """chunk: (N, size) uint8. Returns list of arrays (UVPair yields two) or None if unsupported."""
    t, s = m.type, m.semantic
    n = chunk.shape[0]

    def as_(dt, cnt):
        return np.frombuffer(chunk.tobytes(), np.dtype(dt).newbyteorder(e)).reshape(n, cnt)

    if t in (0x01, 0x02, 0x03):
        v = as_('f4', {0x01: 2, 0x02: 3, 0x03: 4}[t])
        if s == 0:
            return [v[:, :3]]
        if s == 5 and t == 0x03:
            return [v[:, :2], v[:, 2:]]
        return [v]

    if s == 0:
        return None
    if s == 1:  # bone weights
        if t == 0x10:
            return [as_('i1', 4) / 127.0]
        if t == 0x13:
            return [as_('u1', 4) / 255.0]
        if t in (0x15, 0x1A):
            return [as_('i2', 4) / 32767.0]
        return None
    if s == 2:  # bone indices
        if t in (0x11, 0x2F, 0x13):
            return [as_('u1', 4).astype(np.int32)]
        if t == 0x18:
            return [as_('u2', 4).astype(np.int32)]
        return None
    if s in (3, 6, 7):  # normal / tangent / bitangent
        if t in (0x10, 0x11, 0x12, 0x13, 0x2F):
            return [(as_('u1', 4).astype(np.float32) - 127.0) / 127.0]
        if t == 0x1A:
            return [as_('i2', 4) / 32767.0]
        if t == 0x2E:
            return [(as_('u2', 4).astype(np.float32) - 32767.0) / 32767.0]
        return None
    if s == 5:  # uv
        if t in (0x10, 0x11, 0x12, 0x13, 0x15):
            return [as_('i2', 2) / uv_factor]
        if t == 0x16:
            v = as_('i2', 4) / uv_factor
            return [v[:, :2], v[:, 2:]]
        if t == 0x2E:
            return [as_('i2', 4)[:, :3] / uv_factor]
        return None
    if s == 10:  # color
        if t in (0x10, 0x13):
            return [as_('u1', 4) / 255.0]
        return None
    return None


def read_flver(data, decode_vertices=True):
    b = dcx_decompress(data)
    if b[:6] != b'FLVER\0':
        raise ValueError('not a FLVER')
    e = '>' if b[6:8] == b'B\0' else '<'
    warnings = []
    ver, data_off, _data_size, n_dummy, n_mat, n_bone, n_mesh, n_vb = struct.unpack_from(e + '8i', b, 8)
    bbmin, bbmax = _vec(b, 0x28, 3, e), _vec(b, 0x34, 3, e)
    header_vis = b[0x48]
    unicode_ = b[0x49] != 0
    n_fs, n_layout, n_tex = struct.unpack_from(e + '3i', b, 0x50)
    if not unicode_:
        warnings.append('non-unicode strings')
    string = utf16z

    pos = 0x80
    dummies = []
    for _ in range(n_dummy):
        dummies.append(Dummy(_vec(b, pos, 3, e), _vec(b, pos + 0x10, 3, e), _vec(b, pos + 0x20, 3, e),
                             *struct.unpack_from(e + 'hh', b, pos + 0x1C),
                             struct.unpack_from(e + 'h', b, pos + 0x2C)[0]))
        pos += 0x40

    mat_raw = []
    for _ in range(n_mat):
        mat_raw.append(struct.unpack_from(e + '8i', b, pos))
        pos += 0x20

    bones = []
    for _ in range(n_bone):
        name_off, = struct.unpack_from(e + 'I', b, pos + 0x0C)
        parent, child = struct.unpack_from(e + 'hh', b, pos + 0x1C)
        nxt, prv = struct.unpack_from(e + 'hh', b, pos + 0x2C)
        bones.append(Bone(string(b, name_off), parent, child, nxt, prv,
                          _vec(b, pos, 3, e), _vec(b, pos + 0x10, 3, e), _vec(b, pos + 0x20, 3, e),
                          _vec(b, pos + 0x30, 3, e), _vec(b, pos + 0x40, 3, e)))
        pos += 0x80

    mesh_raw = []
    for _ in range(n_mesh):
        mesh_raw.append(struct.unpack_from(e + '12i', b, pos))
        pos += 0x30

    face_sets = []
    for _ in range(n_fs):
        flags, strip, cull, _u6, _u7, count, ioff, _ilen, _z, isize, _z2 = struct.unpack_from(e + 'IBBBBiiiiii', b, pos)
        size = isize or header_vis or 16
        dt = np.dtype('u2' if size == 16 else 'u4').newbyteorder(e)
        idx = np.frombuffer(b, dt, count, data_off + ioff)
        face_sets.append(FaceSet(flags, bool(strip), bool(cull), size, idx))
        pos += 0x20

    vbufs = []
    for _ in range(n_vb):
        bi, li, vs, vc, _z0, _z1, _length, off = struct.unpack_from(e + '8i', b, pos)
        vbufs.append(VertexBuffer(bi, li, vs, vc, off))
        pos += 0x20

    layouts = []
    for _ in range(n_layout):
        count, _z0, _z1, moff = struct.unpack_from(e + '4i', b, pos)
        layouts.append([Member(*struct.unpack_from(e + '5i', b, moff + i * 0x14)) for i in range(count)])
        pos += 0x10

    textures = []
    for _ in range(n_tex):
        path_off, type_off, sx, sy = struct.unpack_from(e + 'IIff', b, pos)
        textures.append(Texture(string(b, type_off), string(b, path_off), (sx, sy), bytes(b[pos + 0x10:pos + 0x20])))
        pos += 0x20

    materials = []
    for name_off, mtd_off, tcount, tindex, flags, gx_off, _u18, _z in mat_raw:
        materials.append(Material(string(b, name_off), string(b, mtd_off), flags, gx_off,
                                  textures[tindex:tindex + tcount]))

    uv_factor = 2048.0 if ver >= 0x2000F else 1024.0
    meshes = []
    for i, (dyn, mat, z0, z1, defbone, bcount, bboff, boff, fscount, fsoff, vbcount, vboff) in enumerate(mesh_raw):
        if z0 or z1:
            warnings.append(f'mesh {i}: nonzero reserved {z0} {z1}')
        bone_idx = list(struct.unpack_from(f'{e}{bcount}i', b, boff)) if bcount else []
        mbmin = _vec(b, bboff, 3, e) if bboff else None
        mbmax = _vec(b, bboff + 12, 3, e) if bboff else None
        fsi = struct.unpack_from(f'{e}{fscount}i', b, fsoff)
        vbi = struct.unpack_from(f'{e}{vbcount}i', b, vboff)
        mesh = Mesh(bool(dyn & 0xFF), mat, defbone, bone_idx, mbmin, mbmax,
                    [face_sets[j] for j in fsi], [vbufs[j] for j in vbi])
        if decode_vertices:
            for vb in mesh.vertex_buffers:
                layout = layouts[vb.layout_index]
                raw = np.frombuffer(b, np.uint8, vb.vertex_count * vb.vertex_size,
                                    data_off + vb.offset).reshape(vb.vertex_count, vb.vertex_size)
                for m in layout:
                    size = TYPE_SIZE.get(m.type)
                    if size is None:
                        warnings.append(f'unknown member type {m.type:#x} sem {m.semantic}')
                        continue
                    dec = _decode_member(raw[:, m.struct_offset:m.struct_offset + size], m, uv_factor, e)
                    if dec is None:
                        warnings.append(f'unsupported type {m.type:#x} for semantic {m.semantic}')
                        continue
                    key = SEMANTICS.get(m.semantic, f'sem{m.semantic}')
                    if key in ('uv', 'tangent', 'bitangent', 'color'):
                        mesh.vertices.setdefault(key, []).extend(dec)
                    else:
                        mesh.vertices[key] = dec[0]
        meshes.append(mesh)

    return Flver(ver, bbmin, bbmax, header_vis, unicode_, dummies, materials, bones, meshes, layouts, warnings)


def write_obj(flver, path, lod_flags=0):
    """Debug export: LOD0 triangles, positions/normals/uv0, one group per mesh."""
    with open(path, 'w') as f:
        base = 1
        for mi, mesh in enumerate(flver.meshes):
            v = mesh.vertices
            pos = v['position']
            f.write(f'o mesh{mi}_{flver.materials[mesh.material_index].name}\n')
            np.savetxt(f, pos, fmt='v %.5f %.5f %.5f')
            has_uv = bool(v.get('uv'))
            has_n = 'normal' in v
            if has_uv:
                uv = v['uv'][0].copy()
                uv[:, 1] = 1.0 - uv[:, 1]
                np.savetxt(f, uv, fmt='vt %.5f %.5f')
            if has_n:
                np.savetxt(f, v['normal'][:, :3], fmt='vn %.4f %.4f %.4f')
            fs = next((s for s in mesh.face_sets if s.flags == lod_flags), None)
            if fs is None:
                base += len(pos)
                continue
            tri = fs.triangles() + base
            if has_uv and has_n:
                rows = np.repeat(tri, 3, axis=1).reshape(-1, 9)
                np.savetxt(f, rows, fmt='f %d/%d/%d %d/%d/%d %d/%d/%d')
            else:
                np.savetxt(f, tri, fmt='f %d %d %d')
            base += len(pos)
