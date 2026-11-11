"""How high each map flame effect's fire stands over its origin, from every capture: the light-weighted mean height of the additive
effect sprites drawn within reach of a placed flame (assets/ffx/map_flames.tsv gives the places and effects per map).

    python scripts/gen_flame_heights.py [capture ...]      writes assets/ffx/flame_heights.tsv

Per effect: the median over all sightings of the centre height, of the height under which a quarter / three quarters of the fire's
light is, and of its horizontal spread. A sighting needs 4 sprites. The centre bounds how far a light may be raised and still stand
in its fire (scripts/gen_flame_lift.py). Sprites are taken by place (within 0.6 m of the placed flame), so captures of any age do.
"""
import os
import struct
import sys
from collections import defaultdict
from multiprocessing import Pool

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "lookdev"))
import analyze  # noqa: E402

OUT = os.path.join(HERE, "..", "assets", "ffx", "flame_heights.tsv")
FLAMES = defaultdict(list)
for line in open(os.path.join(HERE, "..", "assets", "ffx", "map_flames.tsv"), encoding="utf-8"):
    if line.startswith("#") or not line.strip():
        continue
    m, ffx, x, y, z = line.split("\t")
    FLAMES[m].append((int(ffx), float(x), float(y), float(z)))
RADIUS = 0.6


def scan(cap):
    out = []
    try:
        names, decls, rs, tex = {}, {}, {}, {}
        decl, flames, P, W = 0, None, [], []
        for t, p in analyze.records(analyze.resolve(cap)):
            if t == 130:
                if flames is None:
                    map_id = struct.unpack_from("<I3f3ffI", p)[8]
                    flames = FLAMES.get(f"m{map_id >> 24:02d}_{(map_id >> 16) & 255:02d}_00_00", [])
                    if not flames:
                        return cap, out
            elif t == 132:
                names[struct.unpack_from("<Q", p)[0]] = bytes(p[8:]).decode(errors="replace")
            elif t == 11:
                did = struct.unpack_from("<Q", p)[0]
                pos = col = None
                for i in range(8, len(p) - 7, 8):
                    stream, off, typ, meth, usage, uidx = struct.unpack_from("<HHBBBB", p, i)
                    if stream == 0xFF:
                        break
                    if stream == 0 and usage == 0 and typ in (2, 3):
                        pos = off
                    elif stream == 0 and usage == 10 and uidx == 0 and typ == 4:
                        col = off
                decls[did] = (pos, col)
            elif t == 64:
                s, v = struct.unpack_from("<II", p)
                rs[s] = v
            elif t == 67:
                slot, tid = struct.unpack_from("<IQ", p)
                tex[slot] = tid
            elif t == 70:
                decl = struct.unpack_from("<Q", p)[0]
            elif t in (98, 99):
                if not (rs.get(27) and rs.get(20) == 2) or not names.get(tex.get(0), "").startswith("s"):
                    continue
                pos, col = decls.get(decl, (None, None))
                if pos is None or col is None:
                    continue
                if t == 98:
                    prim, count, stride, vbytes = struct.unpack_from("<4I", p)
                    vb = bytes(p[16:16 + vbytes])
                else:
                    prim, mn, nv, count, ifmt, stride, ib, vbytes = struct.unpack_from("<8I", p)
                    vb = bytes(p[32 + ib:32 + ib + vbytes])
                n = len(vb) // stride if stride else 0
                if n < 4 or n % 4:
                    continue
                raw = np.frombuffer(vb[:n * stride], np.uint8).reshape(n, stride)
                xyz = raw[:, pos:pos + 12].copy().view(np.float32).reshape(n, 3).astype(np.float64)
                bgra = raw[:, col:col + 4].astype(np.float64) / 255.0
                q = xyz.reshape(-1, 4, 3)
                c = bgra.reshape(-1, 4, 4).mean(1)
                area = np.linalg.norm(np.cross(q[:, 1] - q[:, 0], q[:, 3] - q[:, 0]), axis=1)
                P.append(q.mean(1))
                W.append(area * c[:, 3] * c[:, :3].max(1))
        if not P or not flames:
            return cap, out
        P, W = np.concatenate(P), np.concatenate(W)
        ok = np.isfinite(P).all(1) & np.isfinite(W) & (W > 0)
        P, W = P[ok], W[ok]
        for ffx, x, y, z in flames:
            near = (np.hypot(P[:, 0] - x, P[:, 2] - z) <= RADIUS) & (P[:, 1] > y - 0.3) & (P[:, 1] < y + 3.0)
            if near.sum() < 4:
                continue
            h, w = P[near, 1] - y, W[near]
            order = np.argsort(h)
            cum = np.cumsum(w[order]) / w.sum()
            centre = float((h * w).sum() / w.sum())
            spread = float(np.sqrt((((P[near, 0] - x) ** 2 + (P[near, 2] - z) ** 2) * w).sum() / w.sum()))
            out.append((ffx, centre, float(h[order][np.searchsorted(cum, 0.25)]), float(h[order][np.searchsorted(cum, 0.75)]), spread, int(near.sum())))
    except Exception as e:  # noqa: BLE001
        return cap, [("error", str(e))]
    return cap, out


if __name__ == "__main__":
    caps = sys.argv[1:] or sorted(p.stem for p in analyze.CAPTURE_DIR.glob("*.ds1cap"))
    per = defaultdict(list)
    with Pool(6) as pool:
        for cap, rows in pool.imap_unordered(scan, caps):
            for r in rows:
                if r[0] == "error":
                    print("error", cap, r[1], file=sys.stderr)
                else:
                    per[r[0]].append(r[1:])
    tab, nl = chr(9), chr(10)
    with open(OUT, "w", encoding="utf-8", newline=nl) as f:
        f.write("# FFX id <TAB> sightings <TAB> centre m <TAB> quarter m <TAB> three quarters m <TAB> horizontal spread m <TAB> centre min..max. "
                "Height of a map flame's fire over its origin, median over captures. Generated by scripts/gen_flame_heights.py." + nl)
        for ffx in sorted(per):
            a = np.array([r[:4] for r in per[ffx]])
            med = np.median(a, 0)
            f.write(tab.join([str(ffx), str(len(a)), f"{med[0]:.2f}", f"{med[1]:.2f}", f"{med[2]:.2f}", f"{med[3]:.2f}",
                              f"{a[:, 0].min():.2f}..{a[:, 0].max():.2f}"]) + nl)
    print(f"{len(per)} effects -> {os.path.abspath(OUT)}")
