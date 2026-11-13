#pragma once

#include <unordered_map>

namespace shared::common
{
	class config
	{
	public:
		static config& get();

		void load(const std::string& ini_path);
		bool is_loaded() const { return loaded_; }

		// A second INI whose keys win over the loaded one, if the file exists. The
		// lookdev host uses remix-comp-proxy.host.ini for experiments that must not
		// touch the game's own INI.
		void set_overlay(const std::string& path);

		// Keys ("Section.Key") that win over both INIs while the player is in an area (game/area_overrides), and are
		// left out when the INI is written. Re-reads the settings: anything changed in the menu and not saved is lost.
		void set_area_values(std::unordered_map<std::string, std::string> values);

		// Before load: the lookdev modes (-lookdev, -lookdev-host) are tools that capture and edit options live, so they run
		// with dev mode on whatever the INI says.
		void force_dev_mode() { dev_forced_ = true; }

		// [Dev] Enabled: the developer's tools. Off (the default), they are not only hidden but inert: the F4 tabs FFP, Perf,
		// Cheats, Debug, Cutscenes and Dev, the hotkeys other than F4, the cheat hooks and [Cheats] keys, the tracer, frame
		// captures, live option files and dev dumps. Read once at load: area overrides and the host overlay do not change it.
		struct dev_settings
		{
			bool enabled = false;
			// F4's "Dev view" checkbox off (dev mode only, this session): the menu and hotkeys as a player has them. What dev
			// mode installed at start (hooks, file triggers, logs) stays; only what a player sees and presses follows it.
			bool user_view = false;
		} dev;

		// Dev mode's menu and hotkeys showing: dev mode, and not switched to the player's view in F4.
		bool dev_view() const { return dev.enabled && !dev.user_view; }

		// The logs a player does not need (camera.log, eye_log.txt, hitch lines): with dev mode, or with
		// [Troubleshooting] Verbose=1 when a player is asked for them.
		bool dev_logs() const { return dev.enabled || troubleshooting.verbose; }

		int get_int(const char* section, const char* key, int default_val) const;
		std::string get_string(const char* section, const char* key, const char* default_val) const;
		float get_float(const char* section, const char* key, float default_val) const;
		bool get_bool(const char* section, const char* key, bool default_val) const;
		// "x, y, z" -> out; leaves out untouched when the key is missing or malformed.
		bool get_float3(const char* section, const char* key, float* out) const;

		// Writes one key back to the loaded INI. False when no INI is loaded or the
		// write fails.
		bool set_string(const char* section, const char* key, const std::string& value) const;
		const std::string& ini_path() const { return ini_path_; }

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
			bool menu_scene_path_trace = false;
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
			// Carry the per-draw colour and fade (PS c139) of converted draws into
			// Remix. Every FRPG pixel shader that reads c139 ends with
			// rgb *= c139.rgb, a *= c139.w (before fog); FFP conversion drops the
			// shader, so without this fire meshes lose their orange tint and their
			// fade-out, and fading map pieces/characters pop. The value rides as
			// D3DRS_TEXTUREFACTOR on stage 1 (MODULATE CURRENT x TFACTOR, colour and
			// alpha), which the fork runtime applies to albedo, emissive and opacity.
			bool draw_tint = true;
			// Albedo policy for Env/Reflect water (no static stage-0 diffuse
			// exists): when true, bind the generated solid-color fallback —
			// its fresh hash cannot collide with rtx.conf ignore/hide lists.
			// When false, bind the first static (non-RT) texture on s0-s7
			// (the scroll normal maps), which is only useful for diagnosing
			// texture-hash filtering: those hashes may be ignored/hidden in
			// rtx.conf, making the water instance vanish.
			bool water_fallback_albedo = true;
			// [FFP] WaterAreaStandIn: 1 = the water draws with the second stand-in colour (0xFF2F5D74 instead of 0xFF2F5D73), so its
			// own mod material applies (ds1_water: New Londo's murk). Meant for area_overrides.ini, not the main INI.
			int water_area_stand_in = 0;
			// [FFP] WaterReflectionScale: scale per channel on the reflection level of water that takes the game's look (Ash Lake), on
			// top of the game's own tint and weight (c132 * c131). The game's reflection is a small image sampled far off its mirror
			// point (c124), so its water shows the sky's brightness where a traced ray meets the dark far shore. Meant for
			// area_overrides.ini; the product is sent in 8 bits, so it ends at 1 (Ash Lake's level 0.21 leaves room up to 4.7).
			float water_reflection_scale[3] = { 1.0f, 1.0f, 1.0f };
			// Query D3DRS_ZWRITEENABLE from the device on every draw (one extra
			// bridge round-trip per draw). Catches changes applied through
			// StateBlock::Apply that the SetRenderState intercept can't see.
			// When off, the prepass gate uses the passively tracked value.
			bool zwrite_query_per_draw = true;
		} ffp;

