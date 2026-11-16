"""Builds the Remix material for the Blighttown swamp from the game's own textures.

  python scripts/gen_swamp_material.py <capture with the swamp on screen>

DS1 draws the swamp with FRPG_Snow (see the runtime's fork_ds1_swamp.h): its colour texture
(m14_mist, stage 0) is both the surface's crust colour and, in three layers, its height field;
the tar detail normal (m14_detail_tar_n, stage 5) bumps it at 10x the mesh UV. The proxy tags the
draw and Remix shades it from the height field itself; this mod gives that material its textures:
  DATA/rtx-remix/mods/ds1_swamp/mod.usda       replacement material keyed on the colour texture's hash
  DATA/rtx-remix/mods/ds1_swamp/swamp_colour.dds
                                               the colour texture as stored (R8G8B8A8, raw), so the
                                               runtime reads the values the game's height pass reads;
                                               alpha = the wake ring texture the WaterWave draws stamp
                                               (s00019, red * alpha as their shader reads it; the swamp
                                               draw is opaque, so Remix never reads alpha as opacity)
  DATA/rtx-remix/mods/ds1_swamp/swamp_tar_normal.dds
                                               the tar normal map, x/y = the game's (rg * 2 - 1),
                                               as normalize(x, y, 1) in unsigned octahedral R16G16
and prints the mesh's metres per UV (rtx.ds1Swamp.metresPerUv), measured from its vertices.
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

GAME_DIR = Path(ds1paths.GAME + r"\DATA")
MOD_DIR = GAME_DIR / "rtx-remix" / "mods" / "ds1_swamp"
SNOW_PS = "FRPG_Snow____"


def mip0_bytes(capture, tid):
    """The texture's level 0 as the game uploaded it (what Remix hashes)."""
    for t, p in A.records(A.resolve(capture)):
        if t == 32:
            rid, flags, face, level, rows, row_bytes, depth = struct.unpack_from("<QIIIIII", p)
            if rid == tid and level == 0 and face == 0:
                return bytes(p[32:32 + rows * row_bytes])
    sys.exit(f"texture {tid:#x} has no level 0 data in {capture}")


def swamp_draw(capture):
    best = None
    for d in A.replay_state(A.resolve(capture)):
        name = A.shader_name(d.shaders.get(d.ps, b"")) or ""
        if name.startswith(SNOW_PS) and (best is None or d.count > best.count):
            best = d
    if best is None:
        sys.exit(f"{capture}: no {SNOW_PS}* draw (is the swamp on screen?)")
    return best


def ring_texture(capture):
    """The WaterWave draws' stage-0 texture, as their pixel shader reads it (r * a)."""
    for d in A.replay_state(A.resolve(capture)):
        name = A.shader_name(d.shaders.get(d.ps, b"")) or ""
        if name == "FRPG_WWS_Dif________________WaterWave":
            found = A.texture_image(capture, hex(d.tex.get(0)))
            if found:
                img, label, _ = found
                a = np.asarray(img.convert("RGBA"), dtype=np.float64) / 255.0
                return a[..., 0] * a[..., 3], label
    sys.exit(f"{capture}: no WaterWave draw with its ring texture (wade into the swamp for the capture)")


