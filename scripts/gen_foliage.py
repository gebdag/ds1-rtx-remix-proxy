"""Generate z_pbr/foliage.usda: Remix's thin opaque material on DS1's alpha-cut leaf textures.

DS1 lights its plants with flat ambient and no occlusion. Path traced, dense leaf cards block each other's light:
rosettes get black hearts and hard leaf shadows, tall grass dark clumps (Firelink has no sun, so indirect light is
all they get). Remix's thin opaque material (an AperturePBR_Opacity with subsurface_measurement_distance > 0 and no
diffusion profile) adds a diffuse transmission lobe: light from behind a leaf scatters through it (Hanrahan single
scattering). Alpha-cut texels stay cut. Shadow rays pass a thin opaque leaf with only about 1% of the light, so leaf
shadows stay; the gain is the light the transmission lobe carries through the leaves.

Colour: Remix does not tint transmitted light with the albedo, only with subsurface_transmittance_color and
subsurface_single_scattering_albedo (a grey guess turned the hearts pale cyan). Each texture gets the hue of its own
leaf texels: the linear mean colour of texels with alpha > 0.5, normalised to its largest channel, times SCALE
(transmittance floored at 0.25 of that per channel). The volume attenuation uses the distance squared, so at
DISTANCE 0.5 the applied normal-incidence transmittance is the square root of the authored one.

The layer is a z_pbr sublayer, not a mod of its own: Remix takes a texture's material from the first mod that has its
hash, so a separate mod would drop z_pbr's high-res albedo and normal maps for these leaves. pbr_tools/
build_pbr.py keeps foliage.usda in z_pbr's sublayers; this script adds it to the current mod.usda if missing.

Only hashes a capture proved (PROVEN), plus those the user approved in the game (USER_APPROVED), are written: in those captures the plants move towards the game's look and
nothing else gets worse. A texture name can have several copies (map, object, other areas' folders) with their own
hashes; each copy is proven on its own, by the hash the capture uploads.

A texture only partly made of leaves (MASKED) gets a thickness mask (subsurface_thickness_texture: the measurement
distance per texel, 0 = no transmission) from the triangles its model's named bones drive, and the hue of those texels.

One hue per texture is a compromise where a card mixes leaves with flowers (m10_grass_14/15: dark leaves, white petals,
so their transmission comes out near grey); a per-texel single scattering texture would follow both.

Usage: python gen_foliage.py [--all] [--list]
  --all   every alpha-cut leaf texture by name, written to the live layer (testing candidates; rerun without it after)
  --list  print the picks, write nothing
"""
import os  # noqa: E402  (ds1paths: the local folders, see paths.example.ini)
import sys  # noqa: E402
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)))))
import ds1paths  # noqa: E402

import json
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image

INDEX = Path(ds1paths.PBR_TOOLS + r"\hash_index.json")
Z_PBR = Path(ds1paths.GAME + r"\DATA\rtx-remix\mods\z_pbr")
LAYER = "foliage.usda"

