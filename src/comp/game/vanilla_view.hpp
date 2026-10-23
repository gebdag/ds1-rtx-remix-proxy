#pragma once

namespace comp::game::vanilla_view
{
	// Vanilla graphics: the game's own look in the running game, for comparing with Remix.
	// Switching on turns off Remix path tracing (rtx.enableRaytracing), FFP conversion,
	// Skip Post-FX and Suppress Aux Passes, the same settings as the F4 menu and Remix menu;
	// switching off restores what they were. F8 or the F4 menu button.

	bool active();

	// Asks for a switch; it happens at the next frame start on the render thread.
	void request_toggle();

	// At BeginScene: applies a pending switch.
	void frame_update();
}
