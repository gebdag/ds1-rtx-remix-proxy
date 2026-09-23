#include "std_include.hpp"
#include "game_lights.hpp"
#include "../game/point_lights.hpp"
#include "../game/map_lights.hpp"
#include "../game/sun_light.hpp"
#include "shared/common/remix_api.hpp"
#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"

#include <algorithm>
#include <cmath>

using shared::common::log;
using shared::common::LOG_TYPE;
using shared::utils::va;
using shared::utils::string_hash64;

namespace comp
{
	namespace
	{
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
	}

	game_lights::game_lights()
	{
		p_this = this;

		const auto& cfg = shared::common::config::get();
		enabled = cfg.lights.enabled;
		intensity_scale = cfg.lights.intensity_scale;
		radius_scale = cfg.lights.radius_scale;
		game::point_lights::set_resolve_entities(cfg.lights.resolve_entity_params);
		game::map_lights::set_enabled(cfg.lights.synthesized_lights);

		sun_enabled = cfg.sun.enabled;
		sun_intensity_scale = cfg.sun.intensity_scale;
		sun_angular_diameter = cfg.sun.angular_diameter;
		std::memcpy(sun_fallback_colour, cfg.sun.fallback_colour, sizeof(sun_fallback_colour));
	}

	game_lights::~game_lights()
	{
		destroy_all_handles();
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

		if (!enabled)
			return;

		for (auto& [slot, light] : live_lights_)
		{
			if (light.handle)
				api.m_bridge.DrawLightInstance(light.handle);
		}
	}

