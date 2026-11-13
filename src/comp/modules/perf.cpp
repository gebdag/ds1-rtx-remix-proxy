// Perf tab: per-category draw accounting and rendering kill switches.
//
// Two independent layers:
//  - game-side switches (render_toggles): DS1's own scheduler flags, killing
//    work before it is planned — the honest way to measure what a feature
//    costs, since draws, culling, and bridge traffic all disappear.
//  - proxy-side filters (this module): per-draw classification at the D3D
//    layer with the option to drop a category. The draw still costs the
//    game-side CPU and one proxy round-trip, so this layer is for isolating
//    Remix/bridge cost per category and for categories the scheduler cannot
//    separate.
#include "std_include.hpp"
#include "perf.hpp"

#include "rt_tracker.hpp"
#include "benchmark.hpp"
#include "../game/render_toggles.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/ffp_prof.hpp"
#include "shared/common/config.hpp"

namespace comp::perf
{
	using game::render_toggles::toggle;

	namespace
	{
		constexpr int CAT_COUNT = static_cast<int>(draw_cat::count);
		constexpr int TOGGLE_COUNT = static_cast<int>(toggle::count);
		constexpr int DELTA_NONE = INT_MIN;
		constexpr int PROBE_FRAMES = 40; // ~2/3 s at 60 fps — lets the scheduler settle

		bool s_render[CAT_COUNT] = { true, true, true, true, true, true,
									 true, true, true, true, true, true };
		UINT s_count_frame[CAT_COUNT] = {};
		UINT s_count_last[CAT_COUNT] = {};
		UINT s_total_frame = 0;
		UINT s_total_last = 0;

		// Draw-count delta probe: measures how many draws/frame a game-side
		// toggle added or removed, sampled PROBE_FRAMES after the click.
		int s_probe_toggle = -1;
		int s_probe_frames_left = 0;
		UINT s_probe_baseline = 0;
		int s_toggle_delta[TOGGLE_COUNT];
		bool s_delta_init = false;

		// ---- draw-call inspector state ----

		struct inspector_info
		{
			bool valid = false;
			UINT index = 0;
			char call_type[8] = {};
			D3DPRIMITIVETYPE prim_type = D3DPT_TRIANGLELIST;
			UINT prim_count = 0, num_verts = 0, stride = 0;
			INT base_vtx = 0;
			UINT start_idx = 0;
			// pipeline state at draw time
			bool has_normal = false, has_texcoord = false, has_color = false;
			bool skinned = false, pos_t = false, water_ps = false;
			int texcoord_type = -1, num_bones = 0, bone_start = 0;
			void* vs = nullptr;
			void* ps = nullptr;
			uint32_t ps_sampler_mask = 0;
			bool zwrite = false, ablend = false;
			float world_pos[3] = {};
			// render target
			int rt_index = -1;
			UINT rt_w = 0, rt_h = 0;
			int rt_fmt = 0;
			bool rt_is_bb = false, rt_bb_size = false;
			// textures (tracked bindings + resolved sizes where queryable)
			void* textures[8] = {};
			UINT tex_w[8] = {}, tex_h[8] = {};
			int tex_fmt[8] = {};
			int albedo_stage = 0;
			draw_cat category = draw_cat::other;
			bool category_valid = false;
		};

		bool s_insp_enabled = false;
		bool s_insp_mark = false;
		bool s_insp_solo = false;
		int s_insp_index = 0;          // selected draw (0-based, issue order)
		UINT s_insp_counter = 0;       // running index this frame
		UINT s_insp_total_last = 0;    // draws counted last frame
		bool s_insp_capture_pending = false; // selected draw awaiting classification
		inspector_info s_insp_info;
		IDirect3DTexture9* s_insp_placeholder = nullptr;

		// Sticky selection: a draw's identity key (geometry source + stage-0
		// texture + occurrence ordinal for multi-pass repeats). Once locked,
		// the inspector follows the draw across frames even as its index in
		// the issue order shifts; moving the slider re-locks onto the new pick.
		bool s_insp_follow = true;
		bool s_insp_lock_active = false;
		uint64_t s_insp_lock_key = 0;
		int s_insp_lock_occ = 0;
		bool s_insp_lock_found_frame = false; // matched during the current frame
		bool s_insp_lock_found_last = true;   // matched during the last full frame
		std::unordered_map<uint64_t, int> s_insp_occ; // per-frame occurrence counts

		// Refs held on the snapshot's textures so the F4 preview can never
		// draw a texture the game has already released.
		IDirect3DBaseTexture9* s_insp_retained[8] = {};

		void release_retained_textures()
		{
			for (auto*& t : s_insp_retained)
			{
				if (t) t->Release();
				t = nullptr;
			}
		}

		const char* k_cat_labels[CAT_COUNT] =
		{
			"Shadow atlas draws",
			"Reflection pass draws",
			"Aux half-res pass draws",
			"Other offscreen draws",
			"Water surfaces",
			"Skinned meshes",
			"Depth prepass draws",
			"Alpha-blended draws",
			"World geometry (rigid)",
			"No-normal geometry",
			"HUD/menu quads",
			"Other draws",
		};

