#include "std_include.hpp"
#include "config.hpp"

namespace shared::common
{
	config& config::get()
	{
		static config instance;
		return instance;
	}

	void config::resolution_settings::follow_game(UINT& back_buffer_width, UINT& back_buffer_height)
	{
		// The game can hand back the parameters this wrote into: that size is not its choice.
		const bool ours = present_width && back_buffer_width == present_width && back_buffer_height == present_height;
		if (!ours)
		{
			game_width = back_buffer_width;
			game_height = back_buffer_height;
		}
		auto remember = [](std::vector<uint32_t>& sizes, UINT width, UINT height)
		{
			const uint32_t size = (width << 16) | height;
			if (width && height && std::find(sizes.begin(), sizes.end(), size) == sizes.end())
				sizes.push_back(size);
		};
		remember(earlier_render, render_width, render_height);
		remember(earlier_present, present_width, present_height);
		const bool manual_render = ini_render_width && ini_render_height;
		const bool manual_present = ini_present_width && ini_present_height;
		render_width = manual_render ? ini_render_width : game_width;
		render_height = manual_render ? ini_render_height : game_height;
		present_width = manual_present ? ini_present_width : render_width;
		present_height = manual_present ? ini_present_height : render_height;
		if (render_width && render_height)
		{
			back_buffer_width = present_width;
			back_buffer_height = present_height;
		}
	}

	bool config::resolution_settings::was_render_size(UINT width, UINT height) const
	{
		return std::find(earlier_render.begin(), earlier_render.end(), (width << 16) | height) != earlier_render.end();
	}

	bool config::resolution_settings::was_present_size(UINT width, UINT height) const
	{
		return std::find(earlier_present.begin(), earlier_present.end(), (width << 16) | height) != earlier_present.end();
	}