	// Remix lights are immutable once created, so a light that moved is replaced with a
	// fresh handle under the same hash — Remix treats that as the same light moving.
	void game_lights::sync_lights()
	{
		const UINT frame = shared::common::ffp_state::get().frame_count();

		// Engine lights first, then the synthesized stand-ins for maps the engine never
		// lights. They share a shape and a slot-id space, so from here on they are
		// treated identically.
		const auto& harvested = game::point_lights::harvest();
		const auto& synthetic = game::map_lights::get();
		synthetic_count_ = static_cast<int>(synthetic.size());

		std::vector<const game::point_lights::light*> sources;
		sources.reserve(harvested.size() + synthetic.size());
		for (const auto& l : harvested) sources.push_back(&l);
		for (const auto& l : synthetic) sources.push_back(&l);

		harvested_count_ = static_cast<int>(sources.size());
		create_failed_ = 0;
		zero_radiance_ = 0;

		const int budget = max_active_lights_ <= 0
			? static_cast<int>(sources.size())
			: std::min(max_active_lights_, static_cast<int>(sources.size()));

		for (int i = 0; i < budget; ++i)
		{
			const auto& src = *sources[i];

			// Neither a bank row nor a resolvable entity: nothing to light with.
			if (!src.has_params)
				continue;

			// Remix takes radiance and an emitter size rather than a falloff distance, so
			// the engine's dwindle end sets how much energy the light needs to still read
			// at that range (inverse-square), and dwindle begin approximates the emitter.
			const float range = std::max(0.01f, src.dwindle_end * radius_scale);
			const float emitter = src.dwindle_begin > 0.01f
				? src.dwindle_begin * radius_scale
				: emitter_radius_;
			const float falloff = range * range * kRadianceNormalisation;
			const remixapi_Float3D radiance = {
				src.r * intensity_scale * falloff,
				src.g * intensity_scale * falloff,
				src.b * intensity_scale * falloff,
			};
			const remixapi_Float3D position = {
				src.x + manual_offset_[0],
				src.y + manual_offset_[1],
				src.z + manual_offset_[2],
			};

			if (radiance.x <= 0.0f && radiance.y <= 0.0f && radiance.z <= 0.0f)
				++zero_radiance_;

			auto& light = live_lights_[src.slot];
			const bool unchanged = light.handle &&
				light.sphere.position.x == position.x &&
				light.sphere.position.y == position.y &&
				light.sphere.position.z == position.z &&
				light.info.radiance.x == radiance.x &&
				light.info.radiance.y == radiance.y &&
				light.info.radiance.z == radiance.z;

			light.last_seen_frame = frame;
			if (unchanged)
				continue;

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

		// Drop lights whose slot went dark this frame, otherwise Remix keeps rendering
		// them — rtx.antiCulling.light extends light lifetime aggressively.
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
	// albedo * radiance * cos, so c95 is used as the radiance unchanged.
	void game_lights::sync_sun()
	{
		const auto& sun = game::sun_light::current(shared::common::ffp_state::get().frame_count(),
			sun_fallback_colour);

		remixapi_Float3D radiance = {
			sun.key.colour[0] * sun_intensity_scale,
			sun.key.colour[1] * sun_intensity_scale,
			sun.key.colour[2] * sun_intensity_scale,
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

	void game_lights::frame_update()
	{
		if (!enabled && !live_lights_.empty())
			destroy_all_handles();

		if (!try_init_remix_api())
			return;

		sync_sun();

		if (!enabled)
			return;

		if (probe_light_mode_)
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

		sync_lights();
	}

	void game_lights::sun_imgui()
	{
		const auto& sun = game::sun_light::current(shared::common::ffp_state::get().frame_count(),
			sun_fallback_colour);

		ImGui::Checkbox("Sun Enabled", &sun_enabled);
		ImGui::SameLine();
		if (!sun.valid)
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
				"no sun-direction draws for %u frames", sun.frames_since_seen);
		else if (sun_handle_)
			ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "[live]");
		else
			ImGui::TextDisabled("[off]");
		if (sun_create_failed_ > 0)
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
				"Remix rejected %d sun CreateLight call(s)", sun_create_failed_);
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(
				"Direction: the game's shadow light (pixel shader c175, ShadowBank), which every\n"
				"shadowed shader reads; else directional light 0 (c92). Colour: light 0 (c95)\n"
				"from a HemDir3 draw, else the last one seen, else the fallback colour.\n"
				"Each frame the draws reading them vote, weighted by primitive count.");

		ImGui::SliderFloat("Sun Intensity Scale", &sun_intensity_scale, 0.0f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Sun Angular Diameter", &sun_angular_diameter, 0.05f, 10.0f, "%.2f deg");
		ImGui::ColorEdit3("Sun Fallback Colour", sun_fallback_colour, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);

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
		static const char* kColourSource[] = { "fallback", "map light 0 c95", "character light 0 c95", "last c95 seen" };
		const float to_sun_y = -sun.key.dir[1];
		const float heading = std::atan2(-sun.key.dir[0], -sun.key.dir[2]) * 57.2957795f;
		ImGui::TextDisabled("  elevation %.1f deg, heading %.1f deg  | direction: %s | colour: %s",
			std::asin(std::clamp(to_sun_y, -1.0f, 1.0f)) * 57.2957795f, heading,
			kDirSource[static_cast<int>(sun.dir_from)], kColourSource[static_cast<int>(sun.colour_from)]);
		ImGui::TextDisabled("  last frame: %u shadow draws (%d values), %u map / %u character HemDir3 draws (%d values)",
			sun.shadow_draws, sun.shadow_candidates, sun.map_dir3_draws, sun.chr_dir3_draws, sun.dir3_candidates);

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

	void game_lights::tab_imgui()
	{
		sun_imgui();
		ImGui::Separator();

		ImGui::Checkbox("Game Lights Enabled", &enabled);
		ImGui::SameLine();
		ImGui::TextDisabled("(%d live / %d harvested)", active_light_count(), total_light_count());
		if (create_failed_count() > 0)
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
				"Remix rejected %d CreateLight call(s) last frame", create_failed_count());
		if (zero_radiance_count() > 0)
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
				"%d light(s) created with zero radiance — their LIGHT_BANK row is black",
				zero_radiance_count());
		ImGui::SameLine();
		if (remix_api_ready_)
			ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "[Remix API]");
		else
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "[Waiting For Remix API]");

