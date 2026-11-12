#pragma once

#include <Xinput.h>

// What the game receives from each input path, logged once a second with troubleshooting logs on ([Troubleshooting]
// Verbose, or dev mode): the pad as the game reads it (after an input mod such as the Dark Souls Input Customizer has
// written its own), raw keyboard and mouse input through the menu gate, DirectInput reads by device kind, the game's
// keyboard state, and whether a menu holds input. A report of "no input" then shows where the input stops.
namespace comp::game::input_trace
{
	bool enabled();
	// Whether a return address lies in the game's own image (its calls, not an input mod's or the bridge's).
	bool is_game_address(const void* return_address);

	// XInputGetState as its caller gets it back.
	void on_xinput(bool from_game, DWORD user, DWORD result, const XINPUT_STATE* state);
	// One raw input read through the menu gate (the Input Customizer's): kind, press or release, and whether it was dropped.
	void on_raw_keyboard(bool press, bool dropped);
	void on_raw_mouse(LONG dx, LONG dy, USHORT buttons, bool held);
	// A DirectInput GetDeviceState call by its buffer size (keyboard 256, mouse 16 or 20, joystick 80 or 272).
	void on_di_state(bool from_game, DWORD size, bool held);
	// The game's GetKeyboardState: how many keys it saw down.
	void on_keyboard_state(const BYTE* keys, bool held);

	// Once a frame: writes the second's report when one is due and anything happened.
	void flush();
}
