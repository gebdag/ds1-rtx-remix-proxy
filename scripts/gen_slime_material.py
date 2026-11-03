"""Add the Gaping Dragon slime's detail normal to the ds1_swamp mod.

The slime is FRPG_Snow with a height texture of its own, which the proxy bakes into the colour texture's alpha (game/swamp.cpp), so
the draw reaches Remix with a texture no mod knows. Remix takes normal maps only from mods, and without this one the slime has no
lumps. The material is keyed on the baked texture's hash, from the first level as the proxy uploads it (bytes in D3D order).

    py -3.12 scripts/gen_slime_material.py <capture with the slime> <the baked level: width * height * 4 bytes>
"""
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

MOD_DIR = Path(r"<game dir>\DATA\rtx-remix\mods\ds1_swamp")
NORMAL_NAME = "m10_00_slime_n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture")
    ap.add_argument("baked", type=Path)
    a = ap.parse_args()

    import xxhash
    found = A.texture_image(a.capture, NORMAL_NAME)
    if not found:
        sys.exit(f"{NORMAL_NAME} has no data in {a.capture}")
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
    write_dds(MOD_DIR / "slime_normal.dds", levels)

    mat = f"mat_{xxhash.xxh3_64_intdigest(a.baked.read_bytes()):016X}"
    entry = f'''        over "{mat}"
        {{
            over "Shader"
            {{
                uniform token info:implementationSource = "sourceAsset"
                uniform asset info:mdl:sourceAsset = @AperturePBR_Opacity.mdl@
                uniform token info:mdl:sourceAsset:subIdentifier = "AperturePBR_Opacity"
                asset inputs:normalmap_texture = @./slime_normal.dds@ (
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
