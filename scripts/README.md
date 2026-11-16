# Tools

Scripts that rebuild the mod's data from the game's own files: the light, sun and flame tables in `assets/`, the
helper meshes and materials of the Remix mods (glass, foliage, thin walls, sky, water, swamp, ground cards, hole covers,
the Izalith floor), and a few checks. The lookdev tools in `../lookdev` drive the lookdev host.

## Setup

- Python 3.10 or newer with `numpy`, `Pillow`, `shapely`, `xxhash`, `embreex` and `pefile` (each script imports only
  what it needs).
- The game's archives unpacked into `DATA\unpacked` (the folder layout an unpacker for Dark Souls: Prepare to Die
  Edition writes).
- Your folders, as environment variables or in `paths.local.ini` at the repo root (copy `paths.example.ini`; the file
  is not tracked). `ds1paths.py` lists them:
  - `DS1_GAME_DIR`: the game folder that holds `DATA\`.
  - `DS1_LOOKDEV_GAME_DIR`: the folder the lookdev host runs from (default: `DS1_GAME_DIR\DATA`).
  - `DS1_RUNTIME_DIR`: a checkout of the RTX Remix runtime fork (`lookdev/redeploy.py`).
  - `DS1_PBR_TOOLS`: the `pbr_tools` folder of the Vibe Reverse Engineering toolkit (`gen_foliage`, `gen_glass`,
    `gen_painted_meshes`, `gen_texture_fixes`).
  - `DS1_ANALYSIS_DIR`: the effect notes `gen_ffx_tables` reads (or pass the folder as its argument).
- `cold_cache.cmd` takes your Steam folder from `DS1_STEAM_DIR`.
- `collision/colexport` is a .NET 8 project that references DSMapStudio's `SoulsFormats.dll`; set `DSMS_DIR` to your
  DSMapStudio folder.

`bbformats/` is a small reader for FromSoftware's DCX, BND4 and FLVER2 formats that the mesh tools use.

Each script's header says what it reads and writes.