NAME = re.compile(r"grass|leaf|ivy|fern|tree|bush|plant|weed|flower", re.I)
SKIP = re.compile(r"_(n|s|l)$|ground|giant|crystal|charcoal|_base|bottom", re.I)
# Remix texture hash -> where a capture proved it (the hash's texture name is in hash_index.json).
PROVEN = {
    # Firelink Shrine, map/m10 copies (frame_20261107_083232, 083353, frame_20261106_114512, frame_20261104_115459)
    "EB28A9D80D972F4B": "m10_grass_02", "FEFDA0837BA43BD0": "m10_grass_03", "BC71D7637331D69B": "m10_grass_04",
    "D7C0F1902256A036": "m10_grass_05", "B97C693B8D7082C7": "m10_grass_06", "F37F164ABCC5FB4A": "m10_grass_08",
    "EAA8999C1EAFEECC": "m10_grass_10", "AB745F80A36F4871": "m10_grass_11", "4189BFCB3F2C9E2A": "m10_grass_12",
    "5385E0E17F7EF306": "m10_grass_14", "30BC8E55C8F218C2": "m10_grass_15", "9D893F72FFCE6E7A": "m10_grass_16",
    "FE1851B22ED0B5A1": "m10_grass_18", "6E0F9FF9B7BF6952": "m10_grass_19", "C1B37DD0B83DE215": "m10_grass_21",
    # Undead Burg (frame_20261107_025500)
    "BE798D0C7579B50F": "m10_grass_00", "1CAB90B15D1AAD7E": "m10_grass_diteal", "20C1D68DA46295D6": "m10_ivy_01",
    "898339D99DA7C2E9": "m10_01_grass_01", "E5B975B00D7C6319": "m10_01_tree_02", "7F86267370401636": "m10_01_tree_05",
}
# Approved by the user in the game (2026-11-07): Darkroot (m12_00) and Royal Wood / Oolacile (m12_01) leaves, map/m12 copies
# plus the m15_tree_04 copies Darkroot draws. In captures these canopies come out brighter than the game
# (frame_20261101_225235: 21.1 game, 23.6 without, 32.1 with); the user prefers them in play.
USER_APPROVED = {
    "97D9264B99B5A592": "m12_00_leaf_01", "7B6C68F699946078": "m12_00_leaf_02", "1DA22CB8590E9422": "m12_00_leaf_03",
    "E0B772F779343B03": "m12_00_tree_05", "F8CBA5C287706719": "m12_dlc_flower_01", "0B73BC006A0E7473": "m12_dlc_flower_02",
    "3690CBFEDF027476": "m12_dlc_grass_01", "84983DA40448A699": "m12_dlc_grass_02", "9E27E2A5BB6596E8": "m12_dlc_grass_19",
    "7D670475EAAB0225": "m12_dlc_grass_20", "14D698256D49E4C3": "m12_dlc_m10_01_grass_10",
    "F08A2C3A2CC910D8": "m12_dlc_m15_tree_04", "AA0B10A229DC8A09": "m12_dlc_tree_04", "4130D7C3E33EC50B": "m12_dlc_tree_05",
    "58A983497F45651B": "m12_grass_01", "0D4A4F3191DC5ACE": "m12_grass_04", "8C6D048C671D1E3B": "m12_grass_05",
    "027410C040EFA07E": "m12_grass_06", "10C2EB7343BB5D1C": "m12_grass_14", "5E6EF61336340305": "m12_grass_15",
    "2AEB94AEE985F103": "m12_grass_18", "9E560E01B855CB51": "m12_grass_19", "200EB0FC0652B3C2": "m12_grass_20",
    "161FFCB06D189E9F": "m12_ivy_01", "38673BD39852E891": "m12_ivy_diteal_01", "E6038802825B41E6": "m12_n_plant_03",
    "9A6A6FA96E1F3A77": "m15_tree_01", "68A09E87F54336C5": "m15_tree_04", "B371E9901947C69B": "m15_tree_04",
    "F5EEB66B183BA916": "m15_tree_04",
}
# Textures only partly leaves: Remix hash -> (texture name, model under unpacked/, bone name part). Moonlight Butterfly (c3230):
# one atlas for body and wings, only the wing membranes transmit (user's request, 2026-11-07; Remix already draws these
# wings brighter than the game).
MASKED = {
    "4AA48A4508A90EE5": ("c3230", "chr/c3230/c3230.flver", "wing"),
}
UNPACKED = Path(ds1paths.GAME + r"\DATA\unpacked")
FLVER_TOOLS = ds1paths.SCRIPTS
PBR_TOOLS = ds1paths.PBR_TOOLS
MASK_DIR = "assets/thin_opaque"
SCALE = 0.55        # hue scale of transmittance and single scattering albedo
DISTANCE = 0.5      # subsurface_measurement_distance (> 0.05: diffuse reflection keeps its full weight)
MIN_COVER, MAX_COVER = 0.02, 0.9   # share of texels with alpha > 0.5: leaf cards are cut out, bark is not


def leaf_hue(path):
    """(cover, hue): share of kept texels and their normalised linear mean colour (None when not a cut-out card)."""
    a = np.asarray(Image.open(path).convert("RGBA"), np.float32) / 255.0
    keep = a[..., 3] > 0.5
    cover = float(keep.mean())
    if not MIN_COVER <= cover <= MAX_COVER:
        return cover, None
    c = a[..., :3][keep]
    lin = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4).mean(0)
    return cover, lin / max(float(lin.max()), 1e-6)


def bone_mask(model, bone_part, size):
    """Texels (H x W bool) of the model's triangles whose vertices are mostly driven by bones named with bone_part; texels
    other triangles also cover are left out."""
    sys.path.insert(0, FLVER_TOOLS)
    from bbformats.flver import read_flver
    from PIL import ImageDraw
    f = read_flver((UNPACKED / model).read_bytes())
    names = [b.name.lower() for b in f.bones]
    w, h = size
    hit, other = Image.new("L", size, 0), Image.new("L", size, 0)
    draw_hit, draw_other = ImageDraw.Draw(hit), ImageDraw.Draw(other)
    first = lambda v: np.asarray(v[0] if isinstance(v, list) else v)
    for m in f.meshes:
        uv = first(m.vertices["uv"])[:, :2]
        bi = first(m.vertices["bone_indices"]).astype(int)
        bw = first(m.vertices["bone_weights"]).astype(float)
        top = bi[np.arange(len(bi)), bw.argmax(1)]
        local = getattr(m, "bone_indices", None)
        bones = np.asarray(local)[top] if local is not None and len(local) else top
        driven = np.array([bone_part in names[b] for b in bones])
        tris = m.lod0().triangles()
        for t in tris[(tris < len(uv)).all(1)]:
            points = [(float(uv[i, 0] * w), float(uv[i, 1] * h)) for i in t]
            (draw_hit if driven[t].sum() >= 2 else draw_other).polygon(points, fill=255)
    return (np.asarray(hit) > 0) & ~(np.asarray(other) > 0)


