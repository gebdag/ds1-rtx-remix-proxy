"""Parse Dark Souls 1 PTDE light data from .param and .msb files.

Extracts:
- PointLightBank.param: color, intensity, attenuation per light ID
- LightBank.param: directional lights (3), ambient (upper/lower), specular
- MSB files: light event positions via Region references

Outputs JSON suitable for the proxy DLL's light injection system.
"""

import os  # noqa: E402  (ds1paths: the local folders, see paths.example.ini)
import sys  # noqa: E402
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)))))
import ds1paths  # noqa: E402

import struct
import json
import math
import sys
from pathlib import Path

DATA_DIR = Path(ds1paths.GAME + r"\DATA\unpacked")
PARAM_DIR = DATA_DIR / "param" / "DrawParam"
MSB_DIR = DATA_DIR / "map" / "MapStudio"
OUTPUT_DIR = Path(__file__).parent.parent / "assets" / "lights"


# ============================================================
# Param binary parser (little-endian)
# ============================================================

def parse_param(path: Path):
    """Parse a DS1 .param file. Returns (param_type, [(row_id, row_bytes), ...])."""
    data = path.read_bytes()
    if len(data) < 0x30:
        raise ValueError(f"File too small: {path}")

    strings_offset = struct.unpack_from('<I', data, 0x00)[0]
    data_start_short = struct.unpack_from('<H', data, 0x04)[0]
    row_count = struct.unpack_from('<H', data, 0x0A)[0]
    param_type_raw = data[0x0C:0x2C]
    param_type = param_type_raw.split(b'\x00')[0].decode('ascii', errors='replace')
    # DS1 params: row headers always at 0x30, 12 bytes each (ID + dataIdx + nameIdx)
    row_header_start = 0x30
    row_header_size = 12

    rows = []
    for i in range(row_count):
        off = row_header_start + i * row_header_size
        row_id = struct.unpack_from('<I', data, off)[0]
        data_idx = struct.unpack_from('<I', data, off + 4)[0]
        rows.append((row_id, data_idx))

    if len(rows) >= 2:
        row_size = rows[1][1] - rows[0][1]
    elif len(rows) == 1:
        row_size = strings_offset - rows[0][1]
    else:
        return param_type, []

    result = []
    for row_id, data_idx in rows:
        result.append((row_id, data[data_idx:data_idx + row_size]))
    return param_type, result


# ============================================================
# PointLightBank parser (16 bytes per row)
# ============================================================

def parse_point_light_bank(path: Path):
    """Parse PointLightBank.param -> {row_id: light_info}."""
    _, rows = parse_param(path)
    lights = {}
    for row_id, row_data in rows:
        if len(row_data) < 16:
            continue
        dwindle_begin, dwindle_end = struct.unpack_from('<ff', row_data, 0)
        col_r, col_g, col_b, col_a = struct.unpack_from('<hhhh', row_data, 8)
        lights[row_id] = {
            'id': row_id,
            'dwindle_begin': dwindle_begin,
            'dwindle_end': dwindle_end,
            'r': col_r, 'g': col_g, 'b': col_b,
            'intensity_pct': col_a,
        }
    return lights


# ============================================================
# LightBank parser (92 bytes per row)
# ============================================================

def parse_light_bank(path: Path):
    """Parse LightBank.param -> {row_id: light_set}."""
    _, rows = parse_param(path)
    sets = {}
    for row_id, row_data in rows:
        if len(row_data) < 0x5C:
            continue
        fields = struct.unpack_from('<' + 'h' * 46, row_data, 0)
        entry = {'id': row_id, 'directional': []}
        for i in range(3):
            base = i * 6
            entry['directional'].append({
                'rot_x': fields[base], 'rot_y': fields[base + 1],
                'r': fields[base + 2], 'g': fields[base + 3],
                'b': fields[base + 4], 'intensity_pct': fields[base + 5],
            })
        entry['ambient_upper'] = {'r': fields[18], 'g': fields[19], 'b': fields[20], 'intensity_pct': fields[21]}
        entry['ambient_lower'] = {'r': fields[22], 'g': fields[23], 'b': fields[24], 'intensity_pct': fields[25]}
        entry['specular'] = {
            'rot_x': fields[26], 'rot_y': fields[27],
            'r': fields[28], 'g': fields[29], 'b': fields[30], 'intensity_pct': fields[31],
        }
        entry['env_diffuse'] = {'r': fields[32], 'g': fields[33], 'b': fields[34], 'intensity_pct': fields[35]}
        entry['env_specular'] = {'r': fields[36], 'g': fields[37], 'b': fields[38], 'intensity_pct': fields[39]}
        entry['env_dif_tex'] = fields[40]
        entry['env_spc_tex'] = [fields[41], fields[42], fields[43], fields[44]]
        sets[row_id] = entry
    return sets


