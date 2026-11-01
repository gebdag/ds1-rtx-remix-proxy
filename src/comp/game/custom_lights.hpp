#pragma once

#include "point_lights.hpp"

namespace comp::game::custom_lights
{
	// Lights placed by hand, per map: `<game>\rtx_comp\lights\<map>.custom.txt`, read when that map is
	// the current one (map_lights::current_map_name) and only then, so a light saved in one map never
	// appears in another. Positions are the engine's world coordinates (x, up, depth), the space the
	// game's own lights and the map tables use. Shipped tweaks and the F4 > Lights > Custom lights editor
	// write the same file.
	//
	// One light per line:
	//   x y z  dwindleBegin dwindleEnd  r g b  intensityPct  name  [spot dx dy dz coneDeg softness]
	// colour 0-255 times intensityPct / 100 as in the game's POINT_LIGHT_BANK rows; the optional spot
	// tail aims the light along (dx, dy, dz) with a cone of coneDeg (half angle) and an edge softness 0-1.
	struct light
	{
		std::string name = "light";
		float pos[3] = {};
		float dwindle_begin = 0.5f;
		float dwindle_end = 4.0f;
		float colour[3] = { 1.0f, 0.85f, 0.6f };   // 0-1
		float intensity = 1.0f;                     // intensityPct / 100
		bool enabled = true;
		bool spot = false;
		float dir[3] = { 0.0f, -1.0f, 0.0f };
		float cone_deg = 30.0f;
		float softness = 0.2f;
	};

	// Slot ids of the lights get() hands out, kFirstSlot + their index in edit(): past the engine's slots,
	// map_lights' (0x10000), effect_lights' (0x20000) and ffx_lights' (0x30000).
	constexpr int kFirstSlot = 0x40000;

	// The current map's lights (reloading on a map change). Edit them in place; save() writes the file.
	std::vector<light>& edit();
	// The current map's enabled lights as the injector takes them (source custom, slots from 0x40000).
	const std::vector<point_lights::light>& get();

	bool save();
	void reload();
	const std::string& path();
	const std::string& status();
	// True when edit() differs from the file.
	bool dirty();
	void mark_dirty();
}
