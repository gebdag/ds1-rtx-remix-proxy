"""Generate assets/ffx/flame_lift.tsv: how far a light that stands at a placed flame must be raised to get out of what holds the flame.

The game puts a flame's light at the effect's origin, and its lights cast no shadows. Path traced, a dish, cup or sconce round the
origin shadows the light: the four flame dishes of the Kiln's brazier hold their lights 4 cm under the rim, and nothing round them
was lit (frame_20261111_142320: the knight beside one read 3.6 of 255, the game's 27).

For each placed flame (assets/ffx/map_flames.tsv; the maps' Light events stand on these same spots), rays go out all round (32 directions,
level and 25 and 45 degrees down, 1.5 m long: towards where people stand and the ground round the holder) from the spot and from
the spot raised in steps of 5 cm, against the map pieces and the placed objects. A spot is shut in when raising it frees a quarter
of its rays or more. Its lift is the smallest rise that frees them (within 0.08 of the best a rise up to 1 m gives), and never
more than:
  - the centre height of that flame's fire (measured from captures: assets/ffx/flame_heights.tsv, and the exact ffx_sizes.tsv
    where it has the effect, the lower of the two): the light stays in the lower half of its own fire, so it cannot hang over it;
  - half a metre, whatever the fire;
  - a rise that leaves 15 cm free above the raised spot (not up under a lid or a ceiling);
  - a rise that crosses no surface straight above, but for what lies in the first 10 cm: a wall torch's light sits inside the
    torch's head and leaves it there, while a fire inside a hearth or a cage must not come out through its top (the Burg's
    FFX 100101 would have risen 0.95 m). The test is one ray straight up: a grate off that line does not show in it.
Placed objects count as fixed geometry (Blighttown's scaffolds are objects); one that can be broken away leaves its light 5 to 20 cm
up in the fire, which is where a torch's light may stand in any case.
Where the fire is too low to clear the holder, the lift goes as far as the fire allows if that still frees a quarter of the rays,
and is none otherwise (a candle deep in a niche stays where it is: the niche is its room).

Usage: py -3.12 scripts/gen_flame_lift.py [map ...] [--detail] [--point "map,x,y,z"]
"""
import os  # noqa: E402  (ds1paths: the local folders, see paths.example.ini)
import sys  # noqa: E402
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)))))
import ds1paths  # noqa: E402

import os
import struct
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, ds1paths.SCRIPTS)
from bbformats.flver import read_flver  # noqa: E402
from gen_map_lights import MSB_DIR, read_events  # noqa: E402
from map_geometry import _section, _sjis, blocks_rays, first, rotation, table_sources  # noqa: E402
from embreex import rtcore_scene  # noqa: E402
from embreex.mesh_construction import TriangleMesh  # noqa: E402

DATA = Path(ds1paths.GAME + r"\DATA\unpacked")
ASSETS = HERE.parent / "assets"
OUT = ASSETS / "ffx" / "flame_lift.tsv"
PART_MAP_PIECE, PART_OBJECT = 0, 1
STEP, TOP = 0.05, 1.0
REACH = 1.5
SHUT = 0.25        # share of rays a rise must free for the spot to count as shut in
PLATEAU = 0.08     # "freed": within this of the best any rise gives
HEADROOM = 0.15
SMALLEST = 0.05
EMBEDDED = 0.10    # a surface crossed within this of the spot is what the light is embedded in
HIGHEST = 0.5      # no light is raised further than this


def parts(msb_path):
    """(kind, model name, position, rotation in degrees, scale) of the map pieces and objects (MSB1 layout, as map_geometry)."""
    data = open(msb_path, "rb").read()
    model_offsets, pos = _section(data, 0)
    models = [_sjis(data, o + struct.unpack_from("<i", data, o)[0]) for o in model_offsets]
    _, pos = _section(data, pos)
    _, pos = _section(data, pos)
    part_offsets, _ = _section(data, pos)
    out = []
    for o in part_offsets:
        _, kind, _, model = struct.unpack_from("<iIii", data, o)
        if kind in (PART_MAP_PIECE, PART_OBJECT) and 0 <= model < len(models):
            out.append((kind, models[model], np.array(struct.unpack_from("<3f", data, o + 0x14), np.float64),
                        struct.unpack_from("<3f", data, o + 0x20), np.array(struct.unpack_from("<3f", data, o + 0x2C), np.float64)))
    return out