		/*
		 * Per-step switches for the FFP conversion pipeline ([FFPSteps]).
		 *
		 * Every default here reproduces the stock behaviour exactly, so the
		 * section is inert until something is changed. They exist so each step
		 * of the conversion can be turned off (or made lazy) live from the Perf
		 * tab and its real cost read off the frame rate — the proxy-side timer
		 * cannot see the bridge server or the GPU, so measuring by subtraction
		 * is the only honest way to attribute cost.
		 *
		 * The tri-state ints all follow the same scale:
		 *   2 = every draw (stock)   1 = only when it changed   0 = never
		 * Level 1 is a genuine optimisation, not just a probe: if it costs
		 * nothing visually it can simply be left on.
		 */
		struct ffp_step_settings
		{
			// Master switch for the profiler itself. Off by default — the
			// scopes cost a bool test when disabled, two QPC reads when on.
			bool profile = false;

			// setup_texture_stages: 29 SetTextureStageState per draw. The state
			// is identical for nearly every converted draw, so level 1 applies
			// it once and only re-applies after something else has disturbed it.
			int tex_stages = 2;

			// apply_transforms: re-applies VIEW/PROJECTION/WORLD on every draw
			// even when no VS constant changed. Level 1 skips the redundant
			// SetTransform calls.
			int transforms = 2;

			// restore_textures: 8 SetTexture after every converted draw.
			// Level 1 restores only the stages the conversion actually touched.
			int restore_textures = 2;

			// Null stages 1-7 in setup_albedo_or_fallback. Off binds stage 0
			// only and leaves the rest, relying on the stage-disable state from
			// setup_texture_stages to keep them out of the material.
			bool albedo_null_stages = true;

			// Serve a cached expanded VB when the source vertex range is
			// unchanged, instead of re-locking, re-converting and re-uploading
			// it every draw. This is the single biggest lever: a re-written VB
			// is memcpy'd into the bridge IPC channel on Unlock and re-uploaded
			// by the runtime every frame.
			//
			// Correctness note: the stock code deliberately never serves cached
			// data because the game can destroy and re-create a VB at the same
			// address, which would make a pointer-keyed hit return garbage. The
			// reuse path therefore validates with a cheap content fingerprint
			// (sampled bytes + geometry dimensions) rather than trusting the
			// pointer. That is strong but not a proof — turn it off first when
			// any geometry corruption is reported.
			bool expand_reuse = true;

			// apply_blended_decal_marker's 3 GetRenderState per draw.
			bool decal_query = true;

			// save_overlay_render_states is currently called before the overlay
			// pass is known to be needed, so all 7 GetRenderState are paid on
			// every converted draw. When true they are saved only once the
			// overlay is actually going to be drawn.
			bool overlay_save_lazy = false;

			// Skinned draws upload only the bones their weighted vertices reach,
			// not every bone the palette ever held (83 in Blighttown, ~4 used).
			bool skin_upload_used_bones = true;

			// Hand the game's per-frame depth-stencil surfaces out again instead
			// of creating new ones, each of which waits for the Remix server.
			bool pool_depth_surfaces = true;

			// No ImGui render pass when nothing is drawn (menu closed, no overlay text). The pass creates a state block and locks
			// two buffers every frame; through the bridge each of those waits until the Remix server has caught up with every
			// call before it, so the game stopped once a frame for Remix, and a slow Remix moment became a hitch.
			bool imgui_skip_empty = true;
		} ffp_steps;

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

		// [Cheats]: read only with dev mode; a player's INI cannot turn them on.
		struct cheats_settings
		{
			bool god_mode = false;
			bool enemies_ignore_player = false;
			bool no_fall_damage = false;
		} cheats;

		// Enabled and AutoCapture default to dev mode and stay off without it, whatever the INI says: the
		// auto capture writes rtx_comp\diagnostics.log 50 s into every session.
		struct diagnostics_settings
		{
			bool enabled = false;
			bool auto_capture = false;
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

