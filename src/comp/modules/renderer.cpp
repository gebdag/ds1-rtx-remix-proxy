#include "std_include.hpp"
#include "../game/waterfall.hpp"
#include "renderer.hpp"
#include "game_lights.hpp"

#include "imgui.hpp"
#include "diagnostics.hpp"
#include "perf.hpp"
#include "rt_tracker.hpp"
#include "skinning.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/ffp_prof.hpp"
#include "../game/sky_meshes.hpp"
#include "../game/estus.hpp"
#include "../game/shaft_source.hpp"
#include "../game/glass.hpp"
#include "../game/effect_lights.hpp"
#include "../game/sun_light.hpp"
#include "../game/atmosphere.hpp"
#include "../game/lightmap_sun.hpp"
#include "../game/swamp.hpp"
#include "../game/texture_names.hpp"
#include "../game/map_lights.hpp"
#include "../game/mesh_lights.hpp"
#include "../game/texture_scroll.hpp"
#include "../game/env_reflection.hpp"
#include "../game/vanilla_view.hpp"

namespace comp
{
	namespace
	{
		// Stage-0 D3DTSS_CONSTANT value the custom runtime (rtx.skyMarkerEnabled)
		// reads as "this draw is sky", like the UI (0xD15D0011) and decal
		// (0xD15DECA1) markers.
		constexpr DWORD SKY_MARKER = 0xD15D05C1;
		// A blended shell drawn as a cutout ([Sky] ShellMode 2) is alpha-tested at this reference (0.25 of the
		// texture alpha times the fade): the game fades the giant trees' tops into the mist, and there the cut sits
		// where Ash Lake's trunks reach in the game's frame.
		constexpr DWORD kShellCutoutAlphaRef = 64;
		// Stage 4's D3DTSS_CONSTANT on a window glass pass: the runtime keeps it single-sided by the
		// game's cull mode, though it is blended. Anor Londo builds many windows twice, an outer pane
		// facing out and an inner one facing in, and the game shows each view only its own pane.
		constexpr DWORD kSingleSidedGlassMarker = 0xD4000002;
		// Stage 4's D3DTSS_CONSTANT on a thin wall (the confirmed map meshes, sky_meshes::is_thin_wall_draw, and the Moonlight
		// Butterfly's wings, texture_names::is_butterfly_wing): the runtime lets lighting rays through its faces where they face
		// the surface the ray left (Surface::isDs1ThinWall).
		constexpr DWORD kThinWallMarker = 0xD4000003;
		// Stage 4's D3DTSS_CONSTANT on a lamp the runtime's lighting rays pass (game/mesh_lights.hpp).
		constexpr DWORD kLampMarker = 0xD4000004;
		// Stage 4's D3DTSS_CONSTANT on foliage (texture_names::is_foliage): lighting rays pass it in part (Surface::isDs1Foliage).
		constexpr DWORD kFoliageMarker = 0xD4000006;

		// A draw of a sky model mesh (dome, far cloud layers, the backdrop they
		// are blended over) in a pass whose camera is the main one: no user clip
		// plane is enabled (the water reflection pass is the only one that
		// enables one).
		bool is_sky_layer_draw(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count)
		{
			auto& ffp = shared::common::ffp_state::get();
			if (!game::sky_meshes::is_sky_draw(ffp.cur_ib(), type, start_index, prim_count))
				return false;

			DWORD clip_planes = 0;
			dev->GetRenderState(D3DRS_CLIPPLANEENABLE, &clip_planes);
			return clip_planes == 0;
		}

		// The game's diffuse for this draw is an atmospheric or backdrop layer
		// (mist, fog, clouds, light shafts, backdrop cards) by its TPF name.
		// The Catacombs' light shafts are the last 106 triangles of its sky draw (m13_sky, sky model m9900B0A13: a dome of
		// strip entries 0-822, then 15 beams from entry 823, vertices 230-285). As sky they showed only where the view reached
		// the sky, behind the cave. The draw goes to the sky as before, then the beams again as a light shaft
		// (on_draw_indexed_prim): s_force_shaft is set for that second draw.
		constexpr UINT kCatacombsSkyPrims = 929;
		constexpr UINT kCatacombsBeamStart = 823;
		bool s_force_shaft = false;
		bool s_beam_split = false;
		// A no-shadow mesh with prims no view shows (game/sky_meshes.hpp, no_shadow_hidden_prims) is drawn in the pieces
		// around them (on_draw_indexed_prim). The pieces no longer match the table by their index range, so they carry the
		// mesh's no-shadow tag through s_force_no_shadow.
		bool s_hidden_split = false;
		bool s_force_no_shadow = false;
		// The whole draw's range while its pieces are drawn: the lightmap sun level is measured over the mesh as before.
		UINT s_split_start = 0;
		UINT s_split_prims = 0;

		bool is_catacombs_sky_with_beams(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT prim_count)
		{
			if (type != D3DPT_TRIANGLESTRIP || prim_count != kCatacombsSkyPrims)
				return false;
			// Not the water reflection's redraw of the sky (the only pass with a clip plane), as for sky draws.
			DWORD clip_planes = 0;
			dev->GetRenderState(D3DRS_CLIPPLANEENABLE, &clip_planes);
			if (clip_planes != 0)
				return false;
			auto& ffp = shared::common::ffp_state::get();
			const int stage = shared::common::config::get().ffp.albedo_stage;
			return game::texture_names::name_of(ffp.cur_texture(static_cast<UINT>(stage))) == "m13_sky";
		}

		// A light shaft mesh (texture_names::is_light_shaft), not skinned.
		bool is_light_shaft_draw()
		{
			if (s_force_shaft)
				return true;
			auto& ffp = shared::common::ffp_state::get();
			const int stage = shared::common::config::get().ffp.albedo_stage;
			return !ffp.cur_decl_is_skinned() && game::texture_names::is_light_shaft(ffp.cur_texture(static_cast<UINT>(stage)));
		}

		bool is_atmospheric_layer()
		{
			if (s_force_shaft)
				return false;
			auto& ffp = shared::common::ffp_state::get();
			const int stage = shared::common::config::get().ffp.albedo_stage;
			return game::texture_names::is_atmospheric(ffp.cur_texture(static_cast<UINT>(stage)));
		}

		// A blended atmospheric layer the game draws over its far backdrop: the FRPG_NtoA mist and
		// light-shaft planes (their alpha fades by view angle, c176) and the backdrop cards
		// (m10_bg_giant_tree). Their look is the game shader's own: its probe light, tint, fog,
		// scattering and view fade. As world geometry Remix lights them instead and loses the fade,
		// so the edge-on mist stack at the horizon turned into a dark band. They go to the sky
		// raster with the game's shaders, like the dome and cloud sea (shown where the view reaches
		// the sky, which is where they sit).
		bool is_backdrop_layer(IDirect3DDevice9* dev)
		{
			auto& ffp = shared::common::ffp_state::get();
			if (!is_atmospheric_layer())
				return false;
			DWORD blend = FALSE, clip_planes = 0;
			dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
			dev->GetRenderState(D3DRS_CLIPPLANEENABLE, &clip_planes);
			if (!blend || clip_planes != 0)
				return false;
			const int stage = shared::common::config::get().ffp.albedo_stage;
			const auto name = game::texture_names::name_of(ffp.cur_texture(static_cast<UINT>(stage)));
			if (ffp.cur_ps_light_flags() & ffp.PS_VIEW_FADE)
				return true;
			return name.find("_bg_") != std::string::npos || name.find("back_ground") != std::string::npos;
		}
	}