		void count(draw_cat c)
		{
			s_count_frame[static_cast<int>(c)]++;
			s_total_frame++;
		}

		draw_cat classify()
		{
			auto& ffp = shared::common::ffp_state::get();
			const auto* rt = rt_tracker::current_info();

			// Off-screen passes are identified by their render target. The main
			// scene RT matches the backbuffer size (Remix raytraces only those).
			if (rt && !rt->is_backbuffer && !rt->matches_backbuffer_size)
			{
				const int atlas = game::render_toggles::shadow_map_size();
				const UINT atlas_edge = (atlas >= 512 && atlas <= 2048) ? static_cast<UINT>(atlas) : 2048u;
				if (rt->width == atlas_edge && rt->height == atlas_edge)
					return draw_cat::shadow_atlas;
				if (rt->width == 256 && rt->height == 180)
					return draw_cat::reflection;
				if (rt->width == 512 && rt->height == 360)
					return draw_cat::aux_halfres;
				return draw_cat::offscreen_other;
			}

			if (ffp.cur_ps_is_water_surface())
				return draw_cat::water;
			if (ffp.cur_decl_is_skinned())
				return draw_cat::skinned;
			if (ffp.zwrite_enabled())
				return draw_cat::prepass;
			if (ffp.alpha_blend_enabled())
				return draw_cat::translucent;
			if (ffp.cur_decl_has_normal())
				return draw_cat::world_rigid;
			if (ffp.last_decl())
				return draw_cat::hud_geo;
			return draw_cat::other;
		}
	}

	const char* cat_label(draw_cat c)
	{
		return k_cat_labels[static_cast<int>(c)];
	}

	bool classify_and_filter()
	{
		const auto cat = classify();
		count(cat);
		if (s_insp_capture_pending)
		{
			s_insp_info.category = cat;
			s_insp_info.category_valid = true;
			s_insp_capture_pending = false;
		}
		return !s_render[static_cast<int>(cat)];
	}

	hud_quad_result filter_hud_quad(D3DPRIMITIVETYPE type, UINT num_verts, UINT prim_count,
		D3DFORMAT index_format, UINT stride)
	{
		const bool is_hud =
			stride == 12 && index_format == D3DFMT_INDEX16 &&
			((type == D3DPT_TRIANGLEFAN && num_verts == 4 && prim_count == 2) ||
			 (type == D3DPT_TRIANGLESTRIP && num_verts == 10 && prim_count == 8));
		if (!is_hud)
			return {};

		count(draw_cat::hud_quads);
		if (s_insp_capture_pending)
		{
			s_insp_info.category = draw_cat::hud_quads;
			s_insp_info.category_valid = true;
			s_insp_capture_pending = false;
		}
		return { true, !s_render[static_cast<int>(draw_cat::hud_quads)] };
	}

	bool render_enabled(draw_cat c) { return s_render[static_cast<int>(c)]; }
	void set_render(draw_cat c, bool render) { s_render[static_cast<int>(c)] = render; }

	UINT count_last(draw_cat c) { return s_count_last[static_cast<int>(c)]; }
	UINT total_last() { return s_total_last; }

	void on_reset() { release_retained_textures(); }

	void on_present()
	{
		for (int i = 0; i < CAT_COUNT; i++)
		{
			s_count_last[i] = s_count_frame[i];
			s_count_frame[i] = 0;
		}
		s_total_last = s_total_frame;
		s_total_frame = 0;

		s_insp_total_last = s_insp_counter;
		s_insp_counter = 0;
		s_insp_capture_pending = false;
		s_insp_lock_found_last = s_insp_lock_found_frame || !s_insp_lock_active;
		s_insp_lock_found_frame = false;
		s_insp_occ.clear();

		if (s_probe_frames_left > 0 && --s_probe_frames_left == 0 && s_probe_toggle >= 0)
		{
			s_toggle_delta[s_probe_toggle] =
				static_cast<int>(s_total_last) - static_cast<int>(s_probe_baseline);
			s_probe_toggle = -1;
		}
	}

	// ------ ImGui ------

	namespace
	{
		void begin_probe(toggle t)
		{
			s_probe_toggle = static_cast<int>(t);
			s_probe_frames_left = PROBE_FRAMES;
			s_probe_baseline = s_total_last;
		}

		// One game-side toggle row: checkbox, engine flag name, optional live
		// draw count from the proxy classifier, and the measured draws/frame
		// delta from the last toggle of this switch.
		void game_toggle_row(toggle t, int count_of = -1)
		{
			if (!s_delta_init)
			{
				for (auto& d : s_toggle_delta) d = DELTA_NONE;
				s_delta_init = true;
			}

			const int i = static_cast<int>(t);
			const auto& desc = game::render_toggles::describe(t);

			bool render = game::render_toggles::get_render(t);
			if (ImGui::Checkbox(desc.label, &render))
			{
				game::render_toggles::set_render(t, render);
				begin_probe(t);
			}

			ImGui::SameLine(320);
			if (count_of >= 0)
				ImGui::TextDisabled("%u draws", s_count_last[count_of]);
			else
				ImGui::TextDisabled("--");

			ImGui::SameLine(430);
			if (s_probe_toggle == i && s_probe_frames_left > 0)
				ImGui::TextDisabled("measuring...");
			else if (s_toggle_delta[i] != DELTA_NONE)
				ImGui::TextColored(s_toggle_delta[i] <= 0
					? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
					"%+d/frame", s_toggle_delta[i]);
			else
				ImGui::TextDisabled(" ");

			ImGui::SameLine(540);
			ImGui::TextDisabled("%s", desc.game_name);
		}

