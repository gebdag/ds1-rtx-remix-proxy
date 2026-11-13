#pragma once

namespace comp::game::cheats
{
	void init();

	bool is_god_mode_available();
	bool& god_mode_enabled();

	void tab_imgui();

	// Saved teleport spots (F4 > Cheats, rtx_comp\teleport_spots.txt): a reproducible place to stand, for tests. The live keys
	// proxy.Cheats.TeleportSpot = <name> and proxy.Cheats.TeleportXYZ = x, y, z do the same from a script.
	bool teleport_to_spot(const std::string& name);
	bool teleport_to_xyz(float x, float y, float z);
	// The player's position on screen, top left, while on (F4 > Cheats, "Show coordinates on screen").
	void draw_coordinates_overlay();
}
