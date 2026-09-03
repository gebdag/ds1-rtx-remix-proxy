#pragma once

namespace comp::game::render_toggles
{
	// DS1 schedule-level render kill switches.
	//
	// Every switch writes one of the game's own scheduler inputs — the draw
	// GROUP globals, the GameRend draw STAGE booleans, or the DrawEnvMan chain
	// gates — so killed work is never planned, culled, submitted, or dispatched.
	// These are the exact flags DS1's internal "GAME REND" debug menu binds.
	// This replaces the retired [Effects] NOP patches, which suppressed
	// already-scheduled dispatch inside the retained draw plan and made
	// performance worse (findings.md 2026-07-26, "perf paradox").
	enum class toggle : int
	{
		// Draw groups — which entity classes are scheduled at all
		group_map = 0,      // static world geometry
		group_obj,          // dynamic/physics objects
		group_chr,          // characters (player, NPCs, enemies)
		group_sfx,          // particle effects
		group_remo,         // cutscene models
		group_light_shaft,  // light shaft volumes

		// Draw stages — which passes scheduled entities are drawn in
		stage_z_prepass,          // IsEnableZPrePass (main-scene depth prepass)
		stage_depth_normal,       // DepthDraw_DepthNormal (depth+normal buffer)
		stage_depth_tex_edge,     // DepthDraw_DepthTexEdge
		stage_normal,             // NormalDraw_Normal (main opaque geometry)
		stage_bounding_region,    // NormalDraw_BoundingRegion
		stage_tex_edge,           // NormalDraw_TexEdge (outline/edge draws)
		stage_water_mask,         // NormalDraw_WaterMask (offscreen water mask)
		stage_water,              // NormalDraw_Water (water surfaces)
		stage_trans,              // NormalDraw_Trans (translucent geometry)
		stage_shadow_normal,      // ShadowDepth_ShadowDepthNormal
		stage_shadow_tex_edge,    // ShadowDepth_ShadowDepthTexEdge

		// Pass / effect schedulers
		shadow_cascades,     // DrawEnvMan.shadow_cascade_count = 0 (no shadow loops at all)
		water_reflection,    // reflect stage-mask fold (2-byte JE->JMP at 0xEAF61D)
		offscreen_sfx,       // GameRend offsfx_switch_mode = 1 (force half-res SFX off)
		msaa,                // GameRend msaa_switch_mode = 1 (force off)
		env_capture,         // env-map cube capture gate (one-byte JE->JMP at 0xF8F053)

		// Post-processing filters (ImageFilter chain) — killed with the engine's
		// own token 0x15 SetEnable(0) on the filter's retained plan node,
		// re-emitted every frame. Experimental: see findings.md 2026-07-27.
		postfx_dof,          // ImageProcessDof
		postfx_bloom,        // ImageProcessBloom
		postfx_tonemap,      // ImageProcessToneMap (kill LightShaft+Bloom first)
		postfx_light_shaft,  // ImageProcessLightShaft (the radial-blur filter itself)
		postfx_camera_blur,  // ImageProcessCameraBlur
		postfx_downsample,   // ImageProcessDownSample (source for DoF/bloom chain)
		postfx_combine,      // ImageProcessCombine

		count
	};

	struct toggle_desc
	{
		const char* label;      // human-readable
		const char* game_name;  // engine flag name (as in the DS1 debug menu)
	};

	const toggle_desc& describe(toggle t);

	// True once the GameRend/DrawEnvMan pointer chains resolve (in-game).
	bool available();

	bool get_render(toggle t);            // true = rendering (stock behavior)
	void set_render(toggle t, bool render);

	// Extra knobs (live game values; 0 / INT_MIN when unavailable)
	int  shadow_map_size();               // g_ShadowMapSize (atlas edge, 512..2048)
	void set_shadow_map_size(int size);
	int  shadow_cascade_count();          // live DrawEnvMan+0x30
	int  lod_bias();                      // GameRend LOD bias (min -2)
	void set_lod_bias(int bias);

	// Seed desired states from [Perf] INI config (called once, lazily).
	void init_from_config();

	// Resolve pointers and re-assert every active override. Pure in-process
	// byte writes — zero bridge traffic. Call once per frame from Present.
	void on_present();
}
