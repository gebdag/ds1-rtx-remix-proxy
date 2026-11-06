"""Generate the ds1_texture_fixes Remix mod: game textures whose colour reads wrong under the path tracer, rescaled.

Each fix is the game's own texture with its colour scaled (alpha, and so any cutout, kept), bound as the material's
albedo. Remix names a material after its albedo's hash (pbr_tools/hash_index.json), so the override is mat_<hash>.
Remix takes a material whole from the first mod that defines it (this one sorts before z_pbr), so whatever z_pbr sets on
the same material (its normal map, roughness...) is written here too, pointing into z_pbr's folder.

- m19_decal_floor_rubble (Undead Asylum): the flakes in the shallow water (frame_20261106_114950). The game shows them
  dark grey, barely lighter than the wet floor; in Remix they are the first surface the camera meets (the albedo view
  shows them, where the floor around them shows the water's), and came out white. 0.4 in linear light still left them
  bright under the tone curve; 0.15 matches the game's look by eye. The texture is the Asylum's rubble everywhere.

Usage: py -3.12 scripts/gen_texture_fixes.py [--data <DATA folder>]
Needs texconv.exe (pbr_tools).
"""
import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

DATA = r"<game dir>\DATA"
TEXCONV = r"<workspace>\Vibe-Reverse-Engineering\pbr_tools\texconv.exe"

# name, material hash (the albedo's), source texture under DATA\unpacked, linear colour scale.
FIXES = [
    ("m19_decal_floor_rubble", "DBF566CCF80EB4D4", r"map\m18\m19_decal_floor_rubble.dds", 0.15),
]


def texconv(args):
    subprocess.run([TEXCONV, "-nologo", "-y"] + args, check=True, capture_output=True)


def scaled_texture(src, dst, scale, tmp):
    # Decoded at full size (level 0), scaled in linear light, encoded again as BC1 with its 1-bit alpha and mips.
    texconv(["-f", "R8G8B8A8_UNORM", "-ft", "png", "-m", "1", "-o", tmp, src])
    png = os.path.join(tmp, os.path.splitext(os.path.basename(src))[0] + ".png")
    rgba = np.asarray(Image.open(png).convert("RGBA"), np.float32) / 255.0
    rgb = np.power(np.power(rgba[..., :3], 2.2) * scale, 1.0 / 2.2)
    out = np.concatenate([rgb, rgba[..., 3:]], axis=2)
    Image.fromarray(np.clip(out * 255.0 + 0.5, 0, 255).astype(np.uint8), "RGBA").save(png)
    texconv(["-f", "BC1_UNORM", "-at", "0.5", "-m", "0", "-o", os.path.dirname(dst), png])
    shutil.move(os.path.join(os.path.dirname(dst), os.path.basename(png)[:-4] + ".dds"), dst)


def z_pbr_inputs(z_pbr, material):
    """The Shader inputs z_pbr's layers set on mat_<material>, but the albedo, with their asset paths made relative to this mod."""
    lines = []
    for layer in sorted(Path(z_pbr).glob("*.usda")):
        text = layer.read_text(encoding="utf-8", errors="replace").splitlines()
        for i, line in enumerate(text):
            if line.strip() != f'over "mat_{material}"':
                continue
            depth = 0
            for body in text[i + 1:]:
                depth += body.count("{") - body.count("}")
                if "inputs:" in body and "diffuse_texture" not in body:
                    lines.append(body.strip().replace("@./", "@../z_pbr/"))
                if depth <= 0 and "}" in body:
                    break
    return list(dict.fromkeys(lines))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=DATA)
    args = ap.parse_args()
    mod = os.path.join(args.data, "rtx-remix", "mods", "ds1_texture_fixes")
    assets = os.path.join(mod, "assets")
    os.makedirs(assets, exist_ok=True)

    looks = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, material, source, scale in FIXES:
            scaled_texture(os.path.join(args.data, "unpacked", source), os.path.join(assets, name + ".dds"), scale, tmp)
            inputs = [f"asset inputs:diffuse_texture = @./assets/{name}.dds@"]
            inputs += z_pbr_inputs(os.path.join(args.data, "rtx-remix", "mods", "z_pbr"), material)
            body = "".join("                " + line + "\n" for line in inputs)
            looks.append(f'''        over "mat_{material}"
        {{
            over "Shader"
            {{
{body}            }}
        }}
''')

    text = f'''#usda 1.0
(
    customLayerData = {{
        string lightspeed_game_name = "DarkSouls1"
        string lightspeed_layer_type = "replacement"
        string lightspeed_mod_name = "ds1_texture_fixes"
        string lightspeed_mod_notes = "Game textures whose colour reads wrong under the path tracer, rescaled (scripts/gen_texture_fixes.py in the DS1 proxy)."
        string lightspeed_mod_version = "1.0"
    }}
    upAxis = "Y"
)

over "RootNode"
{{
    over "Looks"
    {{
{"".join(looks)}    }}
}}
'''
    path = os.path.join(mod, "mod.usda")
    with open(path, "w", newline="\n") as f:
        f.write(text)
    print("wrote", path, len(FIXES), "texture(s)")


if __name__ == "__main__":
    main()
