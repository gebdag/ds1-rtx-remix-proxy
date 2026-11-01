#pragma once

namespace comp::game::dlss_nr
{
	// F1 switches DLSS 3D-Guided Neural Rendering (rtx.dlssNeuralRendering.enable) on and off. Its starting
	// state is what the game's user.conf or rtx.conf sets (else off, Remix's default); a switch made in the
	// Remix menu is not seen here, so the next F1 flips from the proxy's own state.

	// Asks for a switch; it happens at the next frame start on the render thread (Remix API calls belong there).
	void request_toggle();

	// At BeginScene: applies a pending switch.
	void frame_update();
}