# ============================================================
# MSB parser (little-endian MSBD format)
#
# Section header layout:
#   +0x00: int32 reserved (always 0)
#   +0x04: int32 nameOffset (absolute -> section name string)
#   +0x08: int32 offsetCount (entries + 1)
#   +0x0C: int32[offsetCount-1] entry offsets (ABSOLUTE file positions)
#         : int32 nextParamOffset (ABSOLUTE, 0 if final section)
#
# Sub-offsets within entries (nameOffset, baseDataOffset, typeDataOffset)
# are RELATIVE to the entry's start position.
#
# Sections: MODEL -> EVENT -> POINT -> PARTS
# ============================================================

def _read_sjis(data: bytes, offset: int) -> str:
    end = data.index(b'\x00', offset)
    return data[offset:end].decode('shift-jis', errors='replace')


def _parse_section_header(data: bytes, pos: int):
    """Parse one MSB section header.

    Returns (entry_offsets: list[int], next_section_pos: int).
    """
    _reserved, _name_off, offset_count = struct.unpack_from('<iii', data, pos)

    entry_offsets = []
    cur = pos + 12
    for _ in range(offset_count - 1):
        entry_offsets.append(struct.unpack_from('<i', data, cur)[0])
        cur += 4
    next_param = struct.unpack_from('<i', data, cur)[0]
    return entry_offsets, next_param


def _parse_region(data: bytes, offset: int):
    """Parse a POINT_PARAM_ST entry.

    +0x00: int32 nameOffset (relative)
    +0x04: int32 pad
    +0x08: int32 shapeType
    +0x0C: int32 ID
    +0x10: float3 position
    +0x1C: float3 rotation
    """
    name_off = struct.unpack_from('<i', data, offset)[0]
    shape_type = struct.unpack_from('<i', data, offset + 0x08)[0]
    region_id = struct.unpack_from('<i', data, offset + 0x0C)[0]
    px, py, pz = struct.unpack_from('<fff', data, offset + 0x10)
    rx, ry, rz = struct.unpack_from('<fff', data, offset + 0x1C)

    try:
        name = _read_sjis(data, offset + name_off)
    except (ValueError, IndexError):
        name = f'region_{region_id}'

    return {
        'name': name, 'id': region_id, 'shape_type': shape_type,
        'position': [px, py, pz], 'rotation': [rx, ry, rz],
    }


def _parse_event(data: bytes, offset: int, regions: list, part_names: list):
    """Parse an EVENT_PARAM_ST entry.

    +0x00: int32 nameOffset (relative)
    +0x04: int32 EventID
    +0x08: uint32 EventType (0=Light, 1=Sound, 2=SFX, ...)
    +0x0C: int32 ID
    +0x10: int32 baseDataOffset (relative)
    +0x14: int32 typeDataOffset (relative)
    +0x18: pad (8)
    """
    name_off = struct.unpack_from('<i', data, offset)[0]
    event_id = struct.unpack_from('<i', data, offset + 4)[0]
    event_type = struct.unpack_from('<I', data, offset + 8)[0]
    base_data_off = struct.unpack_from('<i', data, offset + 0x10)[0]
    type_data_off = struct.unpack_from('<i', data, offset + 0x14)[0]

    try:
        name = _read_sjis(data, offset + name_off)
    except (ValueError, IndexError):
        name = f'event_{event_id}'

    # Base data: partIndex(i32), regionIndex(i32), entityID(i32)
    base_abs = offset + base_data_off
    part_idx, region_idx, entity_id = struct.unpack_from('<iii', data, base_abs)

    part_name = part_names[part_idx] if 0 <= part_idx < len(part_names) else ''
    region_name = regions[region_idx]['name'] if 0 <= region_idx < len(regions) else ''

    evt = {
        'name': name, 'event_id': event_id, 'event_type': event_type,
        'entity_id': entity_id, 'part_name': part_name, 'region_name': region_name,
    }

    # Light event: type = 0
    if event_type == 0:
        type_abs = offset + type_data_off
        if type_abs + 8 <= len(data):
            point_light_id, unk_t04 = struct.unpack_from('<ii', data, type_abs)
            evt['type'] = 'Light'
            evt['point_light_id'] = point_light_id
            evt['unk_t04'] = unk_t04

    return evt