def write_thickness(mask, name):
    """The mask as a BC4 thickness texture (DISTANCE inside, 0 outside) in z_pbr; returns its path in the layer."""
    import tempfile
    sys.path.insert(0, PBR_TOOLS)
    import build_pbr
    g = np.where(mask, int(round(DISTANCE * 255)), 0).astype(np.uint8)
    rgba = np.stack([g, g, g, np.full_like(g, 255)], -1)
    dst = Z_PBR / MASK_DIR / f"{name}_thickness.dds"
    with tempfile.TemporaryDirectory() as tmp:
        build_pbr._compress([rgba], dst, "BC4_UNORM", tmp, 0)
    return f"./{MASK_DIR}/{name}_thickness.dds"


def masked_picks(write):
    entries = json.load(open(INDEX))["entries"]
    out = []
    for h, (stem, model, bone_part) in sorted(MASKED.items()):
        e = entries[h]
        if e["stem"].lower() != stem:
            sys.exit(f"masked {h} is labelled {stem} but the index names it {e['stem']}")
        a = np.asarray(Image.open(e["path"]).convert("RGBA"), np.float32) / 255.0
        mask = bone_mask(model, bone_part, (a.shape[1], a.shape[0]))
        c = a[..., :3][mask & (a[..., 3] > 0.5)]
        lin = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4).mean(0)
        hue = lin / max(float(lin.max()), 1e-6)
        thickness = write_thickness(mask, stem) if write else None
        out.append((h, stem, hue, thickness))
        print(f"{h} {stem:24s} masked to '{bone_part}' bones ({mask.mean():.2f} of texels) hue {np.round(hue, 2)}")
    return out


def is_leaf_name(entry):
    return entry.get("role") == "albedo" and NAME.search(entry["stem"]) and not SKIP.search(entry["stem"])


def picks(every):
    entries = json.load(open(INDEX))["entries"]
    listed = {**PROVEN, **USER_APPROVED}
    hashes = sorted(h for h, e in entries.items() if is_leaf_name(e)) if every else sorted(listed)
    out = []
    for h in hashes:
        e = entries[h]
        if not every and e["stem"].lower() != listed[h]:
            sys.exit(f"listed {h} is labelled {listed[h]} but the index names it {e['stem']}")
        cover, hue = leaf_hue(e["path"])
        if hue is None:
            if not every:
                sys.exit(f"listed {h} {e['stem']} is not a cut-out card (cover {cover:.2f})")
            continue
        out.append((h, e["stem"], hue, None))
        print(f"{h} {e['stem']:24s} cover {cover:.2f} hue {np.round(hue, 2)}")
    return out


def layer_text(materials):
    out = ['#usda 1.0\n(\n    upAxis = "Y"\n)\n\nover "RootNode"\n{\n    over "Looks"\n    {\n']
    for h, stem, hue, thickness in materials:
        tr = ", ".join(f"{v * SCALE:.3f}" for v in np.maximum(hue, 0.25))
        ssa = ", ".join(f"{v * SCALE:.3f}" for v in hue)
        out.append(f'        over "mat_{h}"  # {stem}\n        {{\n            over "Shader"\n            {{\n'
                   f'                float inputs:subsurface_measurement_distance = {DISTANCE}\n'
                   + (f'                asset inputs:subsurface_thickness_texture = @{thickness}@\n' if thickness else '') +
                   f'                color3f inputs:subsurface_transmittance_color = ({tr})\n'
                   f'                color3f inputs:subsurface_single_scattering_albedo = ({ssa})\n'
                   f'            }}\n        }}\n')
    out.append("    }\n}\n")
    return "".join(out)


def register_layer():
    """Lists the layer first in z_pbr's sublayers (as build_pbr.py does), keeping the file's line endings."""
    mod = Z_PBR / "mod.usda"
    raw = mod.read_bytes()
    if f"@./{LAYER}@".encode() in raw:
        return
    nl = b"\r\n" if b"\r\n" in raw else b"\n"
    head = b"subLayers = [" + nl
    if raw.count(head) != 1:
        sys.exit(f"{mod}: no single subLayers list to add {LAYER} to")
    mod.write_bytes(raw.replace(head, head + f"        @./{LAYER}@,".encode() + nl))
    print("registered", LAYER, "in", mod)


def main():
    materials = picks("--all" in sys.argv) + masked_picks("--list" not in sys.argv)
    print(len(materials), "materials")
    if "--list" in sys.argv:
        return
    (Z_PBR / LAYER).write_text(layer_text(materials), encoding="utf-8", newline="\n")
    register_layer()
    print("wrote", Z_PBR / LAYER)


if __name__ == "__main__":
    main()
