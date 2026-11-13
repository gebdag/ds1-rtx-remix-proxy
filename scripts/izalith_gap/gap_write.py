"""Writes the ds1_izalith_gap mod from a sheet grid (gap_full.py) under the anchor mesh (its transform from the capture)."""
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
grid, x0, z0, cell, level, walk = sheet["grid"], float(sheet["x0"]), float(sheet["z0"]), float(sheet["cell"]), float(sheet["level"]), float(sheet["walk"])
inv = np.linalg.inv(xf)

# rows of cells merged into runs: fewer quads, same coverage
vid, points, uvs, counts, indices = {}, [], [], [], []
def vertex(i, j):
    k = (i, j)
    if k not in vid:
        w = np.array([x0 + i * cell, level, z0 + j * cell, 1.0])
        vid[k] = len(points)
        points.append((w @ inv)[:3])
        uvs.append((w[0] / UV_METRES, w[2] / UV_METRES))
    return vid[k]
for j in range(grid.shape[0]):
    i = 0
    while i < grid.shape[1]:
        if not grid[j, i]:
            i += 1
            continue
        s = i
        while i < grid.shape[1] and grid[j, i]:
            i += 1
        # one quad per run, but its long edges need the vertices of neighbouring rows' run ends to avoid T-junction cracks:
        # simplest exact choice is one quad per cell along the run (shared vertices)
        for k in range(s, i):
            counts.append(4)
            indices += [vertex(k, j), vertex(k, j + 1), vertex(k + 1, j + 1), vertex(k + 1, j)]
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
        string lightspeed_mod_notes = "Lost Izalith, Bed of Chaos entrance: a ground sheet {walk - level:.0f} m under the walkway where the sky showed through. Checked against every visible map piece of m14_01 and m14_00 (not only what a capture drew): nothing of the map under it, nothing within 0.4 m. Textured with the walkway's own floor texture. Built by gap_full.py / gap_write.py."
        string lightspeed_mod_version = "1.1"
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
print("wrote", len(counts), "quads", len(points), "vertices at level", level)
