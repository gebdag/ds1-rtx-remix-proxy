"""Generate stand-in point lights for every DS1 flame the engine does not light.

Background (measured, not assumed):

* Several maps place flame effects but no MSB `Light` events at all -- Blighttown has 99
  torch placements and zero. Others cover most flames but not all.
* The flame effects themselves mostly declare no light node: tracing
  `FrpgFxDrawEntityHostPointLight::Update` (VA 0x00D32340) for 600 hits in Blighttown
  returns a single `this` -- the bonfire's -- and `KPointLightMan`'s allocation cursor
  reads 2. So nothing can be harvested for the torches.

What IS shipped is the placement and the palette: every flame has an SFX event with a
region origin, and every map has its own `POINT_LIGHT_BANK` DrawParam. This joins them.

Two rules keep the result honest:

* **Never double-light.** A flame with an MSB `Light` event within `--radius` already has
  a real engine light and is skipped. That is what makes this safe to run over all 18
  maps: Undead Burg contributes 2 lights, not 53.
* **Use the map's own colour.** When a map has `Light` events, the row they mostly
  reference is reused, so a synthesized torch matches a real one in the same map exactly.
  Maps with no `Light` events fall back to row 39 (the standard 0.50-4.00 m / 255,240,170
  / 200% torch row, authored in m13/m14/m18) or, failing that, to the authored row closest
  to that profile.

Usage::

    python scripts/gen_map_lights.py              # write assets/lights/*.txt
    python scripts/gen_map_lights.py --list       # report only, write nothing
    python scripts/gen_map_lights.py --no-extras  # open flames only

Every light carries a category (the effect it came from), which is how the proxy groups
them: tuning "torch_small" moves all 85 torches in the game at once.
"""

import argparse
import re
import struct
from collections import Counter
from pathlib import Path

DATA = Path(r"<steam dir>\steamapps\common"
            r"\Dark Souls Prepare to Die Edition\DATA\unpacked")
MSB_DIR = DATA / "map" / "MapStudio"
PARAM_DIR = DATA / "param" / "DrawParam"
OUT_DIR = Path(__file__).parent.parent / "assets" / "lights"

# Open flames and lamps: things that unambiguously light their surroundings.
FLAME_FFX = {511: "candlestick", 513: "torch", 530: "torch_small", 540: "lantern"}

# On by default; drop with --no-extras. Identified from the SFX inventory and the sprite
# set each effect draws; see analysis/ds1_sfx_light_candidates.md for the reasoning.
EXTRA_FFX = {
    512: "catacombs_brazier",     # m13_00, 110 placements, flame sheet s00016
    180000: "kiln_embers",        # m18_00, 14, flame sheet s00009
    110003: "painted_bonfire",    # m11_00, 6
    # Not 120059 (Oolacile lantern): it has its own game light at the flame, and a stand-in at the effect's spot, on the
    # ground under it, was a warm 12 m light that the game does not have.
}

# Effects that are not torches carry their own colour, which wins over the map's torch row:
# the constant colour keys of the effect's FFX colour block (sfx\*.ffx, R/G/B keyframes).
#   540: Tomb of the Giants' lantern-reactive halo, R 0.392 G 0.824 B 1.0 (both colour blocks).
EFFECT_COLOUR = {540: (100, 210, 255)}

AREA_NAMES = {
    "m10_00": "Depths", "m10_01": "Undead Burg / Parish", "m10_02": "Firelink Shrine",
    "m11_00": "Painted World", "m12_00": "Darkroot Garden/Basin", "m12_01": "Oolacile (DLC)",
    "m13_00": "Catacombs", "m13_01": "Tomb of the Giants", "m13_02": "Great Hollow / Ash Lake",
    "m14_00": "Blighttown / Quelaag's Domain", "m14_01": "Demon Ruins / Lost Izalith",
    "m15_00": "Sen's Fortress", "m15_01": "Anor Londo", "m16_00": "New Londo Ruins",
    "m17_00": "Duke's Archives", "m18_00": "Kiln of the First Flame", "m18_01": "Undead Asylum",
}

