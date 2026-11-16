"""Second test for the m14_00_00_00 custom lights, against the map's visible geometry (its opaque map piece FLVERs, loaded as
scripts/gen_domain_lights.py does): 256 rays per light;
  vis_enclosed  share of rays that hit a visible surface within 200 m
  vis_front     share of those hits on a surface facing the light (by the mesh's own vertex normals): a light inside a room
                sees the room's faces; one behind a wall or inside a rock sees their backs
  vis_near      nearest hit
Usage: py -3.12 -P domain_visual_check.py <lights.txt>"""
import os  # noqa: E402  (ds1paths: the local folders, see paths.example.ini)
import sys  # noqa: E402
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")))
import ds1paths  # noqa: E402

import os
import sys

import numpy as np

sys.path.insert(0, ds1paths.SCRIPTS)
from map_geometry import blocks_rays, first, map_pieces, rotation, table_sources  # noqa: E402
from bbformats.flver import read_flver  # noqa: E402
from embreex import rtcore_scene as rtcs  # noqa: E402
from embreex.mesh_construction import TriangleMesh  # noqa: E402

DATA = ds1paths.GAME + r"\DATA\unpacked"
REPO = ds1paths.REPO
MAP = "m14_00_00_00"


def fibonacci_dirs(n):
    i = np.arange(n) + 0.5
    phi = np.arccos(1 - 2 * i / n)
    theta = np.pi * (1 + 5 ** 0.5) * i
    return np.stack([np.cos(theta) * np.sin(phi), np.cos(phi), np.sin(theta) * np.sin(phi)], 1)


def main():
    tables = os.path.join(REPO, "assets", "mesh_tables")
    no_block = table_sources(os.path.join(tables, "sky_meshes.txt")) | table_sources(os.path.join(tables, "glass_meshes.txt"))
    flvers, tris, tri_normals = {}, [], []
    for model, at, rot, scale, _ in map_pieces(os.path.join(DATA, "map", "MapStudio", f"{MAP}.msb")):
        path = os.path.join(DATA, "map", MAP, f"{model}A14.flver")
        if not os.path.exists(path):
            continue
        if model not in flvers:
            flvers[model] = read_flver(open(path, "rb").read())
        flver = flvers[model]
        r = rotation(rot) * scale
        for i, mesh in enumerate(flver.meshes):
            fs = mesh.lod0()
            if fs is None or "position" not in mesh.vertices:
                continue
            p = np.asarray(first(mesh.vertices["position"]), np.float64)[:, :3]
            t = fs.triangles()
            t = t[(t < len(p)).all(1)]
            if not len(t):
                continue
            if f"{MAP}/{model}A14/mesh{i}/lod0" in no_block or not blocks_rays(str(flver.materials[mesh.material_index].mtd)):
                continue
            world = p @ r.T + at
            if "normal" in mesh.vertices:
                n = np.asarray(first(mesh.vertices["normal"]), np.float64)[:, :3] @ r.T
                tn = n[t].sum(1)
            else:
                w = world[t]
                tn = np.cross(w[:, 1] - w[:, 0], w[:, 2] - w[:, 0])
            tn /= np.maximum(np.linalg.norm(tn, axis=1, keepdims=True), 1e-12)
            tris.append(world[t])
            tri_normals.append(tn)
    T, N = np.concatenate(tris), np.concatenate(tri_normals)
    scene = rtcs.EmbreeScene()
    TriangleMesh(scene, T.astype(np.float32))
    print(f"{len(T)} visible opaque triangles", file=sys.stderr)
    dirs = fibonacci_dirs(256)
    print("name,vis_enclosed,vis_front,vis_near,nearest_front,down,up,front_within3m,back_within3m")
    for line in open(sys.argv[1], encoding="utf-8"):
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        f = s.split()
        p = np.array([float(f[0]), float(f[1]), float(f[2])])
        res = scene.run(np.repeat(p[None], len(dirs), 0).astype(np.float32), dirs.astype(np.float32), output=1)
        prim, tfar = np.asarray(res["primID"]), np.asarray(res["tfar"])
        hit = (prim >= 0) & (tfar < 200)
        hp = prim[hit]
        front = np.mean((N[hp] * -dirs[hit]).sum(1) > 0) if hit.any() else 0.0
        faces = (N[np.maximum(prim, 0)] * -dirs).sum(1) > 0
        nearest = np.argmin(np.where(hit, tfar, np.inf))
        def probe(d):
            r = scene.run(p[None].astype(np.float32), np.array([d], np.float32), output=1)
            pr, tf = int(np.asarray(r["primID"])[0]), float(np.asarray(r["tfar"])[0])
            if pr < 0 or tf > 200:
                return "open"
            return f"{'front' if (N[pr] * -np.array(d)).sum() > 0 else 'BACK'}@{tf:.1f}"
        close = hit & (tfar < 3.0)
        print(f"{f[9]},{hit.mean():.2f},{front:.2f},{tfar[hit].min() if hit.any() else np.inf:.2f},"
              f"{'front' if faces[nearest] else 'BACK'},{probe((0, -1, 0))},{probe((0, 1, 0))},{(close & faces).sum()},{(close & ~faces).sum()}")


if __name__ == "__main__":
    main()