		// [Troubleshooting] Verbose=1: more in the log for tracking down crashes and missing pieces. A startup report of
		// every file and library the mod needs (and which are missing), every module loaded in the game with its
		// version, and on a crash the whole crash report (exception, registers, stack, recent events) in the log, with
		// the console kept open until Enter is pressed. Off by default; the normal log is always written.
		struct troubleshooting_settings
		{
			bool verbose = false;
		} troubleshooting;

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
			bool keep_light_blockers = true;
			float light_blocker_min_size = 8.0f;
			float light_blocker_near_range = 50.0f;
			float light_blocker_sun_reach = 200.0f;
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
			bool synthesized_lights = false;
			// Scale on the lights stood in for fire effects the game does not light (a fire breath;
			// game::effect_lights): 1, the default, = the game's own torch to bonfire lights. 2 was the default while
			// every game light reached Remix at a third of its strength (game_lights.cpp); with them corrected it lit a
			// fireball's surroundings three times as bright as before.
			float fire_light_scale = 1.0f;
			float effect_table_scale = 1.0f; // [Lights] EffectTableScale, the colour of table effect lights: fire 1.8x in light, as the game's own fire lights
		} lights;

		// [FogGate] Enable: fog gates as the runtime's volume instead of the game's sprites (game/fog_gates.hpp).
		bool fog_gate_enable = false; // a prototype, off for now

		// The light the game hangs on the player's head (a LerpBank point light on a
		// character dummy-poly, coloured by the current area's POINT_LIGHT_BANK). It is
		// split out of the harvested lights so it can be tuned on its own; [Lights]
		// scales do not touch it. Each Use* key follows the game's value when 1.
		struct player_light_settings
		{
			bool enabled = true;
			bool use_game_colour = true;
			float colour[3] = { 1.0f, 0.82f, 0.51f };
			bool use_game_range = true;
			float range = 7.0f;          // dwindle end: sets the radiance, like the other lights
			bool use_game_radius = true;
			float radius = 0.5f;         // emitter sphere radius
			float intensity = 1.0f;      // radiance multiplier (inverse-square light)
			// With the game's falloff: a multiplier on colour^2.2 / pi (3.14 would be the game's own strength head-on). 0.7 since
			// 2026-11-11, the user's call: at 1 the player and the ground round them read too bright.
			float falloff_intensity = 0.7f;
			// With the game's falloff: scales the game's dwindle begin and end together (1 = the
			// game's reach). Range / UseGameRange apply to the inverse-square light only.
			float falloff_range_scale = 1.0f;
			float falloff_max_colour = 0.0f; // the strongest channel the game's colour may reach, 0 = no limit
			bool falloff_gamma_add = true;   // its colour as it comes out added to the area's ambient in gamma, as the game adds it
			// How Offset is applied: 0 world axes (x, y, z); 1 view: turns with the camera
			// heading (right, up, forward); 2 character: turns with the body (right, up,
			// forward). View and character use the heading only, so the light stays level.
			int offset_frame = 1;
			float offset[3] = {};
		} player_light;

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

		// [HDR] HDR10 output, with the hdr10 runtime (rtx.hdr.*). The game composites Remix's frame into its own
		// 8-bit scene target and copies that to the back buffer through its brightness LUT, which would clip
		// everything above SDR white: with this on, the game's back-buffer-sized 8-bit render textures are created
		// as A16B16G16R16F and the final LUT pass is a plain copy (the LUT is identity at default brightness; HDR
		// has its own paper-white setting), and rtx.hdr.enable is sent to the runtime. Textures are created at
		// start and on a resolution change, so a toggle takes full effect at the next game start.
		struct hdr_settings
		{
			bool enabled = false;
		} hdr;

