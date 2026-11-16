"""Add the detail normal of a swamp with its own height texture to the ds1_swamp mod: the Gaping Dragon's slime, Sen's Fortress's tar.

The slime is FRPG_Snow with a height texture of its own, which the proxy bakes into the colour texture's alpha (game/swamp.cpp), so
the draw reaches Remix with a texture no mod knows. Remix takes normal maps only from mods, and without this one the slime has no
lumps. The material is keyed on the baked texture's hash, from the first level as the proxy uploads it (bytes in D3D order).

    py -3.12 scripts/gen_slime_material.py <capture with the slime> <the baked level: width * height * 4 bytes> [--normal m15_detail_tar_n --out sen_tar_normal.dds]
"""
import os  # noqa: E402  (ds1paths: the local folders, see paths.example.ini)
import sys  # noqa: E402
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)))))
import ds1paths  # noqa: E402

import argparse
import struct
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "lookdev"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze as A  # noqa: E402
from gen_water_material import octahedral, write_dds  # noqa: E402

MOD_DIR = Path(ds1paths.GAME + r"\DATA\rtx-remix\mods\ds1_swamp")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture")
    ap.add_argument("baked", type=Path)
    ap.add_argument("--normal", default="m10_00_slime_n", help="the game's detail normal texture")
    ap.add_argument("--out", default="slime_normal.dds", help="file name in the mod")
    a = ap.parse_args()
    if (a.normal == "m10_00_slime_n") != (a.out == "slime_normal.dds"):
        sys.exit("--normal and --out go together: another swamp's normal must not overwrite the slime's")

    import xxhash
    found = A.texture_image(a.capture, a.normal)
    if not found:
        sys.exit(f"{a.normal} has no data in {a.capture}")
    t = np.asarray(found[0].convert("RGBA"), dtype=np.float64) / 255.0
    # The game reads xy = rg * 2 - 1 and adds them to its normal as tangent offsets, as the tar's in gen_swamp_material.py.
    n = np.stack([t[..., 0] * 2.0 - 1.0, t[..., 1] * 2.0 - 1.0, np.ones(t.shape[:2])], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    levels = []
    while True:
        levels.append(octahedral(n))
        if n.shape[0] == 1 or n.shape[1] == 1:
            break
        n = n.reshape(n.shape[0] // 2, 2, n.shape[1] // 2, 2, 3).mean(axis=(1, 3))
        n /= np.linalg.norm(n, axis=-1, keepdims=True)
    write_dds(MOD_DIR / a.out, levels)

    mat = f"mat_{xxhash.xxh3_64_intdigest(a.baked.read_bytes()):016X}"
    entry = f'''        over "{mat}"
        {{
            over "Shader"
            {{
                uniform token info:implementationSource = "sourceAsset"
                uniform asset info:mdl:sourceAsset = @AperturePBR_Opacity.mdl@
                uniform token info:mdl:sourceAsset:subIdentifier = "AperturePBR_Opacity"
                asset inputs:normalmap_texture = @./{a.out}@ (
                    colorSpace = "raw"
                )
            }}
        }}
'''
    mod = MOD_DIR / "mod.usda"
    text = mod.read_text(encoding="utf-8")
    if mat not in text:
        key = '    over "Looks"\n    {\n'
        if text.count(key) != 1:
            sys.exit(f"no Looks block in {mod}")
        mod.write_text(text.replace(key, key + entry), encoding="utf-8", newline="\n")
    print(f"{mat}, {found[0].size}, {len(levels)} mips -> {mod}")


if __name__ == "__main__":
    main()
