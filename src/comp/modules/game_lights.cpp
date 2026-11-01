#include "std_include.hpp"
#include "game_lights.hpp"
#include "../game/point_lights.hpp"
#include "../game/map_lights.hpp"
#include "../game/effect_lights.hpp"
#include "../game/ffx_names.hpp"
#include "../game/ffx_lights.hpp"
#include "../game/custom_lights.hpp"
#include "../game/sun_light.hpp"
#include "../game/atmosphere.hpp"
#include "../game/camera.hpp"
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
		constexpr float kRadToDeg = 57.2957795f;

		// Short source name per kind (switches, the selected light's heading), in section order.
		const char* const kKindSource[] = {
			"Sun", "Player", "Map", "Map entity", "Character", "Effect", "Stand-in", "Effect stand-in", "Unlit effect",
			"Custom", "No parameters",
		};
		static_assert(std::size(kKindSource) == static_cast<size_t>(kind::count));

		// Section heading per kind in the grouped list.
		const char* const kKindSection[] = {
			"Sun", "Player",
			"Map lights (MSB Light events)",
			"Map lights (bank entities)",
			"Character lights",
			"Effect lights (spells, items, SFX)",
			"Stand-ins (synthesized for unlit flames)",
			"Effect stand-ins (lightning bolts)",
			"Unlit effects (lights from the effect table)",
			"Custom lights (rtx_comp\\lights\\<map>.custom.txt)",
			"Engine slots without parameters (never drawn)",
		};
		static_assert(std::size(kKindSection) == static_cast<size_t>(kind::count));

		// State column text and colour.
		const char* state_text(game_lights::row_state s)
		{
			using st = game_lights::row_state;
			switch (s)
			{
			case st::on: return "drawn";
			case st::rejected: return "Remix refused";
			case st::lights_off: return "off";
			case st::no_params: return "no parameters";
			case st::effect_off: return "effect not shown";
			case st::source_off: return "source off";
			case st::disabled: return "switched off";
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
				return string_hash64(va("ffx:%d", l.ffx_id)) | 0x8000000000000000ull;
			const uint64_t h = is_static(k)
				? string_hash64(va("pos:%d:%ld:%ld:%ld", static_cast<int>(k),
					std::lround(l.x * 20.0f), std::lround(l.y * 20.0f), std::lround(l.z * 20.0f)))
				: string_hash64(va("slot:%d:%d", static_cast<int>(k), l.slot));
			// Top bit: never KEY_SUN / KEY_PLAYER. Next bit: a session-only key, which
			// serialize() and save_all() skip.
			return h | 0x8000000000000000ull | (is_static(k) ? 0ull : kSessionKeyBit);
		}

		kind classify(const game::point_lights::light& l)
		{
			using src = game::point_lights::param_source;
			if (l.source == src::synthetic) return kind::synthetic;
			if (l.source == src::effect) return kind::effect;
			if (l.source == src::ffx_table) return kind::ffx_table;
			if (l.source == src::custom) return kind::custom;
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

		const auto& cfg = shared::common::config::get();
		enabled = cfg.lights.enabled;
		intensity_scale = cfg.lights.intensity_scale;
		fire_light_scale = cfg.lights.fire_light_scale;
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
		saved_text_ = serialize();
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
		if (flash_selected_ && (ident ? ident : key) == selected_)
			m *= kFlashMultiplier;
		return m;
	}

	void game_lights::observe_lightmapped_draw(const float* ps_const, int light_count)
	{
		float ox = 0.0f, oy = 0.0f, oz = 0.0f;
		if (!ps_const || !shared::common::ffp_state::get().world_offset(ox, oy, oz))
			return;
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
			fprintf(f, "kind %d state %d slot %d %s | pos %.2f %.2f %.2f dist %.1f | colour %.3f %.3f %.3f range %.2f-%.2f | radiance %.3f %.3f %.3f %s\n",
				static_cast<int>(r.kind), static_cast<int>(r.state), r.slot, r.name.c_str(), r.pos[0], r.pos[1], r.pos[2], r.distance,
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
		case kind::ffx_table:
		{
			const char* effect = game::ffx_names::name(l.ffx_id);
			row.name = std::format("{} (FFX {})", effect ? effect : "Effect", l.ffx_id);
			row.origin = std::format("ffx_lights.tsv: the developers' effect-light rule on FFX {}", l.ffx_id);
			if (const auto* e = game::ffx_lights::find(l.ffx_id))
			{
				row.group = pretty_category(e->category);
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
		game::effect_lights::set_covers(sources);
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
		if (kind_enabled_[static_cast<int>(kind::ffx_table)])
		{
			std::vector<const game::point_lights::light*> map_lights;
			for (const auto* s : sources)
			{
				const kind k = classify(*s);
				if ((k == kind::map_row || k == kind::bank_entity || k == kind::synthetic) && s->has_params)
					map_lights.push_back(s);
			}
			for (const auto& l : game::ffx_lights::collect(map_lights))
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
					characters_only(src.x, src.y, src.z) ? row_state::characters
				: row_state::on;
			if (row.state != row_state::on)
			{
				rows_.push_back(std::move(row));
				continue;
			}
			++active;

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
					row.kind == kind::custom ||
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
			const remixapi_Float3D position = {
				src.x + manual_offset_[0],
				src.y + manual_offset_[1],
				src.z + manual_offset_[2],
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
	void game_lights::sync_sun()
	{
		const auto& sun = game::sun_light::current(shared::common::ffp_state::get().frame_count(),
			sun_fallback_colour);

		float ambient[3] = {};
		const float facing_sun[3] = { -sun.key.dir[0], -sun.key.dir[1], -sun.key.dir[2] };
		game::atmosphere::ambient_toward(facing_sun, ambient);
		game::atmosphere::set_sun_gamma(ambient, sun.key.colour);
		float linear[3];
		for (int c = 0; c < 3; ++c)
		{
			const float base = std::max(ambient[c], 0.0f);
			linear[c] = std::pow(base + std::max(sun.key.colour[c], 0.0f), 2.2f) - std::pow(base, 2.2f);
		}

		const float mult = sun_intensity_scale * edit_multiplier(KEY_SUN);
		remixapi_Float3D radiance = {
			linear[0] * mult,
			linear[1] * mult,
			linear[2] * mult,
		};
		const bool lit = radiance.x > 0.0f || radiance.y > 0.0f || radiance.z > 0.0f;

		if (!sun_enabled || !sun.valid || !lit)
		{
			release_sun();
			return;
		}

		const remixapi_Float3D direction = { sun.key.dir[0], sun.key.dir[1], sun.key.dir[2] };
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
			const float scale = c.falloff_intensity * mult / 3.14159265f;
			radiance = {
				std::pow(std::max(colour[0], 0.0f), 2.2f) * scale,
				std::pow(std::max(colour[1], 0.0f), 2.2f) * scale,
				std::pow(std::max(colour[2], 0.0f), 2.2f) * scale,
			};
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
			return;
		}

		// With the point lights off the list still fills, and the player light,
		// which has its own toggle, still runs.
		sync_lights(enabled);
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
	bool game_lights::load_overrides()
	{
		overrides_.clear();
		std::ifstream file(overrides_path());
		if (!file)
			return false;

		std::string line;
		while (std::getline(file, line))
		{
			if (line.empty() || line[0] == '#')
				continue;
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
			overrides_[std::stoull(key_text, nullptr, 16)] = o;
		}
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
			s += std::format("{:016x} {} {:.3f} {} {} {}\n", key, o->disabled ? 1 : 0, o->multiplier,
				o->recolour ? 1 : 0, format_float3(o->colour), o->include_map ? 1 : 0);
		return s;
	}

	bool game_lights::save_all()
	{
		auto& cfg = shared::common::config::get();
		auto flag = [](bool v) { return std::string(v ? "1" : "0"); };
		bool ok = true;

		ok &= cfg.set_string("Lights", "Enabled", flag(enabled));
		ok &= cfg.set_string("Lights", "IntensityScale", std::format("{:.3f}", intensity_scale));
		ok &= cfg.set_string("Lights", "FireLightScale", std::format("{:.3f}", fire_light_scale));
		ok &= cfg.set_string("Lights", "RadiusScale", std::format("{:.3f}", radius_scale));
		ok &= cfg.set_string("Lights", "GameFalloff", game_falloff_ ? "1" : "0");
		ok &= cfg.set_string("Lights", "SkipCharacterOnly", skip_characters_only_ ? "1" : "0");
		ok &= cfg.set_string("Lights", "ResolveEntityParams", flag(game::point_lights::resolve_entities()));
		ok &= cfg.set_string("Lights", "SynthesizedLights", flag(game::map_lights::enabled()));

		ok &= cfg.set_string("Sun", "Enabled", flag(sun_enabled));
		ok &= cfg.set_string("Sun", "IntensityScale", std::format("{:.3f}", sun_intensity_scale));
		ok &= cfg.set_string("Sun", "AngularDiameter", std::format("{:.3f}", sun_angular_diameter));
		ok &= cfg.set_string("Sun", "FallbackColour", format_float3(sun_fallback_colour));

		const auto& p = player_cfg_;
		ok &= cfg.set_string("PlayerLight", "Enabled", flag(p.enabled));
		ok &= cfg.set_string("PlayerLight", "UseGameColour", flag(p.use_game_colour));
		ok &= cfg.set_string("PlayerLight", "Colour", format_float3(p.colour));
		ok &= cfg.set_string("PlayerLight", "UseGameRange", flag(p.use_game_range));
		ok &= cfg.set_string("PlayerLight", "Range", std::format("{:.3f}", p.range));
		ok &= cfg.set_string("PlayerLight", "UseGameRadius", flag(p.use_game_radius));
		ok &= cfg.set_string("PlayerLight", "Radius", std::format("{:.3f}", p.radius));
		ok &= cfg.set_string("PlayerLight", "Intensity", std::format("{:.3f}", p.intensity));
		ok &= cfg.set_string("PlayerLight", "FalloffIntensity", std::format("{:.3f}", p.falloff_intensity));
		ok &= cfg.set_string("PlayerLight", "FalloffRangeScale", std::format("{:.3f}", p.falloff_range_scale));
		ok &= cfg.set_string("PlayerLight", "OffsetFrame", kFrameNames[std::clamp(p.offset_frame, 0, 2)]);
		ok &= cfg.set_string("PlayerLight", "Offset", format_float3(p.offset));

		// Overrides of static lights, with the row name as a comment for hand editing.
		{
			const auto path = overrides_path();
			std::error_code ec;
			std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
			std::ofstream file(path, std::ios::trunc);
			if (file)
			{
				file << "# Per-light edits from F4 -> Lights. key disabled multiplier recolour r g b include_map  name\n";
				for (const auto& [key, o] : overrides_)
				{
					if (o.is_default() || (key & kSessionKeyBit) != 0)
						continue;
					std::string name;
					for (const auto& r : rows_)
						if (r.key == key) name = r.name;
					file << std::format("{:016x} {} {:.3f} {} {:.3f} {:.3f} {:.3f} {}  {}\n", key, o.disabled ? 1 : 0,
						o.multiplier, o.recolour ? 1 : 0, o.colour[0], o.colour[1], o.colour[2], o.include_map ? 1 : 0, name);
				}
			}
			else
			{
				ok = false;
			}
		}

		ok &= game::map_lights::save_categories();

		saved_text_ = serialize();
		save_status_ = ok ? "saved" : "save FAILED (see console)";
		return ok;
	}

	void game_lights::revert_to_saved()
	{
		auto& cfg = shared::common::config::get();
		enabled = cfg.get_bool("Lights", "Enabled", true);
		intensity_scale = cfg.get_float("Lights", "IntensityScale", 1.0f);
		fire_light_scale = std::max(0.0f, cfg.get_float("Lights", "FireLightScale", 1.0f));
		radius_scale = cfg.get_float("Lights", "RadiusScale", 1.0f);
		game_falloff_ = cfg.get_bool("Lights", "GameFalloff", true);
		skip_characters_only_ = cfg.get_bool("Lights", "SkipCharacterOnly", true);
		game::point_lights::set_resolve_entities(cfg.get_bool("Lights", "ResolveEntityParams", true));
		game::map_lights::set_enabled(cfg.get_bool("Lights", "SynthesizedLights", true));

		sun_enabled = cfg.get_bool("Sun", "Enabled", true);
		sun_intensity_scale = cfg.get_float("Sun", "IntensityScale", 1.0f);
		sun_angular_diameter = cfg.get_float("Sun", "AngularDiameter", 0.53f);
		cfg.get_float3("Sun", "FallbackColour", sun_fallback_colour);

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
		const auto frame = cfg.get_string("PlayerLight", "OffsetFrame", "View");
		p.offset_frame = _stricmp(frame.c_str(), "World") == 0 ? 0 : _stricmp(frame.c_str(), "Character") == 0 ? 2 : 1;
		p.offset[0] = p.offset[1] = p.offset[2] = 0.0f;
		cfg.get_float3("PlayerLight", "Offset", p.offset);

		load_overrides();
		solo_ = 0;
		flash_selected_ = false;
		saved_text_ = serialize();
		save_status_ = "reverted to saved";
	}

	// ------------------------------------------------------------------ UI

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
			"Engine slots with no colour or reach. Never drawn.",
		};
		static_assert(std::size(kKindHelp) == static_cast<size_t>(kind::count));
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
		tip("Writes [Lights], [Sun] and [PlayerLight] to remix-comp-proxy.ini, per-light edits to\n"
			"rtx_comp\\lights\\light_overrides.txt and the stand-in categories to categories.txt.\n"
			"Custom lights have their own Save button.");
		ImGui::SameLine();
		if (ImGui::Button("Revert to saved"))
			revert_to_saved();
		tip("Throws away unsaved changes and reloads the saved settings and per-light edits.");
		ImGui::SameLine();
		if (ImGui::Button("Reset per-light edits"))
		{
			overrides_.clear();
			solo_ = 0;
			flash_selected_ = false;
		}
		tip("Clears every light's own switch, brightness and colour, and any solo. Saved only with Save.");
		ImGui::SameLine();
		if (serialize() != saved_text_)
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "unsaved changes");
		else
			ImGui::TextDisabled("%s", save_status_.empty() ? "no changes" : save_status_.c_str());
		ImGui::Spacing();

		const bool open_all = shared::globals::imgui_open_light_sections;
		shared::globals::imgui_open_light_sections = false;
		auto header = [&](const char* label, ImGuiTreeNodeFlags flags = 0)
		{
			if (open_all)
				ImGui::SetNextItemOpen(true);
			return ImGui::CollapsingHeader(label, flags);
		};
		if (header("Light sources"))
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
	}

	void game_lights::sources_imgui()
	{
		ImGui::Checkbox("Draw point lights", &enabled);
		tip("Every light in the list except the sun and the player light, which have their own switch below.");
		ImGui::SameLine(0.0f, ImGui::GetFontSize() * 2.0f);
		ImGui::Checkbox("Use the game's falloff", &game_falloff_);
		tip("For the sources ticked under 'Game falloff': full strength out to dwindle begin, then linearly\n"
			"to zero at dwindle end, as the game lights them. Off: inverse square for every light.");

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
		if (ImGui::BeginTable("##sources", 4, flags, ImVec2(ImGui::GetFontSize() * 46.0f, 0.0f)))
		{
			ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Draw");
			ImGui::TableSetupColumn("Game falloff");
			ImGui::TableSetupColumn("Drawn now");
			ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
			ImGui::TableNextColumn();
			ImGui::TableHeader("Source");
			tip("Hover a source for what it is.");
			ImGui::TableNextColumn();
			ImGui::TableHeader("Draw");
			tip("Saved with Save: Sun, Player and Stand-ins. The other switches last for this session.");
			ImGui::TableNextColumn();
			ImGui::TableHeader("Game falloff");
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
				if (lk == kind::sun)
					ImGui::Checkbox(draw_id.c_str(), &sun_enabled);
				else if (lk == kind::player)
					ImGui::Checkbox(draw_id.c_str(), &player_cfg_.enabled);
				else if (lk == kind::synthetic)
				{
					// Stand-ins are also not built at all while off ([Lights] SynthesizedLights, saved).
					bool on = game::map_lights::enabled() && kind_enabled_[k];
					if (ImGui::Checkbox(draw_id.c_str(), &on))
					{
						game::map_lights::set_enabled(on);
						kind_enabled_[k] = on;
					}
				}
				else if (lk == kind::unresolved)
					ImGui::TextDisabled("-");
				else
					ImGui::Checkbox(draw_id.c_str(), &kind_enabled_[k]);

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
			"them, so they start off; each can be switched on in its own settings ('Light the map as well').\n"
			"Untick to draw them all. Saved as [Lights] SkipCharacterOnly.");
		bool resolve = game::point_lights::resolve_entities();
		if (form_checkbox("Read entity light colours", "##resolve", &resolve,
			"Effect, character and bank-entity lights keep their colour on the light entity, not on a map row.\n"
			"Off: only map lights (and the stand-ins) get a colour and reach. Saved as [Lights] ResolveEntityParams."))
			game::point_lights::set_resolve_entities(resolve);
	}

	void game_lights::scales_imgui()
	{
		ImGui::TextDisabled("For every light in the list. The sun and the player light have their own settings (select them).");
		form_label("Brightness", "Every point light but the custom Remix lights, which keep their own units.\n"
			"1 = the tuned default. Saved as [Lights] IntensityScale.");
		ImGui::SliderFloat("##intensity", &intensity_scale, 0.01f, 50.0f, "x %.2f", ImGuiSliderFlags_Logarithmic);
		form_label("Fire stand-in brightness",
			"Lights stood in for fire effects the game gives no light (fire breath, fire spells).\n"
			"1 = torch to bonfire strength. Saved as [Lights] FireLightScale.");
		ImGui::SliderFloat("##fire", &fire_light_scale, 0.0f, 10.0f, "x %.2f", ImGuiSliderFlags_Logarithmic);
		form_label("Reach", "How far every point light reaches: its dwindle begin and end times this (not the custom Remix\n"
			"lights). Saved as [Lights] RadiusScale.");
		ImGui::SliderFloat("##reach", &radius_scale, 0.1f, 10.0f, "x %.2f");
		form_label("Emitter size", "Radius of the sphere Remix shines each point light from (custom Remix lights have their\n"
			"own). Bigger = softer shadows, and a big sphere next to a wall lights it as a bright disc. This session.");
		ImGui::SliderFloat("##emitter", &emitter_radius_, 0.01f, 2.0f, "%.2f m");
		form_label("Most lights drawn", "Lights past this count are listed as 'over the limit'. This session.");
		ImGui::SliderInt("##max", &max_active_lights_, 0, 512, max_active_lights_ == 0 ? "no limit" : "%d");
	}

	void game_lights::table_imgui()
	{
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
		ImGui::InputTextWithHint("##filter", "Filter by name or details", filter_, sizeof(filter_));
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
		sun_row.key = KEY_SUN;
		sun_row.kind = kind::sun;
		sun_row.name = "Sun";
		{
			static const char* kDirSource[] = { "no direction yet", "shadow light c175", "map light 0 c92", "character light 0 c92" };
			sun_row.origin = kDirSource[static_cast<int>(sun.dir_from)];
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
			ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
		if (ImGui::BeginTable("##light_list", 7, flags, ImVec2(0.0f, ImGui::GetFontSize() * 22.0f)))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Light", ImGuiTableColumnFlags_WidthStretch, 2.0f);
			const float em = ImGui::GetFontSize();
			ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, em * 9.0f);
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 14.0f);
			ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthFixed, em * 4.5f);
			ImGui::TableSetupColumn("Game colour", ImGuiTableColumnFlags_WidthFixed, em * 9.0f);
			ImGui::TableSetupColumn("Reach (m)", ImGuiTableColumnFlags_WidthFixed, em * 5.5f);
			ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 3.0f);
			ImGui::TableHeadersRow();

			list_row_imgui(sun_row);
			list_row_imgui(player_row);

			// A collapsible heading row with how many of its lights are drawn.
			auto section = [](const std::string& label, const light_row* const* first, const light_row* const* last,
				bool open_by_default, const char* id)
			{
				const auto on = std::count_if(first, last, [](const light_row* r) { return r->state == row_state::on; });
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				return ImGui::TreeNodeEx(std::format("{}  ({} of {} drawn)###{}", label, on, last - first, id).c_str(),
					ImGuiTreeNodeFlags_SpanAllColumns | (open_by_default ? ImGuiTreeNodeFlags_DefaultOpen : 0));
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
						std::format("kind{}", static_cast<int>(k)).c_str()))
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
								std::format("group{}{}", static_cast<int>(k), rows[g]->group).c_str()))
							{
								for (size_t r = g; r < g_end; ++r)
									list_row_imgui(*rows[r]);
								ImGui::TreePop();
							}
							g = g_end;
						}
						ImGui::TreePop();
					}
					i = end;
				}
			}
			ImGui::EndTable();
		}
	}

	void game_lights::selected_imgui()
	{
		ImGui::Spacing();
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

	// Solo and flash work on every light, the pinned ones included.
	void game_lights::solo_flash_imgui(uint64_t key)
	{
		bool solo = solo_ == key;
		if (form_checkbox("Solo", "##solo", &solo, "Draw only this light, to see what it lights. Every other light shows 'hidden by solo'."))
			solo_ = solo ? key : 0;
		form_checkbox("Flash to find it", "##flash", &flash_selected_, "Makes the selected light 20 times brighter while ticked.");
	}

	void game_lights::custom_lights_imgui()
	{
		auto& lights = game::custom_lights::edit();
		const std::string& path = game::custom_lights::path();
		if (path.empty())
		{
			ImGui::TextDisabled("No map loaded.");
			return;
		}
		ImGui::TextWrapped("Lights you place yourself. They are saved to this map's own file and load only while this map is "
			"the current one, so a light never shows up in another map.");
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
			custom_selected_ = static_cast<int>(lights.size()) - 1;
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
		if (ImGui::Button("Save to file"))
			game::custom_lights::save();
		tip("Writes this map's custom lights to its file.");
		ImGui::SameLine();
		if (ImGui::Button("Reload file"))
		{
			game::custom_lights::reload();
			custom_selected_ = -1;
		}
		tip("Throws away unsaved custom light changes and reads the file again.");

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
				if (ImGui::Selectable(std::format("{}##custom{}", l.name, i).c_str(), custom_selected_ == i,
					ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
					custom_selected_ = i;
				ImGui::TableNextColumn();
				changed |= ImGui::Checkbox(std::format("##custom_on{}", i).c_str(), &l.enabled);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(l.spot ? (l.game_falloff ? "spotlight, game falloff" : "spotlight")
					: (l.game_falloff ? "point, game falloff" : "point"));
				ImGui::TableNextColumn();
				ImGui::Text("%.1f, %.1f, %.1f", l.pos[0], l.pos[1], l.pos[2]);
			}
			ImGui::EndTable();
		}

		if (custom_selected_ >= 0 && custom_selected_ < static_cast<int>(lights.size()))
		{
			auto& l = lights[custom_selected_];
			ImGui::SeparatorText(l.name.c_str());
			char name[64] = {};
			std::snprintf(name, sizeof(name), "%s", l.name.c_str());
			form_label("Name", "One word: spaces become underscores in the file.");
			if (ImGui::InputText("##cl_name", name, sizeof(name)))
			{
				l.name = name;
				changed = true;
			}
			changed |= form_checkbox("Switched on", "##cl_on", &l.enabled, nullptr);
			form_label("Position (x, height, z)", "The game's world coordinates for this map.");
			changed |= ImGui::DragFloat3("##cl_pos", l.pos, 0.02f, 0.0f, 0.0f, "%.2f");
			ImGui::SameLine();
			if (ImGui::Button("Move to camera") && have_cam)
			{
				std::copy(std::begin(cam), std::end(cam), std::begin(l.pos));
				changed = true;
			}
			form_label("Colour", nullptr);
			changed |= ImGui::ColorEdit3("##cl_colour", l.colour);
			changed |= form_checkbox("Use the game's falloff", "##cl_game", &l.game_falloff,
				"Off: a plain Remix sphere light (physical inverse-square falloff, Remix's units).\n"
				"On: lit as the game lights its own: full strength out to a distance, then linearly to zero.");
			if (!l.game_falloff)
			{
				form_label("Intensity", "Radiance on top of the colour, in Remix's units (as the Remix toolkit's light Intensity).");
				changed |= ImGui::DragFloat("##cl_rintensity", &l.remix_intensity, 0.1f, 0.0f, 100000.0f, "%.2f",
					ImGuiSliderFlags_Logarithmic);
				form_label("Exposure", "Radiance x 2^exposure, in stops (as the toolkit's Exposure).");
				changed |= ImGui::DragFloat("##cl_exposure", &l.exposure, 0.05f, -20.0f, 20.0f, "%.2f EV");
				form_label("Radius", "The emitting sphere. Bigger = softer shadows; it also sets how bright the light looks up close.");
				changed |= ImGui::DragFloat("##cl_radius", &l.radius, 0.005f, 0.001f, 10.0f, "%.3f m");
				form_label("Volumetric scale", "How strongly it lights Remix's volumetric fog (1 = as bright as on surfaces).");
				changed |= ImGui::DragFloat("##cl_volumetric", &l.volumetric, 0.01f, 0.0f, 10.0f, "%.2f");
			}
			else
			{
				form_label("Intensity", "Brightness on top of the colour, as the game's light rows give it: 1 = the colour as is\n"
					"(the file stores it as a percentage).");
				changed |= ImGui::DragFloat("##cl_intensity", &l.intensity, 0.01f, 0.0f, 50.0f, "%.2f");
				form_label("Full strength out to", "Dwindle begin: the light is at full strength out to here...");
				changed |= ImGui::DragFloat("##cl_begin", &l.dwindle_begin, 0.05f, 0.0f, l.dwindle_end, "%.2f m");
				form_label("Fades to zero at", "... and fades linearly to nothing at this distance (dwindle end).");
				changed |= ImGui::DragFloat("##cl_end", &l.dwindle_end, 0.05f, 0.05f, 200.0f, "%.2f m");
			}
			changed |= form_checkbox("Spotlight", "##cl_spot", &l.spot, "Shines in a cone instead of all around.");
			if (l.spot)
			{
				form_label("Aim direction", "The direction the cone points, (x, height, z); it does not need to be normalised.");
				changed |= ImGui::DragFloat3("##cl_dir", l.dir, 0.01f, -1.0f, 1.0f, "%.3f");
				ImGui::SameLine();
				if (ImGui::Button("Aim along camera") && have_axes)
				{
					std::copy(std::begin(forward), std::end(forward), std::begin(l.dir));
					changed = true;
				}
				form_label("Cone half angle", "From the aim direction to the cone's edge.");
				changed |= ImGui::SliderFloat("##cl_cone", &l.cone_deg, 1.0f, 89.0f, "%.1f deg");
				form_label("Cone edge softness", "0 = a hard edge, 1 = fades over the whole cone.");
				changed |= ImGui::SliderFloat("##cl_soft", &l.softness, 0.0f, 1.0f, "%.2f");
				if (!l.game_falloff)
				{
					form_label("Focus", "Concentrates the light towards the aim within the cone (the toolkit's focus exponent; 0 = even).");
					changed |= ImGui::DragFloat("##cl_focus", &l.focus, 0.05f, 0.0f, 100.0f, "%.2f");
				}
			}
			if (ImGui::Button("Delete this light"))
			{
				lights.erase(lights.begin() + custom_selected_);
				custom_selected_ = -1;
				changed = true;
			}
		}
		if (changed)
			game::custom_lights::mark_dirty();
	}

	void game_lights::sun_imgui()
	{
		const auto& sun = game::sun_light::current(shared::common::ffp_state::get().frame_count(),
			sun_fallback_colour);

		ImGui::SeparatorText("Sun (distant light)");
		if (!sun.valid)
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "No draw has given a sun direction for %u frames.", sun.frames_since_seen);
		else if (sun_handle_)
			ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Drawn.");
		else
			ImGui::TextDisabled("Not drawn.");
		tip("Direction: the game's shadow light (pixel shader c175, ShadowBank), which every shadowed\n"
			"shader reads; else directional light 0 (c92). Colour: light 0 (c95) from a HemDir3 draw, else the\n"
			"irradiance probe's sun, else the last one seen, else the fallback colour. Each frame the draws\n"
			"reading them vote, weighted by primitive count.");

		form_checkbox("Draw the sun", "##sun_on", &sun_enabled, "Saved as [Sun] Enabled.");
		form_label("Brightness", "1 = the game's. Saved as [Sun] IntensityScale.");
		ImGui::SliderFloat("##sun_int", &sun_intensity_scale, 0.0f, 20.0f, "x %.2f", ImGuiSliderFlags_Logarithmic);
		form_label("Size in the sky", "Angular diameter: bigger = softer shadows. The real sun is 0.53 deg. Saved as [Sun] AngularDiameter.");
		ImGui::SliderFloat("##sun_size", &sun_angular_diameter, 0.05f, 10.0f, "%.2f deg");
		form_label("Colour when the game gives none", "Used while no draw supplies a sun colour. Saved as [Sun] FallbackColour.");
		ImGui::ColorEdit3("##sun_fallback", sun_fallback_colour, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		solo_flash_imgui(KEY_SUN);

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

		form_checkbox("Draw the player light", "##pl_on", &c.enabled, "Saved as [PlayerLight] Enabled.");
		form_checkbox("Use the game's colour", "##pl_gc", &c.use_game_colour, "Saved as [PlayerLight] UseGameColour.");
		if (!c.use_game_colour)
		{
			form_label("Colour", "Saved as [PlayerLight] Colour.");
			ImGui::ColorEdit3("##pl_colour", c.colour, ImGuiColorEditFlags_Float);
		}

		if (game_falloff)
		{
			form_label("Reach", "The game's dwindle begin and end, both times this (1 = the game's reach,\n"
				"which changes by area). Saved as [PlayerLight] FalloffRangeScale.");
			ImGui::SliderFloat("##pl_reach", &c.falloff_range_scale, 0.1f, 4.0f, "x %.2f", ImGuiSliderFlags_Logarithmic);
			form_label("Emitter size", "Radius of the sphere the light shines from. Saved as [PlayerLight] Radius.");
			ImGui::SliderFloat("##pl_radius", &c.radius, 0.01f, 2.0f, "%.2f m");
			form_label("Brightness", "1 = the game's brightness. Saved as [PlayerLight] FalloffIntensity,\n"
				"apart from the inverse-square brightness.");
			ImGui::SliderFloat("##pl_fint", &c.falloff_intensity, 0.0f, 10.0f, "x %.2f", ImGuiSliderFlags_Logarithmic);
		}
		else
		{
			form_checkbox("Use the game's reach", "##pl_gr", &c.use_game_range, "Saved as [PlayerLight] UseGameRange.");
			if (!c.use_game_range)
			{
				form_label("Reach", "Saved as [PlayerLight] Range.");
				ImGui::SliderFloat("##pl_range", &c.range, 0.1f, 30.0f, "%.2f m");
			}
			form_checkbox("Use the game's emitter size", "##pl_gs", &c.use_game_radius,
				"The game's dwindle begin as the emitter. Saved as [PlayerLight] UseGameRadius.");
			if (!c.use_game_radius)
			{
				form_label("Emitter size", "Saved as [PlayerLight] Radius.");
				ImGui::SliderFloat("##pl_radius", &c.radius, 0.01f, 2.0f, "%.2f m");
			}
			form_label("Brightness", "Saved as [PlayerLight] Intensity.");
			ImGui::SliderFloat("##pl_int", &c.intensity, 0.0f, 20.0f, "x %.2f", ImGuiSliderFlags_Logarithmic);
		}

		form_label("Offset relative to",
			"World: offset along the map's x / height / z.\n"
			"View: (right, up, forward) against the camera heading, so a forward offset always lights\n"
			"what you are looking at.\n"
			"Character: (right, up, forward) against the way the body faces.\n"
			"View and Character use the heading only; the light never tilts with pitch.");
		ImGui::Combo("##pl_frame", &c.offset_frame, kFrameNames, 3);
		form_label(c.offset_frame == 0 ? "Offset (x, height, z)" : "Offset (right, up, forward)", "Saved as [PlayerLight] Offset.");
		ImGui::DragFloat3("##pl_offset", c.offset, 0.01f, -5.0f, 5.0f, "%.2f");

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
		if (ImGui::Selectable(std::format("{}{}##{:016x}", r.name, solo_ == r.key ? "  [solo]" : "", r.ident()).c_str(),
			selected_ == r.ident(), ImGuiSelectableFlags_SpanAllColumns))
			selected_ = r.ident();
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
		ImGui::Text("%.2f %.2f %.2f", r.colour[0], r.colour[1], r.colour[2]);
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
		form_value("Source", kKindSection[static_cast<int>(row.kind)]);
		form_value("Comes from", row.origin);
		form_value("Position", std::format("{:.2f}, {:.2f}, {:.2f}  ({:.1f} m from the player)",
			row.pos[0], row.pos[1], row.pos[2], row.distance));
		form_value("Game colour", std::format("{:.3f} {:.3f} {:.3f}", row.colour[0], row.colour[1], row.colour[2]));
		form_value("Game reach", std::format("full to {:.2f} m, zero at {:.2f} m", row.range_begin, row.range_end));
		form_value("Sent to Remix", std::format("radiance {:.2f} {:.2f} {:.2f}", row.radiance[0], row.radiance[1], row.radiance[2]));
		if (row.kind == kind::ffx_table)
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
				"Unlit effect: changes apply to every instance of this effect and are saved.");
		else if (!is_static(row.kind))
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
				"Moving light: changes follow its slot for this session and are not saved.");
		ImGui::Spacing();

		auto& o = overrides_[row.key];
		bool draw = !o.disabled;
		if (form_checkbox("Draw this light", "##pt_draw", &draw, "Switch this one light off or on."))
			o.disabled = !draw;
		if ((row.kind == kind::map_row || row.kind == kind::bank_entity) && (row.state == row_state::characters || o.include_map))
			form_checkbox("Light the map as well", "##pt_include", &o.include_map,
				"The game lights only characters with this light; the map around it gets its light from the\n"
				"lightmap. Ticked, Remix draws it as a normal light, which lights the walls and floor too.");
		form_label("Brightness", "This light only, on top of the source brightness. 0 = off.");
		ImGui::SliderFloat("##pt_mult", &o.multiplier, 0.0f, 10.0f, "x %.2f", ImGuiSliderFlags_Logarithmic);
		if (form_checkbox("Own colour", "##pt_recolour", &o.recolour, "Replace the game's colour with your own.") && o.recolour)
			std::memcpy(o.colour, row.colour, sizeof(o.colour));
		if (o.recolour)
		{
			form_label("Colour", nullptr);
			ImGui::ColorEdit3("##pt_colour", o.colour, ImGuiColorEditFlags_Float);
		}
		solo_flash_imgui(row.key);

		if (row.kind == kind::custom)
		{
			if (ImGui::Button("Edit position and aim"))
			{
				custom_selected_ = row.slot - game::custom_lights::kFirstSlot;
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
					bool changed = form_checkbox("Category on", "##cat_on", &on, nullptr);
					changed |= form_checkbox("Follows its effect", "##cat_follows", &follows,
						"Each light follows its effect's sprites drawn at its spot, as bright as they are opaque,\n"
						"for effects that come and go (Tomb of the Giants' lantern halos). Off: always lit.");
					form_label("Category brightness", nullptr);
					changed |= ImGui::DragFloat("##cat_intensity", &intensity, 0.01f, 0.0f, 20.0f, "x %.2f");
					form_label("Category reach", nullptr);
					changed |= ImGui::DragFloat("##cat_radius", &radius, 0.01f, 0.05f, 10.0f, "x %.2f");
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