		void proxy_filter_row(draw_cat c)
		{
			const int i = static_cast<int>(c);
			ImGui::Checkbox(k_cat_labels[i], &s_render[i]);
			ImGui::SameLine(320);
			ImGui::TextDisabled("%u draws", s_count_last[i]);
		}

		// ------ FFP conversion step profiler ------

		void help_marker(const char* text)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered())
			{
				ImGui::BeginTooltip();
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
				ImGui::TextUnformatted(text);
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}
		}

		// Tri-state row: 2 = every draw (stock), 1 = only when changed, 0 = never.
		void step_level_row(const char* label, int* value, const char* help)
		{
			const char* items[] = { "never", "when changed", "every draw" };
			ImGui::SetNextItemWidth(150.0f);
			ImGui::Combo(label, value, items, 3);
			help_marker(help);
		}

		void ffp_steps_ui()
		{
			namespace prof = shared::common::ffp_prof;
			auto& cfg = shared::common::config::get();
			auto& ffp = shared::common::ffp_state::get();

			ImGui::TextWrapped(
				"Converting one draw to fixed-function replaces a single DrawIndexedPrimitive "
				"with ~60 D3D9 calls plus a full CPU rebuild of that draw's vertices into a "
				"proxy-owned vertex buffer. The rebuilt buffer is memcpy'd into the bridge IPC "
				"channel on every Unlock and re-uploaded by the runtime, so the conversion can "
				"cost far more than the shader draw it replaces.");
			ImGui::Spacing();

			bool ffp_on = ffp.is_enabled();
			if (ImGui::Checkbox("FFP conversion enabled (master A/B)", &ffp_on))
				ffp.set_enabled(ffp_on);
			ImGui::SameLine(360);
			bool profiling = prof::profiling();
			if (ImGui::Checkbox("Profile steps", &profiling))
			{
				prof::set_profiling(profiling);
				cfg.ffp_steps.profile = profiling;
			}
			help_marker(
				"Times each step with QueryPerformanceCounter. The scopes cost a bool test "
				"when off. NOTE: the bridge is asynchronous, so server-side and GPU cost do "
				"NOT appear in the ms column - use the MB/frame figure and the frame rate "
				"response to the switches below to attribute those.");

			ImGui::Spacing();
			ImGui::Separator();

			const double frame = prof::frame_ms();
			ImGui::Text("Frame: %.2f ms (%.0f fps)", frame, frame > 0.0 ? 1000.0 / frame : 0.0);
			ImGui::SameLine(260);
			ImGui::Text("converted: %u", prof::converted_draws());
			ImGui::SameLine(400);
			ImGui::Text("passthrough: %u", prof::passthrough_draws());

			const double total = prof::total_ms();
			ImGui::Text("Proxy CPU in FFP steps: %.2f ms/frame", total);
			if (frame > 0.0)
			{
				ImGui::SameLine(300);
				ImGui::TextDisabled("(%.0f%% of the frame)", 100.0 * total / frame);
			}

			ImGui::Text("Vertex data pushed at the bridge: %.2f MB/frame", prof::expand_mb());
			help_marker(
				"Bytes the expanded vertex buffers hand to the bridge on Unlock. This is the "
				"proxy-side stand-in for Remix/GPU cost, which the ms column cannot see. If "
				"this number is large, buffer reuse is the lever that matters.");
			if (cfg.ffp_steps.expand_reuse)
			{
				ImGui::Text("  avoided by reuse: %.2f MB/frame", prof::saved_mb());
				ImGui::SameLine(320);
				const UINT hits = prof::reuse_hits(), misses = prof::reuse_misses();
				const UINT tot = hits + misses;
				ImGui::TextDisabled("hit rate %u/%u (%.0f%%)", hits, tot,
					tot ? 100.0 * hits / tot : 0.0);
			}

			ImGui::Spacing();

			if (!prof::profiling())
			{
				ImGui::TextDisabled("Enable 'Profile steps' for the per-step breakdown.");
			}
			else if (ImGui::BeginTable("ffp_steps", 5,
				ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit))
			{
				ImGui::TableSetupColumn("Step", ImGuiTableColumnFlags_WidthFixed, 190.0f);
				ImGui::TableSetupColumn("self ms", ImGuiTableColumnFlags_WidthFixed, 70.0f);
				ImGui::TableSetupColumn("share", ImGuiTableColumnFlags_WidthFixed, 110.0f);
				ImGui::TableSetupColumn("calls", ImGuiTableColumnFlags_WidthFixed, 70.0f);
				ImGui::TableSetupColumn("us/call", ImGuiTableColumnFlags_WidthFixed, 70.0f);
				ImGui::TableHeadersRow();

				for (int i = 0; i < prof::STEP_COUNT; i++)
				{
					const auto st = static_cast<prof::step>(i);
					const double ms = prof::self_ms(st);
					const UINT n = prof::calls(st);

					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					if (prof::is_child(st))
						ImGui::TextDisabled("  %s", prof::label(st));
					else
						ImGui::TextUnformatted(prof::label(st));

					if (ImGui::IsItemHovered())
					{
						ImGui::BeginTooltip();
						ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
						ImGui::TextUnformatted(prof::describe(st));
						ImGui::PopTextWrapPos();
						ImGui::EndTooltip();
					}

					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%.3f", ms);

					ImGui::TableSetColumnIndex(2);
					const float share = (total > 0.0) ? static_cast<float>(ms / total) : 0.0f;
					ImGui::ProgressBar(share, ImVec2(-1.0f, 0.0f), "");

					ImGui::TableSetColumnIndex(3);
					ImGui::Text("%u", n);

					ImGui::TableSetColumnIndex(4);
					if (n > 0)
						ImGui::Text("%.2f", (ms * 1000.0) / static_cast<double>(n));
					else
						ImGui::TextDisabled("--");
				}
				ImGui::EndTable();
			}

			ImGui::Spacing();
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Step switches");
			ImGui::TextWrapped(
				"Every default here is stock behaviour except buffer reuse, which is on. "
				"Flip one at a time and watch the frame rate - that is the only way to see cost "
				"that lands on the bridge server or the GPU rather than in the proxy.");
			ImGui::Spacing();

			ImGui::Checkbox("Reuse expanded vertex buffers", &cfg.ffp_steps.expand_reuse);
			help_marker(
				"THE BIG ONE. Stock always re-expands and re-uploads every draw's vertices, "
				"because a VB can be destroyed and re-created at the same address and a "
				"pointer-keyed cache hit would then serve garbage. With this on, a hit is "
				"validated against a content fingerprint of the source bytes instead, and a "
				"match skips the destination lock, the conversion and the Unlock entirely - "
				"so nothing reaches the bridge and Remix sees no upload. The fingerprint is "
				"sampled, not exhaustive: strong in practice, not a proof. On by default; "
				"turn it off first if a rigid mesh freezes, stretches or shows wrong UVs.");

			step_level_row("Texture stage setup", &cfg.ffp_steps.tex_stages,
				"29 SetTextureStageState per converted draw, setting stage 0 to MODULATE and "
				"disabling stages 1-7. The configuration is identical for nearly every draw, "
				"so 'when changed' applies it once and re-applies only after the game itself "
				"touches a stage (tracked through the device wrapper).");

			step_level_row("Transform upload", &cfg.ffp_steps.transforms,
				"SetTransform(VIEW/PROJECTION/WORLD). Stock re-applies all three on every draw "
				"even when no VS constant changed; 'when changed' skips the redundant ones. "
				"'never' is diagnostic only - geometry will be placed with a stale transform.");

			step_level_row("Restore textures", &cfg.ffp_steps.restore_textures,
				"8 SetTexture putting the game's bindings back after a converted draw. "
				"'when changed' restores only the stages the conversion actually rebound.");

			ImGui::Checkbox("Null texture stages 1-7 for albedo", &cfg.ffp_steps.albedo_null_stages);
			help_marker(
				"Stock unbinds stages 1-7 so Remix cannot pick up the game's shadow maps and "
				"LUTs as material inputs. Turning it off halves the SetTexture traffic per "
				"draw, relying on the stage COLOROP/ALPHAOP disables instead. Watch for wrong "
				"materials if you do.");

			ImGui::Checkbox("Blended-decal state query", &cfg.ffp_steps.decal_query);
			help_marker(
				"3 GetRenderState per draw detecting the game's own alpha-blended edge-fade "
				"strips so they can be tagged as decals. Off, those strips reach Remix as "
				"unordered translucency (dark fringes) - a measurement switch, not a setting "
				"to ship off.");

			ImGui::Checkbox("Save overlay states only when needed", &cfg.ffp_steps.overlay_save_lazy);
			help_marker(
				"save_overlay_render_states costs 7 GetRenderState and is currently called on "
				"EVERY converted draw, even though only two-layer map materials ever run an "
				"overlay pass. On, the save is deferred until the draw is known to need one. "
				"This is a straight win with no visual change.");

			ImGui::Checkbox("Two-layer overlay pass", &cfg.ffp.texture_layer_blend);
			help_marker(
				"The coplanar decal re-draw for two-layer map materials. It doubles the draw "
				"count AND the expanded-vertex traffic for every affected mesh, so it is the "
				"second-biggest lever after buffer reuse. Off loses the second diffuse layer.");

			ImGui::Checkbox("Per-draw Z-write query", &cfg.ffp.zwrite_query_per_draw);
			help_marker(
				"GetRenderState(D3DRS_ZWRITEENABLE) per draw feeding the depth-prepass gate. "
				"Served from the bridge's client-side shadow so it does not round-trip, but it "
				"still takes the bridge device lock. Off uses the passively tracked value, "
				"which misses changes applied through StateBlock::Apply.");
		}
	}

	void tab_imgui()
	{
		auto& cfg = shared::common::config::get();
		namespace rt = game::render_toggles;

		ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Rendering Cost Isolation");
		ImGui::SameLine();
		ImGui::TextDisabled("game draws last frame: %u", s_total_last);
		ImGui::SameLine();
		if (rt::available())
			ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[scheduler hooked]");
		else
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "[waiting for GameRend — load into the game]");

		ImGui::TextWrapped(
			"Game-side switches write DS1's own scheduler flags (the GAME REND debug menu "
			"bytes), so unchecked work is never planned or submitted — the real cost "
			"disappears, unlike the old suppression patches. Draw counts come from the "
			"proxy classifier; the +/- column shows the measured change in total game "
			"draws per frame after toggling.");
		ImGui::Spacing();

		if (ImGui::CollapsingHeader("Ray Tracing Geometry", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Checkbox("Cut-out foliage as triangle lists", &cfg.meshes.cutout_lists);
			ImGui::SameLine();
			ImGui::TextDisabled("[Meshes] CutoutLists");
			ImGui::TextWrapped(
				"The game draws grass, leaves and branches as triangle strips stitched with empty triangles: two of every three "
				"triangles Remix gets have no area. On, those draws reach Remix as lists of their real triangles: the same picture "
				"with a third of the triangles in the ray tracing structures and the opacity micromaps (Firelink: ~0.9 ms, a third "
				"less micromap memory). Switching it rebuilds that foliage's micromaps (about a minute).");
		}

		if (ImGui::CollapsingHeader("Draw Groups (what is rendered)", ImGuiTreeNodeFlags_DefaultOpen))
		{
			game_toggle_row(toggle::group_map, static_cast<int>(draw_cat::world_rigid));
			game_toggle_row(toggle::group_obj);
			game_toggle_row(toggle::group_chr, static_cast<int>(draw_cat::skinned));
			game_toggle_row(toggle::group_sfx, static_cast<int>(draw_cat::translucent));
			game_toggle_row(toggle::group_remo);
			game_toggle_row(toggle::group_light_shaft);
			ImGui::TextDisabled("Map/Obj both land in the rigid-world count; Sfx draws are mostly alpha-blended.");
		}

		if (ImGui::CollapsingHeader("Render Stages (which passes run)", ImGuiTreeNodeFlags_DefaultOpen))
		{
			game_toggle_row(toggle::stage_z_prepass, static_cast<int>(draw_cat::prepass));
			game_toggle_row(toggle::stage_depth_normal, static_cast<int>(draw_cat::prepass));
			game_toggle_row(toggle::stage_depth_tex_edge);
			game_toggle_row(toggle::stage_normal, static_cast<int>(draw_cat::world_rigid));
			game_toggle_row(toggle::stage_bounding_region);
			game_toggle_row(toggle::stage_tex_edge);
			game_toggle_row(toggle::stage_water_mask, static_cast<int>(draw_cat::offscreen_other));
			game_toggle_row(toggle::stage_water, static_cast<int>(draw_cat::water));
			game_toggle_row(toggle::stage_trans, static_cast<int>(draw_cat::translucent));
			game_toggle_row(toggle::stage_shadow_normal, static_cast<int>(draw_cat::shadow_atlas));
			game_toggle_row(toggle::stage_shadow_tex_edge, static_cast<int>(draw_cat::shadow_atlas));
		}

		if (ImGui::CollapsingHeader("Passes & Effects", ImGuiTreeNodeFlags_DefaultOpen))
		{
			game_toggle_row(toggle::shadow_cascades, static_cast<int>(draw_cat::shadow_atlas));
			game_toggle_row(toggle::water_reflection, static_cast<int>(draw_cat::reflection));
			game_toggle_row(toggle::offscreen_sfx, static_cast<int>(draw_cat::aux_halfres));
			game_toggle_row(toggle::msaa);
			game_toggle_row(toggle::env_capture);

			ImGui::Spacing();

			// Shadow atlas size — engine-supported 512..2048; applies when the
			// atlas is recreated (area load).
			int atlas = rt::shadow_map_size();
			if (atlas >= 256 && atlas <= 4096)
			{
				if (ImGui::SliderInt("Shadow atlas size", &atlas, 512, 2048))
					rt::set_shadow_map_size(atlas);
				ImGui::SameLine();
				ImGui::TextDisabled("cascades live: %d (applies on area load)", rt::shadow_cascade_count());
			}

			// Post-FX quad skipping (proxy-side, pre-existing feature)
			ImGui::Checkbox("Post-FX fullscreen quads (proxy skip)", &cfg.ffp.skip_postfx);
			ImGui::SameLine(430);
			ImGui::TextDisabled("%d passes seen — details in Dev > Post-FX", cfg.postfx.pass_count);
		}

		if (ImGui::CollapsingHeader("Post-Processing Filters (experimental)", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::TextWrapped(
				"Kills individual ImageFilter nodes with the engine's own SetEnable plan "
				"token, re-emitted every frame — the effect's GPU passes stop being "
				"scheduled entirely. DS1 has no engine boolean for these (findings.md "
				"2026-07-27). Kill order matters: Light shaft filter -> Bloom -> Tone "
				"mapping, since Bloom and Light shaft read the tone-map output. DoF is "
				"engine-driven on demand (already off outside aiming/cutscenes).");
			ImGui::Spacing();
			game_toggle_row(toggle::postfx_dof);
			game_toggle_row(toggle::postfx_bloom);
			game_toggle_row(toggle::postfx_tonemap);
			game_toggle_row(toggle::postfx_light_shaft);
			game_toggle_row(toggle::postfx_camera_blur);
			game_toggle_row(toggle::postfx_downsample);
			game_toggle_row(toggle::postfx_combine);
		}

		if (ImGui::CollapsingHeader("FFP Conversion Steps (why FFP costs what it costs)"))
			ffp_steps_ui();

		if (ImGui::CollapsingHeader("Proxy Draw Filters (D3D-level, still pays bridge cost)"))
		{
			ImGui::TextWrapped(
				"Drops draws inside the proxy after the game already produced them. Use to "
				"isolate Remix path-tracing / bridge cost per category; prefer the "
				"scheduler switches above for real savings.");
			ImGui::Spacing();
			for (int i = 0; i < CAT_COUNT; i++)
				proxy_filter_row(static_cast<draw_cat>(i));
		}

		ImGui::Spacing();
		ImGui::TextDisabled("All switches revert to stock when re-checked. INI: [Perf] section.");
	}

	// ------ Draw-call inspector ------

	namespace
	{
		const char* prim_type_name(D3DPRIMITIVETYPE t)
		{
			switch (t)
			{
			case D3DPT_POINTLIST:     return "POINTLIST";
			case D3DPT_LINELIST:      return "LINELIST";
			case D3DPT_LINESTRIP:     return "LINESTRIP";
			case D3DPT_TRIANGLELIST:  return "TRIANGLELIST";
			case D3DPT_TRIANGLESTRIP: return "TRIANGLESTRIP";
			case D3DPT_TRIANGLEFAN:   return "TRIANGLEFAN";
			default:                  return "?";
			}
		}

		const char* texcoord_type_name(int t)
		{
			switch (t)
			{
			case D3DDECLTYPE_FLOAT2: return "FLOAT2";
			case D3DDECLTYPE_FLOAT4: return "FLOAT4";
			case D3DDECLTYPE_SHORT2: return "SHORT2";
			case D3DDECLTYPE_SHORT4: return "SHORT4";
			case -1:                 return "none";
			default:                 return "other";
			}
		}

		void snapshot_selected_draw(const char* call_type, D3DPRIMITIVETYPE prim_type,
			UINT prim_count, UINT num_verts, INT base_vtx, UINT start_idx, UINT stride)
		{
			auto& ffp = shared::common::ffp_state::get();
			auto& info = s_insp_info;

			info = {};
			info.valid = true;
			info.index = s_insp_counter;
			strncpy_s(info.call_type, call_type, sizeof(info.call_type) - 1);
			info.prim_type = prim_type;
			info.prim_count = prim_count;
			info.num_verts = num_verts;
			info.base_vtx = base_vtx;
			info.start_idx = start_idx;
			info.stride = stride;

			info.has_normal = ffp.cur_decl_has_normal();
			info.has_texcoord = ffp.cur_decl_has_texcoord();
			info.has_color = ffp.cur_decl_has_color();
			info.skinned = ffp.cur_decl_is_skinned();
			info.pos_t = ffp.cur_decl_has_pos_t();
			info.water_ps = ffp.cur_ps_is_water_surface();
			info.texcoord_type = ffp.cur_decl_texcoord_type();
			info.num_bones = ffp.num_bones();
			info.bone_start = ffp.bone_start_reg();
			info.vs = ffp.last_vs();
			info.ps = ffp.last_ps();
			info.ps_sampler_mask = ffp.cur_ps_sampler_mask();
			info.zwrite = ffp.zwrite_enabled();
			info.ablend = ffp.alpha_blend_enabled();
			info.albedo_stage = shared::common::config::get().ffp.albedo_stage;

			// World translation from the 4x3 world matrix (c8-c10, .w = row translation)
			const float* c = ffp.vs_const_data();
			info.world_pos[0] = c[8 * 4 + 3];
			info.world_pos[1] = c[9 * 4 + 3];
			info.world_pos[2] = c[10 * 4 + 3];

			if (const auto* rt = rt_tracker::current_info())
			{
				info.rt_w = rt->width;
				info.rt_h = rt->height;
				info.rt_fmt = static_cast<int>(rt->format);
				info.rt_is_bb = rt->is_backbuffer;
				info.rt_bb_size = rt->matches_backbuffer_size;
			}

			release_retained_textures();
			for (UINT s = 0; s < 8; s++)
			{
				auto* tex = ffp.cur_texture(s);
				info.textures[s] = tex;
				if (tex)
				{
					tex->AddRef();
					s_insp_retained[s] = tex;
					if (tex->GetType() == D3DRTYPE_TEXTURE)
					{
						D3DSURFACE_DESC desc{};
						if (SUCCEEDED(static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &desc)))
						{
							info.tex_w[s] = desc.Width;
							info.tex_h[s] = desc.Height;
							info.tex_fmt[s] = static_cast<int>(desc.Format);
						}
					}
				}
			}
		}

		std::string inspector_info_text()
		{
			const auto& i = s_insp_info;
			if (!i.valid)
				return "no draw captured yet";

			std::string decl;
			if (i.has_normal) decl += "NORMAL ";
			if (i.has_texcoord) decl += std::format("TEXCOORD({}) ", texcoord_type_name(i.texcoord_type));
			if (i.has_color) decl += "COLOR ";
			if (i.pos_t) decl += "POSITIONT ";
			if (i.skinned) decl += std::format("SKINNED(bones={} start=c{}) ", i.num_bones, i.bone_start);
			if (decl.empty()) decl = "(none tracked)";

			std::string texes;
			for (int s = 0; s < 8; s++)
			{
				if (!i.textures[s]) continue;
				if (!texes.empty()) texes += "  ";
				texes += std::format("s{}=0x{:08X}", s, reinterpret_cast<uintptr_t>(i.textures[s]));
				if (i.tex_w[s])
					texes += std::format(" {}x{} fmt{}", i.tex_w[s], i.tex_h[s], i.tex_fmt[s]);
				if (s == i.albedo_stage) texes += " [albedo]";
			}
			if (texes.empty()) texes = "(none)";

			return std::format(
				"draw #{} of {}  {} {}  prims={} verts={} baseVtx={} startIdx={} stride={}\n"
				"category: {}\n"
				"decl: {}\n"
				"vs=0x{:08X} ps=0x{:08X} psSamplers=0x{:02X}{}\n"
				"state: zwrite={} alphaBlend={}  world=({:.1f}, {:.1f}, {:.1f})\n"
				"rt: {}x{} fmt{}{}{}\n"
				"textures: {}",
				i.index, s_insp_total_last, i.call_type, prim_type_name(i.prim_type),
				i.prim_count, i.num_verts, i.base_vtx, i.start_idx, i.stride,
				i.category_valid ? k_cat_labels[static_cast<int>(i.category)] : "(not classified — UP draw or filtered)",
				decl,
				reinterpret_cast<uintptr_t>(i.vs), reinterpret_cast<uintptr_t>(i.ps),
				i.ps_sampler_mask, i.water_ps ? " [water PS]" : "",
				i.zwrite ? 1 : 0, i.ablend ? 1 : 0,
				i.world_pos[0], i.world_pos[1], i.world_pos[2],
				i.rt_w, i.rt_h, i.rt_fmt,
				i.rt_is_bb ? " (backbuffer)" : "", i.rt_bb_size && !i.rt_is_bb ? " (scene)" : "",
				texes);
		}
	}

	IDirect3DBaseTexture9* inspector_placeholder_texture()
	{
		if (s_insp_placeholder)
			return s_insp_placeholder;

		const auto dev = shared::globals::d3d_device;
		if (!dev)
			return nullptr;

		if (FAILED(dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s_insp_placeholder, nullptr)))
			return nullptr;

		D3DLOCKED_RECT lr{};
		if (SUCCEEDED(s_insp_placeholder->LockRect(0, &lr, nullptr, 0)))
		{
			for (int y = 0; y < 64; y++)
			{
				auto* row = reinterpret_cast<DWORD*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch);
				for (int x = 0; x < 64; x++)
					row[x] = (((x >> 3) ^ (y >> 3)) & 1) ? 0xFFFF00FF : 0xFF00FF00; // magenta/green checker
			}
			s_insp_placeholder->UnlockRect(0);
		}
		return s_insp_placeholder;
	}

	inspector_result inspector_on_draw(const char* call_type, D3DPRIMITIVETYPE prim_type,
		UINT prim_count, UINT num_verts, INT base_vtx, UINT start_idx, UINT stride)
	{
		inspector_result r;
		if (!s_insp_enabled || shared::globals::imgui_is_rendering)
			return r;

		// Draw identity: geometry source + parameters + stage-0 texture.
		// The stage-0 texture separates multi-pass repeats of the same
		// geometry (prepass placeholder vs main-pass diffuse).
		auto& ffp = shared::common::ffp_state::get();
		uint64_t key = 1469598103934665603ull;
		const auto mix = [&key](uint64_t v) { key ^= v; key *= 1099511628211ull; };
		mix(static_cast<uint64_t>(call_type[0]) | (static_cast<uint64_t>(call_type[1]) << 8) |
			(static_cast<uint64_t>(prim_type) << 16));
		mix(reinterpret_cast<uintptr_t>(ffp.stream_vb(0)));
		mix(reinterpret_cast<uintptr_t>(ffp.cur_ib()));
		mix(static_cast<uint32_t>(base_vtx));
		mix(start_idx);
		mix(prim_count);
		mix(stride);
		mix(reinterpret_cast<uintptr_t>(ffp.cur_texture(0)));
		const int occ = s_insp_occ[key]++;

		bool selected;
		if (s_insp_follow && s_insp_lock_active)
		{
			selected = key == s_insp_lock_key && occ == s_insp_lock_occ;
			if (selected)
			{
				s_insp_index = static_cast<int>(s_insp_counter); // slider follows the draw
				s_insp_lock_found_frame = true;
			}
		}
		else
		{
			selected = s_insp_counter == static_cast<UINT>(s_insp_index);
			if (selected && s_insp_follow)
			{
				s_insp_lock_active = true;
				s_insp_lock_key = key;
				s_insp_lock_occ = occ;
				s_insp_lock_found_frame = true;
			}
		}

		if (selected)
		{
			snapshot_selected_draw(call_type, prim_type, prim_count, num_verts, base_vtx, start_idx, stride);
			s_insp_capture_pending = true; // classifier fills the category
			r.selected = true;
			r.mark = s_insp_mark && inspector_placeholder_texture() != nullptr;
		}
		else
		{
			// Solo drops scene draws only. The UP paths carry the post chain
			// and the final scene-RT-to-backbuffer blit — killing those froze
			// the presented image instead of isolating the draw.
			const bool up_call = strstr(call_type, "UP") != nullptr;
			r.drop = s_insp_solo && !up_call;
		}

		s_insp_counter++;
		return r;
	}

	void inspector_imgui()
	{
		ImGui::Checkbox("Enable Inspector", &s_insp_enabled);
		ImGui::SameLine();
		ImGui::TextDisabled("steps through the frame's draws in issue order");
		if (!s_insp_enabled)
		{
			s_insp_mark = false;
			s_insp_solo = false;
			s_insp_lock_active = false;
			release_retained_textures();
			s_insp_info.valid = false;
			return;
		}

		const int max_idx = s_insp_total_last > 0 ? static_cast<int>(s_insp_total_last) - 1 : 0;
		if (s_insp_index > max_idx) s_insp_index = max_idx;
		if (s_insp_index < 0) s_insp_index = 0;

		const int index_before = s_insp_index;

		if (ImGui::ArrowButton("##insp_prev", ImGuiDir_Left) && s_insp_index > 0)
			s_insp_index--;
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 220.0f);
		ImGui::SliderInt("##insp_idx", &s_insp_index, 0, max_idx, "draw %d");
		const bool slider_focused = ImGui::IsItemActive() || ImGui::IsItemFocused();
		ImGui::SameLine();
		if (ImGui::ArrowButton("##insp_next", ImGuiDir_Right) && s_insp_index < max_idx)
			s_insp_index++;
		ImGui::SameLine();
		ImGui::Text("/ %u", s_insp_total_last);

		// Precise stepping with the arrow keys while the slider is active/focused.
		if (slider_focused)
		{
			if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) && s_insp_index > 0)
				s_insp_index--;
			if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) && s_insp_index < max_idx)
				s_insp_index++;
		}

		// A user-driven index change re-locks onto whatever now sits at that index.
		if (s_insp_index != index_before)
			s_insp_lock_active = false;

		ImGui::Checkbox("Mark draw (placeholder texture)", &s_insp_mark);
		ImGui::SameLine();
		ImGui::Checkbox("Solo (kill other scene draws)", &s_insp_solo);
		ImGui::SameLine();
		if (ImGui::Checkbox("Follow draw", &s_insp_follow) && !s_insp_follow)
			s_insp_lock_active = false;

		if (s_insp_follow && s_insp_lock_active && !s_insp_lock_found_last)
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
				"locked draw not present this frame (object culled or pass gone) — keeping lock");
		else if (s_insp_follow && s_insp_lock_active)
			ImGui::TextDisabled("locked onto draw identity — index follows it across frames");

		ImGui::Separator();

		// Live info for the selected draw (re-captured every frame).
		const auto text = inspector_info_text();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.95f, 1.0f, 1.0f));
		ImGui::TextWrapped("%s", text.c_str());
		ImGui::PopStyleColor();

		if (s_insp_info.valid && s_insp_info.index != static_cast<UINT>(s_insp_index))
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
				"showing draw #%u — frame currently has fewer draws than the selection", s_insp_info.index);

		// Bound-texture previews (2D textures only; refs held by the snapshot).
		if (s_insp_info.valid)
		{
			bool any = false;
			for (int s = 0; s < 8; s++)
			{
				if (!s_insp_retained[s] || !s_insp_info.tex_w[s])
					continue;
				if (any) ImGui::SameLine();
				any = true;

				ImGui::BeginGroup();
				ImGui::Image(reinterpret_cast<ImTextureID>(s_insp_retained[s]), ImVec2(72.0f, 72.0f));
				ImGui::TextDisabled("s%d%s", s, s == s_insp_info.albedo_stage ? "*" : "");
				ImGui::EndGroup();

				if (ImGui::IsItemHovered())
				{
					ImGui::BeginTooltip();
					ImGui::Image(reinterpret_cast<ImTextureID>(s_insp_retained[s]), ImVec2(288.0f, 288.0f));
					ImGui::Text("stage %d — %ux%u fmt%d%s", s,
						s_insp_info.tex_w[s], s_insp_info.tex_h[s], s_insp_info.tex_fmt[s],
						s == s_insp_info.albedo_stage ? "  [albedo stage]" : "");
					ImGui::EndTooltip();
				}
			}
			if (any)
				ImGui::TextDisabled("* = albedo stage; hover a thumbnail to enlarge");
		}

		ImGui::Spacing();
		if (ImGui::Button("Log to console", ImVec2(200, 0)))
		{
			shared::common::log("DrawInspector", "\n" + inspector_info_text(),
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("written to rtx_comp\\console.log");
	}
}
