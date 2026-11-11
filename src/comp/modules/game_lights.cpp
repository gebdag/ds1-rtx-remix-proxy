#include "std_include.hpp"
#include "game_lights.hpp"
#include "../game/point_lights.hpp"
#include "../game/map_lights.hpp"
#include "../game/effect_lights.hpp"
#include "../game/ffx_names.hpp"
#include "../game/ffx_lights.hpp"
#include "../game/mesh_lights.hpp"
#include "../game/custom_lights.hpp"
#include "../game/sun_light.hpp"
#include "../game/map_sun.hpp"
#include "../game/atmosphere.hpp"
#include "../game/camera.hpp"
#include "../game/area_overrides.hpp"
#include "shared/common/remix_api.hpp"
#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

using shared::common::log;
using shared::common::LOG_TYPE;
using shared::utils::va;
using shared::utils::string_hash64;

namespace comp
{
	namespace
	{
		// The own sun's reach when Save writes it: the whole map, or a box this far around the player.
		int g_sun_scope_area = 0;
		float g_sun_box_reach = 30.0f;

		using kind = game_lights::light_kind;

		// A slot may be reused by a different light later, so the hash mixes the slot
		// index with a fixed salt rather than being a bare index Remix might collide.
		uint64_t light_hash_for_slot(int slot)
		{
			return string_hash64(va("ds1_point_light_%d", slot));
		}

		// The radiance model below folds the engine's dwindle-end distance in as an
		// inverse-square term, which lands roughly 8x hotter than Remix wants: a stock
		// torch (4 m dwindle end, 200% row) came out around 32 rather than near 1, and
		// the wider map rows are worse still. This normalises the default back down;
		// [Lights] IntensityScale is still the user-facing multiplier on top, so
		// IntensityScale=8 restores the previous brightness exactly.
		constexpr float kRadianceNormalisation = 0.125f;

		// Player light detection. The game hangs it on a head dummy-poly, measured at
		// 1.7 units above the character origin (Sen's Fortress, LerpBank entity).
		// Acquire: the live light nearest that point inside a head-sized box. Hold:
		// keep the same slot while it stays within kHoldRadius of the head point
		// (rolls and attacks swing the head well away from rest).
		constexpr float kHeadHeight = 1.7f;
		constexpr float kAcquireHorizontal = 1.0f;
		constexpr float kAcquireMinUp = 0.5f;
		constexpr float kAcquireMaxUp = 2.8f;
		constexpr float kHoldRadius = 3.0f;

		constexpr float kFlashMultiplier = 20.0f;
		// A follows-effect stand-in below this level is not drawn (its effect has faded out).
		constexpr float kEffectLevelOff = 0.002f;
		constexpr uint64_t kSessionKeyBit = 0x4000000000000000ull;