		// DS1's particles (FRPG_Sfx_* pixel shaders) reach Remix as shader draws,
		// which it reads through the stage-0 texture stage state, not the shader.
		struct sfx_settings
		{
			// Draw sprite particles (Sprite / SoftSprite / LitSprite UP draws) fixed-
			// function in the converted geometry's space: their shaders use an absolute
			// view-projection that the camera-relative transforms Remix is given cannot
			// place, so as shader draws they land elsewhere or nowhere.
			bool ffp_sprites = true;
			// Diagnostic: draw those sprites with blending and alpha test off, as solid
			// quads, to see where they land.
			bool debug_opaque = false;
			// Sprites on the game's plain white textures (s00917, a fog gate's quad) are drawn with the proxy's own white texture:
			// Remix finds a mod's material by the texture's content, and a character part with the same plain white diffuse (the
			// ghost armour's arms) gave its additive ghost material to the gate's quad, which then covered nothing.
			bool own_white = true;
			// Sprites that take the game's own fog and scattering for them in the proxy (set_own_fog_test: the wading droplets, s00027),
			// and none of the look's haze, which only knows the surfaces' sets. Off: as every other sprite.
			bool own_fog = true;
			// The smoke the bonfire's rest effect throws round the camera as the player sits down (d3d9ex sit_smoke_left_out):
			// left out of the Remix view. On: drawn as every other sprite.
			bool sit_smoke = false;
			// Express what each particle shader computes as stage state Remix reads:
			// colour and alpha = texture * vertex colour. Off leaves whatever stage
			// state the last converted draw left (alpha often texture-only).
			bool translate = true;
			// Drop the particle draws Remix can only get wrong: depth-encoding passes
			// (encoded depth in rgb), frame-copy distortion (heat haze) and blurs.
			bool drop_screen_effects = true;
			// Diagnostic: drop every particle draw, to see what the rest of the frame
			// contributes on its own.
			bool hide = false;
		} sfx;

		struct sky_settings
		{
			// Send the meshes of the map sky models (domes, far cloud layers and
			// the backdrop they are blended over, recognised by their index data
			// via rtx_comp\mesh_tables\sky_meshes.txt) through with the game's own shaders
			// and the stage-0 D3DTSS_CONSTANT sky marker, so the custom Remix runtime
			// (rtx.skyMarkerEnabled) rasterizes them into its sky exactly as the
			// game shades them: tint, fog, light scattering, scrolling, blending.
			bool mark_sky_draws = true;
		} sky;

		// [Meshes] (old name [Sky], still read): what the per-mesh tables (game/mesh_tables.hpp) do.
		struct meshes_settings
		{
			// Blended shells (rtx_comp\mesh_tables\shell_meshes.txt): 0 = decals like the other blended map
			// draws (the old behaviour), 1 = plain blended geometry, 2 = alpha-tested cutouts.
			int shell_mode = 2;
			// A shell marked 'gap' (a skirt bridging a trench, New Londo) is cut at this alpha reference (0..255) instead of 64,
			// so the faded part over the trench stays solid. Live: proxy.Meshes.GapSkirtAlphaRef.
			int gap_skirt_alpha_ref = 8;
			// Skirts in rtx_comp\mesh_tables\skirt_backing.txt stay decals and get their hanging faces drawn once more as a solid
			// cutout, at GapSkirtAlphaRef (game/mesh_tables.hpp skirt_backing_indices). 0: bare decals, the sky shows through the
			// seam under them. Live: proxy.Meshes.SkirtBacking.
			bool skirt_backing = true;
			// Meshes in rtx_comp\mesh_tables\veil_meshes.txt are drawn as unlit layers in their vertex colour, out of the look's haze
			// (game/mesh_tables.hpp is_veil_draw). 0: decals, as before. Live: proxy.Meshes.Veils.
			bool veils = true;
			// A veil listed with a tuck (veil_meshes.txt 'tuck') has its solid edge moved this far (m) under the wall it meets, closing
			// the seam rays slipped through (the Kiln doorway's bright outline). 0: where the game has it. Live: proxy.Meshes.VeilTuck.
			float veil_tuck = 0.1f;
			// Window glass (rtx_comp\mesh_tables\glass_meshes.txt): draw the leaded came as an alpha-tested
			// opaque surface and the glass as a separate blended surface with its derived texture
			// (rtx_comp\glass), which the ds1_glass mod makes translucent. Off: as the game draws it.
			bool glass_split = true;
			// Thin walls (rtx_comp\mesh_tables\thin_wall_meshes.txt, the Moonlight Butterfly's wings): tagged
			// so lighting rays pass a face turned back towards the surface they left, within 0.5 m (Surface::isDs1ThinWall). Off:
			// no draw is tagged, every face stops rays (for testing light leaks; walls of two facing faces go dark).
			bool thin_walls = true;
			// No-shadow meshes (rtx_comp\mesh_tables\no_shadow_meshes.txt): tagged so lighting rays pass them (Surface::isDs1NoShadow).
			// Off: they cast shadows like any mesh. Lamp meshes (mesh_lights) keep their tag.
			bool no_shadow_meshes = true;
			// Ground cards (rtx_comp\mesh_tables\ground_cards.txt): flat alpha-tested leaves, lily pads, bones and rubble the artists lifted
			// a few centimetres off the ground are brought down vertex by vertex: each vertex keeps ground_card_height_kept of its
			// height above the ground under it, plus ground_card_clearance metres; shadows stay. Off: drawn where the game has them.
			bool ground_cards = true;
			float ground_card_height_kept = 0.2f;
			float ground_card_clearance = 0.003f;
			// Cut meshes (rtx_comp\mesh_tables\cut_meshes.bin, scripts/gen_cut_meshes.py), a test of drawing the game's cut-out
			// (alpha-tested) map meshes as cards cut along their texture's alpha: 0 = as the game draws them, 1 = cut and solid
			// (no alpha test), 2 = cut, alpha test kept, 3 = the whole card solid (how fast it would be with no cut-outs at all).
			// Live: proxy.Meshes.CutMeshes.
			int cut_meshes = 0;
			// Only the cut meshes whose name ('<map>/<model>/mesh<n>/lod<n> <texture>') holds this text. Live: proxy.Meshes.CutMeshFilter.
			std::string cut_mesh_filter;
			// The game's cut-out (alpha-tested) map meshes are triangle strips stitched with degenerate triangles: two of every three
			// triangles Remix gets are empty. On: such a draw goes to Remix as a triangle list of its real triangles, the same
			// picture with a third of the triangles in the acceleration structure and in the opacity micromaps.
			// Live: proxy.Meshes.CutoutLists.
			bool cutout_lists = false;
		} meshes;