def metres_per_uv(capture, draw_index):
    """Fits the swamp mesh's UV against world x, z (one of each, the mapping is planar)."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from swamp_mesh import mesh_vertices  # noqa: E402
    world, uv = mesh_vertices(capture, draw_index)
    xz = world[:, [0, 2]]
    best = []
    for k in (0, 1):
        # Pieces of the mesh carry different UV offsets, so fit the slope by differences between neighbours.
        order = np.argsort(xz[:, 0] + xz[:, 1] * 1e-3)
        dx = np.diff(xz[order], axis=0)
        du = np.diff(uv[order, k])
        keep = np.abs(du) < 2.0
        slope, *_ = np.linalg.lstsq(dx[keep], du[keep], rcond=None)
        best.append(float(np.linalg.norm(slope)))
    return 1.0 / np.mean(best)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", help="capture with the swamp on screen")
    ap.add_argument("--out", type=Path, default=MOD_DIR)
    a = ap.parse_args()

    import xxhash
    d = swamp_draw(a.capture)
    colour_id, normal_id = d.tex.get(0), d.tex.get(5)
    found = A.texture_image(a.capture, hex(colour_id))
    if not found:
        sys.exit(f"colour texture {colour_id:#x} has no data in {a.capture}")
    colour, colour_label, _ = found
    found = A.texture_image(a.capture, hex(normal_id))
    if not found:
        sys.exit(f"tar normal texture {normal_id:#x} has no data in {a.capture}")
    tar, tar_label, _ = found
    colour_hash = xxhash.xxh3_64_intdigest(mip0_bytes(a.capture, colour_id))
    print(f"swamp draw: {d.count} prims; colour {colour_label} {colour.size} hash {colour_hash:016X}; tar normal {tar_label} {tar.size}")

    a.out.mkdir(parents=True, exist_ok=True)

    # Colour: RGBA8 UNORM with box-filtered mips, the ring texture in alpha.
    c = np.asarray(colour.convert("RGBA"), dtype=np.float64) / 255.0
    ring, ring_label = ring_texture(a.capture)
    if ring.shape != c.shape[:2]:
        from PIL import Image
        ring = np.asarray(Image.fromarray((ring * 255).astype(np.uint8)).resize(c.shape[1::-1], Image.BILINEAR), np.float64) / 255.0
    c[..., 3] = ring
    levels = []
    while True:
        levels.append(c)
        if c.shape[0] == 1 or c.shape[1] == 1:
            break
        c = c.reshape(c.shape[0] // 2, 2, c.shape[1] // 2, 2, 4).mean(axis=(1, 3))
    h, w = levels[0].shape[:2]
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x8 | 0x20000
    header = struct.pack("<4sIIIIIII44x", b"DDS ", 124, flags, h, w, w * 4, 0, len(levels))
    pixelformat = struct.pack("<II4sIIIII", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    caps = struct.pack("<IIII4x", 0x1000 | 0x8 | 0x400000, 0, 0, 0)
    dx10 = struct.pack("<IIIII", 28, 3, 0, 1, 0)  # DXGI_FORMAT_R8G8B8A8_UNORM
    body = b"".join(np.clip(np.rint(lv * 255.0), 0, 255).astype(np.uint8).tobytes() for lv in levels)
    (a.out / "swamp_colour.dds").write_bytes(header + pixelformat + caps + dx10 + body)

    # Tar normal: the game reads xy = rg * 2 - 1 and adds them to its normal as tangent offsets.
    t = np.asarray(tar.convert("RGBA"), dtype=np.float64) / 255.0
    n = np.stack([t[..., 0] * 2.0 - 1.0, t[..., 1] * 2.0 - 1.0, np.ones(t.shape[:2])], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    normal_levels = []
    while True:
        normal_levels.append(octahedral(n))
        if n.shape[0] == 1 or n.shape[1] == 1:
            break
        n = n.reshape(n.shape[0] // 2, 2, n.shape[1] // 2, 2, 3).mean(axis=(1, 3))
        n /= np.linalg.norm(n, axis=-1, keepdims=True)
    write_dds(a.out / "swamp_tar_normal.dds", normal_levels)

    mat = f"mat_{colour_hash:016X}"
    (a.out / "mod.usda").write_text(f'''#usda 1.0
(
    customLayerData = {{
        string lightspeed_game_name = "DarkSouls1"
        string lightspeed_layer_type = "replacement"
        string lightspeed_mod_name = "ds1_swamp"
        string lightspeed_mod_notes = """The Blighttown swamp (FRPG_Snow): its colour texture ({colour_label}) as stored, which the runtime also reads as the game's height field (rtx.ds1Swamp), with the wake ring texture ({ring_label}) in alpha, and the tar detail normal ({tar_label}). Built by scripts/gen_swamp_material.py."""
        string lightspeed_mod_version = "1.0"
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
                uniform asset info:mdl:sourceAsset = @AperturePBR_Opacity.mdl@
                uniform token info:mdl:sourceAsset:subIdentifier = "AperturePBR_Opacity"
                asset inputs:diffuse_texture = @./swamp_colour.dds@ (
                    colorSpace = "raw"
                )
                asset inputs:normalmap_texture = @./swamp_tar_normal.dds@ (
                    colorSpace = "raw"
                )
            }}
        }}
    }}
}}
''')
    print(f"wrote {a.out / 'mod.usda'} ({mat}), swamp_colour.dds ({len(levels)} mips), swamp_tar_normal.dds ({len(normal_levels)} mips)")
    print(f"rtx.ds1Swamp.metresPerUv = {metres_per_uv(a.capture, d.index):.3f}")


if __name__ == "__main__":
    main()