def _parse_part_name(data: bytes, offset: int) -> str:
    name_off = struct.unpack_from('<i', data, offset)[0]
    try:
        return _read_sjis(data, offset + name_off)
    except (ValueError, IndexError):
        return ''


def parse_msb(path: Path):
    """Parse MSBD file. Returns (light_events_with_positions, all_regions)."""
    data = path.read_bytes()
    if len(data) < 12:
        return [], []

    # Section 1: MODEL_PARAM_ST (skip entries)
    _, next_pos = _parse_section_header(data, 0)

    # Section 2: EVENT_PARAM_ST
    event_offsets, next_pos2 = _parse_section_header(data, next_pos)

    # Section 3: POINT_PARAM_ST (Regions)
    region_offsets, next_pos3 = _parse_section_header(data, next_pos2)

    # Section 4: PARTS_PARAM_ST
    part_offsets, _ = _parse_section_header(data, next_pos3)

    # Parse regions and parts first (events reference them by index)
    regions = [_parse_region(data, off) for off in region_offsets]
    part_names = [_parse_part_name(data, off) for off in part_offsets]

    # Parse events
    events = [_parse_event(data, off, regions, part_names) for off in event_offsets]

    # Filter light events and attach region positions
    lights = []
    for evt in events:
        if evt.get('type') != 'Light':
            continue
        position = None
        for r in regions:
            if r['name'] == evt.get('region_name', ''):
                position = r['position']
                break
        lights.append({
            'name': evt['name'],
            'point_light_id': evt.get('point_light_id', -1),
            'unk_t04': evt.get('unk_t04', 0),
            'position': position,
            'region_name': evt.get('region_name', ''),
            'part_name': evt.get('part_name', ''),
            'entity_id': evt.get('entity_id', -1),
        })

    return lights, regions


# ============================================================
# Direction from angular rotation (degrees)
# ============================================================

def direction_from_angles(deg_rot_x: int, deg_rot_y: int):
    """Convert DS1 directional light angles to a unit direction vector."""
    rx = math.radians(deg_rot_x)
    ry = math.radians(deg_rot_y)
    dx = math.cos(rx) * math.sin(ry)
    dy = -math.sin(rx)
    dz = math.cos(rx) * math.cos(ry)
    return [dx, dy, dz]


# ============================================================
# Main
# ============================================================

def process_map(map_prefix: str):
    pl_path = PARAM_DIR / f"{map_prefix}_PointLightBank.param"
    point_lights = parse_point_light_bank(pl_path) if pl_path.exists() else {}

    lb_path = PARAM_DIR / f"{map_prefix}_LightBank.param"
    light_bank = parse_light_bank(lb_path) if lb_path.exists() else {}

    msb_files = sorted(MSB_DIR.glob(f"{map_prefix}_*.msb"))
    all_light_events = []
    all_regions = []
    for msb_path in msb_files:
        light_events, regions = parse_msb(msb_path)
        all_light_events.extend(light_events)
        all_regions.extend(regions)

    return {
        'map': map_prefix,
        'point_light_bank': point_lights,
        'light_bank': light_bank,
        'light_events': all_light_events,
        'region_count': len(all_regions),
    }


