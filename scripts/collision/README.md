# Checking custom lights against the game's own map data

Used on 2026-11-12 to prune `assets/lights/m14_00_00_00.custom.txt` (Quelaag's Domain) to the lights that are inside the Domain.

1. **Collision meshes.** `colexport.cs` / `colexport.csproj` (.NET 8, references DSMapStudio's `SoulsFormats.dll`, `DotNext*.dll`,
   `Andre.Core.dll` from your DSMapStudio folder, set as the `DSMS_DIR` environment variable) writes every collision model of a map as an OBJ:

       dotnet run -c Release -- "<DATA>\unpacked\map\m14_00_00_00" scripts\collision\out\col_m14

2. **Place names.** `msb_collisions.py <map.msb>` lists the MSB's collision parts with their MapNameID (the place name shown
   on entering: 1400 Blighttown, 1401 Quelaag's Domain in m14_00; negative = the same name, not announced).

3. **Per light, against collision** (`domain_lights_check.py <lights.txt>`): 256 rays; share enclosed, share of hits on the
   area's collision parts, share seen from the playable side (these collision meshes face away from the playable space),
   nearest wall.

4. **Per light, against the visible map pieces** (`domain_visual_check.py <lights.txt>`): the same with the FLVERs and their
   vertex normals: share of surfaces facing the light, what lies straight below and above.

5. **Line of sight** (`domain_los.py <lights.txt> <tested> <references>`): which known-good lights each tested light sees.

Run the Python with `py -3.12 -P` (numpy, embreex; `-P` keeps a stray `dis.py` beside a script from shadowing the stdlib).