def scene_of(map_name, no_block):
    area = map_name[1:3]
    flvers, tris, objects = {}, [], 0
    for kind, model, at, rot, scale in parts(MSB_DIR / f"{map_name}.msb"):
        path = DATA / "map" / map_name / f"{model}A{area}.flver" if kind == PART_MAP_PIECE else DATA / "obj" / model / f"{model}.flver"
        if not path.exists():
            continue
        if model not in flvers:
            try:
                flvers[model] = read_flver(path.read_bytes())
            except Exception:  # noqa: BLE001
                flvers[model] = None
        flver = flvers[model]
        if flver is None:
            continue
        r = rotation(rot) * scale
        used = False
        for i, mesh in enumerate(flver.meshes):
            fs = mesh.lod0()
            if fs is None or "position" not in mesh.vertices:
                continue
            p = np.asarray(first(mesh.vertices["position"]), np.float64)[:, :3]
            t = fs.triangles()
            t = t[(t < len(p)).all(1)]
            mtd = os.path.basename(str(flver.materials[mesh.material_index].mtd))
            if not len(t) or not blocks_rays(mtd) or (kind == PART_MAP_PIECE and f"{map_name}/{model}A{area}/mesh{i}/lod0" in no_block):
                continue
            tris.append((p @ r.T + at)[t])
            used = True
        objects += used and kind == PART_OBJECT
    if not tris:
        return None, 0
    scene = rtcore_scene.EmbreeScene()
    TriangleMesh(scene, np.concatenate(tris).astype(np.float32))
    return scene, objects


AZ = np.linspace(0, 2 * np.pi, 32, endpoint=False)


def ring(elevation_deg):
    e = np.radians(elevation_deg)
    return np.c_[np.cos(AZ) * np.cos(e), np.full(len(AZ), np.sin(e)), np.sin(AZ) * np.cos(e)]


RAYS = np.concatenate([ring(0), ring(-25), ring(-45)]).astype(np.float32)
RISES = np.arange(0.0, TOP + 1e-6, STEP)


def measure(scene, spot):
    """(blocked share per rise, free height above the spot at each rise)."""
    origins = np.repeat(np.asarray(spot, np.float32)[None] + np.c_[np.zeros(len(RISES)), RISES, np.zeros(len(RISES))].astype(np.float32), len(RAYS), 0)
    res = scene.run(origins, np.tile(RAYS, (len(RISES), 1)), output=True)
    dist = np.asarray(res["tfar"]).astype(np.float64)
    dist[np.asarray(res["primID"]) < 0] = np.inf
    blocked = (dist < REACH).reshape(len(RISES), len(RAYS)).mean(1)
    starts = np.asarray(spot, np.float32)[None] + np.c_[np.zeros(len(RISES)), RISES, np.zeros(len(RISES))].astype(np.float32)
    up = scene.run(starts, np.tile(np.asarray([[0, 1, 0]], np.float32), (len(RISES), 1)), output=True)
    above = np.asarray(up["tfar"]).astype(np.float64)
    above[np.asarray(up["primID"]) < 0] = np.inf
    return blocked, above


