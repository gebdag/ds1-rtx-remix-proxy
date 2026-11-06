#pragma once

namespace comp::game::area_overrides
{
	// Per-area settings from a file made to be read and edited: `<game>\rtx_comp\settings\area_overrides.ini`. An area is one
	// section: a map and optionally a box in the world. While the player is in it, its `proxy.<Section>.<Key>` lines win
	// over remix-comp-proxy.ini and its `rtx.<option>` lines are set on Remix. Leaving, each goes back to what it was.
	// The file is watched, so an edit applies while the game runs. The format is explained at the top of the file itself
	// (assets/area_overrides.ini).
	void frame_update(unsigned int frame);

	// The F4 sun panel's section of the file for this map, or for a box here: colour and direction as [Sun] Colour and Direction. It applies through the file like any other.
	bool save_sun(const std::string& map, const float* box_min, const float* box_max, const float* colour, const float* direction);
	// Takes out the panel's sections of this map.
	bool remove_sun(const std::string& map);
	// Whether this map has a section from the panel.
	bool has_saved_sun(const std::string& map);
}
