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
	//   x y z  dwindleBegin dwindleEnd  r g b  intensityPct  name  [remix intensity exposure radius volumetric]
	//   [spot dx dy dz coneDeg softness] [focus exponent] [off]
	// With the remix tail the light is a plain Remix sphere light: radiance = colour 0-255 / 255 x intensity
	// x 2^exposure, from a sphere of that radius (metres). Without it, it is lit as the game lights its own:
	// colour times intensityPct / 100 as in the game's POINT_LIGHT_BANK rows, full strength out to
	// dwindleBegin and fading linearly to nothing at dwindleEnd. Both sets are kept, so a light can be
	// switched between them. The optional spot tail aims the light along (dx, dy, dz) with a cone of coneDeg
	// (half angle), an edge softness 0-1 and, for Remix lights, a focus exponent.
	struct light
	{
		std::string name = "light";
		float pos[3] = {};
		float colour[3] = { 1.0f, 0.85f, 0.6f };   // 0-1
		bool enabled = true;
		int when = 0;                               // 0 always, 1 only in the map's dark version, 2 only in its light one (words 'dark' / 'light' in the file)
		// Lit with the game's falloff instead of as a Remix light.
		bool game_falloff = false;
		// Remix sphere light.
		float remix_intensity = 50.0f;
		float exposure = 0.0f;                      // EV: radiance x 2^exposure
		float radius = 0.1f;                        // metres
		float volumetric = 1.0f;                    // volumetric radiance scale
		// The game's falloff.
		float dwindle_begin = 0.5f;
		float dwindle_end = 4.0f;
		float intensity = 1.0f;                     // intensityPct / 100
		// Spotlight.
		bool spot = false;
		float dir[3] = { 0.0f, -1.0f, 0.0f };
		float cone_deg = 30.0f;
		float softness = 0.2f;
		float focus = 0.0f;                         // Remix lights only
		// Which light of the file this is (saved()), -1 for one added since it was read or saved.
		int file_index = -1;
	};

	// Slot ids of the lights get() hands out, kFirstSlot + their index in edit(): past the engine's slots,
	// map_lights' (0x10000), effect_lights' (0x20000) and ffx_lights' (0x30000).
	constexpr int kFirstSlot = 0x40000;

	// The current map's lights (reloading on a map change). Edit them in place; save() writes the file.
	std::vector<light>& edit();
	// The current map's enabled lights as the injector takes them (source custom, slots from 0x40000).
	const std::vector<point_lights::light>& get();

	bool save();
	// Throws away the current map's unsaved changes and reads its file again.
	void reload();
	// Maps left with unsaved changes, kept in memory until the player is back in them (or they are saved or thrown away
	// below): map, light count. Crossing a map boundary does not lose placed lights.
	std::vector<std::pair<std::string, size_t>> kept();
	bool save_kept(const std::string& map);
	void discard_kept(const std::string& map);
	// Goes up each time the lights are read from a file (a map change, a reload): indices into edit() from before are stale.
	unsigned int generation();
	// The light as the file has it (when it was last read or saved), nullptr for one added since.
	const light* saved(const light& l);
	const std::string& path();
	const std::string& status();
	// True when edit() differs from the file.
	bool dirty();
	void mark_dirty();
}
