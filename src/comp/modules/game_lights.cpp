#include "std_include.hpp"
#include "game_lights.hpp"
#include "../game/point_lights.hpp"
#include "../game/map_lights.hpp"
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
		constexpr uint64_t kSessionKeyBit = 0x4000000000000000ull;
		constexpr float kRadToDeg = 57.2957795f;

		// Column text per kind, in sort-by-type order.
		const char* const kKindSource[] = {
			"Sun", "Player", "MSB map", "Bank", "Character", "FX", "Synthesized", "Unresolved",
		};
		static_assert(std::size(kKindSource) == static_cast<size_t>(kind::count));

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
			return k == kind::map_row || k == kind::bank_entity || k == kind::synthetic;
		}

		uint64_t row_key(kind k, const game::point_lights::light& l)
		{
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
		radius_scale = cfg.lights.radius_scale;
		game_falloff_ = cfg.get_bool("Lights", "GameFalloff", true);
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

	// How the runtime treats API lights (ds1-dxvk-nr rtx_remix_api.cpp):
	//  - the handle IS the hash;
	//  - CreateLight applies at once; on a hash that already exists it updates that
	//    light in place, but only applies the new values every frame when the light
	//    is flagged isDynamic (a static one is put to sleep and frozen);
	//  - DestroyLight is only queued, and the queue is flushed at Present.
	// So a changed light must be updated with CreateLight alone. Destroying it first
	// and re-creating it under the same hash lets the queued destroy remove the new
	// light at Present: it shows for one frame and is gone until its next change.
	bool game_lights::submit_light(const remixapi_LightInfo& info, remixapi_LightHandle& handle)
	{
		if (destroyed_this_frame(info.hash))
			return false; // would be removed by this frame's pending destroy; retry next frame

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

	void REMIXAPI_PTR game_lights::remix_draw_callback()
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
	float game_lights::edit_multiplier(uint64_t key) const
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
		if (flash_selected_ && key == selected_)
			m *= kFlashMultiplier;
		return m;
	}

	game_lights::light_row game_lights::make_row(const game::point_lights::light& l, const float* player) const
	{
		light_row row;
		row.kind = classify(l);
		row.key = row_key(row.kind, l);
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
			row.name = "Map light";
			row.origin = std::format("slot {}, MSB light row", l.slot);
			break;
		case kind::bank_entity:
			row.name = "Map light (" + bank_row_text(l.entity, false) + ")";
			row.origin = std::format("slot {}, Bank entity", l.slot);
			break;
		case kind::lerp_entity:
			row.name = "Character light (" + bank_row_text(l.entity, true) + ")";
			row.origin = std::format("slot {}, LerpBank entity", l.slot);
			break;
		case kind::fx_direct:
			row.name = "FX light";
			row.origin = std::format("slot {}, Direct entity", l.slot);
			break;
		case kind::synthetic:
		{
			std::string name, category;
			if (game::map_lights::describe(l.slot, name, category))
				row.name = category + " " + name;
			else
				row.name = "Stand-in light";
			row.origin = game::map_lights::current_map_name() + ".txt";
			break;
		}
		default:
			row.name = "No parameters";
			row.origin = std::format("slot {}, handle 0x{:08X}", l.slot, l.entity_handle);
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
		sync_player_light(player);

		const auto& synthetic = game::map_lights::get();
		synthetic_count_ = static_cast<int>(synthetic.size());

		std::vector<const game::point_lights::light*> sources;
		sources.reserve(harvested.size() + synthetic.size());
		for (const auto& l : harvested)
			if (&l != player) sources.push_back(&l);
		for (const auto& l : synthetic) sources.push_back(&l);

		harvested_count_ = static_cast<int>(sources.size());
		create_failed_ = 0;
		zero_radiance_ = 0;

		rows_.clear();
		rows_.reserve(sources.size());

		int active = 0;
		std::string falloff_table;
		for (const auto* src_ptr : sources)
		{
			const auto& src = *src_ptr;
			light_row row = make_row(src, have_player ? player_pos : nullptr);

			const float mult = edit_multiplier(row.key);
			const bool draw = generic && src.has_params && kind_enabled_[static_cast<int>(row.kind)] &&
				mult > 0.0f && (max_active_lights_ <= 0 || active < max_active_lights_);
			if (!draw)
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
			const float emitter = emitter_radius_;
			const auto* o = find_override(row.key);
			const float* colour = o && o->recolour ? o->colour : row.colour;
			remixapi_Float3D radiance;
			if (const bool game_falloff = game_falloff_ && kind_game_falloff_[static_cast<int>(row.kind)])
			{
				// The game's falloff (rtx.ds1Lights.falloffTable): Remix lights the surface with
				// pi * radiance * att(d)^2.2, so radiance = colour^2.2 / pi gives (colour * att)^2.2,
				// the game's light in linear space, at every distance.
				const float scale = intensity_scale * mult / 3.14159265f;
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
				light.sphere.radius == std::max(0.01f, emitter) &&
				light.info.radiance.x == radiance.x &&
				light.info.radiance.y == radiance.y &&
				light.info.radiance.z == radiance.z;

			if (!unchanged)
			{
				// Changed or new: (re)submit under the slot's hash, which updates an
				// existing light in place (see submit_light).
				light.hash = light_hash_for_slot(src.slot);
				light.sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
				light.sphere.pNext = nullptr;
				light.sphere.position = position;
				light.sphere.radius = std::max(0.01f, emitter);
				light.sphere.shaping_hasvalue = FALSE;
				light.sphere.shaping_value = {};
				light.sphere.volumetricRadianceScale = 1.0f;

				light.info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
				light.info.pNext = &light.sphere;
				light.info.hash = light.hash;
				light.info.radiance = radiance;
				light.info.isDynamic = TRUE;

				if (!submit_light(light.info, light.handle) && !destroyed_this_frame(light.hash))
					++create_failed_;
			}

			row.drawn = light.handle != nullptr;
			row.radiance[0] = radiance.x; row.radiance[1] = radiance.y; row.radiance[2] = radiance.z;
			rows_.push_back(std::move(row));
		}

		// The lights with the game's falloff, when the set or a range changes (once: a runtime
		// without the option refuses it, and asking again every frame would not change that).
		if (falloff_table != falloff_table_sent_)
		{
			auto& bridge = shared::common::remix_api::get().m_bridge;
			if (bridge.SetConfigVariable)
				bridge.SetConfigVariable("rtx.ds1Lights.falloffTable", falloff_table.c_str());
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
		// Dynamic, or the runtime freezes it after a few static frames and later
		// direction changes never land.
		sun_info_.isDynamic = TRUE;

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

	void game_lights::sync_player_light(const game::point_lights::light* src)
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
		const float range = std::max(0.01f, c.use_game_range && game ? src->dwindle_end : c.range);
		const float radius = std::max(0.01f,
			c.use_game_radius && game && src->dwindle_begin > 0.01f ? src->dwindle_begin : c.radius);

		// Same radiance model as the other harvested lights, so the defaults keep the
		// brightness the light had before it was split out.
		const float falloff = range * range * kRadianceNormalisation * c.intensity * mult;
		const remixapi_Float3D radiance = { colour[0] * falloff, colour[1] * falloff, colour[2] * falloff };
		player_radiance_[0] = radiance.x; player_radiance_[1] = radiance.y; player_radiance_[2] = radiance.z;

		float offset[3] = {};
		if (!player_offset_world(offset))
			std::memcpy(offset, c.offset, sizeof(offset)); // no heading yet: world axes
		const remixapi_Float3D position = { src->x + offset[0], src->y + offset[1], src->z + offset[2] };

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
		light.info.isDynamic = TRUE;

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
			light.info.isDynamic = TRUE;

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

	// "key disabled multiplier recolour r g b  name", one static light per line.
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
			"player {} {} {} {} {:.3f} {} {:.3f} {:.3f} {} {}\n",
			enabled, intensity_scale, radius_scale, game::point_lights::resolve_entities(), game::map_lights::enabled(),
			sun_enabled, sun_intensity_scale, sun_angular_diameter, format_float3(sun_fallback_colour),
			p.enabled, p.use_game_colour, format_float3(p.colour), p.use_game_range, p.range, p.use_game_radius,
			p.radius, p.intensity, p.offset_frame, format_float3(p.offset));

		std::vector<std::pair<uint64_t, const light_override*>> saved;
		for (const auto& [key, o] : overrides_)
			if (!o.is_default() && (key & kSessionKeyBit) == 0)
				saved.emplace_back(key, &o);
		std::sort(saved.begin(), saved.end());
		for (const auto& [key, o] : saved)
			s += std::format("{:016x} {} {:.3f} {} {}\n", key, o->disabled ? 1 : 0, o->multiplier,
				o->recolour ? 1 : 0, format_float3(o->colour));
		return s;
	}

	bool game_lights::save_all()
	{
		auto& cfg = shared::common::config::get();
		auto flag = [](bool v) { return std::string(v ? "1" : "0"); };
		bool ok = true;

		ok &= cfg.set_string("Lights", "Enabled", flag(enabled));
		ok &= cfg.set_string("Lights", "IntensityScale", std::format("{:.3f}", intensity_scale));
		ok &= cfg.set_string("Lights", "RadiusScale", std::format("{:.3f}", radius_scale));
		ok &= cfg.set_string("Lights", "GameFalloff", game_falloff_ ? "1" : "0");
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
				file << "# Per-light edits from F4 -> Lights. key disabled multiplier recolour r g b  name\n";
				for (const auto& [key, o] : overrides_)
				{
					if (o.is_default() || (key & kSessionKeyBit) != 0)
						continue;
					std::string name;
					for (const auto& r : rows_)
						if (r.key == key) name = r.name;
					file << std::format("{:016x} {} {:.3f} {} {:.3f} {:.3f} {:.3f}  {}\n", key, o.disabled ? 1 : 0,
						o.multiplier, o.recolour ? 1 : 0, o.colour[0], o.colour[1], o.colour[2], name);
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
		radius_scale = cfg.get_float("Lights", "RadiusScale", 1.0f);
		game_falloff_ = cfg.get_bool("Lights", "GameFalloff", true);
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

	void game_lights::tab_imgui()
	{
		list_imgui();
	}

	void game_lights::list_imgui()
	{
		// --- global controls
		if (remix_api_ready_)
			ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "[Remix API]");
		else
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "[Waiting For Remix API]");
		ImGui::SameLine();
		ImGui::TextDisabled("%d point lights drawn of %d  |  sun %s  |  player light %s",
			active_light_count(), total_light_count(), sun_handle_ ? "on" : "off", player_light_.handle ? "on" : "off");
		if (create_failed_count() > 0 || sun_create_failed_ > 0)
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Remix rejected %d point / %d sun CreateLight call(s)",
				create_failed_count(), sun_create_failed_);

		ImGui::Checkbox("Point lights", &enabled);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Every light below the sun and the player light. Those two have their own switches.");
		ImGui::SameLine();
		bool resolve = game::point_lights::resolve_entities();
		if (ImGui::Checkbox("Resolve entity params", &resolve))
			game::point_lights::set_resolve_entities(resolve);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("FX, character and bank-entity lights keep their colour on the light entity, not on a\n"
				"map row. Off: only MSB map lights (and the synthesized ones) get parameters.");
		ImGui::SameLine();
		bool synth = game::map_lights::enabled();
		if (ImGui::Checkbox("Synthesized lights", &synth))
			game::map_lights::set_enabled(synth);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Stand-ins from rtx_comp\\lights\\<map>.txt for flames the engine never lights\n"
				"(Blighttown, Tomb of the Giants, Duke's Archives, the Kiln).");

		// Per-source switches, with how many rows each has.
		int counts[static_cast<int>(kind::count)] = {};
		for (const auto& r : rows_)
			++counts[static_cast<int>(r.kind)];
		for (int k = static_cast<int>(kind::map_row); k < static_cast<int>(kind::count); ++k)
		{
			ImGui::Checkbox(std::format("{} ({})###kind{}", kKindSource[k], counts[k], k).c_str(), &kind_enabled_[k]);
			if (k + 1 < static_cast<int>(kind::count))
				ImGui::SameLine();
		}
		// Which of them Remix lights with the game's falloff instead of inverse square.
		ImGui::Checkbox("Game falloff:", &game_falloff_);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Flat to dwindle begin, linear to zero at dwindle end, as the game lights them,\n"
				"for the kinds ticked here. Default: the lights mounted in the map (torches,\n"
				"lanterns, synthesized flames). Off: inverse square for every light.");
		ImGui::BeginDisabled(!game_falloff_);
		for (int k = static_cast<int>(kind::map_row); k < static_cast<int>(kind::count); ++k)
		{
			ImGui::SameLine();
			ImGui::Checkbox(std::format("{}###falloff{}", kKindSource[k], k).c_str(), &kind_game_falloff_[k]);
		}
		ImGui::EndDisabled();

		ImGui::SetNextItemWidth(180.0f);
		ImGui::SliderFloat("Intensity scale", &intensity_scale, 0.01f, 50.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("All point lights. 1.0 is the tuned default. Not the sun or the player light.");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(140.0f);
		ImGui::SliderFloat("Radius scale", &radius_scale, 0.1f, 10.0f, "%.2f");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(120.0f);
		ImGui::SliderFloat("Emitter radius", &emitter_radius_, 0.01f, 2.0f, "%.2f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Sphere size for lights whose row has no dwindle begin.");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(110.0f);
		ImGui::SliderInt("Max drawn", &max_active_lights_, 0, 512);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("0 = unlimited");

		ImGui::SetNextItemWidth(220.0f);
		ImGui::InputText("Filter (name / origin)", filter_, sizeof(filter_));
		ImGui::SameLine();
		ImGui::TextUnformatted("Sort:");
		static const char* kSortNames[] = { "type", "distance", "source", "name" };
		for (int i = 0; i < 4; ++i)
		{
			ImGui::SameLine();
			if (ImGui::RadioButton(kSortNames[i], sort_mode_ == i))
				sort_mode_ = i;
		}

		if (ImGui::Button("Save"))
			save_all();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Writes [Lights], [Sun] and [PlayerLight] to remix-comp-proxy.ini, per-light edits to\n"
				"rtx_comp\\lights\\light_overrides.txt and the synthesized categories to categories.txt.");
		ImGui::SameLine();
		if (ImGui::Button("Revert to saved"))
			revert_to_saved();
		ImGui::SameLine();
		if (ImGui::Button("Clear edits"))
		{
			overrides_.clear();
			solo_ = 0;
			flash_selected_ = false;
		}
		ImGui::SameLine();
		if (serialize() != saved_text_)
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "unsaved changes");
		else
			ImGui::TextDisabled("%s", save_status_.empty() ? "no changes" : save_status_.c_str());

		// --- the table: sun and player pinned, then the rest filtered and sorted
		std::vector<const light_row*> rows;
		rows.reserve(rows_.size());
		for (const auto& r : rows_)
			if (contains_no_case(r.name, filter_) || contains_no_case(r.origin, filter_))
				rows.push_back(&r);

		std::sort(rows.begin(), rows.end(), [&](const light_row* a, const light_row* b)
		{
			switch (sort_mode_)
			{
			case 1:
				return a->distance < b->distance;
			case 2:
				if (a->kind != b->kind) return a->kind < b->kind;
				return a->origin < b->origin;
			case 3:
				if (a->name != b->name) return a->name < b->name;
				return a->distance < b->distance;
			default:
				if (a->kind != b->kind) return a->kind < b->kind;
				return a->distance < b->distance;
			}
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

		rows.insert(rows.begin(), { &sun_row, &player_row });

		constexpr size_t kMaxRows = 400;
		if (rows.size() > kMaxRows)
		{
			ImGui::TextDisabled("showing the first %zu of %zu rows", kMaxRows, rows.size());
			rows.resize(kMaxRows);
		}

		const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
			ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
		if (ImGui::BeginTable("##lights", 9, flags, ImVec2(0.0f, 300.0f)))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("State");
			ImGui::TableSetupColumn("");
			ImGui::TableSetupColumn("Source");
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Type");
			ImGui::TableSetupColumn("Dist");
			ImGui::TableSetupColumn("Colour");
			ImGui::TableSetupColumn("Range");
			ImGui::TableSetupColumn("Origin", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			for (const auto* r : rows)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();

				const auto* o = find_override(r->key);
				const bool pinned = r->kind == kind::sun || r->kind == kind::player;
				const char* state = r->drawn ? "on" : (o && o->disabled) || (solo_ && solo_ != r->key) ? "off" : "-";
				if (ImGui::Selectable(std::format("{}{}##{:016x}", state, solo_ == r->key ? " S" : "", r->key).c_str(),
					selected_ == r->key, ImGuiSelectableFlags_SpanAllColumns))
					selected_ = r->key;

				ImGui::TableNextColumn();
				{
					// Hue at a glance: the drawn colour scaled so its brightest channel is 1.
					const float* c = o && o->recolour ? o->colour : r->colour;
					const float peak = std::max({ c[0], c[1], c[2], 1e-6f });
					ImGui::ColorButton(std::format("##sw{:016x}", r->key).c_str(),
						ImVec4(c[0] / peak, c[1] / peak, c[2] / peak, 1.0f),
						ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(12, 12));
				}
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(kKindSource[static_cast<int>(r->kind)]);
				ImGui::TableNextColumn();
				if (pinned)
					ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s", r->name.c_str());
				else
					ImGui::TextUnformatted(r->name.c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(r->kind == kind::sun ? "Distant" : "Point");
				ImGui::TableNextColumn();
				if (r->kind == kind::sun) ImGui::TextUnformatted("-");
				else ImGui::Text("%.1f", r->distance);
				ImGui::TableNextColumn();
				ImGui::Text("%.2f %.2f %.2f", r->colour[0], r->colour[1], r->colour[2]);
				ImGui::TableNextColumn();
				if (r->range_end > 0.0f) ImGui::Text("%.1f-%.1f", r->range_begin, r->range_end);
				else ImGui::TextUnformatted("-");
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(r->origin.c_str());
			}
			ImGui::EndTable();
		}

		// --- the selected light
		ImGui::Separator();
		if (selected_ == KEY_SUN)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "Sun  [distant light]");
			sun_imgui();
		}
		else if (selected_ == KEY_PLAYER)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "Player head light  [point light]");
			player_light_imgui();
		}
		else
		{
			const light_row* sel = nullptr;
			for (const auto& r : rows_)
				if (r.key == selected_) sel = &r;
			if (sel)
				point_light_imgui(*sel);
			else
				ImGui::TextDisabled("Select a row to inspect and edit a light.");
		}

		// Solo and flash also apply to the pinned lights, so they sit outside the
		// per-kind panels.
		if (selected_ == KEY_SUN || selected_ == KEY_PLAYER)
		{
			bool solo = solo_ == selected_;
			if (ImGui::Checkbox("Solo (only this light)", &solo))
				solo_ = solo ? selected_ : 0;
			ImGui::SameLine();
			ImGui::Checkbox("Flash x20 (find it)", &flash_selected_);
		}

		if (ImGui::CollapsingHeader("Diagnostics"))
			diagnostics_imgui();
	}

	void game_lights::sun_imgui()
	{
		const auto& sun = game::sun_light::current(shared::common::ffp_state::get().frame_count(),
			sun_fallback_colour);

		ImGui::Checkbox("Sun enabled", &sun_enabled);
		ImGui::SameLine();
		if (!sun.valid)
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
				"no sun-direction draws for %u frames", sun.frames_since_seen);
		else if (sun_handle_)
			ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "[live]");
		else
			ImGui::TextDisabled("[off]");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(
				"Direction: the game's shadow light (pixel shader c175, ShadowBank), which every\n"
				"shadowed shader reads; else directional light 0 (c92). Colour: light 0 (c95)\n"
				"from a HemDir3 draw, else the last one seen, else the fallback colour.\n"
				"Each frame the draws reading them vote, weighted by primitive count.");

		ImGui::SliderFloat("Intensity##sun", &sun_intensity_scale, 0.0f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Angular diameter##sun", &sun_angular_diameter, 0.05f, 10.0f, "%.2f deg");
		ImGui::ColorEdit3("Fallback colour##sun", sun_fallback_colour, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);

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

		ImGui::Checkbox("Player light enabled", &c.enabled);
		ImGui::SameLine();
		if (player_seen_)
		{
			const auto& l = player_src_;
			ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "slot %d, %s, %.2f from head point",
				l.slot, l.entity_vtable ? game::point_lights::entity_class_name(l.entity_vtable)
					: (l.param_row ? "bank row" : "unresolved"), player_head_dist_);
		}
		else
		{
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "no light at the player's head");
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(
				"The light the game attaches to the player's head. It is taken out of the\n"
				"point lights, so their scales and switches do not affect it. Found as the\n"
				"live light nearest %.1f units above the player origin, then held while it\n"
				"stays within %.1f units of that point.", kHeadHeight, kHoldRadius);
		if (player_seen_)
			ImGui::TextDisabled("  game: rgb (%.2f, %.2f, %.2f)  range %.2f - %.2f   Remix radiance %.2f %.2f %.2f",
				player_src_.r, player_src_.g, player_src_.b, player_src_.dwindle_begin, player_src_.dwindle_end,
				player_radiance_[0], player_radiance_[1], player_radiance_[2]);

		ImGui::Checkbox("Game colour##pl", &c.use_game_colour);
		ImGui::SameLine();
		ImGui::BeginDisabled(c.use_game_colour);
		ImGui::ColorEdit3("Colour##pl", c.colour, ImGuiColorEditFlags_Float);
		ImGui::EndDisabled();

		ImGui::Checkbox("Game range##pl", &c.use_game_range);
		ImGui::SameLine();
		ImGui::BeginDisabled(c.use_game_range);
		ImGui::SliderFloat("Range##pl", &c.range, 0.1f, 30.0f, "%.2f");
		ImGui::EndDisabled();

		ImGui::Checkbox("Game radius##pl", &c.use_game_radius);
		ImGui::SameLine();
		ImGui::BeginDisabled(c.use_game_radius);
		ImGui::SliderFloat("Emitter radius##pl", &c.radius, 0.01f, 2.0f, "%.2f");
		ImGui::EndDisabled();

		ImGui::SliderFloat("Intensity##pl", &c.intensity, 0.0f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic);

		ImGui::SetNextItemWidth(130.0f);
		ImGui::Combo("Offset frame##pl", &c.offset_frame, kFrameNames, 3);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(
				"World: offset along the map's x / y / z.\n"
				"View: (right, up, forward) against the camera heading, so a forward offset\n"
				"always lights what you are looking at.\n"
				"Character: (right, up, forward) against the way the body faces.\n"
				"View and Character use the heading only; the light never tilts with pitch.");
		ImGui::SameLine();
		ImGui::DragFloat3(c.offset_frame == 0 ? "Offset x y z##pl" : "Offset right up fwd##pl",
			c.offset, 0.01f, -5.0f, 5.0f, "%.2f");

		{
			float r[3] = {}, f[3] = {};
			const bool cam = shared::common::ffp_state::get().view_axes(r, f) && normalise_xz(f);
			float yaw = 0.0f;
			const bool chr = game::camera::get_char_heading(yaw);
			ImGui::TextDisabled("  heading: camera %s   character %s",
				cam ? std::format("{:.1f} deg", std::atan2(f[0], f[2]) * kRadToDeg).c_str() : "?",
				chr ? std::format("{:.1f} deg", yaw * kRadToDeg).c_str() : "?");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("With the camera straight behind the character the two should match.\n"
					"Character heading is read from the player's position block (+0x04).");
		}

		if (ImGui::Button("Copy game values##pl") && player_seen_ && player_src_.has_params)
		{
			// Starting point for an override: the game's own values, used verbatim.
			c.colour[0] = player_src_.r; c.colour[1] = player_src_.g; c.colour[2] = player_src_.b;
			c.range = player_src_.dwindle_end;
			if (player_src_.dwindle_begin > 0.01f) c.radius = player_src_.dwindle_begin;
		}
	}

	void game_lights::point_light_imgui(const light_row& row)
	{
		ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s  [%s, point light]",
			row.name.c_str(), kKindSource[static_cast<int>(row.kind)]);
		ImGui::TextDisabled("  %s   position %.2f %.2f %.2f   %.1f from the player",
			row.origin.c_str(), row.pos[0], row.pos[1], row.pos[2], row.distance);
		ImGui::TextDisabled("  game colour %.3f %.3f %.3f   range %.2f - %.2f   Remix radiance %.2f %.2f %.2f  %s",
			row.colour[0], row.colour[1], row.colour[2], row.range_begin, row.range_end,
			row.radiance[0], row.radiance[1], row.radiance[2], row.drawn ? "drawn" : "not drawn");
		if (!is_static(row.kind))
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
				"  moving light: edits follow its slot for this session and are not saved");

		auto& o = overrides_[row.key];
		ImGui::Checkbox("Disabled", &o.disabled);
		ImGui::SameLine();
		bool solo = solo_ == row.key;
		if (ImGui::Checkbox("Solo (only this light)", &solo))
			solo_ = solo ? row.key : 0;
		ImGui::SameLine();
		ImGui::Checkbox("Flash x20 (find it)", &flash_selected_);
		ImGui::SliderFloat("Intensity x", &o.multiplier, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		if (ImGui::Checkbox("Recolour", &o.recolour) && o.recolour)
			std::memcpy(o.colour, row.colour, sizeof(o.colour));
		if (o.recolour)
		{
			ImGui::SameLine();
			ImGui::ColorEdit3("Colour##pt", o.colour, ImGuiColorEditFlags_Float);
		}

		// Synthesized lights also belong to a category, which scales every light
		// made from the same effect across the game.
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
					float intensity = categories[i].intensity;
					float radius = categories[i].radius;
					ImGui::TextDisabled("Category '%s' (%d on this map), every map:", category.c_str(), categories[i].count);
					bool changed = ImGui::Checkbox("Category on", &on);
					ImGui::SameLine();
					ImGui::SetNextItemWidth(140.0f);
					changed |= ImGui::DragFloat("Category intensity", &intensity, 0.01f, 0.0f, 20.0f, "x%.2f");
					ImGui::SameLine();
					ImGui::SetNextItemWidth(140.0f);
					changed |= ImGui::DragFloat("Category radius", &radius, 0.01f, 0.05f, 10.0f, "x%.2f");
					if (changed)
						game::map_lights::set_category(i, intensity, radius, on);
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

		ImGui::TextDisabled("Synthesized table: 0x%08X %s: %s", game::map_lights::current_map_id(),
			game::map_lights::current_map_name().empty() ? "no map" : game::map_lights::current_map_name().c_str(),
			game::map_lights::status().c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton("Reload table"))
		{
			game::map_lights::reload();
			destroy_all_handles();
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Reset categories"))
			game::map_lights::reset_categories();

		if (ImGui::Button("Dump harvested lights to log"))
		{
			const auto path = game::point_lights::dump_to_log();
			log("GameLights", path.empty()
				? std::string("Could not write the light dump")
				: std::format("Light dump appended to {}", path),
				path.empty() ? LOG_TYPE::LOG_TYPE_ERROR : LOG_TYPE::LOG_TYPE_STATUS, true);
		}
		ImGui::SameLine();
		ImGui::Checkbox("Show position HUD", &show_pos_hud_);

		ImGui::DragFloat3("Manual offset (all point lights)", manual_offset_, 1.0f, -50000.0f, 50000.0f, "%.2f");

		if (ImGui::Checkbox("Probe light mode", &probe_light_mode_))
			destroy_all_handles();
		if (probe_light_mode_)
		{
			ImGui::DragFloat3("Probe position", probe_position_, 1.0f, -50000.0f, 50000.0f, "%.2f");
			ImGui::SliderFloat("Probe radius", &probe_radius_, 0.01f, 10.0f, "%.2f");
			ImGui::SliderFloat("Probe radiance", &probe_radiance_, 0.01f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
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
