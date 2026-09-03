#pragma once
#include <windows.h>

namespace comp::game::fps_unlock
{
	void init();
	bool is_active();
	int  target_fps();
	void set_target_fps(int fps);
	// Temporarily disable the frame-cap busywait (used by the benchmark so
	// measured FPS isn't clamped to TargetFPS). Timestep patches stay active.
	void set_cap_bypass(bool bypass);
	void on_present();
}
