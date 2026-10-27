"""Builds the Remix water material from the game's own water data.

  python scripts/gen_water_material.py <capture> --speed <capture A> <capture B>

DS1 draws water in two passes (shaders disassembled with lookdev/analyze.py):
  FRPG_Water_HeightMap  samples ONE tileable 128x128 wave height texture at three
                        UV layers, layer = mesh_uv * c141.{x,y,z} + c246/c247/c248
                        (VS), and writes h = sum(c134.i * layer_i) * fade / 3 (PS);
                        the offsets grow with time.
  FRPG_Water_Reflect*   turns that height field into a normal, then mixes refraction
                        (scene colour) and planar reflection by Fresnel. The normal
                        tilts by c126.x * dh / (2 * span), dh taken between points
                        offset along tangent/bitangent by span = depth * c142.x * 16 / c142.w
                        (FRPG_Water VS). The span grows with depth: a blur of a
                        constant ~48 pixels at 4K, which leaves only broad swells
                        on distant water. The reflection is not traced: the planar
                        reflection texture is looked up at screen UV + normal.xz * c124,
                        a far smaller wobble than a physically bent ray, so the slopes
                        are scaled by c124 * fov_x / 2 for Remix's traced reflection.

The proxy draws every water surface with one generated stand-in albedo
(ffp_state::setup_water_albedo, 8x8 of 0xFF2F5D73), so a single replacement
material keyed on that texture's Remix hash covers all water. This script writes:
  DATA/rtx-remix/mods/ds1_water/mod.usda    translucent material (AperturePBR_Translucent)
  DATA/rtx-remix/mods/ds1_water/water_normal.dds
                                            the wave height texture as a full-detail
                                            unsigned octahedral normal map (R16G16),
                                            TILES x TILES repeats (the layers repeat
                                            7-10 times per mesh UV), wave height in
                                            metres from c126/c134, mips by normal averaging
and prints the rtx.conf lines: the stand-in hash as animated water, and both layers' LOD
bias set to the game's normal span over a pixel's footprint, so the mip chain blurs the
waves the way the game does at every distance. Where Remix samples the two normal layers
and how strongly each tilts comes from each water surface's own height pass at runtime
(the proxy's game/texture_scroll.cpp, which holds this build's TILES and reference slope);
the layer velocities printed from the two --speed captures are for reference only.
"""
import argparse
import math
import struct
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "lookdev"))
import analyze as A  # noqa: E402

GAME_DIR = Path(r"<game dir>\DATA")
MOD_DIR = GAME_DIR / "rtx-remix" / "mods" / "ds1_water"
FPS = 60.0  # the capture frame index counts presents; DS1 runs its water at 60

# Remix hash (XXH3-64 of mip 0) of the proxy's water stand-in albedo.
STANDIN_PIXEL = 0xFF2F5D73


def standin_hash():
    import xxhash
    return xxhash.xxh3_64_intdigest(struct.pack("<I", STANDIN_PIXEL) * 64)


def find_draw(capture, vs_name, exact=False):
    for d in A.replay_state(A.resolve(capture)):
        name = A.shader_name(d.shaders.get(d.vs, b"")) or ""
        if name == vs_name if exact else name.startswith(vs_name):
            return d
    sys.exit(f"{capture}: no {vs_name} draw (is water on screen?)")


def frame_index(capture):
    hdr = A.resolve(capture).read_bytes()[:200]
    return struct.unpack_from("<21IQ", hdr, 0)[21]


def octahedral(n):
    """hemisphereDirectionToUnsignedOctahedral (Remix utility/packing.slangh)."""
    p = n[..., :2] / (np.abs(n[..., 0]) + np.abs(n[..., 1]) + n[..., 2])[..., None]
    s = np.stack([p[..., 0] + p[..., 1], p[..., 0] - p[..., 1]], axis=-1)
    return s * 0.5 + 0.5


def normals_from_height(h_m, texel_m):
    """Unit normals of a tiling height field in metres, central differences with wrap."""
    dx = (np.roll(h_m, -1, axis=1) - np.roll(h_m, 1, axis=1)) / (2 * texel_m)
    dy = (np.roll(h_m, -1, axis=0) - np.roll(h_m, 1, axis=0)) / (2 * texel_m)
    n = np.stack([-dx, -dy, np.ones_like(h_m)], axis=-1)
    return n / np.linalg.norm(n, axis=-1, keepdims=True)


