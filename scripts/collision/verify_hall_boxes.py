"""Checks the library-hall boxes (from cover_hall_boxes.main) with the proxy's containment: own floor left out (where), other
floors inside (navmesh and collision), the balcony points and the east corridor junction the review named."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import cover_hall_boxes as C

def samples(v, f, per_m2=16):
    rng = np.random.default_rng(1)
    out = []
    for t in v[f]:
        a = 0.5 * abs(np.cross(t[1] - t[0], t[2] - t[0])[1])
        k = max(1, int(a * per_m2))
        r1, r2 = rng.random(k), rng.random(k)
        s = np.sqrt(r1)
        out.append((1 - s)[:, None] * t[0] + (s * (1 - r2))[:, None] * t[1] + (s * r2)[:, None] * t[2])
    return np.concatenate(out) if out else np.zeros((0, 3))

def contains(p, b):
    b = np.array(b); lo, hi = np.minimum(b[:3], b[3:]), np.maximum(b[:3], b[3:])
    c, h = (lo + hi) / 2, (hi - lo) / 2
    d = p - c; r = np.radians(C.YAW); cs, sn = np.cos(r), np.sin(r)
    x = d[:, 0] * cs + d[:, 2] * sn; z = -d[:, 0] * sn + d[:, 2] * cs
    return (np.abs(x) <= h[0]) & (np.abs(d[:, 1]) <= h[1]) & (np.abs(z) <= h[2])

import hall_boxes_final as H
boxes = [H.world_box(r) for r in H.RECTS.values()]
def inany(p):
    k = np.zeros(len(p), bool)
    for b in boxes: k |= contains(p, b)
    return k
import glob, os
nav = {os.path.basename(p)[:7]: C.load(p) for p in glob.glob(os.path.join(C.COL, "n*.obj"))}
own = sum(C.GROUPS.values(), [])
print("\nown floor left out (16 samples/m2, feet + 0.05):")
for k in own:
    v, f = nav[k]; s = samples(v, f) + [0, 0.05, 0]
    m = ~inany(s)
    if m.any():
        q = s[m]
        print(f"  {k}: {m.sum()/16:.1f} m2 of {len(s)/16:.0f}, x {q[:,0].min():.1f}..{q[:,0].max():.1f} y {q[:,1].min():.1f}..{q[:,1].max():.1f} z {q[:,2].min():.1f}..{q[:,2].max():.1f}")
print("\nother navmesh floor inside:")
for k, (v, f) in sorted(nav.items()):
    if k in own or not len(f): continue
    s = samples(v, f) + [0, 0.05, 0]; m = inany(s)
    if m.any(): print(f"  {k}: {m.sum()/16:.1f} m2")
print("\nballony points (302,289,512) (304,289,514):", inany(np.array([[302, 288.7, 512], [304, 288.7, 514]])))
print("east junction n0090/1/2 inside (m2):", {k: round(inany(samples(*nav[k]) + [0, .05, 0]).sum() / 16, 1) for k in ("n0090B0", "n0091B0", "n0092B0")})
col = {os.path.basename(p)[:7]: C.load(p) for p in glob.glob(os.path.join(C.COL, "h*.obj"))}
for k in ("h0091B0", "h0092B0", "h0049B0", "h0041B0"):
    if k not in col: continue
    v, f = col[k]; t3 = v[f]; nn = np.cross(t3[:,1]-t3[:,0], t3[:,2]-t3[:,0]); nn /= np.maximum(np.linalg.norm(nn,axis=1,keepdims=True),1e-9)
    s = samples(v, f[np.abs(nn[:,1]) > 0.7]) + [0, .05, 0]
    print(f"collision floor {k} inside: {inany(s).sum()/16:.1f} m2")
# overlap where hall 1 meets hall 2 along the walkways (x+z ~ 858): any gap?
pts = np.concatenate([samples(*nav[k]) for k in ("n0006B0","n0007B0","n0008B0","n0016B0","n0017B0","n0018B0","n0003B0")]) + [0, .05, 0]
sel = np.abs(pts[:, 0] + pts[:, 2] - 858.0) < 2.0
print("walkway seam samples:", sel.sum(), "outside every box:", (~inany(pts[sel])).sum())