		struct resolution_settings
		{
			// The sizes in use, from follow_game. 0 until the game has asked for a device.
			UINT render_width = 0;
			UINT render_height = 0;
			UINT present_width = 0;
			UINT present_height = 0;
			// [Resolution] as the ini has it.
			UINT ini_render_width = 0;   // 0 = the game's resolution
			UINT ini_render_height = 0;
			UINT ini_present_width = 0;  // 0 = same as the render size
			UINT ini_present_height = 0;
			// What the game last asked for.
			UINT game_width = 0;
			UINT game_height = 0;
			// Sizes handed out before, encoded width << 16 | height.
			std::vector<uint32_t> earlier_render;
			std::vector<uint32_t> earlier_present;

			// From the game's back buffer request at CreateDevice and Reset: works out the sizes in use and
			// replaces the request with the present size. A size of 0 (the window's) leaves it all alone.
			void follow_game(UINT& back_buffer_width, UINT& back_buffer_height);
			// Rendering at another size than presenting ([Resolution] RenderWidth/Height apart from PresentWidth/Height): the game's
			// scene targets are render_width x render_height, the back buffer present_width x present_height. False when they are
			// the same (all 0, or a render size alone): then the scene targets are the back buffer's size.
			bool split() const
			{
				return render_width && render_height && present_width && present_height &&
					(render_width != present_width || render_height != present_height);
			}

			// After a Reset the game remakes its render textures at the size it was given, not the 1024x720 and
			// 1280x720 it first asked for: a request of that size is one of those.
			bool was_render_size(UINT width, UINT height) const;
			bool was_present_size(UINT width, UINT height) const;
		} resolution;

		struct benchmark_settings
		{
			int settle_frames = 90;   // frames to wait after switching options
			int measure_frames = 240; // frames measured per phase
		} benchmark;

		// Lookdev frame capture (modules/lookdev_capture). Only lookdev\lookdev.ps1
		// turns it on, in remix-comp-proxy.lookdev.ini; the normal INI never has it.
		struct lookdev_settings
		{
			bool enabled = false;
			int capture_key = VK_F9;
			// The capture key also works in the normal game, with Remix (lookdev_capture.hpp). Dev mode only: its default is
			// dev mode, and without it the INI cannot turn it on.
			bool main_game_capture = false;
			// ... and first switches to vanilla graphics, for the file's game image.
			bool main_game_comparison = true;
			int freeze_input_key = VK_F8;
			std::string output_dir = "rtx_comp\\lookdev";
			// Live stream pacing: resource contents sent per frame, so a resync
			// (everything in use at once) is spread over frames instead of hitching.
			int stream_budget_kb = 2048; // texel / buffer bytes per frame (one larger object still goes when it is the frame's first)
		} lookdev;

	private:
		std::string ini_path_;
		std::string overlay_path_;
		std::unordered_map<std::string, std::string> area_values_;
		bool loaded_ = false;
		bool dev_forced_ = false;

		bool overlay_value(const char* section, const char* key, std::string& out) const;

		void parse_all();
	};
}