def write_dds(path, levels):
    """R16G16_UNORM (DX10 header) with a full mip chain; levels are HxWx2 octahedral in [0,1].
    16 bits because gentle water slopes span only a few 8-bit steps around the centre,
    and those steps facet the surface into a fine regular pattern."""
    h, w = levels[0].shape[:2]
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x8 | 0x20000  # caps height width pixelformat pitch mipcount
    header = struct.pack("<4sIIIIIII44x", b"DDS ", 124, flags, h, w, w * 4, 0, len(levels))
    pixelformat = struct.pack("<II4sIIIII", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    caps = struct.pack("<IIII4x", 0x1000 | 0x8 | 0x400000, 0, 0, 0)
    dx10 = struct.pack("<IIIII", 35, 3, 0, 1, 0)  # DXGI_FORMAT_R16G16_UNORM, TEXTURE2D, array 1
    body = bytearray()
    for lv in levels:
        body += np.clip(np.rint(lv * 65535.0), 0, 65535).astype("<u2").tobytes()
    path.write_bytes(header + pixelformat + caps + dx10 + bytes(body))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", help="capture with water on screen (texture, layer scales, constants)")
    ap.add_argument("--speed", nargs=2, metavar=("A", "B"), required=True,
                    help="two captures of the same water some seconds apart (layer velocities)")
    ap.add_argument("--tiles", type=int, default=8, help="wave tile repeats per mesh UV (layers use 7, 9, 10)")
    ap.add_argument("--metres-per-uv", type=float, default=10.0,
                    help="world size of one water mesh UV (measured on m12 water)")
    ap.add_argument("--render-width", type=float, default=None,
                    help="Remix render width for the LOD bias (default: the capture's back buffer)")
    ap.add_argument("--reflection-match", type=float, default=None,
                    help="slope scale for the traced reflection (default: c124 * fov_x / 2)")
    ap.add_argument("--out", type=Path, default=MOD_DIR)
    a = ap.parse_args()

    hm = find_draw(a.capture, "FRPG_Water_HeightMap")
    # Water bodies carry their own c124-c126 (m12: 0.2 on a small pool, 0.7 on the
    # river); one stand-in material serves them all, so take the largest surface.
    surfaces = [d for d in A.replay_state(A.resolve(a.capture))
                if (A.shader_name(d.shaders.get(d.vs, b"")) or "") == "FRPG_Water"]
    if not surfaces:
        sys.exit(f"{a.capture}: no FRPG_Water surface draw")
    ws = max(surfaces, key=lambda d: d.count)
    found = A.texture_image(a.capture, hex(hm.tex.get(2)))
    if not found:
        sys.exit(f"wave texture {hex(hm.tex.get(2))} has no data in {a.capture}")
    img, label, _ = found
    scales = hm.vsf[141][:3]
    weights = hm.psf[134][:3]
    c126 = ws.psf[126][0]
    c142 = ws.vsf[142]
    print(f"wave height texture {label} {img.size[0]}x{img.size[1]}; layer scales {scales}, weights {tuple(round(w, 3) for w in weights)}, c126 {c126:g}")

    # layer velocities (layer UV / s) -> pattern speed in mesh UV / s
    da, db = find_draw(a.speed[0], "FRPG_Water_HeightMap"), find_draw(a.speed[1], "FRPG_Water_HeightMap")
    seconds = (frame_index(a.speed[1]) - frame_index(a.speed[0])) / FPS
    vel = []
    for i, reg in enumerate((246, 247, 248)):
        v = [(db.vsf[reg][k] - da.vsf[reg][k]) / seconds for k in (0, 1)]
        vel.append((v[0] / scales[i], v[1] / scales[i]))
        print(f"layer {i + 1}: {v[0]:+.4f} {v[1]:+.4f} layer UV/s -> {vel[-1][0]:+.5f} {vel[-1][1]:+.5f} mesh UV/s")

    # Wave height in metres. The game tilts its normal by c126 * dh / (2 * span) with
    # h = sum(w_i * layer_i) / 3, i.e. a height field of c126 / 2 * h metres. Remix
    # has two layers (combined by RNM, slopes adding) for the game's three; each gets
    # the RMS share of all three weights.
    layer_weight = math.sqrt(sum(w * w for w in weights) / 2.0)
    height_scale_m = c126 / 2.0 * layer_weight / 3.0
    # The game does not trace its reflection: it shifts the reflection texture lookup by
    # normal.xz * c124 in screen UV, i.e. by c124 * normal * fov_x radians. Remix bends the
    # reflected ray by 2 * normal, so distant reflected scenery moves 2 / (c124 * fov_x)
    # times further for the same normal. Scaling the slopes by c124 * fov_x / 2 gives the
    # game's reflection wobble back.
    sx = float(np.linalg.norm(ws.vsf[0][:3]))
    fov_x = 2.0 * math.atan(1.0 / sx)
    reflection_match = a.reflection_match if a.reflection_match is not None else ws.psf[124][0] * fov_x / 2.0
    height_scale_m *= reflection_match
    print(f"reflection match: c124 {ws.psf[124][0]:g}, fov_x {math.degrees(fov_x):.1f} deg -> slope scale {reflection_match:.3f}")
    tex = np.asarray(img.convert("RGBA"), dtype=np.float64)[..., 0] / 255.0  # the height pass samples .x
    tile_texels = tex.shape[0]
    h_m = np.tile(tex, (a.tiles, a.tiles)) * height_scale_m
    texel_m = a.metres_per_uv / (a.tiles * tile_texels)
    n = normals_from_height(h_m, texel_m)
    slope = np.sqrt(np.mean(1.0 / n[..., 2] ** 2 - 1.0))
    print(f"wave height {height_scale_m * 100:.2f} cm per unit; texel {texel_m * 100:.2f} cm; RMS slope at mip 0 {slope:.3f}")
    levels = []
    while True:
        levels.append(octahedral(n))
        if n.shape[0] == 1:
            break
        n = n.reshape(n.shape[0] // 2, 2, n.shape[1] // 2, 2, 3).mean(axis=(1, 3))
        n /= np.linalg.norm(n, axis=-1, keepdims=True)

    # LOD bias: the game's normal span per metre of depth over a pixel's footprint per
    # metre of depth (2 / (Sx * width), Sx = |ViewProj row 0|).
    span_per_m = c142[0] * 16.0 / c142[3]
    width = a.render_width or 3840.0
    pixel_per_m = 2.0 / (sx * width)
    lod_bias = math.log2(span_per_m / pixel_per_m)
    print(f"normal span {span_per_m * 100:.2f} cm per m of depth = {span_per_m / pixel_per_m:.0f} px at width {width:g} -> LOD bias {lod_bias:.2f}")

    a.out.mkdir(parents=True, exist_ok=True)
    write_dds(a.out / "water_normal.dds", levels)

    mat = f"mat_{standin_hash():016X}"
    (a.out / "mod.usda").write_text(f'''#usda 1.0
(
    customLayerData = {{
        string lightspeed_game_name = "DarkSouls1"
        string lightspeed_layer_type = "replacement"
        string lightspeed_mod_name = "ds1_water"
        string lightspeed_mod_notes = """Translucent water for the proxy's water stand-in albedo. Normal map = the game's wave height texture ({label}), {a.tiles}x{a.tiles} tiles, height from c126/c134; scrolled and blurred by rtx.translucentMaterial.animatedWater*. Built by scripts/gen_water_material.py."""
        string lightspeed_mod_version = "1.1"
    }}
    upAxis = "Y"
)

over "RootNode"
{{
    over "Looks"
    {{
        over "{mat}"
        {{
            over "Shader"
            {{
                uniform token info:implementationSource = "sourceAsset"
                uniform asset info:mdl:sourceAsset = @AperturePBR_Translucent.mdl@
                uniform token info:mdl:sourceAsset:subIdentifier = "AperturePBR_Translucent"
                asset inputs:normalmap_texture = @./water_normal.dds@ (
                    colorSpace = "raw"
                )
                float inputs:ior_constant = 1.33
                color3f inputs:transmittance_color = (0.866, 0.866, 0.866)
                float inputs:transmittance_measurement_distance = 1.5
                bool inputs:thin_walled = 0
            }}
        }}
    }}
}}
''')
    print(f"wrote {a.out / 'mod.usda'} ({mat}) and water_normal.dds ({h_m.shape[1]}x{h_m.shape[0]}, {len(levels)} mips)")
    print()
    print("rtx.conf:")
    print(f"rtx.animatedWaterTextures = 0x{standin_hash():016X}")
    print("rtx.translucentMaterial.animatedWaterEnable = True")
    print(f"rtx.translucentMaterial.animatedWaterPrimaryNormalMotion = {vel[0][0]:.5f}, {vel[0][1]:.5f}")
    print(f"rtx.translucentMaterial.animatedWaterSecondaryNormalMotion = {vel[1][0]:.5f}, {vel[1][1]:.5f}")
    print(f"rtx.translucentMaterial.animatedWaterPrimaryNormalLodBias = {lod_bias:.2f}")
    print(f"rtx.translucentMaterial.animatedWaterSecondaryNormalLodBias = {lod_bias:.2f}")


if __name__ == "__main__":
    main()
