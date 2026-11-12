#pragma once
#include "../game/map_sun.hpp"

#include "shared/common/config.hpp"
#include <array>
#include <set>
#include "../game/point_lights.hpp"
#include "../game/custom_lights.hpp"

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
		// For screenshots of the sun's panel (live option proxy.Lights.SelectSun).
		void select_sun();
		void draw_pos_hud();

		static void remix_draw_callback();

		bool enabled = true;   // the point lights (harvested + synthesized); sun and player have their own
		void set_enabled(bool e)
		{
			enabled = e;
		}

		float fire_light_scale = 1.0f;
		unsigned lightmap_bound_map_id_ = 0;
		// A map with no named sun row: the sun the game draws with there now.
		unsigned seen_sun_map_ = 0;
		int seen_sun_frames_ = 0;
		bool seen_map_light_ = false; // the map's geometry has shown its own key light (map c95) since the map was entered
		game::map_sun::values seen_sun_;
		bool turned_off_as_character_only(const game::point_lights::light& l);
		float effect_table_scale = 1.0f; // [Lights] EffectTableScale, the colour of table effect lights: fire 1.8x in light, as the game's own fire lights
		float intensity_scale = 1.0f;
		float radius_scale = 1.0f;

		// The sun: the game's key directional light as a Remix distant light.
		bool sun_enabled = true;
		float sun_intensity_scale = 1.0f;
		float sun_angular_diameter = 0.53f;
		// [Ambient] Lock (gamma, usually from an area override): the ambient as this one value on every side, whatever the game's dominant region says this frame.
		// [Ambient] PreferScale: the area's ambient comes only from the draws of the set whose c86.x this is (0 = the frame's dominant set).
		float ambient_prefer_scale = 0.0f;
		bool ambient_lock_set = false;
		float ambient_lock[3] = {};
		float sun_fallback_colour[3] = { 0.7f, 0.7f, 0.7f };
		// An area's own sun ([Sun] Colour and Direction, from area_overrides.ini) where the game bakes its sun into the
		// lightmaps and its probe holds none. Colour in the game's gamma units, direction of travel.
		bool sun_colour_set = false;
		float sun_colour[3] = {};
		bool sun_direction_set = false;
		float sun_direction[3] = {};

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
		// Logs sudden changes of the frame's light (ambient, sun, strongest live light), budgeted, for what a capture cannot show.
		void log_look_watch();
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
			ffx_table,    // an unlit effect's light from the effect table (game::ffx_lights)
			custom,       // placed by hand, per map (game::custom_lights)
			mesh,         // on a mesh the game gives no light, by its texture (game::mesh_lights)
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
			characters,   // the game binds it to characters only (it lights none of the map)
		};

		// F4 > Lights > Game falloff for one kind (also proxy.Lights.GameFalloff<Kind> live).
		void set_kind_game_falloff(light_kind k, bool on) { kind_game_falloff_[static_cast<int>(k)] = on; }
		// Appends the light list as drawn (kind, state, position, the game's colour and range, radiance) to
		// rtx_comp\light_rows.txt (live option proxy.Lights.Dump).
		void dump_rows() const;
		// A lightmapped map draw whose shader reads the game's point lights (c112-c119): the lights the game
		// bound to it, camera-relative positions in c112-c115 (colour c116-c119). A map light the game binds to
		// lightmapped geometry is a dynamic light there and goes to Remix at the game's strength; one it never
		// binds is baked into the lightmap, and Remix's light stands in for that glow (sync_lights).
		void observe_lightmapped_draw(const float* ps_const, int light_count);
		// Any draw whose pixel shader reads the game's point lights: which lights the game binds to map
		// geometry and which to characters (skinned draws, and the weapons, shields and armour a bone
		// holds). A light it gives characters only (a map Light event lighting whoever stands in a
		// beam of baked sunlight) lights none of the map in the game; drawn
		// as a Remix light it lights the map around it (row_state::characters).
		void observe_draw_lights(const float* ps_const, int light_count, bool character);
		// The INI's values changed under the running game (game/area_overrides): takes them, keeps per-light edits.
		void reread_settings();
		// The sun's own colour and direction as the settings give them now (the INI, and an area's saved sun on top).
		void read_own_sun();

		// Live diagnostics (atmosphere.cpp set_proxy_option).
		void set_emitter_radius(float r) { emitter_radius_ = r; }
		void set_player_intensity(float v) { player_cfg_.falloff_intensity = std::max(v, 0.0f); }
		void set_player_gamma_add(bool on) { player_cfg_.falloff_gamma_add = on; }
		void set_kind_enabled(int k, bool on) { if (k >= 0 && k < static_cast<int>(light_kind::count)) kind_enabled_[k] = on; }

		// A path traced menu view's light (proxy.MenuScene.Key): position, radiance, radius. Off-screen, front left and above.
		float menu_key[7] = { -1.5f, 2.5f, -1.8f, 120.0f, 112.0f, 100.0f, 0.3f };

	private:
		void read_settings();
		// World positions of the lights the game has bound to lightmapped draws (observe_lightmapped_draw).
		std::vector<std::array<float, 3>> lightmap_bound_;
		bool bound_to_lightmap(float x, float y, float z) const;
		// The same for one light, kept for the map once matched, by its engine slot and entity handle: a map light that sways (the
		// intro shaft's torch bank lights move a few cm a frame) matched the positions seen only now and then, so its strength
		// jumped by pi from frame to frame until enough of its positions were seen (F6 dump lights_20261111_224543).
		bool bound_to_lightmap(const game::point_lights::light& l);
		std::unordered_map<uint64_t, std::array<float, 3>> lightmap_bound_lights_; // where each was latched (light_identity / latched in game_lights.cpp)
		unsigned lightmap_bound_lights_map_ = 0;
		// Lights the game bound to map draws and to character draws on the current map (observe_draw_lights).
		std::vector<std::array<float, 3>> map_bound_, character_bound_;
		unsigned bound_map_id_ = 0;
		bool characters_only(float x, float y, float z) const;
		// The same for one light, by its engine slot and entity handle, kept for the map: once the game has bound it to a map draw
		// it lights the map; otherwise once it has been bound to a character draw it lights characters only. Matched by position
		// each frame instead, the intro shaft's swaying torch light (Map light 17) fell outside every map position it had been seen
		// at for single frames and went out (F6 dump lights_20261111_225447: 5.6, 6.7 and 11.7 s).
		bool characters_only(const game::point_lights::light& l);
		std::unordered_map<uint64_t, std::array<float, 3>> lit_map_lights_, lit_character_lights_;
		unsigned lit_lights_map_ = 0;
		bool skip_characters_only_ = true;
		void custom_lights_imgui();
		// Every setting of one custom light (the custom section and a custom row's panel); true when one changed.
		bool custom_light_fields_imgui(game::custom_lights::light& l);
		// Duplicate, copy and paste for one custom light. May add to the list: nothing held into it stays valid after.
		void custom_light_actions_imgui(int index);
		// Save / reload of the current map's custom lights file.
		void custom_lights_file_buttons();
		int custom_selected_ = -1;
		// Every custom light selected under 'Custom lights for this map' (Shift / Ctrl), by index; empty: just custom_selected_.
		std::set<int> custom_selection_;
		int custom_anchor_ = -1;               // where a Shift range starts
		// The custom lights the light list's editor works on, by index: a row is told by its light's position, which the editor changes.
		std::vector<int> list_custom_indices_;
		// Selects the list rows of these custom lights (those that have one: lights switched off or away have none).
		template <typename Indices> void mirror_custom_to_list(const Indices& indices);
		unsigned int custom_generation_ = 0;   // the reading of the file (custom_lights::generation) the section's indices belong to
		unsigned int list_custom_generation_ = 0;   // and the list editor's
		unsigned int rows_custom_generation_ = 0;   // the reading of the file rows_ were made from (custom rows' ids)
		bool custom_owns_list_ = false;        // the list's selection mirrors the section's, until a row of the list is clicked
		// Selects one custom light (-1: none) and drops any multi-selection.
		void select_custom(int index);
		// A click on a custom light's row: plain, Ctrl (toggle) or Shift (range from the anchor).
		void custom_click(int index, int count);
		// Several custom lights at once: a change goes to all of them, and only the setting changed.
		void custom_multi_imgui(const std::vector<int>& picked);
		bool open_custom_ = false;   // open the Custom lights section on the next frame (a custom row's editor)
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
			bool include_map = false;   // a character-only light drawn anyway (row_state::characters)
			float offset[3] = {};       // moved from where the game puts it, metres (a bonfire's light off its flame)
			bool is_default() const
			{
				return !disabled && multiplier == 1.0f && !recolour && !include_map && offset[0] == 0.0f && offset[1] == 0.0f && offset[2] == 0.0f;
			}
		};

		// A row of the list, rebuilt every frame by sync_lights.
		struct light_row
		{
			uint64_t key = 0;
			// The row's own identity (selection, flash, ImGui ids) when it shares its edit key with other
			// rows: every instance of an unlit effect is edited as one. 0 = the key.
			uint64_t id = 0;
			uint64_t ident() const { return id ? id : key; }
			// An effect's lights are edited as one (group_key). own_key is this light's own, by position, which it
			// edits under instead when decoupled. 0: not part of a group.
			uint64_t group_key = 0;
			uint64_t own_key = 0;
			bool decoupled = false;
			light_kind kind = light_kind::unresolved;
			row_state state = row_state::on;
			int slot = -1;
			std::string name;
			std::string group;          // sub-group within its source (a stand-in's category), else empty
			std::string origin;         // where the light comes from, for the Details column
			float pos[3] = {};
			float distance = 0.0f;      // from the player (0 for the sun)
			float lift = 0.0f;          // drawn this far above pos: out of what holds its flame (map_lights::flame_lift)
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
		void sync_menu_key();
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
		// The edit as light_overrides.txt has it (last read or saved), nullptr for none.
		const light_override* find_saved_override(uint64_t key) const;
		float edit_multiplier(uint64_t key, uint64_t ident = 0) const;   // solo / flash / override, 0 = off

		// Settings persistence: [Lights], [Sun], [PlayerLight] in the INI, overrides
		// in rtx_comp\lights\light_overrides.txt, categories through map_lights.
		std::string serialize() const;
		bool save_all();
		void revert_to_saved();
		std::string overrides_path() const;
		bool load_overrides();

		// UI pieces.
		void list_imgui();
		void sources_imgui();
		void own_sun_imgui();
		void scales_imgui();
		void table_imgui();
		void selected_imgui();
		void multi_imgui(const std::vector<const light_row*>& rows, int pinned);
		void solo_flash_imgui(uint64_t key);
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
		// The last frame's player light, for the log of its sudden changes (sync_player_light).
		struct { bool seen = false; int slot = -1; float peak = 0.0f; float pos[3] = {}; int budget = 300; } player_trace_;
		live_light player_light_;
		int player_slot_ = -1;            // KPointLightMan slot currently taken as the player light
		unsigned player_handle_ = 0; // the held light's entity handle: the slot alone is reused by other lights
		bool player_seen_ = false;        // found on the last harvest
		game::point_lights::light player_src_ = {}; // its last harvested values, for the readout
		float player_head_dist_ = 0.0f;   // distance of that light from the head point
		float player_radiance_[3] = {};
		bool player_hash_sent_ = false;   // rtx.ds1Lights.playerLight
		bool player_clearance_refused_ = false; // the runtime refused playerLight / playerAnchor
		float player_anchor_sent_[3] = {}; // rtx.ds1Lights.playerAnchor as last sent

		remixapi_LightHandle menu_key_handle_ = nullptr;
		remixapi_LightInfoSphereEXT menu_key_sphere_ = {};
		remixapi_LightInfoDistantEXT menu_key_distant_ = {};
		bool menu_key_row_ = false;
		remixapi_LightInfo menu_key_info_ = {};
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
		std::unordered_map<uint64_t, light_override> saved_overrides_; // light_overrides.txt as last read or saved
		// Copy / Paste of a light's edits in the list.
		light_override clipboard_;
		bool have_clipboard_ = false;
		game::custom_lights::light custom_clipboard_;
		bool have_custom_clipboard_ = false;
		// Duplicate: a custom light made from a row (its spot, colour and reach).
		void duplicate_as_custom(const light_row& row);
		std::unordered_set<uint64_t> decoupled_;   // own_key of the lights edited apart from their group, saved
		// The guessed effect lights (light_kind::effect) start off.
		bool kind_enabled_[static_cast<int>(light_kind::count)] = { true, true, true, true, true, true, true, false, true, true, true, true };
		// This sync's copies of the game's fire effect lights whose reach the flame size extends (ffx_lights::sized_reach_at).
		std::vector<game::point_lights::light> sized_game_lights_;
		// Kinds lit with the game's falloff (rtx.ds1Lights.falloffTable) instead of inverse square:
		// every kind the game gives a falloff (it shades all its point lights flat to dwindle begin,
		// then linearly to zero at dwindle end). Only the sun, and unresolved slots with no
		// parameters, stay out.
		bool kind_game_falloff_[static_cast<int>(light_kind::count)] = { false, true, true, true, true, true, true, true, true, true, true, false };
		bool kind_game_falloff_initial_[static_cast<int>(light_kind::count)] = {}; // the above at start, for Default
		bool game_falloff_ = true;        // master switch ([Lights] GameFalloff)
		std::string falloff_table_sent_;
		// What each light Remix draws is and where its values come from, for the runtime's DS1 light view (Alt+X > Lights,
		// rtx.ds1Lights.labels): sent when the set changes, at most every few frames.
		void send_labels();
		std::string labels_sent_;
		unsigned int labels_sent_frame_ = 0;
		bool sun_authored_ = false;       // the sun's values come from the map's light banks (sync_sun)
		uint64_t selected_ = KEY_SUN;              // the last row clicked
		std::unordered_set<uint64_t> selection_;   // every selected row (Shift / Ctrl); empty: just selected_
		uint64_t selection_anchor_ = KEY_SUN;      // where a Shift range starts
		std::vector<uint64_t> row_order_, row_order_next_;   // the rows as listed, top to bottom, for a Shift range
		uint64_t solo_ = 0;
		bool flash_selected_ = false;
		bool group_rows_ = true;          // sections by source (stand-ins by category), else one flat list
		// Within a section (or the flat list), by the light list's clicked header: name or distance, either way.
		static constexpr unsigned kSortColumnName = 1, kSortColumnDistance = 2;
		bool sort_by_name_ = false;
		bool sort_descending_ = false;
		char filter_[64] = {};
		std::string saved_text_;
		std::string save_status_;
		// The pending-save list under Save (pending_imgui): every INI setting Save writes with its file value as last read or
		// saved, the decoupled set as saved, and the items ticked out of the next save ("ini:Section.Key", "light:<key>",
		// "decoupled:<key>", "cat:<name>"), which Save leaves as the files have them.
		struct ini_value { const char* section; const char* key; const char* label; std::string value; };
		std::vector<ini_value> ini_values() const;
		std::unordered_map<std::string, std::string> saved_ini_values_;
		std::unordered_set<uint64_t> saved_decoupled_;
		std::unordered_set<std::string> save_skip_;
		void remember_saved();
		void pending_imgui();
		// The tab's height never shrinks while the menu is open: a page scrolled down would otherwise jump under the cursor
		// when a toggle takes rows away.
		float content_height_max_ = 0.0f;
		int content_height_frame_ = -10;
	};
}