# The row every Undead Burg torch light uses, in shape: small, warm, double intensity.
CANONICAL_TORCH = (0.5, 4.0, 200)
UNSET_ROW = (0.5, 2.0, 255, 255, 255, 100)


# --------------------------------------------------------------------------- MSB

def _sjis(data, offset):
    return data[offset:data.index(b"\x00", offset)].decode("shift-jis", errors="replace")


def _section(data, pos):
    _reserved, _name_off, count = struct.unpack_from("<iii", data, pos)
    offsets = [struct.unpack_from("<i", data, pos + 12 + i * 4)[0] for i in range(count - 1)]
    return offsets, struct.unpack_from("<i", data, pos + 12 + (count - 1) * 4)[0]


def read_events(msb_path):
    """(flame SFX, Light events) as lists of (name, payload, (x, y, z))."""
    data = msb_path.read_bytes()
    _, after_models = _section(data, 0)
    event_offsets, after_events = _section(data, after_models)
    region_offsets, _ = _section(data, after_events)

    regions = [struct.unpack_from("<fff", data, off + 0x10) for off in region_offsets]

    sfx, lights = [], []
    for off in event_offsets:
        name_off, _id, event_type = struct.unpack_from("<iII", data, off)
        base_off, type_off = struct.unpack_from("<ii", data, off + 0x10)
        if event_type not in (0, 2):
            continue
        payload = struct.unpack_from("<i", data, off + type_off)[0]
        region_idx = struct.unpack_from("<i", data, off + base_off + 4)[0]
        if not 0 <= region_idx < len(regions):
            continue
        entry = (_sjis(data, off + name_off), payload, regions[region_idx])
        (sfx if event_type == 2 else lights).append(entry)
    return sfx, lights


# ------------------------------------------------------------------- DrawParam

def read_bank(prefix):
    data = (PARAM_DIR / f"{prefix}_PointLightBank.param").read_bytes()
    strings_offset = struct.unpack_from("<I", data, 0x00)[0]
    count = struct.unpack_from("<H", data, 0x0A)[0]
    headers = [struct.unpack_from("<II", data, 0x30 + i * 12) for i in range(count)]
    size = (headers[1][1] - headers[0][1]) if count > 1 else strings_offset - headers[0][1]

    bank = {}
    for row_id, offset in headers:
        row = data[offset:offset + size]
        begin, end = struct.unpack_from("<ff", row, 0)
        r, g, b, pct = struct.unpack_from("<hhhh", row, 8)
        bank[row_id] = (begin, end, r, g, b, pct)
    return bank


def param_prefix(map_stem):
    """m15_01 uses the m15_1 DrawParam set; everything else is just the area."""
    area, block = map_stem[1:3], map_stem[4:6]
    for candidate in (f"m{area}_{int(block)}", f"m{area}"):
        if (PARAM_DIR / f"{candidate}_PointLightBank.param").exists():
            return candidate
    raise KeyError(f"no DrawParam set for {map_stem}")


def torch_row(bank, lights):
    """(row id, values, why) -- the map's own torch colour where it has one."""
    used = Counter(payload for _n, payload, _p in lights if payload in bank)
    if used:
        row_id = used.most_common(1)[0][0]
        return row_id, bank[row_id], f"row the map's own Light events use ({used[row_id]}x)"

    if bank.get(39) and bank[39] != UNSET_ROW:
        return 39, bank[39], "standard torch row 39"

    # Nothing authored to copy: take the row that most resembles a torch.
    def distance(values):
        begin, end, r, g, b, pct = values
        warm_penalty = 0.0 if r >= g >= b and r > b else 5.0
        return (abs(begin - CANONICAL_TORCH[0]) + abs(end - CANONICAL_TORCH[1])
                + abs(pct - CANONICAL_TORCH[2]) / 100.0 + warm_penalty)

    authored = {rid: v for rid, v in bank.items() if v != UNSET_ROW and v[5] > 0}
    if not authored:
        raise KeyError("bank has no authored rows")
    row_id = min(authored, key=lambda rid: distance(authored[rid]))
    return row_id, authored[row_id], "closest authored row to a torch profile"


