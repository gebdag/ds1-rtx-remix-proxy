#pragma once

namespace shared::common
{
	class config
	{
	public:
		static config& get();

		void load(const std::string& ini_path);
		bool is_loaded() const { return loaded_; }

		int get_int(const char* section, const char* key, int default_val) const;
		std::string get_string(const char* section, const char* key, const char* default_val) const;
		float get_float(const char* section, const char* key, float default_val) const;
		bool get_bool(const char* section, const char* key, bool default_val) const;

		struct ffp_settings
		{
			bool enabled = true;
			bool auto_enable = false;
			int auto_enable_threshold = 50;
			int auto_enable_frames = 5;
			int albedo_stage = 0;
			// 0=off, 1=keep first, 2=keep last, 3=keep all
			int dedup_pass_mode = 0;
			bool show_dedup_ui = false;
			bool tex_filter = false;
			int tex_filter_threshold = 80;
			bool expand_skinned_uv = true;
			bool skip_postfx = true;
			// Suppress FFP conversion when D3DRS_ZWRITEENABLE is TRUE (depth-building
			// passes). DS1 uses a depth-prepass pattern: the same geometry is drawn
			// first with Z-write ON (building depth), then again with Z-write OFF
			// (main render reading from pre-built depth). Only the Z-write-OFF pass
			// has correct diffuse textures and should be sent to Remix via FFP.
			bool suppress_aux_passes = true;
			// Fix UV format for Remix compatibility without full FFP conversion.
			// DS1 stores texcoords as SHORT2 with 1/1024 scale applied in the vertex shader.
			// RTX Remix reads UV directly from the vertex buffer and cannot interpret raw SHORT
			// integer values as UV coordinates. When enabled, world-geometry draws in shader mode
			// expand SHORT2/SHORT4 texcoords to FLOAT2 with 1/1024 scale applied while preserving
			// the game's original texture bindings, blend masks, and alpha-tested materials.
			bool shader_tex_fix = false;
			// DS1 multi-layer diffuse compositing.
			// Two-layer map materials ([M] MTDs) carry both UV pairs in one SHORT4
			// TEXCOORD0 and lerp the second diffuse (PS stage 3) over the base by
			// painted vertex-color alpha. When enabled, the base layer renders as a
			// normal opaque FFP draw and the overlay is re-drawn as a coplanar
			// alpha-blended pass from its own VB (second UV set, vertex alpha as
			// opacity), tagged via a D3DTSS_CONSTANT marker for the custom RTX Remix
			// runtime (rtx.fixedFunctionDecalMarkerEnabled) which classifies it as a
			// static decal and composites it onto the base surface in the path tracer.
			bool texture_layer_blend = true;
			// Convert water-surface draws (FRPG_Water_Env*/Reflect*, FRPG_WWS_Dif*)
			// to FFP. These draws fail every standard rigid gate — the WWS decl has
			// no NORMAL, Env binds no stage-0 diffuse, Reflect binds the per-frame
			// reflection RT — so without this path water is absent from the
			// path-traced scene. The FFP water draw is forced opaque with a static
			// albedo; assign a translucent water material in Remix by texture hash.
			bool water_surface = true;
			// Albedo policy for Env/Reflect water (no static stage-0 diffuse
			// exists): when true, bind the generated solid-color fallback —
			// its fresh hash cannot collide with rtx.conf ignore/hide lists.
			// When false, bind the first static (non-RT) texture on s0-s7
			// (the scroll normal maps), which is only useful for diagnosing
			// texture-hash filtering: those hashes may be ignored/hidden in
			// rtx.conf, making the water instance vanish.
			bool water_fallback_albedo = true;
			// Query D3DRS_ZWRITEENABLE from the device on every draw (one extra
			// bridge round-trip per draw). Catches changes applied through
			// StateBlock::Apply that the SetRenderState intercept can't see.
			// When off, the prepass gate uses the passively tracked value.
			bool zwrite_query_per_draw = true;
		} ffp;

		struct postfx_settings
		{
			std::unordered_set<int> skip_passes; // pass indices to skip
			std::string skip_passes_str;          // editable text for ImGui
			int pass_count = 0;                    // observed count last frame (read-only)

			void parse_skip_string();
			void rebuild_skip_string();
		} postfx;

