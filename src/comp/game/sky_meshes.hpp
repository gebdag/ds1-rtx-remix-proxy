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

	// A new index buffer can reuse a freed one's address: forget its ranges.
	void on_index_buffer_created(IDirect3DIndexBuffer9* ib);
}
