"""Writes DATA/rtx-remix/mods/ds1_water_murk: New Londo's murkier water.

The proxy draws New Londo's water with the second water stand-in (0xFF2F5D74 instead of 0xFF2F5D73; [FFP] WaterAreaStandIn = 1,
set by the [New Londo] section of rtx_comp/settings/area_overrides.ini), so Remix hashes it apart (XXH3-64 of the 8x8 texels,
0x5A7C3B5F4E728996) and this mod gives it its own material: ds1_water's translucent water plus Remix's diffuse layer, whose
colour and opacity come from murk_new_londo.dds. Both are the game's own values for New Londo's water (frame_20261029_154158,
FRPG_Water_HeightMap draws 294-298 and Reflect draws 814-816): murk colour c127.rgb (0.012, 0.020, 0.024), mixed over the
refracted scene at c127.w * vertex alpha = 0.502 * 0.5. Reflections stay path traced.

rtx.conf must list the second stand-in in rtx.animatedWaterTextures (0x5A7C3B5F4E728996) for the wave normals to move.

Run: python scripts/gen_water_murk.py
"""
import shutil
import struct
from pathlib import Path

import xxhash
from PIL import Image

MODS = Path(r"<game dir>\DATA\rtx-remix\mods")
OUT = MODS / "ds1_water_murk"
STANDIN_PIXEL = 0xFF2F5D74
MURK_RGB = (0.012, 0.020, 0.024)
# The game's mix weight is c127.w * vertex alpha = 0.502 * 0.5, but its water colour is then multiplied by the mesh's vertex colour
# (FRPG_Water_Reflect: mul r0.xyz, r2, v3), and what shows in the game is near black water with no floor under it: the floor beside
# the player reads (0, 0, 3) of 255 on frame_20261111_014707 and 014716, where a quarter of murk left it at (32, 34, 28) with the
# cobbles plain to see. 0.9 hides the floor as the game does and keeps a trace of it in the shallows.
MURK_OPACITY = 0.9

standin = xxhash.xxh3_64_intdigest(struct.pack("<I", STANDIN_PIXEL) * 64)
OUT.mkdir(parents=True, exist_ok=True)
shutil.copy2(MODS / "ds1_water" / "water_normal.dds", OUT / "water_normal.dds")
texel = tuple(round(v * 255) for v in MURK_RGB) + (round(MURK_OPACITY * 255),)
Image.new("RGBA", (8, 8), texel).save(OUT / "murk_new_londo.dds")

(OUT / "mod.usda").write_text(f'''#usda 1.0
(
    customLayerData = {{
        string lightspeed_game_name = "DarkSouls1"
        string lightspeed_layer_type = "replacement"
        string lightspeed_mod_name = "ds1_water_murk"
        string lightspeed_mod_notes = """New Londo's water: the second water stand-in (0x{STANDIN_PIXEL:08X}, [FFP] WaterAreaStandIn = 1 from area_overrides.ini [New Londo]). ds1_water's material plus Remix's diffuse layer: the game's murk colour c127.rgb at opacity c127.w * vertex alpha, murk_new_londo.dds. Built by scripts/gen_water_murk.py."""
        string lightspeed_mod_version = "1.0"
    }}
    upAxis = "Y"
)

over "RootNode"
{{
    over "Looks"
    {{
        over "mat_{standin:016X}"
        {{
            over "Shader"
            {{
                uniform token info:implementationSource = "sourceAsset"
                uniform asset info:mdl:sourceAsset = @AperturePBR_Translucent.mdl@
                uniform token info:mdl:sourceAsset:subIdentifier = "AperturePBR_Translucent"
                asset inputs:normalmap_texture = @./water_normal.dds@ (
                    colorSpace = "raw"
                )
                asset inputs:transmittance_texture = @./murk_new_londo.dds@ (
                    colorSpace = "raw"
                )
                bool inputs:use_diffuse_layer = 1
                float inputs:ior_constant = 1.33
                color3f inputs:transmittance_color = (0.866, 0.866, 0.866)
                float inputs:transmittance_measurement_distance = 1.5
                bool inputs:thin_walled = 0
            }}
        }}
    }}
}}
''', encoding="utf-8")
print("ds1_water_murk written:", f"mat_{standin:016X}", "murk texel", texel)