def main():
    maps = ['m10', 'm11', 'm12', 'm13', 'm14', 'm15', 'm15_1', 'm16', 'm17', 'm18']

    if len(sys.argv) > 1:
        result = process_map(sys.argv[1])
        print(json.dumps(result, indent=2, default=str))
        return

    total_point = 0
    total_events = 0
    all_data = {}

    for m in maps:
        result = process_map(m)
        all_data[m] = result

        n_point = len(result['point_light_bank'])
        n_events = len(result['light_events'])
        total_point += n_point
        total_events += n_events

        positioned = [e for e in result['light_events'] if e.get('position')]
        print(f"\n=== {m} ===")
        print(f"  PointLightBank rows: {n_point}")
        print(f"  LightBank rows: {len(result['light_bank'])}")
        print(f"  Light events: {n_events} ({len(positioned)} with position)")
        for e in positioned[:5]:
            pl = result['point_light_bank'].get(e['point_light_id'], {})
            pos = e['position']
            print(f"    {e['name']}: pos=({pos[0]:.1f}, {pos[1]:.1f}, {pos[2]:.1f}) "
                  f"plID={e['point_light_id']} "
                  f"color=({pl.get('r', '?')},{pl.get('g', '?')},{pl.get('b', '?')}) "
                  f"atten={pl.get('dwindle_begin', '?')}-{pl.get('dwindle_end', '?')}m")
        if len(positioned) > 5:
            print(f"    ... and {len(positioned) - 5} more")

        for lb_id, lb in list(result['light_bank'].items())[:1]:
            print(f"  LightBank[{lb_id}]:")
            for i, d in enumerate(lb['directional']):
                dv = direction_from_angles(d['rot_x'], d['rot_y'])
                intensity = d['intensity_pct'] / 100.0
                print(f"    Dir{i}: rot=({d['rot_x']},{d['rot_y']}) "
                      f"dir=({dv[0]:.2f},{dv[1]:.2f},{dv[2]:.2f}) "
                      f"color=({d['r']},{d['g']},{d['b']})x{intensity:.0%}")

    print(f"\n=== TOTALS ===")
    print(f"Point light definitions: {total_point}")
    print(f"MSB light events: {total_events}")

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    out_path = OUTPUT_DIR / "ds1_lights.json"
    with open(out_path, 'w') as f:
        json.dump(all_data, f, indent=2, default=str)
    print(f"\nWrote: {out_path}")

    # Write compact text format for C++ loader
    txt_path = OUTPUT_DIR / "ds1_lights.txt"
    write_text_format(all_data, txt_path)
    print(f"Wrote: {txt_path}")


def write_text_format(all_data: dict, path: Path):
    """Write light data in a simple text format for the C++ proxy loader.

    Format:
      MAP <name>
      PL <id> <dwindle_begin> <dwindle_end> <r> <g> <b> <intensity_pct>
      EV <point_light_id> <x> <y> <z>
      DIR <rot_x> <rot_y> <r> <g> <b> <intensity_pct>
      END
    """
    with open(path, 'w') as f:
        for map_name, data in all_data.items():
            f.write(f"MAP {map_name}\n")

            for pl_id, pl in data['point_light_bank'].items():
                f.write(f"PL {pl['id']} {pl['dwindle_begin']:.4f} {pl['dwindle_end']:.4f} "
                        f"{pl['r']} {pl['g']} {pl['b']} {pl['intensity_pct']}\n")

            for ev in data['light_events']:
                pos = ev.get('position')
                if not pos:
                    continue
                f.write(f"EV {ev['point_light_id']} {pos[0]:.4f} {pos[1]:.4f} {pos[2]:.4f}\n")

            # Write directional lights from LightBank row 0 (primary lighting set)
            lb = data['light_bank']
            if lb:
                first_lb = next(iter(lb.values()))
                for d in first_lb['directional']:
                    if d['intensity_pct'] <= 0:
                        continue
                    f.write(f"DIR {d['rot_x']} {d['rot_y']} "
                            f"{d['r']} {d['g']} {d['b']} {d['intensity_pct']}\n")

            f.write("END\n")


if __name__ == '__main__':
    main()
