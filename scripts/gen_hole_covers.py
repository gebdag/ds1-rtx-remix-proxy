"""Generate the ds1_hole_covers Remix mod: dark quads under gaps in the game's own floors.

Where the game leaves a gap in a floor, its frame shows the dark it was cleared to; Remix's rays pass through to whatever
lies behind (a grey patch in the Catacombs, frame_20261105_225102, under the blend skirt of draw 287). A quad a little
under the floor fills it from behind: the floor around it hides it everywhere else.

A Remix mod cannot place free geometry, so each quad is a child of a captured mesh near it, in that mesh's own space:
its instance transform in the capture (rtx-remix\\captures\\capture_2026-11-06_00-18-43.usd) maps world to local.

Usage: py -3.12 scripts/gen_hole_covers.py [--data <DATA folder>]
"""
import os  # noqa: E402  (ds1paths: the local folders, see paths.example.ini)
import sys  # noqa: E402
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)))))
import ds1paths  # noqa: E402

import argparse
import os

DATA = ds1paths.GAME + r"\DATA"

# name, anchor mesh hash, the anchor instance's world transform (x sign, translation), world rectangle (Remix space: x is the
# game's x negated) x0 x1 z0 z1, world y.
COVERS = [
    # Catacombs, by the beetles past the second bonfire: the game draws no floor at game (43.7-44.4, -119.2, 208.7-209.0).
    # The floor round it lies at y -119.27 to -118.30; the quad is 0.3 m under its lowest point.
    ("catacombs_beetles", "8E3A518C1AE74B08", (-1.0, (-46.0, -105.0, 182.0)), (-45.4, -42.7, 207.7, 210.1), -119.57),
]


def local_point(sign_x, t, x, y, z):
    # world = (sign_x * lx + tx, ly + ty, lz + tz)
    return ((x - t[0]) / sign_x, y - t[1], z - t[2])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=DATA)
    args = ap.parse_args()
    out_dir = os.path.join(args.data, "rtx-remix", "mods", "ds1_hole_covers")
    os.makedirs(out_dir, exist_ok=True)

    meshes = []
    for name, anchor, (sign_x, t), (x0, x1, z0, z1), y in COVERS:
        corners = [local_point(sign_x, t, x, y, z) for x, z in ((x0, z0), (x1, z0), (x1, z1), (x0, z1))]
        pts = ", ".join(f"({p[0]:.4f}, {p[1]:.4f}, {p[2]:.4f})" for p in corners)
        meshes.append(f'''        over "mesh_{anchor}"
        {{
            def Mesh "hole_cover_{name}"
            {{
                bool doubleSided = 1
                int[] faceVertexCounts = [4]
                int[] faceVertexIndices = [0, 1, 2, 3]
                normal3f[] normals = [(0, 1, 0), (0, 1, 0), (0, 1, 0), (0, 1, 0)] (
                    interpolation = "vertex"
                )
                point3f[] points = [{pts}]
                texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
                    interpolation = "vertex"
                )
                uniform token subdivisionScheme = "none"
                rel material:binding = </RootNode/Looks/ds1_hole_cover>
            }}
        }}
''')

    text = f'''#usda 1.0
(
    customLayerData = {{
        string lightspeed_game_name = "DarkSouls1"
        string lightspeed_layer_type = "replacement"
        string lightspeed_mod_name = "ds1_hole_covers"
        string lightspeed_mod_notes = "Dark quads under gaps in the game's floors (scripts/gen_hole_covers.py in the DS1 proxy)."
        string lightspeed_mod_version = "1.0"
    }}
    upAxis = "Y"
)

over "RootNode"
{{
    over "Looks"
    {{
        def Material "ds1_hole_cover"
        {{
            token outputs:mdl:surface.connect = </RootNode/Looks/ds1_hole_cover/Shader.outputs:out>

            def Shader "Shader"
            {{
                uniform token info:implementationSource = "sourceAsset"
                uniform asset info:mdl:sourceAsset = @AperturePBR_Opacity.mdl@
                uniform token info:mdl:sourceAsset:subIdentifier = "AperturePBR_Opacity"
                color3f inputs:diffuse_color_constant = (0.02, 0.02, 0.02)
                float inputs:reflection_roughness_constant = 1
                token outputs:out
            }}
        }}
    }}

    over "meshes"
    {{
{"".join(meshes)}    }}
}}
'''
    path = os.path.join(out_dir, "mod.usda")
    with open(path, "w", newline="\n") as f:
        f.write(text)
    print("wrote", path, len(COVERS), "cover(s)")


if __name__ == "__main__":
    main()
