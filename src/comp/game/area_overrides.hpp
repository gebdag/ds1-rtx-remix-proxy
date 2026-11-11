#pragma once

namespace comp::game::area_overrides
{
	// Per-area settings from a file made to be read and edited: `<game>\rtx_comp\settings\area_overrides.ini`. An area is one
	// section: a map and optionally a box in the world. While the player is in it, its `proxy.<Section>.<Key>` lines win
	// over remix-comp-proxy.ini and its `rtx.<option>` lines are set on Remix. Leaving, each goes back to what it was.
	// The file is watched, so an edit applies while the game runs. The format is explained at the top of the file itself
	// (assets/area_overrides.ini).
	void frame_update(unsigned int frame);

	// `lights = <prefix>` in a section: this map's custom lights whose names start with the prefix belong to that area, and
	// are there only while the player is in it (in any of them, when several sections claim the prefix). The area's name
	// when the light belongs to one, nullptr otherwise.
	const char* custom_light_area(const std::string& light_name);
	// True while the light's area (custom_light_area) is not the one the player is in.
	bool custom_light_away(const std::string& light_name);

	// The F4 sun panel's section of the file for this map, or for a box here: colour and direction as [Sun] Colour and Direction. It applies through the file like any other.
	bool save_sun(const std::string& map, const float* box_min, const float* box_max, const float* colour, const float* direction);
	// Takes out the panel's sections of this map.
	bool remove_sun(const std::string& map);
	// Whether this map has a section from the panel.
	bool has_saved_sun(const std::string& map);
	// The panel's saved sun for this map: its colour, direction and whether it is for a box. False when none is saved.
	bool saved_sun(const std::string& map, float colour[3], float direction[3], bool& in_box);
}
