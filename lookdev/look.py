"""The game's look in a capture: the frame it presented and every shader constant
that shapes it (exposure, haze, grade, bloom, adaptation, lights, depth of field).

    python lookdev.py look [capture]...     -> <capture>_replay\\look.json (+ vanilla.png if missing)
    python lookdev.py look --table [capture]...

The frame comes straight from the capture's reference image, so no D3D device is
needed. Constants are read from the draws that use them:
  lit         the largest draw whose pixel shader ends with the haze tail (reads
              c104 and c110): exposure c135, haze c103-c111, lights c86-c99, shadow c121/c122/c175
  vs_fog      that draw's vertex shader c128 (fog start, 1 / range)
  grade       FRPG_Fil_HDR_ColAdj: c56 (bloom rate, light shaft, brightness), c62-c65 matrix, c68 grain, c71
  bloom / bright_pass / adapted_lum / sample_lum / light_shaft / dof*: their post passes, every
              constant they set that is non-zero
"""
import json
import struct
from pathlib import Path

import numpy as np
from PIL import Image

import analyze
import lookdev

REC_REFERENCE_IMAGE = 128
POST_PASSES = {
    "grade": "FRPG_Fil_HDR_ColAdj",
    "bloom": "FRPG_Fil_Bloom",
    "bright_pass": "FRPG_Fil_BrightPassFilter",
    "adapted_lum": "FRPG_Fil_CalcAdaptedLum",
    "sample_lum_initial": "FRPG_Fil_SampleLumInitial",
    "sample_lum_final": "FRPG_Fil_SampleLumFinal",
    "dof": "FRPG_Fil_Dof",
    "camera_blur": "FRPG_Fil_CameraBlur",
}
LIT_REGISTERS = (86, 87, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 103, 104, 105, 106, 107, 108, 109, 110, 111, 121, 122, 135, 139, 175)


def reference_image(path: Path) -> Image.Image | None:
    """The back buffer the game presented, from the capture's REFERENCE_IMAGE record."""
    for rtype, payload in analyze.records(path):
        if rtype != REC_REFERENCE_IMAGE:
            continue
        width, height, fmt, row_bytes = struct.unpack_from("<4I", payload)
        pixels = bytes(payload[16:16 + height * row_bytes])
        return Image.frombuffer("RGBA", (width, height), pixels, "raw", "BGRA", row_bytes, 1).convert("RGB")
    return None


def _consts(table: dict, registers=None) -> dict:
    regs = registers if registers is not None else sorted(table)
    return {f"c{r}": [round(v, 6) for v in table[r]] for r in regs if r in table and any(table[r])}


def look_values(path: Path) -> dict:
    """Shader constants that shape the frame's look, read from the draws that use them."""
    lit, lit_count, post = None, -1, {}
    for d in analyze.replay_state(path):
        psn = analyze.shader_name(d.shaders.get(d.ps))
        # A lit shader (hemisphere/environment lighting), not a depth or velocity pre-pass.
        if 104 in d.psf and 110 in d.psf and "Hem" in psn:
            if d.count > lit_count:
                lit_count = d.count
                lit = {"ps": psn, "vs": analyze.shader_name(d.shaders.get(d.vs)), "prims": d.count,
                       "ps_const": _consts(d.psf, LIT_REGISTERS), "vs_fog": _consts(d.vsf, (128,))}
        for key, name in POST_PASSES.items():
            if psn == name and key not in post:
                post[key] = {"ps": psn, "ps_const": _consts(d.psf)}
    return {"lit": lit, **post}


def summary_row(name: str, look: dict, frame: Image.Image | None) -> str:
    lit = (look.get("lit") or {}).get("ps_const", {})
    grade = (look.get("grade") or {}).get("ps_const", {})
    c135 = lit.get("c135", [0, 1, 0, 0])
    exposure = c135[0] / c135[1] if c135[1] else 0.0
    mean = np.asarray(frame, dtype=np.float32).reshape(-1, 3).mean(0) if frame else [0, 0, 0]
    c103 = lit.get("c103", [0, 0, 0, 0])
    c110 = lit.get("c110", [0, 0, 0, 0])
    c56 = grade.get("c56", [0, 0, 0, 0])
    return (f"{name:24s} mean rgb {mean[0]:5.1f} {mean[1]:5.1f} {mean[2]:5.1f} | exposure {exposure:5.3f} "
            f"| fog x{c103[3]:.2f} scatter x{c110[3]:.2f} | grade scale {c56[2]:.2f} bloom {c56[0]:.2f} shaft {c56[1]:.2f} "
            f"| {(look.get('lit') or {}).get('ps', '-')}")


def cmd_look(args) -> int:
    paths = [lookdev.resolve(c) for c in args.captures] if args.captures else lookdev.captures()
    for path in paths:
        out_dir = path.parent / (path.stem + "_replay")
        out_dir.mkdir(exist_ok=True)
        frame = None
        vanilla = out_dir / "vanilla.png"
        if vanilla.exists():
            frame = Image.open(vanilla).convert("RGB")
        else:
            frame = reference_image(path)
            if frame:
                frame.save(vanilla)
        look_file = out_dir / "look.json"
        if args.table and look_file.exists():
            look = json.loads(look_file.read_text(encoding="utf-8"))
        else:
            look = look_values(path)
            look_file.write_text(json.dumps(look, indent=1), encoding="utf-8")
        print(summary_row(path.stem, look, frame))
    return 0


def add_parser(sub) -> None:
    p = sub.add_parser("look", help="the game's look values in captures (see look.py)")
    p.add_argument("captures", nargs="*")
    p.add_argument("--table", action="store_true", help="reuse existing look.json files")
    p.set_defaults(fn=cmd_look)
