"""Cut meshes for the game's alpha-tested (cut-out) map geometry: assets/mesh_tables/cut_meshes.bin.

A cut-out mesh (an MTD with _Edge: grass, leaves, branches) is a set of cards whose texture alpha says where the card is
there. Path traced, a ray that crosses a card where it is clear still finds the card, has its alpha looked up and goes on
to the next one. A cut mesh is the same card cut along its texture's alpha: only the part round the opaque texels is left
as triangles, so a ray through a gap between leaves meets nothing.

For each cut-out mesh of the maps given, each triangle is clipped in UV space against the texture's opaque region
(alpha above the game's reference 128, traced at --trace texels and grown by --tolerance texels of that trace):
  - a triangle with no opaque texel is dropped,
  - one that is opaque nearly all over (--keep) stays as it is,
  - the rest is replaced by a triangulation of its opaque part.
The cut is never inside the opaque texels (a trace texel counts as opaque when any texel under it is), so with the alpha
test kept on the cut mesh the picture is the game's; without it the cut's outline shows.
New vertices are written as a blend of the source triangle's three (indices into the mesh's vertex array, which is the
draw's from its base vertex, and two weights), so the proxy builds them from whatever vertices the draw carries.

The table (little endian): 'DS1CUT02', u32 mesh count; per mesh u64 FNV-1a of the face set's index bytes, u32 index
count, u32 new vertex count, u32 new triangle count, u16 name length, the name ('<map>/<model>/mesh<n>/lod<n> <texture>'),
then per vertex u32 i0, i1, i2, f32 w0, w1 (w2 = 1 - w0 - w1), then per triangle 3 x u32. Each face set (LOD) of a mesh
has its own entry. Two meshes with the same index bytes and different cuts are both left out (the proxy knows a mesh by
its indices alone).

    python gen_cut_meshes.py m10_02_00_00 [more maps] [--trace 64] [--trace-for TEXTURE=TRACE ...] [--tolerance 1.0]
                             [--keep 0.97] [--only TEXTURE ...] [--max-growth 400] [--out FILE] [--preview DIR]
"""
import os  # noqa: E402  (ds1paths: the local folders, see paths.example.ini)
import sys  # noqa: E402
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)))))
import ds1paths  # noqa: E402

import argparse
import collections
import os
import struct
import sys
import time

import numpy as np
import shapely
import shapely.affinity
from PIL import Image, ImageDraw
from shapely.geometry import Polygon, box
from shapely.ops import unary_union

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ds1paths.SCRIPTS)
from bbformats.flver import read_flver  # noqa: E402
from map_geometry import first, map_pieces  # noqa: E402

DATA = ds1paths.GAME + "/DATA/unpacked/map"
ALPHA_REF = 128  # the game's cut-out draws test alpha > 128


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def polygons(geometry):
    if geometry.is_empty:
        return []
    if geometry.geom_type == "Polygon":
        return [geometry]
    return [p for g in getattr(geometry, "geoms", []) for p in polygons(g)]


