#pragma once

#include "shared/common/config.hpp"
#include "../game/point_lights.hpp"

namespace comp
{
	// Mirrors the game's lights into RTX Remix: the sun (the game's shadow / key
	// directional light), the player's head light, and every live point light the
	// engine's light manager holds (map lights, FX lights, character lights), plus
	// synthesized stand-ins for maps the engine never lights. F4 -> Lights lists them
	// all in one table; the sun and the player light are pinned on top.
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

		bool enabled = true;   // the point lights (harvested + synthesized); sun and player have their own
		void set_enabled(bool e)
		{
			enabled = e;
		}

		float intensity_scale = 1.0f;
		float radius_scale = 1.0f;

		// The sun: the game's key directional light as a Remix distant light.
		bool sun_enabled = true;
		float sun_intensity_scale = 1.0f;
		float sun_angular_diameter = 0.53f;
		float sun_fallback_colour[3] = { 0.7f, 0.7f, 0.7f };

		int active_light_count() const { return static_cast<int>(live_lights_.size()); }
		int total_light_count() const { return harvested_count_; }
		int create_failed_count() const { return create_failed_; }
		int zero_radiance_count() const { return zero_radiance_; }

		// What a list row is, which also decides its Source column and sort rank.
		enum class light_kind : unsigned char
		{
			sun,          // distant light from the game's shadow / key light constants
			player,       // the player's head light
			map_row,      // KPointLightMan slot with a POINT_LIGHT_BANK row: MSB map lights
			bank_entity,  // BankPointLightEntity resolved through its handle
			lerp_entity,  // LerpBankPointLightEntity (character lights)
			fx_direct,    // DirectPointLightEntity (FX / SFX / item lights)
			synthetic,    // stand-in from rtx_comp\lights\<map>.txt
			unresolved,   // live slot with no usable parameters
			count
		};

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

		// Per-light edits made in the list. Static lights (map rows, bank entities,
		// synthesized) are keyed by kind + position and saved; moving ones by slot,
		// for this session only.
		struct light_override
		{
			bool disabled = false;
			float multiplier = 1.0f;
			bool recolour = false;
			float colour[3] = { 1.0f, 1.0f, 1.0f };
			bool is_default() const { return !disabled && multiplier == 1.0f && !recolour; }
		};

		// A row of the list, rebuilt every frame by sync_lights.
		struct light_row
		{
			uint64_t key = 0;
			light_kind kind = light_kind::unresolved;
			int slot = -1;
			std::string name;
			std::string origin;
			float pos[3] = {};
			float distance = 0.0f;      // from the player (0 for the sun)
			float colour[3] = {};       // the game's colour
			float range_begin = 0.0f;
			float range_end = 0.0f;
			float radiance[3] = {};     // what was sent to Remix
			bool drawn = false;
		};

		static constexpr uint64_t KEY_SUN = 1;
		static constexpr uint64_t KEY_PLAYER = 2;

		bool try_init_remix_api();
		void sync_lights(bool generic);
		void sync_sun();
		void release_light(live_light& light);
		void release_sun();
		bool submit_light(const remixapi_LightInfo& info, remixapi_LightHandle& handle);
		void note_destroyed(uint64_t hash);
		bool destroyed_this_frame(uint64_t hash) const;

		const game::point_lights::light* find_player_light(const std::vector<game::point_lights::light>& harvested);
		void sync_player_light(const game::point_lights::light* src);
		bool player_offset_world(float* out) const;

		// Row construction and per-light edits.
		light_row make_row(const game::point_lights::light& l, const float* player) const;
		const light_override* find_override(uint64_t key) const;
		float edit_multiplier(uint64_t key) const;   // solo / flash / override, 0 = off

		// Settings persistence: [Lights], [Sun], [PlayerLight] in the INI, overrides
		// in rtx_comp\lights\light_overrides.txt, categories through map_lights.
		std::string serialize() const;
		bool save_all();
		void revert_to_saved();
		std::string overrides_path() const;
		bool load_overrides();

		// UI pieces.
		void list_imgui();
		void sun_imgui();
		void player_light_imgui();
		void point_light_imgui(const light_row& row);
		void diagnostics_imgui();

		std::unordered_map<int, live_light> live_lights_;
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

		// The player's head light, split out of the harvested lights (see
		// config::player_light_settings). Found by attachment: the live light nearest
		// the player's head, then held while it stays with the player.
		shared::common::config::player_light_settings player_cfg_;
		live_light player_light_;
		int player_slot_ = -1;            // KPointLightMan slot currently taken as the player light
		bool player_seen_ = false;        // found on the last harvest
		game::point_lights::light player_src_ = {}; // its last harvested values, for the readout
		float player_head_dist_ = 0.0f;   // distance of that light from the head point
		float player_radiance_[3] = {};

		remixapi_LightHandle sun_handle_ = nullptr;
		remixapi_LightInfoDistantEXT sun_distant_ = {};
		remixapi_LightInfo sun_info_ = {};
		int sun_create_failed_ = 0;

		// Frame in which each light hash was last destroyed. The runtime defers
		// DestroyLight to Present and keys lights by hash, so a create for the same
		// hash earlier in that frame is destroyed along with the old light.
		std::unordered_map<uint64_t, UINT> destroyed_frame_;

		// The list.
		std::vector<light_row> rows_;
		std::unordered_map<uint64_t, light_override> overrides_;
		bool kind_enabled_[static_cast<int>(light_kind::count)] = { true, true, true, true, true, true, true, true };
		uint64_t selected_ = KEY_SUN;
		uint64_t solo_ = 0;
		bool flash_selected_ = false;
		int sort_mode_ = 0;               // 0 type, 1 distance, 2 source, 3 name
		char filter_[64] = {};
		std::string saved_text_;
		std::string save_status_;
	};
}
