#pragma once

namespace comp::game::frustum_culling
{
	void init();
	void install_hooks();
	bool is_available();
	bool is_enabled();
	float radius();
	void set_enabled(bool enabled);
	void set_radius(float radius);

	// Camera position recovered from a frustum's own planes. The engine builds those
	// planes in world space from the camera transform (Gribb-Hartmann, VA 0x00FF6770),
	// so the apex is an absolute world position in the same space as the map geometry —
	// the one thing the camera-relative main pass never uploads.
	struct apex_sample
	{
		const void* frustum;
		float x;
		float y;
		float z;
		UINT tests;
	};

	// Distinct frustums the proxy will solve per frame. Generous on purpose: the main
	// camera has to be among them, and it competes with shadow cascades, the cull
	// camera, the water-reflection camera and the env-capture faces.
	constexpr int MAX_FRAME_APEXES = 16;

	// Distinct frustums seen this frame (main camera plus shadow cascades).
	int frame_apexes(apex_sample* out, int max_out);
}