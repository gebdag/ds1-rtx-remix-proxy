#pragma once

// Free camera (dev mode, F3): the game's own drawn camera is replaced by one flown with WASD and the mouse, so the game culls,
// sorts and renders from it (Remix with it). The player stands still meanwhile: the game sees no keyboard, mouse or pad
// (the input freeze), and its world keeps running; F7 still stops the game's time.
//
// Where: GameRend::Update (0xEB3420, stdcall GameRend*, float dt) runs every frame after FUN_00EAE760 copied the source camera
// into GameRend + 0x14 (the culling camera, whose position also makes the main pass camera-relative) and + 0x18 (FrpgPersCam).
// It adds camera shake to both and sends them to their render entities (token 0x3D). The free camera is written into both
// (camera + 0x10: rows right, up, forward, position) as it starts, so everything after (culling, reflection, shake, the render
// thread's camera) follows it. The game's own camera system keeps its state and takes over again when the free camera stops.
namespace comp::game::free_cam
{
	void init();

	bool available();
	bool active();
	void toggle();              // F3; takes effect on the next frame the game draws
	void set_active(bool on);

	// Live key Cheats.FreeCamNudge: "forward right up yaw_degrees pitch_degrees", applied once per new value (for tests that
	// may not send input to the game).
	void nudge(float forward, float right, float up, float yaw_degrees, float pitch_degrees);

	float& speed();             // metres a second (Shift x5, Ctrl x0.2)
	float& mouse_sensitivity(); // radians per pixel

	void tab_imgui();
}
