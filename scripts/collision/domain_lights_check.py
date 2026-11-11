"""Classifies the custom lights of m14_00_00_00 against the map's collision meshes (exported by colexport to col_m14\\*.obj)
and the collision parts' place names from the MSB (1401 Quelaag's Domain, 1400 Blighttown, -1 none).

Per light, 256 rays in all directions:
  enclosed  share of rays that hit something within 200 m (the caves are closed; the swamp's mound is open to the sky)
  domain    share of the hits that land on Quelaag's Domain collision parts (place name +-1401)
  backface  share of the hits that see a surface from behind (a light inside a rock or behind a wall)
  near      the nearest hit distance (a light closer than its own radius to a wall clips into it)
Usage: py -3.12 domain_lights_check.py <lights.txt> [--csv out.csv]"""
import struct
import sys
from pathlib import Path

import numpy as np
from embreex import rtcore_scene as rtcs
from embreex.mesh_construction import TriangleMesh

HERE = Path(__file__).parent
MSB = Path(r"<game dir>\DATA\unpacked\map\MapStudio\m14_00_00_00.msb")
sys.path.insert(0, r"<projects>\ds1-comp-proxy-nv\scripts")
from map_geometry import _section, _sjis  # noqa: E402  (scripts/)


def place_names():
    data = MSB.read_bytes()
    model_offsets, pos = _section(data, 0)
    models = [_sjis(data, o + struct.unpack_from("<i", data, o)[0]) for o in model_offsets]
    _, pos = _section(data, pos)
    _, pos = _section(data, pos)
    part_offsets, _ = _section(data, pos)
    out = {}
    for o in part_offsets:
        _, kind, _, model = struct.unpack_from("<iIii", data, o)
        if kind == 5:
            t = o + struct.unpack_from("<i", data, o + 0x5C)[0]
            out[models[model]] = struct.unpack_from("<h", data, t + 0x24)[0]
    return out


def load_obj(path):
    v, f = [], []
    for line in open(path):
        if line.startswith("v "):
            v.append([float(x) for x in line.split()[1:4]])
        elif line.startswith("f "):
            f.append([int(x) - 1 for x in line.split()[1:4]])
    return np.array(v, np.float32).reshape(-1, 3), np.array(f, np.int64).reshape(-1, 3)


def load_lights(path):
    out = []
    for line in open(path, encoding="utf-8"):
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        f = s.split()
        radius = float(f[13]) if len(f) > 13 and f[10] == "remix" else 0.5
        out.append((f[9], np.array([float(f[0]), float(f[1]), float(f[2])]), radius, line.rstrip("\r\n")))
    return out


def fibonacci_dirs(n):
    i = np.arange(n) + 0.5
    phi = np.arccos(1 - 2 * i / n)
    theta = np.pi * (1 + 5 ** 0.5) * i
    return np.stack([np.cos(theta) * np.sin(phi), np.cos(phi), np.sin(theta) * np.sin(phi)], 1).astype(np.float32)


def main():
    names = place_names()
    verts, tris, owner, model_names = [], [], [], []
    base = 0
    for obj in sorted((HERE / "out" / "col_m14").glob("*.obj")):
        v, f = load_obj(obj)
        if not len(f):
            continue
        model = obj.stem[:7]  # h0050B0A14 -> h0050B0
        verts.append(v)
        tris.append(f + base)
        owner.append(np.full(len(f), len(model_names)))
        model_names.append(model)
        base += len(v)
    V, F, O = np.concatenate(verts), np.concatenate(tris), np.concatenate(owner)
    tri = V[F]
    normals = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    normals /= np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-12)
    place = np.array([names.get(m, -1) for m in model_names])
    tri_place = place[O]
    scene = rtcs.EmbreeScene()
    TriangleMesh(scene, tri.astype(np.float32))
    print(f"{len(F)} collision triangles from {len(model_names)} models; Domain (+-1401) models: "
          f"{sorted(m for m, p in zip(model_names, place) if abs(p) == 1401)}", file=sys.stderr)

    dirs = fibonacci_dirs(256)
    rows = []
    for name, p, radius, line in load_lights(sys.argv[1]):
        org = np.repeat(p[None].astype(np.float32), len(dirs), 0)
        res = scene.run(org, dirs, output=1)
        prim, tfar = res["primID"], res["tfar"]
        hit = (prim >= 0) & (tfar < 200)
        n_hit = hit.sum()
        hp = prim[hit]
        domain = np.mean(np.abs(tri_place[hp]) == 1401) if n_hit else 0.0
        blight = np.mean(tri_place[hp] == 1400) if n_hit else 0.0
        back = np.mean((normals[hp] * dirs[hit]).sum(1) > 0) if n_hit else 0.0
        near = tfar[hit].min() if n_hit else np.inf
        top = np.bincount(O[hp], minlength=len(model_names)).argmax() if n_hit else -1
        rows.append((name, p, radius, n_hit / len(dirs), domain, blight, back, near, model_names[top] if top >= 0 else "-", line))
    print("name,x,y,z,radius,enclosed,domain,blighttown,backface,near,main_collision")
    for r in rows:
        print(f"{r[0]},{r[1][0]:.1f},{r[1][1]:.1f},{r[1][2]:.1f},{r[2]:.2f},{r[3]:.2f},{r[4]:.2f},{r[5]:.2f},{r[6]:.2f},{r[7]:.2f},{r[8]}")


if __name__ == "__main__":
    main()
