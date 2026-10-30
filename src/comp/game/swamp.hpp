#pragma once

#include "shared/common/ffp_state.hpp"

namespace comp::game::swamp
{
	// Dark Souls' FRPG_Snow surfaces (the Blighttown swamp) as the runtime reproduces them
	// (rtx.ds1Swamp.*, the runtime's fork_ds1_swamp.h):
	//
	// FRPG_Snow_HeightMap draws a height field into a 512x360 buffer: three layers of its s2 texture
	// (the swamp's own colour texture, Gaping Dragon's slime a noise texture) at mesh UV * VS c143.xyz
	// + c246 / c247 / c248.xy (the scroll), weighted by PS c171.xyz, faded out by view depth
	// (PS c163.z). The surface scales that height by its vertex alpha (0 along the mesh's border,
	// where it melts into the floor) and its slope by c169.x too. WaterWave draws (FRPG_WWS_Dif________________WaterWave) then subtract rings where
	// a character wades: a ring texture on a small disc, bone 0 = VS c8-c10, strength
	// c100.x * c100.w * c139.x * c139.w. The FRPG_Snow surface draws colour, tilt and reflect the
	// surface by that field (PS c87 c100 c101 c170 c172 c173, VS c142 c144 c145).
	//
	// The surface draws go to Remix tagged with the swamp marker; the constants go out when they
	// change, the frame's rings every frame they change (rtx.ds1Swamp.rings, each as the mapping from
	// world x, z to the ring texture). Remix evaluates the field itself: the game's buffer is
	// screen-space and too coarse for path tracing.

	using pass = shared::common::ffp_state::swamp_pass;

	// A draw of one of the passes, with its constants and the render target it draws into.
	void observe(pass kind, const float* vs_const, const float* ps_const, IDirect3DDevice9* dev, unsigned int frame, unsigned int prim_count);

	// A surface draw's mesh (the game's vertex buffer and declaration offsets): measures its metres per
	// texture coordinate once, which the runtime's slope span and ring footprint are in.
	void observe_surface_mesh(IDirect3DVertexBuffer9* vb, UINT stride, INT base_vertex, UINT min_vertex, UINT num_vertices,
		int pos_offset, int uv_offset, int uv_type, unsigned int frame, unsigned int prim_count);

	// The texture a surface draw goes to Remix with: its colour texture with the height texture in its
	// alpha when the height pass reads a texture of its own (FRPG_Snow's s2: Gaping Dragon's slime reads
	// m10_water), else null (the colour texture as bound, Blighttown's swamp).
	IDirect3DBaseTexture9* surface_texture(IDirect3DDevice9* dev, IDirect3DBaseTexture9* colour);

	// A probe-lit map draw (PS_AMBIENT): its c100, the material colour scale Remix does not apply,
	// which the swamp's colours are divided by to keep their ratio to the scene, and its lightmap
	// texture (null when it has none): a lightmapped surface takes the c100 of the geometry sharing its own.
	void observe_map_diffuse(const float* ps_const, unsigned int frame, unsigned int prim_count, IDirect3DBaseTexture9* lightmap);

	// Once per frame: hands the last frame's values to Remix when they changed.
	void frame_update(unsigned int frame);

	// Stage-0 D3DTSS_CONSTANT value of a swamp surface draw (the runtime's kFixedFunctionDs1SwampMarker).
	constexpr DWORD kMarker = 0xD15D5A4F;
}
