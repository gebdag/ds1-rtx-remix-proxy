#pragma once

namespace comp::game::camera
{
	void init();

	// Returns true if the camera position could be read from game memory.
	bool get_world_pos(float& x, float& y, float& z);

	// Returns true if the character position could be read from game memory.
	bool get_char_pos(float& x, float& y, float& z);

	// Character facing as the angle stored beside the position in the player's
	// position block (+0x04, radians); see
	// game_lights' player light readout, which prints it next to the camera heading.
	bool get_char_heading(float& radians);

	// True camera world position recovered from the main culling frustum's apex,
	// in the engine's raw {X, Z, Y} order. False until a main camera frustum has been
	// identified (see the lock in camera.cpp).
	bool get_camera_apex_pos(float& x, float& y, float& z);

	// Why the last get_override_pos() answered the way it did — the F4 tab shows this,
	// because the silent fall back to the player position is what makes lights slide.
	struct apex_lock_state
	{
		const void* frustum;    // locked main-camera frustum, null while unlocked
		int frustums_seen;      // distinct frustums offered last frame
		float distance_to_char; // of the locked apex, in engine raw order
		unsigned relocks;       // how often the lock had to be re-acquired
		bool used_player_fallback;
	};
	const apex_lock_state& lock_state();

	bool is_available();

	// Runtime toggles (exposed to F4 menu)
	struct settings
	{
		bool use_game_camera = true;    // Read position from game memory AOB
		int override_source_mode = 0;   // 0=Auto, 1=Camera, 2=Character
		bool use_frustum_apex = true;   // Camera position from the culling frustum apex
		bool inject_view_pos = true;    // Inject game memory camera translation into View matrix
		bool offset_world = true;       // Add camera pos to World matrix translations (camera-relative → absolute)
		float manual_offset[3] = {};    // Manual XYZ offset for testing
	};

	settings& get_settings();

	// Resolve the current transform-override position based on runtime settings.
	bool get_override_pos(float& x, float& y, float& z, const char** sourceLabel = nullptr);

	// ImGui tab for F4 menu
	void tab_imgui();
}
