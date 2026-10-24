"""Vertices of one draw in a lookdev capture, in world space (bone 0) with their UVs.

  python scripts/swamp_mesh.py <capture> <draw index>
"""
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "lookdev"))
import analyze as A  # noqa: E402


def mesh_vertices(capture, want):
    """(world xyz, uv) of the vertices draw `want` uses; uv = the SHORT2 texcoord / 1024."""
    cap = A.resolve(capture)
    buffers, decls = {}, {}
    stream0 = indices = None
    decl = 0
    n = 0
    vs_f = {}
    for t, p in A.records(cap):
        if t in (7, 8):
            bid, size = struct.unpack_from("<QI", p)
            buffers[bid] = bytearray(size)
        elif t == 34:
            bid, flags, off, size = struct.unpack_from("<QIII", p)
            if bid in buffers:
                buffers[bid][off:off + size] = bytes(p[20:20 + size])
        elif t == 78:
            s, sid, off, stride = struct.unpack_from("<IQII", p)
            if s == 0:
                stream0 = (sid, off, stride)
        elif t == 80:
            indices = struct.unpack_from("<Q", p)[0]
        elif t == 11:
            did = struct.unpack_from("<Q", p)[0]
            els = []
            for i in range(8, len(p) - 7, 8):
                s_, off, typ, meth, usage, uidx = struct.unpack_from("<HHBBBB", p, i)
                if s_ == 0xFF:
                    break
                els.append((off, typ, usage))
            decls[did] = els
        elif t == 70:
            decl = struct.unpack_from("<Q", p)[0]
        elif t == 72:
            start, count = struct.unpack_from("<II", p)
            vals = struct.unpack_from(f"<{count * 4}f", p, 8)
            for r in range(count):
                vs_f[start + r] = vals[r * 4:r * 4 + 4]
        elif t in (96, 97, 98, 99):
            idx = n
            n += 1
            if idx != want:
                continue
            prim, base, mn, nv, si, cnt = struct.unpack_from("<IiIIII", p)
            usages = {u: (off, typ) for off, typ, u in decls[decl]}
            sid, soff, stride = stream0
            vb = bytes(buffers[sid])
            ib = np.frombuffer(bytes(buffers[indices]), np.uint16)
            n_idx = cnt + 2 if prim == 5 else cnt * 3
            used = np.unique(ib[si:si + n_idx]).astype(np.int64) + base
            pos, uv = [], []
            for v in used:
                o = soff + v * stride
                pos.append(struct.unpack_from("<3f", vb, o + usages[0][0]))
                uv.append([x / 1024.0 for x in struct.unpack_from("<hh", vb, o + usages[5][0])])
            m = np.array([vs_f[8], vs_f[9], vs_f[10]], dtype=np.float64)
            local = np.array(pos, dtype=np.float64)
            world = local @ m[:, :3].T + m[:, 3]
            return world, np.array(uv, dtype=np.float64)
    sys.exit(f"{capture}: no draw {want}")


if __name__ == "__main__":
    w, uv = mesh_vertices(sys.argv[1], int(sys.argv[2]))
    print(f"{len(w)} vertices; world min {w.min(0).round(2)} max {w.max(0).round(2)}; uv min {uv.min(0).round(2)} max {uv.max(0).round(2)}")