	namespace tex_addons
	{
		bool initialized = false;
		LPDIRECT3DTEXTURE9 icon = nullptr;

		void init_texture_addons(bool release)
		{
			if (release)
			{
				if (tex_addons::icon) tex_addons::icon->Release();
				tex_addons::icon = nullptr; // a failed Reset skips the reload
				return;
			}

			const auto dev = shared::globals::d3d_device;
			const char* icon_path = "rtx_comp\\textures\\icon.png";

			// Only load if the file exists — no icon is shipped by default
			if (GetFileAttributesA(icon_path) != INVALID_FILE_ATTRIBUTES)
			{
				HRESULT hr = D3DXCreateTextureFromFileA(dev, icon_path, &tex_addons::icon);
				if (FAILED(hr))
					shared::common::log("Renderer", std::format("Failed to load {}", icon_path), shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			}

			tex_addons::initialized = true;
		}
	}


	// ----

	drawcall_mod_context& setup_context(IDirect3DDevice9* dev)
	{
		auto& ctx = renderer::dc_ctx;
		ctx.info.device_ptr = dev;
		return ctx;
	}

	// Saving the overlay blend state costs 7 GetRenderState. The stock code pays
	// that on every converted draw even though only two-layer map materials ever
	// run an overlay pass, so [FFPSteps] OverlaySaveLazy=1 defers it until the
	// draw is known to need one.
	bool should_save_overlay_states(shared::common::ffp_state& ffp)
	{
		auto& cfg = shared::common::config::get();
		if (!cfg.ffp_steps.overlay_save_lazy)
			return true;
		return ffp.overlay_pass_applicable();
	}

	void save_overlay_render_states(drawcall_mod_context& ctx, IDirect3DDevice9* dev)
	{
		shared::common::ffp_prof::scope prof(shared::common::ffp_prof::step::overlay_save);

		const D3DRENDERSTATETYPE states[] = {
			D3DRS_ZWRITEENABLE,
			D3DRS_ALPHATESTENABLE,
			D3DRS_ALPHABLENDENABLE,
			D3DRS_SRCBLEND,
			D3DRS_DESTBLEND,
			D3DRS_BLENDOP,
			D3DRS_SEPARATEALPHABLENDENABLE,
		};

		for (const auto state : states)
			ctx.save_rs(dev, state);
	}

	// A window glass draw (sky_meshes::glass_texture) with a derived glass texture: the derived
	// texture, else null (the draw then goes through as before).
	IDirect3DBaseTexture9* glass_split_texture(IDirect3DDevice9* dev, shared::common::ffp_state& ffp,
		D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count, DWORD& cut)
	{
		if (!shared::common::config::get().sky.glass_split)
			return nullptr;
		uint8_t table_cut = 0;
		const std::string* derived = nullptr;
		const std::string* listed = game::sky_meshes::glass_texture(ffp.cur_ib(), type, start_index, prim_count,
			table_cut, derived);
		if (!listed)
			return nullptr;
		const int stage = shared::common::config::get().ffp.albedo_stage;
		if (game::texture_names::name_of(ffp.cur_texture(static_cast<UINT>(stage))) != *listed)
			return nullptr;
		cut = table_cut;
		return game::glass::derived_texture(dev, *derived);
	}

	// The came pass of a window glass draw: the game's texture, opaque, alpha test >= cut. Remix
	// renders it as an alpha-tested opaque surface (lit, shadowing, culled by the game's cull mode).
	// Both passes take the texture's alpha alone (the came tests it, the glass blends with it): the
	// game's opaque glass shaders ignore vertex alpha, and several opaque glass meshes carry 0 there
	// (m2400B1A15 mesh14); the blended panes' vertex alpha is 1. The stage states go back with
	// ctx.restore_all, so the FFP stage cache holds.
	void apply_came_pass_states(drawcall_mod_context& ctx, IDirect3DDevice9* dev, DWORD cut)
	{
		save_overlay_render_states(ctx, dev);
		ctx.save_rs(dev, D3DRS_ALPHAFUNC);
		ctx.save_rs(dev, D3DRS_ALPHAREF);
		ctx.save_tss(dev, D3DTSS_ALPHAOP);
		ctx.save_tss(dev, D3DTSS_ALPHAARG1);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
		dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
		dev->SetRenderState(D3DRS_ALPHAREF, cut);
		dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	}


	// ----