# ----------------------------------------------------------------- generation

def short_name(msb_name):
    """'【SFX】o4404_1000' -> 'o4404_1000'; ASCII tail, safe to print anywhere."""
    name = msb_name
    for bracket in ("】", "]"):
        if bracket in name:
            name = name.rsplit(bracket, 1)[-1]
    name = "".join(c if c.isascii() and (c.isalnum() or c in "_-.") else "_"
                   for c in name.strip())
    # Japanese collapses to runs of underscores; squeeze them so the label stays short.
    return re.sub(r"_+", "_", name).strip("_") or "unnamed"


def build(map_stem, radius, keep_sub, wanted):
    sfx, lights = read_events(MSB_DIR / f"{map_stem}.msb")
    flames = [(n, f, p) for n, f, p in sfx if f in wanted]
    if not keep_sub:
        # A torch carries a main flame plus a "_sub" flame a metre below; one light each
        # would double the energy at the same spot.
        flames = [f for f in flames if not f[0].endswith("_sub")]

    lit = [p for _n, _payload, p in lights]
    unlit = [f for f in flames
             if not any(sum((a - b) ** 2 for a, b in zip(f[2], l)) <= radius * radius
                        for l in lit)]
    if not unlit:
        return None, len(flames), 0, ""

    prefix = param_prefix(map_stem)
    bank = read_bank(prefix)
    row_id, (begin, end, r, g, b, pct), why = torch_row(bank, lights)
    own_colour = sorted({f for _n, f, _p in unlit if f in EFFECT_COLOUR})

    header = [
        f"# Synthesized point lights for {map_stem} ({AREA_NAMES.get(map_stem[:6], '?')})",
        "# Generated by scripts/gen_map_lights.py -- regenerate after editing that script.",
        "#",
        f"# {len(flames)} flame placement(s) here; {len(flames) - len(unlit)} already have an",
        f"# MSB Light event within {radius:g} units and are left to the engine.",
        f"# Colour and falloff: {prefix}_PointLightBank row {row_id} ({why}).",
        *([f"# Colour of FFX {', '.join(map(str, own_colour))}: the effect's own (EFFECT_COLOUR)."] if own_colour else []),
        "#",
        "# Edit freely -- press Reload Light Table in F4 > Lights to pick changes up.",
        "#",
        "# Category groups lights across every map, so all torches tune together.",
        "#",
        "# x y z  dwindleBegin dwindleEnd  r g b  intensityPct  category  name",
    ]
    body = [f"{p[0]:.4f} {p[1]:.4f} {p[2]:.4f}  {begin:.2f} {end:.2f}  "
            f"{' '.join(map(str, EFFECT_COLOUR.get(f, (r, g, b))))}  {pct}  {wanted[f]}  {short_name(n)}"
            for n, f, p in unlit]
    return header + body, len(flames), len(unlit), f"{prefix} row {row_id}"


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--list", action="store_true", help="report only, write nothing")
    ap.add_argument("--no-extras", action="store_true",
                    help="emit only open flames, skipping EXTRA_FFX")
    ap.add_argument("--keep-sub", action="store_true", help="also light '_sub' flames")
    ap.add_argument("--radius", type=float, default=6.0,
                    help="how close an MSB Light must be for a flame to count as lit")
    args = ap.parse_args()

    wanted = dict(FLAME_FFX)
    if not args.no_extras:
        wanted.update(EXTRA_FFX)

    if not args.list:
        OUT_DIR.mkdir(parents=True, exist_ok=True)

    total = 0
    print(f"{'map':<14} {'area':<26} {'flames':>7} {'synth':>6}  row")
    for msb in sorted(MSB_DIR.glob("*.msb")):
        stem = msb.stem
        lines, flames, synth, row = build(stem, args.radius, args.keep_sub, wanted)
        if flames == 0:
            continue
        print(f"{stem:<14} {AREA_NAMES.get(stem[:6], '?'):<26} {flames:>7} {synth:>6}  {row}")
        total += synth
        if lines and not args.list:
            (OUT_DIR / f"{stem}.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{'':<14} {'':<26} {'':>7} {total:>6}  total")
