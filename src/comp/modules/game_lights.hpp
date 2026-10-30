#pragma once

#include "shared/common/config.hpp"
#include <array>
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

		static void remix_draw_callback();

		bool enabled = true;   // the point lights (harvested + synthesized); sun and player have their own
		void set_enabled(bool e)
		{
			enabled = e;
		}

		float fire_light_scale = 2.0f;
		float intensity_scale = 1.0f;
		float radius_scale = 1.0f;

		// The sun: the game's key directional light as a Remix distant light.
		bool sun_enabled = true;
		float sun_intensity_scale = 1.0f;
		float sun_angular_diameter = 0.53f;
		float sun_fallback_colour[3] = { 0.7f, 0.7f, 0.7f };

		// The sun as last submitted to Remix (radiance and direction); false while there is none.
		bool submitted_sun(float* radiance, float* direction) const
		{
			if (!sun_handle_)
				return false;
			radiance[0] = sun_info_.radiance.x; radiance[1] = sun_info_.radiance.y; radiance[2] = sun_info_.radiance.z;
			direction[0] = sun_distant_.direction.x; direction[1] = sun_distant_.direction.y; direction[2] = sun_distant_.direction.z;
			return true;
		}

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
			effect,       // stand-in for a lit effect's draws (game::effect_lights)
			unresolved,   // live slot with no usable parameters
			count
		};

		// Whether a list row's light reaches Remix, and if not, why.
		enum class row_state : unsigned char
		{
			on,           // drawn
			rejected,     // Remix refused the light
			lights_off,   // the Point lights switch is off
			no_params,    // the engine slot has no colour or range to draw
			effect_off,   // a follows-effect stand-in whose effect is not drawn at its spot
			source_off,   // its source's switch is off
			disabled,     // switched off in the list
			solo_other,   // another light is soloed
			over_limit,   // past Max drawn
		};

		// F4 > Lights > Game falloff for one kind (also proxy.Lights.GameFalloff<Kind> live).
		void set_kind_game_falloff(light_kind k, bool on) { kind_game_falloff_[static_cast<int>(k)] = on; }
		// A lightmapped map draw whose shader reads the game's point lights (c112-c119): the lights the game
		// bound to it, camera-relative positions in c112-c115 (colour c116-c119). A map light the game binds to
		// lightmapped geometry is a dynamic light there and goes to Remix at the game's strength; one it never
		// binds is baked into the lightmap, and Remix's light stands in for that glow (sync_lights).
		void observe_lightmapped_draw(const float* ps_const);

	private:
		// World positions of the lights the game has bound to lightmapped draws (observe_lightmapped_draw).
		std::vector<std::array<float, 3>> lightmap_bound_;
		bool bound_to_lightmap(float x, float y, float z) const;
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
			row_state state = row_state::on;
			int slot = -1;
			std::string name;
			std::string group;          // sub-group within its source (a stand-in's category), else empty
			std::string origin;         // where the light comes from, for the Details column
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
		// Appends the player light to falloff_table when it uses the game's falloff.
		void sync_player_light(const game::point_lights::light* src, std::string& falloff_table);
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
		void list_row_imgui(const light_row& row);
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
		bool player_hash_sent_ = false;   // rtx.ds1Lights.playerLight
		bool player_clearance_refused_ = false; // the runtime refused playerLight / playerAnchor
		float player_anchor_sent_[3] = {}; // rtx.ds1Lights.playerAnchor as last sent

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
		bool kind_enabled_[static_cast<int>(light_kind::count)] = { true, true, true, true, true, true, true, true, true };
		// Kinds lit with the game's falloff (rtx.ds1Lights.falloffTable) instead of inverse square:
		// every kind the game gives a falloff (it shades all its point lights flat to dwindle begin,
		// then linearly to zero at dwindle end). Only the sun, and unresolved slots with no
		// parameters, stay out.
		bool kind_game_falloff_[static_cast<int>(light_kind::count)] = { false, true, true, true, true, true, true, true, false };
		bool game_falloff_ = true;        // master switch ([Lights] GameFalloff)
		std::string falloff_table_sent_;
		uint64_t selected_ = KEY_SUN;
		uint64_t solo_ = 0;
		bool flash_selected_ = false;
		bool group_rows_ = true;          // sections by source (stand-ins by category), else one flat list
		int sort_mode_ = 0;               // within a section (or the flat list): 0 distance, 1 name
		char filter_[64] = {};
		std::string saved_text_;
		std::string save_status_;
	};
}
