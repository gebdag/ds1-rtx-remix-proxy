#pragma once

namespace comp::game::input_customizer
{
	// Support for the "Dark Souls Input Customizer" mod (a dinput8.dll in the game folder).
	// Does nothing when the mod is not installed.
	void init();

	// Call from the game window procedure, which runs on the window's thread.
	void on_window_message(HWND window);

	// The mod's module, or null. It turns keyboard and mouse into a virtual pad behind its own XInputGetState detour, so a
	// pad read can come from it on the game's behalf (lookdev_capture's menu input hold).
	HMODULE module();
}
