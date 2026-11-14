#pragma once

namespace comp::game::game_ui
{
	// The game's HUD and menus hidden and shown again (F2), for clean screenshots: Remix's rtx.showGameUi. Everything drawn
	// after the path traced frame goes with it, the F4 menu and the proxy's overlays too; the Remix menu (Alt+X) stays.
	void request_toggle();
	// Applies a requested toggle; once a frame, before its draws.
	void frame_update();
}
