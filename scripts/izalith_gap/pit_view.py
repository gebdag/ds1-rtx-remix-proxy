"""Renders the real map geometry (every piece of m14_01 and m14_00, from the MSB + FLVERs) around the Bed of Chaos pit from a
camera: grey = map surface shaded by its normal, red = the view ray escapes (where the game shows the sky). Saves the image
and, for the red pixels, nothing else; with --edges it also prints the outline of the opening on the walls.

Usage: py -3.12 -P pit_view.py <cams_iz.npy> <out.png> [yaw_deg pitch_deg dx dy dz]
       py -3.12 -P pit_view.py <cams_iz.npy> <out.png> --look ex ey ez fx fy fz tanx tany   (Remix world eye, forward)
The --look values for a lookdev capture: eye = its GAME_FRAME camera with x negated, forward = VS c3.xyz (the main
view-projection, translation 0) with x negated, tanx/tany = |c3.xyz| / |c0.xyz|, |c3.xyz| / |c1.xyz|."""
import os  # noqa: E402  (ds1paths: the local folders, see paths.example.ini)
import sys  # noqa: E402
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")))
import ds1paths  # noqa: E402

import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, ds1paths.SCRIPTS)
from map_geometry import first, map_pieces, rotation  # noqa: E402
from bbformats.flver import read_flver  # noqa: E402
from embreex import rtcore_scene as rtcs  # noqa: E402
from embreex.mesh_construction import TriangleMesh  # noqa: E402

DATA = ds1paths.GAME + r"\DATA\unpacked"


def load_map():
    tris, owner = [], []
    for MAP in ("m14_01_00_00", "m14_00_00_00"):
        flvers = {}
        for model, at, rot, scale, name in map_pieces(os.path.join(DATA, "map", "MapStudio", f"{MAP}.msb")):
            path = os.path.join(DATA, "map", MAP, f"{model}A14.flver")
            if not os.path.exists(path):
                continue
            if model not in flvers:
                flvers[model] = read_flver(open(path, "rb").read())
            r = rotation(rot) * scale
            for mesh in flvers[model].meshes:
                fs = mesh.lod0()
                if fs is None or "position" not in mesh.vertices:
                    continue
                p = np.asarray(first(mesh.vertices["position"]), np.float64)[:, :3]
                t = fs.triangles()
                t = t[(t < len(p)).all(1)]
                if len(t):
                    tris.append((p @ r.T + at)[t])
                    owner += [f"{MAP}:{name}:{model}"] * len(t)
    G = np.concatenate(tris) * np.array([-1.0, 1.0, 1.0])  # Remix world (x mirrored)
    return G, owner


if __name__ == "__main__":
    cams = np.load(sys.argv[1])
    out = sys.argv[2]
    G, owner = load_map()
    scene = rtcs.EmbreeScene()
    TriangleMesh(scene, G.astype(np.float32))
    xf = cams[0]
    eye = xf[3, :3].copy()
    R = xf[:3, :3].copy()
    TX, TY = 0.8, 0.45
    if len(sys.argv) > 3 and sys.argv[3] == "--look":
        v = list(map(float, sys.argv[4:12]))
        eye = np.array(v[0:3])
        fwd = np.array(v[3:6]); fwd /= np.linalg.norm(fwd)
        right = np.cross(fwd, [0, 1, 0]); right /= np.linalg.norm(right)
        up = np.cross(right, fwd)
        R = np.stack([right, up, -fwd])
        TX, TY = v[6], v[7]
    elif len(sys.argv) > 3:
        yaw, pitch, dx, dy, dz = map(float, sys.argv[3:8])
        cy, sy, cp, sp = np.cos(np.radians(yaw)), np.sin(np.radians(yaw)), np.cos(np.radians(pitch)), np.sin(np.radians(pitch))
        fwd = np.array([sy * cp, sp, cy * cp])
        right = np.cross(fwd, [0, 1, 0]); right /= np.linalg.norm(right)
        up = np.cross(right, fwd)
        R = np.stack([right, up, -fwd])
        eye = eye + [dx, dy, dz]
    W, H = 960, 540
    gx, gy = np.meshgrid(((np.arange(W) + 0.5) / W - 0.5) * 2 * TX, -((np.arange(H) + 0.5) / H - 0.5) * 2 * TY)
    d = np.stack([gx, gy, -np.ones_like(gx)], -1).reshape(-1, 3) @ R
    d /= np.linalg.norm(d, axis=1, keepdims=True)
    o = np.repeat(eye[None], len(d), 0)
    res = scene.run(o.astype(np.float32), d.astype(np.float32), output=1)
    prim = np.asarray(res["primID"])
    tfar = np.asarray(res["tfar"])
    img = np.zeros((len(d), 3), np.float32)
    hit = prim >= 0
    tri = G[prim[hit]]
    n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-12
    shade = 0.25 + 0.75 * np.abs(n @ np.array([0.3, 0.8, 0.5]) / np.linalg.norm([0.3, 0.8, 0.5]))
    fog = np.exp(-tfar[hit] / 120.0)
    img[hit] = (shade * (0.35 + 0.65 * fog))[:, None] * np.array([0.85, 0.8, 0.72])
    img[~hit] = [0.9, 0.1, 0.1]
    Image.fromarray((img.reshape(H, W, 3) * 255).clip(0, 255).astype(np.uint8)).save(out)
    print(f"eye {np.round(eye, 2)}; escaped {int((~hit).sum())} of {len(d)} pixels; saved {out}")