	HRESULT renderer::on_draw_primitive(IDirect3DDevice9* dev, const D3DPRIMITIVETYPE& PrimitiveType, const UINT& StartVertex, const UINT& PrimitiveCount)
	{
		if (!is_initialized() || shared::globals::imgui_is_rendering) {
			return dev->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
		}

		static auto im = imgui::get();
		im->m_stats._drawcall_prim_incl_ignored.track_single();

		auto& ctx = setup_context(dev);
		auto& ffp = shared::common::ffp_state::get();
		ffp.increment_draw_count();
		ffp.sync_zwrite(dev);
		rt_tracker::on_draw();

		// Perf category accounting + optional per-category drop.
		bool route_drop;
		{
			shared::common::ffp_prof::scope rprof(shared::common::ffp_prof::step::route);
			route_drop = perf::classify_and_filter();
		}
		if (route_drop)
		{
			ctx.restore_all(dev);
			ctx.reset_context();
			return S_OK;
		}

		// The draw's own haze set, as for indexed draws (so it never inherits the last one's tag).
		game::atmosphere::tag_haze_set(dev, ffp.view_proj_valid() && (ffp.cur_ps_light_flags() & ffp.PS_HAZE),
			ffp.ps_const_data(), ffp.vs_const_data(), ffp.frame_count());
		game::atmosphere::tag_self_lit(dev, ffp.view_proj_valid() && (ffp.cur_ps_light_flags() & ffp.PS_AMBIENT), ffp.ps_const_data(),
			ffp.probe_texture(), game::texture_names::is_lava(ffp.cur_texture(static_cast<UINT>(shared::common::config::get().ffp.albedo_stage))),
			ffp.view_proj_valid() && (ffp.cur_ps_light_flags() & ffp.PS_UNLIT), ffp.view_proj_valid() && ffp.cur_is_lit_effect_mesh(),
			ffp.view_proj_valid() && is_light_shaft_draw() && !is_atmospheric_layer());
		game::atmosphere::tag_view_fade(dev, ffp.view_proj_valid() && (ffp.cur_ps_light_flags() & ffp.PS_VIEW_FADE), ffp.ps_const_data());
		game::env_reflection::tag(dev, ffp.view_proj_valid() && (ffp.cur_ps_sampler_mask() & ((1u << 11) | (1u << 12))) == ((1u << 11) | (1u << 12)),
			ffp.ps_const_data());
		game::lightmap_sun::untag(dev);

		auto hr = S_OK;

		/*
		 * FFP draw routing for non-indexed draws.
		 *
		 * GAME-SPECIFIC: Adjust conditions if your game's non-indexed draws
		 * include world geometry that should be converted to FFP.
		 */
		// Count all world-space geometry draws for auto-enable.
		// The threshold is meant to distinguish menus from in-game scenes, so skinned
		// world draws should contribute even though their render path differs from rigid FFP.
		bool world_draw_base = ffp.view_proj_valid() &&
			ffp.last_decl() && !ffp.cur_decl_has_pos_t();
		if (world_draw_base) ffp.increment_eligible_draws();

		bool ffp_eligible_base = world_draw_base;

		// Drop prepass world draws entirely. The depth prepass draws the same geometry
		// with wrong/placeholder textures. Passing through (even via shaders) lets Remix
		// record the wrong texture for the vertex hash. RTX Remix path-traces its own
		// visibility — it doesn't use DS1's rasterized depth buffer, so skipping these
		// draws has no visible impact while preventing the texture conflict.
		if (ffp_eligible_base && ffp.is_depth_prepass())
		{
			ctx.restore_all(dev);
			ctx.reset_context();
			return S_OK;
		}

		bool ffp_eligible = ffp_eligible_base;

		if (ffp.is_enabled() && ffp_eligible)
		{
			// World-space non-indexed draw: engage FFP
			ffp.engage(dev);

			if (!ffp.setup_albedo_or_fallback(dev))
			{
				ffp.restore_textures(dev);
				ctx.restore_all(dev);
				ctx.reset_context();
				return S_OK;
			}

			// Compute vertex count from primitive type
			UINT num_verts = PrimitiveCount * 3; // conservative default (TRIANGLELIST)
			if (PrimitiveType == D3DPT_TRIANGLESTRIP || PrimitiveType == D3DPT_TRIANGLEFAN)
				num_verts = PrimitiveCount + 2;

			bool expanded = ffp.expand_rigid_texcoords(dev, StartVertex, 0, num_verts);

			if (expanded)
			{
				// Game-blended draws (edge-fade strips) are tagged as decals so
				// Remix composites them onto the surface beneath. Free-standing
				// atmospheric layers are not: a decal only survives on a surface
				// behind it, so mist or a backdrop card in front of the sky vanished.
				const bool light_shaft = !is_atmospheric_layer() && is_light_shaft_draw();
				const bool base_decal = !is_atmospheric_layer() && !light_shaft && ffp.apply_blended_decal_marker(dev);
				// A light shaft is a glow layer, as on the indexed path (its colour is the self-lit tag).
				const bool shaft_layer = light_shaft && ffp.apply_unlit_layer_marker(dev, true);
				// Hazed by the game with its own set, not the set of the wall behind it (atmosphere.hpp tag_unhazed).
				if (shaft_layer)
					game::atmosphere::tag_unhazed(dev);
				const bool tinted = ffp.apply_draw_tint(dev);
				const bool effect = ffp.apply_sp_effect(dev);
				const bool toned = ffp.apply_skin_tone(dev, false);
				const bool scrolled = game::texture_scroll::apply(dev, false, false, nullptr, nullptr,
					0, 0, 0, ffp.frame_count());
				{
					shared::common::ffp_prof::scope dprof(shared::common::ffp_prof::step::draw_base);
					hr = dev->DrawPrimitive(PrimitiveType, 0, PrimitiveCount);
				}
				if (scrolled)
					game::texture_scroll::clear(dev);
				if (toned)
					ffp.clear_skin_tone(dev);
				shared::common::ffp_prof::count_converted_draw();
				if (base_decal || shaft_layer)
					ffp.clear_decal_marker(dev);

				// Two-layer material: re-draw the overlay diffuse as a coplanar
				// alpha-blended decal pass (composited by Remix decal blending).
				if (SUCCEEDED(hr))
				{
					if (should_save_overlay_states(ffp))
						save_overlay_render_states(ctx, dev);
					if (ffp.setup_overlay_decal_pass(dev, StartVertex, 0, num_verts))
					{
						shared::common::ffp_prof::scope oprof(shared::common::ffp_prof::step::overlay_draw);
						const bool overlay_scrolled = game::texture_scroll::apply(dev, true, false, nullptr, nullptr,
							0, 0, 0, ffp.frame_count());
						const bool overlay_toned = ffp.apply_skin_tone(dev, true);
						auto overlay_hr = dev->DrawPrimitive(PrimitiveType, 0, PrimitiveCount);
						if (overlay_toned)
							ffp.clear_skin_tone(dev);
						if (overlay_scrolled)
							game::texture_scroll::clear(dev);
						ffp.clear_decal_marker(dev);
						if (FAILED(overlay_hr))
							hr = overlay_hr;
					}
				}
				if (tinted)
					ffp.clear_draw_tint(dev);
				if (effect)
					ffp.clear_sp_effect(dev);
			}
			else
			{
				ffp.disengage(dev);
				ffp.restore_textures(dev);
				hr = dev->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
				im->m_stats._drawcall_using_vs.track_single();
			}
			im->m_stats._drawcall_prim.track_single();

			ffp.restore_rigid_source(dev);
			ffp.restore_textures(dev);
		}
		else
		{
			// Passthrough: POSITIONT / no decl / pre-viewProj / skinned / FFP disabled.
			// ShaderTexFix in shader mode only expands SHORT2 UV to FLOAT2. Do not rebind
			// or null textures here — DS1's pixel shaders rely on multi-texture inputs for
			// material blends and alpha-cutout surfaces like leaves.
			ffp.disengage(dev);
			auto& cfg_dp = shared::common::config::get();
			bool do_dp_fix = cfg_dp.ffp.shader_tex_fix && ffp_eligible;
			bool dp_expanded = false;
			if (do_dp_fix)
			{
				UINT num_verts = PrimitiveCount * 3;
				if (PrimitiveType == D3DPT_TRIANGLESTRIP || PrimitiveType == D3DPT_TRIANGLEFAN)
					num_verts = PrimitiveCount + 2;
				dp_expanded = ffp.expand_skinned_texcoords(dev, StartVertex, 0, num_verts);
			}
			hr = dev->DrawPrimitive(PrimitiveType, dp_expanded ? 0 : StartVertex, PrimitiveCount);
			shared::common::ffp_prof::count_passthrough_draw();
			if (dp_expanded) ffp.restore_rigid_source(dev);
			im->m_stats._drawcall_prim.track_single();
			im->m_stats._drawcall_using_vs.track_single();
		}

		if (auto* d = diagnostics::get()) d->on_draw_primitive(ffp.draw_call_count(), PrimitiveType, StartVertex, PrimitiveCount);

		ctx.restore_all(dev);
		ctx.reset_context();

		return hr;
	}


	// ----