def opaque_region(texture_path, trace, tolerance):
    """The texture's opaque area as a polygon in UV space (u right, v down, both 0 to 1), and its share of the texture."""
    alpha = np.asarray(Image.open(texture_path).convert("RGBA"))[..., 3] > ALPHA_REF
    h, w = alpha.shape
    # A trace texel is opaque when any texel under it is: thin stems survive, and the cut never eats into a leaf.
    step = max(1, max(w, h) // trace)
    th, tw = max(1, h // step), max(1, w // step)
    mask = alpha[:th * step, :tw * step].reshape(th, step, tw, step).any(axis=(1, 3))
    boxes = []
    for y in range(th):
        row = np.flatnonzero(np.diff(np.concatenate(([0], mask[y].astype(np.int8), [0]))))
        for x0, x1 in zip(row[0::2], row[1::2]):
            boxes.append(box(x0 / tw, y / th, x1 / tw, (y + 1) / th))
    if not boxes:
        return Polygon(), 0.0
    # Outwards by the tolerance before simplifying, so the simplified outline stays outside the opaque texels.
    grow = tolerance / max(tw, th)
    region = unary_union(boxes).buffer(grow, join_style="mitre").simplify(grow, preserve_topology=True).buffer(0)
    return region.intersection(box(0, 0, 1, 1)), float(alpha.mean())


class Cutter:
    """Cuts UV triangles against one texture's opaque region. Cards repeat the same few UV triangles thousands of times,
    so each distinct one is cut once."""

    def __init__(self, region, keep):
        self.region = region
        self.keep = keep
        self.tiled = {}
        self.cache = {}
        shapely.prepare(self.region)

    def target(self, cells):
        if cells == (0, 0, 0, 0):
            return self.region
        if cells not in self.tiled:
            self.tiled[cells] = unary_union([shapely.affinity.translate(self.region, x, y)
                                             for x in range(cells[0], cells[2] + 1) for y in range(cells[1], cells[3] + 1)])
            shapely.prepare(self.tiled[cells])
        return self.tiled[cells]

    def cut(self, a, b, c):
        """'whole', 'none', or (points as barycentric weights (n, 3), triangles (m, 3) into them)."""
        # A tiling texture: bring the triangle to the tile its centre lies in, and tile the region as far as it reaches.
        shift = np.floor((a + b + c) / 3.0)
        a, b, c = a - shift, b - shift, c - shift
        key = tuple(np.round(np.concatenate((a, b, c)), 5))
        if key in self.cache:
            return self.cache[key]
        self.cache[key] = result = self._cut(a, b, c)
        return result

    def _cut(self, a, b, c):
        area = 0.5 * ((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]))
        if abs(area) < 1e-12:
            return "whole"
        lo = np.floor(np.minimum(np.minimum(a, b), c) + 1e-6).astype(int)
        hi = np.floor(np.maximum(np.maximum(a, b), c) - 1e-6).astype(int)
        target = self.target((int(lo[0]), int(lo[1]), int(hi[0]), int(hi[1])))
        tri = Polygon([a, b, c])
        if not target.intersects(tri):
            return "none"
        part = target.intersection(tri)
        if part.area >= self.keep * tri.area:
            return "whole"
        # Barycentric weights of a UV point in the triangle.
        m = np.linalg.inv(np.array([[a[0], b[0], c[0]], [a[1], b[1], c[1]], [1.0, 1.0, 1.0]]))
        points, index, triangles = [], {}, []
        for poly in polygons(part):
            if poly.area < 1e-9:
                continue
            for piece in polygons(shapely.constrained_delaunay_triangles(poly)):
                ring = np.asarray(piece.exterior.coords)[:3]
                piece_area = 0.5 * ((ring[1][0] - ring[0][0]) * (ring[2][1] - ring[0][1]) -
                                    (ring[1][1] - ring[0][1]) * (ring[2][0] - ring[0][0]))
                if abs(piece_area) < 1e-10:
                    continue
                if (piece_area > 0) != (area > 0):  # keep the source triangle's winding
                    ring = ring[[0, 2, 1]]
                new = []
                for p in ring:
                    k = (round(float(p[0]), 6), round(float(p[1]), 6))
                    if k not in index:
                        w = np.clip(m @ np.array([p[0], p[1], 1.0]), 0.0, 1.0)
                        index[k] = len(points)
                        points.append(w / w.sum())
                    new.append(index[k])
                triangles.append(new)
        if not triangles:
            return "none"
        return np.asarray(points), np.asarray(triangles)


def cut_mesh(uv, triangles, cutter):
    """(vertices as (i0, i1, i2, w0, w1), triangles) of the cut mesh, and how many source triangles went which way."""
    vertices, out = [], []
    corner = {}  # source vertex -> new vertex
    counts = collections.Counter()

    def corner_vertex(i):
        if i not in corner:
            corner[i] = len(vertices)
            vertices.append((i, i, i, 1.0, 0.0))
        return corner[i]

    for ia, ib, ic in triangles:
        ia, ib, ic = int(ia), int(ib), int(ic)
        result = cutter.cut(uv[ia], uv[ib], uv[ic])
        if isinstance(result, str):
            counts[result] += 1
            if result == "whole":
                out.append((corner_vertex(ia), corner_vertex(ib), corner_vertex(ic)))
            continue
        counts["cut"] += 1
        points, pieces = result
        new = []
        for w in points:
            if w.max() > 0.9999:
                new.append(corner_vertex((ia, ib, ic)[int(w.argmax())]))
            else:
                new.append(len(vertices))
                vertices.append((ia, ib, ic, float(w[0]), float(w[1])))
        out.extend((new[p[0]], new[p[1]], new[p[2]]) for p in pieces)
    return vertices, out, counts


def preview(texture_path, region, out_path):
    """The texture over magenta with the cut's outline, to judge the cut by eye."""
    im = Image.open(texture_path).convert("RGBA")
    scale = max(1, 1024 // max(im.size))
    im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
    canvas = Image.new("RGBA", im.size, (200, 0, 200, 255))
    canvas.alpha_composite(im)
    draw = ImageDraw.Draw(canvas)
    for poly in polygons(region):
        for ring in [poly.exterior, *poly.interiors]:
            draw.line([(x * im.width, y * im.height) for x, y in ring.coords], fill=(255, 255, 0, 255), width=1)
    canvas.convert("RGB").save(out_path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("maps", nargs="+")
    ap.add_argument("--trace", type=int, default=64)
    ap.add_argument("--trace-for", nargs="*", default=[], help="TEXTURE=TRACE: another trace for one texture")
    ap.add_argument("--tolerance", type=float, default=1.0)
    ap.add_argument("--keep", type=float, default=0.97)
    ap.add_argument("--only", nargs="*", default=[], help="texture names; the others are left as the game draws them")
    ap.add_argument("--meshes", nargs="*", default=[], help="'<model>/mesh<n>' names; only these meshes")
    ap.add_argument("--max-growth", type=float, default=400.0, help="a mesh that would grow past this many times is left out")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "mesh_tables", "cut_meshes.bin"))
    ap.add_argument("--preview", default="")
    a = ap.parse_args()

    cutters = {}
    entries = {}
    clashes = set()
    totals = collections.Counter()
    by_texture = collections.defaultdict(collections.Counter)
    start = time.time()
    for map_id in a.maps:
        area = map_id[1:3]
        models = sorted({m for m, *_ in map_pieces(os.path.join(DATA, "MapStudio", f"{map_id}.msb"))})
        for model in models:
            path = os.path.join(DATA, map_id, f"{model}A{area}.flver")
            if not os.path.exists(path):
                continue
            fl = read_flver(open(path, "rb").read())
            for index, mesh in enumerate(fl.meshes):
                material = fl.materials[mesh.material_index]
                if "_edge" not in os.path.basename(material.mtd).lower():
                    continue
                name = os.path.splitext(os.path.basename(material.textures[0].path))[0]
                if a.only and name not in a.only:
                    continue
                if a.meshes and f"{model}/mesh{index}" not in a.meshes:
                    continue
                texture = os.path.join(DATA, f"m{area}", name + ".dds")
                if not os.path.exists(texture):
                    print(f"  {model} mesh{index}: no texture {name}, skipped")
                    continue
                if texture not in cutters:
                    trace = int(next((t.split("=")[1] for t in a.trace_for if t.split("=")[0] == name), a.trace))
                    region, share = opaque_region(texture, trace, a.tolerance)
                    cutters[texture] = (Cutter(region, a.keep), share)
                    if a.preview:
                        os.makedirs(a.preview, exist_ok=True)
                        preview(texture, region, os.path.join(a.preview, f"{name}_trace{trace}.png"))
                cutter, share = cutters[texture]
                uv = np.asarray(first(mesh.vertices["uv"]), dtype=float)[:, :2] * np.asarray(material.textures[0].scale, dtype=float)
                for lod, face_set in enumerate(mesh.face_sets):
                    indices = np.asarray(face_set.indices, dtype="<u2")
                    key = fnv1a64(indices.tobytes())
                    triangles = np.asarray(face_set.triangles()).reshape(-1, 3)
                    triangles = triangles[(triangles < len(uv)).all(1)]
                    if not len(triangles):
                        continue
                    vertices, out, counts = cut_mesh(uv, triangles, cutter)
                    label = f"{map_id}/{model}/mesh{index}/lod{lod} {name}"
                    if key in entries:
                        if entries[key][1] != vertices or entries[key][2] != out:
                            clashes.add(key)
                            print(f"  CLASH {label} has the indices of {entries[key][3]} and another cut: both left out")
                        continue
                    if len(out) > a.max_growth * len(triangles):
                        print(f"  {label}: {len(triangles)} -> {len(out)} triangles, past --max-growth, left out")
                        continue
                    entries[key] = (len(indices), vertices, out, label)
                    if lod == 0:
                        totals["meshes"] += 1
                        totals["source triangles"] += len(triangles)
                        totals["cut triangles"] += len(out)
                        totals["cut vertices"] += len(vertices)
                        totals.update(counts)
                        by_texture[name].update({"meshes": 1, "source": len(triangles), "cut": len(out)})
                        print(f"  {model} mesh{index:<3} {name:18} opaque {share:4.0%}, cut area {cutter.region.area:4.0%}: "
                              f"{len(triangles):>6} -> {len(out):>7} triangles ({counts['whole']} whole, {counts['cut']} cut, {counts['none']} dropped)")
    for key in clashes:
        entries.pop(key, None)
    with open(a.out, "wb") as f:
        f.write(b"DS1CUT02" + struct.pack("<I", len(entries)))
        for key, (index_count, vertices, out, label) in sorted(entries.items()):
            name = label.encode("ascii", "replace")
            f.write(struct.pack("<QIIIH", key, index_count, len(vertices), len(out), len(name)) + name)
            packed = np.zeros(len(vertices), dtype=[("i", "<u4", 3), ("w", "<f4", 2)])
            packed["i"] = [v[:3] for v in vertices]
            packed["w"] = [v[3:] for v in vertices]
            f.write(packed.tobytes())
            f.write(np.asarray(out, dtype="<u4").reshape(-1, 3).tobytes())
    print("per texture (LOD 0):")
    for name, c in sorted(by_texture.items(), key=lambda kv: -kv[1]["cut"]):
        print(f"  {name:18} {c['meshes']:>3} meshes {c['source']:>7} -> {c['cut']:>8} triangles")
    print(dict(totals))
    print(f"{len(entries)} face sets in {os.path.normpath(a.out)}, {os.path.getsize(a.out) / 1e6:.1f} MB, {time.time() - start:.0f} s")


if __name__ == "__main__":
    main()