def lift_of(blocked, above, fire):
    """(lift, rise that would free it, share freed by the lift)."""
    best = blocked.min()
    if blocked[0] - best < SHUT:
        return 0.0, 0.0, 0.0
    k_need = int(np.argmax(blocked <= best + PLATEAU))
    need = float(RISES[k_need])
    # The highest rise allowed: in the fire's lower half, and with room above it. From there down to the first that frees enough.
    j0 = int(np.argmax(RISES >= EMBEDDED - 1e-6))
    def crosses(k):
        return any(above[j] < RISES[k] - RISES[j] for j in range(j0, k))
    allowed = [k for k in range(1, len(RISES)) if RISES[k] <= min(fire, HIGHEST) + 1e-6 and above[k] >= HEADROOM and not crosses(k)]
    # The first allowed rise from the needed one up that frees a quarter (the needed one can fall just short of it); failing that
    # the highest allowed one under it.
    k = next((k for k in allowed if k >= k_need and blocked[0] - blocked[k] >= SHUT), max((k for k in allowed if k < k_need), default=0))
    freed = float(blocked[0] - blocked[k])
    if k == 0 or RISES[k] < SMALLEST or freed < SHUT:
        return 0.0, need, 0.0
    return float(RISES[k]), need, freed


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    detail = "--detail" in sys.argv
    point = sys.argv[sys.argv.index("--point") + 1] if "--point" in sys.argv else None
    if point:
        args = [a for a in args if a != point]
    no_block = table_sources(ASSETS / "mesh_tables" / "sky_meshes.txt") | table_sources(ASSETS / "mesh_tables" / "glass_meshes.txt")
    # The centre of each flame's fire: by place over all captures (flame_heights.tsv; an effect seen fewer than 5 times has no
    # measure and is not raised), and where the exact measure by the sprites' owner exists (ffx_sizes.tsv) the lower of the two:
    # the by-place one takes in sparks and smoke rising off the fire (the Kiln's dishes: 0.85 m by place, 0.25 m exact).
    fire = {}
    for line in (ASSETS / "ffx" / "flame_heights.tsv").read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            f = line.split("\t")
            if int(f[1]) >= 5:
                fire[int(f[0])] = float(f[2])
    for line in (ASSETS / "ffx" / "ffx_sizes.tsv").read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            f = line.split("\t")
            if int(f[0]) in fire and float(f[2]) > 0.0:
                fire[int(f[0])] = min(fire[int(f[0])], float(f[2]))
    flames = defaultdict(list)
    for line in (ASSETS / "ffx" / "map_flames.tsv").read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            m, ffx, x, y, z = line.split("\t")
            flames[m].append((int(ffx), np.array([float(x), float(y), float(z)])))
    if point:
        m, x, y, z = point.split(",")
        scene, objects = scene_of(m, no_block)
        blocked, above = measure(scene, [float(x), float(y), float(z)])
        print(f"{m} ({x}, {y}, {z}): {objects} objects in the scene")
        print("  rise    " + " ".join(f"{r:4.2f}" for r in RISES))
        print("  blocked " + " ".join(f"{b:4.2f}" for b in blocked))
        print("  above   " + " ".join(f"{min(a, 9.99):4.2f}" for a in above))
        return
    rows, summary = [], []
    for m in sorted(flames):
        if args and m not in args:
            continue
        if not (MSB_DIR / f"{m}.msb").exists():
            continue
        scene, objects = scene_of(m, no_block)
        if scene is None:
            continue
        # The flames' own spots do for the map's Light events too: every one of those is within 2 cm of its flame or more than
        # half a metre from any (a duplicate placement gives one row).
        spots, seen = [], set()
        for ffx, p in flames[m]:
            key = tuple(np.round(p, 2))
            if key not in seen:
                seen.add(key)
                spots.append((ffx, p, "flame"))
        lifted = 0
        for ffx, p, what in spots:
            blocked, above = measure(scene, p)
            lift, need, freed = lift_of(blocked, above, fire.get(ffx, 0.0))
            if detail and (lift > 0 or need > 0):
                print(f"  {m} {what} ffx {ffx:6} ({p[0]:8.2f}, {p[1]:8.2f}, {p[2]:8.2f}) blocked {blocked[0]:.2f} best {blocked.min():.2f} needs +{need:.2f} "
                      f"fire {fire.get(ffx, 0.0):.2f} -> lift {lift:.2f} (frees {freed:.2f})")
            if lift > 0:
                lifted += 1
                rows.append(f"{m}\t{p[0]:.3f}\t{p[1]:.3f}\t{p[2]:.3f}\t{lift:.2f}\t{ffx}\t{need:.2f}\t{blocked[0]:.2f}\t{blocked[0] - freed:.2f}")
        summary.append(f"{m}: {len(spots)} spots, {lifted} lifted ({objects} objects in the scene)")
        print(summary[-1], flush=True)
    if not args:
        header = ("# map <TAB> x <TAB> y <TAB> z <TAB> lift m <TAB> FFX id <TAB> rise that frees the spot <TAB> share of rays blocked at the spot "
                  "<TAB> share blocked at the lift. A light at this spot is raised by the lift (map_lights::flame_lift). "
                  "Generated by scripts/gen_flame_lift.py.")
        OUT.write_text("\n".join([header] + rows) + "\n", encoding="utf-8", newline="\n")
        print(f"{len(rows)} lifted spots -> {OUT}")


if __name__ == "__main__":
    main()