		struct skinning_settings
		{
			bool enabled = false;
		} skinning;

		struct cheats_settings
		{
			bool god_mode = false;
		} cheats;

		struct diagnostics_settings
		{
			bool enabled = true;
			bool auto_capture = true;
			int delay_ms = 50000;
			int log_frames = 3;

			// Log categories (defaults, overridable from ImGui at runtime)
			bool log_draw_calls = true;
			bool log_vs_constants = true;
			bool log_vertex_data = true;
			bool log_declarations = true;
			bool log_textures = true;
			bool log_present_info = true;
		} diagnostics;

		struct remix_settings
		{
			bool enabled = true;
			std::string dll_name = "d3d9_remix.dll";
		} remix;

		struct chain_settings
		{
			std::string preload;   // semicolon-separated DLLs/ASIs loaded before d3d9 chain
			std::string postload;  // semicolon-separated DLLs/ASIs loaded after init
		} chain;

		struct intro_skip_settings
		{
			bool enabled = true;
		} intro_skip;

		struct fps_unlock_settings
		{
			bool enabled = false;
			int target_fps = 60;
		} fps_unlock;

		struct culling_settings
		{
			bool enable_frustum_culling = true;
			float frustum_relaxation_radius = 0.0f;
		} culling;

		struct tracer_settings
		{
			int backtrace_depth = 8;
			std::string output_dir = "captures";
		} tracer;

		struct lights_settings
		{
			bool enabled = true;
			float intensity_scale = 1.0f;
			float radius_scale = 1.0f;
			// Resolve the DLRF handle on each KPointLightMan slot and read the light's
			// parameters through its own GetLightParams. Without this only map lights
			// placed as MSB Light events survive: FX/SFX lights (Blighttown's torches,
			// spell and pickup lights) keep their colour on the entity, never on a row.
			bool resolve_entity_params = true;
			// Inject the stand-in lights in rtx_comp\lights\<map>.txt for maps the
			// engine never lights (Blighttown's torches and friends).
			bool synthesized_lights = true;
		} lights;

		struct sun_settings
		{
			// Mirror the game's key directional light (pixel shader c92/c95, the
			// light its cascaded shadow map shades) into Remix as a distant light.
			bool enabled = true;
			// Multiplier on the game's light colour. 1.0 gives the lambert term the
			// game's own shader computes, before its exposure (c135).
			float intensity_scale = 1.0f;
			// Apparent size of the sun disc; sets how soft the shadows are.
			float angular_diameter = 0.53f;
			// Sun colour until a HemDir3 draw has shown the game's own (c95). Maps
			// lit by irradiance cubes (HemEnv, e.g. Sen's Fortress) never bind it to a
			// map shader. LIGHT_BANK light 0 is typically white at 50-100%.
			float fallback_colour[3] = { 0.7f, 0.7f, 0.7f };
		} sun;

		struct hud_settings
		{
			// Tag EzMenu HUD/menu quads with the stage-0 D3DTSS_CONSTANT UI marker
			// so the custom Remix runtime (rtx.uiMarkerEnabled) classifies them as
			// UI without rtx.uiTextures hash lists.
			bool mark_ui_draws = true;
		} hud;

		struct sky_settings
		{
			// Send the meshes of the map sky models (domes, far cloud layers and
			// the backdrop they are blended over, recognised by their index data
			// via rtx_comp\sky\sky_meshes.txt) through with the game's own shaders
			// and the stage-0 D3DTSS_CONSTANT sky marker, so the custom Remix runtime
			// (rtx.skyMarkerEnabled) rasterizes them into its sky exactly as the
			// game shades them: tint, fog, light scattering, scrolling, blending.
			bool mark_sky_draws = true;
		} sky;

		struct resolution_settings
		{
			UINT render_width = 0;   // 0 = no override (use game default)
			UINT render_height = 0;
			UINT present_width = 0;  // 0 = same as render_width
			UINT present_height = 0; // 0 = same as render_height
		} resolution;

		struct benchmark_settings
		{
			int settle_frames = 90;   // frames to wait after switching options
			int measure_frames = 240; // frames measured per phase
		} benchmark;

	private:
		std::string ini_path_;
		bool loaded_ = false;

		void parse_all();
	};
}