		if (game::point_lights::is_available())
		{
			const int live = game::point_lights::slots_live();
			const int from_bank = game::point_lights::slots_from_bank();
			const int from_entity = game::point_lights::slots_from_entity();
			const int unresolved = game::point_lights::slots_unresolved();
			ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f),
				"KPointLightMan: FOUND  (%d slots live — %d bank row, %d entity)",
				live, from_bank, from_entity);
			if (unresolved > 0)
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
					"  %d slot(s) resolved neither a row nor an entity and are skipped.", unresolved);

			bool resolve = game::point_lights::resolve_entities();
			if (ImGui::Checkbox("Resolve Entity Params (FX / SFX lights)", &resolve))
				game::point_lights::set_resolve_entities(resolve);

			if (resolve && ImGui::TreeNode("Entity table walk"))
			{
				ImGui::TextUnformatted(game::point_lights::chain_diagnostics().c_str());
				ImGui::TreePop();
			}

			bool synth = game::map_lights::enabled();
			if (ImGui::Checkbox("Synthesized Map Lights", &synth))
			{
				game::map_lights::set_enabled(synth);
				destroy_all_handles();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					"Blighttown places 99 torch effects and no MSB Light events, and the torch\n"
					"effect declares no light node -- the engine holds two point lights there,\n"
					"both the bonfire's. These stand-ins come from the shipped flame placements,\n"
					"coloured by that map's own POINT_LIGHT_BANK row.\n"
					"Edit rtx_comp\\lights\\<map>.txt and press Reload.");

			ImGui::SameLine();
			ImGui::TextDisabled("(0x%08X %s: %s)",
				game::map_lights::current_map_id(),
				game::map_lights::current_map_name().empty()
					? "no map" : game::map_lights::current_map_name().c_str(),
				game::map_lights::status().c_str());

			if (synth && ImGui::Button("Reload Light Table"))
			{
				game::map_lights::reload();
				destroy_all_handles();
			}

			// Categories are global — tuning "torch_small" moves every torch in the game,
			// not just the ones on this map, which is the point of grouping them.
			const auto& categories = game::map_lights::categories();
			if (synth && !categories.empty()
				&& ImGui::TreeNodeEx("Light Categories", ImGuiTreeNodeFlags_DefaultOpen))
			{
				ImGui::TextDisabled("on   intensity        radius         category (count here)");
				for (size_t i = 0; i < categories.size(); ++i)
				{
					const auto& category = categories[i];
					bool on = category.enabled;
					float intensity = category.intensity;
					float radius = category.radius;

					ImGui::PushID(static_cast<int>(i));
					bool changed = ImGui::Checkbox("##enabled", &on);
					ImGui::SameLine();
					ImGui::SetNextItemWidth(110.0f);
					changed |= ImGui::DragFloat("##intensity", &intensity, 0.01f, 0.0f, 20.0f, "x%.2f");
					ImGui::SameLine();
					ImGui::SetNextItemWidth(110.0f);
					changed |= ImGui::DragFloat("##radius", &radius, 0.01f, 0.05f, 10.0f, "r%.2f");
					ImGui::SameLine();
					ImGui::Text("%s (%d)", category.name.c_str(), category.count);
					if (changed)
						game::map_lights::set_category(i, intensity, radius, on);
					ImGui::PopID();
				}

				if (ImGui::Button("Save Categories"))
				{
					const bool ok = game::map_lights::save_categories();
					log("GameLights", ok
						? std::format("Saved light categories to {}",
							game::map_lights::categories_path())
						: std::string("Could not write the category file"),
						ok ? LOG_TYPE::LOG_TYPE_STATUS : LOG_TYPE::LOG_TYPE_ERROR, true);
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Written to rtx_comp\\lights\\categories.txt and "
						"reloaded on the next launch.");
				ImGui::SameLine();
				if (ImGui::Button("Reset To Table Values"))
					game::map_lights::reset_categories();

				ImGui::TreePop();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					"Direct lights — the class the SFX system uses — zero the slot's bank-row\n"
					"pointer and keep colour and falloff on the light entity, so they only appear\n"
					"once the DLRF handle is resolved. Blighttown's 99 torches are all of this kind:\n"
					"its MSB places no Light events at all, so with this off the map has no lights.\n"
					"Turn it off to fall back to MSB map lights only.");
		}
		else
		{
			ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
				"KPointLightMan: not reachable (menu / world not loaded)");
		}

		ImGui::TextDisabled("Map lights, character torches and FX lights all come from this manager.");

		if (ImGui::Button("Dump Harvested Lights To Log"))
		{
			const auto path = game::point_lights::dump_to_log();
			log("GameLights", path.empty()
				? std::string("Could not write the light dump")
				: std::format("Light dump appended to {}", path),
				path.empty() ? LOG_TYPE::LOG_TYPE_ERROR : LOG_TYPE::LOG_TYPE_STATUS, true);
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(
				"Stand next to a torch that lights correctly, press this, then stand next to one\n"
				"that does not and press it again. Each row names its source (row / ent), the\n"
				"entity class (Bank / Direct / LerpBank) and the resolved colour, so a light that\n"
				"is dark in the engine is told apart from one the proxy failed to resolve.");

		ImGui::Separator();

		ImGui::SliderFloat("Intensity Scale", &intensity_scale, 0.01f, 50.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("1.0 is the tuned default. 8.0 restores the pre-2026-09-02 brightness.");
		ImGui::SliderFloat("Radius Scale", &radius_scale, 0.1f, 10.0f, "%.2f");
		ImGui::SliderFloat("Emitter Radius", &emitter_radius_, 0.01f, 2.0f, "%.2f");
		ImGui::SameLine();
		ImGui::TextDisabled("(fallback when the row has no dwindle begin)");
		ImGui::SliderInt("Max Active Lights", &max_active_lights_, 0, 512);
		ImGui::SameLine();
		ImGui::TextDisabled("0 = unlimited");
		ImGui::DragFloat3("Manual Offset", manual_offset_, 1.0f, -50000.0f, 50000.0f, "%.2f");

		ImGui::Separator();

		if (ImGui::Checkbox("Probe Light Mode", &probe_light_mode_))
			destroy_all_handles();
		if (probe_light_mode_)
		{
			ImGui::DragFloat3("Probe Position", probe_position_, 1.0f, -50000.0f, 50000.0f, "%.2f");
			ImGui::SliderFloat("Probe Radius", &probe_radius_, 0.01f, 10.0f, "%.2f");
			ImGui::SliderFloat("Probe Radiance", &probe_radiance_, 0.01f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		}

		ImGui::Separator();
		ImGui::Checkbox("Show Position HUD", &show_pos_hud_);

		if (ImGui::CollapsingHeader("Harvested Lights", ImGuiTreeNodeFlags_DefaultOpen))
		{
			const auto& lights = game::point_lights::harvest();
			if (lights.empty())
			{
				ImGui::TextDisabled("  (none)");
			}
			else if (ImGui::BeginTable("##harvested", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
			{
				ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 50.0f);
				ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthFixed, 80.0f);
				ImGui::TableSetupColumn("Y", ImGuiTableColumnFlags_WidthFixed, 80.0f);
				ImGui::TableSetupColumn("Z", ImGuiTableColumnFlags_WidthFixed, 80.0f);
				ImGui::TableSetupColumn("RGB");
				ImGui::TableSetupColumn("Dwindle", ImGuiTableColumnFlags_WidthFixed, 100.0f);
				ImGui::TableSetupColumn("Handle", ImGuiTableColumnFlags_WidthFixed, 90.0f);
				ImGui::TableHeadersRow();

				const int shown = std::min(static_cast<int>(lights.size()), 32);
				for (int i = 0; i < shown; ++i)
				{
					const auto& l = lights[i];
					ImGui::TableNextRow();
					ImGui::TableNextColumn(); ImGui::Text("%d", l.slot);
					ImGui::TableNextColumn(); ImGui::Text("%.2f", l.x);
					ImGui::TableNextColumn(); ImGui::Text("%.2f", l.y);
					ImGui::TableNextColumn(); ImGui::Text("%.2f", l.z);
					ImGui::TableNextColumn();
					if (l.has_params)
						ImGui::TextColored(ImVec4(l.r, l.g, l.b, 1.0f), "%.2f %.2f %.2f", l.r, l.g, l.b);
					else
						ImGui::TextDisabled("no row");
					ImGui::TableNextColumn(); ImGui::Text("%.1f - %.1f", l.dwindle_begin, l.dwindle_end);
					ImGui::TableNextColumn(); ImGui::Text("0x%08X", l.entity_handle);
				}
				ImGui::EndTable();
			}
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
