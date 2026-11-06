#pragma once
#include <windows.h>

namespace comp::game::fps_unlock
{
	void init();
	bool is_active();

	// Freezes the game's time: its step per frame all but stops, which the game reads everywhere it advances anything, cutscenes
	// included. Not exactly 0: nothing divides by it in the game's own code, but the physics may.
	void set_frozen(bool frozen);
	bool is_frozen();
	int  target_fps();
	void set_target_fps(int fps);
	// Temporarily disable the frame-cap busywait (used by the benchmark so
	// measured FPS isn't clamped to TargetFPS). Timestep patches stay active.
	void set_cap_bypass(bool bypass);
	void on_present();
}
