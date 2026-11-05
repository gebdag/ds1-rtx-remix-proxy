#pragma once

#include "shared/common/ffp_state.hpp"

namespace comp::game::waterfall
{
	// Whether the player stands in falling water, for the runtime's soak and the droplets bouncing off the body (rtx.ds1Wet.fall*).
	// Two signals: the near-vertical triangles of the map's waterfall meshes, known by texture name, kept for the map once drawn; and the
	// sprites of a waterfall effect. Those sprites are mostly shared textures (s00018 is common smoke), so they only count near a live
	// instance of a waterfall effect, which only the game has: the lookdev host sees the meshes alone.
	void observe_draw(IDirect3DDevice9* dev, IDirect3DBaseTexture9* texture, D3DPRIMITIVETYPE prim, INT base_vertex, UINT start_index, UINT prim_count);
	void observe_sprites(const shared::common::ffp_state::sprite_point* points, UINT count);
	void frame_update(unsigned int frame);
}
