"""Splits a capture's irradiance probe (the HemEnv shaders' s11 cube) into a sun and an ambient part.

DS1's HemEnv shaders light a surface with irradianceCube(N) * c86 + the c98/c99
hemisphere and never read a light direction: the area's key light is baked into
the cube. Modelling the cube as ambient + sun * max(0, dot(N, -L)), with L the
shadow light's direction of travel (c175), gives a sun colour and an ambient
colour a path tracer can use as a real distant light and skylight.

    python lookdev.py probe [capture]...
"""
import struct

import numpy as np

import analyze
import lookdev

FACE_NORMALS = np.array([[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]], dtype=np.float64)


def probe_faces(path) -> tuple[np.ndarray | None, dict]:
    """Mean RGB of each s11 cube face for the largest HemEnv draw, and that draw's constants."""
    best, best_count = None, -1
    for d in analyze.replay_state(path):
        psn = analyze.shader_name(d.shaders.get(d.ps))
        if "HemEnv" in psn and d.tex.get(11) and d.count > best_count:
            best, best_count = d, d.count
    if best is None:
        return None, {}
    cube_id = best.tex[11]
    faces = {}
    for rtype, p in analyze.records(path):
        if rtype == 32:
            tid, flags, face, level, rows, row_bytes, depth = struct.unpack_from("<QIIIIII", p)
            if tid == cube_id and level == 0:
                px = np.frombuffer(bytes(p[32:32 + rows * row_bytes]), dtype=np.uint8).reshape(rows, row_bytes // 4, 4)
                faces[face] = px[..., [2, 1, 0]].reshape(-1, 3).mean(0) / 255.0  # BGRX -> RGB
    if len(faces) != 6:
        return None, {}
    return np.array([faces[i] for i in range(6)]), best.psf


def fit(faces: np.ndarray, light_dir: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray, float]:
    """Least-squares sky (lerp(down, up, n.y * 0.5 + 0.5)) + sun * max(0, dot(n, -L)) per channel.
    Returns (sky up, sky down, sun, rms)."""
    to_light = -light_dir / np.linalg.norm(light_dir)
    lambert = np.clip(FACE_NORMALS @ to_light, 0.0, None)
    t = FACE_NORMALS[:, 1] * 0.5 + 0.5
    a = np.stack([t, 1.0 - t, lambert], axis=1)
    coef, *_ = np.linalg.lstsq(a, faces, rcond=None)
    rms = float(np.sqrt(((a @ coef - faces) ** 2).mean()))
    return np.clip(coef[0], 0, None), np.clip(coef[1], 0, None), np.clip(coef[2], 0, None), rms


def cmd_probe(args) -> int:
    paths = [lookdev.resolve(c) for c in args.captures] if args.captures else lookdev.captures()
    for path in paths:
        faces, psf = probe_faces(path)
        if faces is None:
            print(f"{path.stem:18s} no HemEnv probe")
            continue
        c86 = np.array(psf.get(86, (1, 1, 1, 0))[:3])
        c175 = np.array(psf.get(175, (0, -1, 0, 0))[:3])
        up, down, sun, rms = fit(faces * c86, c175)
        f = lambda v: "(" + ", ".join(f"{x:.2f}" for x in v) + ")"
        print(f"{path.stem:18s} sun {f(sun)}  sky up {f(up)} down {f(down)}  rms {rms:.3f}  | faces max {(faces * c86).max():.2f}  L {f(c175)}")
    return 0


def add_parser(sub) -> None:
    p = sub.add_parser("probe", help="split captures' irradiance probes into sun + ambient (see probe_fit.py)")
    p.add_argument("captures", nargs="*")
    p.set_defaults(fn=cmd_probe)
