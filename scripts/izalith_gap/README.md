# Lost Izalith gap filler (mod ds1_izalith_gap)

Near the Bed of Chaos entrance the sky shows through the ring around the walkway (in the vanilla game too). The mod adds a
ground sheet under the walkway, only where view rays escape to the sky, checked against the FULL map (every visible piece
of m14_01 and m14_00), not against what a capture drew (captures are culled: a quad at -477 built from captures alone cut
through 3,395 map triangles that the captures did not have).

1. `py -3.12 -P gap_full.py cams_iz.npy full_sheet.npz` scans levels: per level, the sheet cells where steep escaping rays
   cross, small holes closed, dropped where any map piece is straight below or within 0.4 m. 2026-11-13: from -384.7 to
   -392.7 the sheet is 100 % clean; the mod uses -386.74 (28 m under the walkway, 6,044 cells of 0.5 m).
2. `py -3.12 -P gap_full.py cams_iz.npy full_sheet.npz -386.74` saves that sheet.
3. `python gap_write.py full_sheet.npz anchor_4069.npy <mods>\ds1_izalith_gap <capture textures>\AB1A93758512AAA4.dds`
   writes mod.usda (under mesh_4069520337E21066: single-instance, opaque ring structure; transform = x mirror, so the
   sheet's points are game-world coordinates) and copies a plain stone ground texture of the area (AB1A93758512AAA4; the walkway floor 8CE69FC5 is cracked stone that looked like holes).

Remix world = game world with x mirrored (checked: 96 % of captured vertices lie on the map with that mapping).
`cams_iz.npy` is the camera of the Remix capture used for the view rays; `anchor_4069.npy` the anchor's transform.