		// A key that is saved: the top bit keeps it apart from KEY_SUN / KEY_PLAYER, and the session bit must not come from the hash.
		constexpr uint64_t saved_key(uint64_t hash) { return (hash & ~kSessionKeyBit) | 0x8000000000000000ull; }
		// The sun's edits in the list (brightness, on/off, colour) are the current map's: "SUN" | map id. The top two bits clear, so
		// never a row key (top bit set) and saved (no session bit). A sun set to 0.1x in one map starts at 1x in the next.
		uint64_t sun_map_key(unsigned map_id) { return 0x1355'4E00'0000'0000ull | map_id; }
		constexpr float kRadToDeg = 57.2957795f;

		// Short source name per kind (switches, the selected light's heading), in section order.
		const char* const kKindSource[] = {
			"Sun", "Player", "Map", "Map entity", "Character", "Effect", "Stand-in", "Effect stand-in", "Unlit effect",
			"Custom", "Mesh", "No parameters",
		};
		static_assert(std::size(kKindSource) == static_cast<size_t>(kind::count));

		// Section heading per kind in the grouped list.
		const char* const kKindSection[] = {
			"Sun", "Player",
			"Map lights",
			"Map entity lights",
			"Character lights",
			"Effect lights",
			"Placed flame lights",
			"Guessed lights (fire, lightning, crystal lizard)",
			"Added effect lights",
			"Custom lights",
			"Lantern and lamp lights",
			"Not drawn: no light data",
		};
		static_assert(std::size(kKindSection) == static_cast<size_t>(kind::count));

		// State column text and colour.
		const char* state_text(game_lights::row_state s)
		{
			using st = game_lights::row_state;
			switch (s)
			{
			case st::on: return "on";
			case st::rejected: return "Remix refused";
			case st::lights_off: return "all lights off";
			case st::no_params: return "no parameters";
			case st::effect_off: return "effect not shown";
			case st::source_off: return "group off";
			case st::disabled: return "turned off";
			case st::solo_other: return "hidden by solo";
			case st::over_limit: return "over the limit";
			case st::characters: return "characters only";
			}
			return "?";
		}

		ImVec4 state_colour(game_lights::row_state s)
		{
			using st = game_lights::row_state;
			if (s == st::on) return ImVec4(0.4f, 0.9f, 0.4f, 1.0f);
			if (s == st::rejected) return ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
			if (s == st::disabled || s == st::over_limit) return ImVec4(1.0f, 0.75f, 0.3f, 1.0f);
			return ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
		}

		// "torch_small" -> "Torch small".
		std::string pretty_category(const std::string& category)
		{
			std::string s = category;
			std::replace(s.begin(), s.end(), '_', ' ');
			if (!s.empty())
				s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
			return s;
		}

		const char* const kFrameNames[] = { "World", "View", "Character" };

		unsigned short read_u16(unsigned address)
		{
			__try { return *reinterpret_cast<const unsigned short*>(address); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFF; }
		}

		std::string format_float3(const float* v)
		{
			return std::format("{:.3f}, {:.3f}, {:.3f}", v[0], v[1], v[2]);
		}

		// Map lights, bank entities and table lights stay put, so a position identifies
		// them across sessions and slot reuse. Everything else is keyed by its slot.
		bool is_static(kind k)
		{
			return k == kind::map_row || k == kind::bank_entity || k == kind::synthetic || k == kind::custom;
		}

		uint64_t row_key(kind k, const game::point_lights::light& l)
		{
			// Unlit effect lights are keyed by their effect, saved: an edit applies to every instance of it.
			if (k == kind::ffx_table)
				return saved_key(string_hash64(va("ffx:%d", l.ffx_id)));
			const uint64_t h = is_static(k)
				? string_hash64(va("pos:%d:%ld:%ld:%ld", static_cast<int>(k),
					std::lround(l.x * 20.0f), std::lround(l.y * 20.0f), std::lround(l.z * 20.0f)))
				: string_hash64(va("slot:%d:%d", static_cast<int>(k), l.slot));
			// Top bit: never KEY_SUN / KEY_PLAYER. Next bit: a session-only key, which
			// serialize() and save_all() skip.
			return saved_key(h) | (is_static(k) ? 0ull : kSessionKeyBit);
		}

		kind classify(const game::point_lights::light& l)
		{
			using src = game::point_lights::param_source;
			if (l.source == src::synthetic) return kind::synthetic;
			if (l.source == src::effect) return kind::effect;
			if (l.source == src::ffx_table) return kind::ffx_table;
			if (l.source == src::custom) return kind::custom;
			if (l.source == src::mesh) return kind::mesh;
			if (l.source == src::bank_row) return kind::map_row;
			if (l.source == src::entity)
			{
				const std::string_view cls = game::point_lights::entity_class_name(l.entity_vtable);
				if (cls == "Bank") return kind::bank_entity;
				if (cls == "LerpBank") return kind::lerp_entity;
				if (cls == "Direct") return kind::fx_direct;
			}
			return kind::unresolved;
		}

		// Bank / LerpBank entities keep (bank << 8 | row) at +0x44 (LerpBank: A, B).
		std::string bank_row_text(unsigned entity, bool lerp)
		{
			const unsigned short a = read_u16(entity + 0x44);
			if (!lerp)
				return std::format("bank {} row {}", (a >> 8) & 0x7F, a & 0xFF);
			const unsigned short b = read_u16(entity + 0x46);
			return a == b
				? std::format("bank {} row {}", (a >> 8) & 0x7F, a & 0xFF)
				: std::format("bank {} row {} / bank {} row {}", (a >> 8) & 0x7F, a & 0xFF, (b >> 8) & 0x7F, b & 0xFF);
		}

		bool contains_no_case(const std::string& hay, const char* needle)
		{
			if (!needle || !*needle) return true;
			const auto it = std::search(hay.begin(), hay.end(), needle, needle + std::strlen(needle),
				[](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
			return it != hay.end();
		}

		// Player position in the engine's raw (x, up, depth) order, the order slot
		// positions and Remix use. get_char_pos hands back (x, depth, up).
		bool player_raw_pos(float* out)
		{
			float x = 0.0f, depth = 0.0f, up = 0.0f;
			if (!game::camera::get_char_pos(x, depth, up))
				return false;
			out[0] = x; out[1] = up; out[2] = depth;
			return true;
		}

		bool normalise_xz(float* v)
		{
			v[1] = 0.0f;
			const float len = std::sqrt(v[0] * v[0] + v[2] * v[2]);
			if (len < 1e-4f) return false;
			v[0] /= len; v[2] /= len;
			return true;
		}
	}

	game_lights::game_lights()
	{
		p_this = this;
		std::memcpy(kind_game_falloff_initial_, kind_game_falloff_, sizeof(kind_game_falloff_initial_));

		const auto& cfg = shared::common::config::get();
		enabled = cfg.lights.enabled;
		intensity_scale = cfg.lights.intensity_scale;
		fire_light_scale = cfg.lights.fire_light_scale;
		effect_table_scale = cfg.lights.effect_table_scale;
		radius_scale = cfg.lights.radius_scale;
		game_falloff_ = cfg.get_bool("Lights", "GameFalloff", true);
		skip_characters_only_ = cfg.get_bool("Lights", "SkipCharacterOnly", true);
		game::point_lights::set_resolve_entities(cfg.lights.resolve_entity_params);
		game::map_lights::set_enabled(cfg.lights.synthesized_lights);

		player_cfg_ = cfg.player_light;

		sun_enabled = cfg.sun.enabled;
		sun_intensity_scale = cfg.sun.intensity_scale;
		sun_angular_diameter = cfg.sun.angular_diameter;
		std::memcpy(sun_fallback_colour, cfg.sun.fallback_colour, sizeof(sun_fallback_colour));

		load_overrides();
		remember_saved();
	}

	game_lights::~game_lights()
	{
		destroy_all_handles();
		release_light(player_light_);
		release_sun();
		p_this = nullptr;
	}

	// How the stock runtime treats API lights (rtx_remix_api.cpp, LightManager::addExternalLight):
	//  - the handle IS the hash;
	//  - CreateLight on a hash that already exists overwrites that light in place and keeps
	//    its stable identity, so the new values apply on the next frame;
	//  - DestroyLight erases at once, and DestroyLight and CreateLight apply in call order.
	// So a changed light is updated with CreateLight alone; destroying it first is not needed.
	bool game_lights::submit_light(const remixapi_LightInfo& info, remixapi_LightHandle& handle)
	{
		remixapi_LightHandle out = nullptr;
		if (shared::common::remix_api::get().m_bridge.CreateLight(&info, &out) != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			handle = nullptr;
			return false;
		}
		handle = out;
		return true;
	}

	void game_lights::note_destroyed(uint64_t hash)
	{
		destroyed_frame_[hash] = shared::common::ffp_state::get().frame_count();
	}

	bool game_lights::destroyed_this_frame(uint64_t hash) const
	{
		const auto it = destroyed_frame_.find(hash);
		return it != destroyed_frame_.end() && it->second == shared::common::ffp_state::get().frame_count();
	}

	void game_lights::release_sun()
	{
		if (sun_handle_ && remix_api_ready_)
		{
			shared::common::remix_api::get().m_bridge.DestroyLight(sun_handle_);
			note_destroyed(sun_info_.hash);
		}
		sun_handle_ = nullptr;
	}

	void game_lights::release_light(live_light& light)
	{
		if (light.handle && remix_api_ready_)
		{
			shared::common::remix_api::get().m_bridge.DestroyLight(light.handle);
			note_destroyed(light.hash);
		}
		light.handle = nullptr;
	}

	void game_lights::destroy_all_handles()
	{
		for (auto& [slot, light] : live_lights_)
		{
			release_light(light);
		}
		live_lights_.clear();
	}

	bool game_lights::try_init_remix_api()
	{
		if (remix_api_ready_)
			return true;

		if (!shared::common::remix_api::is_initialized())
			return false;

		remix_api_ready_ = true;

		// DrawLightInstance is only honoured inside the bridge's begin_scene callback.
		shared::common::remix_api::get().begin_scene_callback_external = remix_draw_callback;
		log("GameLights", "Remix API ready — harvesting engine point lights", LOG_TYPE::LOG_TYPE_STATUS);
		return true;
	}

	void game_lights::remix_draw_callback()
	{
		if (auto* gl = get())
			gl->draw_light_instances();
	}

	void game_lights::draw_light_instances()
	{
		if (!remix_api_ready_)
			return;

		auto& api = shared::common::remix_api::get();
		if (sun_handle_)
			api.m_bridge.DrawLightInstance(sun_handle_);
		if (menu_key_handle_)
			api.m_bridge.DrawLightInstance(menu_key_handle_);
		if (player_light_.handle)
			api.m_bridge.DrawLightInstance(player_light_.handle);

		if (!enabled)
			return;

		for (auto& [slot, light] : live_lights_)
		{
			if (light.handle)
				api.m_bridge.DrawLightInstance(light.handle);
		}
	}

	// ------------------------------------------------------------------ per-light edits

	const game_lights::light_override* game_lights::find_saved_override(uint64_t key) const
	{
		const auto it = saved_overrides_.find(key);
		return it == saved_overrides_.end() ? nullptr : &it->second;
	}

	const game_lights::light_override* game_lights::find_override(uint64_t key) const
	{
		const auto it = overrides_.find(key);
		return it == overrides_.end() ? nullptr : &it->second;
	}

	// Brightness factor of a light from the list's edits: 0 = not drawn.
	float game_lights::edit_multiplier(uint64_t key, uint64_t ident) const
	{
		if (solo_ != 0 && solo_ != key)
			return 0.0f;
		float m = 1.0f;
		if (const auto* o = find_override(key))
		{
			if (o->disabled)
				return 0.0f;
			m = o->multiplier;
		}
		if (flash_selected_ && ((ident ? ident : key) == selected_ || selection_.count(ident ? ident : key)))
			m *= kFlashMultiplier;
		return m;
	}

	void game_lights::observe_lightmapped_draw(const float* ps_const, int light_count)
	{
		float ox = 0.0f, oy = 0.0f, oz = 0.0f;
		if (!ps_const || !shared::common::ffp_state::get().world_offset(ox, oy, oz))
			return;
		// Per map, like the other bindings: over a session the list filled up with lights from elsewhere and later lights stayed 3.14x dim.
		if (const unsigned map = game::map_lights::current_map_id(); map != lightmap_bound_map_id_)
		{
			lightmap_bound_.clear();
			lightmap_bound_map_id_ = map;
		}
		for (int i = 0; i < std::min(light_count, 4); i++)
		{
			const float* colour = &ps_const[(116 + i) * 4];
			if (colour[0] <= 0.0f && colour[1] <= 0.0f && colour[2] <= 0.0f)
				continue;
			const float* pos = &ps_const[(112 + i) * 4];
			const std::array<float, 3> world = { pos[0] + ox, pos[1] + oy, pos[2] + oz };
			if (!bound_to_lightmap(world[0], world[1], world[2]) && lightmap_bound_.size() < 1024)
				lightmap_bound_.push_back(world);
		}
	}

	void game_lights::observe_draw_lights(const float* ps_const, int light_count, bool character)
	{
		float ox = 0.0f, oy = 0.0f, oz = 0.0f;
		if (!ps_const || !shared::common::ffp_state::get().world_offset(ox, oy, oz))
			return;
		// Map lights stay put, so a light's bindings are kept for the map and dropped when it changes.
		if (const unsigned map = game::map_lights::current_map_id(); map != bound_map_id_)
		{
			map_bound_.clear();
			character_bound_.clear();
			bound_map_id_ = map;
		}
		auto& bound = character ? character_bound_ : map_bound_;
		for (int i = 0; i < std::min(light_count, 4); i++)
		{
			const float* colour = &ps_const[(116 + i) * 4];
			if (colour[0] <= 0.0f && colour[1] <= 0.0f && colour[2] <= 0.0f)
				continue;
			const float* pos = &ps_const[(112 + i) * 4];
			const std::array<float, 3> world = { pos[0] + ox, pos[1] + oy, pos[2] + oz };
			constexpr float kSame = 0.1f;
			const bool known = std::any_of(bound.begin(), bound.end(), [&](const std::array<float, 3>& p)
				{ return (p[0] - world[0]) * (p[0] - world[0]) + (p[1] - world[1]) * (p[1] - world[1]) + (p[2] - world[2]) * (p[2] - world[2]) < kSame * kSame; });
			if (!known && bound.size() < 1024)
				bound.push_back(world);
		}
	}

	bool game_lights::turned_off_as_character_only(const game::point_lights::light& l) const
	{
		const kind k = classify(l);
		if (!skip_characters_only_ || (k != kind::map_row && k != kind::bank_entity) || !characters_only(l.x, l.y, l.z) ||
			game::map_lights::is_flame(l.x, l.y, l.z))
			return false;
		// One the user forced on shines.
		const auto* edit = find_override(row_key(k, l));
		return !(edit && edit->include_map);
	}

	bool game_lights::characters_only(float x, float y, float z) const
	{
		constexpr float kSame = 0.1f;
		auto in = [&](const std::vector<std::array<float, 3>>& list)
		{
			return std::any_of(list.begin(), list.end(), [&](const std::array<float, 3>& p)
				{ return (p[0] - x) * (p[0] - x) + (p[1] - y) * (p[1] - y) + (p[2] - z) * (p[2] - z) < kSame * kSame; });
		};
		return in(character_bound_) && !in(map_bound_);
	}

	bool game_lights::bound_to_lightmap(float x, float y, float z) const
	{
		// The game's copy of a light's position and the harvested one agree to float precision; a light
		// that moves (a map light never does) only has to match where it was seen.
		constexpr float kSame = 0.1f;
		return std::any_of(lightmap_bound_.begin(), lightmap_bound_.end(), [&](const std::array<float, 3>& p)
			{ return (p[0] - x) * (p[0] - x) + (p[1] - y) * (p[1] - y) + (p[2] - z) * (p[2] - z) < kSame * kSame; });
	}

	void game_lights::dump_rows() const
	{
		FILE* f = nullptr;
		if (fopen_s(&f, "rtx_comp\\light_rows.txt", "a") || !f)
			return;
		fprintf(f, "=== rows %zu\n", rows_.size());
		for (const auto& r : rows_)
			fprintf(f, "key %016llx kind %d state %d slot %d %s | pos %.2f %.2f %.2f dist %.1f | colour %.3f %.3f %.3f range %.2f-%.2f | radiance %.3f %.3f %.3f %s\n",
				static_cast<unsigned long long>(r.key), static_cast<int>(r.kind), static_cast<int>(r.state), r.slot, r.name.c_str(), r.pos[0], r.pos[1], r.pos[2], r.distance,
				r.colour[0], r.colour[1], r.colour[2], r.range_begin, r.range_end, r.radiance[0], r.radiance[1], r.radiance[2], r.origin.c_str());
		for (const auto& p : map_bound_)
			fprintf(f, "map-bound %.2f %.2f %.2f\n", p[0], p[1], p[2]);
		for (const auto& p : character_bound_)
			fprintf(f, "character-bound %.2f %.2f %.2f\n", p[0], p[1], p[2]);
		fclose(f);
	}

	game_lights::light_row game_lights::make_row(const game::point_lights::light& l, const float* player) const
	{
		light_row row;
		row.kind = classify(l);
		row.key = row_key(row.kind, l);
		if (row.kind == kind::ffx_table)
			row.id = string_hash64(va("ffx_slot:%d", l.slot)) | 0x8000000000000000ull | kSessionKeyBit;
		// The lights of one effect are one group: an edit applies to all of them and is saved. A game light's own
		// FFX is mostly a shared light node (2101), so the effect that called it is what tells them apart.
		const int group_effect = row.kind == kind::ffx_table ? l.ffx_id
			: row.kind == kind::fx_direct ? (l.ffx_top_id ? l.ffx_top_id : l.ffx_id) : 0;
		if (group_effect)
		{
			if (row.kind == kind::fx_direct)
			{
				row.id = row.key;
				row.key = saved_key(string_hash64(va("ffx_group:%d", group_effect)));
			}
			row.group_key = row.key;
			row.own_key = saved_key(string_hash64(va("ffx_own:%d:%ld:%ld:%ld", group_effect,
				std::lround(l.x * 2.0f), std::lround(l.y * 2.0f), std::lround(l.z * 2.0f))));
			if (decoupled_.count(row.own_key))
			{
				row.key = row.own_key;
				row.decoupled = true;
			}
		}
		// The lamps of one texture are one group the same way.
		if (row.kind == kind::mesh)
		{
			const auto texture = game::mesh_lights::texture_of(l.slot);
			row.id = row.key;
			row.key = saved_key(string_hash64(va("mesh_group:%s", texture.c_str())));
			row.group_key = row.key;
			row.own_key = saved_key(string_hash64(va("mesh_own:%s:%ld:%ld:%ld", texture.c_str(),
				std::lround(l.x * 2.0f), std::lround(l.y * 2.0f), std::lround(l.z * 2.0f))));
			if (decoupled_.count(row.own_key))
			{
				row.key = row.own_key;
				row.decoupled = true;
			}
		}
		row.slot = l.slot;
		row.pos[0] = l.x; row.pos[1] = l.y; row.pos[2] = l.z;
		if (player)
		{
			const float dx = l.x - player[0], dy = l.y - player[1], dz = l.z - player[2];
			row.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
		}
		row.colour[0] = l.r; row.colour[1] = l.g; row.colour[2] = l.b;
		row.range_begin = l.dwindle_begin;
		row.range_end = l.dwindle_end;

		switch (row.kind)
		{
		case kind::map_row:
			row.name = std::format("Map light {}", l.slot);
			row.origin = std::format("engine slot {}, colour from its MSB Light event's POINT_LIGHT_BANK row", l.slot);
			break;
		case kind::bank_entity:
			row.name = std::format("Map light {} ({})", l.slot, bank_row_text(l.entity, false));
			row.origin = std::format("engine slot {}, BankPointLightEntity", l.slot);
			break;
		case kind::lerp_entity:
			row.name = std::format("Character light {} ({})", l.slot, bank_row_text(l.entity, true));
			row.origin = std::format("engine slot {}, LerpBankPointLightEntity (blends two rows)", l.slot);
			break;
		case kind::fx_direct:
		{
			// The effect that made the light (fx_owner), named from the effect catalogue.
			const char* effect = l.ffx_id ? game::ffx_names::name(l.ffx_id) : nullptr;
			const char* caller = l.ffx_top_id && l.ffx_top_id != l.ffx_id ? game::ffx_names::name(l.ffx_top_id) : nullptr;
			if (!l.ffx_id)
				row.name = std::format("Effect light {}", l.slot);
			else
				row.name = std::format("{} (FFX {})", caller ? caller : effect ? effect : "Effect light", l.ffx_id);
			row.origin = std::format("engine slot {}, DirectPointLightEntity (colour set by the effect)", l.slot);
			if (l.ffx_id)
				row.origin += std::format("; light node of FFX {}{}", l.ffx_id,
					l.ffx_top_id && l.ffx_top_id != l.ffx_id ? std::format(", called by FFX {}", l.ffx_top_id) : std::string());
			if (group_effect)
			{
				const char* group_name = game::ffx_names::name(group_effect);
				row.group = std::format("{} (FFX {})", group_name ? group_name : "Effect light", group_effect);
			}
			break;
		}
		case kind::synthetic:
		{
			std::string name, category;
			if (game::map_lights::describe(l.slot, name, category))
			{
				row.group = pretty_category(category);
				row.name = row.group + " " + name;
				row.origin = std::format("{}.txt, category '{}'", game::map_lights::current_map_name(), category);
			}
			else
			{
				row.name = "Stand-in";
				row.origin = game::map_lights::current_map_name() + ".txt";
			}
			break;
		}
		case kind::effect:
		{
			const bool particles = l.entity_vtable == game::effect_lights::kParticleMarker;
			row.name = particles ? "Fire particles" : "Lightning bolts";
			row.origin = particles
				? "synthesized from a cluster of fire-coloured additive particles the game lights with no light of its own"
				: "synthesized from the breath's bolt draws (s00502 / s00504): the game lights only its first flash";
			break;
		}
		case kind::custom:
		{
			const auto& custom = game::custom_lights::edit();
			const size_t index = static_cast<size_t>(l.slot - 0x40000);
			row.name = index < custom.size() ? custom[index].name : std::format("Custom light {}", l.slot);
			row.origin = game::custom_lights::path() + (l.spot_cone_deg > 0.0f ? std::format(", spot {:.0f} deg", l.spot_cone_deg) : "");
			break;
		}
		case kind::mesh:
			row.group = game::mesh_lights::texture_of(l.slot);
			row.name = std::format("{} lamp {}", row.group, l.slot - game::mesh_lights::kFirstSlot + 1);
			row.origin = game::mesh_lights::path();
			break;
		case kind::ffx_table:
		{
			const char* effect = game::ffx_names::name(l.ffx_id);
			row.name = std::format("{} (FFX {})", effect ? effect : "Effect", l.ffx_id);
			row.origin = std::format("ffx_lights.tsv: the developers' effect-light rule on FFX {}", l.ffx_id);
			if (const auto* e = game::ffx_lights::find(l.ffx_id))
			{
				row.group = std::format("{}: {} (FFX {})", pretty_category(e->category), effect ? effect : "Effect", l.ffx_id);
				row.origin += std::format(" ({} confidence{}{})", e->confidence == 'h' ? "high" : e->confidence == 'm' ? "medium" : "low",
					e->planar ? ", planar sprite" : "", e->persistent ? ", persistent" : "");
			}
			if (l.ffx_top_id && l.ffx_top_id != l.ffx_id)
				row.origin += std::format(", called by FFX {}", l.ffx_top_id);
			break;
		}
		default:
			row.name = std::format("Engine slot {}", l.slot);
			row.origin = l.entity
				? std::format("entity 0x{:08X} of an unknown class gave no colour or range", l.entity)
				: l.entity_handle
					? std::format("handle 0x{:08X} did not resolve to a light entity", l.entity_handle)
					: std::string("no light entity and no POINT_LIGHT_BANK row");
			break;
		}
		return row;
	}

	// ------------------------------------------------------------------ sync

	void game_lights::sync_lights(bool generic)
	{
		const UINT frame = shared::common::ffp_state::get().frame_count();
		float player_pos[3] = {};
		const bool have_player = player_raw_pos(player_pos);

		// Engine lights first, then the synthesized stand-ins for maps the engine never
		// lights. They share a shape and a slot-id space, so from here on they are
		// treated identically.
		const auto& harvested = game::point_lights::harvest();

		// The player light is taken out before anything else sees it: it has its own
		// settings, and none of the [Lights] scales below apply to it.
		const auto* player = find_player_light(harvested);
		std::string falloff_table;
		sync_player_light(player, falloff_table);

		const auto& synthetic = game::map_lights::get();
		synthetic_count_ = static_cast<int>(synthetic.size());
		game::map_lights::update_effect_gate();
		game::effect_lights::set_fire_scale(fire_light_scale);

		std::vector<const game::point_lights::light*> sources;
		sources.reserve(harvested.size() + synthetic.size() + 32);
		for (const auto& l : harvested)
			if (&l != player) sources.push_back(&l);
		for (const auto& l : synthetic) sources.push_back(&l);
		// The particle stand-ins these lights already stand for are dropped inside effect_lights, before
		// its light cap (they took slots there and pushed visible stand-ins out and back in), and with
		// hysteresis on the distance and on the game light's presence (its own fire lights flicker).
		// A map light the character-only rule turns off lights nothing, so it must not stand in for a flame either: the flame ended with no light at all.
		std::vector<const game::point_lights::light*> lit_sources;
		lit_sources.reserve(sources.size());
		for (const auto* s : sources)
			if (!turned_off_as_character_only(*s))
				lit_sources.push_back(s);
		game::effect_lights::set_covers(lit_sources);
		const auto& effects = game::effect_lights::collect();
		static unsigned particle_log_frame = 0;
		const bool particle_log = frame - particle_log_frame >= 300;
		if (particle_log)
			particle_log_frame = frame;
		for (const auto& l : effects)
		{
			if (l.entity_vtable != game::effect_lights::kParticleMarker)
			{
				sources.push_back(&l);
				continue;
			}
			const game::point_lights::light* nearest = nullptr;
			float nearest_d2 = std::numeric_limits<float>::max();
			for (const auto* s : sources)
			{
				const float d2 = (s->x - l.x) * (s->x - l.x) + (s->y - l.y) * (s->y - l.y) + (s->z - l.z) * (s->z - l.z);
				if (s->source != game::point_lights::param_source::effect && d2 < nearest_d2)
				{
					nearest = s;
					nearest_d2 = d2;
				}
			}
			// The calibration data: the particles' summed light next to the game's own light for them.
			if (particle_log)
			{
				shared::common::log("EffectLights", nearest
					? std::format("particles at ({:.1f}, {:.1f}, {:.1f}): light ({:.3f}, {:.3f}, {:.3f}) range {:.1f}; nearest game light {:.1f} m away: ({:.3f}, {:.3f}, {:.3f}) dwindle {:.1f}-{:.1f}",
						l.x, l.y, l.z, l.r, l.g, l.b, l.dwindle_end, std::sqrt(nearest_d2), nearest->r, nearest->g, nearest->b, nearest->dwindle_begin, nearest->dwindle_end)
					: std::format("particles at ({:.1f}, {:.1f}, {:.1f}): light ({:.3f}, {:.3f}, {:.3f}) range {:.1f}; no game light",
						l.x, l.y, l.z, l.r, l.g, l.b, l.dwindle_end));
			}
			sources.push_back(&l);
		}
		if (kind_enabled_[static_cast<int>(kind::custom)])
			for (const auto& l : game::custom_lights::get())
				sources.push_back(&l);
		if (kind_enabled_[static_cast<int>(kind::mesh)])
			for (const auto& l : game::mesh_lights::get())
				sources.push_back(&l);
		if (kind_enabled_[static_cast<int>(kind::ffx_table)])
		{
			std::vector<const game::point_lights::light*> map_lights;
			for (const auto* s : sources)
			{
				const kind k = classify(*s);
				if ((k == kind::map_row || k == kind::bank_entity || k == kind::synthetic) && s->has_params && !turned_off_as_character_only(*s))
					map_lights.push_back(s);
			}
			for (auto& l : game::ffx_lights::collect(map_lights, effect_table_scale))
				sources.push_back(&l);
		}

		harvested_count_ = static_cast<int>(sources.size());
		create_failed_ = 0;
		zero_radiance_ = 0;

		rows_.clear();
		rows_.reserve(sources.size());

		int active = 0;
		for (const auto* src_ptr : sources)
		{
			const auto& src = *src_ptr;
			light_row row = make_row(src, have_player ? player_pos : nullptr);

			// Stand-ins of an effect that comes and goes shine as strongly as it is drawn.
			const float effect = row.kind == kind::synthetic ? game::map_lights::effect_level(src.slot) : 1.0f;
			const float mult = edit_multiplier(row.key, row.ident()) * effect;
			const auto* edit = find_override(row.key);
			row.state = !generic ? row_state::lights_off
				: !src.has_params ? row_state::no_params
				: effect < kEffectLevelOff ? row_state::effect_off
				: !kind_enabled_[static_cast<int>(row.kind)] ? row_state::source_off
				: edit && edit->disabled ? row_state::disabled
				: mult <= 0.0f ? row_state::solo_other
				: max_active_lights_ > 0 && active >= max_active_lights_ ? row_state::over_limit
				: skip_characters_only_ && !(edit && edit->include_map) &&
					(row.kind == kind::map_row || row.kind == kind::bank_entity) &&
					characters_only(src.x, src.y, src.z) && !game::map_lights::is_flame(src.x, src.y, src.z) ? row_state::characters
				: row_state::on;
			if (row.state != row_state::on)
			{
				rows_.push_back(std::move(row));
				continue;
			}
			++active;

			// The green light seen when the player is hit (not every time): every green-dominant light of an effect that
			// comes on within 5 m of the player is logged once, with what made it, so the effect can be named and fixed.
			if (row.kind != kind::custom && row.kind != kind::mesh && row.kind != kind::sun && row.kind != kind::player &&
				row.distance > 0.0f && row.distance < 5.0f && row.colour[1] > 1.2f * row.colour[0] && row.colour[1] > 0.9f * row.colour[2])
			{
				// By slot and effect: a slot reused by another effect is logged again.
				static std::unordered_set<uint64_t> logged_green;
				if (logged_green.size() < 200 && logged_green.insert(row.ident() ^ (static_cast<uint64_t>(static_cast<uint32_t>(src.ffx_id)) << 32)).second)
					shared::common::log("GreenLight", std::format("{} '{}' ffx {} slot {} colour ({:.3f}, {:.3f}, {:.3f}) reach {:.1f}-{:.1f} m, {:.1f} m from the player at ({:.1f}, {:.1f}, {:.1f}) frame {}",
						kKindSource[static_cast<int>(row.kind)], row.name, src.ffx_id, src.slot, row.colour[0], row.colour[1], row.colour[2],
						row.range_begin, row.range_end, row.distance, src.x, src.y, src.z, shared::common::ffp_state::get().frame_count()));
			}

			// Remix takes radiance and an emitter size rather than a falloff distance, so
			// the engine's dwindle end sets how much energy the light needs to still read
			// at that range (inverse-square). The emitter is the flame, emitter_radius_:
			// dwindle begin is where the game's linear falloff starts (0.5 for a map
			// torch), and a sphere that size around a wall-mounted flame sits half inside
			// the wall and lights it as a bright disc. The light's intensity (radiance x
			// emitter area) stays what it was with dwindle begin as the emitter.
			const float range = std::max(0.01f, src.dwindle_end * radius_scale);
			// A custom light set up as a Remix light keeps its own sphere, cone focus and volumetric scale.
			const bool physical = row.kind == kind::custom && src.physical;
			const float emitter = physical ? src.emitter_radius : emitter_radius_;
			const float focus = physical && src.spot_cone_deg > 0.0f ? src.spot_focus : 0.0f;
			const float volumetric = physical ? src.volumetric_scale : 1.0f;
			const auto* o = find_override(row.key);
			const float* colour = o && o->recolour ? o->colour : row.colour;
			remixapi_Float3D radiance;
			// Custom lights choose the game's falloff per light; the other sources by the switches.
			const bool game_falloff = row.kind == kind::custom ? !physical
				: game_falloff_ && kind_game_falloff_[static_cast<int>(row.kind)];
			if (physical)
			{
				// Remix's own units: the radiance the light was given (colour x intensity x 2^exposure), only
				// the list's per-light edits (brightness, solo) on top.
				radiance = { colour[0] * mult, colour[1] * mult, colour[2] * mult };
			}
			else if (game_falloff)
			{
				// The game's falloff (rtx.ds1Lights.falloffTable): Remix lights the surface with irradiance
				// pi * radiance * att(d)^2.2, and a diffuse surface returns albedo / pi of that, so
				// radiance = colour^2.2 gives albedo * (colour * att)^2.2, the game's albedo * light in
				// linear space, at every distance (as the sun and the ambient sky are matched). Divided by
				// pi as well, these lights reached a third of the game's (Remix's demodulated direct diffuse
				// next to a hollow's torch, frame_20261030_092329: 0.19-0.23 of the game's).
				// A map light the game never binds to a lightmapped draw keeps that third: its glow on the
				// lightmapped map is baked into the lightmap, and Remix's light stands in for it (at the game's
				// own strength it lit Painted World's lightmapped ground twice as bright as the game, frame mean
				// 98 vs 49; torches1's torches likewise reach no lightmapped draw). The game picks a draw's
				// lights per draw (the first four intersecting ones from the lists its category mask allows), and
				// some map lights do reach lightmapped draws (Parish nave lights, msg_scene's torches): those
				// are dynamic there and take the game's strength (observe_lightmapped_draw).
				// Character lights (the player's head light among them, harvested as a LerpBank entity) keep
				// the third: the game's strength was only measured for effect lights, and the head light at it
				// lit Painted World's close-up ground 1.4x the game's (frame_20261030_093046).
				const bool exact = row.kind == kind::fx_direct || row.kind == kind::effect || row.kind == kind::ffx_table ||
					row.kind == kind::custom || row.kind == kind::mesh ||
					(is_static(row.kind) && bound_to_lightmap(src.x, src.y, src.z));
				const float scale = intensity_scale * mult / (exact ? 1.0f : 3.14159265f);
				radiance = {
					std::pow(std::max(colour[0], 0.0f), 2.2f) * scale,
					std::pow(std::max(colour[1], 0.0f), 2.2f) * scale,
					std::pow(std::max(colour[2], 0.0f), 2.2f) * scale,
				};
				falloff_table += std::format("{:x}:{:.4g}:{:.4g};", light_hash_for_slot(src.slot),
					std::max(0.0f, src.dwindle_begin) * radius_scale, range);
			}
			else
			{
				const float reference = src.dwindle_begin > 0.01f ? src.dwindle_begin * radius_scale : emitter;
				const float falloff = range * range * kRadianceNormalisation * (reference * reference) / (emitter * emitter);
				const float scale = intensity_scale * falloff * mult;
				radiance = { colour[0] * scale, colour[1] * scale, colour[2] * scale };
			}
			// The list's own offset (Move) on top: shared by every light of a group, as its other edits are.
			const float* moved = o ? o->offset : nullptr;
			const remixapi_Float3D position = {
				src.x + manual_offset_[0] + (moved ? moved[0] : 0.0f),
				src.y + manual_offset_[1] + (moved ? moved[1] : 0.0f),
				src.z + manual_offset_[2] + (moved ? moved[2] : 0.0f),
			};

			if (radiance.x <= 0.0f && radiance.y <= 0.0f && radiance.z <= 0.0f)
				++zero_radiance_;

			auto& light = live_lights_[src.slot];
			light.last_seen_frame = frame;
			const bool unchanged = light.handle &&
				light.sphere.position.x == position.x &&
				light.sphere.position.y == position.y &&
				light.sphere.position.z == position.z &&
				light.sphere.radius == std::max(0.001f, emitter) &&
				light.sphere.volumetricRadianceScale == volumetric &&
				light.sphere.shaping_value.focusExponent == focus &&
				light.info.radiance.x == radiance.x &&
				light.info.radiance.y == radiance.y &&
				light.info.radiance.z == radiance.z &&
				light.sphere.shaping_value.coneAngleDegrees == (src.spot_cone_deg > 0.0f ? src.spot_cone_deg : 0.0f) &&
				light.sphere.shaping_value.coneSoftness == (src.spot_cone_deg > 0.0f ? src.spot_softness : 0.0f) &&
				light.sphere.shaping_value.direction.x == (src.spot_cone_deg > 0.0f ? src.spot_dir[0] : 0.0f) &&
				light.sphere.shaping_value.direction.y == (src.spot_cone_deg > 0.0f ? src.spot_dir[1] : 0.0f) &&
				light.sphere.shaping_value.direction.z == (src.spot_cone_deg > 0.0f ? src.spot_dir[2] : 0.0f);

			if (!unchanged)
			{
				// Changed or new: (re)submit under the slot's hash, which updates an
				// existing light in place (see submit_light).
				light.hash = light_hash_for_slot(src.slot);
				light.sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
				light.sphere.pNext = nullptr;
				light.sphere.position = position;
				light.sphere.radius = std::max(0.001f, emitter);
				// A spotlight (custom lights): its cone, and for a Remix light its focus exponent.
				light.sphere.shaping_hasvalue = src.spot_cone_deg > 0.0f ? TRUE : FALSE;
				light.sphere.shaping_value = {};
				if (light.sphere.shaping_hasvalue)
				{
					light.sphere.shaping_value.direction = { src.spot_dir[0], src.spot_dir[1], src.spot_dir[2] };
					light.sphere.shaping_value.coneAngleDegrees = src.spot_cone_deg;
					light.sphere.shaping_value.coneSoftness = src.spot_softness;
					light.sphere.shaping_value.focusExponent = focus;
				}
				light.sphere.volumetricRadianceScale = volumetric;

				light.info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
				light.info.pNext = &light.sphere;
				light.info.hash = light.hash;
				light.info.radiance = radiance;

				if (!submit_light(light.info, light.handle) && !destroyed_this_frame(light.hash))
					++create_failed_;
			}

			row.drawn = light.handle != nullptr;
			if (!row.drawn)
				row.state = row_state::rejected;
			row.radiance[0] = radiance.x; row.radiance[1] = radiance.y; row.radiance[2] = radiance.z;
			rows_.push_back(std::move(row));
		}

		// The lights with the game's falloff, when the set or a range changes (once: a runtime
		// without the option refuses it, and asking again every frame would not change that).
		if (falloff_table != falloff_table_sent_)
		{
			auto& bridge = shared::common::remix_api::get().m_bridge;
			const bool ok = bridge.SetConfigVariable &&
				bridge.SetConfigVariable("rtx.ds1Lights.falloffTable", falloff_table.c_str()) == REMIXAPI_ERROR_CODE_SUCCESS;
			const auto count = std::count(falloff_table.begin(), falloff_table.end(), ';');
			const bool has_player = falloff_table.find(std::format("{:x}:", string_hash64("ds1_player_light"))) != std::string::npos;
			// FX lights can change their range every frame: log only what the set looks like.
			static long long s_logged_count = -1;
			static bool s_logged_player = false, s_logged_ok = true;
			if (count != s_logged_count || has_player != s_logged_player || ok != s_logged_ok)
			{
				shared::common::log("GameLights", std::format("game falloff for {} light(s){}{}", count,
					has_player ? ", the player light among them" : "", ok ? "" : " (Remix refused the table)"));
				s_logged_count = count;
				s_logged_player = has_player;
				s_logged_ok = ok;
			}
			falloff_table_sent_ = falloff_table;
		}

		// Drop lights not drawn this frame (slot went dark, filtered, disabled),
		// otherwise Remix keeps rendering them — rtx.antiCulling.light extends light
		// lifetime aggressively.
		for (auto it = live_lights_.begin(); it != live_lights_.end();)
		{
			if (it->second.last_seen_frame != frame)
			{
				release_light(it->second);
				it = live_lights_.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	// The game's key light travels along c92 (its shadow light along c175) and the
	// HemDir3 shaders light with max(0, -dot(c92, N)) * c95 * albedo. A Remix distant
	// light's direction is also the direction of travel, and its lambert response is
	// albedo * radiance * cos.
	//
	// The game adds that light to the ambient in gamma space (its shaders multiply the
	// texture's stored, gamma-encoded colour), Remix in linear space. On a surface
	// facing the sun the game shows albedo * (ambient + sun); Remix shows the same
	// when its sun adds (ambient + sun)^2.2 - ambient^2.2 to the linear ambient^2.2
	// the bounce-ray sky gives (game::atmosphere sends that cube).
	void game_lights::sync_menu_key()
	{
		const bool lit = menu_key[3] > 0.0f || menu_key[4] > 0.0f || menu_key[5] > 0.0f;
		const bool row = shared::common::ffp_state::get().menu_row_active();
		if (!shared::common::ffp_state::get().menu_is_path_traced() || !lit || (menu_key_handle_ && row != menu_key_row_))
		{
			if (menu_key_handle_)
			{
				shared::common::remix_api::get().m_bridge.DestroyLight(menu_key_handle_);
				note_destroyed(menu_key_info_.hash);
				menu_key_handle_ = nullptr;
			}
			return;
		}
		menu_key_row_ = row;
		// The same light on a character (taken at 1 m up) from a 5 degree distant light: every one of a row gets it alike.
		// A distant light's radiance scales a facing surface's albedo directly (as the sun's does); the sphere's is r^2 / d^2 of it.
		if (row)
		{
			const float dx = -menu_key[0], dy = 1.0f - menu_key[1], dz = -menu_key[2];
			const float d2 = std::max(dx * dx + dy * dy + dz * dz, 1e-3f), d = std::sqrt(d2);
			const float scale = menu_key[6] * menu_key[6] / d2;
			const remixapi_Float3D direction = { dx / d, dy / d, dz / d };
			const remixapi_Float3D row_radiance = { menu_key[3] * scale, menu_key[4] * scale, menu_key[5] * scale };
			if (menu_key_handle_ && !std::memcmp(&menu_key_distant_.direction, &direction, sizeof(direction)) &&
				!std::memcmp(&menu_key_info_.radiance, &row_radiance, sizeof(row_radiance)))
				return;
			menu_key_distant_ = {};
			menu_key_distant_.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
			menu_key_distant_.direction = direction;
			menu_key_distant_.angularDiameterDegrees = 5.0f;
			menu_key_info_ = {};
			menu_key_info_.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
			menu_key_info_.pNext = &menu_key_distant_;
			menu_key_info_.hash = string_hash64("ds1_menu_key_row");
			menu_key_info_.radiance = row_radiance;
			submit_light(menu_key_info_, menu_key_handle_);
			return;
		}
		const remixapi_Float3D position = { menu_key[0], menu_key[1], menu_key[2] };
		const remixapi_Float3D radiance = { menu_key[3], menu_key[4], menu_key[5] };
		if (menu_key_handle_ && !std::memcmp(&menu_key_sphere_.position, &position, sizeof(position)) &&
			!std::memcmp(&menu_key_info_.radiance, &radiance, sizeof(radiance)) && menu_key_sphere_.radius == menu_key[6])
			return;
		menu_key_sphere_ = {};
		menu_key_sphere_.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
		menu_key_sphere_.position = position;
		menu_key_sphere_.radius = std::max(0.001f, menu_key[6]);
		menu_key_sphere_.volumetricRadianceScale = 0.0f;
		menu_key_info_ = {};
		menu_key_info_.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
		menu_key_info_.pNext = &menu_key_sphere_;
		menu_key_info_.hash = string_hash64("ds1_menu_key");
		menu_key_info_.radiance = radiance;
		submit_light(menu_key_info_, menu_key_handle_);
	}

	void game_lights::sync_sun()
	{
		const auto& sun = game::sun_light::current(shared::common::ffp_state::get().frame_count(),
			sun_fallback_colour);

		// One sun, the map's: it stays where it is and as it is wherever the player stands (game/map_sun.hpp). A map without a named row goes by what the
		// game draws with, keeping the strongest it has seen. [Sun] Colour and Direction in an area override either.
		const unsigned sun_map = game::map_lights::current_map_id();
		const auto& authored = game::map_sun::get(sun_map);
		if (sun_map != seen_sun_map_)
		{
			// A colour and direction set in the panel but not saved belong to the map they were set in.
			if (seen_sun_map_ != 0)
				read_own_sun();
			// Solo on the last map's sun would leave every light dark here.
			if (solo_ == sun_map_key(seen_sun_map_))
				solo_ = 0;
			seen_sun_map_ = sun_map;
			seen_sun_ = {};
			seen_sun_frames_ = 0;
			seen_map_light_ = false;
		}
		// Not in the first moments of a map, while its draws can still carry the last one's values.
		constexpr int kSeenSunAfterFrames = 60;
		if (seen_sun_frames_ < kSeenSunAfterFrames)
			seen_sun_frames_++;
		float ambient[3] = {};
		// Only a colour the game draws with now: otherwise current() hands over [Sun] FallbackColour or the last map's colour, and a map whose banks
		// have no sun row (Firelink, whose game sun is black) kept that white as its sun.
		const bool colour_measured = sun.colour_from != game::sun_light::colour_source::fallback &&
			sun.colour_from != game::sun_light::colour_source::last_seen;
		// A map whose banks have no sun row follows the game's sun as it is drawn now, as every map did before the banks were read. Kept at the
		// strongest ever seen (adb0e30), anything that showed for a moment stayed for good: the probe's lobe or a character's key light became
		// Firelink's white sun, where the game's own is black. Once the map's own geometry has shown its key light (map c95), only that counts:
		// in views where its vote falls short the probe's or a character's would take over for those frames and the sun would come and go.
		// The last value counted holds until the next.
		if (!authored.authored)
		{
			const bool from_map = sun.colour_from == game::sun_light::colour_source::map_c95;
			if (!sun.valid)
				seen_sun_.has_sun = false;
			else if (colour_measured && seen_sun_frames_ >= kSeenSunAfterFrames && (from_map || !seen_map_light_))
			{
				seen_map_light_ = seen_map_light_ || from_map;
				const float facing[3] = { -sun.key.dir[0], -sun.key.dir[1], -sun.key.dir[2] };
				seen_sun_.has_sun = true;
				std::memcpy(seen_sun_.direction, sun.key.dir, sizeof(seen_sun_.direction));
				std::memcpy(seen_sun_.lobe, sun.key.colour, sizeof(seen_sun_.lobe));
				game::atmosphere::ambient_toward(facing, seen_sun_.base);
			}
		}
		const auto& map_sun = authored.authored ? authored : seen_sun_;
		sun_authored_ = authored.authored;
		// Not at the title or in a menu's character view, which have their own light.
		const bool in_a_map = sun_map != 0 && ((sun_map >> 24) & 0xFF) < 99 && !shared::common::ffp_state::get().menu_is_path_traced();
		const bool sun_wanted = sun_enabled && in_a_map && (map_sun.has_sun || sun_colour_set);
		game::sun_light::set_world_direction(sun_wanted ? (sun_direction_set ? sun_direction : map_sun.direction) : nullptr);
		const float* key_direction = sun_direction_set ? sun_direction : map_sun.direction;
		const float* key_colour = sun_colour_set ? sun_colour : map_sun.lobe;
		if (map_sun.has_sun && !sun_colour_set)
			std::memcpy(ambient, map_sun.base, sizeof(ambient));
		else
		{
			const float facing_sun[3] = { -key_direction[0], -key_direction[1], -key_direction[2] };
			game::atmosphere::ambient_toward(facing_sun, ambient);
		}
		game::atmosphere::set_sun_gamma(ambient, key_colour);
		float linear[3];
		for (int c = 0; c < 3; ++c)
		{
			const float base = std::max(ambient[c], 0.0f);
			linear[c] = std::pow(base + std::max(key_colour[c], 0.0f), 2.2f) - std::pow(base, 2.2f);
		}

		const float mult = sun_intensity_scale * edit_multiplier(sun_map_key(sun_map), KEY_SUN);
		remixapi_Float3D radiance = {
			linear[0] * mult,
			linear[1] * mult,
			linear[2] * mult,
		};
		const bool lit = radiance.x > 0.0f || radiance.y > 0.0f || radiance.z > 0.0f;

		if (!sun_wanted || !lit)
		{
			release_sun();
			return;
		}

		const remixapi_Float3D direction = { key_direction[0], key_direction[1], key_direction[2] };
		const bool unchanged = sun_handle_ &&
			sun_distant_.direction.x == direction.x &&
			sun_distant_.direction.y == direction.y &&
			sun_distant_.direction.z == direction.z &&
			sun_distant_.angularDiameterDegrees == sun_angular_diameter &&
			sun_info_.radiance.x == radiance.x &&
			sun_info_.radiance.y == radiance.y &&
			sun_info_.radiance.z == radiance.z;
		if (unchanged)
			return;

		// Changed or new: submit under the same hash, updating the light in place.
		sun_distant_.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
		sun_distant_.pNext = nullptr;
		sun_distant_.direction = direction;
		sun_distant_.angularDiameterDegrees = sun_angular_diameter;
		sun_distant_.volumetricRadianceScale = 1.0f;

		sun_info_.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
		sun_info_.pNext = &sun_distant_;
		sun_info_.hash = string_hash64("ds1_sun");
		sun_info_.radiance = radiance;

		if (!submit_light(sun_info_, sun_handle_) && !destroyed_this_frame(sun_info_.hash))
			++sun_create_failed_;
	}

	const game::point_lights::light* game_lights::find_player_light(
		const std::vector<game::point_lights::light>& harvested)
	{
		player_seen_ = false;

		float p[3] = {};
		if (!player_raw_pos(p))
		{
			player_slot_ = -1;
			return nullptr;
		}
		const float head[3] = { p[0], p[1] + kHeadHeight, p[2] };
		auto head_dist = [&](const game::point_lights::light& l)
		{
			const float dx = l.x - head[0], dy = l.y - head[1], dz = l.z - head[2];
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		};

		const game::point_lights::light* found = nullptr;
		if (player_slot_ >= 0)
		{
			for (const auto& l : harvested)
				if (l.slot == player_slot_ && head_dist(l) < kHoldRadius)
					found = &l;
		}

		if (!found)
		{
			float best = FLT_MAX;
			for (const auto& l : harvested)
			{
				const float dx = l.x - p[0], dz = l.z - p[2];
				const float up = l.y - p[1];
				if (std::sqrt(dx * dx + dz * dz) > kAcquireHorizontal ||
					up < kAcquireMinUp || up > kAcquireMaxUp)
					continue;
				const float d = head_dist(l);
				if (d < best)
				{
					best = d;
					found = &l;
				}
			}
		}

		player_slot_ = found ? found->slot : -1;
		if (found)
		{
			player_seen_ = true;
			player_src_ = *found;
			player_head_dist_ = head_dist(*found);
		}
		return found;
	}

	// Offset in world space. World frame: the offset as given. View / Character: the
	// offset is (right, up, forward) against the camera or body heading, levelled so
	// the light turns with the heading but never tilts with the camera's pitch.
	bool game_lights::player_offset_world(float* out) const
	{
		const auto& c = player_cfg_;
		out[0] = c.offset[0]; out[1] = c.offset[1]; out[2] = c.offset[2];
		if (c.offset_frame == 0)
			return true;

		float cam_right[3] = {}, cam_fwd[3] = {};
		const bool have_cam = shared::common::ffp_state::get().view_axes(cam_right, cam_fwd) &&
			normalise_xz(cam_right) && normalise_xz(cam_fwd);

		float fwd[3] = {}, right[3] = {};
		if (c.offset_frame == 1)
		{
			if (!have_cam) return false;
			std::memcpy(fwd, cam_fwd, sizeof(fwd));
			std::memcpy(right, cam_right, sizeof(right));
		}
		else
		{
			float yaw = 0.0f;
			if (!game::camera::get_char_heading(yaw)) return false;
			fwd[0] = std::sin(yaw); fwd[1] = 0.0f; fwd[2] = std::cos(yaw);
			// Right = up x forward, with the handedness the camera's own axes have.
			right[0] = fwd[2]; right[1] = 0.0f; right[2] = -fwd[0];
			if (have_cam)
			{
				const float cam_cross[3] = { cam_fwd[2], 0.0f, -cam_fwd[0] };
				if (cam_cross[0] * cam_right[0] + cam_cross[2] * cam_right[2] < 0.0f)
				{
					right[0] = -right[0]; right[2] = -right[2];
				}
			}
		}

		for (int i = 0; i < 3; ++i)
			out[i] = right[i] * c.offset[0] + fwd[i] * c.offset[2];
		out[1] += c.offset[1];
		return true;
	}

	void game_lights::sync_player_light(const game::point_lights::light* src, std::string& falloff_table)
	{
		const auto& c = player_cfg_;
		const float mult = edit_multiplier(KEY_PLAYER);
		// A short log of what goes wrong with this light in play (the first few hundred events of a session).
		if (!src && c.enabled && player_trace_.seen && player_trace_.budget > 0)
		{
			--player_trace_.budget;
			log("PlayerLight", std::format("frame {}: no player light this frame (last slot {})", shared::common::ffp_state::get().frame_count(), player_trace_.slot));
			player_trace_.seen = false;
		}
		if (!c.enabled || !src || mult <= 0.0f)
		{
			release_light(player_light_);
			player_radiance_[0] = player_radiance_[1] = player_radiance_[2] = 0.0f;
			return;
		}

		// Each property follows the game unless overridden; without resolved game
		// parameters (Resolve Entity Params off) the configured values stand in.
		const bool game = src->has_params;
		const float colour[3] = {
			c.use_game_colour && game ? src->r : c.colour[0],
			c.use_game_colour && game ? src->g : c.colour[1],
			c.use_game_colour && game ? src->b : c.colour[2],
		};
		const bool game_falloff = game_falloff_ && kind_game_falloff_[static_cast<int>(light_kind::player)];
		const float range = std::max(0.01f, c.use_game_range && game ? src->dwindle_end : c.range);
		// With the game's falloff the dwindle begin is where the falloff starts (up to 6 in some
		// areas), not an emitter size: a sphere that big sits in the walls around the player.
		const float radius = std::max(0.01f,
			!game_falloff && c.use_game_radius && game && src->dwindle_begin > 0.01f ? src->dwindle_begin : c.radius);

		remixapi_Float3D radiance;
		if (game_falloff)
		{
			// The game's falloff, as for the character lights: radiance colour^2.2 / pi, flat to the
			// game's dwindle begin and linear to zero at its end, both times FalloffRangeScale,
			// with its own intensity, FalloffIntensity (1 = the game's brightness). Begin and end
			// come from the same light: the area's player light ranges from 0..2.5 to 6..25, and a
			// fixed end under the game's begin cut the light off in a hard ring (begin 6 against
			// an end of 4.2 left 4 cm of falloff). Range and Intensity are the inverse-square
			// light's (0.02 was about right there).
			const float game_end = std::max(0.01f, (game ? src->dwindle_end : c.range) * c.falloff_range_scale);
			const float game_begin = game
				? std::min(std::max(0.0f, src->dwindle_begin) * c.falloff_range_scale, game_end * 0.99f) : 0.0f;
			// The Skull Lantern makes the game's colour 4, three times the usual head light's, and in the path tracer's dark
			// surroundings that blows out. A limit per area (area_overrides.ini) takes the lantern down and leaves the usual one alone.
			const float peak = std::max({ colour[0], colour[1], colour[2], 1e-4f });
			const float limit = c.falloff_max_colour > 0.0f ? std::min(c.falloff_max_colour / peak, 1.0f) : 1.0f;
			const float scale = c.falloff_intensity * mult / 3.14159265f;
			radiance = {
				std::pow(std::max(colour[0] * limit, 0.0f), 2.2f) * scale,
				std::pow(std::max(colour[1] * limit, 0.0f), 2.2f) * scale,
				std::pow(std::max(colour[2] * limit, 0.0f), 2.2f) * scale,
			};
			// The game adds this light to the surface's ambient in gamma, before the albedo: (ambient + colour * att)^2.2 on
			// screen. Alone and linearised, a strong tint keeps all of its saturation: Lost Izalith's (1.2, 2, 2) came out
			// teal on the player, where the game's stays close to white on top of its ambient of about (1.25, 1.2, 1.03).
			// What it adds to that ambient gives its hue, at the same luminance, so this does not change its brightness.
			const float side[3] = { 1.0f, 0.0f, 0.0f };
			if (float base[3]; c.falloff_gamma_add && game::atmosphere::ambient_toward(side, base))
			{
				constexpr float kAtt = 0.7f;   // about the middle of the falloff, where it meets the body and the ground nearby
				constexpr float kLuma[3] = { 0.2126f, 0.7152f, 0.0722f };
				float added[3], plain = 0.0f, shaped = 0.0f;
				for (int i = 0; i < 3; i++)
				{
					const float lit = std::max(colour[i] * limit, 0.0f) * kAtt;
					const float floor = std::max(base[i], 0.0f);
					added[i] = std::pow(floor + lit, 2.2f) - std::pow(floor, 2.2f);
					plain += kLuma[i] * std::pow(lit, 2.2f);
					shaped += kLuma[i] * added[i];
				}
				if (shaped > 1e-5f)
				{
					const float keep = plain / shaped / std::pow(kAtt, 2.2f) * scale;
					radiance = { added[0] * keep, added[1] * keep, added[2] * keep };
				}
			}
			falloff_table += std::format("{:x}:{:.4g}:{:.4g};", string_hash64("ds1_player_light"), game_begin, game_end);
		}
		else
		{
			// Same radiance model as the other harvested lights, so the defaults keep the
			// brightness the light had before it was split out.
			const float falloff = range * range * kRadianceNormalisation * c.intensity * mult;
			radiance = { colour[0] * falloff, colour[1] * falloff, colour[2] * falloff };
		}
		player_radiance_[0] = radiance.x; player_radiance_[1] = radiance.y; player_radiance_[2] = radiance.z;

		float offset[3] = {};
		if (!player_offset_world(offset))
			std::memcpy(offset, c.offset, sizeof(offset)); // no heading yet: world axes
		const remixapi_Float3D position = { src->x + offset[0], src->y + offset[1], src->z + offset[2] };

		// The head the offset hangs from: the runtime pulls the light back towards it when the offset
		// would put it behind a wall (rtx.ds1Lights.playerLightClearance).
		// Asked once: a runtime without the options refuses them, and asking again every frame
		// would not change that.
		auto& bridge = shared::common::remix_api::get().m_bridge;
		if (bridge.SetConfigVariable && !player_clearance_refused_)
		{
			if (!player_hash_sent_)
			{
				player_hash_sent_ = true;
				player_clearance_refused_ = bridge.SetConfigVariable("rtx.ds1Lights.playerLight",
					std::format("{:x}", string_hash64("ds1_player_light")).c_str()) != REMIXAPI_ERROR_CODE_SUCCESS;
			}
			const float anchor[3] = { src->x, src->y, src->z };
			if (!player_clearance_refused_ && std::memcmp(anchor, player_anchor_sent_, sizeof(anchor)) != 0)
			{
				std::memcpy(player_anchor_sent_, anchor, sizeof(anchor));
				player_clearance_refused_ = bridge.SetConfigVariable("rtx.ds1Lights.playerAnchor",
					std::format("{}, {}, {}", anchor[0], anchor[1], anchor[2]).c_str()) != REMIXAPI_ERROR_CODE_SUCCESS;
			}
			if (player_clearance_refused_)
				shared::common::log("GameLights", "Remix refused the player light's wall clearance options (older runtime)");
		}

		{
			auto& t = player_trace_;
			const float peak = std::max({ radiance.x, radiance.y, radiance.z });
			const float dx = position.x - t.pos[0], dy = position.y - t.pos[1], dz = position.z - t.pos[2];
			const float moved = std::sqrt(dx * dx + dy * dy + dz * dz);
			const bool jump = t.seen && (src->slot != t.slot || moved > 0.75f || peak > t.peak * 1.4f || peak < t.peak * 0.7f);
			if (jump && t.budget > 0)
			{
				--t.budget;
				log("PlayerLight", std::format("frame {}: slot {} -> {}, moved {:.2f} m, peak radiance {:.3f} -> {:.3f}, colour ({:.3f}, {:.3f}, {:.3f}), dwindle {:.2f}-{:.2f}, params {}",
					shared::common::ffp_state::get().frame_count(), t.slot, src->slot, moved, t.peak, peak, src->r, src->g, src->b, src->dwindle_begin, src->dwindle_end, src->has_params));
			}
			t.seen = true;
			t.slot = src->slot;
			t.peak = peak;
			t.pos[0] = position.x; t.pos[1] = position.y; t.pos[2] = position.z;
		}

		auto& light = player_light_;
		const bool unchanged = light.handle &&
			light.sphere.position.x == position.x &&
			light.sphere.position.y == position.y &&
			light.sphere.position.z == position.z &&
			light.sphere.radius == radius &&
			light.info.radiance.x == radiance.x &&
			light.info.radiance.y == radiance.y &&
			light.info.radiance.z == radiance.z;
		if (unchanged)
			return;

		light.hash = string_hash64("ds1_player_light");
		light.sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
		light.sphere.pNext = nullptr;
		light.sphere.position = position;
		light.sphere.radius = radius;
		light.sphere.shaping_hasvalue = FALSE;
		light.sphere.shaping_value = {};
		light.sphere.volumetricRadianceScale = 1.0f;

		light.info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
		light.info.pNext = &light.sphere;
		light.info.hash = light.hash;
		light.info.radiance = radiance;

		submit_light(light.info, light.handle);
	}

	void game_lights::frame_update()
	{
		if (!enabled && !live_lights_.empty())
			destroy_all_handles();

		if (!try_init_remix_api())
			return;

		sync_sun();
		sync_menu_key();

		if (probe_light_mode_ && enabled)
		{
			auto& light = live_lights_[-1];
			light.last_seen_frame = shared::common::ffp_state::get().frame_count();

			light.sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
			light.sphere.pNext = nullptr;
			light.sphere.position = { probe_position_[0], probe_position_[1], probe_position_[2] };
			light.sphere.radius = probe_radius_;
			light.sphere.shaping_hasvalue = FALSE;
			light.sphere.volumetricRadianceScale = 1.0f;

			light.info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
			light.info.pNext = &light.sphere;
			light.hash = string_hash64("ds1_probe_light");
			light.info.hash = light.hash;
			light.info.radiance = { probe_radiance_, probe_radiance_, probe_radiance_ };

			submit_light(light.info, light.handle);
			send_labels();
			return;
		}

		// With the point lights off the list still fills, and the player light,
		// which has its own toggle, still runs.
		sync_lights(enabled);
		send_labels();
		log_look_watch();
	}

	void game_lights::log_look_watch()
	{
		static int budget = 400;
		static unsigned int last_logged = 0;
		static float last[9] = {};
		if (budget <= 0)
			return;
		const unsigned int map = game::map_lights::current_map_id();
		if (map == 0 || ((map >> 24) & 0xFF) >= 99)
			return;
		const float up_dir[3] = { 0.0f, 1.0f, 0.0f }, down_dir[3] = { 0.0f, -1.0f, 0.0f };
		float up[3] = {}, down[3] = {};
		game::atmosphere::ambient_toward(up_dir, up);
		game::atmosphere::ambient_toward(down_dir, down);
		// The strongest live light by radiance times the area it starts from: what can light a big surface at once.
		int strongest = 0;
		float power = 0.0f, radiance = 0.0f, radius = 0.0f, where[3] = {};
		for (const auto& [key, light] : live_lights_)
		{
			const float r = std::max({ light.info.radiance.x, light.info.radiance.y, light.info.radiance.z });
			const float p = r * light.sphere.radius * light.sphere.radius;
			if (p > power)
			{
				power = p, strongest = key, radiance = r, radius = light.sphere.radius;
				where[0] = light.sphere.position.x, where[1] = light.sphere.position.y, where[2] = light.sphere.position.z;
			}
		}
		const float sun = sun_handle_ ? std::max({ sun_info_.radiance.x, sun_info_.radiance.y, sun_info_.radiance.z }) : 0.0f;
		const float now[9] = { up[0], up[1], up[2], down[0], down[1], down[2], sun, power, static_cast<float>(live_lights_.size()) };
		bool jump = false;
		for (int i = 0; i < 9; i++)
		{
			const float bigger = std::max(std::abs(now[i]), std::abs(last[i]));
			const float floor = i < 6 ? 0.02f : i == 6 ? 0.002f : i == 7 ? 0.02f : 12.0f;
			jump = jump || (bigger > floor && std::abs(now[i] - last[i]) > 0.15f * bigger);
		}
		const unsigned int frame = shared::common::ffp_state::get().frame_count();
		if (!jump || frame - last_logged < 2)
			return;
		last_logged = frame;
		budget--;
		std::memcpy(last, now, sizeof(last));
		shared::common::log("LookWatch", std::format("frame {} up {:.3f} {:.3f} {:.3f} down {:.3f} {:.3f} {:.3f} sun {:.4f} lights {} strongest key {} radiance {:.2f} radius {:.2f} at {:.1f} {:.1f} {:.1f}",
			frame, up[0], up[1], up[2], down[0], down[1], down[2], sun, live_lights_.size(), strongest, radiance, radius, where[0], where[1], where[2]));
	}

	void game_lights::send_labels()
	{
		// One entry per light Remix draws: hash | provenance | kind | name | origin;
		// Provenance, for the runtime's colours: G the game's own light (read live), D derived from the game's data by the
		// developers' rule (the effect table), T a table of ours from game placements (map flames, lamp meshes), H placed or
		// set by hand, X a guess from what is drawn (particle stand-ins), M the proxy's own (menu key, debug probe).
		auto clean = [](std::string s)
		{
			for (auto& ch : s)
				if (ch == '|' || ch == ';' || ch == '\n' || ch == '\r')
					ch = ',';
			if (s.size() > 160)
				s.resize(160);
			return s;
		};
		auto provenance = [](light_kind k)
		{
			switch (k)
			{
			case light_kind::map_row: case light_kind::bank_entity: case light_kind::lerp_entity: case light_kind::fx_direct:
			case light_kind::unresolved: case light_kind::player: case light_kind::sun: return 'G';
			case light_kind::ffx_table: return 'D';
			case light_kind::synthetic: case light_kind::mesh: return 'T';
			case light_kind::custom: return 'H';
			case light_kind::effect: return 'X';
			default: return 'M';
			}
		};
		// Built only when it could go out: at most every 10 frames (the last change still goes out by then).
		const unsigned int frame = shared::common::ffp_state::get().frame_count();
		if (frame - labels_sent_frame_ < 10)
			return;
		std::string labels;
		for (const auto& row : rows_)
			if (row.drawn && row.slot >= 0)
				labels += std::format("{:x}|{}|{}|{}|{};", light_hash_for_slot(row.slot), provenance(row.kind),
					kKindSource[static_cast<int>(row.kind)], clean(row.name), clean(row.origin));
		if (sun_handle_)
		{
			// Where its colour and direction come from, as sync_sun picks them.
			const char* from_game = sun_authored_ ? "the map's light banks" : "the game's draws (light 0, c92 / c95)";
			const std::string origin = std::format("colour from {}, direction from {}",
				sun_colour_set ? "[Sun] Colour (area_overrides.ini or the ini)" : from_game,
				sun_direction_set ? "[Sun] Direction (area_overrides.ini or the ini)" : from_game);
			labels += std::format("{:x}|{}|Sun|Sun|{};", string_hash64("ds1_sun"), sun_colour_set && sun_direction_set ? 'H' : 'G', clean(origin));
		}
		if (player_light_.handle)
			labels += std::format("{:x}|G|Player|Player light|{};", string_hash64("ds1_player_light"),
				clean("the game's head light (its character light bank row), at the proxy's [PlayerLight] offset and range"));
		if (menu_key_handle_)
			labels += std::format("{:x}|M|Menu|Menu key light|{};", menu_key_info_.hash, clean("the character menu's key light"));
		if (const auto probe = live_lights_.find(-1); probe != live_lights_.end() && probe->second.handle)
			labels += std::format("{:x}|M|Probe|Probe light|{};", probe->second.hash, clean("the debug probe light (F4)"));

		// Sent when the set changes: stand-ins come and go with their effects.
		if (labels == labels_sent_)
			return;
		auto& bridge = shared::common::remix_api::get().m_bridge;
		if (bridge.SetConfigVariable)
			bridge.SetConfigVariable("rtx.ds1Lights.labels", labels.c_str());
		labels_sent_ = std::move(labels);
		labels_sent_frame_ = frame;
	}

	// ------------------------------------------------------------------ persistence

	std::string game_lights::overrides_path() const
	{
		const auto& ini = shared::common::config::get().ini_path();
		const auto dir = ini.empty() ? std::filesystem::path(".") : std::filesystem::path(ini).parent_path();
		return (dir / "rtx_comp" / "lights" / "light_overrides.txt").string();
	}

	// "key disabled multiplier recolour r g b include_map  name", one static light per line (files from before
	// include_map have the name right after the colour).
	namespace
	{
		// What the INI holds, for the Saved buttons: read now and then, not on every frame the menu is open.
		std::unordered_map<std::string, std::string> s_saved_ini;
		int s_saved_ini_frame = -1000;

		void forget_saved_ini() { s_saved_ini.clear(); }

		std::string saved_ini(const char* section, const char* key)
		{
			const int frame = ImGui::GetFrameCount();
			if (frame - s_saved_ini_frame >= 30 || frame < s_saved_ini_frame)
			{
				s_saved_ini.clear();
				s_saved_ini_frame = frame;
			}
			const std::string name = std::string(section) + "." + key;
			const auto it = s_saved_ini.find(name);
			if (it != s_saved_ini.end())
				return it->second;
			return s_saved_ini[name] = shared::common::config::get().get_string(section, key, "");
		}

		float saved_float(const char* section, const char* key, float fallback)
		{
			const auto text = saved_ini(section, key);
			try { return text.empty() ? fallback : std::stof(text); }
			catch (...) { return fallback; }
		}

		bool saved_bool(const char* section, const char* key, bool fallback)
		{
			const auto text = saved_ini(section, key);
			return text.empty() ? fallback : std::atoi(text.c_str()) != 0;
		}

		void saved_float3(const char* section, const char* key, const float* fallback, float* out)
		{
			float v[3] = {};
			const auto text = saved_ini(section, key);
			const bool ok = std::sscanf(text.c_str(), "%f , %f , %f", &v[0], &v[1], &v[2]) == 3;
			for (int c = 0; c < 3; ++c)
				out[c] = ok ? v[c] : fallback[c];
		}
	}

	bool game_lights::load_overrides()
	{
		overrides_.clear();
		saved_overrides_.clear();
		decoupled_.clear();
		saved_decoupled_.clear();
		std::ifstream file(overrides_path());
		if (!file)
			return false;

		std::string line;
		while (std::getline(file, line))
		{
			if (line.empty() || line[0] == '#')
				continue;
			if (line.rfind("decoupled ", 0) == 0)
			{
				decoupled_.insert(std::stoull(line.substr(10), nullptr, 16) & ~kSessionKeyBit);
				continue;
			}
			std::istringstream in(line);
			std::string key_text;
			light_override o;
			int disabled = 0, recolour = 0;
			if (!(in >> key_text >> disabled >> o.multiplier >> recolour >> o.colour[0] >> o.colour[1] >> o.colour[2]))
				continue;
			o.disabled = disabled != 0;
			o.recolour = recolour != 0;
			std::string include;
			o.include_map = (in >> include) && include == "1";
			// "move x y z" after include_map (files from before it have the name there).
			std::string word;
			if ((in >> word) && word == "move" && !(in >> o.offset[0] >> o.offset[1] >> o.offset[2]))
				o.offset[0] = o.offset[1] = o.offset[2] = 0.0f;
			// Only saved keys are ever written. Older files hold some with the session bit set by their hash alone.
			overrides_[std::stoull(key_text, nullptr, 16) & ~kSessionKeyBit] = o;
		}
		saved_overrides_ = overrides_;
		saved_decoupled_ = decoupled_;
		return true;
	}

	// Everything Save writes, as text: "unsaved changes" is this differing from
	// what was last saved or loaded. Session-only overrides (moving lights) are left
	// out, since they are never written.
	std::string game_lights::serialize() const
	{
		const auto& p = player_cfg_;
		std::string s = std::format(
			"lights {} {:.3f} {:.3f} {} {}\n"
			"sun {} {:.3f} {:.3f} {}\n"
			"player {} {} {} {} {:.3f} {} {:.3f} {:.3f} {:.3f} {:.3f} {} {}\n",
			enabled, intensity_scale, radius_scale, game::point_lights::resolve_entities(), game::map_lights::enabled(),
			sun_enabled, sun_intensity_scale, sun_angular_diameter, format_float3(sun_fallback_colour),
			p.enabled, p.use_game_colour, format_float3(p.colour), p.use_game_range, p.range, p.use_game_radius,
			p.radius, p.intensity, p.falloff_intensity, p.falloff_range_scale, p.offset_frame, format_float3(p.offset));

		std::vector<std::pair<uint64_t, const light_override*>> saved;
		for (const auto& [key, o] : overrides_)
			if (!o.is_default() && (key & kSessionKeyBit) == 0)
				saved.emplace_back(key, &o);
		std::sort(saved.begin(), saved.end());
		for (const auto& [key, o] : saved)
			s += std::format("{:016x} {} {:.3f} {} {} {} move {:.3f} {:.3f} {:.3f}\n", key, o->disabled ? 1 : 0, o->multiplier,
				o->recolour ? 1 : 0, format_float3(o->colour), o->include_map ? 1 : 0, o->offset[0], o->offset[1], o->offset[2]);
		std::vector<uint64_t> decoupled(decoupled_.begin(), decoupled_.end());
		std::sort(decoupled.begin(), decoupled.end());
		for (const uint64_t key : decoupled)
			s += std::format("decoupled {:016x}\n", key);
		return s;
	}

	// Every INI setting Save writes, as it would write it now.
	std::vector<game_lights::ini_value> game_lights::ini_values() const
	{
		auto flag = [](bool v) { return std::string(v ? "1" : "0"); };
		auto num = [](float v) { return std::format("{:.3f}", v); };
		const auto& p = player_cfg_;
		return {
			{ "Lights", "Enabled", "Point lights on", flag(enabled) },
			{ "Lights", "IntensityScale", "Point light brightness", num(intensity_scale) },
			{ "Lights", "FireLightScale", "Fire light brightness", num(fire_light_scale) },
			{ "Lights", "EffectTableScale", "Effect light brightness", num(effect_table_scale) },
			{ "Lights", "RadiusScale", "Point light reach", num(radius_scale) },
			{ "Lights", "GameFalloff", "The game's falloff", flag(game_falloff_) },
			{ "Lights", "SkipCharacterOnly", "Character-only lights start off", flag(skip_characters_only_) },
			{ "Lights", "ResolveEntityParams", "Read entity lights' parameters", flag(game::point_lights::resolve_entities()) },
			{ "Lights", "SynthesizedLights", "Stand-in lights", flag(game::map_lights::enabled()) },
			{ "Sun", "Enabled", "Sun on", flag(sun_enabled) },
			{ "Sun", "IntensityScale", "Sun brightness", num(sun_intensity_scale) },
			{ "Sun", "AngularDiameter", "Sun size", num(sun_angular_diameter) },
			{ "Sun", "FallbackColour", "Sun colour when the game has none", format_float3(sun_fallback_colour) },
			{ "PlayerLight", "Enabled", "Player light on", flag(p.enabled) },
			{ "PlayerLight", "UseGameColour", "Player light: the game's colour", flag(p.use_game_colour) },
			{ "PlayerLight", "Colour", "Player light colour", format_float3(p.colour) },
			{ "PlayerLight", "UseGameRange", "Player light: the game's reach", flag(p.use_game_range) },
			{ "PlayerLight", "Range", "Player light reach", num(p.range) },
			{ "PlayerLight", "UseGameRadius", "Player light: the game's emitter size", flag(p.use_game_radius) },
			{ "PlayerLight", "Radius", "Player light emitter size", num(p.radius) },
			{ "PlayerLight", "Intensity", "Player light brightness", num(p.intensity) },
			{ "PlayerLight", "FalloffIntensity", "Player light brightness (game falloff)", num(p.falloff_intensity) },
			{ "PlayerLight", "FalloffGammaAdd", "Player light mixed as the game mixes it", flag(p.falloff_gamma_add) },
			{ "PlayerLight", "FalloffRangeScale", "Player light reach (game falloff)", num(p.falloff_range_scale) },
			{ "PlayerLight", "OffsetFrame", "Player light offset frame", kFrameNames[std::clamp(p.offset_frame, 0, 2)] },
			{ "PlayerLight", "Offset", "Player light offset", format_float3(p.offset) },
		};
	}

	// What the files hold now, for the pending-save list and "unsaved changes".
	void game_lights::remember_saved()
	{
		saved_text_ = serialize();
		saved_ini_values_.clear();
		for (const auto& v : ini_values())
			saved_ini_values_[std::string(v.section) + "." + v.key] = v.value;
		save_skip_.clear();
	}

	// Writes everything that differs from the files, except what is ticked out in the list under Save: that stays as the
	// files have it (and stays listed, unsaved).
	bool game_lights::save_all()
	{
		auto& cfg = shared::common::config::get();
		bool ok = true;

		for (const auto& v : ini_values())
		{
			const std::string name = std::string(v.section) + "." + v.key;
			if (save_skip_.count("ini:" + name))
				continue;
			ok &= cfg.set_string(v.section, v.key, v.value);
			saved_ini_values_[name] = v.value;
		}

		// Per-light edits: the file's, with every light not ticked out taking its current edit.
		auto skipped = [&](const char* prefix, uint64_t key) { return save_skip_.count(std::format("{}:{:016x}", prefix, key)) != 0; };
		std::map<uint64_t, light_override> written(saved_overrides_.begin(), saved_overrides_.end());
		for (const auto& [key, o] : overrides_)
			if ((key & kSessionKeyBit) == 0 && !skipped("light", key))
			{
				if (o.is_default()) written.erase(key);
				else written[key] = o;
			}
		for (const auto& [key, o] : saved_overrides_)
			if (!overrides_.count(key) && !skipped("light", key))
				written.erase(key);
		std::unordered_set<uint64_t> written_decoupled = saved_decoupled_;
		for (const uint64_t key : decoupled_)
			if (!skipped("decoupled", key)) written_decoupled.insert(key);
		for (const uint64_t key : saved_decoupled_)
			if (!decoupled_.count(key) && !skipped("decoupled", key)) written_decoupled.erase(key);

		// Overrides of static lights, with the row name as a comment for hand editing.
		{
			const auto path = overrides_path();
			std::error_code ec;
			std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
			std::ofstream file(path, std::ios::trunc);
			if (file)
			{
				file << "# Per-light edits from F4 -> Lights. key disabled multiplier recolour r g b include_map move dx dy dz  name\n";
				for (const auto& [key, o] : written)
				{
					std::string name;
					for (const auto& r : rows_)
						if (r.key == key) name = r.name;
					file << std::format("{:016x} {} {:.3f} {} {:.3f} {:.3f} {:.3f} {} move {:.3f} {:.3f} {:.3f}  {}\n", key, o.disabled ? 1 : 0,
						o.multiplier, o.recolour ? 1 : 0, o.colour[0], o.colour[1], o.colour[2], o.include_map ? 1 : 0,
						o.offset[0], o.offset[1], o.offset[2], name);
				}
				std::vector<uint64_t> decoupled(written_decoupled.begin(), written_decoupled.end());
				std::sort(decoupled.begin(), decoupled.end());
				for (const uint64_t key : decoupled)
					file << std::format("decoupled {:016x}\n", key);
			}
			else
			{
				ok = false;
			}
		}

		// Categories ticked out are written as saved, then given back their current values.
		struct held { size_t index; game::map_lights::category current; };
		std::vector<held> holds;
		const auto& categories = game::map_lights::categories();
		for (size_t i = 0; i < categories.size(); ++i)
		{
			const auto* saved = game::map_lights::saved_category(categories[i].name);
			if (saved && save_skip_.count("cat:" + categories[i].name))
			{
				holds.push_back({ i, categories[i] });
				game::map_lights::set_category(i, saved->intensity, saved->radius, saved->enabled, saved->follows_effect);
			}
		}
		ok &= game::map_lights::save_categories();
		for (const auto& h : holds)
			game::map_lights::set_category(h.index, h.current.intensity, h.current.radius, h.current.enabled, h.current.follows_effect);

		// This map's own sun, for the map or for a box around the player, or none.
		{
			auto close3 = [](const float* x, const float* y)
			{ return std::fabs(x[0] - y[0]) <= 5e-4f && std::fabs(x[1] - y[1]) <= 5e-4f && std::fabs(x[2] - y[2]) <= 5e-4f; };
			const auto map_name = game::map_lights::current_map_name();
			float saved_colour[3] = {}, saved_direction[3] = {};
			bool saved_box = false;
			const bool have_saved = game::area_overrides::saved_sun(map_name, saved_colour, saved_direction, saved_box);
			const bool own = sun_colour_set && sun_direction_set;
			const bool differs = own != have_saved ||
				(own && (!close3(sun_colour, saved_colour) || !close3(sun_direction, saved_direction) || saved_box != (g_sun_scope_area != 0)));
			if (!map_name.empty() && differs && !save_skip_.count("sun:" + map_name))
			{
				if (!own)
					ok &= game::area_overrides::remove_sun(map_name);
				else
				{
					float x = 0.0f, depth = 0.0f, up = 0.0f;
					const bool have_position = game::camera::get_char_pos(x, depth, up) && !(x == 0.0f && depth == 0.0f && up == 0.0f);
					const float r = g_sun_box_reach;
					const float box_min[3] = { x - r, up - r, depth - r }, box_max[3] = { x + r, up + r, depth + r };
					const bool box = g_sun_scope_area != 0 && have_position;
					ok &= game::area_overrides::save_sun(map_name, box ? box_min : nullptr, box ? box_max : nullptr, sun_colour, sun_direction);
				}
			}
			if (game::custom_lights::dirty() && !save_skip_.count("custom:" + map_name))
				ok &= game::custom_lights::save();
		}

		saved_overrides_.clear();
		for (const auto& [key, o] : written)
			saved_overrides_[key] = o;
		saved_decoupled_ = written_decoupled;
		forget_saved_ini();
		saved_text_ = serialize();
		save_status_ = ok ? (save_skip_.empty() ? "saved" : "saved (ticked-out items kept as they were)") : "save FAILED (see console)";
		return ok;
	}

	void game_lights::revert_to_saved()
	{
		forget_saved_ini();
		read_settings();
		load_overrides();
		solo_ = 0;
		flash_selected_ = false;
		remember_saved();
		save_status_ = "reverted to saved";
	}

	void game_lights::reread_settings()
	{
		read_settings();
		remember_saved();
	}

	void game_lights::read_settings()
	{
		auto& cfg = shared::common::config::get();
		enabled = cfg.get_bool("Lights", "Enabled", true);
		intensity_scale = cfg.get_float("Lights", "IntensityScale", 1.0f);
		fire_light_scale = std::max(0.0f, cfg.get_float("Lights", "FireLightScale", 1.0f));
		effect_table_scale = std::max(0.0f, cfg.get_float("Lights", "EffectTableScale", 1.0f));
		radius_scale = cfg.get_float("Lights", "RadiusScale", 1.0f);
		game_falloff_ = cfg.get_bool("Lights", "GameFalloff", true);
		skip_characters_only_ = cfg.get_bool("Lights", "SkipCharacterOnly", true);
		game::point_lights::set_resolve_entities(cfg.get_bool("Lights", "ResolveEntityParams", true));
		game::map_lights::set_enabled(cfg.get_bool("Lights", "SynthesizedLights", false));

		ambient_lock_set = cfg.get_float3("Ambient", "Lock", ambient_lock);
		ambient_prefer_scale = cfg.get_float("Ambient", "PreferScale", 0.0f);
		sun_enabled = cfg.get_bool("Sun", "Enabled", true);
		sun_intensity_scale = cfg.get_float("Sun", "IntensityScale", 1.0f);
		sun_angular_diameter = cfg.get_float("Sun", "AngularDiameter", 0.53f);
		cfg.get_float3("Sun", "FallbackColour", sun_fallback_colour);
		read_own_sun();

		auto& p = player_cfg_;
		p.enabled = cfg.get_bool("PlayerLight", "Enabled", true);
		p.use_game_colour = cfg.get_bool("PlayerLight", "UseGameColour", true);
		cfg.get_float3("PlayerLight", "Colour", p.colour);
		p.use_game_range = cfg.get_bool("PlayerLight", "UseGameRange", true);
		p.range = std::max(0.01f, cfg.get_float("PlayerLight", "Range", 7.0f));
		p.use_game_radius = cfg.get_bool("PlayerLight", "UseGameRadius", true);
		p.radius = std::max(0.01f, cfg.get_float("PlayerLight", "Radius", 0.5f));
		p.intensity = std::max(0.0f, cfg.get_float("PlayerLight", "Intensity", 1.0f));
		p.falloff_intensity = std::max(0.0f, cfg.get_float("PlayerLight", "FalloffIntensity", 1.0f));
		p.falloff_range_scale = std::max(0.01f, cfg.get_float("PlayerLight", "FalloffRangeScale", 1.0f));
		p.falloff_max_colour = std::max(0.0f, cfg.get_float("PlayerLight", "FalloffMaxColour", 0.0f));
		p.falloff_gamma_add = cfg.get_bool("PlayerLight", "FalloffGammaAdd", true);
		const auto frame = cfg.get_string("PlayerLight", "OffsetFrame", "View");
		p.offset_frame = _stricmp(frame.c_str(), "World") == 0 ? 0 : _stricmp(frame.c_str(), "Character") == 0 ? 2 : 1;
		p.offset[0] = p.offset[1] = p.offset[2] = 0.0f;
		cfg.get_float3("PlayerLight", "Offset", p.offset);
	}

	// ------------------------------------------------------------------ UI

	// A per-light edit as the pending-save list describes it.
	struct game_lights_override_view
	{
		bool present = false, disabled = false, recolour = false, include_map = false;
		float multiplier = 1.0f, colour[3] = {}, offset[3] = {};
	};

	namespace
	{
		// Settings read as a form: each label in a fixed left column (its help on hover), its control after it.
		float form_label_width() { return ImGui::GetFontSize() * 22.0f; }
		float form_control_width() { return ImGui::GetFontSize() * 20.0f; }

		void tip(const char* text)
		{
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", text);
		}

		// The label, then the cursor in the control column with the control's width set.
		void form_label(const char* label, const char* help)
		{
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(label);
			if (help)
				tip(help);
			ImGui::SameLine(form_label_width());
			ImGui::SetNextItemWidth(form_control_width());
		}

		bool form_checkbox(const char* label, const char* id, bool* v, const char* help)
		{
			form_label(label, help);
			const bool changed = ImGui::Checkbox(id, v);
			if (help)
				tip(help);
			return changed;
		}

		bool near_equal(float a, float b) { return std::abs(a - b) <= 5e-4f * std::max(1.0f, std::abs(b)); }
		bool near_equal(bool a, bool b) { return a == b; }
		bool near_equal(int a, int b) { return a == b; }
		bool near_equal3(const float* a, const float* b) { return near_equal(a[0], b[0]) && near_equal(a[1], b[1]) && near_equal(a[2], b[2]); }

		// Two INI values alike: numbers (one or a comma list) compared as numbers, anything else as text.
		bool same_values(const std::string& a, const std::string& b)
		{
			auto numbers = [](const std::string& s, std::vector<float>& out)
			{
				std::string t = s;
				std::replace(t.begin(), t.end(), ',', ' ');
				std::istringstream in(t);
				std::string word;
				while (in >> word)
				{
					char* end = nullptr;
					const float v = std::strtof(word.c_str(), &end);
					if (!end || *end)
						return false;
					out.push_back(v);
				}
				return !out.empty();
			};
			std::vector<float> x, y;
			if (numbers(a, x) && numbers(b, y) && x.size() == y.size())
			{
				for (size_t i = 0; i < x.size(); ++i)
					if (!near_equal(x[i], y[i]))
						return false;
				return true;
			}
			return _stricmp(a.c_str(), b.c_str()) == 0;
		}

		std::string describe_edit(const game_lights_override_view& o)
		{
			if (!o.present)
				return "the game's own";
			std::string s;
			auto add = [&](const std::string& part) { s += (s.empty() ? "" : ", ") + part; };
			if (o.disabled) add("off");
			if (std::abs(o.multiplier - 1.0f) > 5e-4f) add(std::format("x{:.2f}", o.multiplier));
			if (o.recolour) add(std::format("colour {:.2f} {:.2f} {:.2f}", o.colour[0], o.colour[1], o.colour[2]));
			if (o.include_map) add("lights the map");
			if (o.offset[0] != 0.0f || o.offset[1] != 0.0f || o.offset[2] != 0.0f)
				add(std::format("moved {:.2f} {:.2f} {:.2f}", o.offset[0], o.offset[1], o.offset[2]));
			return s.empty() ? "the game's own" : s;
		}

		// After a setting: "Saved" puts back what the INI or the light files hold, "Default" the game's own value (the
		// shipped default for the settings of our own), each shown only while the setting differs from it. Saved is left
		// out when it is the default, and either when there is none (nullptr). True when one was pressed.
		int revert_pair(const char* id, bool differs_saved, bool saved_is_default, bool differs_default, bool have_default)
		{
			int pressed = 0;
			// A clear gap after the control, so a click meant for a tick never lands on a button that has just appeared.
			const float gap = ImGui::GetFontSize();
			if (differs_saved && !(have_default && saved_is_default))
			{
				ImGui::SameLine(0.0f, gap);
				if (ImGui::SmallButton(std::format("Saved##rv_s_{}", id).c_str()))
					pressed = 1;
				tip("Back to the saved value (remix-comp-proxy.ini, light_overrides.txt, categories.txt or the map's custom light file).");
			}
			if (have_default && differs_default)
			{
				ImGui::SameLine(0.0f, differs_saved && !saved_is_default ? -1.0f : gap);
				if (ImGui::SmallButton(std::format("Default##rv_d_{}", id).c_str()))
					pressed = 2;
				tip("Back to the game's own value (the shipped default for settings of our own).");
			}
			return pressed;
		}

		template <class T>
		bool revert_buttons(const char* id, T& value, const std::type_identity_t<T>* saved, const std::type_identity_t<T>* fallback)
		{
			const bool differs_saved = saved && !near_equal(value, *saved);
			const bool saved_is_default = saved && fallback && near_equal(*saved, *fallback);
			const int pressed = revert_pair(id, differs_saved, saved_is_default, fallback && !near_equal(value, *fallback), fallback != nullptr);
			if (pressed)
				value = pressed == 1 ? *saved : *fallback;
			return pressed != 0;
		}

		template <class T>
		bool revert_buttons(const char* id, T& value, const std::type_identity_t<T>& saved, const std::type_identity_t<T>& fallback)
		{
			return revert_buttons<T>(id, value, &saved, &fallback);
		}

		bool revert_buttons3(const char* id, float* value, const float* saved, const float* fallback)
		{
			const bool differs_saved = saved && !near_equal3(value, saved);
			const bool saved_is_default = saved && fallback && near_equal3(saved, fallback);
			const int pressed = revert_pair(id, differs_saved, saved_is_default, fallback && !near_equal3(value, fallback), fallback != nullptr);
			if (pressed)
				std::memcpy(value, pressed == 1 ? saved : fallback, sizeof(float) * 3);
			return pressed != 0;
		}

		// A read-only fact about the selected light.
		void form_value(const char* label, const std::string& value)
		{
			ImGui::TextDisabled("%s", label);
			ImGui::SameLine(form_label_width());
			ImGui::TextUnformatted(value.c_str());
		}

		const char* state_help(game_lights::row_state s)
		{
			using st = game_lights::row_state;
			switch (s)
			{
			case st::on: return "Sent to Remix and drawn.";
			case st::rejected: return "Remix refused the light.";
			case st::lights_off: return "Switched off (Light sources).";
			case st::no_params: return "The engine slot has no colour or reach to draw.";
			case st::effect_off: return "Follows its effect, and the effect is not shown at its spot right now.";
			case st::source_off: return "Its source is switched off under Light sources.";
			case st::disabled: return "Switched off in its own settings.";
			case st::solo_other: return "Another light is soloed.";
			case st::over_limit: return "Past 'Most lights drawn' under Brightness and reach.";
			case st::characters: return "The game lights only characters with it (for example whoever stands in a sunbeam that is\n"
				"baked into the lightmap). As a Remix light it would light the walls around it too, so it\n"
				"starts off. Select it and tick 'Light the map as well' to draw it anyway.";
			}
			return "";
		}

		// What each source is, for the Light sources table.
		const char* const kKindHelp[] = {
			"The game's key / shadow light, drawn as a Remix distant light.",
			"The light the game hangs on the player's head. It has its own settings (select it in the list).",
			"Lights placed in the map (MSB Light events), with their colour from the POINT_LIGHT_BANK row.",
			"Map lights the engine holds as bank entities.",
			"Lights carried by enemies and NPCs (LerpBank entities).",
			"Lights spawned by effects: spells, items, SFX.",
			"Synthesized for flames the engine never lights (rtx_comp\\lights\\<map>.txt):\n"
				"Blighttown, Tomb of the Giants, Duke's Archives, the Kiln.",
			"Synthesized for lit effect draws the game gives no light (lightning bolts, fire breath).",
			"Lights for effects the game draws without one, from the effect table.",
			"Lights placed by hand for this map (Custom lights for this map, below).",
			"Lamps the game gives no light: every piece of a draw with a texture listed in mesh_lights.txt.",
			"Engine slots with no colour or reach. Never drawn.",
		};
		static_assert(std::size(kKindHelp) == static_cast<size_t>(kind::count));
	}

	void game_lights::select_sun()
	{
		selected_ = KEY_SUN;
		selection_.clear();
		list_custom_indices_.clear();
		custom_owns_list_ = false;
	}

	void game_lights::tab_imgui()
	{
		list_imgui();
	}

	void game_lights::list_imgui()
	{
		// --- status and saving, always in view
		if (remix_api_ready_)
			ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Remix API ready");
		else
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Waiting for the Remix API");
		ImGui::SameLine();
		ImGui::TextDisabled("|  %d of %d point lights drawn  |  sun %s  |  player light %s",
			active_light_count(), total_light_count(), sun_handle_ ? "on" : "off", player_light_.handle ? "on" : "off");
		if (create_failed_count() > 0 || sun_create_failed_ > 0)
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Remix rejected %d point / %d sun CreateLight call(s)",
				create_failed_count(), sun_create_failed_);

		if (ImGui::Button("Save"))
			save_all();
		tip("Writes every light change listed below: [Lights], [Sun] and [PlayerLight] to remix-comp-proxy.ini, per-light edits to\n"
			"rtx_comp\\lights\\light_overrides.txt, the stand-in categories to categories.txt, this map's own sun to\n"
			"rtx_comp\\settings\\area_overrides.ini and this map's custom lights to their file.");
		ImGui::SameLine();
		if (ImGui::Button("Revert to saved"))
			revert_to_saved();
		tip("Throws away unsaved changes and reloads the saved settings and per-light edits.");
		ImGui::SameLine();
		if (ImGui::Button("Clear single-light edits"))
		{
			overrides_.clear();
			decoupled_.clear();
			solo_ = 0;
			flash_selected_ = false;
		}
		tip("Clears every light's own switch, brightness and colour, and any solo. Saved only with Save.");
		pending_imgui();
		ImGui::Spacing();

		const bool open_all = shared::globals::imgui_open_light_sections;
		shared::globals::imgui_open_light_sections = false;
		// Screenshots of every section (proxy.Menu.Tab=Lights+) also show the map's first custom light selected.
		if (open_all && !game::custom_lights::edit().empty())
		{
			select_custom(0);
			for (const auto& r : rows_)
				if (r.kind == kind::custom && r.slot == game::custom_lights::kFirstSlot)
				{
					selected_ = r.ident();
					selection_.clear();
				}
		}
		auto header = [&](const char* label, ImGuiTreeNodeFlags flags = 0)
		{
			if (open_all)
				ImGui::SetNextItemOpen(true);
			return ImGui::CollapsingHeader(label, flags);
		};
		if (header("Turn groups on or off"))
			sources_imgui();
		if (header("Brightness and reach of point lights"))
			scales_imgui();
		if (header("Light list", ImGuiTreeNodeFlags_DefaultOpen))
		{
			table_imgui();
			selected_imgui();
		}
		if (open_custom_)
		{
			ImGui::SetNextItemOpen(true);
			open_custom_ = false;
		}
		if (header("Custom lights for this map"))
			custom_lights_imgui();
		if (header("Diagnostics"))
			diagnostics_imgui();

		// Never shorter than it has been since the menu opened (a gap of frames = it was closed): a toggle that takes rows
		// away from a page scrolled down would otherwise pull everything down under the cursor.
		const float end_y = ImGui::GetCursorPosY();
		const int frame = ImGui::GetFrameCount();
		if (frame - content_height_frame_ > 2)
			content_height_max_ = 0.0f;
		content_height_frame_ = frame;
		content_height_max_ = std::max(content_height_max_, end_y);
		if (end_y < content_height_max_)
			ImGui::Dummy(ImVec2(1.0f, content_height_max_ - end_y));
	}

	// Under Save: every change Save would write, each with a tick (unticked: Save leaves it as the files have it) and a
	// Revert. A box of fixed height, so a change never moves the rest of the menu.
	void game_lights::pending_imgui()
	{
		struct item { std::string id, what, change; int revert = -1; };
		std::vector<item> items;
		std::vector<std::function<void()>> reverts;

		for (const auto& v : ini_values())
		{
			const std::string name = std::string(v.section) + "." + v.key;
			const auto it = saved_ini_values_.find(name);
			const std::string saved = it == saved_ini_values_.end() ? std::string() : it->second;
			if (same_values(saved, v.value))
				continue;
			items.push_back({ "ini:" + name, v.label, std::format("{} -> {}   [{}] {}", saved.empty() ? "unset" : saved, v.value, v.section, v.key) });
		}

		auto light_name = [&](uint64_t key)
		{
			for (const auto& r : rows_)
				if (r.key == key)
					return r.group.empty() || r.own_key == key ? r.name : std::format("{} (group '{}')", r.name, r.group);
			return std::format("light {:016x} (not on this map)", key);
		};
		std::set<uint64_t> keys;
		for (const auto& [key, o] : overrides_)
			if ((key & kSessionKeyBit) == 0 && !o.is_default())
				keys.insert(key);
		for (const auto& [key, o] : saved_overrides_)
			keys.insert(key);
		for (const uint64_t key : keys)
		{
			const auto* cur = find_override(key);
			const auto* saved = find_saved_override(key);
			const light_override none {};
			const auto& a = cur && (key & kSessionKeyBit) == 0 ? *cur : none;
			const auto& b = saved ? *saved : none;
			if (a.disabled == b.disabled && near_equal(a.multiplier, b.multiplier) && a.recolour == b.recolour &&
				(!a.recolour || near_equal3(a.colour, b.colour)) && a.include_map == b.include_map && near_equal3(a.offset, b.offset))
				continue;
			auto view = [](const light_override& o, bool present)
			{
				game_lights_override_view v;
				v.present = present && !o.is_default();
				v.disabled = o.disabled; v.multiplier = o.multiplier; v.recolour = o.recolour; v.include_map = o.include_map;
				std::memcpy(v.colour, o.colour, sizeof(v.colour));
				std::memcpy(v.offset, o.offset, sizeof(v.offset));
				return v;
			};
			reverts.push_back([this, key]()
			{
				if (const auto* s = find_saved_override(key)) overrides_[key] = *s;
				else overrides_.erase(key);
			});
			items.push_back({ std::format("light:{:016x}", key), light_name(key),
				std::format("{} -> {}", describe_edit(view(b, saved != nullptr)), describe_edit(view(a, cur != nullptr))),
				static_cast<int>(reverts.size()) - 1 });
		}

		std::set<uint64_t> decoupled_keys(decoupled_.begin(), decoupled_.end());
		decoupled_keys.insert(saved_decoupled_.begin(), saved_decoupled_.end());
		for (const uint64_t key : decoupled_keys)
		{
			const bool now = decoupled_.count(key) != 0;
			if (now == (saved_decoupled_.count(key) != 0))
				continue;
			reverts.push_back([this, key, now]()
			{
				if (now) decoupled_.erase(key);
				else decoupled_.insert(key);
				// Its own edit as the file has it, or none.
				if (const auto* s = find_saved_override(key)) overrides_[key] = *s;
				else overrides_.erase(key);
			});
			items.push_back({ std::format("decoupled:{:016x}", key), light_name(key),
				now ? "edited on its own, apart from its group" : "follows its group again", static_cast<int>(reverts.size()) - 1 });
		}

		const auto& categories = game::map_lights::categories();
		for (size_t i = 0; i < categories.size(); ++i)
		{
			const auto& c = categories[i];
			const auto* s = game::map_lights::saved_category(c.name);
			if (!s || (c.enabled == s->enabled && c.follows_effect == s->follows_effect && near_equal(c.intensity, s->intensity) &&
				near_equal(c.radius, s->radius)))
				continue;
			auto text = [](const game::map_lights::category& x)
			{
				return std::format("{}{} x{:.2f} reach x{:.2f}", x.enabled ? "on" : "off", x.follows_effect ? ", follows effect" : "", x.intensity, x.radius);
			};
			const game::map_lights::category saved = *s;
			reverts.push_back([i, saved]() { game::map_lights::set_category(i, saved.intensity, saved.radius, saved.enabled, saved.follows_effect); });
			items.push_back({ "cat:" + c.name, std::format("Stand-in category '{}'", c.name), std::format("{} -> {}", text(saved), text(c)),
				static_cast<int>(reverts.size()) - 1 });
		}

		// This map's own sun (written to area_overrides.ini) and its custom lights (their file).
		{
			const auto map_name = game::map_lights::current_map_name();
			float saved_colour[3] = {}, saved_direction[3] = {};
			bool saved_box = false;
			const bool have_saved = game::area_overrides::saved_sun(map_name, saved_colour, saved_direction, saved_box);
			const bool own = sun_colour_set && sun_direction_set;
			const bool differs = own != have_saved ||
				(own && (!near_equal3(sun_colour, saved_colour) || !near_equal3(sun_direction, saved_direction) || saved_box != (g_sun_scope_area != 0)));
			if (!map_name.empty() && differs)
			{
				auto text = [](bool on, const float* colour, const float* direction, bool box)
				{
					if (!on)
						return std::string("the game's");
					const float elevation = std::asin(std::clamp(-direction[1], -1.0f, 1.0f)) * kRadToDeg;
					const float heading = std::atan2(-direction[0], -direction[2]) * kRadToDeg;
					return std::format("{:.3f},{:.3f},{:.3f} at {:.1f} / {:.1f} deg{}", colour[0], colour[1], colour[2], elevation, heading,
						box ? ", an area" : "");
				};
				reverts.push_back([this, have_saved, saved_box]()
				{
					float c[3] = {}, d[3] = {};
					bool box = false;
					if (have_saved && game::area_overrides::saved_sun(game::map_lights::current_map_name(), c, d, box))
					{
						std::memcpy(sun_colour, c, sizeof(c));
						std::memcpy(sun_direction, d, sizeof(d));
						sun_colour_set = sun_direction_set = true;
						g_sun_scope_area = box ? 1 : 0;
					}
					else
						sun_colour_set = sun_direction_set = false;
				});
				items.push_back({ "sun:" + map_name, std::format("Sun of {}", map_name),
					std::format("{} -> {}", text(have_saved, saved_colour, saved_direction, saved_box),
						text(own, sun_colour, sun_direction, g_sun_scope_area != 0)),
					static_cast<int>(reverts.size()) - 1 });
			}
			if (game::custom_lights::dirty())
			{
				reverts.push_back([this]() { game::custom_lights::reload(); select_custom(-1); });
				items.push_back({ "custom:" + map_name, std::format("Custom lights of {}", map_name), "changed",
					static_cast<int>(reverts.size()) - 1 });
			}
		}

		// Ticks of items that no longer differ are forgotten.
		for (auto it = save_skip_.begin(); it != save_skip_.end();)
		{
			const bool listed = std::any_of(items.begin(), items.end(), [&](const item& x) { return x.id == *it; });
			it = listed ? std::next(it) : save_skip_.erase(it);
		}
		const auto ticked = std::count_if(items.begin(), items.end(), [&](const item& x) { return !save_skip_.count(x.id); });

		ImGui::SameLine();
		if (items.empty())
			ImGui::TextDisabled("%s", save_status_.empty() ? "no changes" : save_status_.c_str());
		else
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%d unsaved change%s, %d ticked to save%s%s", static_cast<int>(items.size()),
				items.size() == 1 ? "" : "s", static_cast<int>(ticked), save_status_.empty() ? "" : "  |  last: ", save_status_.c_str());

		const float em = ImGui::GetFontSize();
		int revert_now = -1;
		if (ImGui::BeginChild("##pending_save", ImVec2(0.0f, em * 6.5f), ImGuiChildFlags_Borders))
		{
			if (items.empty())
				ImGui::TextDisabled("Save would write nothing: everything is as the files have it.");
			for (const auto& x : items)
			{
				ImGui::PushID(x.id.c_str());
				bool include = !save_skip_.count(x.id);
				if (ImGui::Checkbox("##save", &include))
				{
					if (include) save_skip_.erase(x.id);
					else save_skip_.insert(x.id);
				}
				tip("Ticked: Save writes this. Unticked: Save leaves it as the files have it; the change stays here, unsaved.");
				ImGui::SameLine();
				ImGui::BeginDisabled(x.revert < 0);
				if (ImGui::SmallButton("Revert") && x.revert >= 0)
					revert_now = x.revert;
				ImGui::EndDisabled();
				tip(x.revert >= 0 ? "Puts back what the files hold for this one." : "Use the setting's own Saved button.");
				ImGui::SameLine();
				ImGui::TextUnformatted(x.what.c_str());
				ImGui::SameLine();
				ImGui::TextDisabled("%s", x.change.c_str());
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
		if (revert_now >= 0)
			reverts[revert_now]();
	}

	void game_lights::read_own_sun()
	{
		auto& cfg = shared::common::config::get();
		sun_colour_set = cfg.get_float3("Sun", "Colour", sun_colour);
		sun_direction_set = cfg.get_float3("Sun", "Direction", sun_direction);
		if (sun_direction_set)
		{
			const float length = std::sqrt(sun_direction[0] * sun_direction[0] + sun_direction[1] * sun_direction[1] + sun_direction[2] * sun_direction[2]);
			sun_direction_set = length > 0.0f;
			for (float& d : sun_direction)
				d /= std::max(length, 1e-6f);
		}
	}

	void game_lights::duplicate_as_custom(const light_row& row)
	{
		auto& lights = game::custom_lights::edit();
		game::custom_lights::light l;
		l.name = row.name.empty() ? "copy" : row.name + "_copy";
		std::replace(l.name.begin(), l.name.end(), ' ', '_');
		const auto* o = find_override(row.key);
		for (int c = 0; c < 3; ++c)
			l.pos[c] = row.pos[c] + (o ? o->offset[c] : 0.0f);
		// A custom light's copy a little above it: at its exact spot it would share its key (kind and position) and so its edits.
		if (row.kind == kind::custom)
			l.pos[1] += 0.25f;
		const float* colour = o && o->recolour ? o->colour : row.colour;
		const float peak = std::max({ colour[0], colour[1], colour[2] });
		const bool lit = peak > 1e-3f;
		for (int c = 0; c < 3; ++c)
			l.colour[c] = lit ? std::clamp(colour[c] / peak, 0.0f, 1.0f) : 1.0f;
		// The game's falloff and reach, as bright as the row is drawn (its colour's peak times its edit); a light the game
		// gave no parameters a plain torch's (0.5-4 m, 1).
		l.game_falloff = true;
		const bool reach = row.range_end > row.range_begin && row.range_end > 0.05f;
		l.dwindle_begin = reach ? std::max(row.range_begin, 0.01f) : 0.5f;
		l.dwindle_end = reach ? row.range_end : 4.0f;
		l.intensity = (lit ? peak : 1.0f) * (o ? o->multiplier : 1.0f);
		lights.push_back(l);
		game::custom_lights::mark_dirty();
		select_custom(static_cast<int>(lights.size()) - 1);
		open_custom_ = true;
	}

	void game_lights::own_sun_imgui()
	{
		const auto& sun = game::sun_light::current(shared::common::ffp_state::get().frame_count(), sun_fallback_colour);
		{
			const unsigned map_id = game::map_lights::current_map_id();
			const auto map_name = game::map_lights::current_map_name();
			const auto& authored = game::map_sun::get(map_id);
			const auto& map_values = authored.authored ? authored : seen_sun_;
			const bool saved = game::area_overrides::has_saved_sun(map_name);
			if (map_values.has_sun)
				ImGui::TextUnformatted("This map has a sun.");
			else
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "This map has no sun.");

			bool own = sun_colour_set && sun_direction_set;
			form_checkbox("Set my own colour and direction", "##sun_own", &own,
				"Saved with Save at the top, like every light change; unsaved, it is lost when you enter another area.\n"
				"Turning this off and saving takes the saved sun of this map away.\n"
				"Lightmaps still dim it on lightmapped surfaces, as they do the game's sun.");
			{
				// Saved: the colour and direction the INI or area_overrides.ini give here. Default: the game's sun, for
				// this session (a saved sun stays saved).
				float saved_colour[3] = {}, saved_direction[3] = {};
				const auto colour_text = saved_ini("Sun", "Colour"), direction_text = saved_ini("Sun", "Direction");
				const bool have_saved =
					std::sscanf(colour_text.c_str(), "%f , %f , %f", &saved_colour[0], &saved_colour[1], &saved_colour[2]) == 3 &&
					std::sscanf(direction_text.c_str(), "%f , %f , %f", &saved_direction[0], &saved_direction[1], &saved_direction[2]) == 3;
				const float length = std::sqrt(saved_direction[0] * saved_direction[0] + saved_direction[1] * saved_direction[1] + saved_direction[2] * saved_direction[2]);
				for (float& d : saved_direction)
					d /= std::max(length, 1e-6f);
				const bool differs_saved = have_saved && (!own || !near_equal3(sun_colour, saved_colour) || !near_equal3(sun_direction, saved_direction));
				const int pressed = revert_pair("sun_own", differs_saved, false, own, true);
				if (pressed == 1)
				{
					read_own_sun();
					own = sun_colour_set && sun_direction_set;
				}
				else if (pressed == 2)
				{
					sun_colour_set = sun_direction_set = false;
					own = false;
				}
			}
			if (own != (sun_colour_set && sun_direction_set))
			{
				sun_colour_set = sun_direction_set = own;
				if (own)
				{
					// From what is there now, so turning it on changes nothing until it is edited.
					const bool from_map = map_values.has_sun;
					for (int c = 0; c < 3; ++c)
					{
						sun_colour[c] = from_map ? map_values.lobe[c] : 0.3f;
						sun_direction[c] = from_map ? map_values.direction[c] : sun.key.dir[c];
					}
					if (!from_map && !sun.valid)
					{
						sun_direction[0] = -0.5f;
						sun_direction[1] = -0.7f;
						sun_direction[2] = -0.5f;
					}
				}
			}
			if (own)
			{
				form_label("Colour", "In the game's gamma space, as the banks give it. A sun of Anor Londo's strength is about 0.7, 0.5, 0.4.\n"
					"'Saved' and 'Default' next to 'Set my own colour and direction' put back the saved sun or the game's.");
				ImGui::ColorEdit3("##sun_own_colour", sun_colour, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);

				const float length = std::max(std::sqrt(sun_direction[0] * sun_direction[0] + sun_direction[1] * sun_direction[1] + sun_direction[2] * sun_direction[2]), 1e-5f);
				float elevation = std::asin(std::clamp(-sun_direction[1] / length, -1.0f, 1.0f)) * kRadToDeg;
				float heading = std::atan2(-sun_direction[0], -sun_direction[2]) * kRadToDeg;
				form_label("Elevation", "Degrees above the horizon. 90 is straight overhead.");
				bool moved = ImGui::SliderFloat("##sun_own_elevation", &elevation, 1.0f, 90.0f, "%.1f deg");
				form_label("Heading", "Degrees around the horizon.");
				moved = ImGui::SliderFloat("##sun_own_heading", &heading, -180.0f, 180.0f, "%.1f deg") || moved;
				if (moved)
				{
					const float e = elevation / kRadToDeg, h = heading / kRadToDeg;
					sun_direction[0] = -std::cos(e) * std::sin(h);
					sun_direction[1] = -std::sin(e);
					sun_direction[2] = -std::cos(e) * std::cos(h);
				}

				form_label("Applies to", "Where Save puts this sun: the whole map, or a box this far around where you stand now.\n"
					"Written to rtx_comp\\settings\\area_overrides.ini, which you can also edit by hand. Each map keeps one saved sun.");
				ImGui::RadioButton("This map##sun_scope_map", &g_sun_scope_area, 0);
				ImGui::SameLine();
				ImGui::RadioButton("An area##sun_scope_area", &g_sun_scope_area, 1);
				if (g_sun_scope_area)
				{
					ImGui::SameLine();
					ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
					ImGui::SliderFloat("##sun_own_box", &g_sun_box_reach, 2.0f, 200.0f, "%.0f m around you");
				}
			}
			if (saved)
				ImGui::TextDisabled("This map has a saved sun of its own. Save at the top writes changes to it.");
			else if (own)
				ImGui::TextDisabled("Not saved yet: Save at the top writes it.");
		}
	}

	void game_lights::sources_imgui()
	{
		ImGui::Checkbox("Draw point lights", &enabled);
		tip("Every light in the list except the sun and the player light, which have their own switch below.");
		revert_buttons("lights_on", enabled, saved_bool("Lights", "Enabled", true), true);
		ImGui::SameLine(0.0f, ImGui::GetFontSize() * 2.0f);
		ImGui::Checkbox("Use the game's falloff", &game_falloff_);
		tip("For the groups ticked under 'Game-style fade': full strength out to dwindle begin, then linearly\n"
			"to zero at dwindle end, as the game lights them. Off: inverse square for every light.");
		revert_buttons("game_falloff", game_falloff_, saved_bool("Lights", "GameFalloff", true), true);

		int counts[static_cast<int>(kind::count)] = {};
		int drawn[static_cast<int>(kind::count)] = {};
		for (const auto& r : rows_)
		{
			++counts[static_cast<int>(r.kind)];
			if (r.state == row_state::on)
				++drawn[static_cast<int>(r.kind)];
		}

		const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuter |
			ImGuiTableFlags_SizingFixedFit;
		// Every column right of the stretching Source column has a fixed width, its buttons' room included. With them fitting their contents, a Default button
		// appearing after a tick was cleared widened the column and moved the tick from under the mouse.
		const auto& table_style = ImGui::GetStyle();
		const float button_slot = ImGui::GetFrameHeight() + ImGui::GetFontSize() + ImGui::CalcTextSize("Saved").x + ImGui::CalcTextSize("Default").x +
			table_style.FramePadding.x * 4.0f + table_style.ItemSpacing.x + table_style.CellPadding.x * 2.0f;
		const float count_width = ImGui::CalcTextSize("Drawn now 0000").x + table_style.CellPadding.x * 2.0f;
		const float table_width = std::min(ImGui::GetFontSize() * 32.0f + button_slot * 2.0f + count_width, ImGui::GetContentRegionAvail().x);
		if (ImGui::BeginTable("##sources", 4, flags, ImVec2(table_width, 0.0f)))
		{
			ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Draw", ImGuiTableColumnFlags_WidthFixed, button_slot);
			ImGui::TableSetupColumn("Game-style fade", ImGuiTableColumnFlags_WidthFixed, button_slot);
			ImGui::TableSetupColumn("Drawn now", ImGuiTableColumnFlags_WidthFixed, count_width);
			ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
			ImGui::TableNextColumn();
			ImGui::TableHeader("Source");
			tip("Hover a source for what it is.");
			ImGui::TableNextColumn();
			ImGui::TableHeader("Draw");
			tip("Saved with Save: Sun, Player and Stand-ins. The other switches last for this session.");
			ImGui::TableNextColumn();
			ImGui::TableHeader("Game-style fade");
			tip("Lit with the game's falloff instead of inverse square (needs 'Use the game's falloff'). This session.");
			ImGui::TableNextColumn();
			ImGui::TableHeader("Drawn now");

			for (int k = 0; k < static_cast<int>(kind::count); ++k)
			{
				const auto lk = static_cast<kind>(k);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(kKindSection[k]);
				tip(kKindHelp[k]);

				ImGui::TableNextColumn();
				const std::string draw_id = std::format("##draw{}", k);
				const std::string revert_id = std::format("draw{}", k);
				if (lk == kind::sun)
				{
					ImGui::Checkbox(draw_id.c_str(), &sun_enabled);
					revert_buttons(revert_id.c_str(), sun_enabled, saved_bool("Sun", "Enabled", true), true);
				}
				else if (lk == kind::player)
				{
					ImGui::Checkbox(draw_id.c_str(), &player_cfg_.enabled);
					revert_buttons(revert_id.c_str(), player_cfg_.enabled, saved_bool("PlayerLight", "Enabled", true), true);
				}
				else if (lk == kind::synthetic)
				{
					// Stand-ins are also not built at all while off ([Lights] SynthesizedLights, saved).
					bool on = game::map_lights::enabled() && kind_enabled_[k];
					if (ImGui::Checkbox(draw_id.c_str(), &on) |
						revert_buttons(revert_id.c_str(), on, saved_bool("Lights", "SynthesizedLights", false), false))
					{
						game::map_lights::set_enabled(on);
						kind_enabled_[k] = on;
					}
				}
				else if (lk == kind::unresolved)
					ImGui::TextDisabled("-");
				else
				{
					ImGui::Checkbox(draw_id.c_str(), &kind_enabled_[k]);
					const bool on = lk != kind::effect; // the guesses start off
					revert_buttons(revert_id.c_str(), kind_enabled_[k], nullptr, &on);
				}

				ImGui::TableNextColumn();
				if (lk == kind::sun || lk == kind::unresolved)
					ImGui::TextDisabled("-");
				else if (lk == kind::custom)
				{
					ImGui::TextDisabled("per light");
					tip("Each custom light is a Remix light unless its 'Use the game's falloff' is ticked (Custom lights).");
				}
				else
				{
					ImGui::BeginDisabled(!game_falloff_);
					ImGui::Checkbox(std::format("##falloff{}", k).c_str(), &kind_game_falloff_[k]);
					revert_buttons(std::format("falloff{}", k).c_str(), kind_game_falloff_[k], nullptr, &kind_game_falloff_initial_[k]);
					ImGui::EndDisabled();
				}

				ImGui::TableNextColumn();
				if (lk == kind::sun)
					ImGui::TextUnformatted(sun_handle_ ? "on" : "off");
				else if (lk == kind::player)
					ImGui::TextUnformatted(player_light_.handle ? "on" : "off");
				else if (counts[k] == 0)
					ImGui::TextDisabled("none here");
				else
					ImGui::Text("%d of %d", drawn[k], counts[k]);
			}
			ImGui::EndTable();
		}

		ImGui::Spacing();
		form_checkbox("Character-only lights start off", "##charonly", &skip_characters_only_,
			"Map lights the game binds to characters only light none of the map in the game (a light for whoever\n"
			"stands in a sunbeam baked into the lightmap). As Remix lights they would light the walls around\n"
			"them, so they start off, unless the light is a flame's the map places (a torch stays a torch); each can\n"
			"be switched on in its own settings ('Light the map as well').\n"
			"Untick to draw them all. Saved as [Lights] SkipCharacterOnly.");
		revert_buttons("charonly", skip_characters_only_, saved_bool("Lights", "SkipCharacterOnly", true), true);
		bool resolve = game::point_lights::resolve_entities();
		if (form_checkbox("Read entity light colours", "##resolve", &resolve,
			"Effect, character and bank-entity lights keep their colour on the light entity, not on a map row.\n"
			"Off: only map lights (and the stand-ins) get a colour and reach. Saved as [Lights] ResolveEntityParams.") |
			revert_buttons("resolve", resolve, saved_bool("Lights", "ResolveEntityParams", true), true))
			game::point_lights::set_resolve_entities(resolve);
	}

	void game_lights::scales_imgui()
	{
		ImGui::TextDisabled("For every light in the list. The sun and the player light have their own settings (select them).");
		form_label("Brightness", "Every point light but the custom Remix lights, which keep their own units.\n"
			"1 = the tuned default. Saved as [Lights] IntensityScale.");
		ImGui::SliderFloat("##intensity", &intensity_scale, 0.01f, 50.0f, "x %.2f", ImGuiSliderFlags_Logarithmic);
		revert_buttons("intensity", intensity_scale, saved_float("Lights", "IntensityScale", 1.0f), 1.0f);
		form_label("Fire stand-in brightness",
			"Lights stood in for fire effects the game gives no light (fire breath, fire spells).\n"
			"1 = torch to bonfire strength. Saved as [Lights] FireLightScale.");
		ImGui::SliderFloat("##fire", &fire_light_scale, 0.0f, 10.0f, "x %.2f", ImGuiSliderFlags_Logarithmic);
		revert_buttons("fire", fire_light_scale, std::max(0.0f, saved_float("Lights", "FireLightScale", 1.0f)), 1.0f);
		form_label("Effect light brightness",
			"Lights from ffx_lights.tsv, for effects the game draws bright but gives no light (spells, attacks, ambient effects).\n"
			"Their colour is a fit from the effect's sprites, weaker than the game's own effect lights of the same kind: fire by 1.8,\n"
			"magic by 7, other glows by 18, in median light. This scales their colour; light goes with its 2.2 power.\n"
			"Saved as [Lights] EffectTableScale.");
		ImGui::SliderFloat("##effect_table", &effect_table_scale, 0.0f, 10.0f, "x %.2f", ImGuiSliderFlags_Logarithmic);
		revert_buttons("effect_table", effect_table_scale, std::max(0.0f, saved_float("Lights", "EffectTableScale", 1.0f)), 1.0f);
		form_label("Reach", "How far every point light reaches: its dwindle begin and end times this (not the custom Remix\n"
			"lights). Saved as [Lights] RadiusScale.");
		ImGui::SliderFloat("##reach", &radius_scale, 0.1f, 10.0f, "x %.2f");
		revert_buttons("reach", radius_scale, saved_float("Lights", "RadiusScale", 1.0f), 1.0f);
		form_label("Emitter size", "Radius of the sphere Remix shines each point light from (custom Remix lights have their\n"
			"own). Bigger = softer shadows, and a big sphere next to a wall lights it as a bright disc. This session.");
		ImGui::SliderFloat("##emitter", &emitter_radius_, 0.01f, 2.0f, "%.2f m");
		const float emitter_default = 0.1f;
		revert_buttons("emitter", emitter_radius_, nullptr, &emitter_default);
		form_label("Most lights drawn", "Lights past this count are listed as 'over the limit'. This session.");
		ImGui::SliderInt("##max", &max_active_lights_, 0, 512, max_active_lights_ == 0 ? "no limit" : "%d");
		const int max_default = 0;
		revert_buttons("max", max_active_lights_, nullptr, &max_default);
	}

	void game_lights::table_imgui()
	{
		row_order_next_.clear();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
		ImGui::InputTextWithHint("##filter", "Filter by name or details", filter_, sizeof(filter_));
		tip("Click a light to edit it. Ctrl+click adds or removes one, Shift+click selects a range;\n"
			"the panel below then changes all of them.");
		ImGui::SameLine();
		ImGui::Checkbox("Group by source", &group_rows_);
		ImGui::SameLine(0.0f, ImGui::GetFontSize() * 1.5f);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Sort by");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
		ImGui::Combo("##sort", &sort_mode_, "distance\0name\0");

		// The sun and the player light pinned on top, then the rest filtered, sorted and (by default) in a
		// section per source, stand-ins split by category.
		std::vector<const light_row*> rows;
		rows.reserve(rows_.size());
		for (const auto& r : rows_)
			if (contains_no_case(r.name, filter_) || contains_no_case(r.origin, filter_) || contains_no_case(r.group, filter_))
				rows.push_back(&r);

		std::sort(rows.begin(), rows.end(), [&](const light_row* a, const light_row* b)
		{
			if (group_rows_ && a->kind != b->kind) return a->kind < b->kind;
			if (group_rows_ && a->group != b->group) return a->group < b->group;
			if (sort_mode_ == 1 && a->name != b->name) return a->name < b->name;
			return a->distance < b->distance;
		});

		const auto& sun = game::sun_light::current(shared::common::ffp_state::get().frame_count(), sun_fallback_colour);
		light_row sun_row;
		sun_row.key = sun_map_key(game::map_lights::current_map_id());
		sun_row.id = KEY_SUN;
		sun_row.kind = kind::sun;
		sun_row.name = "Sun";
		{
			static const char* kDirSource[] = { "no direction yet", "shadow light c175", "map light 0 c92", "character light 0 c92" };
			sun_row.origin = sun_colour_set && sun_direction_set ? "Custom sun" : kDirSource[static_cast<int>(sun.dir_from)];
		}
		std::memcpy(sun_row.colour, sun.key.colour, sizeof(sun_row.colour));
		if (sun_handle_)
		{
			sun_row.radiance[0] = sun_info_.radiance.x;
			sun_row.radiance[1] = sun_info_.radiance.y;
			sun_row.radiance[2] = sun_info_.radiance.z;
		}
		sun_row.drawn = sun_handle_ != nullptr;
		sun_row.state = sun_row.drawn ? row_state::on : row_state::lights_off;

		light_row player_row;
		player_row.key = KEY_PLAYER;
		player_row.kind = kind::player;
		player_row.name = "Player head light";
		if (player_seen_)
		{
			const auto& l = player_src_;
			player_row.slot = l.slot;
			player_row.pos[0] = l.x; player_row.pos[1] = l.y; player_row.pos[2] = l.z;
			player_row.colour[0] = l.r; player_row.colour[1] = l.g; player_row.colour[2] = l.b;
			player_row.range_begin = l.dwindle_begin;
			player_row.range_end = l.dwindle_end;
			player_row.origin = std::format("slot {}, {}", l.slot,
				l.entity ? bank_row_text(l.entity, classify(l) == kind::lerp_entity) : "no entity");
		}
		else
		{
			player_row.origin = "no light at the player's head";
		}
		std::memcpy(player_row.radiance, player_radiance_, sizeof(player_row.radiance));
		player_row.drawn = player_light_.handle != nullptr;
		player_row.state = player_row.drawn ? row_state::on : player_seen_ ? row_state::lights_off : row_state::no_params;

		const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
			ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Hideable;
		if (ImGui::BeginTable("##light_list", 7, flags, ImVec2(0.0f, ImGui::GetFontSize() * 14.0f)))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Light", ImGuiTableColumnFlags_WidthStretch, 4.0f);
			const float em = ImGui::GetFontSize();
			ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, em * 9.0f);
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 14.0f);
			ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthFixed, em * 4.5f);
			ImGui::TableSetupColumn("Game colour", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, em * 9.0f);
			ImGui::TableSetupColumn("Reach (m)", ImGuiTableColumnFlags_WidthFixed, em * 5.5f);
			ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultHide, 3.0f);
			ImGui::TableHeadersRow();

			// A band across the table: what follows belongs to it, level with every other row.
			const ImU32 band_colour = ImGui::GetColorU32(ImVec4(0.26f, 0.38f, 0.58f, 0.55f));
			const ImU32 sub_band_colour = ImGui::GetColorU32(ImVec4(0.26f, 0.38f, 0.58f, 0.22f));

			ImGui::TableNextRow();
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, band_colour);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted("Sun and player");
			list_row_imgui(sun_row);
			list_row_imgui(player_row);

