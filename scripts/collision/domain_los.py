"""Line of sight between custom lights through the map's visible geometry: for each light in <names>, the definite Domain
lights it can see directly (an open straight line), i.e. whether it shares the air of the cave system.
Usage: py -3.12 -P domain_los.py <lights.txt> <comma list of lights to test> <comma list of reference lights>"""
import importlib.util
import sys
from pathlib import Path

import numpy as np

spec = importlib.util.spec_from_file_location("vis", str(Path(__file__).parent / "domain_visual_check.py"))
vis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vis)
import os  # noqa: E402
from embreex import rtcore_scene as rtcs  # noqa: E402
from embreex.mesh_construction import TriangleMesh  # noqa: E402


def scene_tris():
    tables = os.path.join(vis.REPO, "assets", "mesh_tables")
    no_block = vis.table_sources(os.path.join(tables, "sky_meshes.txt")) | vis.table_sources(os.path.join(tables, "glass_meshes.txt"))
    flvers, tris = {}, []
    for model, at, rot, scale, _ in vis.map_pieces(os.path.join(vis.DATA, "map", "MapStudio", f"{vis.MAP}.msb")):
        path = os.path.join(vis.DATA, "map", vis.MAP, f"{model}A14.flver")
        if not os.path.exists(path):
            continue
        if model not in flvers:
            flvers[model] = vis.read_flver(open(path, "rb").read())
        flver = flvers[model]
        r = vis.rotation(rot) * scale
        for i, mesh in enumerate(flver.meshes):
            fs = mesh.lod0()
            if fs is None or "position" not in mesh.vertices:
                continue
            p = np.asarray(vis.first(mesh.vertices["position"]), np.float64)[:, :3]
            t = fs.triangles()
            t = t[(t < len(p)).all(1)]
            if not len(t) or f"{vis.MAP}/{model}A14/mesh{i}/lod0" in no_block or not vis.blocks_rays(str(flver.materials[mesh.material_index].mtd)):
                continue
            tris.append((p @ r.T + at)[t])
    return np.concatenate(tris)


lights = {}
for line in open(sys.argv[1], encoding="utf-8"):
    s = line.strip()
    if s and not s.startswith("#"):
        f = s.split()
        lights[f[9]] = np.array([float(f[0]), float(f[1]), float(f[2])])
tests = sys.argv[2].split(",")
refs = sys.argv[3].split(",")
scene = rtcs.EmbreeScene()
TriangleMesh(scene, scene_tris().astype(np.float32))
R = np.array([lights[r] for r in refs])
for name in tests:
    p = lights[name]
    d = R - p
    dist = np.linalg.norm(d, axis=1)
    res = scene.run(np.repeat(p[None], len(R), 0).astype(np.float32), (d / dist[:, None]).astype(np.float32), output=1)
    clear = (np.asarray(res["primID"]) < 0) | (np.asarray(res["tfar"]) >= dist - 0.05)
    seen = [f"{refs[i]}({dist[i]:.0f}m)" for i in np.argsort(dist) if clear[i]]
    print(f"{name}: sees {len(seen)} Domain lights: {' '.join(seen[:6])}")
