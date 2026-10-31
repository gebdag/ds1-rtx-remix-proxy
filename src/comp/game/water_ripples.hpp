#pragma once

namespace comp::game::water_ripples
{
	// The runtime's ripple field on DS1 water is a window of cells that follows the player
	// (rtx.ds1Ripple.playerPosition, the runtime's rtx_dark_souls_ripples.h): their position, sent every
	// frame it changes. It depends on nothing else about the player; the player light, for one, is not
	// found in every area.
	void frame_update();
}
