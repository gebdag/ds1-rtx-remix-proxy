#pragma once

namespace comp::game::sky_meshes
{
	// Dark Souls draws its skies as ordinary map pieces: a sky model holds the
	// dome, the far cloud layers and the backdrop they are blended over (e.g.
	// Firelink's 5 km ground bowl under the cloud sea). The proxy sends all of
	// them to Remix as sky together, recognising each mesh by the hash of the
	// index range it draws. rtx_comp\sky\sky_meshes.txt lists the hashes; it is
	// generated from the game's FLVERs by scripts/gen_sky_meshes.py (the
	// engine's index buffers hold the FLVER face sets verbatim).

	// True when this indexed draw is a sky model mesh. Cheap for every other
	// draw: the index buffer is only read when the index count matches a
	// table entry, and the result is cached per buffer range.
	bool is_sky_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count);

	// True when this indexed draw is a blended shell: an alpha-faded map mesh that does not lie on
	// an opaque surface (the giant trees' fading tops and offset shells around trunks), listed in
	// rtx_comp\sky\shell_meshes.txt by the same generator. The rest of the game's straight-alpha
	// map draws are edge-fade strips lying on their surface, which the proxy tags as decals. A shell
	// tagged as a decal took the lighting of whatever lay behind it, or vanished in front of the sky.
	bool is_shell_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count);

	// For a window glass mesh (rtx_comp\sky\glass_meshes.txt, scripts/gen_glass.py): the diffuse it is
	// listed with, its alpha cut, and the texture its glass pass draws with. The texture paints the
	// leaded came at alpha >= cut and the glass below it; the renderer draws the two apart
	// (game/glass.hpp). Null for any other draw. Index data alone can collide (short face sets of
	// pillars and rails share a hash with listed glass), so the caller checks the draw's own texture
	// against the name.
	const std::string* glass_texture(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count,
		uint8_t& cut, const std::string*& derived);

	// True when this indexed draw is a thin-wall mesh (rtx_comp\sky\thin_wall_meshes.txt, scripts/gen_thin_walls.py):
	// single-sided faces whose fronts face each other a few centimetres apart, measured against every mesh of its
	// model. game/thin_walls.hpp measures each drawn mesh against itself only, and misses walls whose two faces are
	// in different meshes (Firelink's m0100B2A10, mesh0 in front of mesh1's stone wall).
	bool is_thin_wall_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count);

	// True when this indexed draw is a mesh listed in rtx_comp\sky\emitter_meshes.txt: a character that gives off a
	// little light the game bakes into nothing (the crystal lizard), see game/effect_lights.hpp.
	bool is_emitter_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count);

	// A new index buffer can reuse a freed one's address: forget its ranges.
	void on_index_buffer_created(IDirect3DIndexBuffer9* ib);
}
