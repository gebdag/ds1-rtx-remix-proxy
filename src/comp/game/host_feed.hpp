#pragma once

#include "point_lights.hpp"

// Game state for the lookdev host. The host replays a capture through this
// proxy in a process without the game, so everything the modules would read
// from game memory travels in the capture instead (written by lookdev_capture
// in the game) and is pushed in here by the host through the exported
// lookdev_host_feed / lookdev_host_texture_name. In the game process nothing
// here is used.
namespace comp::game::host_feed
{
	// Per-frame values, in the units the game-side accessors return them.
	struct frame_state
	{
		bool camera_valid = false;
		float camera[3] = {};    // camera::get_override_pos
		bool player_valid = false;
		float player[3] = {};    // camera::get_char_pos (raw)
		bool heading_valid = false;
		float heading = 0.0f;    // camera::get_char_heading
		unsigned map_id = 0;     // map_lights::current_map_id
	};

	const frame_state& frame();

	// point_lights::harvest() in the host.
	const std::vector<point_lights::light>& lights();
}
