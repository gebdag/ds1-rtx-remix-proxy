"""Inventory every SFX effect placed in DS1's maps, to decide which deserve a light.

Prints one row per FFX id: how many times it is placed, which maps place it, and the
names the artists gave the events (Shift-JIS, so the Japanese noun is the real label).
`--lights` additionally shows, per map, how many MSB `Light` events exist and how many
flame placements already have one nearby -- the test `gen_map_lights.py` uses to avoid
double-lighting a torch the engine already lights.

Usage::

    python scripts/sfx_inventory.py                # every effect, most-placed first
    python scripts/sfx_inventory.py --min 4        # only effects placed 4+ times
    python scripts/sfx_inventory.py --lights       # per-map light vs flame coverage
"""

import os  # noqa: E402  (ds1paths: the local folders, see paths.example.ini)
import sys  # noqa: E402
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)))))
import ds1paths  # noqa: E402

import argparse
import struct
from collections import Counter, defaultdict
from pathlib import Path

DATA = Path(ds1paths.GAME + r"\DATA\unpacked")
MSB_DIR = DATA / "map" / "MapStudio"

AREA_NAMES = {
    "m10_00": "Depths", "m10_01": "Undead Burg/Parish", "m10_02": "Firelink Shrine",
    "m11_00": "Painted World", "m12_00": "Darkroot", "m12_01": "Oolacile (DLC)",
    "m13_00": "Catacombs", "m13_01": "Tomb of the Giants", "m13_02": "Great Hollow/Ash Lake",
    "m14_00": "Blighttown/Quelaag", "m14_01": "Demon Ruins/Izalith", "m15_00": "Sen's Fortress",
    "m15_01": "Anor Londo", "m16_00": "New Londo Ruins", "m17_00": "Duke's Archives",
    "m18_00": "Kiln", "m18_01": "Undead Asylum",
}

# Japanese nouns that appear in the event names, with what they mean. Used only to
# annotate the report -- the decision about any given effect is still a human one.
GLOSSARY = [
    ("松明", "torch"), ("燭台", "candlestick"), ("蜀台", "candlestick"),
    ("篝火", "bonfire"), ("かがり火", "bonfire"), ("ランタン", "lantern"),
    ("灯", "lamp/light"), ("明かり", "light"), ("光の壁", "fog wall"),
    ("光", "light"), ("炎", "flame"), ("火の粉", "embers"), ("火", "fire"),
    ("溶岩", "lava"), ("マグマ", "magma"), ("結晶", "crystal"), ("魔方陣", "magic circle"),
    ("落雷", "lightning"), ("雷", "lightning"), ("霧", "mist"), ("煙", "smoke"),
    ("埃", "dust"), ("塵", "dust"), ("水", "water"), ("滝", "waterfall"),
    ("蟲", "insects"), ("虫", "insects"), ("毒", "poison"), ("血", "blood"),
    ("風", "wind"), ("蝶", "butterfly"), ("羽", "feather"), ("星", "stars"),
    ("召喚サイン", "summon sign"), ("誘導", "guide marker"), ("封印", "seal"),
    ("粘液", "slime"), ("蜘蛛", "spider"), ("桜", "cherry blossom"),
]


def _sjis(data, offset):
    return data[offset:data.index(b"\x00", offset)].decode("shift-jis", errors="replace")


def _section(data, pos):
    _reserved, _name_off, count = struct.unpack_from("<iii", data, pos)
    offsets = [struct.unpack_from("<i", data, pos + 12 + i * 4)[0] for i in range(count - 1)]
    return offsets, struct.unpack_from("<i", data, pos + 12 + (count - 1) * 4)[0]


def read_map(msb_path):
    """Returns (sfx events, light events) as lists of (name, payload, position)."""
    data = msb_path.read_bytes()
    _, after_models = _section(data, 0)
    event_offsets, after_events = _section(data, after_models)
    region_offsets, _ = _section(data, after_events)

    regions = []
    for off in region_offsets:
        name_off = struct.unpack_from("<i", data, off)[0]
        regions.append(struct.unpack_from("<fff", data, off + 0x10))

    sfx, lights = [], []
    for off in event_offsets:
        name_off, _id, event_type = struct.unpack_from("<iII", data, off)
        base_off, type_off = struct.unpack_from("<ii", data, off + 0x10)
        if event_type not in (0, 2):                 # 0 = Light, 2 = SFX
            continue
        payload = struct.unpack_from("<i", data, off + type_off)[0]
        region_idx = struct.unpack_from("<i", data, off + base_off + 4)[0]
        pos = regions[region_idx] if 0 <= region_idx < len(regions) else None
        entry = (_sjis(data, off + name_off), payload, pos)
        (sfx if event_type == 2 else lights).append(entry)
    return sfx, lights


def gloss(names):
    """Meanings of the Japanese nouns present in a set of event names."""
    joined = " ".join(names)
    hits, seen = [], set()
    for token, meaning in GLOSSARY:
        if token in joined and meaning not in seen:
            seen.add(meaning)
            hits.append(meaning)
    return hits


def inventory():
    placements = Counter()
    maps_using = defaultdict(set)
    names = defaultdict(Counter)
    for msb in sorted(MSB_DIR.glob("*.msb")):
        sfx, _ = read_map(msb)
        for name, ffx, _pos in sfx:
            placements[ffx] += 1
            maps_using[ffx].add(msb.stem[:6])
            names[ffx][name] += 1
    return placements, maps_using, names


def light_coverage(radius):
    """Per map: Light events, flame placements, and flames already lit."""
    from gen_map_lights import FLAME_FFX
    rows = []
    for msb in sorted(MSB_DIR.glob("*.msb")):
        sfx, lights = read_map(msb)
        flames = [p for _n, f, p in sfx if f in FLAME_FFX and p]
        lit_positions = [p for _n, _f, p in lights if p]
        covered = sum(
            1 for f in flames
            if any(sum((a - b) ** 2 for a, b in zip(f, l)) <= radius * radius
                   for l in lit_positions)
        )
        rows.append((msb.stem[:6], len(lights), len(flames), covered))
    return rows


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--min", type=int, default=1, help="hide effects placed fewer times")
    ap.add_argument("--lights", action="store_true", help="per-map light coverage instead")
    ap.add_argument("--radius", type=float, default=6.0,
                    help="how close an MSB Light must be to count a flame as already lit")
    args = ap.parse_args()

    if args.lights:
        print(f"{'map':<8} {'area':<24} {'Light ev':>8} {'flames':>7} {'already lit':>11} {'gap':>5}")
        for key, n_lights, n_flames, covered in light_coverage(args.radius):
            print(f"{key:<8} {AREA_NAMES.get(key, key):<24} {n_lights:>8} {n_flames:>7} "
                  f"{covered:>11} {n_flames - covered:>5}")
        raise SystemExit

    placements, maps_using, names = inventory()
    print(f"{'ffx':>9} {'placed':>6}  {'maps':<26} meaning / sample names")
    for ffx, count in placements.most_common():
        if count < args.min:
            continue
        sample = [n for n, _ in names[ffx].most_common(2)]
        meanings = gloss(list(names[ffx]))
        where = ",".join(sorted(maps_using[ffx]))
        print(f"{ffx:>9} {count:>6}  {where[:26]:<26} "
              f"{'/'.join(meanings) if meanings else '?':<22} {' | '.join(sample)}")
