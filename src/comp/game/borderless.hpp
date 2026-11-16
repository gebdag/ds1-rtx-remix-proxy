#pragma once
#include <windows.h>
#include <d3d9.h>

// [Window] Borderless=1: the game runs in a borderless window that covers its monitor, whether its own setting is
// fullscreen or windowed.
namespace comp::game::borderless
{
	// Posted to the game window; the proxy's window procedure (wnd_proc_hk) answers it with apply() on the window's own
	// thread. Changing a window's style or size from another thread sends it messages and waits for them, and the
	// game's window thread may be waiting on the draw thread at that moment.
	constexpr UINT kApplyMessage = WM_APP + 0x0B1D;

	bool enabled();

	// CreateDevice and Reset, before the call: a fullscreen request becomes a windowed one.
	void adjust_present_params(D3DPRESENT_PARAMETERS* pp, const char* where);

	// After CreateDevice and Reset, and at every Present: if the window has a frame or does not cover its monitor, it is
	// made borderless (at once on the window's thread, posted from any other).
	void check(HWND window);

	// The window procedure calls this once it is installed, so check() may post to it.
	void set_window_proc_ready();

	// On the window's thread: strip the frame and cover the monitor.
	void apply(HWND window);
}
