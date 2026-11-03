#pragma once

namespace comp::game::area_overrides
{
	// Per-area settings from a file made to be read and edited: `<game>\rtx_comp\area_overrides.ini`. An area is one
	// section: a map and optionally a box in the world. While the player is in it, its `proxy.<Section>.<Key>` lines win
	// over remix-comp-proxy.ini and its `rtx.<option>` lines are set on Remix. Leaving, each goes back to what it was.
	// The file is watched, so an edit applies while the game runs. The format is explained at the top of the file itself
	// (assets/area_overrides.ini).
	void frame_update(unsigned int frame);
}
