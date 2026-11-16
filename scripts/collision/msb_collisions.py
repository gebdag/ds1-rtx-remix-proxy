"""Lists the collision parts of a DS1 (MSB1) map with their position and place name id (MapNameID), and the map pieces.
Usage: msb_collisions.py <map.msb>"""
import os  # noqa: E402  (ds1paths: the local folders, see paths.example.ini)
import sys  # noqa: E402
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")))
import ds1paths  # noqa: E402

import struct
import sys
from collections import Counter

sys.path.insert(0, ds1paths.SCRIPTS)
from map_geometry import _section, _sjis  # noqa: E402

data = open(sys.argv[1], "rb").read()
model_offsets, pos = _section(data, 0)
models = [_sjis(data, o + struct.unpack_from("<i", data, o)[0]) for o in model_offsets]
_, pos = _section(data, pos)  # events
_, pos = _section(data, pos)  # regions
part_offsets, _ = _section(data, pos)
names = Counter()
for o in part_offsets:
    name_off, kind, index, model = struct.unpack_from("<iIii", data, o)
    name = _sjis(data, o + name_off)
    p = struct.unpack_from("<3f", data, o + 0x14)
    if kind == 5:  # collision
        t = o + struct.unpack_from("<i", data, o + 0x5C)[0]
        hit_filter, sound, env_spot, reflect = struct.unpack_from("<BBhf", data, t)
        nvm = struct.unpack_from("<4I", data, t + 8)
        map_name_id, disable_start = struct.unpack_from("<hh", data, t + 0x24)
        names[map_name_id] += 1
        print(f"COL {name:<14} model {models[model]:<14} pos ({p[0]:8.1f} {p[1]:8.1f} {p[2]:8.1f}) mapname {map_name_id} filter {hit_filter} nvm {nvm[0]:08X}")
print(names)
