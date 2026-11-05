#pragma once

#include <cstdint>
#include <vector>

struct IDirect3DBaseTexture9;

namespace comp::game::eye_sprites
{
	// Enemy eyes (FFX 12501, 12900, 12901...): tiny additive quads of sprite s01036 at a head's dummy. Returns the vertices to
	// draw: a copy in `copy` with more alpha for an eye's quads ("a bit more emissive"), the same pointer for anything else.
	const void* adjust(IDirect3DBaseTexture9* texture, const void* vertices, unsigned stride, unsigned min_index, unsigned num_verts,
		unsigned frame, int colour_offset, std::vector<std::uint8_t>& copy);

	void set_gain(float gain);
}
