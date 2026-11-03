#pragma once

#include "point_lights.hpp"

namespace comp::game::mesh_lights
{
	// Lights that come from the meshes themselves: emissive meshes the game gives no light (it bakes their glow into
	// lightmaps), named by texture in `<game>\rtx_comp\lights\mesh_lights.txt`. A draw with such a texture is read once:
	// each separate piece of its geometry is one lamp, whose world position comes from the draw's vertices and bones. So
	// every instance gets a light wherever it stands, on any map, with no list of positions. Lamps stay until the map
	// changes, since the mesh is not drawn when out of view while the light on its surroundings still shows.
	//
	// One texture per line: name  dwindleBegin dwindleEnd  r g b  intensityPct
	// The light has the game's falloff and sits at the lamp's middle, or off its wall by `<map>.lamps.txt` (scripts/gen_lamp_offsets.py):
	// at the wall it meets the wall at a grazing angle and lights only the grooves. These meshes are closed boxes, so they carry a
	// marker for the runtime: lighting rays pass them (Surface::isDs1NoShadow).
	// The file is watched, so an edit applies while the game runs.
	constexpr int kFirstSlot = 0x50000;

	// Call with the draw's state still the game's.
	void observe_draw(IDirect3DDevice9* dev, IDirect3DBaseTexture9* texture, D3DPRIMITIVETYPE prim, INT base_vertex,
		UINT start_index, UINT prim_count);

	// True for a texture in the file, once observe_draw has seen it.
	bool is_lamp(IDirect3DBaseTexture9* texture);

	// The lamps seen on this map so far (source mesh, slots from kFirstSlot).
	const std::vector<point_lights::light>& get();
	// The texture a lamp's light comes from, for the F4 list.
	std::string texture_of(int slot);
	const std::string& path();
}
