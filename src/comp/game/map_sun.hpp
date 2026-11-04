#pragma once

namespace comp::game::map_sun
{
	// The one sun of a map, as the game's banks hold it: one direction and strength wherever the player stands.
	//
	// The game's own constants change from region to region: each row of an area's LIGHT_BANK points to a probe cube, and a row's "sun" is that cube
	// read along the direction of its SHADOW_BANK row. Undead Burg's rooms use the same cube as its open air, read along a direction 15 degrees off,
	// so a fifth as strong: no second sun at all. Here: the block's row named basic or outdoors.
	struct values
	{
		float direction[3] = { 0.0f, -1.0f, 0.0f }; // of travel
		float lobe[3] = {};                         // gamma
		float base[3] = {};                         // gamma, the ambient towards the sun, which the lobe's radiance is measured against
		bool has_sun = false;                       // false: the map has none (Firelink, New Londo...)
		bool authored = true;                       // false: the map has no such row: go by what the game draws with
	};

	const values& get(unsigned map_id);
}
