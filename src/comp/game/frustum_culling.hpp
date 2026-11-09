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

	// Light blockers: large geometry the engine culls that a path tracer still needs
	// to stop sky and sun light leaking in (walls, roofs, cliff faces off screen).
	struct light_blocker_settings
	{
		float min_size = 8.0f;     // bounding diameter a shape needs to count as a blocker
		float near_range = 50.0f;  // blockers this close to the camera are always kept
		float sun_reach = 200.0f;  // how far a blocker's shadow is swept along the sun
	};

	// Engine rejections of perspective frustums in the last complete frame, and how
	// many of them were kept.
	struct light_blocker_stats
	{
		UINT rejected = 0;
		UINT kept_near = 0;
		UINT kept_sun = 0;
		UINT kept_regions = 0;     // of the kept ones, region (AABB) tests
		UINT unreadable_boxes = 0; // oriented boxes whose transform layout was not recognised
	};

	bool keeps_light_blockers();
	void set_keep_light_blockers(bool keep);
	light_blocker_settings light_blockers();
	void set_light_blockers(const light_blocker_settings& settings);
	// After the config changed underneath (an area override came or went).
	void reread_light_blockers();
	light_blocker_stats last_frame_light_blockers();

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