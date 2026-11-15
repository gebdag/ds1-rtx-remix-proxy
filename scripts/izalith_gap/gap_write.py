"""Writes the ds1_izalith_gap mod under the anchor mesh (its transform from the capture), from a sheet grid (gap_full.py: one quad
per cell), rectangles (one quad each) or triangles (gap_poly.py)."""
import shutil
import sys
from pathlib import Path
import numpy as np

sheet = np.load(sys.argv[1])
xf = np.load(sys.argv[2])
mod_dir = Path(sys.argv[3])
tex_src = Path(sys.argv[4])
ANCHOR = "4069520337E21066"
UV_METRES = 4.0
level, walk = float(sheet["level"]), float(sheet["walk"])
inv = np.linalg.inv(xf)

points, uvs, counts, indices = [], [], [], []
def vertex(x, z):
    w = np.array([x, level, z, 1.0])
    points.append((w @ inv)[:3])
    uvs.append((w[0] / UV_METRES, w[2] / UV_METRES))
    return len(points) - 1
if "tris" in sheet:
    for tri in sheet["tris"]:
        counts.append(3)
        indices += [vertex(x, z) for x, z in tri]
elif "rects" in sheet:
    for xa, za, xb, zb in sheet["rects"]:
        counts.append(4)
        indices += [vertex(xa, za), vertex(xa, zb), vertex(xb, zb), vertex(xb, za)]
else:
    grid, x0, z0, cell = sheet["grid"], float(sheet["x0"]), float(sheet["z0"]), float(sheet["cell"])
    for j, i in zip(*np.nonzero(grid)):
        xa, za = x0 + i * cell, z0 + j * cell
        counts.append(4)
        indices += [vertex(xa, za), vertex(xa, za + cell), vertex(xa + cell, za + cell), vertex(xa + cell, za)]
up = np.array([0.0, 1.0, 0.0]) @ np.linalg.inv(xf[:3, :3]).T
up /= np.linalg.norm(up)
mod_dir.mkdir(parents=True, exist_ok=True)
(mod_dir / "textures").mkdir(exist_ok=True)
shutil.copyfile(tex_src, mod_dir / "textures" / "izalith_ground.dds")
fmt = lambda v: "(" + ", ".join(f"{x:.4f}" for x in v) + ")"
usda = f'''#usda 1.0
(
    customLayerData = {{
        string lightspeed_game_name = "DarkSouls1"
        string lightspeed_layer_type = "replacement"
        string lightspeed_mod_name = "ds1_izalith_gap"
        string lightspeed_mod_notes = "Lost Izalith, Bed of Chaos entrance: the floor missing from the map in the two mirror-image holes left and right of the walkway (map data: bordered by m3000B1 and m8001B1, nothing below), filled at the height of the floor around them; the openings down to the arena stay open. Textured with a plain stone ground texture of the area. Built by gap_floor.py, gap_poly.py and gap_write.py."
        string lightspeed_mod_version = "2.0"
    }}
    upAxis = "Y"
)

over "RootNode"
{{
    over "Looks"
    {{
        def Material "ds1_izalith_ground"
        {{
            token outputs:mdl:surface.connect = </RootNode/Looks/ds1_izalith_ground/Shader.outputs:out>

            def Shader "Shader"
            {{
                uniform token info:implementationSource = "sourceAsset"
                uniform asset info:mdl:sourceAsset = @AperturePBR_Opacity.mdl@
                uniform token info:mdl:sourceAsset:subIdentifier = "AperturePBR_Opacity"
                asset inputs:diffuse_texture = @./textures/izalith_ground.dds@ (
                    colorSpace = "auto"
                )
                float inputs:reflection_roughness_constant = 0.85
                token outputs:out
            }}
        }}
    }}

    over "meshes"
    {{
        over "mesh_{ANCHOR}"
        {{
            def Mesh "izalith_gap_ground"
            {{
                bool doubleSided = 1
                int[] faceVertexCounts = [{", ".join(map(str, counts))}]
                int[] faceVertexIndices = [{", ".join(map(str, indices))}]
                normal3f[] normals = [{", ".join(fmt(up) for _ in points)}] (
                    interpolation = "vertex"
                )
                point3f[] points = [{", ".join(fmt(p) for p in points)}]
                texCoord2f[] primvars:st = [{", ".join(f"({u:.4f}, {v:.4f})" for u, v in uvs)}] (
                    interpolation = "vertex"
                )
                uniform token subdivisionScheme = "none"
                rel material:binding = </RootNode/Looks/ds1_izalith_ground>
            }}
        }}
    }}
}}
'''
(mod_dir / "mod.usda").write_text(usda, encoding="utf-8")
print("wrote", len(counts), "faces", len(points), "vertices at level", level)
