#pragma once

namespace comp::game::texture_scroll
{
	// Dark Souls scrolls a material's textures by its MTD's g_TexScroll_0/1/2 (a velocity per UV
	// set; the game passes the accumulated offsets as VS c246/c247/c248). Map vertex shaders compute
	// uv = TEXCOORD0.xy / 1024 + c246.xy and the overlay uv = TEXCOORD0.zw / 1024 + c247.xy.
	// FFP conversion bakes TEXCOORD / 1024 into the vertices, so the offset reaches Remix as stage
	// 0's texture transform (D3DTS_TEXTURE0, D3DTTFF_COUNT2): waterfalls, lava and the like flow.
	//
	// Water surfaces (FRPG_Water_*) do not read their UVs: their waves come from a screen-space
	// height field the FRPG_Water_HeightMap pass draws from the same mesh first, three layers of one
	// wave texture at mesh_uv * c141.i + c246 / c247.xy / c248 (in wave tiles), weighted by PS c134.i,
	// times the water's wave height PS c126.x. Remix shades water with two layers of that wave texture
	// as a normal map (the ds1_water mod, TILES tiles per texture coordinate), so each water surface
	// gets the game's own layers:
	//   layer 1   stage 0's texture transform: uv * c141.x / TILES + c246.xy / TILES
	//   layer 2   stage 1's texture transform, relative to layer 1's uv (the runtime's
	//             Surface::ds1WaterLayer2): uv1 * c141.y / c141.x + offset
	//   strength  stage 1's D3DTSS_CONSTANT, two halfs: each layer's slope, c126 * c134.i * c141.i
	//             (layer 3 folded into layer 2), over the slope the normal map was authored with.
	//   reflection D3DRS_TEXTUREFACTOR: alpha = the reflection weight seen face-on, c130 * c131
	//             (the game's weight is lerp(c130, 1, fresnel^c129) * c131), rgb = its tint c132.
	// A FRPG_Water_HeightMap draw: remembers its mesh's wave layers for this frame.
	void observe_water_heightmap(const float* vs_const, const float* ps_const, IDirect3DVertexBuffer9* vb,
		IDirect3DIndexBuffer9* ib, INT base_vertex, UINT start_index, UINT prim_count, unsigned int frame);

	// Sets the texture transforms for the next converted draw: the base uv's scroll (overlay
	// false), the overlay uv's (overlay true), or for a water surface its HeightMap draw's layers.
	// Returns true when it set any; call clear after the draw.
	bool apply(IDirect3DDevice9* dev, bool overlay, bool water_surface, IDirect3DVertexBuffer9* vb,
		IDirect3DIndexBuffer9* ib, INT base_vertex, UINT start_index, UINT prim_count, unsigned int frame);

	// Turns the texture transforms apply set off again.
	void clear(IDirect3DDevice9* dev);
}
