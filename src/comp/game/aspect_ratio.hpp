#pragma once

namespace comp::game::aspect_ratio
{
	// Experimental: screens that are not 16:9. The game hard-codes a 16:9 camera and a table of 16:9 window sizes, and
	// stretches its 1280x720 UI over whatever it gets.
	//
	// At a 16:9 output none of the rendering runs: no hook exists, active() is false and every caller leaves at once.
	// The game's window size list only changes for someone off 16:9: on a screen that is not 16:9 (more choices, the
	// output stays what it was), or when DarkSouls.ini holds a size that is not 16:9, which only editing the file can do.
	// [Aspect] Enable=0 turns all of it off. Never in lookdev mode (the vanilla reference). In the lookdev host the
	// camera constants are corrected, since a replay has no game to hook.

	// From the first Direct3DCreate9, which FrpgWindow's constructor makes just before it reads its table of window
	// sizes. A non-16:9 size set in DarkSouls.ini takes the table's last place, so the game's own rule makes it the
	// size its window and device start at. Otherwise only the display step or menu applies it, and not on every boot.
	void before_game_window();

	// From the game's request for a back buffer. `creating_device`: its first, when the size list can be added to.
	void on_game_resolution(UINT width, UINT height, bool creating_device);

	// True while the output is not 16:9.
	extern bool g_active;
	inline bool active() { return g_active; }

	// What the 16:9 world and UI are scaled by to keep their proportions: below 1 on one axis.
	float fit_x();
	float fit_y();

	// The world camera in a constant write, corrected if it is still 16:9. Returns `data` when it has nothing to do.
	const float* camera_constants(UINT start, const float* data, UINT count);

	// The game's last draw copies its finished image to the back buffer with a clip-space quad, letterboxed for a
	// narrower screen and thrown far off it for a wider one (y about 5 million). The image already has the screen's
	// shape: this returns the quad covering the target, or `vertices` when the draw is no such quad.
	const void* final_quad(const void* vertices, UINT stride, UINT count, IDirect3DVertexDeclaration9* declaration);

	// UI parts at the screen's edges (true), or everything in a centred 16:9 box.
	bool hud_at_edges();

	// F4 > HUD: the position choice, shown while active.
	void hud_position_imgui();
}