			// A collapsible band with how many of its lights are drawn. Its rows are not indented: a light under a band is a light like the sun or the player's.
			auto section = [&](const std::string& label, const light_row* const* first, const light_row* const* last,
				bool open_by_default, const char* id, bool top)
			{
				const auto on = std::count_if(first, last, [](const light_row* r) { return r->state == row_state::on; });
				ImGui::TableNextRow();
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, top ? band_colour : sub_band_colour);
				ImGui::TableNextColumn();
				// The menu's wide frame padding would put the band's text deeper than the lights under it.
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));
				const bool open = ImGui::TreeNodeEx(std::format("{}###{}", label, id).c_str(),
					ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_NoTreePushOnOpen | (open_by_default ? ImGuiTreeNodeFlags_DefaultOpen : 0));
				ImGui::PopStyleVar();
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%d of %d on", static_cast<int>(on), static_cast<int>(last - first));
				return open;
			};

			if (!group_rows_)
			{
				for (const auto* r : rows)
					list_row_imgui(*r);
			}
			else
			{
				for (size_t i = 0; i < rows.size();)
				{
					const kind k = rows[i]->kind;
					size_t end = i;
					while (end < rows.size() && rows[end]->kind == k)
						++end;
					if (section(kKindSection[static_cast<int>(k)], &rows[i], &rows[0] + end, k != kind::unresolved,
						std::format("kind{}", static_cast<int>(k)).c_str(), true))
					{
						for (size_t g = i; g < end;)
						{
							size_t g_end = g;
							while (g_end < end && rows[g_end]->group == rows[g]->group)
								++g_end;
							if (rows[g]->group.empty())
							{
								for (size_t r = g; r < g_end; ++r)
									list_row_imgui(*rows[r]);
							}
							else if (section(rows[g]->group, &rows[g], &rows[0] + g_end, true,
								std::format("group{}{}", static_cast<int>(k), rows[g]->group).c_str(), false))
							{
								for (size_t r = g; r < g_end; ++r)
									list_row_imgui(*rows[r]);
							}
							g = g_end;
						}
					}
					i = end;
				}
			}
			ImGui::EndTable();
		}
		ImGui::TextDisabled("Click a light to change it below. Right-click the header row for more columns.");
		row_order_.swap(row_order_next_);
	}

	void game_lights::selected_imgui()
	{
		ImGui::Spacing();
		// Custom lights picked in the list are worked on by index from then on: moving or switching one off changes or takes away its row.
		if (!list_custom_indices_.empty())
		{
			const int count = static_cast<int>(game::custom_lights::edit().size());
			std::erase_if(list_custom_indices_, [&](int i) { return i < 0 || i >= count; });
			if (game::custom_lights::generation() != list_custom_generation_ || list_custom_indices_.size() < 2)
				list_custom_indices_.clear();
		}
		if (list_custom_indices_.size() > 1)
		{
			mirror_custom_to_list(list_custom_indices_);
			ImGui::SeparatorText(std::format("{} custom lights selected", list_custom_indices_.size()).c_str());
			ImGui::PushID("custom_rows");
			// A copy: Delete and Duplicate end this selection from inside.
			custom_multi_imgui(std::vector<int>(list_custom_indices_));
			ImGui::PopID();
			return;
		}
		if (selection_.size() > 1)
		{
			std::vector<const light_row*> rows;
			for (const auto& r : rows_)
				if (selection_.count(r.ident()))
					rows.push_back(&r);
			const int pinned = static_cast<int>(selection_.count(KEY_SUN) + selection_.count(KEY_PLAYER));
			// Custom lights only: every setting of theirs, not just the switch, brightness, colour and move other lights have.
			if (!pinned && rows.size() > 1 && std::all_of(rows.begin(), rows.end(), [](const light_row* r) { return r->kind == kind::custom; }))
			{
				for (const auto* r : rows)
					list_custom_indices_.push_back(r->slot - game::custom_lights::kFirstSlot);
				list_custom_generation_ = game::custom_lights::generation();
				ImGui::SeparatorText(std::format("{} custom lights selected", list_custom_indices_.size()).c_str());
				ImGui::PushID("custom_rows");
				custom_multi_imgui(std::vector<int>(list_custom_indices_));
				ImGui::PopID();
				return;
			}
			multi_imgui(rows, pinned);
			return;
		}
		if (selected_ == KEY_SUN)
		{
			sun_imgui();
			return;
		}
		if (selected_ == KEY_PLAYER)
		{
			player_light_imgui();
			return;
		}
		const light_row* sel = nullptr;
		for (const auto& r : rows_)
			if (r.ident() == selected_) sel = &r;
		if (sel)
			point_light_imgui(*sel);
		else
			ImGui::TextDisabled("Select a light in the list to see and change its settings.");
	}

	// Several lights selected: a change here goes to all of them, and only the setting changed.
	void game_lights::multi_imgui(const std::vector<const light_row*>& rows, int pinned)
	{
		ImGui::SeparatorText(std::format("{} lights selected", rows.size() + pinned).c_str());
		if (pinned)
			ImGui::TextDisabled("The sun and the player light have their own settings: select one alone to change it.");
		if (rows.empty())
			return;
		std::vector<uint64_t> keys;
		for (const auto* r : rows)
			if (std::find(keys.begin(), keys.end(), r->key) == keys.end())
				keys.push_back(r->key);
		if (keys.size() < rows.size())
			ImGui::TextDisabled("Lights of one effect share their settings, so changes also reach the rest of their group.");

		light_override first;
		bool mixed = false;
		if (const auto* o = find_override(keys[0]))
			first = *o;
		for (const uint64_t key : keys)
		{
			const auto* o = find_override(key);
			mixed |= (o ? o->disabled : false) != first.disabled || (o ? o->multiplier : 1.0f) != first.multiplier ||
				(o ? o->recolour : false) != first.recolour;
			for (int c = 0; c < 3; ++c)
				mixed |= (o ? o->offset[c] : 0.0f) != first.offset[c];
		}
		// One line either way, so the controls below stay put when a change makes them alike or not.
		if (mixed)
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "These differ: the values below are the first light's.");
		else
			ImGui::TextDisabled("All of them have the same settings.");

		bool draw = !first.disabled;
		if (form_checkbox("Draw these lights", "##multi_draw", &draw, "Switches all of them off or on."))
			for (const uint64_t key : keys)
				overrides_[key].disabled = !draw;
		form_label("Brightness", "Set on all of them, on top of the source brightness. 0 = off.");
		if (ImGui::SliderFloat("##multi_mult", &first.multiplier, 0.0f, 10.0f, "x %.2f", ImGuiSliderFlags_Logarithmic))
			for (const uint64_t key : keys)
				overrides_[key].multiplier = first.multiplier;
		if (form_checkbox("Own colour", "##multi_recolour", &first.recolour, "Replace the game's colour with your own on all of them."))
			for (const auto* r : rows)
			{
				auto& o = overrides_[r->key];
				if (first.recolour && !o.recolour)
					std::memcpy(o.colour, r->colour, sizeof(o.colour));
				o.recolour = first.recolour;
			}
		form_label("Colour", nullptr);
		ImGui::BeginDisabled(!first.recolour);
		if (ImGui::ColorEdit3("##multi_colour", first.colour, ImGuiColorEditFlags_Float | (first.recolour ? 0 : ImGuiColorEditFlags_NoDragDrop)) && first.recolour)
			for (const uint64_t key : keys)
				std::memcpy(overrides_[key].colour, first.colour, sizeof(first.colour));
		ImGui::EndDisabled();
		form_label("Move", "Metres from where the game puts each of them (x, height, z), the same for all. Saved with Save.");
		if (ImGui::DragFloat3("##multi_offset", first.offset, 0.01f, -20.0f, 20.0f, "%.2f"))
			for (const uint64_t key : keys)
				std::memcpy(overrides_[key].offset, first.offset, sizeof(first.offset));
		form_checkbox("Flash to find them", "##multi_flash", &flash_selected_, "Makes the selected lights 20 times brighter while ticked.");
		if (ImGui::Button("Reset these"))
			for (const uint64_t key : keys)
				overrides_.erase(key);
		tip("Back to the game's own values: clears their own switch, brightness, colour and move. Saved only with Save.");
		ImGui::SameLine();
		if (ImGui::Button("Revert these to saved"))
			for (const uint64_t key : keys)
			{
				if (const auto* saved = find_saved_override(key))
					overrides_[key] = *saved;
				else
					overrides_.erase(key);
			}
		tip("Gives each of them back its switch, brightness, colour and move as light_overrides.txt has them.");
		ImGui::SameLine();
		ImGui::BeginDisabled(!have_clipboard_);
		if (ImGui::Button("Paste onto these"))
			for (const uint64_t key : keys)
				overrides_[key] = clipboard_;
		ImGui::EndDisabled();
		tip("Gives all of them the copied light's switch, brightness, colour and move.");
	}

	// Solo and flash work on every light, the pinned ones included.
	void game_lights::solo_flash_imgui(uint64_t key)
	{
		bool solo = solo_ == key;
		if (form_checkbox("Show only this light", "##solo", &solo, "Draw only this light, to see what it lights. Every other light shows 'hidden by solo'."))
			solo_ = solo ? key : 0;
		form_checkbox("Flash this light", "##flash", &flash_selected_, "Makes the selected light 20 times brighter while ticked.");
	}

	void game_lights::custom_lights_imgui()
	{
		auto& lights = game::custom_lights::edit();
		const std::string& path = game::custom_lights::path();
		// Indices belong to one reading of one map's file, and only as long as it has that many lights.
		if (const auto generation = game::custom_lights::generation(); generation != custom_generation_)
		{
			custom_generation_ = generation;
			select_custom(-1);
		}
		while (!custom_selection_.empty() && *custom_selection_.rbegin() >= static_cast<int>(lights.size()))
			custom_selection_.erase(std::prev(custom_selection_.end()));
		// The rows of the selected lights change as they move: kept in step, so flashing still finds them.
		if (custom_owns_list_ && custom_selection_.size() > 1)
			mirror_custom_to_list(custom_selection_);
		if (path.empty())
		{
			ImGui::TextDisabled("No map loaded.");
			return;
		}
		ImGui::TextWrapped("Lights you place yourself. They are saved to this map's own file and load only while this map is "
			"the current one, so a light never shows up in another map. Ctrl+click and Shift+click select several, to change, "
			"move, copy or delete them together.");
		ImGui::TextDisabled("%s: %s", path.c_str(), game::custom_lights::status().c_str());
		if (game::custom_lights::dirty())
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "unsaved changes");
		}

		float cam[3] = {}, right[3] = {}, forward[3] = {};
		const bool have_cam = shared::common::ffp_state::get().world_offset(cam[0], cam[1], cam[2]);
		const bool have_axes = shared::common::ffp_state::get().view_axes(right, forward);
		float player[3] = {};
		const bool have_player = player_raw_pos(player);

		auto add = [&](const float* at)
		{
			game::custom_lights::light l;
			for (int c = 0; c < 3; c++)
				l.pos[c] = at[c];
			l.name = std::format("light_{}", lights.size() + 1);
			if (have_axes)
				for (int c = 0; c < 3; c++)
					l.dir[c] = forward[c];
			lights.push_back(l);
			select_custom(static_cast<int>(lights.size()) - 1);
			game::custom_lights::mark_dirty();
		};
		if (ImGui::Button("Add at the camera") && have_cam)
			add(cam);
		tip("A new light where the camera is, aimed where it looks (if you make it a spotlight).");
		ImGui::SameLine();
		if (ImGui::Button("Add above the player") && have_player)
		{
			const float at[3] = { player[0], player[1] + 2.0f, player[2] };
			add(at);
		}
		tip("A new light 2 m above the player.");
		ImGui::SameLine();
		custom_lights_file_buttons();

		if (lights.empty())
		{
			ImGui::TextDisabled("No custom lights on this map yet.");
			return;
		}

		bool changed = false;
		const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
			ImGuiTableFlags_SizingFixedFit;
		if (ImGui::BeginTable("##custom", 4, flags))
		{
			ImGui::TableSetupColumn("Light", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("On");
			ImGui::TableSetupColumn("Type");
			ImGui::TableSetupColumn("Position");
			ImGui::TableHeadersRow();
			for (int i = 0; i < static_cast<int>(lights.size()); i++)
			{
				auto& l = lights[i];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				const bool picked = custom_selection_.empty() ? custom_selected_ == i : custom_selection_.count(i) != 0;
				if (ImGui::Selectable(std::format("{}##custom{}", l.name, i).c_str(), picked,
					ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
					custom_click(i, static_cast<int>(lights.size()));
				ImGui::TableNextColumn();
				changed |= ImGui::Checkbox(std::format("##custom_on{}", i).c_str(), &l.enabled);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(l.spot ? (l.game_falloff ? "spotlight, game falloff" : "spotlight")
					: (l.game_falloff ? "point, game falloff" : "point"));
				// A light that belongs to an area of the map (area_overrides.ini, lights = <name prefix>).
				// Asked first: asking for the area's name can rebuild what the name points into.
				const bool away = game::area_overrides::custom_light_away(l.name);
				if (const char* area = game::area_overrides::custom_light_area(l.name))
				{
					ImGui::SameLine();
					ImGui::TextColored(away ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f), away ? "- away, only in %s" : "- only in %s", area);
					tip("rtx_comp\\settings\\area_overrides.ini gives lights with this name's start to that area (lights = ...):\n"
						"they are there only while the player is in it.");
				}
				ImGui::TableNextColumn();
				ImGui::Text("%.1f, %.1f, %.1f", l.pos[0], l.pos[1], l.pos[2]);
			}
			ImGui::EndTable();
		}

		if (custom_selection_.size() > 1)
		{
			ImGui::SeparatorText(std::format("{} custom lights selected", custom_selection_.size()).c_str());
			ImGui::PushID("custom_section_multi");
			custom_multi_imgui(std::vector<int>(custom_selection_.begin(), custom_selection_.end()));
			ImGui::PopID();
		}
		else if (custom_selected_ >= 0 && custom_selected_ < static_cast<int>(lights.size()))
		{
			auto& l = lights[custom_selected_];
			ImGui::SeparatorText(l.name.c_str());
			// The same fields are drawn in a custom row's own editor: each place needs its own IDs, or a button drawn in both never takes a click.
			ImGui::PushID("custom_section");
			changed |= custom_light_fields_imgui(l);
			ImGui::PopID();
			if (const auto* saved = game::custom_lights::saved(l))
			{
				if (ImGui::Button("Revert this light"))
				{
					l = *saved;
					changed = true;
				}
				tip("Puts back everything about this light as the map's file has it.");
				ImGui::SameLine();
			}
			if (ImGui::Button("Delete this light"))
			{
				lights.erase(lights.begin() + custom_selected_);
				select_custom(-1);
				changed = true;
			}
			if (custom_selected_ >= 0)
			{
				ImGui::PushID("custom_section");
				custom_light_actions_imgui(custom_selected_);
				ImGui::PopID();
			}
		}
		if (changed)
			game::custom_lights::mark_dirty();
	}

	void game_lights::select_custom(int index)
	{
		custom_selected_ = index;
		custom_selection_.clear();
		custom_anchor_ = index;
		list_custom_indices_.clear();
		custom_owns_list_ = false;
		// A selection made from outside the section (a row's Duplicate) belongs to this reading of the file too.
		if (index >= 0)
			custom_generation_ = game::custom_lights::generation();
	}

	template <typename Indices> void game_lights::mirror_custom_to_list(const Indices& indices)
	{
		std::unordered_set<uint64_t> rows;
		uint64_t current = 0;
		for (const auto& r : rows_)
		{
			if (r.kind != kind::custom)
				continue;
			const int index = r.slot - game::custom_lights::kFirstSlot;
			if (std::find(indices.begin(), indices.end(), index) == indices.end())
				continue;
			rows.insert(r.ident());
			if (index == custom_selected_ || !current)
				current = r.ident();
		}
		if (!rows.empty())
		{
			// The current row and the Shift anchor stay where the user put them while they are still among these.
			if (!rows.count(selected_))
				selected_ = current;
			if (!rows.count(selection_anchor_))
				selection_anchor_ = selected_;
			selection_ = std::move(rows);
		}
	}

	void game_lights::custom_click(int index, int count)
	{
		const auto& io = ImGui::GetIO();
		if (custom_selection_.empty() && custom_selected_ >= 0)
			custom_selection_.insert(custom_selected_);
		if (io.KeyShift)
		{
			// From the anchor to this row; Ctrl keeps what was selected.
			if (!io.KeyCtrl)
				custom_selection_.clear();
			if (custom_anchor_ < 0 || custom_anchor_ >= count)
				custom_anchor_ = index;
			const int anchor = custom_anchor_;
			for (int i = std::min(anchor, index); i <= std::max(anchor, index); ++i)
				custom_selection_.insert(i);
			custom_selected_ = index;
		}
		else if (io.KeyCtrl)
		{
			if (custom_selection_.count(index) && custom_selection_.size() > 1)
			{
				custom_selection_.erase(index);
				if (custom_selected_ == index)
					custom_selected_ = *custom_selection_.begin();
			}
			else
			{
				custom_selection_.insert(index);
				custom_selected_ = index;
			}
			custom_anchor_ = index;
		}
		else
		{
			custom_selection_.clear();
			custom_selection_.insert(index);
			custom_selected_ = index;
			custom_anchor_ = index;
		}
		// The same lights in the list above, so flashing finds them, until a row of the list is clicked.
		list_custom_indices_.clear();
		custom_owns_list_ = true;
		mirror_custom_to_list(custom_selection_);
	}

	void game_lights::custom_multi_imgui(const std::vector<int>& picked)
	{
		using light = game::custom_lights::light;
		auto& lights = game::custom_lights::edit();
		std::vector<int> indices;
		for (const int i : picked)
			if (i >= 0 && i < static_cast<int>(lights.size()))
				indices.push_back(i);
		if (indices.size() < 2)
			return;
		ImGui::TextDisabled("A change here goes to all of them, and only the setting changed. The values are the first light's (%s);\n"
			"'differ' marks a setting they do not share.", lights[indices[0]].name.c_str());
		// The first light's values are edited on a copy, and what changed is written to every light.
		light v = lights[indices[0]];
		bool changed = false;
		auto differ = [&](auto member)
		{
			return std::any_of(indices.begin(), indices.end(), [&](int i) { return !(lights[i].*member == v.*member); });
		};
		auto differ3 = [&](float (light::*member)[3])
		{
			return std::any_of(indices.begin(), indices.end(), [&](int i) { return !std::equal(std::begin(v.*member), std::end(v.*member), std::begin(lights[i].*member)); });
		};
		auto to_all = [&](auto member)
		{
			for (const int i : indices)
				lights[i].*member = v.*member;
			changed = true;
		};
		auto to_all3 = [&](float (light::*member)[3])
		{
			for (const int i : indices)
				std::copy(std::begin(v.*member), std::end(v.*member), std::begin(lights[i].*member));
			changed = true;
		};
		auto label = [&](const char* text, bool mixed, const char* help)
		{
			form_label(mixed ? std::format("{}  (differ)", text).c_str() : text, help);
		};

		if (form_checkbox(differ(&light::enabled) ? "Switched on  (differ)" : "Switched on", "##cm_on", &v.enabled, nullptr))
			to_all(&light::enabled);
		form_label("Position of the first (x, height, z)", "Moving it moves all of them by the same amount.");
		{
			float pos[3] = { v.pos[0], v.pos[1], v.pos[2] };
			if (ImGui::DragFloat3("##cm_pos", pos, 0.02f, 0.0f, 0.0f, "%.2f"))
			{
				for (const int i : indices)
					for (int c = 0; c < 3; ++c)
						lights[i].pos[c] += pos[c] - v.pos[c];
				changed = true;
			}
		}
		label("Colour", differ3(&light::colour), nullptr);
		if (ImGui::ColorEdit3("##cm_colour", v.colour))
			to_all3(&light::colour);
		if (form_checkbox(differ(&light::game_falloff) ? "Use the game's falloff  (differ)" : "Use the game's falloff", "##cm_game", &v.game_falloff,
			"Off: a plain Remix sphere light. On: lit as the game lights its own."))
			to_all(&light::game_falloff);
		if (!v.game_falloff)
		{
			label("Intensity", differ(&light::remix_intensity), "Radiance on top of the colour, in Remix's units. Ctrl+click to type one.");
			if (ImGui::DragFloat("##cm_rintensity", &v.remix_intensity, std::max(0.1f, v.remix_intensity * 0.01f), 0.0f, 1000000.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp))
				to_all(&light::remix_intensity);
			label("Exposure", differ(&light::exposure), "Radiance x 2^exposure, in stops.");
			if (ImGui::DragFloat("##cm_exposure", &v.exposure, 0.05f, -20.0f, 20.0f, "%.2f EV"))
				to_all(&light::exposure);
			label("Radius", differ(&light::radius), "The emitting sphere. Bigger = softer shadows.");
			if (ImGui::DragFloat("##cm_radius", &v.radius, 0.005f, 0.001f, 10.0f, "%.3f m"))
				to_all(&light::radius);
			label("Volumetric scale", differ(&light::volumetric), "How strongly they light Remix's volumetric fog.");
			if (ImGui::DragFloat("##cm_volumetric", &v.volumetric, 0.01f, 0.0f, 10.0f, "%.2f"))
				to_all(&light::volumetric);
		}
		else
		{
			label("Intensity", differ(&light::intensity), "Brightness on top of the colour, as the game's light rows give it.");
			if (ImGui::DragFloat("##cm_intensity", &v.intensity, 0.01f, 0.0f, 50.0f, "%.2f"))
				to_all(&light::intensity);
			label("Full strength out to", differ(&light::dwindle_begin), "Dwindle begin.");
			if (ImGui::DragFloat("##cm_begin", &v.dwindle_begin, 0.05f, 0.0f, v.dwindle_end, "%.2f m"))
			{
				// Each light's own end bounds it: the first light's may be further out.
				for (const int i : indices)
					lights[i].dwindle_begin = std::min(v.dwindle_begin, lights[i].dwindle_end);
				changed = true;
			}
			label("Fades to zero at", differ(&light::dwindle_end), "Dwindle end.");
			if (ImGui::DragFloat("##cm_end", &v.dwindle_end, 0.05f, 0.05f, 200.0f, "%.2f m"))
				to_all(&light::dwindle_end);
			ImGui::Dummy(ImVec2(1.0f, ImGui::GetFrameHeight()));
		}
		if (form_checkbox(differ(&light::spot) ? "Spotlight  (differ)" : "Spotlight", "##cm_spot", &v.spot, "Shine in a cone instead of all around."))
			to_all(&light::spot);
		ImGui::BeginDisabled(!v.spot);
		{
			label("Aim direction", differ3(&light::dir), "The direction the cones point, (x, height, z).");
			if (ImGui::DragFloat3("##cm_dir", v.dir, 0.01f, -1.0f, 1.0f, "%.3f"))
				to_all3(&light::dir);
			ImGui::SameLine();
			float right[3] = {}, forward[3] = {};
			if (ImGui::Button("Aim along camera") && shared::common::ffp_state::get().view_axes(right, forward))
			{
				std::copy(std::begin(forward), std::end(forward), std::begin(v.dir));
				to_all3(&light::dir);
			}
			label("Cone half angle", differ(&light::cone_deg), nullptr);
			if (ImGui::SliderFloat("##cm_cone", &v.cone_deg, 1.0f, 89.0f, "%.1f deg"))
				to_all(&light::cone_deg);
			label("Cone edge softness", differ(&light::softness), nullptr);
			if (ImGui::SliderFloat("##cm_soft", &v.softness, 0.0f, 1.0f, "%.2f"))
				to_all(&light::softness);
			ImGui::BeginDisabled(v.game_falloff);
			label("Focus", differ(&light::focus), "Remix lights only.");
			if (ImGui::DragFloat("##cm_focus", &v.focus, 0.05f, 0.0f, 100.0f, "%.2f"))
				to_all(&light::focus);
			ImGui::EndDisabled();
		}
		ImGui::EndDisabled();
		form_checkbox("Flash to find them", "##cm_flash", &flash_selected_, "Makes the selected lights 20 times brighter while ticked (those that are lit now).");

		if (ImGui::Button("Delete these"))
		{
			// From the last to the first, so the indices still to go stay right.
			for (auto it = indices.rbegin(); it != indices.rend(); ++it)
				lights.erase(lights.begin() + *it);
			select_custom(-1);
			selection_.clear();
			game::custom_lights::mark_dirty();
			return;
		}
		tip("Takes all of them out of this map's file once saved. 'Revert custom lights' brings them back until then.");
		ImGui::SameLine();
		if (ImGui::Button("Duplicate these"))
		{
			const int first_new = static_cast<int>(lights.size());
			for (const int i : indices)
			{
				light copy = lights[i];
				copy.name += "_copy";
				copy.file_index = -1;
				// A step up: the list tells lights apart by position, and a copy on the same spot would be the same row as its original.
				copy.pos[1] += 0.06f;
				lights.push_back(std::move(copy));
			}
			select_custom(first_new);
			for (int i = first_new; i < static_cast<int>(lights.size()); ++i)
				custom_selection_.insert(i);
			selection_.clear();
			open_custom_ = true;
			game::custom_lights::mark_dirty();
			return;
		}
		tip("A copy of each 6 cm above it, selected in their place, to move or change on their own.");
		ImGui::SameLine();
		if (ImGui::Button("Revert these"))
		{
			for (const int i : indices)
				if (const auto* saved = game::custom_lights::saved(lights[i]))
				{
					lights[i] = *saved;
					changed = true;
				}
		}
		tip("Puts each back as the map's file has it. Lights added since stay as they are.");
		ImGui::SameLine();
		ImGui::BeginDisabled(!have_custom_clipboard_);
		if (ImGui::Button("Paste onto these"))
		{
			for (const int i : indices)
			{
				auto pasted = custom_clipboard_;
				pasted.name = lights[i].name;
				pasted.file_index = lights[i].file_index;
				std::copy(std::begin(lights[i].pos), std::end(lights[i].pos), std::begin(pasted.pos));
				lights[i] = std::move(pasted);
			}
			changed = true;
		}
		ImGui::EndDisabled();
		tip("Gives all of them the copied light's settings; each keeps its name and position.");
		if (changed)
			game::custom_lights::mark_dirty();
	}

	void game_lights::custom_light_actions_imgui(int index)
	{
		auto& lights = game::custom_lights::edit();
		if (index < 0 || index >= static_cast<int>(lights.size()))
			return;
		float cam[3] = {};
		const bool have_cam = shared::common::ffp_state::get().world_offset(cam[0], cam[1], cam[2]);
		// A new light from the given one: its own entry in the file, selected and shown under 'Custom lights for this map'.
		auto append = [&](game::custom_lights::light l, const std::string& name)
		{
			l.name = name;
			l.file_index = -1;
			lights.push_back(std::move(l));
			select_custom(static_cast<int>(lights.size()) - 1);
			open_custom_ = true;
			game::custom_lights::mark_dirty();
		};
		bool duplicate = ImGui::Button("Duplicate");
		tip("A new light with everything this one has, 6 cm above it, to move or change on its own.");
		ImGui::SameLine();
		if (ImGui::Button("Copy"))
		{
			custom_clipboard_ = lights[index];
			have_custom_clipboard_ = true;
		}
		tip("Remembers this light's settings, also for another map.");
		ImGui::SameLine();
		ImGui::BeginDisabled(!have_custom_clipboard_);
		const bool paste_new = ImGui::Button("Paste at the camera");
		tip("A new light with the copied settings where the camera is.");
		ImGui::SameLine();
		if (ImGui::Button("Paste onto this"))
		{
			auto pasted = custom_clipboard_;
			pasted.name = lights[index].name;
			pasted.file_index = lights[index].file_index;
			std::copy(std::begin(lights[index].pos), std::end(lights[index].pos), std::begin(pasted.pos));
			lights[index] = std::move(pasted);
			game::custom_lights::mark_dirty();
		}
		tip("Gives this light the copied settings; it keeps its name and position.");
		ImGui::EndDisabled();
		if (duplicate)
		{
			// A step up, so the list (which tells lights apart by position) has a row for each.
			auto copy = lights[index];
			copy.pos[1] += 0.06f;
			append(std::move(copy), lights[index].name + "_copy");
		}
		else if (paste_new && have_cam)
		{
			auto pasted = custom_clipboard_;
			std::copy(std::begin(cam), std::end(cam), std::begin(pasted.pos));
			append(std::move(pasted), custom_clipboard_.name + "_copy");
		}
	}

	bool game_lights::custom_light_fields_imgui(game::custom_lights::light& l)
	{
		bool changed = false;
		// Saved: the light as the map's file has it (none for a light added since). Custom lights have no game default.
		const auto* saved = game::custom_lights::saved(l);
		auto back = [&](const char* id, auto& value, auto member)
		{
			changed |= revert_buttons(id, value, saved ? &(saved->*member) : nullptr, static_cast<std::remove_reference_t<decltype(value)>*>(nullptr));
		};
		auto back3 = [&](const char* id, float* value, const float* saved_value)
		{
			changed |= revert_buttons3(id, value, saved ? saved_value : nullptr, nullptr);
		};
		float cam[3] = {}, right[3] = {}, forward[3] = {};
		const bool have_cam = shared::common::ffp_state::get().world_offset(cam[0], cam[1], cam[2]);
		const bool have_axes = shared::common::ffp_state::get().view_axes(right, forward);
		{
			char name[64] = {};
			std::snprintf(name, sizeof(name), "%s", l.name.c_str());
			form_label("Name", "One word: spaces become underscores in the file.");
			if (ImGui::InputText("##cl_name", name, sizeof(name)))
			{
				l.name = name;
				changed = true;
			}
			changed |= form_checkbox("Switched on", "##cl_on", &l.enabled, nullptr);
			back("cl_on", l.enabled, &game::custom_lights::light::enabled);
			form_label("Position (x, height, z)", "The game's world coordinates for this map.");
			changed |= ImGui::DragFloat3("##cl_pos", l.pos, 0.02f, 0.0f, 0.0f, "%.2f");
			back3("cl_pos", l.pos, saved ? saved->pos : nullptr);
			ImGui::SameLine();
			if (ImGui::Button("Move to camera") && have_cam)
			{
				std::copy(std::begin(cam), std::end(cam), std::begin(l.pos));
				changed = true;
			}
			form_label("Colour", nullptr);
			changed |= ImGui::ColorEdit3("##cl_colour", l.colour);
			back3("cl_colour", l.colour, saved ? saved->colour : nullptr);
			changed |= form_checkbox("Use the game's falloff", "##cl_game", &l.game_falloff,
				"Off: a plain Remix sphere light (physical inverse-square falloff, Remix's units).\n"
				"On: lit as the game lights its own: full strength out to a distance, then linearly to zero.");
			back("cl_game", l.game_falloff, &game::custom_lights::light::game_falloff);
			if (!l.game_falloff)
			{
				form_label("Intensity", "Radiance on top of the colour, in Remix's units (as the Remix toolkit's light Intensity).\n"
					"Drags in steps of about 1% of the value; Ctrl+click to type one.");
				changed |= ImGui::DragFloat("##cl_rintensity", &l.remix_intensity, std::max(0.1f, l.remix_intensity * 0.01f),
					0.0f, 1000000.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
				back("cl_rintensity", l.remix_intensity, &game::custom_lights::light::remix_intensity);
				form_label("Exposure", "Radiance x 2^exposure, in stops (as the toolkit's Exposure).");
				changed |= ImGui::DragFloat("##cl_exposure", &l.exposure, 0.05f, -20.0f, 20.0f, "%.2f EV");
				back("cl_exposure", l.exposure, &game::custom_lights::light::exposure);
				form_label("Radius", "The emitting sphere. Bigger = softer shadows; it also sets how bright the light looks up close.");
				changed |= ImGui::DragFloat("##cl_radius", &l.radius, 0.005f, 0.001f, 10.0f, "%.3f m");
				back("cl_radius", l.radius, &game::custom_lights::light::radius);
				form_label("Volumetric scale", "How strongly it lights Remix's volumetric fog (1 = as bright as on surfaces).");
				changed |= ImGui::DragFloat("##cl_volumetric", &l.volumetric, 0.01f, 0.0f, 10.0f, "%.2f");
				back("cl_volumetric", l.volumetric, &game::custom_lights::light::volumetric);
			}
			else
			{
				form_label("Intensity", "Brightness on top of the colour, as the game's light rows give it: 1 = the colour as is\n"
					"(the file stores it as a percentage).");
				changed |= ImGui::DragFloat("##cl_intensity", &l.intensity, 0.01f, 0.0f, 50.0f, "%.2f");
				back("cl_intensity", l.intensity, &game::custom_lights::light::intensity);
				form_label("Full strength out to", "Dwindle begin: the light is at full strength out to here...");
				changed |= ImGui::DragFloat("##cl_begin", &l.dwindle_begin, 0.05f, 0.0f, l.dwindle_end, "%.2f m");
				back("cl_begin", l.dwindle_begin, &game::custom_lights::light::dwindle_begin);
				form_label("Fades to zero at", "... and fades linearly to nothing at this distance (dwindle end).");
				changed |= ImGui::DragFloat("##cl_end", &l.dwindle_end, 0.05f, 0.05f, 200.0f, "%.2f m");
				back("cl_end", l.dwindle_end, &game::custom_lights::light::dwindle_end);
				// As tall as the Remix branch's four rows, so the Spotlight tick below does not move.
				ImGui::Dummy(ImVec2(1.0f, ImGui::GetFrameHeight()));
			}
			changed |= form_checkbox("Spotlight", "##cl_spot", &l.spot, "Shines in a cone instead of all around.");
			back("cl_spot", l.spot, &game::custom_lights::light::spot);
			// The cone's rows are always there (greyed out for a light that shines all around), so ticks below stay put.
			ImGui::BeginDisabled(!l.spot);
			{
				form_label("Aim direction", "The direction the cone points, (x, height, z); it does not need to be normalised.");
				changed |= ImGui::DragFloat3("##cl_dir", l.dir, 0.01f, -1.0f, 1.0f, "%.3f");
				back3("cl_dir", l.dir, saved ? saved->dir : nullptr);
				ImGui::SameLine();
				if (ImGui::Button("Aim along camera") && have_axes)
				{
					std::copy(std::begin(forward), std::end(forward), std::begin(l.dir));
					changed = true;
				}
				form_label("Cone half angle", "From the aim direction to the cone's edge.");
				changed |= ImGui::SliderFloat("##cl_cone", &l.cone_deg, 1.0f, 89.0f, "%.1f deg");
				back("cl_cone", l.cone_deg, &game::custom_lights::light::cone_deg);
				form_label("Cone edge softness", "0 = a hard edge, 1 = fades over the whole cone.");
				changed |= ImGui::SliderFloat("##cl_soft", &l.softness, 0.0f, 1.0f, "%.2f");
				back("cl_soft", l.softness, &game::custom_lights::light::softness);
				ImGui::BeginDisabled(l.game_falloff);
				form_label("Focus", "Concentrates the light towards the aim within the cone (the toolkit's focus exponent; 0 = even).\n"
					"Remix lights only (greyed out on the game's falloff).");
				changed |= ImGui::DragFloat("##cl_focus", &l.focus, 0.05f, 0.0f, 100.0f, "%.2f");
				back("cl_focus", l.focus, &game::custom_lights::light::focus);
				ImGui::EndDisabled();
			}
			ImGui::EndDisabled();
		}
		return changed;
	}

	void game_lights::custom_lights_file_buttons()
	{
		if (ImGui::Button("Revert custom lights"))
		{
			game::custom_lights::reload();
			select_custom(-1);
		}
		tip("Throws away unsaved custom light changes and reads the file again. Save at the top writes them.");
		if (game::custom_lights::dirty())
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "unsaved changes");
		}
	}

	void game_lights::sun_imgui()
	{
		const auto& sun = game::sun_light::current(shared::common::ffp_state::get().frame_count(),
			sun_fallback_colour);

		ImGui::SeparatorText("Sun (distant light)");
		if (!sun.valid)
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "No draw has given a sun direction for %u frames.", sun.frames_since_seen);
		else if (sun_handle_)
			ImGui::TextUnformatted("");
		else
			ImGui::TextDisabled("Not drawn.");
		tip("Direction: the game's shadow light (pixel shader c175, ShadowBank), which every shadowed\n"
			"shader reads; else directional light 0 (c92). Colour: light 0 (c95) from a HemDir3 draw, else the\n"
			"irradiance probe's sun, else the last one seen, else the fallback colour. Each frame the draws\n"
			"reading them vote, weighted by primitive count.");

		{
			const uint64_t sun_key = sun_map_key(game::map_lights::current_map_id());
			auto& here = overrides_[sun_key];
			const light_override none {};
			const auto* saved_here = find_saved_override(sun_key);
			const auto& saved = saved_here ? *saved_here : none;
			bool draw_here = !here.disabled;
			if (form_checkbox("Draw the sun on this map", "##sun_on_map", &draw_here, "This map only; saved with Save. Another map keeps its own."))
				here.disabled = !draw_here;
			revert_buttons("sun_on_map", here.disabled, saved.disabled, none.disabled);
			form_label("Brightness on this map", "This map only, 1 is the game's brightness; saved with Save. Every other map starts at 1.");
			ImGui::SliderFloat("##sun_int_map", &here.multiplier, 0.0f, 20.0f, "x %.2f", ImGuiSliderFlags_Logarithmic);
			revert_buttons("sun_int_map", here.multiplier, saved.multiplier, none.multiplier);
		}
		form_checkbox("Draw the sun (all maps)", "##sun_on", &sun_enabled, nullptr);
		revert_buttons("sun_on", sun_enabled, saved_bool("Sun", "Enabled", true), true);
		form_label("Brightness (all maps)", "On every map, on top of each map's own. 1 is the game's brightness.");
		ImGui::SliderFloat("##sun_int", &sun_intensity_scale, 0.0f, 20.0f, "x %.2f", ImGuiSliderFlags_Logarithmic);
		revert_buttons("sun_int", sun_intensity_scale, std::max(0.0f, saved_float("Sun", "IntensityScale", 1.0f)), 1.0f);
		form_label("Size in the sky", "Bigger gives softer shadows. The real sun is 0.53 degrees.");
		ImGui::SliderFloat("##sun_size", &sun_angular_diameter, 0.05f, 10.0f, "%.2f deg");
		revert_buttons("sun_size", sun_angular_diameter, std::clamp(saved_float("Sun", "AngularDiameter", 0.53f), 0.0f, 45.0f), 0.53f);
		form_label("Fallback colour", "Used when the game gives no sun colour.");
		ImGui::ColorEdit3("##sun_fallback", sun_fallback_colour, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		{
			const float fallback_default[3] = { 0.7f, 0.7f, 0.7f };
			float fallback_saved[3];
			saved_float3("Sun", "FallbackColour", fallback_default, fallback_saved);
			revert_buttons3("sun_fallback", sun_fallback_colour, fallback_saved, fallback_default);
		}
		solo_flash_imgui(sun_map_key(game::map_lights::current_map_id()));

		ImGui::Spacing();
		own_sun_imgui();

		ImGui::SeparatorText("What the game gives");
		auto show_light = [](const char* name, const game::sun_light::light& l)
		{
			ImGui::ColorButton(name, ImVec4(
				std::min(1.0f, l.colour[0]), std::min(1.0f, l.colour[1]), std::min(1.0f, l.colour[2]), 1.0f),
				ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
			ImGui::SameLine();
			ImGui::Text("%s  dir (%.3f, %.3f, %.3f)  rgb (%.3f, %.3f, %.3f)", name,
				l.dir[0], l.dir[1], l.dir[2], l.colour[0], l.colour[1], l.colour[2]);
		};
		show_light("Sun  ", sun.key);

		static const char* kDirSource[] = { "none", "shadow light c175", "map light 0 c92", "character light 0 c92" };
		static const char* kColourSource[] = { "fallback", "map light 0 c95", "character light 0 c95", "irradiance probe", "last seen" };
		const float to_sun_y = -sun.key.dir[1];
		const float heading = std::atan2(-sun.key.dir[0], -sun.key.dir[2]) * kRadToDeg;
		ImGui::TextDisabled("  elevation %.1f deg, heading %.1f deg  | direction: %s | colour: %s",
			std::asin(std::clamp(to_sun_y, -1.0f, 1.0f)) * kRadToDeg, heading,
			kDirSource[static_cast<int>(sun.dir_from)], kColourSource[static_cast<int>(sun.colour_from)]);
		ImGui::TextDisabled("  last frame: %u shadow draws (%d values), %u map / %u character HemDir3 draws (%d values)",
			sun.shadow_draws, sun.shadow_candidates, sun.map_dir3_draws, sun.chr_dir3_draws, sun.dir3_candidates);
		if (sun_create_failed_ > 0)
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Remix rejected %d sun CreateLight call(s)", sun_create_failed_);

		if (const float* ps = game::sun_light::ps_constants(); ps && ImGui::TreeNode("Raw pixel shader constants"))
		{
			for (int reg : { 175, 121, 122, 92, 93, 94, 95, 96, 97, 98, 99, 86, 87 })
				ImGui::Text("c%-3d  %9.4f %9.4f %9.4f %9.4f", reg,
					ps[reg * 4 + 0], ps[reg * 4 + 1], ps[reg * 4 + 2], ps[reg * 4 + 3]);
			ImGui::TreePop();
		}
		if (ImGui::TreeNode("Fill lights / hemisphere (not sent; Remix computes bounce)"))
		{
			show_light("Fill1", sun.fill[0]);
			show_light("Fill2", sun.fill[1]);
			ImGui::Text("Hemi up   (%.3f, %.3f, %.3f)", sun.hemi_up[0], sun.hemi_up[1], sun.hemi_up[2]);
			ImGui::Text("Hemi down (%.3f, %.3f, %.3f)", sun.hemi_down[0], sun.hemi_down[1], sun.hemi_down[2]);
			ImGui::TreePop();
		}
	}

	void game_lights::player_light_imgui()
	{
		auto& c = player_cfg_;

		ImGui::SeparatorText("Player head light (point light)");
		if (player_seen_)
		{
			const auto& l = player_src_;
			ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Found: slot %d, %s, %.2f from the head point",
				l.slot, l.entity_vtable ? game::point_lights::entity_class_name(l.entity_vtable)
					: (l.param_row ? "bank row" : "unresolved"), player_head_dist_);
		}
		else
		{
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "No light at the player's head.");
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			ImGui::SetTooltip(
				"The light the game attaches to the player's head. It is taken out of the point lights,\n"
				"so their brightness, reach and switches do not affect it. Found as the live light nearest\n"
				"%.1f units above the player origin, then held while it stays within %.1f units of that point.",
				kHeadHeight, kHoldRadius);
		if (player_seen_)
			ImGui::TextDisabled("Game: colour %.2f %.2f %.2f, reach %.2f - %.2f m.  Sent to Remix: radiance %.2f %.2f %.2f",
				player_src_.r, player_src_.g, player_src_.b, player_src_.dwindle_begin, player_src_.dwindle_end,
				player_radiance_[0], player_radiance_[1], player_radiance_[2]);

		const bool game_falloff = game_falloff_ && kind_game_falloff_[static_cast<int>(light_kind::player)];
		ImGui::TextDisabled("Falloff: %s (Light sources)", game_falloff ? "the game's (flat, then linear to zero)" : "inverse square");

		const shared::common::config::player_light_settings def;
		form_checkbox("Draw the player light", "##pl_on", &c.enabled, "Saved as [PlayerLight] Enabled.");
		revert_buttons("pl_on", c.enabled, saved_bool("PlayerLight", "Enabled", def.enabled), def.enabled);
		form_checkbox("Use the game's colour", "##pl_gc", &c.use_game_colour, "Saved as [PlayerLight] UseGameColour.");
		revert_buttons("pl_gc", c.use_game_colour, saved_bool("PlayerLight", "UseGameColour", def.use_game_colour), def.use_game_colour);
		{
			form_label("Colour", "Saved as [PlayerLight] Colour. Greyed out while the game's colour is used.");
			ImGui::BeginDisabled(c.use_game_colour);
			ImGui::ColorEdit3("##pl_colour", c.colour, ImGuiColorEditFlags_Float | (c.use_game_colour ? ImGuiColorEditFlags_NoDragDrop : 0));
			ImGui::EndDisabled();
			float saved_colour[3];
			saved_float3("PlayerLight", "Colour", def.colour, saved_colour);
			revert_buttons3("pl_colour", c.colour, saved_colour, def.colour);
		}

		if (game_falloff)
		{
			form_label("Reach", "The game's dwindle begin and end, both times this (1 = the game's reach,\n"
				"which changes by area). Saved as [PlayerLight] FalloffRangeScale.");
			ImGui::SliderFloat("##pl_reach", &c.falloff_range_scale, 0.1f, 4.0f, "x %.2f", ImGuiSliderFlags_Logarithmic);
			revert_buttons("pl_reach", c.falloff_range_scale, std::max(0.01f, saved_float("PlayerLight", "FalloffRangeScale", def.falloff_range_scale)), def.falloff_range_scale);
			form_label("Emitter size", "Radius of the sphere the light shines from. Saved as [PlayerLight] Radius.");
			ImGui::SliderFloat("##pl_radius", &c.radius, 0.01f, 2.0f, "%.2f m");
			revert_buttons("pl_radius", c.radius, std::max(0.01f, saved_float("PlayerLight", "Radius", def.radius)), def.radius);
			form_label("Brightness", "1 = the game's brightness. Saved as [PlayerLight] FalloffIntensity,\n"
				"apart from the inverse-square brightness.");
			ImGui::SliderFloat("##pl_fint", &c.falloff_intensity, 0.0f, 10.0f, "x %.2f", ImGuiSliderFlags_Logarithmic);
			revert_buttons("pl_fint", c.falloff_intensity, std::max(0.0f, saved_float("PlayerLight", "FalloffIntensity", def.falloff_intensity)), def.falloff_intensity);
			ImGui::Checkbox("Colour as the game mixes it with ambient##pl_gadd", &c.falloff_gamma_add);
			revert_buttons("pl_gadd", c.falloff_gamma_add, saved_bool("PlayerLight", "FalloffGammaAdd", def.falloff_gamma_add), def.falloff_gamma_add);
		}
		else
		{
			form_checkbox("Use the game's reach", "##pl_gr", &c.use_game_range, "Saved as [PlayerLight] UseGameRange.");
			revert_buttons("pl_gr", c.use_game_range, saved_bool("PlayerLight", "UseGameRange", def.use_game_range), def.use_game_range);
			{
				form_label("Reach", "Saved as [PlayerLight] Range. Greyed out while the game's reach is used.");
				ImGui::BeginDisabled(c.use_game_range);
				ImGui::SliderFloat("##pl_range", &c.range, 0.1f, 30.0f, "%.2f m");
				ImGui::EndDisabled();
				revert_buttons("pl_range", c.range, std::max(0.01f, saved_float("PlayerLight", "Range", def.range)), def.range);
			}
			form_checkbox("Use the game's emitter size", "##pl_gs", &c.use_game_radius,
				"The game's dwindle begin as the emitter. Saved as [PlayerLight] UseGameRadius.");
			revert_buttons("pl_gs", c.use_game_radius, saved_bool("PlayerLight", "UseGameRadius", def.use_game_radius), def.use_game_radius);
			{
				form_label("Emitter size", "Saved as [PlayerLight] Radius. Greyed out while the game's emitter size is used.");
				ImGui::BeginDisabled(c.use_game_radius);
				ImGui::SliderFloat("##pl_radius", &c.radius, 0.01f, 2.0f, "%.2f m");
				ImGui::EndDisabled();
				revert_buttons("pl_radius", c.radius, std::max(0.01f, saved_float("PlayerLight", "Radius", def.radius)), def.radius);
			}
			form_label("Brightness", "Saved as [PlayerLight] Intensity.");
			ImGui::SliderFloat("##pl_int", &c.intensity, 0.0f, 20.0f, "x %.2f", ImGuiSliderFlags_Logarithmic);
			revert_buttons("pl_int", c.intensity, std::max(0.0f, saved_float("PlayerLight", "Intensity", def.intensity)), def.intensity);
		}

		form_label("Offset relative to",
			"World: offset along the map's x / height / z.\n"
			"View: (right, up, forward) against the camera heading, so a forward offset always lights\n"
			"what you are looking at.\n"
			"Character: (right, up, forward) against the way the body faces.\n"
			"View and Character use the heading only; the light never tilts with pitch.");
		ImGui::Combo("##pl_frame", &c.offset_frame, kFrameNames, 3);
		{
			const auto frame = saved_ini("PlayerLight", "OffsetFrame");
			const int saved_frame = frame.empty() ? def.offset_frame : _stricmp(frame.c_str(), "World") == 0 ? 0 : _stricmp(frame.c_str(), "Character") == 0 ? 2 : 1;
			revert_buttons("pl_frame", c.offset_frame, saved_frame, def.offset_frame);
		}
		form_label(c.offset_frame == 0 ? "Offset (x, height, z)" : "Offset (right, up, forward)", "Saved as [PlayerLight] Offset.");
		ImGui::DragFloat3("##pl_offset", c.offset, 0.01f, -5.0f, 5.0f, "%.2f");
		{
			float saved_offset[3];
			saved_float3("PlayerLight", "Offset", def.offset, saved_offset);
			revert_buttons3("pl_offset", c.offset, saved_offset, def.offset);
		}

		{
			float r[3] = {}, f[3] = {};
			const bool cam = shared::common::ffp_state::get().view_axes(r, f) && normalise_xz(f);
			float yaw = 0.0f;
			const bool chr = game::camera::get_char_heading(yaw);
			ImGui::TextDisabled("Heading: camera %s, character %s",
				cam ? std::format("{:.1f} deg", std::atan2(f[0], f[2]) * kRadToDeg).c_str() : "?",
				chr ? std::format("{:.1f} deg", yaw * kRadToDeg).c_str() : "?");
			tip("With the camera straight behind the character the two should match.\n"
				"Character heading is read from the player's position block (+0x04).");
		}

		if (ImGui::Button("Copy the game's values") && player_seen_ && player_src_.has_params)
		{
			// Starting point for an override: the game's own values, used verbatim.
			c.colour[0] = player_src_.r; c.colour[1] = player_src_.g; c.colour[2] = player_src_.b;
			c.range = player_src_.dwindle_end;
			if (player_src_.dwindle_begin > 0.01f) c.radius = player_src_.dwindle_begin;
		}
		tip("Fills Colour, Reach and Emitter size with the game's current values, to start an override from.");
		solo_flash_imgui(KEY_PLAYER);
	}

	void game_lights::list_row_imgui(const light_row& r)
	{
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		const bool pinned = r.kind == kind::sun || r.kind == kind::player;
		if (pinned)
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.4f, 1.0f));
		const uint64_t ident = r.ident();
		row_order_next_.push_back(ident);
		const bool is_selected = selection_.empty() ? selected_ == ident : selection_.count(ident) != 0;
		if (ImGui::Selectable(std::format("{}{}##{:016x}", r.name, solo_ == r.key ? "  [solo]" : "", ident).c_str(),
			is_selected, ImGuiSelectableFlags_SpanAllColumns))
		{
			const auto& io = ImGui::GetIO();
			list_custom_indices_.clear();
			custom_owns_list_ = false;
			if (selection_.empty())
				selection_.insert(selected_);
			if (io.KeyShift)
			{
				// From the anchor to this row, as listed last frame; Ctrl keeps what was selected.
				if (!io.KeyCtrl)
					selection_.clear();
				const auto a = std::find(row_order_.begin(), row_order_.end(), selection_anchor_);
				const auto b = std::find(row_order_.begin(), row_order_.end(), ident);
				if (a != row_order_.end() && b != row_order_.end())
					selection_.insert(a < b ? a : b, (a < b ? b : a) + 1);
				selection_.insert(ident);
				selected_ = ident;
			}
			else if (io.KeyCtrl)
			{
				if (selection_.count(ident) && selection_.size() > 1)
				{
					selection_.erase(ident);
					if (selected_ == ident)
						selected_ = *selection_.begin();
				}
				else
				{
					selection_.insert(ident);
					selected_ = ident;
				}
				selection_anchor_ = ident;
			}
			else
			{
				selection_.clear();
				selection_.insert(ident);
				selected_ = ident;
				selection_anchor_ = ident;
			}
		}
		if (pinned)
			ImGui::PopStyleColor();

		ImGui::TableNextColumn();
		ImGui::TextColored(state_colour(r.state), "%s", state_text(r.state));
		tip(state_help(r.state));

		ImGui::TableNextColumn();
		{
			// Hue at a glance: the drawn colour scaled so its brightest channel is 1.
			const auto* o = find_override(r.key);
			const float* c = o && o->recolour ? o->colour : r.colour;
			const float peak = std::max({ c[0], c[1], c[2], 1e-6f });
			ImGui::ColorButton(std::format("##sw{:016x}", r.ident()).c_str(),
				ImVec4(c[0] / peak, c[1] / peak, c[2] / peak, 1.0f),
				ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(12, 12));
		}
		ImGui::TableNextColumn();
		if (r.kind == kind::sun) ImGui::TextDisabled("-");
		else ImGui::Text("%.1f m", r.distance);
		ImGui::TableNextColumn();
		if (r.colour[0] + r.colour[1] + r.colour[2] > 0.0f)
			ImGui::Text("%.2f %.2f %.2f", r.colour[0], r.colour[1], r.colour[2]);
		else
			ImGui::TextDisabled("-");
		ImGui::TableNextColumn();
		if (r.range_end > 0.0f) ImGui::Text("%.1f-%.1f", r.range_begin, r.range_end);
		else ImGui::TextDisabled("-");
		ImGui::TableNextColumn();
		ImGui::TextDisabled("%s", r.origin.c_str());
	}

	void game_lights::point_light_imgui(const light_row& row)
	{
		ImGui::SeparatorText(row.name.c_str());
		ImGui::TextColored(state_colour(row.state), "%s", state_text(row.state));
		ImGui::SameLine();
		ImGui::TextDisabled("- %s", state_help(row.state));

		// A custom light is set up here in full: its own settings, saved to the map's file.
		if (row.kind == kind::custom)
		{
			auto& lights = game::custom_lights::edit();
			const int index = row.slot - game::custom_lights::kFirstSlot;
			if (index < 0 || index >= static_cast<int>(lights.size()))
				return;
			form_value("Sent to Remix", std::format("radiance {:.2f} {:.2f} {:.2f}", row.radiance[0], row.radiance[1], row.radiance[2]));
			ImGui::Spacing();
			ImGui::PushID("custom_row");
			if (custom_light_fields_imgui(lights[index]))
				game::custom_lights::mark_dirty();
			solo_flash_imgui(row.key);
			ImGui::Spacing();
			custom_light_actions_imgui(index);
			custom_lights_file_buttons();
			ImGui::PopID();
			return;
		}
		form_value("Source", kKindSection[static_cast<int>(row.kind)]);
		form_value("Comes from", row.origin);
		form_value("Position", std::format("{:.2f}, {:.2f}, {:.2f}  ({:.1f} m from the player)",
			row.pos[0], row.pos[1], row.pos[2], row.distance));
		form_value("Game colour", std::format("{:.3f} {:.3f} {:.3f}", row.colour[0], row.colour[1], row.colour[2]));
		form_value("Game reach", std::format("full to {:.2f} m, zero at {:.2f} m", row.range_begin, row.range_end));
		form_value("Sent to Remix", std::format("radiance {:.2f} {:.2f} {:.2f}", row.radiance[0], row.radiance[1], row.radiance[2]));
		if (row.own_key)
		{
			const auto members = std::count_if(rows_.begin(), rows_.end(), [&](const light_row& r) { return r.group_key == row.group_key; });
			if (row.decoupled)
				ImGui::TextColored(ImVec4(0.5f, 0.85f, 1.0f, 1.0f), "This light has its own settings, apart from its group. Saved.");
			else
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
					"Group '%s': changes apply to all of this effect's lights (%d here now, and any others) and are saved.",
					row.group.c_str(), static_cast<int>(members));
			bool own = row.decoupled;
			if (form_checkbox("Edit this light on its own", "##pt_own", &own,
				"Ticked: this light keeps its own settings, starting from the group's. Unticked: it follows the group again\n"
				"and its own settings are dropped. Told apart by position, so this is for lights that stay put."))
			{
				if (own)
				{
					decoupled_.insert(row.own_key);
					if (const auto* group = find_override(row.group_key))
						overrides_[row.own_key] = *group;
				}
				else
				{
					decoupled_.erase(row.own_key);
					overrides_.erase(row.own_key);
				}
				return; // the row's edit key changes with the next frame's list
			}
		}
		else if (!is_static(row.kind))
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
				"Moving light: changes follow its slot for this session and are not saved.");
		ImGui::Spacing();

		auto& o = overrides_[row.key];
		// Saved: light_override.txt's edit (none for a moving light, whose edits are never saved). Default: the game's.
		const light_override none {};
		const auto* saved_edit = find_saved_override(row.key);
		const auto& saved = saved_edit ? *saved_edit : none;
		bool draw = !o.disabled;
		if (form_checkbox("Draw this light", "##pt_draw", &draw, "Switch this one light off or on."))
			o.disabled = !draw;
		revert_buttons("pt_draw", o.disabled, saved.disabled, none.disabled);
		// Always shown for map lights (greyed out where it does nothing), so toggling a light never adds or takes away a row.
		if (row.kind == kind::map_row || row.kind == kind::bank_entity)
		{
			const bool applies = o.include_map || row.state == row_state::characters ||
				(characters_only(row.pos[0], row.pos[1], row.pos[2]) && !game::map_lights::is_flame(row.pos[0], row.pos[1], row.pos[2]));
			ImGui::BeginDisabled(!applies);
			form_checkbox("Light the map as well", "##pt_include", &o.include_map,
				"The game lights only characters with this light; the map around it gets its light from the\n"
				"lightmap. Ticked, Remix draws it as a normal light, which lights the walls and floor too.\n"
				"Greyed out: this light lights the map already.");
			ImGui::EndDisabled();
			revert_buttons("pt_include", o.include_map, saved.include_map, none.include_map);
		}
		form_label("Brightness", "This light only, on top of the source brightness. 0 = off.");
		ImGui::SliderFloat("##pt_mult", &o.multiplier, 0.0f, 10.0f, "x %.2f", ImGuiSliderFlags_Logarithmic);
		revert_buttons("pt_mult", o.multiplier, saved.multiplier, none.multiplier);
		form_label("Move", row.group_key && !row.decoupled
			? "Metres from where the game puts it (x, height, z). Shared by every light of its group (all of this effect's).\nSaved with Save."
			: "Metres from where the game puts it (x, height, z). Saved with Save.");
		ImGui::DragFloat3("##pt_offset", o.offset, 0.01f, -20.0f, 20.0f, "%.2f");
		revert_buttons3("pt_offset", o.offset, saved.offset, none.offset);
		if (form_checkbox("Own colour", "##pt_recolour", &o.recolour, "Replace the game's colour with your own.") && o.recolour)
			std::memcpy(o.colour, row.colour, sizeof(o.colour));
		if (revert_buttons("pt_recolour", o.recolour, saved.recolour, none.recolour) && o.recolour)
			std::memcpy(o.colour, saved.colour, sizeof(o.colour));
		// Shown (greyed out with the game's colour) while 'Own colour' is off, so the rows below stay put.
		form_label("Colour", nullptr);
		if (o.recolour)
		{
			ImGui::ColorEdit3("##pt_colour", o.colour, ImGuiColorEditFlags_Float);
			revert_buttons3("pt_colour", o.colour, saved.recolour ? saved.colour : nullptr, row.colour);
		}
		else
		{
			float game_colour[3] = { row.colour[0], row.colour[1], row.colour[2] };
			ImGui::BeginDisabled();
			ImGui::ColorEdit3("##pt_colour", game_colour, ImGuiColorEditFlags_Float);
			ImGui::EndDisabled();
		}
		solo_flash_imgui(row.key);

		if (ImGui::Button("Copy"))
		{
			clipboard_ = o;
			have_clipboard_ = true;
		}
		tip("Copies this light's switch, brightness, colour and move, to paste onto others.");
		ImGui::SameLine();
		ImGui::BeginDisabled(!have_clipboard_);
		if (ImGui::Button("Paste"))
			o = clipboard_;
		ImGui::EndDisabled();
		tip("Gives this light the copied switch, brightness, colour and move.");
		ImGui::SameLine();
		if (ImGui::Button("Duplicate"))
			duplicate_as_custom(row);
		tip("Makes a custom light for this map at this light's spot, with its colour and reach on the game's falloff,\n"
			"to move or change on its own. Save it under 'Custom lights for this map'.");

		if (row.kind == kind::custom)
		{
			if (ImGui::Button("Edit position and aim"))
			{
				select_custom(row.slot - game::custom_lights::kFirstSlot);
				open_custom_ = true;
			}
			tip("Opens this light under 'Custom lights for this map'.");
		}

		// Stand-ins also belong to a category, which scales every light made from the same effect across the game.
		if (row.kind == kind::synthetic)
		{
			std::string name, category;
			if (game::map_lights::describe(row.slot, name, category))
			{
				const auto& categories = game::map_lights::categories();
				for (size_t i = 0; i < categories.size(); ++i)
				{
					if (categories[i].name != category)
						continue;
					bool on = categories[i].enabled;
					bool follows = categories[i].follows_effect;
					float intensity = categories[i].intensity;
					float radius = categories[i].radius;
					ImGui::SeparatorText(std::format("Category '{}' ({} on this map), on every map", category, categories[i].count).c_str());
					// Saved: categories.txt. Default: the map tables' own values (1, 1, on); following the effect has no default
					// but the file's.
					const game::map_lights::category plain;
					const auto* saved_cat = game::map_lights::saved_category(category);
					bool changed = form_checkbox("Category on", "##cat_on", &on, nullptr);
					changed |= revert_buttons("cat_on", on, saved_cat ? &saved_cat->enabled : nullptr, &plain.enabled);
					changed |= form_checkbox("Follows its effect", "##cat_follows", &follows,
						"Each light follows its effect's sprites drawn at its spot, as bright as they are opaque,\n"
						"for effects that come and go (Tomb of the Giants' lantern halos). Off: always lit.");
					changed |= revert_buttons("cat_follows", follows, saved_cat ? &saved_cat->follows_effect : nullptr, static_cast<const bool*>(nullptr));
					form_label("Category brightness", nullptr);
					changed |= ImGui::DragFloat("##cat_intensity", &intensity, 0.01f, 0.0f, 20.0f, "x %.2f");
					changed |= revert_buttons("cat_intensity", intensity, saved_cat ? &saved_cat->intensity : nullptr, &plain.intensity);
					form_label("Category reach", nullptr);
					changed |= ImGui::DragFloat("##cat_radius", &radius, 0.01f, 0.05f, 10.0f, "x %.2f");
					changed |= revert_buttons("cat_radius", radius, saved_cat ? &saved_cat->radius : nullptr, &plain.radius);
					if (changed)
						game::map_lights::set_category(i, intensity, radius, on, follows);
					if (categories[i].follows_effect)
						ImGui::TextDisabled("Effect: sprite alpha %.3f at this light, full %.3f (highest seen %.3f), level %.2f",
							game::map_lights::effect_sprite_alpha(row.slot), categories[i].effect_alpha,
							categories[i].effect_alpha_seen, game::map_lights::effect_level(row.slot));
				}
			}
		}
	}

	void game_lights::diagnostics_imgui()
	{
		if (game::point_lights::is_available())
		{
			ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f),
				"KPointLightMan: %d slots live (%d bank row, %d entity, %d unresolved)",
				game::point_lights::slots_live(), game::point_lights::slots_from_bank(),
				game::point_lights::slots_from_entity(), game::point_lights::slots_unresolved());
			if (game::point_lights::resolve_entities() && ImGui::TreeNode("Entity table walk"))
			{
				ImGui::TextUnformatted(game::point_lights::chain_diagnostics().c_str());
				ImGui::TreePop();
			}
		}
		else
		{
			ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "KPointLightMan: not reachable (menu / world not loaded)");
		}

		ImGui::TextDisabled("Stand-in table: 0x%08X %s: %s", game::map_lights::current_map_id(),
			game::map_lights::current_map_name().empty() ? "no map" : game::map_lights::current_map_name().c_str(),
			game::map_lights::status().c_str());
		if (ImGui::Button("Reload stand-in table"))
		{
			game::map_lights::reload();
			destroy_all_handles();
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset stand-in categories"))
			game::map_lights::reset_categories();
		ImGui::SameLine();
		if (ImGui::Button("Dump harvested lights to log"))
		{
			const auto path = game::point_lights::dump_to_log();
			log("GameLights", path.empty()
				? std::string("Could not write the light dump")
				: std::format("Light dump appended to {}", path),
				path.empty() ? LOG_TYPE::LOG_TYPE_ERROR : LOG_TYPE::LOG_TYPE_STATUS, true);
		}
		form_checkbox("Show light count HUD", "##hud", &show_pos_hud_, nullptr);
		form_label("Move every point light by", "A test offset added to every point light's position.");
		ImGui::DragFloat3("##manual_offset", manual_offset_, 1.0f, -50000.0f, 50000.0f, "%.2f");

		if (form_checkbox("Probe light mode", "##probe", &probe_light_mode_, "Replaces the lights with one test light placed by hand."))
			destroy_all_handles();
		if (probe_light_mode_)
		{
			form_label("Probe position", nullptr);
			ImGui::DragFloat3("##probe_pos", probe_position_, 1.0f, -50000.0f, 50000.0f, "%.2f");
			form_label("Probe emitter size", nullptr);
			ImGui::SliderFloat("##probe_radius", &probe_radius_, 0.01f, 10.0f, "%.2f m");
			form_label("Probe radiance", nullptr);
			ImGui::SliderFloat("##probe_radiance", &probe_radiance_, 0.01f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		}
	}

	void game_lights::draw_pos_hud()
	{
		if (!show_pos_hud_)
			return;

		ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowBgAlpha(0.6f);
		if (ImGui::Begin("##pos_hud", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoMove))
		{
			ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
				"Lights: %d live / %d harvested", active_light_count(), total_light_count());
		}
		ImGui::End();
	}
}