	void config::load(const std::string& path)
	{
		ini_path_ = path;

		// Check if the INI file actually exists — GetPrivateProfileInt silently
		// returns defaults for missing files, making it look like settings are ignored.
		if (GetFileAttributesA(ini_path_.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			log("Config", std::format("INI NOT FOUND: {} — using all defaults!", ini_path_),
				LOG_TYPE::LOG_TYPE_ERROR, true);
			loaded_ = false;
			return;
		}

		loaded_ = true;
		parse_all();
	}

	void config::set_overlay(const std::string& path)
	{
		overlay_path_ = GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES ? path : std::string{};
		if (!overlay_path_.empty())
		{
			log("Config", std::format("Overlay: {}", overlay_path_), LOG_TYPE::LOG_TYPE_STATUS, true);
			if (loaded_) parse_all();
		}
	}

	void config::set_area_values(std::unordered_map<std::string, std::string> values)
	{
		area_values_ = std::move(values);
		if (loaded_) parse_all();
	}

	bool config::overlay_value(const char* section, const char* key, std::string& out) const
	{
		if (!area_values_.empty())
			if (const auto it = area_values_.find(std::string(section) + "." + key); it != area_values_.end())
			{
				out = it->second;
				return true;
			}
		if (overlay_path_.empty()) return false;
		char buf[512];
		GetPrivateProfileStringA(section, key, "\x01", buf, sizeof(buf), overlay_path_.c_str());
		if (buf[0] == '\x01' && buf[1] == '\0') return false;
		out = buf;
		return true;
	}

	int config::get_int(const char* section, const char* key, int default_val) const
	{
		if (!loaded_) return default_val;
		if (std::string v; overlay_value(section, key, v))
		{
			try { return std::stoi(v, nullptr, 0); }
			catch (...) { return default_val; }
		}
		return GetPrivateProfileIntA(section, key, default_val, ini_path_.c_str());
	}

	std::string config::get_string(const char* section, const char* key, const char* default_val) const
	{
		if (!loaded_) return default_val;
		if (std::string v; overlay_value(section, key, v))
			return v;
		char buf[512];
		GetPrivateProfileStringA(section, key, default_val, buf, sizeof(buf), ini_path_.c_str());
		return buf;
	}

	float config::get_float(const char* section, const char* key, float default_val) const
	{
		auto str = get_string(section, key, "");
		if (str.empty()) return default_val;
		try { return std::stof(str); }
		catch (...) { return default_val; }
	}

	bool config::get_bool(const char* section, const char* key, bool default_val) const
	{
		return get_int(section, key, default_val ? 1 : 0) != 0;
	}

	bool config::get_float3(const char* section, const char* key, float* out) const
	{
		const auto str = get_string(section, key, "");
		float v[3] = {};
		if (std::sscanf(str.c_str(), "%f , %f , %f", &v[0], &v[1], &v[2]) != 3)
			return false;
		for (int i = 0; i < 3; ++i) out[i] = v[i];
		return true;
	}

	bool config::set_string(const char* section, const char* key, const std::string& value) const
	{
		if (!loaded_ || ini_path_.empty()) return false;
		// An area's value is the area's: the INI keeps what it has.
		if (area_values_.contains(std::string(section) + "." + key)) return true;
		return WritePrivateProfileStringA(section, key, value.c_str(), ini_path_.c_str()) != FALSE;
	}

	void config::parse_all()
	{
		// [Remix]
		remix.enabled = get_bool("Remix", "Enabled", true);
		remix.dll_name = get_string("Remix", "DLLName", "d3d9_remix.dll");

		// [Chain]
		chain.preload = get_string("Chain", "PreLoad", "");
		chain.postload = get_string("Chain", "PostLoad", "");

		// [FFP]
		ffp.enabled = get_bool("FFP", "Enabled", true);
		ffp.auto_enable = get_bool("FFP", "AutoEnable", false);
		ffp.auto_enable_threshold = get_int("FFP", "AutoEnableThreshold", 50);
		ffp.auto_enable_frames = get_int("FFP", "AutoEnableFrames", 5);
		ffp.albedo_stage = get_int("FFP", "AlbedoStage", 0);
		if (ffp.albedo_stage < 0 || ffp.albedo_stage > 7)
			ffp.albedo_stage = 0;
		ffp.dedup_pass_mode = get_int("FFP", "DedupPassMode", 0);
		ffp.show_dedup_ui = get_bool("FFP", "ShowDedupUI", false);
		ffp.tex_filter = get_bool("FFP", "TexFilter", false);
		ffp.tex_filter_threshold = get_int("FFP", "TexFilterThreshold", 80);
		ffp.expand_skinned_uv = get_bool("FFP", "ExpandSkinnedUV", true);
		ffp.skip_postfx = get_bool("FFP", "SkipPostFX", true);
		ffp.suppress_aux_passes = get_bool("FFP", "SuppressAuxPasses", true);
		ffp.menu_scene_path_trace = get_bool("FFP", "MenuScenePathTrace", false);
		ffp.shader_tex_fix = get_bool("FFP", "ShaderTexFix", false);
		ffp.texture_layer_blend = get_bool("FFP", "TextureLayerBlend", true);
		ffp.water_surface = get_bool("FFP", "WaterSurface", true);
		ffp.draw_tint = get_bool("FFP", "DrawTint", true);
		ffp.water_fallback_albedo = get_bool("FFP", "WaterFallbackAlbedo", true);
		ffp.zwrite_query_per_draw = get_bool("FFP", "ZWriteQueryPerDraw", true);

		// [FFPSteps] — per-step switches for the conversion pipeline. Every
		// default reproduces stock behaviour, so the section is inert until
		// something is changed from the Perf tab.
		auto level = [this](const char* key, int def)
		{
			int v = get_int("FFPSteps", key, def);
			return (v < 0) ? 0 : (v > 2 ? 2 : v);
		};
		ffp_steps.profile           = get_bool("FFPSteps", "Profile", false);
		ffp_steps.tex_stages        = level("TextureStages", 2);
		ffp_steps.transforms        = level("Transforms", 2);
		ffp_steps.restore_textures  = level("RestoreTextures", 2);
		ffp_steps.albedo_null_stages = get_bool("FFPSteps", "AlbedoNullStages", true);
		ffp_steps.expand_reuse      = get_bool("FFPSteps", "ExpandReuse", true);
		ffp_steps.decal_query       = get_bool("FFPSteps", "DecalQuery", true);
		ffp_steps.overlay_save_lazy = get_bool("FFPSteps", "OverlaySaveLazy", false);
		ffp_steps.skin_upload_used_bones = get_bool("FFPSteps", "SkinUploadUsedBones", true);
		ffp_steps.pool_depth_surfaces = get_bool("FFPSteps", "PoolDepthSurfaces", true);

		// [PostFX] Comma-separated list of pass indices to skip (empty = skip all)
		postfx.skip_passes_str = get_string("PostFX", "SkipPasses", "");
		postfx.parse_skip_string();

		// [Skinning]
		skinning.enabled = get_bool("Skinning", "Enabled", false);

		// [Cheats]
		cheats.god_mode = get_bool("Cheats", "GodMode", false);
		cheats.enemies_ignore_player = get_bool("Cheats", "EnemiesIgnorePlayer", false);
		cheats.no_fall_damage = get_bool("Cheats", "NoFallDamage", false);

		// [Diagnostics]
		diagnostics.enabled = get_bool("Diagnostics", "Enabled", true);
		diagnostics.auto_capture = get_bool("Diagnostics", "AutoCapture", true);
		diagnostics.delay_ms = get_int("Diagnostics", "DelayMs", 50000);
		diagnostics.log_frames = get_int("Diagnostics", "LogFrames", 3);
		diagnostics.log_draw_calls = get_bool("Diagnostics", "LogDrawCalls", true);
		diagnostics.log_vs_constants = get_bool("Diagnostics", "LogVSConstants", true);
		diagnostics.log_vertex_data = get_bool("Diagnostics", "LogVertexData", true);
		diagnostics.log_declarations = get_bool("Diagnostics", "LogDeclarations", true);
		diagnostics.log_textures = get_bool("Diagnostics", "LogTextures", true);
		diagnostics.log_present_info = get_bool("Diagnostics", "LogPresentInfo", true);

		// [IntroSkip]
		intro_skip.enabled = get_bool("IntroSkip", "Enabled", true);

		// [FPSUnlock]
		fps_unlock.enabled = get_bool("FPSUnlock", "Enabled", false);
		fps_unlock.target_fps = get_int("FPSUnlock", "TargetFPS", 60);

		// [Culling]
		culling.enable_frustum_culling = get_bool("Culling", "EnableFrustumCulling", true);
		culling.frustum_relaxation_radius = std::max(0.0f, get_float("Culling", "FrustumRelaxationRadius", 0.0f));
		culling.keep_light_blockers = get_bool("Culling", "KeepLightBlockers", true);
		culling.light_blocker_min_size = std::max(0.0f, get_float("Culling", "LightBlockerMinSize", 8.0f));
		culling.light_blocker_near_range = std::max(0.0f, get_float("Culling", "LightBlockerNearRange", 50.0f));
		culling.light_blocker_sun_reach = std::max(0.0f, get_float("Culling", "LightBlockerSunReach", 200.0f));

		// [Tracer]
		tracer.backtrace_depth = get_int("Tracer", "BacktraceDepth", 8);
		tracer.output_dir = get_string("Tracer", "OutputDir", "captures");

		// [Lights]
		lights.enabled = get_bool("Lights", "Enabled", true);
		lights.intensity_scale = get_float("Lights", "IntensityScale", 1.0f);
		lights.radius_scale = get_float("Lights", "RadiusScale", 1.0f);
		lights.resolve_entity_params = get_bool("Lights", "ResolveEntityParams", true);
		lights.synthesized_lights = get_bool("Lights", "SynthesizedLights", true);
		lights.fire_light_scale = std::max(0.0f, get_float("Lights", "FireLightScale", 1.0f));
		lights.effect_table_scale = std::max(0.0f, get_float("Lights", "EffectTableScale", 1.0f));
		fog_gate_enable = get_bool("FogGate", "Enable", false);

		// [Hud]
		hud.mark_ui_draws = get_bool("Hud", "MarkUiDraws", true);

		// [PlayerLight]
		player_light.enabled = get_bool("PlayerLight", "Enabled", true);
		player_light.use_game_colour = get_bool("PlayerLight", "UseGameColour", true);
		get_float3("PlayerLight", "Colour", player_light.colour);
		player_light.use_game_range = get_bool("PlayerLight", "UseGameRange", true);
		player_light.range = std::max(0.01f, get_float("PlayerLight", "Range", 7.0f));
		player_light.use_game_radius = get_bool("PlayerLight", "UseGameRadius", true);
		player_light.radius = std::max(0.01f, get_float("PlayerLight", "Radius", 0.5f));
		player_light.intensity = std::max(0.0f, get_float("PlayerLight", "Intensity", 1.0f));
		player_light.falloff_intensity = std::max(0.0f, get_float("PlayerLight", "FalloffIntensity", 1.0f));
		player_light.falloff_gamma_add = get_bool("PlayerLight", "FalloffGammaAdd", true);
		player_light.falloff_range_scale = std::max(0.01f, get_float("PlayerLight", "FalloffRangeScale", 1.0f));
		get_float3("PlayerLight", "Offset", player_light.offset);
		{
			const auto frame = get_string("PlayerLight", "OffsetFrame", "View");
			player_light.offset_frame = _stricmp(frame.c_str(), "World") == 0 ? 0
				: _stricmp(frame.c_str(), "Character") == 0 ? 2 : 1;
		}

		// [Sun]
		sun.enabled = get_bool("Sun", "Enabled", true);
		sun.intensity_scale = std::max(0.0f, get_float("Sun", "IntensityScale", 1.0f));
		sun.angular_diameter = std::clamp(get_float("Sun", "AngularDiameter", 0.53f), 0.0f, 45.0f);
		{
			const auto colour = get_string("Sun", "FallbackColour", "");
			float c[3] = {};
			if (std::sscanf(colour.c_str(), "%f , %f , %f", &c[0], &c[1], &c[2]) == 3)
				for (int i = 0; i < 3; ++i) sun.fallback_colour[i] = std::max(0.0f, c[i]);
		}

		// [Sky]
		sky.mark_sky_draws = get_bool("Sky", "MarkSkyDraws", true);
		sky.shell_mode = std::clamp(get_int("Sky", "ShellMode", 2), 0, 2);
		sky.glass_split = get_bool("Sky", "GlassSplit", true);
		sky.thin_walls = get_bool("Sky", "ThinWalls", true);
		sky.no_shadow_meshes = get_bool("Sky", "NoShadowMeshes", true);

		// [Resolution]
		resolution.ini_render_width = static_cast<UINT>(get_int("Resolution", "RenderWidth", 0));
		resolution.ini_render_height = static_cast<UINT>(get_int("Resolution", "RenderHeight", 0));
		resolution.ini_present_width = static_cast<UINT>(get_int("Resolution", "PresentWidth", 0));
		resolution.ini_present_height = static_cast<UINT>(get_int("Resolution", "PresentHeight", 0));

		// [Benchmark]
		benchmark.settle_frames = get_int("Benchmark", "SettleFrames", 90);
		benchmark.measure_frames = get_int("Benchmark", "MeasureFrames", 240);

		// [Sfx]
		sfx.ffp_sprites = get_bool("Sfx", "FFPSprites", true);
		sfx.debug_opaque = get_bool("Sfx", "DebugOpaque", false);
		sfx.translate = get_bool("Sfx", "Translate", true);
		sfx.drop_screen_effects = get_bool("Sfx", "DropScreenEffects", true);
		sfx.hide = get_bool("Sfx", "Hide", false);

		// [Lookdev]
		lookdev.enabled = get_bool("Lookdev", "Enabled", false);
		lookdev.capture_key = get_int("Lookdev", "CaptureKey", VK_F9);
		lookdev.main_game_capture = get_bool("Lookdev", "MainGameCapture", true);
		lookdev.main_game_comparison = get_bool("Lookdev", "MainGameComparison", true);
		lookdev.freeze_input_key = get_int("Lookdev", "FreezeInputKey", VK_F8);
		lookdev.output_dir = get_string("Lookdev", "OutputDir", "rtx_comp\\lookdev");
		lookdev.stream_budget_kb = std::max(256, get_int("Lookdev", "StreamBudgetKB", 2048));

		// [Perf] is read lazily by comp::game::render_toggles (the keys map 1:1
		// to its toggle table) — nothing to parse here.

		log("Config", std::format("Loaded from: {}", ini_path_));
		log("Config", std::format("FFP={} AlbedoStage={} DedupPassMode={} TextureLayerBlend={}",
			ffp.enabled ? 1 : 0, ffp.albedo_stage, ffp.dedup_pass_mode,
			ffp.texture_layer_blend ? 1 : 0));
		if (skinning.enabled)
			log("Config", "Skinning ENABLED", LOG_TYPE::LOG_TYPE_WARN);
	}

	void config::postfx_settings::parse_skip_string()
	{
		skip_passes.clear();
		std::istringstream ss(skip_passes_str);
		std::string token;
		while (std::getline(ss, token, ','))
		{
			// trim whitespace
			auto start = token.find_first_not_of(" \t");
			if (start == std::string::npos) continue;
			auto end = token.find_last_not_of(" \t");
			token = token.substr(start, end - start + 1);
			if (token.empty()) continue;

			char* endptr = nullptr;
			long val = strtol(token.c_str(), &endptr, 10);
			if (endptr != token.c_str() && *endptr == '\0' && val >= 0)
				skip_passes.insert(static_cast<int>(val));
		}
	}

	void config::postfx_settings::rebuild_skip_string()
	{
		std::vector<int> sorted(skip_passes.begin(), skip_passes.end());
		std::sort(sorted.begin(), sorted.end());
		skip_passes_str.clear();
		for (size_t i = 0; i < sorted.size(); i++)
		{
			if (i > 0) skip_passes_str += ',';
			skip_passes_str += std::to_string(sorted[i]);
		}
	}
}
