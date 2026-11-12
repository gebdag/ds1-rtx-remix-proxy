#pragma once

namespace comp::game::mesh_tables
{
	// Per-mesh tables: map and character meshes recognised by the FNV-1a hash of the index range a draw draws (the engine's
	// index buffers hold the FLVER face sets verbatim), each table in rtx_comp\mesh_tables\ generated from the game's files by a
	// script under scripts\. Sky layers are one of the tables; the others tag glass, shells, thin walls, no-shadow faces,
	// emitters, glow pieces, painted colour and ground cards.

	// Dark Souls draws its skies as ordinary map pieces: a sky model holds the
	// dome, the far cloud layers and the backdrop they are blended over (e.g.
	// Firelink's 5 km ground bowl under the cloud sea). The proxy sends all of
	// them to Remix as sky together, recognising each mesh by the hash of the
	// index range it draws. rtx_comp\mesh_tables\sky_meshes.txt lists the hashes; it is
	// generated from the game's FLVERs by scripts/gen_sky_meshes.py (the
	// engine's index buffers hold the FLVER face sets verbatim).

	// True when this indexed draw is a sky model mesh. Cheap for every other
	// draw: the index buffer is only read when the index count matches a
	// table entry, and the result is cached per buffer range.
	bool is_sky_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count);

	// True when this indexed draw is a blended shell: an alpha-faded map mesh that does not lie on
	// an opaque surface (the giant trees' fading tops and offset shells around trunks), listed in
	// rtx_comp\mesh_tables\shell_meshes.txt by the same generator. The rest of the game's straight-alpha
	// map draws are edge-fade strips lying on their surface, which the proxy tags as decals. A shell
	// tagged as a decal took the lighting of whatever lay behind it, or vanished in front of the sky.
	// gap_skirt (optional out): the entry is marked 'gap', a skirt whose faces hang over a trench (New Londo's walkway and rock
	// skirts m1200B0A16 mesh4 / m1210B0A16 mesh6, scripts/gen_sky_meshes.py gap_skirts, fade from 1 at the top edge to 0 on the floor across a gap of 0.5 to 0.9 m). The game blends it over a trench floor
	// it lights; in Remix that floor is unlit, so the faded part showed a dark seam. It is cut at [Meshes] GapSkirtAlphaRef.
	bool is_shell_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count, bool* gap_skirt = nullptr);

	// For a window glass mesh (rtx_comp\mesh_tables\glass_meshes.txt, scripts/gen_glass.py): the diffuse it is
	// listed with, its alpha cut, and the texture its glass pass draws with. The texture paints the
	// leaded came at alpha >= cut and the glass below it; the renderer draws the two apart
	// (game/glass.hpp). Null for any other draw. Index data alone can collide (short face sets of
	// pillars and rails share a hash with listed glass), so the caller checks the draw's own texture
	// against the name.
	const std::string* glass_texture(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count,
		uint8_t& cut, const std::string*& derived);

	// True when this indexed draw is a thin-wall mesh (rtx_comp\mesh_tables\thin_wall_meshes.txt, scripts/gen_thin_walls.py):
	// a wall built from two single-sided faces whose fronts face each other a few centimetres apart. The game culls the
	// face seen from behind and its lights cast no shadows; ray traced, each face shadows the other and the wall goes
	// black, so the renderer tags these draws and the runtime lets lighting rays pass a tagged face turned back towards
	// the surface they left (Surface::isDs1ThinWall). Only meshes confirmed black in a capture are listed (Firelink's
	// m0100B2A10, Blighttown's m0300B0A14 mesh2).
	bool is_thin_wall_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count);

	// True when this indexed draw is a mesh listed in rtx_comp\mesh_tables\no_shadow_meshes.txt: a face no view shows that only shuts
	// light out (the Asylum's duplicate wall face in front of the wall under its cell windows). Lighting rays pass it.
	bool is_no_shadow_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count);

	// For a no-shadow mesh listed with 'hide <first> <last>': the prims of its draw (counted as the draw counts them) that no
	// view shows, which the renderer leaves out of what it sends to Remix. A face 5 cm in front of a wall and facing it hides
	// that wall from every bounce ray, which passing only lighting rays (the no-shadow tag) does not cure. False for any
	// other draw.
	// Whether any no-shadow entry lists hidden prims: lets the renderer skip the lookup for every draw otherwise.
	bool any_hidden_prims();
	bool no_shadow_hidden_prims(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count,
		UINT& first, UINT& last);

	// True when this indexed draw is a mesh listed in rtx_comp\mesh_tables\emitter_meshes.txt: a character that gives off a
	// little light the game bakes into nothing (the crystal lizard), see game/effect_lights.hpp.
	bool is_emitter_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count);
	// True for a mesh listed in rtx_comp\mesh_tables\glow_meshes.txt: a piece the game only brightens, given a light of its own (the Skull Lantern's eyes).
	bool is_glow_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count);

	// A face set in rtx_comp\mesh_tables\painted_meshes.txt: its vertex colour is shade the artists painted, shown as the game shows it.
	bool is_painted_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count);

	// A flat alpha-tested card lying over the ground (rtx_comp\mesh_tables\ground_cards.txt, scripts/gen_ground_cards.py): the leaves,
	// lily pads, bones and rubble the artists placed a few centimetres up so the game's depth test never let the ground through.
	// The game draws them without shadows, so the gap never shows; ray traced, they float and their shadows sit off them. For a
	// listed draw: each vertex's height above the ground straight below it in mm (0xFFFF = no ground found), in the mesh's vertex
	// order, which is the draw's from its base vertex. The rigid expansion brings the vertices down (ffp_state set_ground_lowering);
	// they keep their shadows. Null for any other draw.
	const std::vector<uint16_t>* ground_card_heights(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count);

	// A blended skirt part of whose faces hang over open air (rtx_comp\mesh_tables\skirt_backing.txt, scripts/gen_sky_meshes.py
	// skirt_backings): the Valley of Drakes' bank skirt m2750B0A16 mesh4 lies on the grass with half its faces and spans the
	// seam to the cliff with the rest, nothing below those within a metre. It stays the decal the game blends; where it hangs
	// the decal had no surface to land on and the sky showed through the seam. For a listed draw: the hanging faces as a
	// triangle list over the draw's own vertices, which the renderer draws once more as a solid cutout. Null for any other draw.
	const std::vector<uint16_t>* skirt_backing_indices(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count);

	// A veil's tuck (veil_meshes.txt 'tuck', scripts/gen_veil_tuck.py): the Kiln ring's solid inner edge meets the doorway wall
	// edge to edge, two meshes; rays slipped through the seam to the passage's white fog behind, a bright line round the wall
	// (frame_20261112_114424). Per vertex of the mesh, x y z in thousandths: the direction along the ring, away from its fading
	// side, that takes its solid edge under the wall; 0 0 0 elsewhere. The rigid expansion moves them [Meshes] VeilTuck metres
	// (ffp_state set_vertex_tuck). Null for any other draw.
	const std::vector<int16_t>* veil_tuck(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count);

	// A veil (rtx_comp\mesh_tables\veil_meshes.txt, hand-kept): a blended map mesh the game lays over a surface it fogs with
	// another set, fading out by vertex alpha. The Kiln's doorway at the end of the white passage: m0010B0A18 mesh17 is a ring of
	// rock wall round the door, solid at the door and clear 10 m out. The game draws it dark, with its own fog, over the
	// passage it has already fogged white, so the white thins to black towards the door. As a decal it was pasted onto the
	// passage and took the passage's haze: white up to the edge of the solid wall inside it, a hard cutout
	// (frame_20261112_020600, _020606). Drawn as an unlit layer in the game's colour for it and left out of the look's haze, it
	// hides the fog behind it by its alpha, as the game's blend does.
	bool is_veil_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count);

	// A new index buffer can reuse a freed one's address: forget its ranges.
	void on_index_buffer_created(IDirect3DIndexBuffer9* ib);
}