	HRESULT renderer::on_draw_indexed_prim(IDirect3DDevice9* dev, const D3DPRIMITIVETYPE& PrimitiveType, const INT& BaseVertexIndex, const UINT& MinVertexIndex, const UINT& NumVertices, const UINT& startIndex, const UINT& primCount)
	{
		if (!is_initialized() || shared::globals::imgui_is_rendering) {
			return dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
		}

		// The Catacombs' sky draw: as it is, then its beams as a light shaft, culling off (the sub-strip starts on an odd
		// triangle, which turns its winding).
		if (!s_beam_split && is_catacombs_sky_with_beams(dev, PrimitiveType, primCount))
		{
			s_beam_split = true;
			const HRESULT sky_hr = on_draw_indexed_prim(dev, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			DWORD cull = D3DCULL_CCW;
			dev->GetRenderState(D3DRS_CULLMODE, &cull);
			dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
			s_force_shaft = true;
			on_draw_indexed_prim(dev, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices,
				startIndex + kCatacombsBeamStart, primCount - kCatacombsBeamStart);
			s_force_shaft = false;
			dev->SetRenderState(D3DRS_CULLMODE, cull);
			s_beam_split = false;
			return sky_hr;
		}

		// A no-shadow mesh with prims no view shows: drawn around them, so Remix never gets them.
		if (!s_hidden_split && !s_beam_split && shared::common::config::get().sky.no_shadow_meshes &&
			(PrimitiveType == D3DPT_TRIANGLESTRIP || PrimitiveType == D3DPT_TRIANGLELIST) &&
			game::sky_meshes::any_hidden_prims() && shared::common::ffp_state::get().view_proj_valid() &&
			!shared::common::ffp_state::get().cur_decl_is_skinned())
		{
			UINT hide_first = 0, hide_last = 0;
			if (IDirect3DIndexBuffer9* ib = shared::common::ffp_state::get().cur_ib())
			{
				const bool hidden = game::sky_meshes::no_shadow_hidden_prims(ib, PrimitiveType, startIndex, primCount, hide_first, hide_last);
				if (hidden)
				{
					s_hidden_split = true;
					s_force_no_shadow = true;
					s_split_start = startIndex;
					s_split_prims = primCount;
					const UINT per_prim = PrimitiveType == D3DPT_TRIANGLESTRIP ? 1u : 3u;
					HRESULT split_hr = D3D_OK;
					if (hide_first > 0)
						split_hr = on_draw_indexed_prim(dev, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, hide_first);
					const UINT rest = hide_last + 1;
					if (rest < primCount)
					{
						// A strip piece that starts on an odd triangle has its winding turned: so is its culling.
						const bool turned = PrimitiveType == D3DPT_TRIANGLESTRIP && (rest & 1u);
						DWORD cull = D3DCULL_CCW;
						if (turned)
						{
							dev->GetRenderState(D3DRS_CULLMODE, &cull);
							dev->SetRenderState(D3DRS_CULLMODE, cull == D3DCULL_CW ? D3DCULL_CCW : cull == D3DCULL_CCW ? D3DCULL_CW : cull);
						}
						const HRESULT rest_hr = on_draw_indexed_prim(dev, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices,
							startIndex + rest * per_prim, primCount - rest);
						if (turned)
							dev->SetRenderState(D3DRS_CULLMODE, cull);
						if (FAILED(rest_hr))
							split_hr = rest_hr;
					}
					s_force_no_shadow = false;
					s_hidden_split = false;
					return split_hr;
				}
			}
		}

		auto& ctx = setup_context(dev);
		const auto im = imgui::get();
		auto& ffp = shared::common::ffp_state::get();
		ffp.increment_draw_count();
		ffp.sync_zwrite(dev);
		rt_tracker::on_draw();

		im->m_stats._drawcall_indexed_prim_incl_ignored.track_single();

		if (ctx.modifiers.do_not_render)
		{
			ctx.restore_all(dev);
			ctx.reset_context();
			return S_OK;
		}

		// Perf category accounting + optional per-category drop.
		bool route_drop;
		{
			shared::common::ffp_prof::scope rprof(shared::common::ffp_prof::step::route);
			route_drop = perf::classify_and_filter();
		}
		if (route_drop)
		{
			ctx.restore_all(dev);
			ctx.reset_context();
			return S_OK;
		}

		// A menu's character view (character creator, load game) keeps the game's raster draws, as its own image: these
		// are not world draws, and their render target is what the menu shows.
		if (ffp.cur_rt_is_menu_scene() && !ffp.cur_rt_is_menu_path_traced())
		{
			ffp.disengage(dev);
			const auto hr_menu_scene = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			ctx.restore_all(dev);
			ctx.reset_context();
			return hr_menu_scene;
		}

		// Draws that read the game's sun constants (shadow direction c175, key
		// light c92/c95) vote for the Remix sun. The swamp's FRPG_Snow shader reads
		// c92/c95 only for a rim lobe weighted by c169.z (0 in Blighttown): with no
		// such lobe it does not use that light, and its c95 must not colour the sun.
		const auto swamp_pass = ffp.cur_ps_swamp_pass();
		uint32_t sun_flags = ffp.cur_ps_light_flags();
		if (swamp_pass == game::swamp::pass::Surface && ffp.ps_const_data() && ffp.ps_const_data()[169 * 4 + 2] == 0.0f)
			sun_flags &= ~ffp.PS_LIGHT_DIR3;
		if (ffp.view_proj_valid() && (sun_flags & (ffp.PS_LIGHT_DIR3 | ffp.PS_LIGHT_SHADOW)))
			game::sun_light::observe(ffp.ps_const_data(), ffp.frame_count(), primCount,
				sun_flags, ffp.cur_decl_is_skinned());

		// Draws that end with the game's fog/scattering tail supply the haze constants.
		if (ffp.view_proj_valid() && (ffp.cur_ps_light_flags() & ffp.PS_HAZE))
			game::atmosphere::observe_haze(ffp.ps_const_data(), ffp.vs_const_data(), ffp.frame_count(), primCount);
		// ... and Remix hazes the draw's surfaces with its own set, as the game does.
		game::atmosphere::tag_haze_set(dev, ffp.view_proj_valid() && (ffp.cur_ps_light_flags() & ffp.PS_HAZE),
			ffp.ps_const_data(), ffp.vs_const_data(), ffp.frame_count());
		// ... and draws the game lights by a bright uniform ambient alone glow (atmosphere.hpp tag_self_lit). A light shaft with its
		// own haze at its centre's distance.
		const bool shaft_draw = ffp.view_proj_valid() && is_light_shaft_draw() && !is_atmospheric_layer();
		float shaft_distance = -1.0f;
		if (float centre[3], ox, oy, oz; shaft_draw && ffp.draw_world_centre(PrimitiveType, BaseVertexIndex, startIndex, primCount, centre) &&
			ffp.world_offset(ox, oy, oz))
			shaft_distance = std::sqrt((centre[0] - ox) * (centre[0] - ox) + (centre[1] - oy) * (centre[1] - oy) + (centre[2] - oz) * (centre[2] - oz));
		game::atmosphere::tag_self_lit(dev, ffp.view_proj_valid() && (ffp.cur_ps_light_flags() & ffp.PS_AMBIENT), ffp.ps_const_data(),
			ffp.probe_texture(), game::texture_names::is_lava(ffp.cur_texture(static_cast<UINT>(shared::common::config::get().ffp.albedo_stage))),
			ffp.view_proj_valid() && (ffp.cur_ps_light_flags() & ffp.PS_UNLIT), ffp.view_proj_valid() && ffp.cur_is_lit_effect_mesh(),
			shaft_draw, ffp.vs_const_data(), shaft_distance);
		game::atmosphere::tag_view_fade(dev, ffp.view_proj_valid() && (ffp.cur_ps_light_flags() & ffp.PS_VIEW_FADE), ffp.ps_const_data());
		game::env_reflection::tag(dev, ffp.view_proj_valid() && (ffp.cur_ps_sampler_mask() & ((1u << 11) | (1u << 12))) == ((1u << 11) | (1u << 12)),
			ffp.ps_const_data());
		// ... and lightmapped draws carry their lightmap's level, which the runtime scales the sun by.
		if (s_hidden_split)
			game::lightmap_sun::tag_draw(dev, PrimitiveType, BaseVertexIndex, s_split_start, s_split_prims, ffp.frame_count());
		else
			game::lightmap_sun::tag_draw(dev, PrimitiveType, BaseVertexIndex, startIndex, primCount, ffp.frame_count());
		// ... and emissive meshes the game gives no light get one of their own (rtx_comp\\lights\\mesh_lights.txt).
		if (!ffp.is_depth_prepass())
		{
			game::mesh_lights::observe_draw(dev, ffp.cur_texture(static_cast<UINT>(shared::common::config::get().ffp.albedo_stage)),
				PrimitiveType, BaseVertexIndex, startIndex, primCount);
			// ... and the map's waterfalls are remembered, to tell when the player stands in one.
			game::waterfall::observe_draw(dev, ffp.cur_texture(static_cast<UINT>(shared::common::config::get().ffp.albedo_stage)),
				PrimitiveType, BaseVertexIndex, startIndex, primCount);
		}

		// Probe-lit draws supply the area's ambient light (irradiance probe + hemisphere).
		if (ffp.view_proj_valid() && (ffp.cur_ps_light_flags() & ffp.PS_AMBIENT) && ffp.probe_texture())
			game::atmosphere::observe_ambient(ffp.ps_const_data(), ffp.probe_texture(), ffp.frame_count(), primCount);

		// The lights the game binds to lightmapped map draws (which map lights it treats as dynamic there).
		if (ffp.view_proj_valid() && ffp.cur_ps_reads_point_lights() && (ffp.cur_ps_sampler_mask() & (1u << 6)))
			if (auto* gl = game_lights::get())
				gl->observe_lightmapped_draw(ffp.ps_const_data(), ffp.cur_ps_point_light_count());
		// ... and which it binds to map geometry and which to characters only. Map pieces carry bone indices too
		// (one rigid bone), as do weapons and shields (a hand bone), so a rigid draw is told by its texture's name;
		// one with no recovered name counts as neither.
		if (ffp.view_proj_valid() && ffp.cur_ps_reads_point_lights())
			if (auto* gl = game_lights::get())
			{
				const auto owner = game::texture_names::owner_of(
					ffp.cur_texture(static_cast<UINT>(shared::common::config::get().ffp.albedo_stage)));
				if (ffp.cur_decl_is_skinned() || owner != game::texture_names::owner::unknown)
					gl->observe_draw_lights(ffp.ps_const_data(), ffp.cur_ps_point_light_count(),
						ffp.cur_decl_is_skinned() || owner == game::texture_names::owner::character);
			}

		// Map materials' c100 (the swamp's colours keep their ratio to them).
		if (ffp.view_proj_valid() && (ffp.cur_ps_light_flags() & ffp.PS_AMBIENT) && swamp_pass == game::swamp::pass::None)
			game::swamp::observe_map_diffuse(ffp.ps_const_data(), ffp.frame_count(), primCount,
				(ffp.cur_ps_sampler_mask() & (1u << 6)) ? ffp.cur_texture(6) : nullptr);

		// The swamp's surface constants, height field layers and wake rings.
		if (ffp.view_proj_valid() && swamp_pass != game::swamp::pass::None)
			game::swamp::observe(swamp_pass, ffp.vs_const_data(), ffp.ps_const_data(), dev, ffp.frame_count(), primCount);
		if (ffp.view_proj_valid() && swamp_pass == game::swamp::pass::WaveRing)
			game::swamp::observe_stamp(ffp.vs_const_data(), ffp.ps_const_data(), dev, ffp.stream_vb(0), ffp.stream_stride(0), BaseVertexIndex, MinVertexIndex, NumVertices,
				ffp.cur_decl_pos_off(), ffp.cur_decl_texcoord_off(), ffp.cur_decl_texcoord_type(), ffp.frame_count());
		if (ffp.view_proj_valid() && swamp_pass == game::swamp::pass::Surface)
			game::swamp::observe_surface_mesh(ffp.stream_vb(0), ffp.stream_stride(0), BaseVertexIndex, MinVertexIndex, NumVertices,
				ffp.cur_decl_pos_off(), ffp.cur_decl_texcoord_off(), ffp.cur_decl_texcoord_type(), ffp.frame_count(), primCount);

		// Water height passes: how far their mesh's waves have flowed, for its surface draw.
		if (ffp.cur_ps_is_water_heightmap())
			game::texture_scroll::observe_water_heightmap(ffp.vs_const_data(), ffp.ps_const_data(), ffp.stream_vb(0), ffp.cur_ib(),
				BaseVertexIndex, startIndex, primCount, ffp.frame_count());

		// Track draw key for dedup learning (all draw types — rigid and skinned).
		{
			shared::common::ffp_prof::scope rprof(shared::common::ffp_prof::step::route);
			ffp.record_draw_key(ffp.stream_vb(0), ffp.cur_ib(), BaseVertexIndex, startIndex, primCount);
		}

		// Log ALL draws (including dedup-skipped) so diagnostics can compare passes
		if (auto* d = diagnostics::get()) d->on_draw_indexed_prim(ffp.draw_call_count(), dev, PrimitiveType, BaseVertexIndex, startIndex, NumVertices, primCount, false);

		auto hr = S_OK;

		/*
		 * FFP draw routing for indexed draws — the main conversion path.
		 *
		 * Decision tree:
		 *   viewProjValid?
		 *   +-- NO  -> passthrough with shaders
		 *   +-- YES
		 *       +-- curDeclIsSkinned?
		 *       |   +-- dedup: keep first pass (characters correct on first pass)
		 *       |   +-- expand SHORT UVs to FLOAT2
		 *       +-- !curDeclHasNormal?
		 *       |   +-- passthrough (HUD/UI)
		 *       +-- else (rigid 3D mesh)
		 *           +-- dedup: keep per dedup_pass_mode (2=keep last recommended)
		 *           +-- tex_filter: skip global-tex/NULL draws, keep per-mesh
		 *           +-- FFP engage + draw + restore
		 */

		// Count all world geometry draws for auto-enable, including skinned meshes.
		bool idx_world_draw = ffp.view_proj_valid() && ffp.cur_decl_has_normal();
		if (idx_world_draw) ffp.increment_eligible_draws();

		// Water-surface draws (identified by pixel shader) fail every standard
		// rigid gate: the WWS decl has no NORMAL, Env water binds no stage-0
		// diffuse, Reflect water binds the per-frame reflection RT. Route them
		// through the rigid path with water-specific texture/blend handling.
		// Skinned water (FRPG_Water_Skin/WWS_PIWN) stays on its normal path.
		const bool water_draw = ffp.is_enabled() && ffp.view_proj_valid() &&
			!ffp.cur_decl_is_skinned() && ffp.cur_ps_is_water_surface() &&
			shared::common::config::get().ffp.water_surface;

		// Map sky layers (domes, far cloud layers) keep the game's shaders and
		// carry the sky marker: the runtime rasterizes them into its sky with the
		// game's own tint, fog, light scattering, scrolling and blending, which FFP
		// conversion would all drop. The water reflection pass redraws the dome
		// from the mirrored ReflectCamera; it is the only pass with a user clip
		// plane, and its copy must stay out of the sky.
		const bool sky_draw = !s_force_shaft && !water_draw && ffp.view_proj_valid() && !ffp.cur_decl_is_skinned() &&
			shared::common::config::get().sky.mark_sky_draws &&
			(is_sky_layer_draw(dev, PrimitiveType, startIndex, primCount) || is_backdrop_layer(dev));

		if (!ffp.view_proj_valid())
		{
			// Transforms not ready: passthrough.
			ffp.disengage(dev);
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			shared::common::ffp_prof::count_passthrough_draw();
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (sky_draw)
		{
			// Same prepass drop as the rigid path below.
			if (ffp.is_depth_prepass())
			{
				ctx.restore_all(dev);
				ctx.reset_context();
				return S_OK;
			}

			// The stage-0 constant is inert for shader draws; only the runtime reads it.
			ffp.disengage(dev);
			// A layer the game also draws into its light shaft source carries the shaft source marker.
			const bool shaft_source = game::shaft_source::observe(dev, ffp.cur_ib(), startIndex, primCount,
				ffp.cur_texture(0), ffp.frame_count());
			dev->SetTextureStageState(0, D3DTSS_CONSTANT, shaft_source ? game::shaft_source::kMarker : SKY_MARKER);
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			dev->SetTextureStageState(0, D3DTSS_CONSTANT, 0);
			shared::common::ffp_prof::count_passthrough_draw();
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (ffp.cur_decl_is_skinned())
		{
			// Drop skinned prepass draws for the same reason as rigid: Remix records the
			// wrong texture for the character mesh vertex hash during the depth prepass.
			if (ffp.is_depth_prepass())
			{
				ctx.restore_all(dev);
				ctx.reset_context();
				return S_OK;
			}

			// Skinned draws: no dedup — different characters share the same mesh
			// geometry but have different bone transforms per draw call.

			// FFP skinning: expand vertices, upload bones, draw with indexed vertex blending.
			// Falls back to shader passthrough with UV expansion if skinning module unavailable.
			auto* skin = skinning::get();
			if (ffp.is_enabled() && skin && ffp.num_bones() > 0 && shared::common::config::get().skinning.enabled)
			{
				// The swamp's surface carries the swamp marker: Remix shades it from the game's
				// height field (rtx.ds1Swamp, game/swamp.hpp).
				const bool swamp = swamp_pass == game::swamp::pass::Surface;
				if (swamp)
					dev->SetTextureStageState(0, D3DTSS_CONSTANT, game::swamp::kMarker);
				// A character with thin wall faces (the Moonlight Butterfly's wings: black patches otherwise).
				const bool thin_wall_skin = shared::common::config::get().sky.thin_walls && game::texture_names::is_butterfly_wing(
					ffp.cur_texture(static_cast<UINT>(shared::common::config::get().ffp.albedo_stage)));
				if (thin_wall_skin)
					dev->SetTextureStageState(4, D3DTSS_CONSTANT, kThinWallMarker);
				hr = skin->draw_skinned_dip(dev, PrimitiveType, BaseVertexIndex,
					MinVertexIndex, NumVertices, startIndex, primCount);
				if (thin_wall_skin)
					dev->SetTextureStageState(4, D3DTSS_CONSTANT, 0);
				if (swamp)
					dev->SetTextureStageState(0, D3DTSS_CONSTANT, 0);
			}
			else
			{
				ffp.disengage(dev);

				auto& cfg = shared::common::config::get();
				// Vanilla graphics: the game's VS scales the SHORT texcoords by 1/1024 itself, so expanded
				// ones (already scaled) sample a single texel.
				bool expanded = !game::vanilla_view::active() && (cfg.ffp.expand_skinned_uv || cfg.ffp.shader_tex_fix) &&
					ffp.expand_skinned_texcoords(dev, BaseVertexIndex, MinVertexIndex, NumVertices);

				// ShaderTexFix in shader mode only changes UV format. Preserve the game's
				// original texture bindings so blend masks and alpha textures keep working.

				if (expanded)
					hr = dev->DrawIndexedPrimitive(PrimitiveType, 0, MinVertexIndex, NumVertices, startIndex, primCount);
				else
					hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);

				ffp.restore_rigid_source(dev);
			}
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (!ffp.cur_decl_has_normal() && !water_draw)
		{
			// GAME-SPECIFIC: No NORMAL in vertex declaration -> likely HUD/UI.
			// Remove or modify this check if your game's world geometry lacks NORMAL elements.
			ffp.disengage(dev);
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			shared::common::ffp_prof::count_passthrough_draw();
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else
		{
			// Rigid 3D mesh with NORMAL: engage FFP conversion or apply shader-mode UV fix.
			// Water is exempt from the prepass drop: it is drawn once (in the
			// NormalDraw_Water stage), so a Z-write-enabled water draw has no
			// second Z-write-off pass that could take its place.
			if (!water_draw && ffp.is_depth_prepass())
			{
				// Drop the draw entirely — prepass geometry carries wrong/placeholder
				// textures. Even a shader passthrough lets Remix record the wrong
				// texture for this vertex hash. RTX Remix doesn't need DS1's
				// rasterized depth buffer, so these draws are safe to suppress.
				ctx.restore_all(dev);
				ctx.reset_context();
				return S_OK;
			}

			if (!ffp.is_enabled())
			{
				ffp.disengage(dev);

				auto& cfg = shared::common::config::get();
				bool uv_expanded = cfg.ffp.shader_tex_fix &&
					ffp.expand_skinned_texcoords(dev, BaseVertexIndex, MinVertexIndex, NumVertices);

				hr = dev->DrawIndexedPrimitive(PrimitiveType,
					uv_expanded ? 0 : BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);

				if (uv_expanded)
					ffp.restore_rigid_source(dev);

				im->m_stats._drawcall_indexed_prim.track_single();
				im->m_stats._drawcall_indexed_prim_using_vs.track_single();
			}
			else
			{

				// Dedup applies only to rigid draws — skinned draws lose textures when skipped.
				bool dedup_hit = ffp.check_dedup_skip(ffp.stream_vb(0), ffp.cur_ib(), BaseVertexIndex, startIndex, primCount);
				if (dedup_hit)
				{
					ctx.restore_all(dev);
					ctx.reset_context();
					return S_OK;
				}

				ffp.engage(dev);

				// Water: force the draw opaque with a static albedo. The game
				// renders water alpha-blended, but a blended FFP draw would be
				// classified as unordered translucency (or decal-composited onto
				// the bottom geometry) by Remix — an opaque surface at the water
				// plane is the correct remap target for a Remix water material.
				if (water_draw)
				{
					ctx.save_rs(dev, D3DRS_ALPHABLENDENABLE);
					ctx.save_rs(dev, D3DRS_ALPHATESTENABLE);
					dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
					dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
				}

				// Set albedo texture: use per-mesh diffuse if available,
				// white fallback for globally-shared textures, skip if NULL.
				const bool albedo_ok = water_draw
					? ffp.setup_water_albedo(dev)
					: ffp.setup_albedo_or_fallback(dev);
				if (!albedo_ok)
				{
					ffp.restore_textures(dev);
					ctx.restore_all(dev);
					ctx.reset_context();
					return S_OK;
				}

				// A wall of two single-sided faces turned towards each other goes to Remix tagged: only the meshes in the
				// confirmed table (sky_meshes::is_thin_wall_draw). Measuring every drawn mesh, and listing every mesh the rule
				// found offline, tagged walls nothing had shown black, and lit through them (Undead Asylum, 2026-11-07).
				const bool inside_out = !water_draw && shared::common::config::get().sky.thin_walls &&
					game::sky_meshes::is_thin_wall_draw(ffp.cur_ib(), PrimitiveType, startIndex, primCount);

				// Expand vertices to clean FLOAT3+FLOAT3+FLOAT2 for FFP compatibility
				bool expanded = ffp.expand_rigid_texcoords(dev, BaseVertexIndex, MinVertexIndex, NumVertices);

				if (expanded)
				{
					// Game-blended draws (edge-fade strips) are tagged as decals so
					// Remix composites them onto the surface beneath. Free-standing
					// atmospheric layers are not: a decal only survives on a surface
					// behind it, so mist or a backdrop card in front of the sky vanished.
					// Nor are blended shells (sky_meshes::is_shell_draw): they do not lie on
					// a surface, so as decals they took the lighting of whatever lay behind
					// them; they are shaded as their own surface instead ([Sky] ShellMode).
					// Nor is the Estus flask (game/estus.hpp), which goes opaque: the game draws it twice at 0.86, so it
					// is all but solid there, and a decal needs something behind it.
					const int shell_mode = shared::common::config::get().sky.shell_mode;
					const bool flask = !water_draw && game::texture_names::is_estus_flask(
						ffp.cur_texture(static_cast<UINT>(shared::common::config::get().ffp.albedo_stage)));
					const bool shell = flask || (shell_mode != 0 &&
						game::sky_meshes::is_shell_draw(ffp.cur_ib(), PrimitiveType, startIndex, primCount));
					// Window glass is split into its came and its glass (game/glass.hpp): the base
					// draw becomes the came, and neither pass is a decal.
					DWORD glass_cut = 0;
					IDirect3DBaseTexture9* const glass_texture = water_draw ? nullptr :
						glass_split_texture(dev, ffp, PrimitiveType, startIndex, primCount, glass_cut);
					if (glass_texture)
						apply_came_pass_states(ctx, dev, glass_cut);
					// A light shaft hangs in the air, a glow layer like the unlit ones: never a decal.
					const bool light_shaft = !water_draw && is_light_shaft_draw();
					const bool base_decal = !shell && !glass_texture && !light_shaft && !is_atmospheric_layer() && ffp.apply_blended_decal_marker(dev);
					// Alpha-blended unlit glow layers take the sprites' unlit marker instead (ffp_state).
					const bool unlit_layer = !base_decal && !shell && !glass_texture && !is_atmospheric_layer() &&
						ffp.apply_unlit_layer_marker(dev, light_shaft);
					// Hazed by the game with its own set, not the set of the wall behind it (atmosphere.hpp tag_unhazed).
					if (unlit_layer && light_shaft)
						game::atmosphere::tag_unhazed(dev);
					if (shell && (shell_mode == 2 || flask))
					{
						// Unblended and alpha-tested, Remix renders it as an opaque cutout (as the glass came pass).
						save_overlay_render_states(ctx, dev);
						ctx.save_rs(dev, D3DRS_ALPHAFUNC);
						ctx.save_rs(dev, D3DRS_ALPHAREF);
						dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
						dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
						dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
						dev->SetRenderState(D3DRS_ALPHAREF, kShellCutoutAlphaRef);
						dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
						// The flask has no holes. And it must be fully opaque for its subsurface scattering: the sample rays
						// are a ray query that commits no non-opaque hit, so they miss a cutout and its diffuse goes black.
						if (flask)
							dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
					}
					const bool tinted = ffp.apply_draw_tint(dev);
					const bool effect = ffp.apply_sp_effect(dev);
					if (inside_out)
						dev->SetTextureStageState(4, D3DTSS_CONSTANT, kThinWallMarker);
					// A lamp holds its light inside: lighting rays pass it (game/mesh_lights.hpp).
					const bool lamp = game::mesh_lights::is_lamp(ffp.cur_texture(static_cast<UINT>(shared::common::config::get().ffp.albedo_stage)));
					// So do the faces in rtx_comp\sky\no_shadow_meshes.txt (game/sky_meshes.hpp): same marker.
					const bool no_shadow = !water_draw && shared::common::config::get().sky.no_shadow_meshes &&
						(s_force_no_shadow || game::sky_meshes::is_no_shadow_draw(ffp.cur_ib(), PrimitiveType, startIndex, primCount));
					if (lamp || no_shadow)
						dev->SetTextureStageState(4, D3DTSS_CONSTANT, kLampMarker);
					// Leaf cards the game lights without occlusion let indirect light through in part (Surface::isDs1Foliage). Only in Firelink
					// Shrine (m10_02), where it was seen: the same textures in New Londo and the Asylum are not checked.
					const bool foliage = !water_draw && !inside_out && !lamp && !no_shadow &&
						game::texture_names::is_foliage(ffp.cur_texture(static_cast<UINT>(shared::common::config::get().ffp.albedo_stage))) &&
						((game::map_lights::current_map_id() >> 16) & 0xFFFF) == ((10u << 8) | 2u);
					if (foliage)
						dev->SetTextureStageState(4, D3DTSS_CONSTANT, kFoliageMarker);
					// Effects the game lights only briefly get a stand-in light (lightning breath bolts).
					game::effect_lights::observe_draw(dev,
						ffp.cur_texture(static_cast<UINT>(shared::common::config::get().ffp.albedo_stage)),
						ffp.vs_const_data(), ffp.ps_const_data());
					// The swamp's surface carries the swamp marker (see the skinned path).
					const bool swamp = !base_decal && swamp_pass == game::swamp::pass::Surface;
					if (swamp)
					{
						dev->SetTextureStageState(0, D3DTSS_CONSTANT, game::swamp::kMarker);
						// With its height texture in the alpha when that is a texture of its own (restored below
						// with the rest of the stages).
						if (auto* surface = game::swamp::surface_texture(dev,
							ffp.cur_texture(static_cast<UINT>(shared::common::config::get().ffp.albedo_stage))))
							dev->SetTexture(0, surface);
					}
					// Restored below with the rest of the stages.
					if (flask)
						if (auto* flask_texture = game::estus::flask_texture(dev, ffp.vs_const_data()))
							dev->SetTexture(0, flask_texture);
					// The source VB and IB, as bound by the game (the expanded VB replaced stream 0).
					IDirect3DVertexBuffer9* const mesh_vb = ffp.stream_vb(0);
					IDirect3DIndexBuffer9* const mesh_ib = ffp.cur_ib();
					const bool scrolled = game::texture_scroll::apply(dev, false, water_draw, mesh_vb, mesh_ib,
						BaseVertexIndex, startIndex, primCount, ffp.frame_count());
					const bool toned = ffp.apply_skin_tone(dev, false);
					{
						shared::common::ffp_prof::scope dprof(shared::common::ffp_prof::step::draw_base);
						hr = dev->DrawIndexedPrimitive(PrimitiveType, 0, MinVertexIndex, NumVertices, startIndex, primCount);
					}
					if (toned)
						ffp.clear_skin_tone(dev);
					if (scrolled)
						game::texture_scroll::clear(dev);
					if (swamp)
						dev->SetTextureStageState(0, D3DTSS_CONSTANT, 0);
					shared::common::ffp_prof::count_converted_draw();
					if (base_decal || unlit_layer)
						ffp.clear_decal_marker(dev);

					if (glass_texture && SUCCEEDED(hr) &&
						ffp.setup_glass_pass(dev, BaseVertexIndex, MinVertexIndex, NumVertices, glass_texture))
					{
						dev->SetTextureStageState(4, D3DTSS_CONSTANT, kSingleSidedGlassMarker);
						const auto glass_hr = dev->DrawIndexedPrimitive(PrimitiveType, 0, MinVertexIndex, NumVertices, startIndex, primCount);
						dev->SetTextureStageState(4, D3DTSS_CONSTANT, 0);
						shared::common::ffp_prof::count_converted_draw();
						if (FAILED(glass_hr))
							hr = glass_hr;
					}

					static int water_log_budget = 16;
					if (water_draw && water_log_budget > 0)
					{
						water_log_budget--;
						shared::common::log("WATER", std::format(
							"converted water draw #{} verts={} prims={} hasNormal={} hasUV={}",
							ffp.draw_call_count(), NumVertices, primCount,
							ffp.cur_decl_has_normal() ? 1 : 0,
							ffp.cur_decl_has_texcoord() ? 1 : 0));
					}

					// Two-layer material: re-draw the overlay diffuse as a coplanar
					// alpha-blended decal pass (composited by Remix decal blending).
					if (SUCCEEDED(hr))
					{
						if (should_save_overlay_states(ffp))
							save_overlay_render_states(ctx, dev);
						if (ffp.setup_overlay_decal_pass(dev, BaseVertexIndex, MinVertexIndex, NumVertices))
						{
							shared::common::ffp_prof::scope oprof(shared::common::ffp_prof::step::overlay_draw);
							const bool overlay_scrolled = game::texture_scroll::apply(dev, true, false, mesh_vb, mesh_ib,
								BaseVertexIndex, startIndex, primCount, ffp.frame_count());
							const bool overlay_toned = ffp.apply_skin_tone(dev, true);
							auto overlay_hr = dev->DrawIndexedPrimitive(PrimitiveType, 0, MinVertexIndex, NumVertices, startIndex, primCount);
							if (overlay_toned)
								ffp.clear_skin_tone(dev);
							if (overlay_scrolled)
								game::texture_scroll::clear(dev);
							ffp.clear_decal_marker(dev);
							if (FAILED(overlay_hr))
								hr = overlay_hr;
						}
					}
					if (tinted)
						ffp.clear_draw_tint(dev);
					if (effect)
						ffp.clear_sp_effect(dev);
					if (inside_out || lamp || no_shadow || foliage)
						dev->SetTextureStageState(4, D3DTSS_CONSTANT, 0);
				}
				else
				{
					// Expansion failed — fall back to shader passthrough to avoid
					// FFP draw with potentially unsupported vertex types (e.g. UBYTE4N normals)
					ffp.disengage(dev);
					ffp.restore_textures(dev);
					hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
					im->m_stats._drawcall_indexed_prim_using_vs.track_single();
				}
				im->m_stats._drawcall_indexed_prim.track_single();

				ffp.restore_rigid_source(dev);
				ffp.restore_textures(dev);
			}
		}

		ctx.restore_all(dev);
		ctx.reset_context();

		return hr;
	}

	// ---

	void renderer::manually_trigger_remix_injection(IDirect3DDevice9* dev)
	{
		if (!m_triggered_remix_injection)
		{
			auto& ctx = dc_ctx;

			dev->SetRenderState(D3DRS_FOGENABLE, FALSE);

			ctx.save_vs(dev);
			dev->SetVertexShader(nullptr);
			ctx.save_ps(dev);
			dev->SetPixelShader(nullptr);

			ctx.save_rs(dev, D3DRS_ZWRITEENABLE);
			dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

			IDirect3DVertexDeclaration9* saved_decl = nullptr;
			dev->GetVertexDeclaration(&saved_decl);
			dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

			struct CUSTOMVERTEX
			{
				float x, y, z, rhw;
				D3DCOLOR color;
			};

			const auto color = D3DCOLOR_COLORVALUE(0, 0, 0, 0);
			const auto w = -0.49f;
			const auto h = -0.495f;

			CUSTOMVERTEX vertices[] =
			{
				{ -0.5f, -0.5f, 0.0f, 1.0f, color },
				{     w, -0.5f, 0.0f, 1.0f, color },
				{ -0.5f,     h, 0.0f, 1.0f, color },
				{     w,     h, 0.0f, 1.0f, color }
			};

			dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(CUSTOMVERTEX));

			if (saved_decl)
			{
				dev->SetVertexDeclaration(saved_decl);
				saved_decl->Release();
			}

			ctx.restore_vs(dev);
			ctx.restore_ps(dev);
			ctx.restore_render_state(dev, D3DRS_ZWRITEENABLE);
			m_triggered_remix_injection = true;
		}
	}


	renderer::renderer()
	{
		p_this = this;

		// Initialize FFP state tracker
		shared::common::ffp_state::get().init(shared::globals::d3d_device);

		// Initialize render target tracker
		rt_tracker::init(shared::globals::d3d_device);

		// GAME-SPECIFIC: Create hooks as required.
		// See documentation for per-object hook examples.

		m_initialized = true;
		shared::common::log("Renderer", "Module initialized.", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}

	renderer::~renderer()
	{
		tex_addons::init_texture_addons(true);
		p_this = nullptr;
	}
}
