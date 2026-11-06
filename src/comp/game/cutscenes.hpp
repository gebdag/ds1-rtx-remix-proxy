#pragma once

namespace comp::game::cutscenes
{
	// F4 > Cutscenes: plays any of the game's cutscenes (remo scenes) on request, the ones that only ever play once included, and
	// freezes the game on a frame of one. A scene goes through the game's own request, RemoManStep at 0xEE1950, called from the event
	// system's own thread: the same place an event script's PlayCutscene runs. The request sets no event flags (the scripts do), so
	// playing a scene changes nothing in the save.
	void init();
	void tab_imgui();

	// Stops and restarts the game's time (F7).
	void toggle_freeze();
}
