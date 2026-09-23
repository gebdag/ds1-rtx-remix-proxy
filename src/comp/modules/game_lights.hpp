#pragma once

namespace comp
{
	// Mirrors the game's live point lights into RTX Remix. Positions come straight from
	// the engine's light manager, so map lights, character torches and FX lights are all
	// covered and follow their owners without any per-map data file.
	class game_lights final : public shared::common::loader::component_module
	{
	public:
		game_lights();
		~game_lights();

		static game_lights* get() { return p_this; }

		void frame_update();
		void destroy_all_handles();
		void draw_light_instances();
		void tab_imgui();
		void draw_pos_hud();

		static void REMIXAPI_PTR remix_draw_callback();

		bool enabled = true;
		void set_enabled(bool e)
		{
			enabled = e;
		}

		float intensity_scale = 1.0f;
		float radius_scale = 1.0f;

		// The sun: the game's key directional light as a Remix distant light.
		// Independent of `enabled`, which covers the point lights only.
		bool sun_enabled = true;
		float sun_intensity_scale = 1.0f;
		float sun_angular_diameter = 0.53f;
		float sun_fallback_colour[3] = { 0.7f, 0.7f, 0.7f };

		int active_light_count() const { return static_cast<int>(live_lights_.size()); }
		int total_light_count() const { return harvested_count_; }
		int create_failed_count() const { return create_failed_; }
		int zero_radiance_count() const { return zero_radiance_; }

	private:
		static inline game_lights* p_this = nullptr;

		// One Remix light per engine slot. Kept across frames and updated in place so
		// moving lights (torches, pickups) keep their identity instead of being
		// destroyed and recreated every frame.
		struct live_light
		{
			remixapi_LightHandle handle = nullptr;
			remixapi_LightInfoSphereEXT sphere = {};
			remixapi_LightInfo info = {};
			uint64_t hash = 0;
			UINT last_seen_frame = 0;
		};

		bool try_init_remix_api();
		void sync_lights();
		void sync_sun();
		void release_light(live_light& light);
		void release_sun();
		bool submit_light(const remixapi_LightInfo& info, remixapi_LightHandle& handle);
		void note_destroyed(uint64_t hash);
		bool destroyed_this_frame(uint64_t hash) const;
		void sun_imgui();

		std::unordered_map<int, live_light> live_lights_;

		remixapi_LightHandle sun_handle_ = nullptr;
		remixapi_LightInfoDistantEXT sun_distant_ = {};
		remixapi_LightInfo sun_info_ = {};
		int sun_create_failed_ = 0;

		// Frame in which each light hash was last destroyed. The runtime defers
		// DestroyLight to Present and keys lights by hash, so a create for the same
		// hash earlier in that frame is destroyed along with the old light.
		std::unordered_map<uint64_t, UINT> destroyed_frame_;
		int harvested_count_ = 0;
		int synthetic_count_ = 0;   // of harvested_count_, how many are stand-ins
		int create_failed_ = 0;   // Remix refused the light this frame
		int zero_radiance_ = 0;   // created, but the row's colour makes it emit nothing
		bool remix_api_ready_ = false;
		bool show_pos_hud_ = false;
		int max_active_lights_ = 0;
		float emitter_radius_ = 0.1f;
		bool probe_light_mode_ = false;
		float probe_position_[3] = {};
		float probe_radiance_ = 1.0f;
		float probe_radius_ = 0.5f;
		float manual_offset_[3] = {};
	};
}
