# Lost Izalith gap filler (mod ds1_izalith_gap)

Near the Bed of Chaos entrance the sky shows through the floor beside the walkway (in the vanilla game too). The map itself
has two mirror-image holes there, left and right of the walkway: no map piece at all under them. The mod adds the missing
floor in those two holes, at the height of the floor around them, and nothing else.

Everything is worked out from the map data (every map piece of m14_01 and m14_00: the MSB placements and the FLVERs), not
from captures: the game culls a lot here (the round arena structure is real geometry it does not draw from the walkway).
Remix world = game world with x mirrored (checked: 96 % of captured vertices lie on the map with that mapping).

1. `py -3.12 -P gap_floor.py <holes.npz>`: a 0.5 m grid over the pit, a ray straight down per cell; a cell where it meets
   nothing is a hole. Holes are grouped and each one's neighbours tell the floor around it. 2026-11-15: two holes of ~3,400
   cells bordered by m3000B1 (-383.35) and m8001B1 (-383.61) are the missing floor; two more border the arena floor
   m3163B1 at -535 and are the openings down to the arena, left open. The fill goes 5 cm under the lowest floor around each
   hole (-383.66) and 3 m under the pieces around it (the walls' base ledge is not drawn from the walkway), never over the
   arena or another hole. Writes `<holes>_fill.npz` (cells and rectangles).
2. `python gap_poly.py <holes>_fill.npz <floor_tris.npz>`: straight edges: the cells as polygons, simplified by < 0.75 m,
   triangulated (2 polygons, 59 triangles). Needs shapely 2 (default python, not 3.12).
3. `python gap_write.py <floor_tris.npz> anchor_4069.npy <mods>\ds1_izalith_gap <ground texture.dds>`: writes mod.usda
   under mesh_4069520337E21066 (single-instance, opaque ring structure; its transform is the x mirror, so the points are
   game-world coordinates) with the plain stone ground texture of the area (AB1A93758512AAA4; the walkway floor 8CE69FC5 is
   cracked stone that looked like holes).

`pit_view.py` renders the map data from a camera (red = the view escapes to the sky); `--look` takes a lookdev capture's
camera (GAME_FRAME position and VS c0/c1/c3, see its docstring). From the 2026-11-15 capture's camera the sky went from
61,440 pixels to 46 with the fill. `anchor_4069.npy` is the anchor's transform, `cams_iz.npy` the 2026-11-13 capture camera.

Notes:
- The Bed of Chaos roots and its grey-green panel are not map pieces; they hang down into these holes, below the floor's
  height, so the floor covers their lower parts.
- `gap_full.py` is the old approach (v1.1): cells where sampled sky rays crossed, which left a ragged outline with holes.
