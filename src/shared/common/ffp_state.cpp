#include "std_include.hpp"
#include "ffp_state.hpp"
#include "ffp_prof.hpp"
#include <cstdio>

namespace shared::common
{
	// Per-frame camera matrix logger — writes one line per frame to rtx_comp/camera.log
	static FILE* s_cam_log = nullptr;
	static UINT  s_cam_log_frame = UINT(-1);

	static void cam_log_open()
	{
		if (s_cam_log) return;
		std::string path = shared::globals::root_path + "\\rtx_comp\\camera.log";
		s_cam_log = fopen(path.c_str(), "w");
		if (s_cam_log)
		{
			fprintf(s_cam_log, "frame,"
				"c4x,c4y,c4z,c4w,"
				"c5x,c5y,c5z,c5w,"
				"c6x,c6y,c6z,c6w,"
				"det3,"
				"v00,v01,v02,v03,"
				"v10,v11,v12,v13,"
				"v20,v21,v22,v23,"
				"v30,v31,v32,v33,"
				"p00,p05,p10,p11,p14\n");
			fflush(s_cam_log);
		}
	}

	static void cam_log_write(UINT frame,
		const float* c4, const float* c5, const float* c6,
		float det3, const float* view, const float* proj)
	{
		cam_log_open();
		if (!s_cam_log || frame == s_cam_log_frame) return;
		s_cam_log_frame = frame;
		fprintf(s_cam_log,
			"%u,"
			"%.6f,%.6f,%.6f,%.6f,"
			"%.6f,%.6f,%.6f,%.6f,"
			"%.6f,%.6f,%.6f,%.6f,"
			"%.6f,"
			"%.6f,%.6f,%.6f,%.6f,"
			"%.6f,%.6f,%.6f,%.6f,"
			"%.6f,%.6f,%.6f,%.6f,"
			"%.6f,%.6f,%.6f,%.6f,"
			"%.6f,%.6f,%.6f,%.6f,%.6f\n",
			frame,
			c4[0], c4[1], c4[2], c4[3],
			c5[0], c5[1], c5[2], c5[3],
			c6[0], c6[1], c6[2], c6[3],
			det3,
			view[0], view[1], view[2], view[3],
			view[4], view[5], view[6], view[7],
			view[8], view[9], view[10], view[11],
			view[12], view[13], view[14], view[15],
			proj[0], proj[5], proj[10], proj[11], proj[14]);
		fflush(s_cam_log);
	}

	ffp_state& ffp_state::get()
	{
		static ffp_state instance;
		return instance;
	}

	void ffp_state::init(IDirect3DDevice9* /*real_device*/)
	{
		cfg_ = &config::get().ffp;
		steps_ = &config::get().ffp_steps;
		ffp_prof::set_profiling(steps_->profile);
		enabled_ = cfg_->enabled && !cfg_->auto_enable;
		create_tick_ = GetTickCount();

		log("FFP", std::format("State tracker initialized, FFP={}", enabled_ ? "ON" : "OFF"));
		log("FFP", std::format("Registers: View=c{}-c{} Proj=c{}-c{} World=c{}-c{}",
			vs_reg_view_start_, vs_reg_view_end_ - 1,
			vs_reg_proj_start_, vs_reg_proj_end_ - 1,
			vs_reg_world_start_, vs_reg_world_end_ - 1));
	}

	// ---- State mutators ----

	void ffp_state::on_set_vs_const_f(UINT start_reg, const float* data, UINT count)
	{
		if (!data || start_reg + count > 256) return;

		std::memcpy(&vs_const_[start_reg * 4], data, count * 4 * sizeof(float));

		// Dirty tracking keyed to game-specific register layout
		UINT end_reg = start_reg + count;
		if (start_reg < static_cast<UINT>(vs_reg_proj_end_) &&
			end_reg > static_cast<UINT>(vs_reg_view_start_))
		{
			view_proj_dirty_ = true;
		}
		if (start_reg < static_cast<UINT>(vs_reg_world_end_) &&
			end_reg > static_cast<UINT>(vs_reg_world_start_))
		{
			world_dirty_ = true;
		}

		// Mark View+Proj valid once both ranges have been written
		auto view_start = static_cast<UINT>(vs_reg_view_start_);
		auto proj_start = static_cast<UINT>(vs_reg_proj_start_);
		auto proj_end = static_cast<UINT>(vs_reg_proj_end_);

		if (start_reg <= view_start && end_reg >= proj_end)
		{
			// Single write spans both View and Proj ranges
			view_proj_valid_ = true;
		}
		else if (start_reg == view_start && count >= 4 && vs_const_write_log_[proj_start])
		{
			view_proj_valid_ = true;
		}
		else if (start_reg == proj_start && count >= 4 && vs_const_write_log_[view_start])
		{
			view_proj_valid_ = true;
		}

		for (UINT i = 0; i < count; i++)
			vs_const_write_log_[start_reg + i] = 1;

		// Bone palette detection (for skinning module)
		// For skinned meshes the world matrix (c8-c10) IS bone 0, and the skinned
		// vertex shaders always read bone i at c8 + 3i (c8[a0.x], a0 = 3 * index).
		// A single SetVSConstantF(8, data, 114) covers the full palette: bone0 at c8,
		// bone1 at c11, ... bone37 at c119. The game also writes only the bones
		// that changed since the previous draw (e.g. start=11 count=21 refreshes
		// bones 1-7); the rest keep their earlier values in the constant shadow,
		// which the shader reads as well. So the palette never moves: it starts at
		// world_start, and a write only tells how far it reaches.
		if (config::get().skinning.enabled)
		{
			UINT bone_thresh = static_cast<UINT>(vs_reg_bone_threshold_);
			UINT rpb = static_cast<UINT>(vs_regs_per_bone_);
			UINT world_start = static_cast<UINT>(vs_reg_world_start_);

			if (end_reg > bone_thresh && end_reg > world_start && rpb > 0)
			{
				const int reach = static_cast<int>((end_reg - world_start + rpb - 1) / rpb);
				bone_start_reg_ = static_cast<int>(world_start);
				num_bones_ = std::max(num_bones_, reach);
			}
		}
	}

	void ffp_state::on_set_ps_const_f(UINT start_reg, const float* data, UINT count)
	{
		if (!data || start_reg + count > 256) return;

		std::memcpy(&ps_const_[start_reg * 4], data, count * 4 * sizeof(float));
	}

	void ffp_state::on_set_vertex_shader(IDirect3DVertexShader9* shader)
	{
		if (shader) shader->AddRef();
		if (last_vs_) last_vs_->Release();
		last_vs_ = shader;
		ffp_active_ = false;

		cur_vs_viewproj_base_ = -1;
		cur_vs_exposure_ = false;
		cur_vs_tex_scroll_ = 0;
		if (shader)
		{
			if (auto it = vs_tex_scroll_.find(shader); it != vs_tex_scroll_.end())
				cur_vs_tex_scroll_ = it->second;
			if (auto it = vs_viewproj_base_.find(shader); it != vs_viewproj_base_.end())
				cur_vs_viewproj_base_ = it->second;
			if (auto it = vs_exposure_.find(shader); it != vs_exposure_.end())
				cur_vs_exposure_ = it->second;
		}
	}

	namespace
	{
		// First constant register of the view-projection a vertex shader applies to its
		// position: the lowest float constant read by its first dp4 instructions (DS1's
		// sprite shaders use c8-c11; some leave the z row out of the dp4s; the point sprite
		// shader's first dp4s are its view, c12-c15, see draw_sprite_up). -1 when the shader
		// has none (relative addressing is ignored).
		int parse_vs_viewproj_base(const DWORD* code)
		{
			if (!code || (code[0] & 0xFFFF0000) != 0xFFFE0000)
				return -1;

			int base = -1;
			int seen = 0;
			constexpr size_t max_tokens = 65536;
			size_t i = 1;
			while (i < max_tokens && seen < 3)
			{
				const DWORD tok = code[i];
				if (tok == 0x0000FFFF)
					break;
				const DWORD opcode = tok & 0xFFFF;
				if (opcode == 0xFFFE) // D3DSIO_COMMENT
				{
					i += 1 + ((tok >> 16) & 0x7FFF);
					continue;
				}
				const DWORD param_count = (tok >> 24) & 0x0F;
				if (opcode == 9 /* D3DSIO_DP4 */)
				{
					for (DWORD p = 2; p <= param_count; ++p)
					{
						const DWORD param = code[i + p];
						const DWORD reg_type = ((param >> 28) & 0x7) | ((param >> 8) & 0x18);
						if (reg_type == 2 /* D3DSPR_CONST */ && !(param & 0x2000) /* relative */)
						{
							const int reg = static_cast<int>(param & 0x7FF);
							base = base < 0 ? reg : std::min(base, reg);
							++seen;
						}
					}
				}
				i += 1 + param_count;
			}
			return base;
		}
	}

	namespace
	{
		// True when the vertex shader divides by c159.w (rcp of c159 with the w component). Half of
		// DS1's sprite vertex shaders scale the sprite colour by c159.z / c159.w, the game's exposure
		// c135.x / c135.y once more, on top of the pixel shader's (FRPG_Sfx_SimpleSprite_Base.fxh).
		bool parse_vs_divides_c159w(const DWORD* code)
		{
			if (!code || (code[0] & 0xFFFF0000) != 0xFFFE0000)
				return false;
			constexpr size_t max_tokens = 65536;
			size_t i = 1;
			while (i < max_tokens)
			{
				const DWORD tok = code[i];
				if (tok == 0x0000FFFF)
					break;
				const DWORD opcode = tok & 0xFFFF;
				if (opcode == 0xFFFE) // D3DSIO_COMMENT
				{
					i += 1 + ((tok >> 16) & 0x7FFF);
					continue;
				}
				const DWORD param_count = (tok >> 24) & 0x0F;
				if (opcode == 6 /* D3DSIO_RCP */ && param_count >= 2)
				{
					const DWORD src = code[i + 2];
					const DWORD reg_type = ((src >> 28) & 0x7) | ((src >> 8) & 0x18);
					if (reg_type == 2 /* D3DSPR_CONST */ && (src & 0x7FF) == 159 && ((src >> 16) & 0x3) == 3)
						return true;
				}
				i += 1 + param_count;
			}
			return false;
		}
	}

	void ffp_state::on_create_vertex_shader(IDirect3DVertexShader9* shader, const DWORD* bytecode)
	{
		if (shader)
		{
			vs_viewproj_base_[shader] = parse_vs_viewproj_base(bytecode);
			vs_exposure_[shader] = parse_vs_divides_c159w(bytecode);
			vs_tex_scroll_[shader] = (reads_float_const(bytecode, 246) ? VS_TEX_SCROLL_0 : 0u) |
				(reads_float_const(bytecode, 247) ? VS_TEX_SCROLL_1 : 0u);
		}
	}

	void ffp_state::on_set_pixel_shader(IDirect3DPixelShader9* shader)
	{
		if (shader) shader->AddRef();
		if (last_ps_) last_ps_->Release();
		last_ps_ = shader;

		// Unknown shaders (not seen at creation) get an empty mask — the overlay
		// decal pass stays off rather than risking a stale-texture overlay.
		cur_ps_sampler_mask_ = 0;
		cur_ps_is_water_surface_ = false;
		cur_ps_is_water_heightmap_ = false;
		cur_ps_light_flags_ = 0;
		cur_ps_sfx_class_ = sfx_class::None;
		cur_ps_sfx_colour_scale_ = 1.0f;
		cur_ps_sfx_reads_depth_ = false;
		cur_ps_reads_exposure_ = false;
		cur_ps_draw_tint_ = false;
		cur_ps_point_lights_ = 0;
		cur_ps_skin_tone_ = false;
		cur_ps_is_eye_ = false;
		cur_ps_swamp_pass_ = swamp_pass::None;
		if (shader)
		{
			if (auto it = ps_point_lights_.find(shader); it != ps_point_lights_.end())
				cur_ps_point_lights_ = it->second;
			if (auto it = ps_swamp_pass_.find(shader); it != ps_swamp_pass_.end())
				cur_ps_swamp_pass_ = it->second;
			if (auto it = ps_draw_tint_.find(shader); it != ps_draw_tint_.end())
				cur_ps_draw_tint_ = it->second;
			if (auto it = ps_skin_tone_.find(shader); it != ps_skin_tone_.end())
				cur_ps_skin_tone_ = it->second;
			if (auto it = ps_is_eye_.find(shader); it != ps_is_eye_.end())
				cur_ps_is_eye_ = it->second;
			if (auto it = ps_sampler_masks_.find(shader); it != ps_sampler_masks_.end())
				cur_ps_sampler_mask_ = it->second;
			if (auto it = ps_sfx_class_.find(shader); it != ps_sfx_class_.end())
				cur_ps_sfx_class_ = it->second;
			if (auto it = ps_sfx_colour_scale_.find(shader); it != ps_sfx_colour_scale_.end())
				cur_ps_sfx_colour_scale_ = it->second;
			if (auto it = ps_sfx_reads_depth_.find(shader); it != ps_sfx_reads_depth_.end())
				cur_ps_sfx_reads_depth_ = it->second;
			if (auto it = ps_reads_exposure_.find(shader); it != ps_reads_exposure_.end())
				cur_ps_reads_exposure_ = it->second;
			if (auto it = ps_water_surface_.find(shader); it != ps_water_surface_.end())
				cur_ps_is_water_surface_ = it->second;
			if (auto it = ps_water_heightmap_.find(shader); it != ps_water_heightmap_.end())
				cur_ps_is_water_heightmap_ = it->second;
			if (auto it = ps_light_flags_.find(shader); it != ps_light_flags_.end())
				cur_ps_light_flags_ = it->second;
		}
	}

	void ffp_state::on_create_pixel_shader(IDirect3DPixelShader9* shader, const DWORD* bytecode)
	{
		if (!shader) return;
		ps_sampler_masks_[shader] = parse_ps_sampler_mask(bytecode);
		ps_light_flags_[shader] = parse_ps_light_flags(bytecode);
		// The *Lit*_Non shaders multiply by their lightmap (s6): lit, by baked light.
		if (ps_sampler_masks_[shader] & (1u << 6))
			ps_light_flags_[shader] &= ~PS_UNLIT;
		ps_draw_tint_[shader] = reads_float_const(bytecode, 139);
		int point_lights = 0;
		while (point_lights < 4 && reads_float_const(bytecode, 116 + point_lights))
			++point_lights;
		ps_point_lights_[shader] = point_lights;
		ps_skin_tone_[shader] = reads_float_const(bytecode, 156);
		ps_is_eye_[shader] = reads_float_const(bytecode, 174);

		// FNV-1a hashes of DS1's water-surface pixel shaders, computed over the
		// token stream from the version token through the END token inclusive:
		// FRPG_Water_Env_* (12), FRPG_Water_Reflect_* (12), FRPG_WWS_Dif* (2).
		// The mask/heightmap water shaders are deliberately absent — their decls
		// lack NORMAL, so they stay on the shader passthrough path untouched.
		static const std::unordered_set<uint32_t> water_ps_hashes = {
			0x768083F5, // FRPG_WWS_Dif______Mul_______WaterWave
			0x8CDD4D9A, // FRPG_WWS_Dif________________WaterWave
			0x9EDFB16C, // FRPG_Water_Env_Csd
			0xBE2715A5, // FRPG_Water_Env_CsdPntS
			0x9CBDF676, // FRPG_Water_Env_CsdPntSS
			0x39623788, // FRPG_Water_Env_CsdPntSSSS
			0x28B00899, // FRPG_Water_Env_Ncs
			0x1D4D196F, // FRPG_Water_Env_NcsPntS
			0xC2758B00, // FRPG_Water_Env_NcsPntSS
			0x23611BEF, // FRPG_Water_Env_NcsPntSSSS
			0xC4DF984A, // FRPG_Water_Env____
			0x9B6319F8, // FRPG_Water_Env____PntS
			0x3A2D4580, // FRPG_Water_Env____PntSS
			0xC2EB253D, // FRPG_Water_Env____PntSSSS
			0xA3719AE6, // FRPG_Water_Reflect_Csd
			0x2DA7B212, // FRPG_Water_Reflect_CsdPntS
			0xD1ACA65F, // FRPG_Water_Reflect_CsdPntSS
			0x6E0A093E, // FRPG_Water_Reflect_CsdPntSSSS
			0x56F5791C, // FRPG_Water_Reflect_Ncs
			0x7DB75E55, // FRPG_Water_Reflect_NcsPntS
			0x3CC833FA, // FRPG_Water_Reflect_NcsPntSS
			0xF338736E, // FRPG_Water_Reflect_NcsPntSSSS
			0xD5F19B0B, // FRPG_Water_Reflect____
			0x96D0393E, // FRPG_Water_Reflect____PntS
			0xBAB132B0, // FRPG_Water_Reflect____PntSS
			0x677D21E3, // FRPG_Water_Reflect____PntSSSS
		};

		const uint32_t hash = hash_shader_bytecode(bytecode);
		const bool is_water = water_ps_hashes.contains(hash);
		ps_water_surface_[shader] = is_water;
		constexpr uint32_t water_height_map_hash = 0x90706CDC; // FRPG_Water_HeightMap
		ps_water_heightmap_[shader] = hash == water_height_map_hash;

		// FRPG_Snow surface shaders (the Lit variants are lightmapped; some share bytecode with
		// the plain ones), its height pass, and the WaterWave ring that stamps into heights.
		static const std::unordered_set<uint32_t> snow_surface_hashes = {
			0xC8F2D2F4, // FRPG_Snow_______
			0x539F3A6A, // FRPG_Snow_______PntS, FRPG_Snow_Lit___PntS
			0xB92DCD52, // FRPG_Snow_______PntSS, FRPG_Snow_Lit___PntSS
			0xD205FD68, // FRPG_Snow_______PntSSSS, FRPG_Snow_Lit___PntSSSS
			0x7B23FEC0, // FRPG_Snow____Csd
			0x03494144, // FRPG_Snow____CsdPntS
			0x4482B2FC, // FRPG_Snow____CsdPntSS
			0x9144323F, // FRPG_Snow____CsdPntSSSS
			0x87FA77D3, // FRPG_Snow____Ncs
			0xE94A7522, // FRPG_Snow____NcsPntS
			0x58D7A349, // FRPG_Snow____NcsPntSS
			0x4725F19C, // FRPG_Snow____NcsPntSSSS
			0xC4B3B54B, // FRPG_Snow_Lit___
			0xB879277B, // FRPG_Snow_LitCsd
			0xD1C7F921, // FRPG_Snow_LitCsdPntS
			0x9F2E5CE6, // FRPG_Snow_LitCsdPntSS
			0xAA43DA75, // FRPG_Snow_LitCsdPntSSSS
			0xABD87677, // FRPG_Snow_LitNcs
			0x27808B3F, // FRPG_Snow_LitNcsPntS
			0x9906D388, // FRPG_Snow_LitNcsPntSS
			0x4F298CB6, // FRPG_Snow_LitNcsPntSSSS
		};
		constexpr uint32_t snow_height_map_hash = 0x22D2F93A; // FRPG_Snow_HeightMap
		constexpr uint32_t water_wave_hash = 0x8CDD4D9A;      // FRPG_WWS_Dif________________WaterWave
		ps_swamp_pass_[shader] = snow_surface_hashes.contains(hash) ? swamp_pass::Surface
			: hash == snow_height_map_hash ? swamp_pass::HeightMap
			: hash == water_wave_hash ? swamp_pass::WaveRing
			: swamp_pass::None;

		struct sfx_entry { uint32_t hash; sfx_class cls; float colour_scale; bool reads_depth; const char* name; };
		static constexpr sfx_entry sfx_shaders[] = {
#include "sfx_shaders.inc"
		};
		sfx_class sfx = sfx_class::None;
		float sfx_scale = 1.0f;
		bool sfx_depth = false;
		for (const auto& e : sfx_shaders)
			if (e.hash == hash) { sfx = e.cls; sfx_scale = e.colour_scale; sfx_depth = e.reads_depth; break; }
		ps_sfx_class_[shader] = sfx;
		ps_sfx_reads_depth_[shader] = sfx_depth;
		ps_sfx_colour_scale_[shader] = sfx_scale;
		ps_reads_exposure_[shader] = reads_float_const(bytecode, 135);
		if (is_water)
			log("WATER", std::format("water surface PS registered ptr={:#x} hash={:#010x}",
				reinterpret_cast<uintptr_t>(shader), hash));
	}

	uint32_t ffp_state::hash_shader_bytecode(const DWORD* code)
	{
		if (!code || (code[0] & 0xFFFE0000) != 0xFFFE0000)
			return 0;

		// Measure the token stream the same way parse_ps_sampler_mask walks it,
		// then hash [version token .. END token] so the result matches hashes
		// precomputed from the shipped .fpo/.vpo files.
		constexpr size_t max_tokens = 65536;
		size_t i = 1;
		while (i < max_tokens)
		{
			const DWORD tok = code[i];
			if (tok == 0x0000FFFF)
			{
				i++;
				break;
			}
			const DWORD opcode = tok & 0xFFFF;
			if (opcode == 0xFFFE) // D3DSIO_COMMENT
			{
				i += 1 + ((tok >> 16) & 0x7FFF);
				continue;
			}
			i += 1 + ((tok >> 24) & 0x0F);
		}
		if (i >= max_tokens) return 0;

		return utils::data_hash32(code, i * sizeof(DWORD));
	}

	uint32_t ffp_state::parse_ps_sampler_mask(const DWORD* code)
	{
		// Pixel shader version token is 0xFFFF**** (vertex shaders are 0xFFFE****).
		if (!code || (code[0] & 0xFFFF0000) != 0xFFFF0000)
			return 0;

		// Walk the SM2/SM3 token stream collecting sampler DCLs. Each non-comment
		// instruction token carries its parameter count in bits 24-27; comment
		// blocks carry their length in bits 16-30; 0x0000FFFF terminates.
		uint32_t mask = 0;
		constexpr size_t max_tokens = 65536;
		size_t i = 1;
		while (i < max_tokens)
		{
			const DWORD tok = code[i];
			if (tok == 0x0000FFFF)
				break;

			const DWORD opcode = tok & 0xFFFF;
			if (opcode == 0xFFFE) // D3DSIO_COMMENT
			{
				i += 1 + ((tok >> 16) & 0x7FFF);
				continue;
			}

			const DWORD param_count = (tok >> 24) & 0x0F;
			if (opcode == 31 /* D3DSIO_DCL */ && param_count >= 2)
			{
				// params: [usage token][destination register token]
				const DWORD dst = code[i + 2];
				const DWORD reg_type = ((dst >> 28) & 0x7) | ((dst >> 8) & 0x18);
				if (reg_type == 10 /* D3DSPR_SAMPLER */)
				{
					const DWORD reg = dst & 0x7FF;
					if (reg < 16)
						mask |= 1u << reg;
				}
			}
			i += 1 + param_count;
		}
		return mask;
	}

	namespace
	{
		// Calls fn(reg) for every float constant an instruction of the shader (pixel or
		// vertex) reads. Same token walk as parse_ps_sampler_mask; DEF/DEFI/DEFB are
		// skipped, their trailing tokens are literals, not register operands.
		template <class Fn>
		void for_each_const_read(const DWORD* code, Fn&& fn)
		{
			if (!code || (code[0] & 0xFFFE0000) != 0xFFFE0000)
				return;

			constexpr size_t max_tokens = 65536;
			size_t i = 1;
			while (i < max_tokens)
			{
				const DWORD tok = code[i];
				if (tok == 0x0000FFFF)
					break;

				const DWORD opcode = tok & 0xFFFF;
				if (opcode == 0xFFFE) // D3DSIO_COMMENT
				{
					i += 1 + ((tok >> 16) & 0x7FFF);
					continue;
				}

				const DWORD param_count = (tok >> 24) & 0x0F;
				const bool is_def = opcode == 81 || opcode == 82 || opcode == 83; // DEF, DEFI, DEFB
				if (!is_def && opcode != 31 /* DCL */)
				{
					for (DWORD p = 1; p <= param_count; ++p)
					{
						const DWORD param = code[i + p];
						const DWORD reg_type = ((param >> 28) & 0x7) | ((param >> 8) & 0x18);
						if (reg_type == 2 /* D3DSPR_CONST */)
							fn(param & 0x7FF);
					}
				}
				i += 1 + param_count;
			}
		}
	}

	bool ffp_state::reads_float_const(const DWORD* code, DWORD reg)
	{
		bool reads = false;
		for_each_const_read(code, [&](DWORD r) { if (r == reg) reads = true; });
		return reads;
	}

	uint32_t ffp_state::parse_ps_light_flags(const DWORD* code)
	{
		// Float constants c92, c95 and c175 among the shader's source operands.
		bool reads_dir = false;
		bool reads_colour = false;
		bool reads_shadow_dir = false;
		bool reads_extinction = false;
		bool reads_inscatter = false;
		bool reads_grade_scale = false;
		bool reads_grade_matrix = false;
		bool reads_probe_scale = false;
		bool reads_hemisphere = false;
		bool reads_view_fade = false;
		bool reads_shaft_origin = false;
		bool reads_shaft_steps = false;
		bool reads_diffuse_scale = false;
		bool reads_effect_rim = false, reads_effect_centre = false, reads_effect_strength = false, reads_spec_scale = false;
		for_each_const_read(code, [&](DWORD reg)
		{
			if (reg == 100) reads_diffuse_scale = true;
			if (reg == 101) reads_spec_scale = true;
			if (reg == 136) reads_effect_rim = true;
			if (reg == 137) reads_effect_centre = true;
			if (reg == 138) reads_effect_strength = true;
			if (reg == 176) reads_view_fade = true;
			if (reg == 69) reads_shaft_origin = true;
			if (reg == 70) reads_shaft_steps = true;
			if (reg == 92) reads_dir = true;
			if (reg == 95) reads_colour = true;
			if (reg == 175) reads_shadow_dir = true;
			if (reg == 104) reads_extinction = true;
			if (reg == 110) reads_inscatter = true;
			if (reg == 56) reads_grade_scale = true;
			if (reg == 62) reads_grade_matrix = true;
			if (reg == 86) reads_probe_scale = true;
			if (reg == 98) reads_hemisphere = true;
		});
		return (reads_dir && reads_colour ? PS_LIGHT_DIR3 : 0u) |
			(reads_shadow_dir ? PS_LIGHT_SHADOW : 0u) |
			(reads_extinction && reads_inscatter ? PS_HAZE : 0u) |
			(reads_grade_scale && reads_grade_matrix ? PS_GRADE : 0u) |
			(reads_probe_scale && reads_hemisphere ? PS_AMBIENT : 0u) |
			(reads_view_fade ? PS_VIEW_FADE : 0u) |
			(reads_shaft_origin && reads_shaft_steps ? PS_LIGHT_SHAFT : 0u) |
			(reads_effect_rim && reads_effect_centre && reads_effect_strength ? PS_SP_EFFECT : 0u) |
			(reads_spec_scale ? PS_SPEC_SCALE : 0u) |
			// No light term at all: no probe or hemisphere (c86, c98), no directional light (c92, c95).
			// A lightmap (s6) is a light term too; on_create_pixel_shader clears the flag for those.
			(reads_diffuse_scale && reads_extinction && reads_inscatter && !reads_probe_scale && !reads_hemisphere
				&& !reads_dir && !reads_colour ? PS_UNLIT : 0u);
	}

	bool ffp_state::world_offset(float& x, float& y, float& z) const
	{
		if (offset_world_ && menu_cell_offset(x, y, z))
			return true;
		if (!offset_world_ || !camera_override_fn_ || cur_rt_menu_path_traced_)
			return false;
		return camera_override_fn_(x, y, z);
	}

	bool ffp_state::view_axes(float* right, float* forward) const
	{
		if (!transforms_cached_)
			return false;
		// Row-vector View maps world -> view, so the view axes in world space are its
		// columns: x (right) = column 0, z (forward) = column 2.
		right[0] = cached_view_[0]; right[1] = cached_view_[4]; right[2] = cached_view_[8];
		forward[0] = cached_view_[2]; forward[1] = cached_view_[6]; forward[2] = cached_view_[10];
		return true;
	}

	void ffp_state::on_set_texture(UINT stage, IDirect3DBaseTexture9* texture)
	{
		if (stage < 8)
			cur_texture_[stage] = texture;
		else if (stage == 11)
			probe_texture_ = texture;
		// A menu showing one of its character views: the only frame order Remix can path trace is one view per frame.
		if (stage == 0 && texture && !cur_rt_menu_scene_)
			for (int i = 0; i < menu_scene_count_; i++)
				if (menu_scene_targets_[i] == texture)
				{
					bool known = false;
					for (int s = 0; s < menu_shown_count_; s++)
						known |= menu_shown_[s] == texture;
					if (!known && menu_shown_count_ < 8)
					{
						menu_shown_[menu_shown_count_] = texture;
						menu_shown_widths_[menu_shown_count_++] = menu_scene_widths_[i];
					}
					break;
				}
	}

	void ffp_state::on_set_stream_source(UINT stream, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride)
	{
		if (stream < 4)
		{
			stream_vb_[stream] = vb;
			stream_offset_[stream] = offset;
			stream_stride_[stream] = stride;
		}
	}

	void ffp_state::on_set_indices(IDirect3DIndexBuffer9* ib)
	{
		cur_ib_ = ib;
	}

	void ffp_state::on_clear(DWORD /*flags*/)
	{
		// No-op: depth-prepass detection uses D3DRS_ZWRITEENABLE queried via sync_zwrite().
	}

	void ffp_state::sync_zwrite(IDirect3DDevice9* dev)
	{
		if (!cfg_ || !cfg_->zwrite_query_per_draw)
			return; // rely on the value tracked via on_set_render_state()

		ffp_prof::scope prof(ffp_prof::step::zwrite_query);
		DWORD val = TRUE;
		dev->GetRenderState(D3DRS_ZWRITEENABLE, &val);
		cur_zwrite_enabled_ = (val != 0);
	}

	void ffp_state::on_set_render_state(D3DRENDERSTATETYPE state, DWORD value)
	{
		if (state == D3DRS_ZWRITEENABLE)
			cur_zwrite_enabled_ = (value != 0);
		else if (state == D3DRS_ALPHABLENDENABLE)
			cur_alpha_blend_enabled_ = (value != 0);
		else if (state == D3DRS_TEXTUREFACTOR)
			game_tfactor_ = value;
	}

	void ffp_state::on_set_texture_stage_state(DWORD /*stage*/, D3DTEXTURESTAGESTATETYPE /*type*/, DWORD /*value*/)
	{
		// The game touched a texture stage, so the FFP stage configuration set
		// by setup_texture_stages can no longer be assumed resident.
		ffp_stages_applied_ = false;
	}

	namespace
	{
		// The row of a load game menu, in the game's world units (metres): one character every kMenuRowSpacing, wide enough
		// that a neighbour stays out of a view, from g_menu_row_distance away. Closer turns the outermost further (8.5 degrees
		// at 8 m); from much further the game's view from below is gone and foreheads show through helmets (seen at 20 m).
		constexpr float kMenuRowSpacing = 0.8f;
		float g_menu_row_distance = 8.0f;
		constexpr float kMenuRowHalfWidth = 2.0f * kMenuRowSpacing;
		constexpr int kMenuRowCells = 4;
	}

	void ffp_state::set_menu_row_distance(float distance)
	{
		g_menu_row_distance = std::max(distance, 1.0f);
	}

	bool ffp_state::menu_cell_offset(float& x, float& y, float& z) const
	{
		if (!cur_rt_menu_path_traced_ || cur_menu_cell_ < 0)
			return false;
		const float along = (cur_menu_cell_ - 0.5f * (kMenuRowCells - 1)) * kMenuRowSpacing;
		x = vs_const_[4 * 4 + 0] * along;
		y = vs_const_[5 * 4 + 0] * along;
		z = vs_const_[6 * 4 + 0] * along;
		return true;
	}

	// Off to the side of the row's camera, a character would be seen at an angle: it turns by that angle about the vertical
	// axis through the view's centre, so that each is seen from the front as the game's own camera sees it. world: 4x4, row
	// vectors, before the row's offset.
	void ffp_state::turn_menu_cell(float* world) const
	{
		if (!cur_rt_menu_path_traced_ || cur_menu_cell_ < 0)
			return;
		const float along = (cur_menu_cell_ - 0.5f * (kMenuRowCells - 1)) * kMenuRowSpacing;
		const float length = std::sqrt(along * along + g_menu_row_distance * g_menu_row_distance);
		const float sine = along / length, cosine = g_menu_row_distance / length;
		const float* c4 = &vs_const_[4 * 4];
		const float* c5 = &vs_const_[5 * 4];
		const float* c6 = &vs_const_[6 * 4];
		const float right[3] = { c4[0], c5[0], c6[0] };
		const float forward[3] = { c4[2], c5[2], c6[2] };
		const float position[3] = { c4[3], c5[3], c6[3] };
		const float distance = std::fabs(position[0] * forward[0] + position[1] * forward[1] + position[2] * forward[2]);
		auto turn = [&](float* v, bool point)
		{
			float p[3];
			for (int i = 0; i < 3; i++)
				p[i] = v[i] - (point ? position[i] + forward[i] * distance : 0.0f);
			const float a = p[0] * right[0] + p[1] * right[1] + p[2] * right[2];
			const float c = p[0] * forward[0] + p[1] * forward[1] + p[2] * forward[2];
			const float turned_a = a * cosine + c * sine;
			const float turned_c = -a * sine + c * cosine;
			for (int i = 0; i < 3; i++)
				v[i] += right[i] * (turned_a - a) + forward[i] * (turned_c - c);
		};
		turn(&world[0], false);
		turn(&world[4], false);
		turn(&world[8], false);
		turn(&world[12], true);
	}

	bool ffp_state::menu_slice_uv(IDirect3DBaseTexture9* texture, float& u0, float& v0, float& du, float& dv) const
	{
		if (const int cell = menu_cell_of(texture); cell >= 0)
		{
			if (menu_view_half_x_ <= 0.0f || menu_view_half_y_ <= 0.0f || menu_frame_half_x_ <= 0.0f || menu_frame_half_y_ <= 0.0f)
				return false;
			const float along = (cell - 0.5f * (kMenuRowCells - 1)) * kMenuRowSpacing;
			du = menu_view_half_x_ / menu_frame_half_x_;
			u0 = 0.5f + (along - menu_view_half_x_) / (2.0f * menu_frame_half_x_);
			dv = menu_view_half_y_ / menu_frame_half_y_;
			v0 = 0.5f - 0.5f * dv;
			return true;
		}
		if (!texture || texture != menu_path_trace_target_ || menu_single_du_ <= 0.0f || menu_single_dv_ <= 0.0f)
			return false;
		du = menu_single_du_;
		dv = menu_single_dv_;
		u0 = 0.5f - 0.5f * du;
		v0 = 0.5f - 0.5f * dv;
		return true;
	}

	void ffp_state::note_menu_quad(IDirect3DDevice9* dev, const void* vertices, UINT stride, UINT count)
	{
		// The private target is the size of the back buffer: Remix sizes its frame from the target it writes, and a frame the
		// size of the game's never resizes its buffers or recreates its upscaler and DLSS-NR features on entering or leaving a
		// menu (about a third of a second each way). The views sit centred in it (the projection widens to its aspect,
		// menu_slice_uv gives the quads their slice). FRPG_Menu_Common: S2 positions in eighths of the menu's 1280x720 space.
		D3DSURFACE_DESC screen = {};
		if (!dev || !vertices || stride != 12 || !count || !cur_rt0_ || FAILED(cur_rt0_->GetDesc(&screen)))
			return;
		D3DSURFACE_DESC back = {};
		IDirect3DSurface9* back_buffer = nullptr;
		if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer)) || !back_buffer)
			return;
		const HRESULT back_desc = back_buffer->GetDesc(&back);
		back_buffer->Release();
		if (FAILED(back_desc))
			return;
		int min_x = INT_MAX, max_x = INT_MIN, min_y = INT_MAX, max_y = INT_MIN;
		for (UINT i = 0; i < count; i++)
		{
			const auto* v = reinterpret_cast<const short*>(static_cast<const char*>(vertices) + static_cast<size_t>(i) * stride);
			min_x = std::min<int>(min_x, v[0]);
			max_x = std::max<int>(max_x, v[0]);
			min_y = std::min<int>(min_y, v[1]);
			max_y = std::max<int>(max_y, v[1]);
		}
		const float pixels_x = (max_x - min_x) / 8.0f * screen.Width / 1280.0f * ui_fit_x_;
		const float pixels_y = (max_y - min_y) / 8.0f * screen.Height / 720.0f * ui_fit_y_;
		const UINT width = back.Width, height = back.Height;
		if (width < 256 || height < 256 || (width == menu_native_width_ && height == menu_native_height_))
			return;
		release_menu_native();
		if (FAILED(dev->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &menu_native_texture_, nullptr)) ||
			!menu_native_texture_ || FAILED(menu_native_texture_->GetSurfaceLevel(0, &menu_native_surface_)))
		{
			release_menu_native();
			return;
		}
		menu_native_width_ = width;
		menu_native_height_ = height;
		shared::common::log("MenuScene", std::format("private target {}x{} for a quad of {:.0f}x{:.0f} pixels", width, height, pixels_x, pixels_y));
	}

	void ffp_state::release_menu_native()
	{
		if (menu_native_surface_) menu_native_surface_->Release();
		if (menu_native_texture_) menu_native_texture_->Release();
		menu_native_surface_ = nullptr;
		menu_native_texture_ = nullptr;
		menu_native_width_ = 0;
		menu_native_height_ = 0;
		// The slices belong to the target's shape: a new target sets them again before a quad can use them.
		menu_frame_half_x_ = menu_frame_half_y_ = 0.0f;
		menu_single_du_ = menu_single_dv_ = 0.0f;
	}

	void ffp_state::on_set_render_target(DWORD index, IDirect3DSurface9* rt)
	{
		if (index != 0) return;
		cur_rt0_ = rt;
		// The menus draw their characters (character creator, load game) into small square targets. The other 3D passes
		// are the 2048 shadow atlas and targets that are not square (seen across 94 captures).
		cur_rt_menu_scene_ = false;
		cur_rt_menu_path_traced_ = false;
		cur_menu_cell_ = -1;
		D3DSURFACE_DESC desc = {};
		if (rt && SUCCEEDED(rt->GetDesc(&desc)))
			cur_rt_menu_scene_ = desc.Width == desc.Height && desc.Width <= 1024;
		if (cur_rt_menu_scene_)
		{
			IDirect3DTexture9* texture = nullptr;
			if (SUCCEEDED(rt->GetContainer(IID_IDirect3DTexture9, reinterpret_cast<void**>(&texture))) && texture)
			{
				texture->Release();
				bool known = false;
				for (int i = 0; i < menu_scene_count_; i++)
					known |= menu_scene_targets_[i] == texture;
				if (!known && menu_scene_count_ < 8)
				{
					menu_scene_targets_[menu_scene_count_] = texture;
					menu_scene_widths_[menu_scene_count_++] = desc.Width;
				}
				cur_menu_cell_ = menu_cell_of(texture);
				cur_rt_menu_path_traced_ = (cur_menu_cell_ >= 0 || texture == menu_path_trace_target_) &&
					cfg_ && shared::common::config::get().ffp.menu_scene_path_trace;
				// Each view of a row has its own place and the row's camera.
				if (cur_rt_menu_path_traced_)
				{
					view_proj_dirty_ = true;
					world_dirty_ = true;
				}
			}
		}
	}

	void ffp_state::on_set_vertex_declaration(IDirect3DVertexDeclaration9* decl)
	{
		last_decl_ = decl;
		cur_decl_is_skinned_ = false;
		cur_decl_has_blend_indices_ = false;
		cur_decl_has_texcoord_ = false;
		cur_decl_has_normal_ = false;
		cur_decl_has_color_ = false;
		cur_decl_has_pos_t_ = false;
		cur_decl_texcoord_type_ = -1;
		cur_decl_texcoord_off_ = 0;
		cur_decl_feather_off_ = -1;
		cur_decl_texcoord1_off_ = -1;
		cur_decl_num_weights_ = 0;
		cur_decl_blend_weight_off_ = 0;
		cur_decl_blend_weight_type_ = 0;
		cur_decl_blend_indices_off_ = 0;
		cur_decl_blend_indices_type_ = -1;
		cur_decl_pos_off_ = 0;
		cur_decl_pos_type_ = -1;
		cur_decl_normal_off_ = 0;
		cur_decl_normal_type_ = -1;
		cur_decl_color_off_ = 0;
		cur_decl_color_type_ = -1;

		if (!decl) return;

		UINT num_elems = 0;
		if (FAILED(decl->GetDeclaration(nullptr, &num_elems))) return;
		if (num_elems == 0 || num_elems > 32) return;

		D3DVERTEXELEMENT9 elems[32];
		if (FAILED(decl->GetDeclaration(elems, &num_elems))) return;

		bool has_blend_weight = false;
		bool has_blend_indices = false;

		for (UINT e = 0; e < num_elems; e++)
		{
			const auto& el = elems[e];
			if (el.Stream == 0xFF) break;

			switch (el.Usage)
			{
			case D3DDECLUSAGE_POSITIONT:
				cur_decl_has_pos_t_ = true;
				break;

			case D3DDECLUSAGE_BLENDWEIGHT:
				has_blend_weight = true;
				cur_decl_blend_weight_off_ = el.Offset;
				cur_decl_blend_weight_type_ = el.Type;
				break;

			case D3DDECLUSAGE_BLENDINDICES:
				has_blend_indices = true;
				cur_decl_has_blend_indices_ = true;
				cur_decl_blend_indices_off_ = el.Offset;
				cur_decl_blend_indices_type_ = el.Type;
				break;

			case D3DDECLUSAGE_POSITION:
				if (el.Stream == 0 && el.UsageIndex == 0)
				{
					cur_decl_pos_off_ = el.Offset;
					cur_decl_pos_type_ = el.Type;
				}
				break;

			case D3DDECLUSAGE_NORMAL:
				if (el.Stream == 0)
				{
					cur_decl_has_normal_ = true;
					cur_decl_normal_off_ = el.Offset;
					cur_decl_normal_type_ = el.Type;
				}
				break;

			case D3DDECLUSAGE_TEXCOORD:
				if (el.UsageIndex == 0 && el.Stream == 0)
				{
					cur_decl_has_texcoord_ = true;
					cur_decl_texcoord_type_ = el.Type;
					cur_decl_texcoord_off_ = el.Offset;
				}
				// The soft sprite vertex shaders read their feather from TEXCOORD2's last component:
				// .x of a float1 (SimpleSpriteType2), .y of a float2 (the Multi sprites, DepthType6).
				else if (el.UsageIndex == 2 && el.Stream == 0 && (el.Type == D3DDECLTYPE_FLOAT1 || el.Type == D3DDECLTYPE_FLOAT2))
				{
					cur_decl_feather_off_ = el.Offset + (el.Type == D3DDECLTYPE_FLOAT2 ? 4 : 0);
				}
				else if (el.UsageIndex == 1 && el.Stream == 0 && el.Type == D3DDECLTYPE_FLOAT2)
				{
					cur_decl_texcoord1_off_ = el.Offset;
				}
				break;

			case D3DDECLUSAGE_COLOR:
				if (el.UsageIndex == 0 && el.Stream == 0)
				{
					cur_decl_has_color_ = true;
					cur_decl_color_off_ = el.Offset;
					cur_decl_color_type_ = el.Type;
				}
				break;
			}
		}

		if (has_blend_weight && has_blend_indices)
		{
			cur_decl_is_skinned_ = true;

			switch (cur_decl_blend_weight_type_)
			{
			case D3DDECLTYPE_FLOAT1:  cur_decl_num_weights_ = 1; break;
			case D3DDECLTYPE_FLOAT2:  cur_decl_num_weights_ = 2; break;
			case D3DDECLTYPE_FLOAT3:  cur_decl_num_weights_ = 3; break;
			case D3DDECLTYPE_FLOAT4:  cur_decl_num_weights_ = 3; break;
			case D3DDECLTYPE_UBYTE4N: cur_decl_num_weights_ = 3; break;
			default:                  cur_decl_num_weights_ = 3; break;
			}
		}
	}

	void ffp_state::on_present()
	{
		{
			IDirect3DBaseTexture9* target = nullptr;
			int cells = 0;
			bool small_views = menu_shown_count_ >= 1 && menu_shown_count_ <= kMenuRowCells;
			for (int i = 0; i < menu_shown_count_; i++)
				small_views &= menu_shown_widths_[i] < 256;
			if (small_views)
			{
				cells = menu_shown_count_;
				for (int i = 0; i < cells; i++)
					menu_cells_[i] = menu_shown_[i];
				target = menu_shown_[0];
			}
			else if (menu_shown_count_ == 1)
				target = menu_shown_[0];
			if (target != menu_path_trace_target_ || cells != menu_cell_count_)
				shared::common::log("MenuScene", std::format("path trace target {}, shown {}, targets {}, row of {}, enabled {}", static_cast<const void*>(target), menu_shown_count_, menu_scene_count_, cells, enabled_));
			menu_path_trace_target_ = target;
			menu_cell_count_ = cells;
		}
		menu_scene_count_ = 0;
		menu_shown_count_ = 0;

		// Auto-enable/disable: track consecutive frames above/below the eligible draw threshold. A loaded map
		// counts as above: a load screen fell below the threshold, and the new map's first frames then reached
		// Remix unconverted until the count came back, its textures smeared (without FFP Remix takes the UVs
		// from the vertex shader's TEXCOORD0 output, in the map shaders the world position).
		if (cfg_ && cfg_->auto_enable && cfg_->enabled)
		{
			int threshold = cfg_->auto_enable_threshold;
			int required = cfg_->auto_enable_frames;
			const bool map_loaded = map_loaded_fn_ && map_loaded_fn_();
			bool above = map_loaded || menu_path_trace_target_ || static_cast<int>(ffp_eligible_draws_) >= threshold;

			if (!enabled_ && above)
			{
				if (manual_disable_)
				{
					// User toggled off in-game — don't re-enable until after a menu cycle
					auto_consecutive_ = 0;
				}
				else
				{
					auto_consecutive_++;
					if (map_loaded || auto_consecutive_ >= required)
					{
						enabled_ = true;
						auto_consecutive_ = 0;
						log("FFP", std::format("Auto-enabled ({}, {} eligible draws)", map_loaded ? "map loaded" : "draw count", ffp_eligible_draws_));
					}
				}
			}
			else if (enabled_ && !above)
			{
				auto_consecutive_++;
				if (auto_consecutive_ >= required)
				{
					enabled_ = false;
					auto_consecutive_ = 0;
					manual_disable_ = false; // menu detected — reset so next enter-game auto-enables
					log("FFP", std::format("Auto-disabled (<{} eligible draws for {} frames)", threshold, required));
				}
			}
			else
			{
				auto_consecutive_ = 0;
			}
		}
		ffp_eligible_draws_ = 0;

		frame_count_++;
		ffp_setup_ = false;
		draw_call_count_ = 0;
		scene_count_ = 0;
		// Do NOT reset cur_zwrite_enabled_ — D3D9 render states persist across frames.
		// Safety net: restore shaders if a draw path exited without calling disengage.
		// No-op when already disengaged (the normal case).
		disengage(shared::globals::d3d_device);
		std::memset(vs_const_write_log_, 0, sizeof(vs_const_write_log_));

		// Frame learning: identify multi-pass geometry keys for next frame
		multi_pass_keys_.clear();
		UINT multi_count = 0;
		int max_passes = 0;
		UINT unique_keys = static_cast<UINT>(draw_key_counts_.size());
		for (auto& [key, count] : draw_key_counts_)
		{
			if (count > 1)
			{
				multi_pass_keys_[key] = count;
				multi_count++;
				if (count > max_passes) max_passes = count;
			}
		}
		dedup_max_passes_ = max_passes;

		// Log dedup stats every 60 frames (~1s)
		if (cfg_ && cfg_->dedup_pass_mode > 0 && frame_count_ % 60 == 0)
		{
			log("DEDUP", std::format("frame={} unique_keys={} multi_pass={} max_passes={} skipped={}",
				frame_count_, unique_keys, multi_count, max_passes, dedup_skipped_));

			// One-time: dump sample keys on first frame with multi-pass data
			if (multi_count > 0 && dedup_log_budget_ > 0)
			{
				int dumped = 0;
				for (auto& [key, count] : draw_key_counts_)
				{
					if (count > 1)
					{
						log("DEDUP", std::format("  key: VB={:#x} IB={:#x} base={} start={} prim={} count={}",
							key.vb, key.ib, key.base_vtx, key.start_idx, key.prim_count, count));
						if (++dumped >= 10) break;
					}
				}
			}
		}

		draw_key_counts_.clear();
		seen_this_frame_.clear();
		dedup_skipped_ = 0;

		// Global texture learning: update peak draw-count per texture.
		// Filtering uses peak >= threshold, so slider changes take effect immediately.
		for (auto& [tex, cnt] : tex0_counts_)
		{
			if (tex != 0)
			{
				auto& peak = tex0_peak_[tex];
				if (cnt > peak)
					peak = cnt;
			}
		}
		tex0_counts_.clear();

		// Prune stale cached VBs every 600 frames (~10s at 60fps)
		if (frame_count_ % 600 == 0)
		{
			for (auto it = rigid_exp_cache_.begin(); it != rigid_exp_cache_.end(); )
			{
				if (frame_count_ - it->second.last_frame > 600)
				{
					if (it->second.vb) it->second.vb->Release();
					it = rigid_exp_cache_.erase(it);
				}
				else
				{
					++it;
				}
			}
			for (auto* cache : { &rigid_overlay_cache_, &rigid_glass_cache_ })
			for (auto it = cache->begin(); it != cache->end(); )
			{
				if (frame_count_ - it->second.last_frame > 600)
				{
					if (it->second.vb) it->second.vb->Release();
					it = cache->erase(it);
				}
				else
				{
					++it;
				}
			}
			for (auto it = skinned_exp_cache_.begin(); it != skinned_exp_cache_.end(); )
			{
				if (frame_count_ - it->second.last_frame > 600)
				{
					if (it->second.vb) it->second.vb->Release();
					it = skinned_exp_cache_.erase(it);
				}
				else
				{
					++it;
				}
			}
		}
	}

	void ffp_state::on_begin_scene()
	{
		ffp_setup_ = false;
		scene_count_++;
		// on_begin_scene_cb sets WORLD to identity right after this: Transforms=1 must
		// apply the transforms again before the next converted draw.
		device_transforms_stale_ = true;
	}

	void ffp_state::on_reset()
	{
		release_menu_native();
		// The bound target is gone with the Reset: nothing may be treated (or marked) as a menu view until the next bind.
		cur_rt_menu_scene_ = false;
		cur_rt_menu_path_traced_ = false;
		cur_menu_cell_ = -1;
		if (last_vs_) { last_vs_->Release(); last_vs_ = nullptr; }
		if (last_ps_) { last_ps_->Release(); last_ps_ = nullptr; }
		last_decl_ = nullptr;

		// Default-pool resources (textures, VBs) are released by the game before Reset
		std::memset(cur_texture_, 0, sizeof(cur_texture_));
		std::memset(stream_vb_, 0, sizeof(stream_vb_));
		std::memset(stream_offset_, 0, sizeof(stream_offset_));
		std::memset(stream_stride_, 0, sizeof(stream_stride_));
		cur_ib_ = nullptr;
		draw_key_counts_.clear();
		multi_pass_keys_.clear();
		seen_this_frame_.clear();
		tex0_counts_.clear();
		tex0_peak_.clear();

		view_proj_valid_ = false;
		ffp_setup_ = false;
		world_dirty_ = false;
		view_proj_dirty_ = false;
		ffp_active_ = false;
		bone_start_reg_ = 0;
		num_bones_ = 0;
		cur_zwrite_enabled_ = true; // D3D9 device default

		cur_decl_is_skinned_ = false;
		cur_decl_has_blend_indices_ = false;
		cur_decl_has_texcoord_ = false;
		cur_decl_has_normal_ = false;
		cur_decl_has_color_ = false;
		cur_decl_has_pos_t_ = false;
		cur_decl_texcoord_type_ = -1;
		cur_decl_texcoord_off_ = 0;
		cur_decl_feather_off_ = -1;
		cur_decl_texcoord1_off_ = -1;
		cur_decl_num_weights_ = 0;
		cur_decl_blend_weight_off_ = 0;
		cur_decl_blend_weight_type_ = 0;
		cur_decl_blend_indices_off_ = 0;
		cur_decl_blend_indices_type_ = -1;
		cur_decl_pos_off_ = 0;
		cur_decl_pos_type_ = -1;
		cur_decl_normal_off_ = 0;
		cur_decl_normal_type_ = -1;
		cur_decl_color_off_ = 0;
		cur_decl_color_type_ = -1;

		std::memset(vs_const_write_log_, 0, sizeof(vs_const_write_log_));

		// Rigid expansion resources
		for (auto& [k, e] : rigid_exp_cache_)
		{
			if (e.vb) e.vb->Release();
		}
		rigid_exp_cache_.clear();
		for (auto& [k, e] : rigid_overlay_cache_)
		{
			if (e.vb) e.vb->Release();
		}
		rigid_overlay_cache_.clear();
		for (auto& [k, e] : rigid_glass_cache_)
		{
			if (e.vb) e.vb->Release();
		}
		rigid_glass_cache_.clear();
		rigid_exp_active_ = false;
		if (rigid_exp_decl_) { rigid_exp_decl_->Release(); rigid_exp_decl_ = nullptr; }

		// Skinned expansion resources
		for (auto& [k, e] : skinned_exp_cache_)
		{
			if (e.vb) e.vb->Release();
		}
		skinned_exp_cache_.clear();
		skinned_exp_active_ = false;
		if (skinned_exp_decl_) { skinned_exp_decl_->Release(); skinned_exp_decl_ = nullptr; }

		if (water_fallback_tex_) { water_fallback_tex_->Release(); water_fallback_tex_ = nullptr; }
		if (white_sprite_tex_) { white_sprite_tex_->Release(); white_sprite_tex_ = nullptr; }
		cur_ps_is_water_surface_ = false;
		cur_ps_swamp_pass_ = swamp_pass::None;
		cur_ps_light_flags_ = 0;
		cur_ps_draw_tint_ = false;
		cur_ps_point_lights_ = 0;

		log("FFP", "State reset");
	}

	void ffp_state::record_draw_key(IDirect3DVertexBuffer9* vb, IDirect3DIndexBuffer9* ib,
		INT base_vtx, UINT start_idx, UINT prim_count)
	{
		if (!cfg_ || cfg_->dedup_pass_mode == 0) return;

		draw_key key{
			reinterpret_cast<uintptr_t>(vb),
			reinterpret_cast<uintptr_t>(ib),
			base_vtx, start_idx, prim_count
		};
		draw_key_counts_[key]++;
	}

	bool ffp_state::check_dedup_skip(IDirect3DVertexBuffer9* vb, IDirect3DIndexBuffer9* ib,
		INT base_vtx, UINT start_idx, UINT prim_count, bool skinned)
	{
		if (!cfg_ || cfg_->dedup_pass_mode == 0) return false;

		draw_key key{
			reinterpret_cast<uintptr_t>(vb),
			reinterpret_cast<uintptr_t>(ib),
			base_vtx, start_idx, prim_count
		};

		auto it = multi_pass_keys_.find(key);
		if (it == multi_pass_keys_.end()) return false;

		int total_passes = it->second;
		int occurrence = seen_this_frame_[key]++;

		bool skip = false;
		int mode = skinned ? 1 : cfg_->dedup_pass_mode;  // skinned always keeps first
		switch (mode)
		{
		case 1: // keep first
			skip = (occurrence != 0);
			break;
		case 2: // keep last
			skip = (occurrence != total_passes - 1);
			break;
		case 3: // keep all
			skip = false;
			break;
		default:
			break;
		}

		if (skip)
		{
			dedup_skipped_++;
			if (dedup_log_budget_ > 0)
			{
				dedup_log_budget_--;
				log("DEDUP", std::format("SKIP draw#{} pass={}/{} prim={} tex0={:#x}",
					draw_call_count_, occurrence + 1, total_passes, prim_count,
					reinterpret_cast<uintptr_t>(cur_texture_[0])));
			}
		}

		return skip;
	}

	bool ffp_state::should_filter_texture()
	{
		if (!cfg_ || !cfg_->tex_filter) return false;

		int as = cfg_->albedo_stage;
		auto* tex = (as >= 0 && as < 8) ? cur_texture_[as] : cur_texture_[0];
		auto tex_ptr = reinterpret_cast<uintptr_t>(tex);

		// Record for global-texture learning
		if (tex_ptr != 0)
			tex0_counts_[tex_ptr]++;

		// Filter: skip textures whose peak draw-count >= threshold
		UINT threshold = cfg_ ? static_cast<UINT>(cfg_->tex_filter_threshold) : 80;
		auto it = tex0_peak_.find(tex_ptr);
		return it != tex0_peak_.end() && it->second >= threshold;
	}

	void ffp_state::clear_global_textures()
	{
		tex0_peak_.clear();
		tex0_counts_.clear();
		log("TEXFLT", "peak counts cleared");
	}

	UINT ffp_state::tex_filtered_count() const
	{
		if (!cfg_) return 0;
		UINT threshold = static_cast<UINT>(cfg_->tex_filter_threshold);
		UINT count = 0;
		for (auto& [tex, peak] : tex0_peak_)
		{
			if (peak >= threshold) count++;
		}
		return count;
	}

	bool ffp_state::setup_albedo_or_fallback(IDirect3DDevice9* dev)
	{
		if (!cfg_ || !dev) return false;

		// Skip draws using the globally-shared texture
		if (should_filter_texture())
			return false;

		int as = cfg_->albedo_stage;
		auto* tex = (as >= 0 && as < 8) ? cur_texture_[as] : cur_texture_[0];
		auto tex_ptr = reinterpret_cast<uintptr_t>(tex);

		// Skip draws with NULL albedo
		if (tex_ptr == 0)
			return false;

		ffp_prof::scope prof(ffp_prof::step::albedo);

		// Bind the per-mesh albedo and clear other stages
		dev->SetTexture(0, tex);
		if (!steps_ || steps_->albedo_null_stages)
		{
			for (DWORD ts = 1; ts < 8; ts++)
				dev->SetTexture(ts, nullptr);
			nulled_high_stages_ = true;
		}
		return true;
	}

	bool ffp_state::setup_water_albedo(IDirect3DDevice9* dev)
	{
		if (!dev) return false;

		auto static_texture_on = [&](DWORD ts) -> IDirect3DBaseTexture9*
		{
			auto* tex = cur_texture_[ts];
			if (!tex || tex->GetType() != D3DRTYPE_TEXTURE)
				return nullptr;

			D3DSURFACE_DESC desc{};
			auto* t2d = static_cast<IDirect3DTexture9*>(tex);
			if (FAILED(t2d->GetLevelDesc(0, &desc)) ||
				(desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)))
				return nullptr;

			return tex;
		};

		// A static stage-0 diffuse (the WWS_Dif case) is always the best albedo.
		// Env water binds nothing on s0; Reflect water binds the per-frame
		// reflection RT — for those, WaterFallbackAlbedo picks the policy:
		// the generated solid-color fallback (default — its fresh hash cannot
		// collide with rtx.conf ignore/hide lists), or the first static texture
		// on s1-s7 (the scroll normal maps; useful only for diagnosing
		// texture-hash filtering, since those hashes may be ignored/hidden in
		// rtx.conf, which makes the water instance vanish).
		IDirect3DBaseTexture9* chosen = static_texture_on(0);
		if (!chosen && !cfg_->water_fallback_albedo)
		{
			for (DWORD ts = 1; ts < 8 && !chosen; ts++)
				chosen = static_texture_on(ts);
		}

		if (!chosen)
		{
			if (!water_fallback_tex_)
			{
				if (SUCCEEDED(dev->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8,
					D3DPOOL_MANAGED, &water_fallback_tex_, nullptr)))
				{
					D3DLOCKED_RECT lr{};
					if (SUCCEEDED(water_fallback_tex_->LockRect(0, &lr, nullptr, 0)))
					{
						for (int y = 0; y < 8; y++)
						{
							auto* row = reinterpret_cast<DWORD*>(
								static_cast<unsigned char*>(lr.pBits) + y * lr.Pitch);
							for (int x = 0; x < 8; x++)
								row[x] = 0xFF2F5D73; // muted water blue
						}
						water_fallback_tex_->UnlockRect(0);
					}
				}
			}
			chosen = water_fallback_tex_;
			if (!chosen) return false;
		}

		static int albedo_log_budget = 8;
		if (albedo_log_budget > 0)
		{
			albedo_log_budget--;
			log("WATER", std::format("albedo={} ptr={:#x}",
				chosen == water_fallback_tex_ ? "fallback" : "static",
				reinterpret_cast<uintptr_t>(chosen)));
		}

		ffp_prof::scope prof(ffp_prof::step::albedo);
		dev->SetTexture(0, chosen);
		if (!steps_ || steps_->albedo_null_stages)
		{
			for (DWORD ts = 1; ts < 8; ts++)
				dev->SetTexture(ts, nullptr);
			nulled_high_stages_ = true;
		}
		return true;
	}

	// ---- State consumers ----

	void ffp_state::engage(IDirect3DDevice9* dev)
	{
		if (!cfg_ || !enabled_ || !dev) return;

		ffp_prof::scope prof(ffp_prof::step::engage);

		// Every draw into the private menu target carries its marker: set again here, since the game can restore sampler state
		// (state blocks) between binding the target and drawing. Only while the private target stands in: the game's own small
		// target (the first menu frame, or after a Reset) is not marked, so its frame stays raster and never resizes Remix's.
		if (menu_native_surface())
			dev->SetSamplerState(15, D3DSAMP_BORDERCOLOR, kMenuSceneMarker);

		if (!ffp_active_)
		{
			dev->SetVertexShader(nullptr);
			dev->SetPixelShader(nullptr);
			ffp_active_ = true;
			// Shaders were just nulled; whatever stage state the game left is
			// not the FFP configuration.
			ffp_stages_applied_ = false;
		}

		// TransformsLevel 2 = re-apply every draw (stock), 1 = only when a VS
		// constant actually changed, 0 = never (diagnostic only — the geometry
		// will be placed with whatever transform was last set).
		const int tf_level = steps_ ? steps_->transforms : 2;
		if (tf_level >= 2 || (tf_level == 1 && (view_proj_dirty_ || world_dirty_ || !transforms_cached_ || device_transforms_stale_)))
		{
			ffp_prof::scope tprof(ffp_prof::step::transforms);
			apply_transforms(dev);
			device_transforms_stale_ = false;
		}
		else if (tf_level == 1)
		{
			// Nothing changed since the last upload, so the device already holds
			// the right matrices. Scoped anyway so the call count shows how often
			// the stock path was re-uploading them for nothing.
			ffp_prof::scope tprof(ffp_prof::step::transforms);
		}

		// TextureStagesLevel 2 = every draw (stock), 1 = only after the game
		// disturbed the stages, 0 = never.
		const int ts_level = steps_ ? steps_->tex_stages : 2;
		if (ts_level >= 2 || (ts_level == 1 && !ffp_stages_applied_))
		{
			ffp_prof::scope sprof(ffp_prof::step::tex_stages);
			setup_texture_stages(dev);
			ffp_stages_applied_ = true;
		}

		if (!ffp_setup_)
		{
			setup_lighting(dev);
			ffp_setup_ = true;
		}
	}

	void ffp_state::disengage(IDirect3DDevice9* dev)
	{
		bool was_ffp = ffp_active_;

		ffp_prof::scope prof(ffp_prof::step::disengage);

		if (ffp_active_ && dev)
		{
			dev->SetVertexShader(last_vs_);
			dev->SetPixelShader(last_ps_);
			dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
			ffp_active_ = false;
			ffp_stages_applied_ = false;
		}

		// Ensure D3D transforms match current VS constants for the upcoming shader draw.
		// Remix's vertex capture reads VIEW/PROJECTION/WORLD and inverts them to recover
		// object-space positions from gl_Position.  We use the raw (uncleaned) projection
		// so that View × Proj = VP exactly — the cleaned version has zeroed off-diagonals
		// that break the inverse.  We also recompute when dirty (VS constants changed since
		// last update) to prevent stale transforms from a prior render pass.
		if (!view_proj_valid_ || !dev) return;

		bool needs_update = view_proj_dirty_ || world_dirty_ || was_ffp || !transforms_cached_;
		if (!needs_update) return;

		if (view_proj_dirty_ || world_dirty_)
			apply_transforms(dev); // full recompute; caches both raw and cleaned proj

		if (transforms_cached_)
		{
			dev->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(cached_view_));
			dev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(cached_proj_raw_));
			dev->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(cached_world_));
			// The device now holds the raw projection, not the cleaned one engage
			// sets: Transforms=1 must apply them again on the next engage.
			device_transforms_stale_ = true;
		}
	}

	void ffp_state::setup_albedo_texture(IDirect3DDevice9* dev)
	{
		if (!cfg_ || !dev) return;

		int as = cfg_->albedo_stage;
		auto* albedo = (as >= 0 && as < 8) ? cur_texture_[as] : cur_texture_[0];

		dev->SetTexture(0, albedo);
		for (DWORD ts = 1; ts < 8; ts++)
			dev->SetTexture(ts, nullptr);
		nulled_high_stages_ = true;
	}

	void ffp_state::restore_textures(IDirect3DDevice9* dev)
	{
		if (!dev) return;

		ffp_prof::scope prof(ffp_prof::step::restore_tex);

		// RestoreTexturesLevel 2 = all 8 stages (stock), 1 = only the stages the
		// conversion actually rebound, 0 = none. Level 1 relies on every path that
		// nulls stages 1-7 (the albedo binds, the skinned albedo and the overlay)
		// setting nulled_high_stages_.
		const int level = steps_ ? steps_->restore_textures : 2;
		if (level == 0) return;

		dev->SetTexture(0, cur_texture_[0]);
		if (level >= 2 || nulled_high_stages_)
		{
			for (DWORD ts = 1; ts < 8; ts++)
				dev->SetTexture(ts, cur_texture_[ts]);
		}
		nulled_high_stages_ = false;
	}

	// ---- Camera position extraction ----

	void ffp_state::camera_world_pos(float& x, float& y, float& z) const
	{
		// Camera position lives in c4.w / c5.w / c6.w (confirmed via RenderDoc capture).
		// These are the real world-space camera coordinates the game uploads each draw.
		const float* c4 = &vs_const_[4 * 4];
		const float* c5 = &vs_const_[5 * 4];
		const float* c6 = &vs_const_[6 * 4];
		x = c4[3];
		y = c5[3];
		z = c6[3];
	}

	void ffp_state::view_to_world_pos(float& x, float& y, float& z) const
	{
		const float* c4 = &vs_const_[4 * 4];
		const float* c5 = &vs_const_[5 * 4];
		const float* c6 = &vs_const_[6 * 4];

		const float view_x = x;
		const float view_y = y;
		const float view_z = z;

		x = view_x * c4[0] + view_y * c5[0] + view_z * c6[0] + c4[3];
		y = view_x * c4[1] + view_y * c5[1] + view_z * c6[1] + c5[3];
		z = view_x * c4[2] + view_y * c5[2] + view_z * c6[2] + c6[3];
	}

	// ---- Internal helpers ----

	static void mat4_mul(float* dst, const float* a, const float* b)
	{
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 4; c++)
			{
				float sum = 0.0f;
				for (int k = 0; k < 4; k++)
					sum += a[r * 4 + k] * b[k * 4 + c];
				dst[r * 4 + c] = sum;
			}
	}

	static void mat4_invert_rigid(float* dst, const float* m)
	{
		// Invert a rigid-body transform: rotation transpose + negated translation
		dst[0]  = m[0]; dst[1]  = m[4]; dst[2]  = m[8];  dst[3]  = 0.0f;
		dst[4]  = m[1]; dst[5]  = m[5]; dst[6]  = m[9];  dst[7]  = 0.0f;
		dst[8]  = m[2]; dst[9]  = m[6]; dst[10] = m[10]; dst[11] = 0.0f;
		dst[12] = -(m[12]*m[0] + m[13]*m[1] + m[14]*m[2]);
		dst[13] = -(m[12]*m[4] + m[13]*m[5] + m[14]*m[6]);
		dst[14] = -(m[12]*m[8] + m[13]*m[9] + m[14]*m[10]);
		dst[15] = 1.0f;
	}

	static bool mat4_invert_affine(float* dst, const float* m)
	{
		const float a = m[0],  b = m[1],  c = m[2];
		const float d = m[4],  e = m[5],  f = m[6];
		const float g = m[8],  h = m[9],  i = m[10];

		const float cofactor00 =  (e * i) - (f * h);
		const float cofactor01 = -((d * i) - (f * g));
		const float cofactor02 =  (d * h) - (e * g);
		const float det = (a * cofactor00) + (b * cofactor01) + (c * cofactor02);
		if (std::fabs(det) < 1.0e-6f)
		{
			return false;
		}

		const float inv_det = 1.0f / det;
		const float inv00 = cofactor00 * inv_det;
		const float inv01 = -((b * i) - (c * h)) * inv_det;
		const float inv02 =  ((b * f) - (c * e)) * inv_det;
		const float inv10 = cofactor01 * inv_det;
		const float inv11 =  ((a * i) - (c * g)) * inv_det;
		const float inv12 = -((a * f) - (c * d)) * inv_det;
		const float inv20 = cofactor02 * inv_det;
		const float inv21 = -((a * h) - (b * g)) * inv_det;
		const float inv22 =  ((a * e) - (b * d)) * inv_det;

		dst[0] = inv00; dst[1] = inv01; dst[2] = inv02; dst[3] = 0.0f;
		dst[4] = inv10; dst[5] = inv11; dst[6] = inv12; dst[7] = 0.0f;
		dst[8] = inv20; dst[9] = inv21; dst[10] = inv22; dst[11] = 0.0f;

		const float tx = m[12];
		const float ty = m[13];
		const float tz = m[14];
		dst[12] = -((tx * inv00) + (ty * inv10) + (tz * inv20));
		dst[13] = -((tx * inv01) + (ty * inv11) + (tz * inv21));
		dst[14] = -((tx * inv02) + (ty * inv12) + (tz * inv22));
		dst[15] = 1.0f;
		return true;
	}

	static BYTE float_to_color(float value)
	{
		value = std::clamp(value, 0.0f, 1.0f);
		return static_cast<BYTE>(value * 255.0f + 0.5f);
	}

	static DWORD pack_decl_color(const unsigned char* vertex, int offset, int type)
	{
		switch (type)
		{
		case D3DDECLTYPE_D3DCOLOR:
			return *reinterpret_cast<const DWORD*>(&vertex[offset]);

		case D3DDECLTYPE_UBYTE4:
		case D3DDECLTYPE_UBYTE4N:
			return D3DCOLOR_ARGB(vertex[offset + 3], vertex[offset], vertex[offset + 1], vertex[offset + 2]);

		case D3DDECLTYPE_FLOAT4:
		{
			auto* color = reinterpret_cast<const float*>(&vertex[offset]);
			return D3DCOLOR_ARGB(
				float_to_color(color[3]),
				float_to_color(color[0]),
				float_to_color(color[1]),
				float_to_color(color[2]));
		}

		default:
			return 0xFFFFFFFFu;
		}
	}

	DWORD ffp_state::cur_vertex_color(const unsigned char* vertex) const
	{
		return cur_decl_has_color_ ? pack_decl_color(vertex, cur_decl_color_off_, cur_decl_color_type_) : 0xFFFFFFFFu;
	}

	bool ffp_state::vs_camera_basis_is_orthonormal() const
	{
		const float* c4 = &vs_const_[4 * 4];
		const float* c5 = &vs_const_[5 * 4];
		const float* c6 = &vs_const_[6 * 4];

		const float rows[3][3] = {
			{ c4[0], c4[1], c4[2] },
			{ c5[0], c5[1], c5[2] },
			{ c6[0], c6[1], c6[2] },
		};

		constexpr float kTolerance = 0.02f;
		for (const auto& row : rows)
		{
			const float length_sq = row[0] * row[0] + row[1] * row[1] + row[2] * row[2];
			if (std::fabs(length_sq - 1.0f) > kTolerance)
			{
				return false;
			}
		}

		for (int i = 0; i < 3; ++i)
		{
			const int j = (i + 1) % 3;
			const float dot = rows[i][0] * rows[j][0] + rows[i][1] * rows[j][1] + rows[i][2] * rows[j][2];
			if (std::fabs(dot) > kTolerance)
			{
				return false;
			}
		}

		return true;
	}

	void ffp_state::apply_transforms(IDirect3DDevice9* dev)
	{
		float override_cam_x = 0.0f;
		float override_cam_y = 0.0f;
		float override_cam_z = 0.0f;
		const bool has_override_camera = menu_cell_offset(override_cam_x, override_cam_y, override_cam_z) ||
			(camera_override_fn_ && !cur_rt_menu_path_traced_ &&
			camera_override_fn_(override_cam_x, override_cam_y, override_cam_z));

		// c0-c6 are shared by every subsystem, each with its own register layout: the
		// world pass uploads a camera basis there, but the 2D/menu pass overwrites the
		// same registers with an orthographic screen transform. Nothing in the constant
		// stream says which layout is resident, so the basis itself is the discriminator
		// — a camera basis is orthonormal, a screen transform is not. Without this test
		// the last (menu) upload of a frame poisons the cache and, because untouched
		// draws re-apply that cache, every following world draw is handed a degenerate
		// camera even though the image still resolves correctly.
		const bool camera_basis_valid = vs_camera_basis_is_orthonormal();
		if (camera_basis_valid)
		{
			++camera_basis_accepted_;
		}
		else
		{
			++camera_basis_rejected_;
		}

		// View + Projection: recompute when dirty, otherwise re-apply cached.
		// Re-apply is needed because disengage overwrites D3D Proj with raw (uncleaned).
		if (view_proj_dirty_ && (camera_basis_valid || !have_valid_camera_))
		{
			have_valid_camera_ = have_valid_camera_ || camera_basis_valid;
			/*
			 * Dark Souls PTDE: VS constants c0-c3 = combined ViewProj (column-major).
			 * No separate View/Proj upload. c4-c6 = camera basis (.xyz) + world pos (.w).
			 *
			 * DS1 uses camera-relative rendering: all world geometry is positioned relative
			 * to the camera, so c4-c6.w = 0 and View would have the camera at the origin.
			 *
			 * Decompose into View and Proj for Remix:
			 *   cam2world = rotation from c4-c6.xyz, translation from inject_view_pos (game memory)
			 *   View = invert_rigid(cam2world)   — camera at absolute world position
			 *   Proj = cam2world_rotation_only * VP  — isolates projection, strips camera translation
			 *   offset_world adds camera pos to each World matrix — converts camera-relative → absolute
			 */
			const float* c4 = &vs_const_[4 * 4];
			const float* c5 = &vs_const_[5 * 4];
			const float* c6 = &vs_const_[6 * 4];

			// cam2world: rotation from c4-c6.xyz, translation from c4-c6.w
			float cam2world[16];
			cam2world[0]  = c4[0]; cam2world[1]  = c5[0]; cam2world[2]  = c6[0]; cam2world[3]  = 0.0f;
			cam2world[4]  = c4[1]; cam2world[5]  = c5[1]; cam2world[6]  = c6[1]; cam2world[7]  = 0.0f;
			cam2world[8]  = c4[2]; cam2world[9]  = c5[2]; cam2world[10] = c6[2]; cam2world[11] = 0.0f;
			cam2world[12] = c4[3]; cam2world[13] = c5[3]; cam2world[14] = c6[3]; cam2world[15] = 1.0f;

			if (inject_view_pos_ && has_override_camera && !cur_rt_menu_path_traced_)
			{
				cam2world[12] = override_cam_x;
				cam2world[13] = override_cam_y;
				cam2world[14] = override_cam_z;
			}

			// A row's camera: the views' own, backed off along its axis. Their frustum at the character gives the slices.
			const bool menu_row = cur_rt_menu_path_traced_ && cur_menu_cell_ >= 0;
			if (menu_row)
			{
				const float distance = std::fabs(c4[3] * c4[2] + c5[3] * c5[2] + c6[3] * c6[2]);
				menu_view_half_x_ = distance / std::max(vs_const_[0], 1e-3f);
				menu_view_half_y_ = distance / std::max(vs_const_[5], 1e-3f);
				for (int i = 0; i < 3; i++)
					cam2world[12 + i] -= cam2world[8 + i] * (g_menu_row_distance - distance);
			}

			if (!mat4_invert_affine(cached_view_, cam2world))
				mat4_invert_rigid(cached_view_, cam2world);
			dev->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(cached_view_));

			// Proj = cam2world_rotation_only * VP (isolate projection).
			// IMPORTANT: strip cam2world translation before multiplying — the injected camera
			// position must NOT contaminate the projection matrix.  Using the full cam2world
			// (with translation) would add a camera-offset term to the perspective transform,
			// producing incorrect clip-space coordinates.  Translation cancels analytically
			// (cam2world_rot × R^T = I) so stripping it is always correct.
			float cam2world_rot[16];
			std::memcpy(cam2world_rot, cam2world, sizeof(cam2world_rot));
			cam2world_rot[12] = 0.0f; cam2world_rot[13] = 0.0f; cam2world_rot[14] = 0.0f;
			// A menu's view is not camera-relative: its VP holds the camera's translation (c4-c6.w) too.
			if (cur_rt_menu_path_traced_)
			{
				cam2world_rot[12] = c4[3]; cam2world_rot[13] = c5[3]; cam2world_rot[14] = c6[3];
			}
			float vp[16];
			mat4_transpose(vp, &vs_const_[vs_reg_view_start_ * 4]);
			mat4_mul(cached_proj_, cam2world_rot, vp);

			// The camera-relative view-projection the particle sprite path compares against.
			std::memcpy(map_viewproj_rows_, &vs_const_[vs_reg_view_start_ * 4], sizeof(map_viewproj_rows_));
			have_map_viewproj_ = true;

			// Save the raw (uncleaned) projection — View × Proj_raw = VP exactly.
			// Vertex capture needs this for correct back-transforms on shader draws.
			std::memcpy(cached_proj_raw_, cached_proj_, sizeof(cached_proj_raw_));

			// Clean off-diagonal noise for a proper perspective matrix:
			// [Sx   0   0   0 ]
			// [ 0  Sy   0   0 ]
			// [ 0   0   A   1 ]  <- D3D LH perspective marker
			// [ 0   0   B   0 ]
			cached_proj_[1]  = 0.0f; cached_proj_[2]  = 0.0f; cached_proj_[3]  = 0.0f;
			cached_proj_[4]  = 0.0f; cached_proj_[6]  = 0.0f; cached_proj_[7]  = 0.0f;
			cached_proj_[8]  = 0.0f; cached_proj_[9]  = 0.0f;
			cached_proj_[11] = 1.0f;
			cached_proj_[12] = 0.0f; cached_proj_[13] = 0.0f; cached_proj_[15] = 0.0f;
			// The private target's shape (height over width): the views sit centred in it.
			const float menu_aspect = menu_native_width_ && menu_native_height_
				? static_cast<float>(menu_native_height_) / static_cast<float>(menu_native_width_) : 0.0f;
			if (menu_row && menu_aspect > 0.0f)
			{
				// The row's width at the characters, or wider where the views' height needs more than the target's shape gives.
				menu_frame_half_x_ = std::max(kMenuRowHalfWidth, 1.02f * menu_view_half_y_ / menu_aspect);
				menu_frame_half_y_ = menu_frame_half_x_ * menu_aspect;
				const float near_z = std::max(g_menu_row_distance - 5.0f, 0.05f), far_z = g_menu_row_distance + 5.0f;
				cached_proj_[0] = g_menu_row_distance / menu_frame_half_x_;
				cached_proj_[5] = g_menu_row_distance / menu_frame_half_y_;
				cached_proj_[10] = far_z / (far_z - near_z);
				cached_proj_[14] = -near_z * cached_proj_[10];
				std::memcpy(cached_proj_raw_, cached_proj_, sizeof(cached_proj_raw_));
			}
			else if (cur_rt_menu_path_traced_ && menu_aspect > 0.0f && cached_proj_[0] > 0.0f && cached_proj_[5] > 0.0f)
			{
				// A single view (the character creator): the game's own field, widened (or heightened) to the target's shape.
				const float half_x = 1.0f / cached_proj_[0], half_y = 1.0f / cached_proj_[5];
				const float frame_x = std::max(half_x, half_y / menu_aspect), frame_y = frame_x * menu_aspect;
				menu_single_du_ = half_x / frame_x;
				menu_single_dv_ = half_y / frame_y;
				cached_proj_[0] = 1.0f / frame_x;
				cached_proj_[5] = 1.0f / frame_y;
			}
			dev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(cached_proj_));

			// Log: determinant of the 3x3 rotation part of cam2world (negative = mirror/flip)
			float det3 = cam2world[0] * (cam2world[5]*cam2world[10] - cam2world[6]*cam2world[9])
			           - cam2world[1] * (cam2world[4]*cam2world[10] - cam2world[6]*cam2world[8])
			           + cam2world[2] * (cam2world[4]*cam2world[9]  - cam2world[5]*cam2world[8]);
			cam_log_write(frame_count_, c4, c5, c6, det3, cached_view_, cached_proj_);

			view_proj_dirty_ = false;
		}
		else if (transforms_cached_)
		{
			dev->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(cached_view_));
			dev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(cached_proj_));
		}

		if (world_dirty_)
		{
			// World = c8-c10: 4x3 bone matrix stored column-major (3 vec4 columns).
			// Convert to 4x4 row-major with implicit 4th column [0,0,0,1].
			const float* src = &vs_const_[vs_reg_world_start_ * 4];
			cached_world_[0]  = src[0]; cached_world_[1]  = src[4]; cached_world_[2]  = src[8];  cached_world_[3]  = 0.0f;
			cached_world_[4]  = src[1]; cached_world_[5]  = src[5]; cached_world_[6]  = src[9];  cached_world_[7]  = 0.0f;
			cached_world_[8]  = src[2]; cached_world_[9]  = src[6]; cached_world_[10] = src[10]; cached_world_[11] = 0.0f;
			cached_world_[12] = src[3]; cached_world_[13] = src[7]; cached_world_[14] = src[11]; cached_world_[15] = 1.0f;

			turn_menu_cell(cached_world_);
			if (offset_world_ && has_override_camera)
			{
				cached_world_[12] += override_cam_x;
				cached_world_[13] += override_cam_y;
				cached_world_[14] += override_cam_z;
			}

			dev->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(cached_world_));

			world_dirty_ = false;
		}
		else if (transforms_cached_)
		{
			dev->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(cached_world_));
		}

		transforms_cached_ = true;
	}

	void ffp_state::setup_lighting(IDirect3DDevice9* dev)
	{
		dev->SetRenderState(D3DRS_LIGHTING, FALSE);

		D3DMATERIAL9 mat = {};
		mat.Diffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
		mat.Ambient = { 1.0f, 1.0f, 1.0f, 1.0f };
		dev->SetMaterial(&mat);
	}

	void ffp_state::setup_texture_stages(IDirect3DDevice9* dev)
	{
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CURRENT);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
		dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);

		// UV scale is baked into the vertices by expand_rigid_texcoords; a scrolling material's
		// offset goes to Remix as stage 0's texture transform (game/texture_scroll.hpp).
		dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);

		// Disable remaining stages: the game binds shadow maps, LUTs, normal maps etc.
		// on higher stages for its pixel shaders. In FFP mode those become active
		// and Remix may consume the wrong textures.
		for (DWORD s = 1; s <= 7; s++)
		{
			dev->SetTextureStageState(s, D3DTSS_COLOROP, D3DTOP_DISABLE);
			dev->SetTextureStageState(s, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
			dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, s);
		}
	}

	bool ffp_state::apply_draw_tint(IDirect3DDevice9* dev)
	{
		// Water reads c139 too, but as a UV scale (FRPG_WWS WaterWave), not a tint.
		if (!dev || !cfg_ || !cfg_->draw_tint || !cur_ps_draw_tint_ || cur_ps_is_water_surface_)
			return false;

		const float* c = &ps_const_[139 * 4];
		if (c[0] == 1.0f && c[1] == 1.0f && c[2] == 1.0f && c[3] == 1.0f)
			return false; // the common case: keep the material identical to an untinted draw

		auto to_byte = [](float v) -> DWORD
		{
			v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
			return static_cast<DWORD>(v * 255.0f + 0.5f);
		};
		const DWORD tfactor = D3DCOLOR_ARGB(to_byte(c[3]), to_byte(c[0]), to_byte(c[1]), to_byte(c[2]));

		dev->SetRenderState(D3DRS_TEXTUREFACTOR, tfactor);
		dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
		dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
		dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
		dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
		dev->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
		dev->SetTextureStageState(1, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
		return true;
	}

	void ffp_state::clear_draw_tint(IDirect3DDevice9* dev)
	{
		if (!dev) return;
		// Back to what setup_texture_stages left, so the TextureStages=1 latch
		// (ffp_stages_applied_) stays true.
		dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		dev->SetRenderState(D3DRS_TEXTUREFACTOR, game_tfactor_);
	}

	bool ffp_state::apply_sp_effect(IDirect3DDevice9* dev)
	{
		if (!dev)
			return false;
		IDirect3DBaseTexture9* const spec_map = cur_texture(1);
		const float* strength = &ps_const_[138 * 4];
		if (!(cur_ps_light_flags_ & PS_SP_EFFECT) || strength[0] == 0.0f)
		{
			if ((cur_ps_light_flags_ & PS_SPEC_SCALE) && spec_map && ps_const_[101 * 4] > 0.0f)
				spec_scale_[spec_map] = ps_const_[101 * 4];
			return false;
		}

		const float* rim = &ps_const_[136 * 4];
		const float* centre = &ps_const_[137 * 4];
		const auto it = spec_map ? spec_scale_.find(spec_map) : spec_scale_.end();
		const float own = it != spec_scale_.end() ? it->second : 1.5f;
		D3DMATRIX m = {};
		m._11 = rim[0] * rim[3] * strength[0];
		m._12 = rim[1] * rim[3] * strength[0];
		m._13 = rim[2] * rim[3] * strength[0];
		m._21 = centre[0] * centre[3] * strength[0];
		m._22 = centre[1] * centre[3] * strength[0];
		m._23 = centre[2] * centre[3] * strength[0];
		m._31 = ps_const_[101 * 4] / own;
		m._32 = ps_const_[102 * 4];
		m._44 = 1.0f;
		dev->SetTransform(D3DTS_TEXTURE4, &m);
		dev->SetTextureStageState(4, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
		return true;
	}

	void ffp_state::clear_sp_effect(IDirect3DDevice9* dev)
	{
		if (dev)
			dev->SetTextureStageState(4, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	}

	namespace
	{
		// At least this share of a DXT1 texture's top level is punch-through (transparent) texels: the rats'
		// fur shell texture is a field of hair tips over holes; a DXT1 texture with a stray transparent
		// block stays out.
		constexpr float kMinPunchThroughShare = 0.005f;

		bool scan_punch_through(IDirect3DBaseTexture9* texture)
		{
			if (!texture || texture->GetType() != D3DRTYPE_TEXTURE)
				return false;
			auto* tex = static_cast<IDirect3DTexture9*>(texture);
			D3DSURFACE_DESC desc{};
			if (FAILED(tex->GetLevelDesc(0, &desc)) || desc.Format != D3DFMT_DXT1 || desc.Width < 4 || desc.Height < 4)
				return false;
			D3DLOCKED_RECT rect{};
			if (FAILED(tex->LockRect(0, &rect, nullptr, D3DLOCK_READONLY)) || !rect.pBits)
				return false;
			// A DXT1 block in its three-colour mode (colour0 <= colour1) makes index 3 transparent.
			const UINT blocks_x = desc.Width / 4, blocks_y = desc.Height / 4;
			size_t transparent = 0;
			for (UINT by = 0; by < blocks_y; by++)
			{
				const auto* row = static_cast<const uint8_t*>(rect.pBits) + by * rect.Pitch;
				for (UINT bx = 0; bx < blocks_x; bx++)
				{
					const uint8_t* b = row + bx * 8;
					const uint16_t c0 = static_cast<uint16_t>(b[0] | b[1] << 8);
					const uint16_t c1 = static_cast<uint16_t>(b[2] | b[3] << 8);
					if (c0 > c1)
						continue;
					const uint32_t bits = static_cast<uint32_t>(b[4] | b[5] << 8 | b[6] << 16 | b[7] << 24);
					for (int i = 0; i < 16; i++)
						transparent += ((bits >> (2 * i)) & 3u) == 3u;
				}
			}
			tex->UnlockRect(0);
			return static_cast<float>(transparent) >= kMinPunchThroughShare * static_cast<float>(desc.Width * desc.Height);
		}
	}

	bool ffp_state::cur_draw_is_fur_shell(IDirect3DDevice9* dev)
	{
		if (!dev || !cur_decl_is_skinned() || !cur_decl_has_color() || !(cur_ps_sampler_mask_ & (1u << 2)))
			return false;
		// Read from the device: the game applies blend state through state blocks.
		DWORD blend = FALSE, test = FALSE, dest = D3DBLEND_ZERO;
		dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
		dev->GetRenderState(D3DRS_ALPHATESTENABLE, &test);
		dev->GetRenderState(D3DRS_DESTBLEND, &dest);
		if (!blend || !test || dest != D3DBLEND_INVSRCALPHA || ps_const_[139 * 4 + 3] < 0.999f)
			return false;
		IDirect3DBaseTexture9* diffuse = cur_texture(0);
		if (!diffuse)
			return false;
		const auto it = punch_through_.find(diffuse);
		if (it != punch_through_.end())
			return it->second;
		const bool fur = scan_punch_through(diffuse);
		log("FFP", std::format("skinned _Alp draw, diffuse {:#x}: {}", reinterpret_cast<uintptr_t>(diffuse),
			fur ? "fur shell (with its vertex colour)" : "no fur shell"));
		return punch_through_[diffuse] = fur;
	}

	void ffp_state::apply_vertex_colour_stage(IDirect3DDevice9* dev)
	{
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
		// Stage 0 now differs from setup_texture_stages; the next converted draw re-applies it.
		ffp_stages_applied_ = false;
	}

	void ffp_state::apply_texture_only_stage(IDirect3DDevice9* dev)
	{
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		// Stage 0 now differs from setup_texture_stages; the next converted draw re-applies it.
		ffp_stages_applied_ = false;
	}

	void ffp_state::on_texture_created(IDirect3DBaseTexture9* texture)
	{
		punch_through_.erase(texture);
		spec_scale_.erase(texture);
	}

	bool ffp_state::apply_skin_tone(IDirect3DDevice9* dev, bool overlay)
	{
		if (!dev || !cur_ps_skin_tone_)
			return false;
		// Two-layer shaders (sampler s3) add it to the second diffuse only.
		const bool two_layer = (cur_ps_sampler_mask_ & (1u << 3)) != 0;
		const float* c = &ps_const_[156 * 4];
		// A face's base layer (FaceGen map) takes no tone, but it is skin: 0xD6 tells Remix so. Not the eyes (FRPG_Phn_FaceEye reads c174).
		if (two_layer && !overlay && !cur_ps_is_eye_ && (c[0] != 0.0f || c[1] != 0.0f || c[2] != 0.0f))
		{
			dev->SetTextureStageState(5, D3DTSS_CONSTANT, 0xD6000000u);
			return true;
		}
		if (overlay != two_layer)
			return false;

		DWORD tag = SKIN_TONE_MARKER;
		bool any = false;
		for (int i = 0; i < 3; i++)
		{
			const long step = std::lround(std::clamp(c[i], -127.0f / 255.0f, 127.0f / 255.0f) * 255.0f);
			any |= step != 0;
			tag |= static_cast<DWORD>(static_cast<uint8_t>(static_cast<int8_t>(step))) << (16 - 8 * i);
		}
		if (!any)
			return false;
		dev->SetTextureStageState(5, D3DTSS_CONSTANT, tag);
		return true;
	}

	void ffp_state::clear_skin_tone(IDirect3DDevice9* dev)
	{
		if (dev)
			dev->SetTextureStageState(5, D3DTSS_CONSTANT, 0);
	}

	float ffp_state::sprite_fog_alpha(const float* position) const
	{
		const float w = sprite_w_row_[0] * position[0] + sprite_w_row_[1] * position[1] + sprite_w_row_[2] * position[2] + sprite_w_row_[3];
		if (!(w > 0.1f))
			return 1.0f;
		const float fog = std::clamp((w - vs_const_[2 * 4]) * vs_const_[2 * 4 + 1] * vs_const_[159 * 4], 0.0f, 1.0f);
		return std::clamp(1.0f - fog * ps_const_[4 * 4 + 3], 0.0f, 1.0f);
	}

	void ffp_state::push_soft_sprite(float* position, float feather) const
	{
		// A soft sprite sits half a feather nearer than its vertices say, so it fades from 50% at a surface
		// (alpha = saturate(gap / feather)) and shows down to half a feather behind one: the Darksign's
		// ground ring, a few centimetres off the floor. On screen it stays where it was, along its view ray.
		if (!(feather > 0.0f) || !std::isfinite(feather))
			return;
		constexpr float kNear = 0.1f;
		const double w = sprite_w_row_[0] * static_cast<double>(position[0]) + sprite_w_row_[1] * static_cast<double>(position[1]) +
			sprite_w_row_[2] * static_cast<double>(position[2]) + sprite_w_row_[3];
		if (!(w > kNear))
			return;
		const double t = std::max(w - 0.5 * feather, static_cast<double>(kNear)) / w;
		for (int c = 0; c < 3; c++)
			position[c] = static_cast<float>(sprite_cam_[c] + (position[c] - sprite_cam_[c]) * t);
	}

	void ffp_state::ensure_white_sprite_tex(IDirect3DDevice9* dev)
	{
		if (white_sprite_tex_ || FAILED(dev->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &white_sprite_tex_, nullptr)))
			return;
		D3DLOCKED_RECT lr{};
		if (SUCCEEDED(white_sprite_tex_->LockRect(0, &lr, nullptr, 0)))
		{
			for (int y = 0; y < 8; y++)
				std::fill_n(reinterpret_cast<DWORD*>(static_cast<unsigned char*>(lr.pBits) + y * lr.Pitch), 8, 0xFFFFFFFFu);
			white_sprite_tex_->UnlockRect(0);
		}
	}

	// A warm streak draw with no known hit near it: an effect missing from the hit table, whose sparks would be gone. For the log only.
	void ffp_state::note_spark_streaks(const float* position)
	{
		static constexpr UINT kRecent = 180;       // frames
		static constexpr float kReach = 8.0f;      // metres: a streak flies about 4 m at most
		static int logged = 0;
		if (logged >= 32 || shared::globals::lookdev_host)
			return;
		for (const auto& h : spark_recent_)
		{
			const float dx = h.x - position[0], dy = h.y - position[1], dz = h.z - position[2];
			if (frame_count_ - h.frame < kRecent && dx * dx + dy * dy + dz * dz < kReach * kReach)
				return;
		}
		++logged;
		log("Sparks", std::format("warm streaks with no known hit near ({:.2f}, {:.2f}, {:.2f}), frame {}", position[0], position[1], position[2], frame_count_));
	}

	// The game's hits become Remix particles here, once a frame, in the first sprite draw (the sprite's camera, target and transforms are set).
	// One small triangle per game streak, at the hit, with its colour and its speed in u; the runtime throws the sparks (particle_system_spawn).
	void ffp_state::draw_spark_emitter(IDirect3DDevice9* dev)
	{
		if ((!spark_source_fn_ && !stamp_texture_fn_) || spark_emit_frame_ == frame_count_ || cur_rt_menu_path_traced_)
			return;
		spark_emit_frame_ = frame_count_;

		// The footprint textures the snow's stamps read: one hidden triangle each, on stage 4's marker, near the camera.
		if (stamp_texture_fn_)
		{
			IDirect3DBaseTexture9* stamps[2] = {};
			stamp_texture_fn_(stamps);
			if (stamps[0] || stamps[1])
			{
				IDirect3DBaseTexture9* bound = nullptr;
				DWORD stage4 = 0;
				dev->GetTexture(0, &bound);
				dev->GetTextureStageState(4, D3DTSS_CONSTANT, &stage4);
				for (DWORD slot = 0; slot < 2; slot++)
				{
					if (!stamps[slot])
						continue;
					const float x = static_cast<float>(sprite_cam_[0]), y = static_cast<float>(sprite_cam_[1]), z = static_cast<float>(sprite_cam_[2]);
					const spark_vertex v[3] = { { x - 0.005f, y, z - 0.005f, 0xFFFFFFFFu, 0.0f, 0.0f }, { x + 0.005f, y, z - 0.005f, 0xFFFFFFFFu, 1.0f, 0.0f }, { x, y + 0.005f, z + 0.005f, 0xFFFFFFFFu, 0.0f, 1.0f } };
					dev->SetTexture(0, stamps[slot]);
					dev->SetTextureStageState(4, D3DTSS_CONSTANT, 0xD400000Au + slot);
					dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(spark_vertex));
				}
				dev->SetTexture(0, bound);
				if (bound)
					bound->Release();
				dev->SetTextureStageState(4, D3DTSS_CONSTANT, stage4);
			}
		}

		spark_emissions_.clear();
		if (spark_source_fn_)
			spark_source_fn_(spark_emissions_);
		if (spark_emissions_.empty())
			return;

		// The shader's own scale: FRPG_Sfx_LineType0 is 0.5, as sfx_shaders.inc has it, by the game's exposure (it reads c135).
		const float exposure = ps_const_[135 * 4 + 1] > 0.0f && ps_const_[135 * 4 + 0] > 0.0f ? ps_const_[135 * 4 + 0] / ps_const_[135 * 4 + 1] : 1.0f;
		const float colour_scale = 0.5f * exposure;
		auto channel = [&](float v) { return static_cast<DWORD>(std::clamp(v * 255.0f * colour_scale + 0.5f, 0.0f, 255.0f)); };

		static constexpr UINT kMostTriangles = 2048;
		static constexpr float kHalf = 0.005f;      // metres
		spark_vertices_.clear();
		for (const auto& e : spark_emissions_)
		{
			const DWORD alpha = static_cast<DWORD>(255.0f * sprite_fog_alpha(e.pos) + 0.5f);
			const DWORD colour = (alpha << 24) | (channel(e.rgb[0]) << 16) | (channel(e.rgb[1]) << 8) | channel(e.rgb[2]);
			for (UINT s = 0; s < e.streaks && spark_vertices_.size() < kMostTriangles * 3; s++)
			{
				spark_vertices_.push_back({ e.pos[0] - kHalf, e.pos[1], e.pos[2] - kHalf, colour, e.speed, 0.0f });
				spark_vertices_.push_back({ e.pos[0] + kHalf, e.pos[1], e.pos[2] - kHalf, colour, e.speed, 0.0f });
				spark_vertices_.push_back({ e.pos[0], e.pos[1] + kHalf, e.pos[2] + kHalf, colour, e.speed, 0.0f });
			}
			spark_recent_[spark_recent_next_++ % spark_recent_.size()] = { e.pos[0], e.pos[1], e.pos[2], frame_count_ };
		}

		ensure_white_sprite_tex(dev);
		// The particles take this draw's material: additive, self-lit, as the game's streaks are. Every state that goes into it is set, so all hits share one
		// particle system whatever sprite happens to be drawing.
		static constexpr D3DRENDERSTATETYPE kStates[] = { D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC };
		static constexpr DWORD kValues[] = { TRUE, D3DBLEND_SRCALPHA, D3DBLEND_ONE, TRUE, 1, D3DCMP_GREATEREQUAL };
		DWORD states[std::size(kStates)] = {};
		for (size_t i = 0; i < std::size(kStates); i++)
		{
			dev->GetRenderState(kStates[i], &states[i]);
			dev->SetRenderState(kStates[i], kValues[i]);
		}
		DWORD stage0 = 0, stage2 = 0, stage4 = 0;
		dev->GetTextureStageState(0, D3DTSS_CONSTANT, &stage0);
		dev->GetTextureStageState(2, D3DTSS_CONSTANT, &stage2);
		dev->GetTextureStageState(4, D3DTSS_CONSTANT, &stage4);
		IDirect3DBaseTexture9* texture = nullptr;
		dev->GetTexture(0, &texture);

		constexpr DWORD kUnlitMarker = 0xD15D0E1D;
		constexpr DWORD kSparksMarker = 0xD4000008;
		dev->SetTexture(0, white_sprite_tex_);
		dev->SetTextureStageState(0, D3DTSS_CONSTANT, kUnlitMarker);
		dev->SetTextureStageState(2, D3DTSS_CONSTANT, 0);
		dev->SetTextureStageState(4, D3DTSS_CONSTANT, kSparksMarker);
		dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(spark_vertices_.size() / 3), spark_vertices_.data(), sizeof(spark_vertex));

		dev->SetTexture(0, texture);
		if (texture)
			texture->Release();
		dev->SetTextureStageState(0, D3DTSS_CONSTANT, stage0);
		dev->SetTextureStageState(2, D3DTSS_CONSTANT, stage2);
		dev->SetTextureStageState(4, D3DTSS_CONSTANT, stage4);
		for (size_t i = 0; i < std::size(kStates); i++)
			dev->SetRenderState(kStates[i], states[i]);
	}

	bool ffp_state::draw_sprite_up(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, UINT prim_count,
		UINT min_index, UINT num_verts, const void* indices, D3DFORMAT index_format,
		const void* vertices, UINT stride, bool unlit, HRESULT& hr)
	{
		auto reject = [](int reason, const char* why)
		{
			static uint32_t logged = 0;
			if (!(logged & (1u << reason)))
			{
				logged |= 1u << reason;
				log("Sfx", std::format("sprite not drawn fixed-function: {}", why));
			}
			return false;
		};
		// Point sprites (FRPG_Sfx_PointSpriteType0) are expanded to quads below. Its view-projection is c8-c11;
		// the dp4s the base was parsed from are its view, c12-c15, which it only needs for the point size.
		const bool points = prim == D3DPT_POINTLIST;
		const int base = points ? 8 : cur_vs_viewproj_base_;
		if (!dev || !vertices || !stride || !cfg_)
			return reject(0, "no vertices");
		if (!enabled_)
			return reject(1, "FFP conversion not enabled yet");
		if (base < 0 || base > 252)
			return reject(2, "vertex shader has no known view-projection");
		if (!have_map_viewproj_ || !view_proj_valid_)
			return reject(3, "no map camera yet");
		// Fixed-function lines do not reach the path tracer as surfaces.
		if (prim != D3DPT_TRIANGLELIST && prim != D3DPT_TRIANGLESTRIP && prim != D3DPT_TRIANGLEFAN && !points)
			return reject(4, "lines");
		if (cur_decl_pos_type_ != D3DDECLTYPE_FLOAT3 && cur_decl_pos_type_ != D3DDECLTYPE_FLOAT4)
			return reject(5, "position not float3/float4");
		// The point's size is in its position's w.
		if (points && (indices || cur_decl_pos_type_ != D3DDECLTYPE_FLOAT4))
			return reject(6, "points without a size in position w");

		// The particle's rows must be the map camera's but for their translation
		// column (x, y and w rows; some sprite shaders compute z themselves).
		const float* P = &vs_const_[base * 4];
		const float* M = map_viewproj_rows_;
		constexpr int rows[3] = { 0, 1, 3 };
		float scale = 0.0f, diff = 0.0f;
		for (int r : rows)
			for (int c = 0; c < 3; c++)
			{
				scale = std::max(scale, std::abs(M[r * 4 + c]));
				diff = std::max(diff, std::abs(M[r * 4 + c] - P[r * 4 + c]));
			}
		if (scale <= 0.0f || diff > 1e-3f * scale)
		{
			static int mismatch_log_budget = 4;
			if (mismatch_log_budget > 0)
			{
				mismatch_log_budget--;
				log("Sfx", std::format("sprite camera differs from the map camera (max diff {:.4f} of {:.4f}), vp base c{}", diff, scale, base));
			}
			return false;
		}

		// P.xyz = M.xyz and P.w = M.w - M.xyz . cam, so M.xyz . cam = M.w - P.w on each row.
		double a[3][3], b[3];
		for (int k = 0; k < 3; k++)
		{
			for (int c = 0; c < 3; c++)
				a[k][c] = M[rows[k] * 4 + c];
			b[k] = static_cast<double>(M[rows[k] * 4 + 3]) - P[rows[k] * 4 + 3];
		}
		const double det =
			a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
			a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
			a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
		if (std::abs(det) < 1e-12)
			return false;
		double cam[3];
		for (int c = 0; c < 3; c++)
		{
			double m[3][3];
			std::memcpy(m, a, sizeof(m));
			for (int k = 0; k < 3; k++)
				m[k][c] = b[k];
			cam[c] = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
				m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
				m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])) / det;
		}

		// Into the space map pieces get: camera-relative, plus the injected camera
		// position when world offsetting is on (see apply_transforms).
		std::memcpy(sprite_cam_, cam, sizeof(sprite_cam_));
		std::memcpy(sprite_w_row_, &P[3 * 4], sizeof(sprite_w_row_));

		float world[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
		world[12] = static_cast<float>(-cam[0]);
		world[13] = static_cast<float>(-cam[1]);
		world[14] = static_cast<float>(-cam[2]);
		float ox = 0.0f, oy = 0.0f, oz = 0.0f;
		const bool offset = offset_world_ && (menu_cell_offset(ox, oy, oz) ||
			(camera_override_fn_ && !cur_rt_menu_path_traced_ && camera_override_fn_(ox, oy, oz)));
		if (offset)
		{
			world[12] += ox;
			world[13] += oy;
			world[14] += oz;
		}
		static int placement_log_budget = 4;
		if (placement_log_budget > 0)
		{
			placement_log_budget--;
			log("Sfx", std::format("sprite placement: vp base c{}, particle camera ({:.2f}, {:.2f}, {:.2f}), world offset {} ({:.2f}, {:.2f}, {:.2f}) -> translation ({:.2f}, {:.2f}, {:.2f})",
				base, cam[0], cam[1], cam[2], offset ? "on" : "off", ox, oy, oz, world[12], world[13], world[14]));
		}

		const DWORD batch_verdict = sprite_batch_fn_ && unlit && indices && !points && cur_decl_has_color_
			? sprite_batch_fn_(cur_texture_[0], vertices, stride, cur_decl_pos_off_, cur_decl_color_off_, min_index + num_verts) : 0;
		if (batch_verdict == 1)
		{
			hr = S_OK;
			return true;
		}
		const DWORD batch_marker = batch_verdict > 1 ? batch_verdict : 0;

		// Repack: position, colour, uv.
		UINT count = points ? prim_count * 6
			: indices ? min_index + num_verts
			: (prim == D3DPT_TRIANGLELIST ? prim_count * 3 : prim_count + 2);
		struct sprite_vertex { float x, y, z; DWORD color; float u, v; };
		sprite_vertices_.resize(static_cast<size_t>(count) * sizeof(sprite_vertex));
		auto* out = reinterpret_cast<sprite_vertex*>(sprite_vertices_.data());
		const auto* src = static_cast<const unsigned char*>(vertices);
		const bool uv_float = cur_decl_has_texcoord_ &&
			(cur_decl_texcoord_type_ == D3DDECLTYPE_FLOAT2 || cur_decl_texcoord_type_ == D3DDECLTYPE_FLOAT4);

		// The Multi sprite shaders multiply the diffuse by masks on s1 (TEXCOORD0.zw) and s3 (TEXCOORD1)
		// with their own scrolling uvs. Remix takes one texture, so those draws are subdivided: the most detailed
		// texture goes to Remix, the others are sampled per vertex (multi_sprite_triangles below). The masks'
		// average stands in where that cannot be done, and for the sprite observer.
		std::array<float, 4> mask = { 1.0f, 1.0f, 1.0f, 1.0f };
		if (unlit && !points)
			for (DWORD s : { 1u, 3u })
				if ((cur_ps_sampler_mask_ & (1u << s)) && cur_texture_[s])
				{
					const auto avg = texture_average(cur_texture_[s]);
					for (int c = 0; c < 4; c++)
						mask[c] *= avg[c];
				}
		const bool masked = mask[0] != 1.0f || mask[1] != 1.0f || mask[2] != 1.0f || mask[3] != 1.0f;

		// A soft sprite's alpha fades by the scene depth: x saturate(0.5 + (scene depth - sprite depth) / feather),
		// the feather a per-vertex distance (FRPG_Sfx_SimpleSprite_Base.fxh ZFeather). Remix fades a particle
		// per surface, so the draw's mean feather goes to the runtime (kFeatherMarker below).
		const bool soft = unlit && cur_ps_sfx_class_ == sfx_class::SoftSprite && cur_decl_feather_off_ >= 0;
		std::vector<float>& feathers = sprite_feathers_;
		feathers.clear();

		// The sprite shaders' colour is tex * vcolor times the scale of their fog step (CalcGetFogCol's
		// 0.5, from sfx_shaders.inc) and the game's exposure: c135.x / c135.y in a pixel shader that
		// reads c135, once more c159.z / c159.w in a vertex shader that scales by it. That is the
		// sprite's display value, which the runtime shows as it is (kUnlitMarker below), whatever
		// exposure Remix runs at. Their fog and scattering are not folded in: the DS1 look's scene pass
		// already hazes the composite, sprites included (with the haze of the surface behind them,
		// not the sprite's own: still open).
		auto ratio = [](float num, float den) { return den > 0.0f && num > 0.0f ? num / den : 1.0f; };
		const float colour_scale = cur_ps_sfx_colour_scale_ *
			(cur_ps_reads_exposure_ ? ratio(ps_const_[135 * 4 + 0], ps_const_[135 * 4 + 1]) : 1.0f) *
			(cur_vs_exposure_ ? ratio(vs_const_[159 * 4 + 2], vs_const_[159 * 4 + 3]) : 1.0f);
		const bool game_colour = unlit && colour_scale != 1.0f;
		auto vertex_colour = [&](DWORD colour)
		{
			// D3DCOLOR is ARGB: alpha in 24-31, red 16-23, green 8-15, blue 0-7.
			auto channel = [&](int shift)
			{
				return static_cast<DWORD>(std::min(((colour >> shift) & 0xFF) * colour_scale + 0.5f, 255.0f)) << shift;
			};
			return (colour & 0xFF000000u) | channel(16) | channel(8) | channel(0);
		};

		// A point becomes a quad facing the camera, with the size the shader gives it: its diameter in pixels is
		// screen height * w / |view position| (with the view position's w of 1, so at the camera's depth z a
		// world diameter of 2 * w * z / (|view position| * P11)), and P11 is the length of the y row's xyz.
		if (points)
		{
			auto row_length = [&](int r) { return std::sqrt(P[r * 4] * P[r * 4] + P[r * 4 + 1] * P[r * 4 + 1] + P[r * 4 + 2] * P[r * 4 + 2]); };
			const float p00 = row_length(0), p11 = row_length(1);
			if (!(p00 > 0.0f) || !(p11 > 0.0f))
				return false;
			UINT written = 0;
			for (UINT p = 0; p < prim_count; p++)
			{
				const unsigned char* sv = src + static_cast<size_t>(p) * stride;
				const auto* pos = reinterpret_cast<const float*>(sv + cur_decl_pos_off_);
				const double dx = pos[0] - cam[0], dy = pos[1] - cam[1], dz = pos[2] - cam[2];
				const double z = P[12] * static_cast<double>(pos[0]) + P[13] * static_cast<double>(pos[1]) +
					P[14] * static_cast<double>(pos[2]) + P[15];
				const float half = static_cast<float>(pos[3] * z / (std::sqrt(dx * dx + dy * dy + dz * dz + 1.0) * p11));
				if (!(half > 0.0f) || !std::isfinite(half))
					continue;
				DWORD colour = cur_decl_has_color_ ? pack_decl_color(sv, cur_decl_color_off_, cur_decl_color_type_) : 0xFFFFFFFFu;
				if (game_colour)
					colour = vertex_colour(colour);
				if (const float fade = sprite_fog_alpha(pos); fade < 1.0f)
					colour = (colour & 0x00FFFFFFu) | (static_cast<DWORD>((colour >> 24) * fade + 0.5f) << 24);
				auto corner = [&](float sx, float sy, float u, float v)
				{
					auto& o = out[written++];
					for (int c = 0; c < 3; c++)
						(&o.x)[c] = pos[c] + half * (sx * P[c] / p00 + sy * P[4 + c] / p11);
					o.color = colour;
					o.u = u; o.v = v;
				};
				corner(-1.0f, 1.0f, 0.0f, 0.0f); corner(1.0f, 1.0f, 1.0f, 0.0f); corner(-1.0f, -1.0f, 0.0f, 1.0f);
				corner(-1.0f, -1.0f, 0.0f, 1.0f); corner(1.0f, 1.0f, 1.0f, 0.0f); corner(1.0f, -1.0f, 1.0f, 1.0f);
			}
			if (!written)
			{
				hr = S_OK;
				return true;
			}
			prim = D3DPT_TRIANGLELIST;
			prim_count = written / 3;
		}
		else
		for (UINT v = 0; v < count; v++)
		{
			const unsigned char* sv = src + static_cast<size_t>(v) * stride;
			const auto* pos = reinterpret_cast<const float*>(sv + cur_decl_pos_off_);
			out[v].x = pos[0]; out[v].y = pos[1]; out[v].z = pos[2];
			if (soft)
				push_soft_sprite(&out[v].x, *reinterpret_cast<const float*>(sv + cur_decl_feather_off_));
			out[v].color = cur_decl_has_color_ ? pack_decl_color(sv, cur_decl_color_off_, cur_decl_color_type_) : 0xFFFFFFFFu;
			if (masked)
			{
				const DWORD c = out[v].color;
				auto channel = [&](int shift, float m) { return static_cast<DWORD>(((c >> shift) & 0xFF) * m + 0.5f) << shift; };
				out[v].color = channel(24, mask[3]) | channel(16, mask[0]) | channel(8, mask[1]) | channel(0, mask[2]);
			}
			if (game_colour)
				out[v].color = vertex_colour(out[v].color);
			if (uv_float)
			{
				const auto* tc = reinterpret_cast<const float*>(sv + cur_decl_texcoord_off_);
				out[v].u = tc[0]; out[v].v = tc[1];
			}
			else
			{
				out[v].u = 0.0f; out[v].v = 0.0f;
			}
			if (soft)
			{
				const float feather = *reinterpret_cast<const float*>(sv + cur_decl_feather_off_);
				if (feather > 0.0f && std::isfinite(feather))
				{
					feathers.push_back(feather);
				}
			}
		}
		// Point sprites are sparkles, nothing the effect lights follow; nor are streaks whose pixel shader samples no texture
		// (FRPG_Sfx_LineType0: a weapon's sparks off stone, orange and additive like fire, which the game lights with nothing).
		if (sprite_observer_fn_ && count && !points && (cur_ps_sampler_mask_ & 1u))
		{
			DWORD blend_on = FALSE, dst_blend = 0;
			dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend_on);
			dev->GetRenderState(D3DRS_DESTBLEND, &dst_blend);
			const bool additive = blend_on && dst_blend == D3DBLEND_ONE;
			const auto tex = additive && cur_texture_[0] ? texture_average(cur_texture_[0]) : std::array<float, 4> { 1.0f, 1.0f, 1.0f, 1.0f };
			// One point per drawn triangle, through the draw's own indices (list, strip or fan).
			auto vertex_of = [&](UINT i) -> UINT
			{
				if (!indices) return i;
				return index_format == D3DFMT_INDEX32 ? static_cast<const uint32_t*>(indices)[i]
					: static_cast<const uint16_t*>(indices)[i];
			};
			sprite_points_.clear();
			for (UINT t = 0; t < prim_count; t++)
			{
				UINT v[3];
				if (prim == D3DPT_TRIANGLELIST) { v[0] = vertex_of(3 * t); v[1] = vertex_of(3 * t + 1); v[2] = vertex_of(3 * t + 2); }
				else if (prim == D3DPT_TRIANGLESTRIP) { v[0] = vertex_of(t); v[1] = vertex_of(t + 1); v[2] = vertex_of(t + 2); }
				else { v[0] = vertex_of(0); v[1] = vertex_of(t + 1); v[2] = vertex_of(t + 2); }
				if (v[0] >= count || v[1] >= count || v[2] >= count)
					continue;
				// Where the particle is, not its position pushed towards the camera for the soft fade: up to 1.45 m along the view, which moved the
				// lights with the camera and could lose the match with their effect.
				float centre[3] = {};
				for (const UINT vi : v)
				{
					const auto* at = reinterpret_cast<const float*>(src + static_cast<size_t>(vi) * stride + cur_decl_pos_off_);
					for (int k = 0; k < 3; k++)
						centre[k] += at[k] / 3.0f;
				}
				sprite_point p { centre[0], centre[1], centre[2],
					static_cast<float>(std::max({ out[v[0]].color >> 24, out[v[1]].color >> 24, out[v[2]].color >> 24 })) / 255.0f };
				float rgb[3] = {};
				for (const UINT vi : v)
				{
					const DWORD c = out[vi].color;
					const float va = static_cast<float>(c >> 24) / 255.0f;
					rgb[0] += static_cast<float>((c >> 16) & 0xFF) / 255.0f * va;
					rgb[1] += static_cast<float>((c >> 8) & 0xFF) / 255.0f * va;
					rgb[2] += static_cast<float>(c & 0xFF) / 255.0f * va;
				}
				p.r = rgb[0] / 3.0f * tex[0] * tex[3];
				p.g = rgb[1] / 3.0f * tex[1] * tex[3];
				p.b = rgb[2] / 3.0f * tex[2] * tex[3];
				const float e1[3] = { out[v[1]].x - out[v[0]].x, out[v[1]].y - out[v[0]].y, out[v[1]].z - out[v[0]].z };
				const float e2[3] = { out[v[2]].x - out[v[0]].x, out[v[2]].y - out[v[0]].y, out[v[2]].z - out[v[0]].z };
				const float cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
				p.area = 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
				p.additive = additive;
				if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && std::isfinite(p.area))
					sprite_points_.push_back(p);
			}
			if (!sprite_points_.empty())
				sprite_observer_fn_(sprite_points_.data(), static_cast<UINT>(sprite_points_.size()));
		}
		// Fog takes from what shows, after the lights above have read the particle: not a fire's light with the camera's distance.
		if (!points)
			for (UINT v = 0; v < count; v++)
			{
				const auto* pos = reinterpret_cast<const float*>(src + static_cast<size_t>(v) * stride + cur_decl_pos_off_);
				if (const float fade = sprite_fog_alpha(pos); fade < 1.0f)
					out[v].color = (out[v].color & 0x00FFFFFFu) | (static_cast<DWORD>((out[v].color >> 24) * fade + 0.5f) << 24);
			}

		// Two effects Remix particles replace (runtime rtx_dark_souls_effect_particles.h), tagged on stage 4 like the fog gates' batches:
		// the green motes over a bloodstain (s01003), and the streaks of a weapon striking stone (FRPG_Sfx_LineType0: additive, its pixel
		// shader samples nothing). Both by the game's own vertex colour too (before the exposure scale above, which moves with the eye):
		// the bloodstain's motes are (130, 255, 180), b/g 0.71, where the Moonlight Butterfly's magic on the same texture is
		// (150, 255, 200), b/g 0.78; a hit's streaks are warm: orange (255, 160, 50), paler (255, 200, 100) or rust (255, 105, 55).
		// The streaks only hide, every one of them. Sparks come from the hits themselves (draw_spark_emitter).
		constexpr DWORD kMotesMarker = 0xD4000007;
		constexpr DWORD kSparksHideMarker = 0xD4000009;
		DWORD effect_marker = 0;
		if (!batch_marker && unlit && !points && !masked && count >= 3 && cur_decl_has_color_)
		{
			DWORD blend_on = FALSE, dst_blend = 0;
			dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend_on);
			dev->GetRenderState(D3DRS_DESTBLEND, &dst_blend);
			if (blend_on && dst_blend == D3DBLEND_ONE)
			{
				double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0;
				for (UINT v = 0; v < count; v++)
				{
					const DWORD raw = pack_decl_color(src + static_cast<size_t>(v) * stride, cur_decl_color_off_, cur_decl_color_type_);
					sum_r += (raw >> 16) & 0xFF; sum_g += (raw >> 8) & 0xFF; sum_b += raw & 0xFF;
				}
				const bool stain_green = sum_r < sum_g * 0.7 && sum_b < sum_g * 0.745;
				const bool spark_warm = sum_g < sum_r * 0.85 && sum_b < sum_r * 0.45;
				if (stain_green && mote_texture_fn_ && (cur_ps_sampler_mask_ & 1u) && mote_texture_fn_(cur_texture_[0]))
					effect_marker = kMotesMarker;
				else if (spark_warm && !(cur_ps_sampler_mask_ & 1u) && count % 4 == 0)
				{
					effect_marker = kSparksHideMarker;
					note_spark_streaks(reinterpret_cast<const float*>(src + cur_decl_pos_off_));
				}
			}
		}

		IDirect3DVertexDeclaration9* game_decl = nullptr;
		dev->GetVertexDeclaration(&game_decl);

		with_cached_transforms([&] { engage(dev); });
		// Like converted geometry: the sprite texture on stage 0, nothing else bound
		// (the sprite shaders also sample the scene depth copy and leftovers, which
		// Remix would read as the draw's textures).
		// FRPG_Sfx_LineType0 samples nothing: what is on stage 0 is left from an earlier draw, and at its uv of
		// (0, 0) usually black, which took the sparks' streaks away. A white texture stands in: Remix reads the
		// draw's markers only from a draw with a texture.
		if (!(cur_ps_sampler_mask_ & 1u))
			ensure_white_sprite_tex(dev);
		dev->SetTexture(0, (cur_ps_sampler_mask_ & 1u) ? cur_texture_[0] : white_sprite_tex_);
		for (DWORD ts = 1; ts < 8; ts++)
			dev->SetTexture(ts, nullptr);
		dev->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(world));
		dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
		DWORD blend = 0, atest = 0;
		const bool debug_opaque = config::get().sfx.debug_opaque;
		if (debug_opaque)
		{
			dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
			dev->GetRenderState(D3DRS_ALPHATESTENABLE, &atest);
			dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
			dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		}
		// Unlit marker (runtime: rtx.fixedFunctionUnlitMarkerEnabled): the sprite's colour is its display
		// value (the game's exposure is in it, vertex_colour above). Stage 0's constant is inert in
		// rasterization: no stage argument references D3DTA_CONSTANT.
		// Unfogged sprites have their own: in the game such a sprite (fog walls, s00903) has its colour and nothing of the haze, which the
		// look's scene pass would take from the far surface behind it (dark teal instead of the game's grey).
		constexpr DWORD kUnlitMarker = 0xD15D0E1D;
		constexpr DWORD kUnhazedUnlitMarker = 0xD15D0E23;
		// An alpha sprite the game's own constants leave unfogged: VS c159.y is 0 (no colour fog, extinction or inscatter at all), or VS c135.w (its
		// extinction and inscatter share) and PS c4.w (its fog colour) are 0.
		DWORD sprite_dest = 0;
		dev->GetRenderState(D3DRS_DESTBLEND, &sprite_dest);
		const bool unfogged = !masked && sprite_dest == D3DBLEND_INVSRCALPHA && (vs_const_[159 * 4 + 1] == 0.0f || (vs_const_[135 * 4 + 3] == 0.0f && ps_const_[4 * 4 + 3] == 0.0f));
		if (unlit)
			dev->SetTextureStageState(0, D3DTSS_CONSTANT, unfogged ? kUnhazedUnlitMarker : kUnlitMarker);
		if (batch_marker)
			dev->SetTextureStageState(4, D3DTSS_CONSTANT, batch_marker);
		if (effect_marker)
			dev->SetTextureStageState(4, D3DTSS_CONSTANT, effect_marker);
		// Soft sprite feather in stage 2's constant (0xD8000000 | millimetres, Surface::ds1ParticleFeather):
		// stage 2 otherwise carries the map meshes' view fade, never a sprite's, so its value is put back after.
		// A sprite whose shader never reads the scene depth (SimpleSpriteType0: a message's runes, laid on the
		// floor) has hard edges in the game: 0 millimetres, so Remix's own soft fade leaves it alone too.
		constexpr DWORD kFeatherMarker = 0xD8000000u;
		DWORD stage2_constant = 0;
		// The median: a draw's particles each have their own feather, often several times apart.
		const bool feathered = !feathers.empty() || ((cur_ps_sfx_class_ == sfx_class::Sprite || cur_ps_sfx_class_ == sfx_class::LitSprite) && !cur_ps_sfx_reads_depth_);
		if (feathered)
		{
			dev->GetTextureStageState(2, D3DTSS_CONSTANT, &stage2_constant);
			DWORD mm = 0;
			if (!feathers.empty())
			{
				std::nth_element(feathers.begin(), feathers.begin() + feathers.size() / 2, feathers.end());
				const double m = std::min(static_cast<double>(feathers[feathers.size() / 2]) * 1000.0, static_cast<double>(0xFFFFFF));
				mm = std::max(static_cast<DWORD>(std::lround(m)), DWORD { 1 });
			}
			dev->SetTextureStageState(2, D3DTSS_CONSTANT, kFeatherMarker | mm);
		}
		// Some particle layers (the Crystal Cave fog) are drawn into a reduced-resolution
		// buffer that a filter pass later upsamples onto the frame. Remix ray-traces only
		// draws aimed at a back-buffer-sized target, and the upsample is a shader draw it
		// ignores, so those sprites are aimed at the back buffer (without the small
		// buffer's depth) for the draw.
		IDirect3DSurface9* game_rt = nullptr;
		IDirect3DSurface9* game_ds = nullptr;
		IDirect3DSurface9* back_buffer = nullptr;
		D3DVIEWPORT9 game_viewport {};
		bool retarget = false;
		if (SUCCEEDED(dev->GetRenderTarget(0, &game_rt)) && game_rt &&
			SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer)) && back_buffer)
		{
			D3DSURFACE_DESC rt_desc {}, bb_desc {};
			game_rt->GetDesc(&rt_desc);
			back_buffer->GetDesc(&bb_desc);
			if (rt_desc.Width < bb_desc.Width || rt_desc.Height < bb_desc.Height)
			{
				dev->GetViewport(&game_viewport);
				dev->GetDepthStencilSurface(&game_ds);
				dev->SetRenderTarget(0, back_buffer);
				dev->SetDepthStencilSurface(nullptr);
				retarget = true;
			}
		}
		// Points are never culled, whatever the cull state the game left.
		DWORD game_cull = D3DCULL_NONE;
		if (points)
		{
			dev->GetRenderState(D3DRS_CULLMODE, &game_cull);
			dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		}
		draw_spark_emitter(dev);
		DWORD carrier = 0;
		const UINT masked_triangles = masked ? multi_sprite_triangles(prim, prim_count, indices, index_format, vertices, stride,
			count, colour_scale, carrier) : 0;
		// A Multi sprite whose detail is on a mask: that mask goes to Remix on stage 0, with its sampler state.
		constexpr D3DSAMPLERSTATETYPE kCarrierStates[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER,
			D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER };
		DWORD stage0_states[std::size(kCarrierStates)] = {};
		const bool carried = masked_triangles && carrier != 0;
		if (carried)
		{
			for (size_t i = 0; i < std::size(kCarrierStates); i++)
			{
				DWORD state = 0;
				dev->GetSamplerState(0, kCarrierStates[i], &stage0_states[i]);
				dev->GetSamplerState(carrier, kCarrierStates[i], &state);
				dev->SetSamplerState(0, kCarrierStates[i], state);
			}
			dev->SetTexture(0, cur_texture_[carrier]);
		}
		hr = masked_triangles
			? dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, masked_triangles, masked_sprite_vertices_.data(), sizeof(sprite_vertex))
			: indices
			? dev->DrawIndexedPrimitiveUP(prim, min_index, num_verts, prim_count, indices, index_format, out, sizeof(sprite_vertex))
			: dev->DrawPrimitiveUP(prim, prim_count, out, sizeof(sprite_vertex));
		if (points)
			dev->SetRenderState(D3DRS_CULLMODE, game_cull);
		if (carried)
		{
			for (size_t i = 0; i < std::size(kCarrierStates); i++)
				dev->SetSamplerState(0, kCarrierStates[i], stage0_states[i]);
			dev->SetTexture(0, cur_texture_[0]);
		}
		if (retarget)
		{
			dev->SetRenderTarget(0, game_rt);
			dev->SetDepthStencilSurface(game_ds);
			dev->SetViewport(&game_viewport);
		}
		if (game_ds) game_ds->Release();
		if (back_buffer) back_buffer->Release();
		if (game_rt) game_rt->Release();
		if (unlit)
			dev->SetTextureStageState(0, D3DTSS_CONSTANT, 0);
		if (batch_marker || effect_marker)
			dev->SetTextureStageState(4, D3DTSS_CONSTANT, 0);
		if (feathered)
			dev->SetTextureStageState(2, D3DTSS_CONSTANT, stage2_constant);
		if (debug_opaque)
		{
			dev->SetRenderState(D3DRS_ALPHABLENDENABLE, blend);
			dev->SetRenderState(D3DRS_ALPHATESTENABLE, atest);
		}

		dev->SetVertexDeclaration(game_decl);
		if (game_decl) game_decl->Release();
		for (DWORD ts = 0; ts < 8; ts++)
			dev->SetTexture(ts, cur_texture_[ts]);
		if (transforms_cached_)
			dev->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(cached_world_));
		return true;
	}

	// Up to 64 of the bound draw's triangles in object space, read once per draw (buffers and range) and cached: the game's own
	// buffers, never write-only video memory. Null when they cannot be read.
	const ffp_state::effect_mesh_entry* ffp_state::sampled_triangles(D3DPRIMITIVETYPE prim, INT base_vertex, UINT start_index, UINT prim_count)
	{
		IDirect3DVertexBuffer9* vb = stream_vb_[0];
		IDirect3DIndexBuffer9* ib = cur_ib_;
		const UINT stride = stream_stride_[0], vb_offset = stream_offset_[0];
		if (!vb || !ib || !stride)
			return nullptr;
		uint64_t key = reinterpret_cast<uintptr_t>(vb);
		auto mix = [&key](uint64_t v) { key = (key ^ v) * 0x100000001B3ull; };
		mix(reinterpret_cast<uintptr_t>(ib));
		mix(static_cast<uint32_t>(base_vertex));
		mix(start_index);
		mix(prim_count);
		mix(static_cast<uint64_t>(prim));
		auto found = effect_meshes_.find(key);
		if (found == effect_meshes_.end())
		{
			if (effect_meshes_.size() > 4096)
				effect_meshes_.clear();
			effect_mesh_entry entry;
			// The game's own buffers: read once per draw. Write-only video memory reads back nothing useful (none sampled).
			D3DINDEXBUFFER_DESC idesc {};
			D3DVERTEXBUFFER_DESC vdesc {};
			int pos_off = -1, bone_off = -1, colour_off = -1, colour_type = 0;
			if (last_decl_)
			{
				D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH];
				UINT n = 0;
				if (SUCCEEDED(last_decl_->GetDeclaration(elements, &n)))
					for (UINT i = 0; i < n && elements[i].Stream != 0xFF; i++)
					{
						if (elements[i].Stream != 0 || elements[i].UsageIndex != 0)
							continue;
						if (elements[i].Usage == D3DDECLUSAGE_POSITION && elements[i].Type == D3DDECLTYPE_FLOAT3)
							pos_off = elements[i].Offset;
						else if (elements[i].Usage == D3DDECLUSAGE_BLENDINDICES)
							bone_off = elements[i].Offset;
						else if (elements[i].Usage == D3DDECLUSAGE_COLOR)
						{
							colour_off = elements[i].Offset;
							colour_type = elements[i].Type;
						}
					}
			}
			const bool readable = pos_off >= 0 && SUCCEEDED(ib->GetDesc(&idesc)) && SUCCEEDED(vb->GetDesc(&vdesc)) &&
				!(idesc.Pool == D3DPOOL_DEFAULT && (idesc.Usage & D3DUSAGE_WRITEONLY)) &&
				!(vdesc.Pool == D3DPOOL_DEFAULT && (vdesc.Usage & D3DUSAGE_WRITEONLY));
			const UINT isz = idesc.Format == D3DFMT_INDEX32 ? 4 : 2;
			const UINT indices = prim == D3DPT_TRIANGLESTRIP ? prim_count + 2 : prim_count * 3;
			void* idata = nullptr;
			void* vdata = nullptr;
			if (readable && static_cast<uint64_t>(start_index + indices) * isz <= idesc.Size &&
				SUCCEEDED(ib->Lock(start_index * isz, indices * isz, &idata, D3DLOCK_READONLY)) && idata)
			{
				if (SUCCEEDED(vb->Lock(0, 0, &vdata, D3DLOCK_READONLY)) && vdata)
				{
					constexpr UINT kMaxTriangles = 64;
					const UINT step = std::max(1u, (prim_count + kMaxTriangles - 1) / kMaxTriangles);
					const auto* bytes = static_cast<const unsigned char*>(vdata);
					auto index_at = [&](UINT i) -> int64_t
					{
						return static_cast<int64_t>(isz == 4 ? static_cast<const uint32_t*>(idata)[i] : static_cast<const uint16_t*>(idata)[i]) + base_vertex;
					};
					UINT taken = 0;
					for (UINT t = 0; t < prim_count; t += step)
					{
						int64_t v[3];
						if (prim == D3DPT_TRIANGLELIST) { v[0] = index_at(3 * t); v[1] = index_at(3 * t + 1); v[2] = index_at(3 * t + 2); }
						else { v[0] = index_at(t); v[1] = index_at(t + 1); v[2] = index_at(t + 2); }
						effect_mesh_triangle tri {};
						bool ok = true;
						float rgba[4] = {};
						for (int k = 0; k < 3 && ok; k++)
						{
							const int64_t at = static_cast<int64_t>(vb_offset) + v[k] * stride;
							if (v[k] < 0 || at + static_cast<int64_t>(stride) > static_cast<int64_t>(vdesc.Size))
							{
								ok = false;
								break;
							}
							std::memcpy(tri.p[k], bytes + at + pos_off, sizeof(tri.p[k]));
							tri.bone[k] = bone_off >= 0 ? bytes[at + bone_off] : 0;
							const DWORD c = colour_off >= 0 ? pack_decl_color(bytes + at, colour_off, colour_type) : 0xFFFFFFFFu;
							rgba[0] += ((c >> 16) & 0xFF) / 255.0f / 3.0f;
							rgba[1] += ((c >> 8) & 0xFF) / 255.0f / 3.0f;
							rgba[2] += (c & 0xFF) / 255.0f / 3.0f;
							rgba[3] += (c >> 24) / 255.0f / 3.0f;
						}
						if (!ok)
							continue;
						std::memcpy(tri.rgba, rgba, sizeof(rgba));
						entry.triangles.push_back(tri);
						taken++;
					}
					// Each sampled triangle stands for step of them.
					entry.scale = taken ? static_cast<float>(prim_count) / static_cast<float>(taken) : 1.0f;
					vb->Unlock();
				}
				ib->Unlock();
			}
			found = effect_meshes_.emplace(key, std::move(entry)).first;
		}
		return &found->second;
	}

	bool ffp_state::draw_world_centre(D3DPRIMITIVETYPE prim, INT base_vertex, UINT start_index, UINT prim_count, float out[3])
	{
		float ox, oy, oz;
		if (!view_proj_valid_ || cur_decl_is_skinned_ || (prim != D3DPT_TRIANGLELIST && prim != D3DPT_TRIANGLESTRIP) || !prim_count ||
			!world_offset(ox, oy, oz))
			return false;
		const effect_mesh_entry* entry = sampled_triangles(prim, base_vertex, start_index, prim_count);
		if (!entry)
			return false;
		double centre[3] = {}, total = 0.0;
		for (const auto& tri : entry->triangles)
		{
			float w[3][3];
			for (int k = 0; k < 3; k++)
			{
				// The world rows (VS c8 on, within the 256 registers), relative to the camera, then the world offset (as mesh_lights).
				const float* m = vs_const_ + (8 + 3 * std::min(static_cast<int>(tri.bone[k]), 82)) * 4;
				const float* p = tri.p[k];
				w[k][0] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3] + ox;
				w[k][1] = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7] + oy;
				w[k][2] = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11] + oz;
			}
			const float e1[3] = { w[1][0] - w[0][0], w[1][1] - w[0][1], w[1][2] - w[0][2] };
			const float e2[3] = { w[2][0] - w[0][0], w[2][1] - w[0][1], w[2][2] - w[0][2] };
			const float cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
			const double area = 0.5 * std::sqrt(static_cast<double>(cx) * cx + static_cast<double>(cy) * cy + static_cast<double>(cz) * cz);
			if (!std::isfinite(area))
				continue;
			for (int a = 0; a < 3; a++)
				centre[a] += area * (w[0][a] + w[1][a] + w[2][a]) / 3.0;
			total += area;
		}
		if (!(total > 0.0))
			return false;
		for (int a = 0; a < 3; a++)
			out[a] = static_cast<float>(centre[a] / total);
		return std::isfinite(out[0] + out[1] + out[2]);
	}

	std::array<float, 4> ffp_state::texture_average(IDirect3DBaseTexture9* texture)
	{
		if (auto it = texture_average_.find(texture); it != texture_average_.end())
			return it->second;

		// The smallest mip level is (close to) the texture's average. Formats DS1 uses
		// for sprite masks: DXT1/3/5 and 8888; anything else counts as 1.
		std::array<float, 4> avg = { 1.0f, 1.0f, 1.0f, 1.0f };
		if (texture && texture->GetType() == D3DRTYPE_TEXTURE)
		{
			auto* t2d = static_cast<IDirect3DTexture9*>(texture);
			const DWORD level = t2d->GetLevelCount() ? t2d->GetLevelCount() - 1 : 0;
			D3DSURFACE_DESC desc {};
			D3DLOCKED_RECT lr {};
			if (SUCCEEDED(t2d->GetLevelDesc(level, &desc)) && SUCCEEDED(t2d->LockRect(level, &lr, nullptr, D3DLOCK_READONLY)) && lr.pBits)
			{
				const auto* p = static_cast<const unsigned char*>(lr.pBits);
				auto rgb565 = [](uint16_t c, float* out)
				{
					out[0] = ((c >> 11) & 31) / 31.0f;
					out[1] = ((c >> 5) & 63) / 63.0f;
					out[2] = (c & 31) / 31.0f;
				};
				// Every block / texel of that level (a texture without mips is averaged whole).
				double sum[4] = {};
				size_t n = 0;
				switch (desc.Format)
				{
				case D3DFMT_DXT1:
				case D3DFMT_DXT3:
				case D3DFMT_DXT5:
				{
					// Per block: the mean of its two endpoint colours. DXT3/5 put 8 bytes of
					// alpha before the colour block.
					const size_t block = desc.Format == D3DFMT_DXT1 ? 8 : 16;
					const size_t colour = desc.Format == D3DFMT_DXT1 ? 0 : 8;
					const UINT bw = std::max(1u, (desc.Width + 3) / 4), bh = std::max(1u, (desc.Height + 3) / 4);
					for (UINT by = 0; by < bh; by++)
						for (UINT bx = 0; bx < bw; bx++, n++)
						{
							const unsigned char* b = p + static_cast<size_t>(by) * lr.Pitch + bx * block;
							float c0[3], c1[3];
							rgb565(*reinterpret_cast<const uint16_t*>(b + colour), c0);
							rgb565(*reinterpret_cast<const uint16_t*>(b + colour + 2), c1);
							for (int c = 0; c < 3; c++)
								sum[c] += 0.5 * (c0[c] + c1[c]);
							sum[3] += desc.Format == D3DFMT_DXT5 ? 0.5 * (b[0] + b[1]) / 255.0
								: desc.Format == D3DFMT_DXT3 ? (b[0] & 0xF) / 15.0 : 1.0;
						}
					break;
				}
				case D3DFMT_A8R8G8B8:
				case D3DFMT_X8R8G8B8:
					for (UINT y = 0; y < desc.Height; y++)
						for (UINT x = 0; x < desc.Width; x++, n++)
						{
							const unsigned char* t = p + static_cast<size_t>(y) * lr.Pitch + x * 4;
							sum[0] += t[2] / 255.0; sum[1] += t[1] / 255.0; sum[2] += t[0] / 255.0;
							sum[3] += desc.Format == D3DFMT_A8R8G8B8 ? t[3] / 255.0 : 1.0;
						}
					break;
				default:
					break;
				}
				if (n)
					for (int c = 0; c < 4; c++)
						avg[c] = static_cast<float>(sum[c] / n);
				t2d->UnlockRect(level);
			}
		}
		texture_average_[texture] = avg;
		return avg;
	}

	const ffp_state::mask_texels* ffp_state::texture_texels(IDirect3DBaseTexture9* texture)
	{
		if (auto it = mask_texels_.find(texture); it != mask_texels_.end())
			return it->second.texels.empty() ? nullptr : &it->second;

		// A subdivided sprite's vertices are several top-level texels apart: a mip this small is what they can
		// carry without aliasing (and crawling as the uvs scroll).
		constexpr UINT kMaskTexelSize = 16;
		mask_texels result;
		if (texture && texture->GetType() == D3DRTYPE_TEXTURE)
		{
			auto* t2d = static_cast<IDirect3DTexture9*>(texture);
			DWORD level = 0;
			D3DSURFACE_DESC desc {};
			if (SUCCEEDED(t2d->GetLevelDesc(0, &desc)))
			{
				result.full_width = desc.Width;
				result.full_height = desc.Height;
			}
			while (SUCCEEDED(t2d->GetLevelDesc(level, &desc)) && desc.Width > kMaskTexelSize && level + 1 < t2d->GetLevelCount())
				++level;
			D3DLOCKED_RECT lr {};
			if (SUCCEEDED(t2d->GetLevelDesc(level, &desc)) && SUCCEEDED(t2d->LockRect(level, &lr, nullptr, D3DLOCK_READONLY)) && lr.pBits)
			{
				const auto* p = static_cast<const unsigned char*>(lr.pBits);
				const UINT w = desc.Width, h = desc.Height;
				auto& t = result.texels;
				auto rgb565 = [](uint16_t c, float* out)
				{
					out[0] = ((c >> 11) & 31) / 31.0f;
					out[1] = ((c >> 5) & 63) / 63.0f;
					out[2] = (c & 31) / 31.0f;
				};
				switch (desc.Format)
				{
				case D3DFMT_DXT1:
				case D3DFMT_DXT3:
				case D3DFMT_DXT5:
				{
					t.assign(static_cast<size_t>(w) * h, { 0.0f, 0.0f, 0.0f, 1.0f });
					const size_t block = desc.Format == D3DFMT_DXT1 ? 8 : 16;
					const size_t colour = desc.Format == D3DFMT_DXT1 ? 0 : 8;
					for (UINT by = 0; by < (h + 3) / 4; by++)
						for (UINT bx = 0; bx < (w + 3) / 4; bx++)
						{
							const unsigned char* b = p + static_cast<size_t>(by) * lr.Pitch + bx * block;
							const uint16_t e0 = *reinterpret_cast<const uint16_t*>(b + colour);
							const uint16_t e1 = *reinterpret_cast<const uint16_t*>(b + colour + 2);
							float pal[4][4] = {};
							rgb565(e0, pal[0]);
							rgb565(e1, pal[1]);
							// DXT1 with e0 <= e1: a three-colour block whose fourth entry is transparent black.
							const bool three = desc.Format == D3DFMT_DXT1 && e0 <= e1;
							for (int c = 0; c < 3; c++)
							{
								pal[2][c] = three ? 0.5f * (pal[0][c] + pal[1][c]) : (2.0f * pal[0][c] + pal[1][c]) / 3.0f;
								pal[3][c] = three ? 0.0f : (pal[0][c] + 2.0f * pal[1][c]) / 3.0f;
							}
							pal[0][3] = pal[1][3] = pal[2][3] = 1.0f;
							pal[3][3] = three ? 0.0f : 1.0f;
							const uint32_t bits = *reinterpret_cast<const uint32_t*>(b + colour + 4);
							// DXT5 alpha: two endpoints and 3-bit indices (8 or 6 interpolated steps).
							float alpha5[8] = {};
							uint64_t alpha_bits = 0;
							if (desc.Format == D3DFMT_DXT5)
							{
								const float a0 = b[0] / 255.0f, a1 = b[1] / 255.0f;
								alpha5[0] = a0; alpha5[1] = a1;
								for (int i = 1; i < 7; i++)
									alpha5[i + 1] = b[0] > b[1] ? ((7 - i) * a0 + i * a1) / 7.0f
										: i < 5 ? ((5 - i) * a0 + i * a1) / 5.0f : (i == 5 ? 0.0f : 1.0f);
								for (int i = 0; i < 6; i++)
									alpha_bits |= static_cast<uint64_t>(b[2 + i]) << (8 * i);
							}
							for (UINT y = 0; y < 4; y++)
								for (UINT x = 0; x < 4; x++)
								{
									const UINT px = bx * 4 + x, py = by * 4 + y;
									if (px >= w || py >= h)
										continue;
									const UINT i = y * 4 + x;
									auto& out = t[static_cast<size_t>(py) * w + px];
									const float* e = pal[(bits >> (2 * i)) & 3];
									out = { e[0], e[1], e[2], e[3] };
									if (desc.Format == D3DFMT_DXT3)
										out[3] = ((b[i / 2] >> (4 * (i & 1))) & 0xF) / 15.0f;
									else if (desc.Format == D3DFMT_DXT5)
										out[3] = alpha5[(alpha_bits >> (3 * i)) & 7];
								}
						}
					break;
				}
				case D3DFMT_A8R8G8B8:
				case D3DFMT_X8R8G8B8:
					t.resize(static_cast<size_t>(w) * h);
					for (UINT y = 0; y < h; y++)
						for (UINT x = 0; x < w; x++)
						{
							const unsigned char* s = p + static_cast<size_t>(y) * lr.Pitch + x * 4;
							t[static_cast<size_t>(y) * w + x] = { s[2] / 255.0f, s[1] / 255.0f, s[0] / 255.0f,
								desc.Format == D3DFMT_A8R8G8B8 ? s[3] / 255.0f : 1.0f };
						}
					break;
				default:
					break;
				}
				if (!t.empty())
				{
					result.width = w;
					result.height = h;
					// Box mips down to one texel, for the area mean over a sprite cell (sample_footprint).
					{
						const std::vector<std::array<float, 4>>* from = &t;
						UINT fw = w, fh = h;
						while (fw > 1 || fh > 1)
						{
							const UINT nw = std::max(fw / 2, 1u), nh = std::max(fh / 2, 1u);
							mask_mip mip { nw, nh, std::vector<std::array<float, 4>>(static_cast<size_t>(nw) * nh) };
							for (UINT y = 0; y < nh; y++)
								for (UINT x = 0; x < nw; x++)
								{
									const UINT x0 = x * fw / nw, x1 = std::min((x + 1) * fw / nw, fw), y0 = y * fh / nh, y1 = std::min((y + 1) * fh / nh, fh);
									std::array<float, 4> sum {};
									for (UINT b = y0; b < y1; b++)
										for (UINT a = x0; a < x1; a++)
											for (int k = 0; k < 4; k++)
												sum[k] += (*from)[static_cast<size_t>(b) * fw + a][k];
									const float n = static_cast<float>((x1 - x0) * (y1 - y0));
									for (int k = 0; k < 4; k++)
										mip.texels[static_cast<size_t>(y) * nw + x][k] = sum[k] / n;
								}
							result.mips.push_back(std::move(mip));
							from = &result.mips.back().texels;
							fw = nw;
							fh = nh;
						}
					}
					result.flat = std::all_of(t.begin(), t.end(), [&](const std::array<float, 4>& texel)
					{
						for (int k = 0; k < 4; k++)
							if (std::abs(texel[k] - t[0][k]) > 2.0f / 255.0f)
								return false;
						return true;
					});
				}
				t2d->UnlockRect(level);
			}
		}
		auto& stored = mask_texels_[texture] = std::move(result);
		return stored.texels.empty() ? nullptr : &stored;
	}

	namespace
	{
		// Bilinear, wrapping (the masks' sampler state).
		std::array<float, 4> sample_wrap(const std::vector<std::array<float, 4>>& texels, UINT width, UINT height, float u, float v)
		{
			const float fx = u * width - 0.5f, fy = v * height - 0.5f;
			const float x0f = std::floor(fx), y0f = std::floor(fy);
			const float tx = fx - x0f, ty = fy - y0f;
			auto wrap = [](long long i, UINT n) { return static_cast<UINT>(((i % static_cast<long long>(n)) + n) % n); };
			const UINT x0 = wrap(static_cast<long long>(x0f), width), x1 = wrap(static_cast<long long>(x0f) + 1, width);
			const UINT y0 = wrap(static_cast<long long>(y0f), height), y1 = wrap(static_cast<long long>(y0f) + 1, height);
			const auto& a = texels[static_cast<size_t>(y0) * width + x0];
			const auto& b = texels[static_cast<size_t>(y0) * width + x1];
			const auto& c = texels[static_cast<size_t>(y1) * width + x0];
			const auto& d = texels[static_cast<size_t>(y1) * width + x1];
			std::array<float, 4> r {};
			for (int k = 0; k < 4; k++)
				r[k] = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty;
			return r;
		}
	}

	namespace
	{
		// The texture averaged over a footprint this wide in uv: the box mip whose texel is about that wide, bilinear, and between mips.
		template <typename T>
		std::array<float, 4> sample_footprint(const T& mask, float u, float v, float footprint)
		{
			const float texels = footprint * static_cast<float>(std::max(mask.width, mask.height));
			if (texels <= 1.0f || mask.mips.empty())
				return sample_wrap(mask.texels, mask.width, mask.height, u, v);
			const float level = std::log2(texels);
			const int below = std::min(static_cast<int>(level), static_cast<int>(mask.mips.size()));
			const int above = std::min(below + 1, static_cast<int>(mask.mips.size()));
			const float t = std::clamp(level - static_cast<float>(below), 0.0f, 1.0f);
			auto at = [&](int m) -> std::array<float, 4>
			{
				if (m == 0)
					return sample_wrap(mask.texels, mask.width, mask.height, u, v);
				const auto& mip = mask.mips[static_cast<size_t>(m) - 1];
				return sample_wrap(mip.texels, mip.width, mip.height, u, v);
			};
			const auto a = at(below), b = at(above);
			return { a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t, a[3] + (b[3] - a[3]) * t };
		}
	}

	UINT ffp_state::multi_sprite_triangles(D3DPRIMITIVETYPE prim, UINT prim_count, const void* indices, D3DFORMAT index_format,
		const void* vertices, UINT stride, UINT count, float colour_scale, DWORD& carrier_sampler)
	{
		// The Multi shaders' colour is s0 (TEXCOORD0.xy) x s1 (TEXCOORD0.zw) x s3 (TEXCOORD1) x vcolor
		// (FRPG_Sfx_SimpleSprite_Base.fxh, PS_Multi). Remix takes one texture: the largest of them carries the
		// detail (the item glow's smoke wisps, s1 s00139, over a fuzzy disc on s0) and goes to Remix at its own
		// uv, the others are sampled per vertex of a subdivided draw. One the CPU cannot read must be the one
		// Remix takes; with two of those, the draw is left to the averaged path.
		struct layer { DWORD sampler; int uv_off; const mask_texels* texels; UINT size; };
		layer layers[3] = {
			{ 0, cur_decl_has_texcoord_ ? cur_decl_texcoord_off_ : -1 },
			{ 1, cur_decl_has_texcoord_ && cur_decl_texcoord_type_ == D3DDECLTYPE_FLOAT4 ? cur_decl_texcoord_off_ + 8 : -1 },
			{ 3, cur_decl_texcoord1_off_ },
		};
		int carrier = -1, unreadable = -1, used = 0;
		for (int i = 0; i < 3; i++)
		{
			layer& l = layers[i];
			if (!(cur_ps_sampler_mask_ & (1u << l.sampler)) || !cur_texture_[l.sampler])
				continue;
			if (l.uv_off < 0)
				return 0;
			++used;
			l.texels = texture_texels(cur_texture_[l.sampler]);
			if (!l.texels)
			{
				if (unreadable >= 0)
					return 0;
				unreadable = i;
				continue;
			}
			l.size = l.texels->full_width * l.texels->full_height;
			if (carrier < 0 || l.size > layers[carrier].size)
				carrier = i;
		}
		if (used < 2)
			return 0;
		if (unreadable >= 0)
			carrier = unreadable;
		// With s0 carrying and every other layer one colour, the averaged path is already exact.
		if (carrier == 0 && std::all_of(layers + 1, layers + 3, [](const layer& l) { return !l.texels || l.texels->flat; }))
			return 0;
		carrier_sampler = layers[carrier].sampler;

		// Sprite quads are a few metres across at most: up to 6 x 6 cells per triangle follow the smooth layers. Fewer by how large it is on
		// screen, a cell about 35 pixels of a 1080p view at most: a fire 90 m off in the Demon Ruins, ten pixels a sprite, took 39,000 triangles
		// a frame and 3 ms of the game thread for what one triangle shows.
		constexpr int kCells = 6;
		constexpr float kCellsPerScreenSize = 30.0f;
		struct vertex { float p[3]; float c[4]; float uv[3][2]; };
		struct sprite_vertex { float x, y, z; DWORD color; float u, v; };
		const auto* src = static_cast<const unsigned char*>(vertices);
		auto read = [&](UINT i)
		{
			const unsigned char* sv = src + static_cast<size_t>(i) * stride;
			vertex v {};
			std::memcpy(v.p, sv + cur_decl_pos_off_, sizeof(v.p));
			const float fog_alpha = sprite_fog_alpha(v.p);
			if (cur_ps_sfx_class_ == sfx_class::SoftSprite && cur_decl_feather_off_ >= 0)
				push_soft_sprite(v.p, *reinterpret_cast<const float*>(sv + cur_decl_feather_off_));
			const DWORD c = cur_decl_has_color_ ? pack_decl_color(sv, cur_decl_color_off_, cur_decl_color_type_) : 0xFFFFFFFFu;
			v.c[0] = ((c >> 16) & 0xFF) / 255.0f; v.c[1] = ((c >> 8) & 0xFF) / 255.0f; v.c[2] = (c & 0xFF) / 255.0f; v.c[3] = (c >> 24) / 255.0f * fog_alpha;
			for (int l = 0; l < 3; l++)
				if (layers[l].uv_off >= 0)
					std::memcpy(v.uv[l], sv + layers[l].uv_off, sizeof(v.uv[l]));
			return v;
		};
		auto vertex_of = [&](UINT i) -> UINT
		{
			if (!indices) return i;
			return index_format == D3DFMT_INDEX32 ? static_cast<const uint32_t*>(indices)[i] : static_cast<const uint16_t*>(indices)[i];
		};
		float footprint[3] = {};
		auto pack = [&](const vertex& v)
		{
			float c[4] = { v.c[0], v.c[1], v.c[2], v.c[3] };
			for (int l = 0; l < 3; l++)
				if (l != carrier && layers[l].texels)
				{
					// Over a cell's footprint: a coarse cell's corners alone can all fall outside a mask's disc, which made distant item glows vanish.
					const auto s = sample_footprint(*layers[l].texels, v.uv[l][0], v.uv[l][1], footprint[l]);
					for (int k = 0; k < 4; k++)
						c[k] *= s[k];
				}
			auto byte = [](float x) { return static_cast<DWORD>(std::clamp(x * 255.0f + 0.5f, 0.0f, 255.0f)); };
			return sprite_vertex { v.p[0], v.p[1], v.p[2],
				byte(c[3]) << 24 | byte(c[0] * colour_scale) << 16 | byte(c[1] * colour_scale) << 8 | byte(c[2] * colour_scale),
				v.uv[carrier][0], v.uv[carrier][1] };
		};

		masked_sprite_vertices_.clear();
		UINT triangles = 0;
		constexpr int kGridPoints = (kCells + 1) * (kCells + 2) / 2;
		vertex grid[kGridPoints];
		sprite_vertex packed[kGridPoints];
		int cells = kCells;
		auto point_of = [&](int i, int j) { return i * (2 * cells + 3 - i) / 2 + j; };
		auto at = [&](int i, int j) -> const sprite_vertex& { return packed[point_of(i, j)]; };
		auto emit = [&](const sprite_vertex& a, const sprite_vertex& b, const sprite_vertex& c)
		{
			for (const sprite_vertex* v : { &a, &b, &c })
			{
				const auto* bytes = reinterpret_cast<const unsigned char*>(v);
				masked_sprite_vertices_.insert(masked_sprite_vertices_.end(), bytes, bytes + sizeof(*v));
			}
			++triangles;
		};
		for (UINT t = 0; t < prim_count; t++)
		{
			UINT idx[3];
			if (prim == D3DPT_TRIANGLELIST) { idx[0] = vertex_of(3 * t); idx[1] = vertex_of(3 * t + 1); idx[2] = vertex_of(3 * t + 2); }
			else if (prim == D3DPT_TRIANGLESTRIP)
			{
				// Every other strip triangle is wound the other way round.
				idx[0] = vertex_of(t); idx[1] = vertex_of(t + 1 + (t & 1)); idx[2] = vertex_of(t + 2 - (t & 1));
			}
			else { idx[0] = vertex_of(0); idx[1] = vertex_of(t + 1); idx[2] = vertex_of(t + 2); }
			if (idx[0] >= count || idx[1] >= count || idx[2] >= count)
				continue;
			const vertex corner[3] = { read(idx[0]), read(idx[1]), read(idx[2]) };
			{
				float longest = 0.0f, distance = 0.0f;
				for (int c = 0; c < 3; c++)
				{
					const vertex& from = corner[c];
					const vertex& to = corner[(c + 1) % 3];
					const float dx = to.p[0] - from.p[0], dy = to.p[1] - from.p[1], dz = to.p[2] - from.p[2];
					longest = std::max(longest, dx * dx + dy * dy + dz * dz);
				}
				for (int k = 0; k < 3; k++)
				{
					const float d = corner[0].p[k] - static_cast<float>(sprite_cam_[k]);
					distance += d * d;
				}
				const float size = distance > 1e-4f ? std::sqrt(longest / distance) : 1.0f;
				cells = std::clamp(static_cast<int>(std::ceil(size * kCellsPerScreenSize)), 1, kCells);
			}
			const int points = (cells + 1) * (cells + 2) / 2;
			for (int l = 0; l < 3; l++)
			{
				float span = 0.0f;
				if (layers[l].uv_off >= 0)
					for (int c = 0; c < 3; c++)
						for (int k = 0; k < 2; k++)
							span = std::max(span, std::abs(corner[(c + 1) % 3].uv[l][k] - corner[c].uv[l][k]));
				// Half a cell: enough to keep a coarse cell's mean (simulated: exact to about 5%, 1 and 2 cells exact) without flattening near sprites' peak.
				footprint[l] = 0.5f * span / static_cast<float>(cells);
			}
			// Barycentric grid: point (i, j) = corner0 + i/n (corner1 - corner0) + j/n (corner2 - corner0).
			for (int i = 0; i <= cells; i++)
				for (int j = 0; i + j <= cells; j++)
				{
					const float w1 = static_cast<float>(i) / cells, w2 = static_cast<float>(j) / cells, w0 = 1.0f - w1 - w2;
					vertex& g = grid[point_of(i, j)];
					auto mix = [&](auto member, int n)
					{
						for (int k = 0; k < n; k++)
							(g.*member)[k] = w0 * (corner[0].*member)[k] + w1 * (corner[1].*member)[k] + w2 * (corner[2].*member)[k];
					};
					mix(&vertex::p, 3);
					mix(&vertex::c, 4);
					for (int l = 0; l < 3; l++)
						for (int k = 0; k < 2; k++)
							g.uv[l][k] = w0 * corner[0].uv[l][k] + w1 * corner[1].uv[l][k] + w2 * corner[2].uv[l][k];
				}
			for (int p = 0; p < points; p++)
				packed[p] = pack(grid[p]);
			for (int i = 0; i < cells; i++)
				for (int j = 0; i + j < cells; j++)
				{
					emit(at(i, j), at(i + 1, j), at(i, j + 1));
					if (i + j + 1 < cells)
						emit(at(i + 1, j), at(i + 1, j + 1), at(i, j + 1));
				}
		}
		return triangles;
	}

	bool ffp_state::cube_face_averages(IDirect3DBaseTexture9* texture, std::array<std::array<float, 3>, 6>& faces)
	{
		{
			std::shared_lock lock(known_cubes_mutex_);
			if (auto it = cube_faces_.find(texture); it != cube_faces_.end())
			{
				faces = it->second;
				return true;
			}
		}
		if (!texture || texture->GetType() != D3DRTYPE_CUBETEXTURE)
			return false;
		auto* cube = static_cast<IDirect3DCubeTexture9*>(texture);
		D3DSURFACE_DESC desc {};
		if (FAILED(cube->GetLevelDesc(0, &desc)) || (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_DXT1))
			return false;
		for (int f = 0; f < 6; f++)
		{
			D3DLOCKED_RECT lr {};
			const auto face = static_cast<D3DCUBEMAP_FACES>(f);
			if (FAILED(cube->LockRect(face, 0, &lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits)
				return false;
			double sum[3] = {};
			const auto* p = static_cast<const unsigned char*>(lr.pBits);
			// The menus' probes are DXT1: each 4x4 block's texels, from its two colours and 2-bit indices.
			if (desc.Format == D3DFMT_DXT1)
			{
				const UINT blocks_x = (desc.Width + 3) / 4, blocks_y = (desc.Height + 3) / 4;
				for (UINT by = 0; by < blocks_y; by++)
					for (UINT bx = 0; bx < blocks_x; bx++)
					{
						const unsigned char* b = p + static_cast<size_t>(by) * lr.Pitch + static_cast<size_t>(bx) * 8;
						const unsigned c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
						double palette[4][3] = {};
						for (int c = 0; c < 3; c++)
						{
							const unsigned shift = c == 0 ? 11 : c == 1 ? 5 : 0;
							const unsigned bits = c == 1 ? 63 : 31;
							const double a = 255.0 * ((c0 >> shift) & bits) / bits, z = 255.0 * ((c1 >> shift) & bits) / bits;
							palette[0][c] = a;
							palette[1][c] = z;
							palette[2][c] = c0 > c1 ? (2.0 * a + z) / 3.0 : 0.5 * (a + z);
							palette[3][c] = c0 > c1 ? (a + 2.0 * z) / 3.0 : 0.0;
						}
						for (int t = 0; t < 16; t++)
						{
							const int index = (b[4 + t / 4] >> (2 * (t % 4))) & 3;
							for (int c = 0; c < 3; c++)
								sum[c] += palette[index][c];
						}
					}
				cube->UnlockRect(face, 0);
				const double texels = 255.0 * blocks_x * blocks_y * 16.0;
				faces[f] = { static_cast<float>(sum[0] / texels), static_cast<float>(sum[1] / texels), static_cast<float>(sum[2] / texels) };
				continue;
			}
			for (UINT y = 0; y < desc.Height; y++)
				for (UINT x = 0; x < desc.Width; x++)
				{
					const unsigned char* t = p + static_cast<size_t>(y) * lr.Pitch + x * 4;
					sum[0] += t[2]; sum[1] += t[1]; sum[2] += t[0];
				}
			cube->UnlockRect(face, 0);
			const double n = 255.0 * desc.Width * desc.Height;
			faces[f] = { static_cast<float>(sum[0] / n), static_cast<float>(sum[1] / n), static_cast<float>(sum[2] / n) };
		}
		std::unique_lock lock(known_cubes_mutex_);
		cube_faces_[texture] = faces;
		return true;
	}

	int ffp_state::secondary_blend_stage() const
	{
		if (!cfg_ || !cfg_->texture_layer_blend || !cur_decl_has_texcoord_ || !cur_decl_has_color_)
			return -1;
		if (cur_decl_texcoord_type_ != D3DDECLTYPE_SHORT4 && cur_decl_texcoord_type_ != D3DDECLTYPE_FLOAT4)
			return -1;

		// The draw must actually sample a second diffuse: only the two-layer "Mul"
		// pixel shaders declare sampler s3. A texture bound on stage 3 alone is not
		// a reliable signal — DS1 leaves stale bindings (e.g. character face maps)
		// on s3+ across material changes.
		if ((cur_ps_sampler_mask_ & (1u << 3)) == 0)
			return -1;

		const int primary_stage = cfg_->albedo_stage;
		auto* primary_tex = (primary_stage >= 0 && primary_stage < 8) ? cur_texture_[primary_stage] : nullptr;
		if (primary_stage != 3 && cur_texture_[3] && cur_texture_[3] != primary_tex)
			return 3;
		return -1;
	}

	bool ffp_state::setup_overlay_decal_pass(IDirect3DDevice9* dev, INT base_vtx, UINT min_vtx, UINT num_verts)
	{
		if (!dev) return false;

		// Cheap rejects first — most draws are single-layer and must not pay for
		// the profiler scope or anything below it.
		const int overlay_stage = secondary_blend_stage();
		if (overlay_stage < 0)
			return false;

		ffp_prof::scope prof(ffp_prof::step::overlay_setup);

		auto* overlay_texture = cur_texture_[overlay_stage];
		if (!overlay_texture)
			return false;

		// Overlay VB with TEXCOORD0.zw as the UV set — its distinct texcoord data
		// gives the draw its own geometry hash (and thus its own BLAS/instance) in
		// Remix, so it cannot clobber the base draw's material.
		if (!expand_rigid_texcoords(dev, base_vtx, min_vtx, num_verts, rigid_layer::overlay))
			return false;

		apply_overlay_decal_states(dev, overlay_texture);
		return true;
	}

	bool ffp_state::setup_glass_pass(IDirect3DDevice9* dev, INT base_vtx, UINT min_vtx, UINT num_verts,
		IDirect3DBaseTexture9* glass_texture)
	{
		if (!dev || !glass_texture)
			return false;
		if (!expand_rigid_texcoords(dev, base_vtx, min_vtx, num_verts, rigid_layer::glass))
			return false;

		// Stage 0 keeps the came pass's ops, so the glass texel's alpha is its opacity as in the game.
		// No alpha test: the derived texture's came is already alpha 0. An inverted test (LESS the cut)
		// made Remix drop the whole pass at a distance (frame_20261028_223122: the window went black).
		dev->SetTexture(0, glass_texture);
		dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
		dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
		return true;
	}

	void ffp_state::apply_overlay_decal_states(IDirect3DDevice9* dev, IDirect3DBaseTexture9* overlay_texture)
	{
		dev->SetTexture(0, overlay_texture);
		for (DWORD ts = 1; ts < 8; ts++)
			dev->SetTexture(ts, nullptr);
		nulled_high_stages_ = true;

		// Albedo = overlay texture tinted by vertex RGB (same as the base layer);
		// opacity = painted vertex alpha — Remix evaluates these per hit from the
		// legacy texture stage state.
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
		dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
		dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		// Stage 0's alpha now differs from setup_texture_stages; TextureStages=1 must
		// re-apply it for the next converted draw.
		ffp_stages_applied_ = false;
		// Per-draw decal tag consumed by the custom RTX Remix runtime
		// (rtx.fixedFunctionDecalMarkerEnabled). D3DTSS_CONSTANT is inert unless a
		// stage argument references D3DTA_CONSTANT.
		dev->SetTextureStageState(0, D3DTSS_CONSTANT, OVERLAY_DECAL_MARKER);

		// Coplanar alpha-blended overlay. Remix requires blending enabled for the
		// decal category to take effect; decal ordering is handled by its any-hit
		// shader sort, so no geometric offset is needed.
		dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
		dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
	}

	bool ffp_state::apply_blended_decal_marker(IDirect3DDevice9* dev)
	{
		if (!dev || !cfg_ || !cfg_->texture_layer_blend)
			return false;

		// [FFPSteps] DecalQuery=0 drops the 3 state reads. The game's own
		// alpha-blended edge-fade strips then reach Remix as unordered
		// translucency (dark fringes), so this is a measurement switch rather
		// than something to ship off.
		if (steps_ && !steps_->decal_query)
			return false;

		ffp_prof::scope prof(ffp_prof::step::decal_query);

		// Classic decal signature only: straight alpha blending. Additive and other
		// blend modes (glows, effects) are handled by Remix's emissive blend paths.
		// Queried from the device because DS1 applies blend state via
		// StateBlock::Apply, bypassing the SetRenderState intercept.
		DWORD blend_enabled = FALSE;
		dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend_enabled);
		if (!blend_enabled)
			return false;

		DWORD src = 0, dst = 0;
		dev->GetRenderState(D3DRS_SRCBLEND, &src);
		dev->GetRenderState(D3DRS_DESTBLEND, &dst);
		if (src != D3DBLEND_SRCALPHA || dst != D3DBLEND_INVSRCALPHA)
			return false;

		// A skinned mesh never lies on a surface: a character drawn blended (a dying one fading out,
		// fur, a veil) is its own surface. As a decal Remix shaded it onto what is behind it.
		if (cur_decl_is_skinned())
			return false;
		// Nor does an unlit one (PS_UNLIT): every alpha-blended _Non mesh in the game is an SFX model
		// (analysis_shader_families.md), free-floating glow layers such as Bed of Chaos's glyph rings.
		// As decals Remix composited them onto the surface behind and they showed as thin bands only
		// where they came near one.
		if (cur_ps_light_flags_ & PS_UNLIT)
			return false;
		// Nor an effect mesh the game lights itself (Quelaag's lava crust). Remix had it dark on the ground.
		if (cur_is_lit_effect_mesh())
			return false;

		// Its own marker: unlike the coplanar overlay layer of a two-layer material, a skirt can hang a
		// little above what it covers, and the runtime lights it there (SKIRT_DECAL_MARKER).
		dev->SetTextureStageState(0, D3DTSS_CONSTANT, SKIRT_DECAL_MARKER);
		return true;
	}

	void ffp_state::clear_decal_marker(IDirect3DDevice9* dev)
	{
		if (dev)
			dev->SetTextureStageState(0, D3DTSS_CONSTANT, 0);
	}

	bool ffp_state::apply_unlit_layer_marker(IDirect3DDevice9* dev, bool light_shaft)
	{
		// An alpha-blended unlit mesh (PS_UNLIT: texture * c100 * vertex colour * c139, every one an SFX
		// glow layer such as Bed of Chaos's glyph rings) is drawn like the unlit sprites: the sprites' unlit
		// marker, so Remix composites it as the game does (alpha-blended in gamma space, lighting nothing),
		// with its exposure count. Its colour is c100 times the texture, which the self-lit tag carries
		// (atmosphere.hpp tag_self_lit). Additive ones (the bonfire and torch flames) keep their own path.
		// So is a lit effect mesh, whose light goes in that tag too, and a light shaft (texture_names::is_light_shaft).
		if (!dev || !((cur_ps_light_flags_ & PS_UNLIT) || cur_is_lit_effect_mesh() || light_shaft) || cur_decl_is_skinned())
			return false;
		DWORD blend_enabled = FALSE, src = 0, dst = 0;
		dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend_enabled);
		dev->GetRenderState(D3DRS_SRCBLEND, &src);
		dev->GetRenderState(D3DRS_DESTBLEND, &dst);
		if (!blend_enabled || src != D3DBLEND_SRCALPHA || dst != D3DBLEND_INVSRCALPHA)
			return false;
		const int exposure_power = (cur_ps_reads_exposure_ ? 1 : 0) + (cur_vs_exposure_ ? 1 : 0);
		dev->SetTextureStageState(0, D3DTSS_CONSTANT, exposure_power ? 0xD15D0E20u + exposure_power : 0xD15D0E1Du);
		return true;
	}

	// ---- Rigid vertex expansion ----

	uint64_t ffp_state::fingerprint_source(const unsigned char* src, UINT total_verts,
		UINT stride, rigid_layer layer) const
	{
		// FNV-1a over the geometry dimensions, the declaration layout the
		// expansion decodes with, and up to 16 evenly spaced 8-byte samples of the
		// source range. Reading ~128 bytes instead of the whole range keeps this
		// far cheaper than the conversion it guards, while still changing if the
		// buffer was destroyed and re-created at the same address with different
		// contents, or the same bytes are drawn through another declaration.
		uint64_t h = 1469598103934665603ull;
		auto mix = [&h](uint64_t v)
		{
			for (int b = 0; b < 8; b++)
			{
				h ^= static_cast<unsigned char>(v >> (b * 8));
				h *= 1099511628211ull;
			}
		};

		mix(total_verts);
		mix(stride);
		mix(static_cast<uint64_t>(layer));
		mix(static_cast<uint64_t>(cur_decl_pos_off_));
		mix(cur_decl_has_normal_ ? ((static_cast<uint64_t>(cur_decl_normal_off_) << 8) | (cur_decl_normal_type_ & 0xFF)) : ~0ull);
		mix(cur_decl_has_color_ ? ((static_cast<uint64_t>(cur_decl_color_off_) << 8) | (cur_decl_color_type_ & 0xFF)) : ~0ull);
		mix(cur_decl_has_texcoord_ ? ((static_cast<uint64_t>(cur_decl_texcoord_off_) << 8) | (cur_decl_texcoord_type_ & 0xFF)) : ~0ull);
		// The bone indices decide whether a range is expanded through the palette.
		mix(cur_decl_has_blend_indices_ ? ((static_cast<uint64_t>(cur_decl_blend_indices_off_) << 8) | (cur_decl_blend_indices_type_ & 0xFF)) : ~0ull);
		mix(cur_decl_is_skinned_ ? 1ull : 0ull);

		const UINT span = total_verts * stride;
		if (span == 0) return h;

		constexpr UINT SAMPLES = 16;
		const UINT step_bytes = (span > SAMPLES * 8) ? (span - 8) / (SAMPLES - 1) : 0;
		for (UINT i = 0; i < SAMPLES; i++)
		{
			const UINT off = (step_bytes == 0)
				? std::min<UINT>(i * 8, (span > 8) ? span - 8 : 0)
				: i * step_bytes;
			if (off + 8 > span) break;
			uint64_t chunk = 0;
			std::memcpy(&chunk, src + off, 8);
			mix(chunk);
		}
		return h;
	}

	bool ffp_state::expand_rigid_texcoords(IDirect3DDevice9* dev, INT base_vtx, UINT min_vtx, UINT num_verts, rigid_layer layer)
	{
		const bool overlay_uv = layer == rigid_layer::overlay;
		if (num_verts == 0) return false;

		ffp_prof::scope prof(ffp_prof::step::expand);

		// The overlay UV pair only exists in 4-component texcoord layouts.
		if (overlay_uv &&
			cur_decl_texcoord_type_ != D3DDECLTYPE_SHORT4 && cur_decl_texcoord_type_ != D3DDECLTYPE_FLOAT4)
			return false;

		auto* src_vb = stream_vb_[0];
		UINT stride = stream_stride_[0];
		if (!src_vb || stride == 0 || base_vtx < 0) return false;

		UINT total_verts = min_vtx + num_verts;

		// Validate source VB has enough data for the requested range
		D3DVERTEXBUFFER_DESC vb_desc;
		if (FAILED(src_vb->GetDesc(&vb_desc))) return false;
		UINT byte_end = static_cast<UINT>(base_vtx) * stride + total_verts * stride;
		if (byte_end > vb_desc.Size) return false;

		const UINT rigid_vtx_size = RIGID_VTX_SIZE;

		if (!rigid_exp_decl_)
		{
			D3DVERTEXELEMENT9 elems[] = {
				{ 0,  0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
				{ 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
				{ 0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
				{ 0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
				D3DDECL_END()
			};
			if (FAILED(dev->CreateVertexDeclaration(elems, &rigid_exp_decl_)))
				return false;
		}

		// Per-draw VB cache keyed by source VB + byte offset. Overlay expansions use
		// their own cache so base and overlay VBs (and thus Remix geometry hashes)
		// stay distinct for the same source range.
		UINT byte_offset = static_cast<UINT>(base_vtx) * stride;
		uint64_t cache_key = reinterpret_cast<uintptr_t>(src_vb) ^
			(static_cast<uint64_t>(byte_offset) << 32);

		auto& entry = layer == rigid_layer::overlay ? rigid_overlay_cache_[cache_key] :
			layer == rigid_layer::glass ? rigid_glass_cache_[cache_key] : rigid_exp_cache_[cache_key];
		entry.last_frame = frame_count_;

		// The stock path always re-expands and never serves cached data: the game
		// can destroy and re-create a VB at the same address, so a pointer-keyed
		// hit could return garbage. That safety costs a full rebuild plus a
		// destination Unlock on every draw, and it is that Unlock which memcpy's
		// the range into the bridge IPC channel and forces a GPU re-upload.
		// [FFPSteps] ExpandReuse=1 keeps the safety but validates with a content
		// fingerprint instead of trusting the pointer.

		// Need to expand: allocate/grow MANAGED VB if needed
		if (!entry.vb || entry.capacity < total_verts)
		{
			if (entry.vb) entry.vb->Release();
			UINT new_cap = (total_verts + 255u) & ~255u;
			if (FAILED(dev->CreateVertexBuffer(new_cap * rigid_vtx_size,
				0, 0, D3DPOOL_MANAGED,
				&entry.vb, nullptr)))
			{
				entry = {};
				return false;
			}
			entry.capacity = new_cap;
			entry.vertex_size = rigid_vtx_size;
			entry.expanded = 0;
			entry.has_fingerprint = false; // fresh allocation holds no valid data
		}

		const UINT dst_bytes = total_verts * rigid_vtx_size;

		// Index-only (PIN*) shaders place each vertex with its own bone, c8[3 * index]:
		// debris chunks, and multi-part objects such as a chest whose lid is a second
		// bone. The draw's world matrix is bone 0, so vertices of any other bone are
		// rewritten from the VS constant palette below.
		const bool palette_possible =
			cur_decl_has_blend_indices_ && !cur_decl_is_skinned_ &&
			cur_decl_blend_indices_type_ == D3DDECLTYPE_UBYTE4 && vs_regs_per_bone_ > 0;

		// Lock source VB
		unsigned char* src_data = nullptr;
		{
			ffp_prof::scope lprof(ffp_prof::step::exp_lock);
			if (FAILED(src_vb->Lock(byte_offset, total_verts * stride,
				reinterpret_cast<void**>(&src_data), D3DLOCK_READONLY)))
				return false;
		}

		// Fingerprint first: only the reuse path below records one, and it only runs on
		// ranges found palette-free, so a match also answers the palette question.
		const bool reuse_enabled = steps_ && steps_->expand_reuse;
		const uint64_t fp = reuse_enabled ? fingerprint_source(src_data, total_verts, stride, layer) : 0;
		const bool fingerprint_match = reuse_enabled &&
			entry.has_fingerprint &&
			entry.fingerprint == fp &&
			entry.src_verts == total_verts &&
			entry.src_stride == stride &&
			entry.expanded >= total_verts;

		// Most index-only draws (map pieces) use bone 0 throughout and keep the plain
		// path. A palette rewrite is not a pure function of the source bytes (the same
		// vertices with a moved palette must be re-expanded), so it is not eligible
		// for fingerprint reuse. The scan reads every vertex (2.25 million a frame in
		// Blighttown), so a range the fingerprint matches skips it.
		bool palette_needed = false;
		if (palette_possible && !fingerprint_match)
		{
			for (UINT v = 0; v < total_verts && !palette_needed; v++)
				palette_needed = src_data[v * stride + cur_decl_blend_indices_off_] != 0;
		}

		const bool try_reuse = reuse_enabled && !palette_needed;
		uint64_t pending_fp = 0;
		bool have_pending_fp = false;

		if (try_reuse)
		{
			const bool hit = fingerprint_match;

			if (hit)
			{
				// The expanded VB already holds exactly this data. Skip the
				// destination lock, the conversion and above all the Unlock —
				// nothing is pushed at the bridge and Remix sees no upload.
				{
					ffp_prof::scope uprof(ffp_prof::step::exp_unlock);
					src_vb->Unlock();
				}
				ffp_prof::add_saved_bytes(dst_bytes);
				ffp_prof::count_expand_reuse_hit();

				ffp_prof::scope bprof(ffp_prof::step::exp_bind);
				dev->SetVertexDeclaration(rigid_exp_decl_);
				dev->SetStreamSource(0, entry.vb, 0, rigid_vtx_size);
				rigid_exp_active_ = true;
				return true;
			}

			ffp_prof::count_expand_reuse_miss();
			pending_fp = fp;
			have_pending_fp = true;
		}
		// Until the expansion below completes, the cached buffer does not hold
		// this range. Clear the claim so a failure part-way cannot be served as
		// a hit next frame.
		entry.has_fingerprint = false;

		constexpr float kScale = 1.0f / 1024.0f;

		int tc_type = cur_decl_texcoord_type_;
		int tc_off  = cur_decl_texcoord_off_;
		int n_off   = cur_decl_normal_off_;
		int n_type  = cur_decl_normal_type_;
		int p_off   = cur_decl_pos_off_;

		bool use_palette_indices = palette_needed;
		const int max_palette_matrices = use_palette_indices
			? std::max(1, (256 - vs_reg_world_start_) / vs_regs_per_bone_)
			: 1;
		float base_world[16] = {};
		float base_world_inv[16] = {};
		float relative_mats[256][16] = {};
		bool relative_valid[256] = {};
		float override_cam_x = 0.0f;
		float override_cam_y = 0.0f;
		float override_cam_z = 0.0f;
		const bool has_override_camera = menu_cell_offset(override_cam_x, override_cam_y, override_cam_z) ||
			(use_palette_indices && camera_override_fn_ && !cur_rt_menu_path_traced_ &&
			camera_override_fn_(override_cam_x, override_cam_y, override_cam_z));

		auto apply_world_offset = [&](float* out)
		{
			turn_menu_cell(out);
			if (offset_world_ && has_override_camera)
			{
				out[12] += override_cam_x;
				out[13] += override_cam_y;
				out[14] += override_cam_z;
			}
		};

		auto load_palette_world = [&](int matrix_index, float* out) -> bool
		{
			const int reg = vs_reg_world_start_ + matrix_index * vs_regs_per_bone_;
			if (matrix_index < 0 || reg < 0 || reg + vs_regs_per_bone_ > 256)
				return false;

			const float* src = &vs_const_[reg * 4];
			if (vs_regs_per_bone_ == 3)
			{
				out[0]  = src[0]; out[1]  = src[4]; out[2]  = src[8];  out[3]  = 0.0f;
				out[4]  = src[1]; out[5]  = src[5]; out[6]  = src[9];  out[7]  = 0.0f;
				out[8]  = src[2]; out[9]  = src[6]; out[10] = src[10]; out[11] = 0.0f;
				out[12] = src[3]; out[13] = src[7]; out[14] = src[11]; out[15] = 1.0f;
				apply_world_offset(out);
				return true;
			}

			if (vs_regs_per_bone_ == 4)
			{
				mat4_transpose(out, src);
				apply_world_offset(out);
				return true;
			}

			return false;
		};

		if (use_palette_indices)
		{
			use_palette_indices = load_palette_world(0, base_world);
			// Effect models scale their bones as they play (Quelaag's flame shells, 0.3 to 0.6). A rigid inverse
			// leaves that scale in, and vertices of the other bones shrank to specks.
			if (use_palette_indices && !mat4_invert_affine(base_world_inv, base_world))
				mat4_invert_rigid(base_world_inv, base_world);
		}

		// A palette draw whose source is unchanged can still be served from the cache when every
		// bone it used kept its place relative to bone 0: the expansion bakes inverse(bone 0) *
		// bone i into the vertices, and the camera offset both matrices carry cancels in that
		// product. A static prop therefore matches to float noise (about 1e-5) frame after frame,
		// and the tolerance keeps the served vertices within 0.1 mm of a fresh expansion; a moving
		// part (a chest lid) fails it and is expanded again. Blighttown has 65 such draws a frame.
		constexpr float kPaletteReuseTolerance = 1e-4f;
		if (use_palette_indices && reuse_enabled && entry.palette_valid && entry.palette_fp == fp &&
			entry.src_verts == total_verts && entry.src_stride == stride && entry.expanded >= total_verts)
		{
			bool same = true;
			for (size_t i = 0; i < entry.palette_used.size() && same; i++)
			{
				float selected_world[16] = {};
				if (!load_palette_world(entry.palette_used[i], selected_world))
					std::memcpy(selected_world, base_world, sizeof(selected_world));
				float rel[16];
				mat4_mul(rel, selected_world, base_world_inv);
				const float* cached = &entry.palette_rel[i * 16];
				for (int k = 0; k < 16 && same; k++)
					same = std::fabs(rel[k] - cached[k]) <= kPaletteReuseTolerance;
			}
			if (same)
			{
				{
					ffp_prof::scope uprof(ffp_prof::step::exp_unlock);
					src_vb->Unlock();
				}
				ffp_prof::add_saved_bytes(dst_bytes);
				ffp_prof::count_expand_reuse_hit();

				ffp_prof::scope bprof(ffp_prof::step::exp_bind);
				dev->SetVertexDeclaration(rigid_exp_decl_);
				dev->SetStreamSource(0, entry.vb, 0, rigid_vtx_size);
				rigid_exp_active_ = true;
				return true;
			}
		}
		entry.palette_valid = false;

		// Lock destination VB
		unsigned char* dst_data = nullptr;
		{
			ffp_prof::scope lprof(ffp_prof::step::exp_lock);
			if (FAILED(entry.vb->Lock(0, dst_bytes,
				reinterpret_cast<void**>(&dst_data), 0)))
			{
				src_vb->Unlock();
				return false;
			}
		}

		const int tc_src = overlay_uv ? 2 : 0;
		// Window glass sits this far off its came (metres), along -normal: enough for distinct
		// geometry and no z-fighting at any distance, far below what the eye can see.
		constexpr float kGlassOffset = 0.002f;
		const float glass_offset = layer == rigid_layer::glass ? kGlassOffset : 0.0f;

		{
		ffp_prof::scope cprof(ffp_prof::step::exp_convert);
		for (UINT v = 0; v < total_verts; v++)
		{
			const auto* sv = &src_data[v * stride];
			auto* dv_raw = &dst_data[v * rigid_vtx_size];
			auto* pos_out = reinterpret_cast<float*>(dv_raw + 0);
			auto* norm_out = reinterpret_cast<float*>(dv_raw + 12);
			auto* color_out = reinterpret_cast<DWORD*>(dv_raw + 24);
			auto* uv0_out = reinterpret_cast<float*>(dv_raw + 28);

			// Position (FLOAT3)
			auto* pos = reinterpret_cast<const float*>(&sv[p_off]);
			pos_out[0] = pos[0]; pos_out[1] = pos[1]; pos_out[2] = pos[2];

			// Normal (UBYTE4N -> FLOAT3, with fallbacks)
			if (cur_decl_has_normal_ && n_type == D3DDECLTYPE_UBYTE4N)
			{
				norm_out[0] = (sv[n_off]     / 127.5f) - 1.0f;
				norm_out[1] = (sv[n_off + 1] / 127.5f) - 1.0f;
				norm_out[2] = (sv[n_off + 2] / 127.5f) - 1.0f;
			}
			else if (cur_decl_has_normal_ && n_type == D3DDECLTYPE_FLOAT3)
			{
				auto* nf = reinterpret_cast<const float*>(&sv[n_off]);
				norm_out[0] = nf[0]; norm_out[1] = nf[1]; norm_out[2] = nf[2];
			}
			else
			{
				norm_out[0] = 0.0f; norm_out[1] = 1.0f; norm_out[2] = 0.0f;
			}

			if (use_palette_indices)
			{
				int matrix_index = static_cast<int>(sv[cur_decl_blend_indices_off_]);
				if (matrix_index < 0 || matrix_index >= max_palette_matrices)
					matrix_index = 0;

				if (!relative_valid[matrix_index])
				{
					float selected_world[16] = {};
					if (!load_palette_world(matrix_index, selected_world))
						std::memcpy(selected_world, base_world, sizeof(selected_world));

					mat4_mul(relative_mats[matrix_index], selected_world, base_world_inv);
					relative_valid[matrix_index] = true;
				}

				const float* rel = relative_mats[matrix_index];
				const float px = pos_out[0];
				const float py = pos_out[1];
				const float pz = pos_out[2];
				pos_out[0] = px * rel[0] + py * rel[4] + pz * rel[8]  + rel[12];
				pos_out[1] = px * rel[1] + py * rel[5] + pz * rel[9]  + rel[13];
				pos_out[2] = px * rel[2] + py * rel[6] + pz * rel[10] + rel[14];

				const float nx = norm_out[0];
				const float ny = norm_out[1];
				const float nz = norm_out[2];
				norm_out[0] = nx * rel[0] + ny * rel[4] + nz * rel[8];
				norm_out[1] = nx * rel[1] + ny * rel[5] + nz * rel[9];
				norm_out[2] = nx * rel[2] + ny * rel[6] + nz * rel[10];

				const float len_sq = norm_out[0] * norm_out[0] + norm_out[1] * norm_out[1] + norm_out[2] * norm_out[2];
				if (len_sq > 0.000001f)
				{
					const float inv_len = 1.0f / std::sqrt(len_sq);
					norm_out[0] *= inv_len;
					norm_out[1] *= inv_len;
					norm_out[2] *= inv_len;
				}
			}

			if (glass_offset != 0.0f)
			{
				pos_out[0] -= norm_out[0] * glass_offset;
				pos_out[1] -= norm_out[1] * glass_offset;
				pos_out[2] -= norm_out[2] * glass_offset;
			}

			*color_out = cur_decl_has_color_
				? pack_decl_color(sv, cur_decl_color_off_, cur_decl_color_type_)
				: 0xFFFFFFFFu;

			// Texcoord (convert to FLOAT2 regardless of source type).
			// tc_src selects the UV pair: .xy for the base layer, .zw for the overlay.
			if (cur_decl_has_texcoord_)
			{
				if (tc_type == D3DDECLTYPE_SHORT2 || tc_type == D3DDECLTYPE_SHORT4)
				{
					auto* tc = reinterpret_cast<const short*>(&sv[tc_off]);
					uv0_out[0] = static_cast<float>(tc[tc_src]) * kScale;
					uv0_out[1] = static_cast<float>(tc[tc_src + 1]) * kScale;
				}
				else if (tc_type == D3DDECLTYPE_FLOAT2 || tc_type == D3DDECLTYPE_FLOAT4)
				{
					auto* tc = reinterpret_cast<const float*>(&sv[tc_off]);
					uv0_out[0] = tc[tc_src]; uv0_out[1] = tc[tc_src + 1];
				}
				else
				{
					uv0_out[0] = 0.0f; uv0_out[1] = 0.0f;
				}
			}
			else
			{
				uv0_out[0] = 0.0f; uv0_out[1] = 0.0f;
			}
		}

		} // exp_convert

		{
			// The destination Unlock is where the bridge client memcpy's the
			// written range into the IPC channel (LockableBuffer::unlock) and
			// where the runtime marks the buffer for GPU re-upload. Everything
			// FFP conversion costs on the Remix side is paid here.
			ffp_prof::scope uprof(ffp_prof::step::exp_unlock);
			entry.vb->Unlock();
			src_vb->Unlock();
		}
		entry.expanded = total_verts;
		ffp_prof::add_expand_bytes(dst_bytes);
		if (have_pending_fp)
		{
			// The buffer now genuinely holds this source range.
			entry.fingerprint = pending_fp;
			entry.src_verts = total_verts;
			entry.src_stride = stride;
			entry.has_fingerprint = true;
		}
		else if (use_palette_indices && reuse_enabled)
		{
			// ... and this palette: the bones the vertices used, as they were baked in.
			entry.palette_used.clear();
			entry.palette_rel.clear();
			for (int i = 0; i < 256; i++)
			{
				if (!relative_valid[i])
					continue;
				entry.palette_used.push_back(static_cast<uint8_t>(i));
				entry.palette_rel.insert(entry.palette_rel.end(), relative_mats[i], relative_mats[i] + 16);
			}
			entry.palette_fp = fp;
			entry.src_verts = total_verts;
			entry.src_stride = stride;
			entry.palette_valid = true;
		}

		{
			ffp_prof::scope bprof(ffp_prof::step::exp_bind);
			dev->SetVertexDeclaration(rigid_exp_decl_);
			dev->SetStreamSource(0, entry.vb, 0, rigid_vtx_size);
		}
		rigid_exp_active_ = true;
		return true;
	}

	void ffp_state::restore_rigid_source(IDirect3DDevice9* dev)
	{
		if (!rigid_exp_active_ && !skinned_exp_active_) return;

		ffp_prof::scope prof(ffp_prof::step::restore_stream);

		if (last_decl_) dev->SetVertexDeclaration(last_decl_);
		dev->SetStreamSource(0, stream_vb_[0], stream_offset_[0], stream_stride_[0]);
		rigid_exp_active_ = false;
		skinned_exp_active_ = false;
	}

	bool ffp_state::expand_skinned_texcoords(IDirect3DDevice9* dev, INT base_vtx, UINT min_vtx, UINT num_verts)
	{
		if (!cur_decl_has_texcoord_ || num_verts == 0) return false;

		int tc_type = cur_decl_texcoord_type_;
		if (tc_type != D3DDECLTYPE_SHORT2 && tc_type != D3DDECLTYPE_SHORT4) return false;

		auto* src_vb = stream_vb_[0];
		UINT stride = stream_stride_[0];
		if (!src_vb || stride == 0 || base_vtx < 0) return false;

		int tc_off = cur_decl_texcoord_off_;
		int tc_size = (tc_type == D3DDECLTYPE_SHORT2) ? 4 : 8;
		UINT new_stride = stride - tc_size + 8;  // replace SHORT with FLOAT2 (8 bytes)
		UINT total_verts = min_vtx + num_verts;

		// Validate source VB has enough data for the requested range
		D3DVERTEXBUFFER_DESC vb_desc;
		if (FAILED(src_vb->GetDesc(&vb_desc))) return false;
		UINT byte_end = static_cast<UINT>(base_vtx) * stride + total_verts * stride;
		if (byte_end > vb_desc.Size) return false;

		// Rebuild declaration if the source layout changed
		if (!skinned_exp_decl_ || skinned_exp_tc_type_ != tc_type ||
			skinned_exp_stride_ != static_cast<int>(stride))
		{
			if (skinned_exp_decl_) { skinned_exp_decl_->Release(); skinned_exp_decl_ = nullptr; }

			// Query elements from the current declaration
			D3DVERTEXELEMENT9 elems[MAX_FVF_DECL_SIZE];
			UINT num_elems = 0;
			if (!last_decl_ || FAILED(last_decl_->GetDeclaration(elems, &num_elems)))
				return false;

			// Clone with texcoord promoted to FLOAT2 and offsets adjusted
			for (UINT i = 0; i < num_elems; i++)
			{
				if (elems[i].Stream == 0xFF) break;  // D3DDECL_END
				if (elems[i].Usage == D3DDECLUSAGE_TEXCOORD && elems[i].UsageIndex == 0)
				{
					elems[i].Type = static_cast<BYTE>(D3DDECLTYPE_FLOAT2);
					// Adjust offset of elements after texcoord (if any)
					int delta = 8 - tc_size;
					for (UINT j = i + 1; j < num_elems && elems[j].Stream != 0xFF; j++)
					{
						if (elems[j].Stream == 0 && elems[j].Offset > elems[i].Offset)
							elems[j].Offset += static_cast<WORD>(delta);
					}
					break;
				}
			}

			if (FAILED(dev->CreateVertexDeclaration(elems, &skinned_exp_decl_)))
				return false;

			skinned_exp_tc_type_ = tc_type;
			skinned_exp_stride_ = static_cast<int>(stride);
			skinned_exp_vtx_size_ = new_stride;
		}

		// Cache keyed on (source VB + byte offset), separate from rigid cache
		UINT byte_offset = static_cast<UINT>(base_vtx) * stride;
		uint64_t cache_key = reinterpret_cast<uintptr_t>(src_vb) ^
			(static_cast<uint64_t>(byte_offset) << 32);

		auto& entry = skinned_exp_cache_[cache_key];
		entry.last_frame = frame_count_;

		// Always re-expand from source — never serve cached data.
		// The game can destroy and re-create VBs at the same address (pointer
		// reuse), which makes stale cache hits return garbage vertex data.
		// A buffer sized for another vertex layout would be written past its end.
		if (!entry.vb || entry.capacity < total_verts || entry.vertex_size != new_stride)
		{
			if (entry.vb) entry.vb->Release();
			UINT new_cap = (total_verts + 255u) & ~255u;
			if (FAILED(dev->CreateVertexBuffer(new_cap * new_stride,
				0, 0, D3DPOOL_MANAGED, &entry.vb, nullptr)))
			{
				entry = {};
				return false;
			}
			entry.capacity = new_cap;
			entry.vertex_size = new_stride;
			entry.expanded = 0;
		}

		unsigned char* src_data = nullptr;
		if (FAILED(src_vb->Lock(byte_offset, total_verts * stride,
			reinterpret_cast<void**>(&src_data), D3DLOCK_READONLY)))
			return false;

		unsigned char* dst_data = nullptr;
		if (FAILED(entry.vb->Lock(0, total_verts * new_stride,
			reinterpret_cast<void**>(&dst_data), 0)))
		{
			src_vb->Unlock();
			return false;
		}

		constexpr float kScale = 1.0f / 1024.0f;
		int bytes_before_tc = tc_off;
		int bytes_after_tc = static_cast<int>(stride) - tc_off - tc_size;

		UINT bad_verts = 0;
		for (UINT v = 0; v < total_verts; v++)
		{
			const auto* sv = &src_data[v * stride];
			auto* dv = &dst_data[v * new_stride];

			// Copy everything before texcoord verbatim (pos, indices, weights, normal, color, etc.)
			std::memcpy(dv, sv, bytes_before_tc);

			// Validate source positions — detect NaN, inf, or extreme values
			auto* pos = reinterpret_cast<const float*>(sv);
			if (std::isnan(pos[0]) || std::isnan(pos[1]) || std::isnan(pos[2]) ||
				std::isinf(pos[0]) || std::isinf(pos[1]) || std::isinf(pos[2]) ||
				std::abs(pos[0]) > 1e6f || std::abs(pos[1]) > 1e6f || std::abs(pos[2]) > 1e6f)
			{
				if (bad_verts == 0)
					log("SKINVTX", std::format("BAD src pos v[{}]: ({}, {}, {}) stride={} tc_off={} VB={:#x}",
						v, pos[0], pos[1], pos[2], stride, tc_off,
						reinterpret_cast<uintptr_t>(src_vb)));
				++bad_verts;
			}

			// Expand texcoord: SHORT → FLOAT2
			auto* tc_src = reinterpret_cast<const short*>(&sv[tc_off]);
			auto* tc_dst = reinterpret_cast<float*>(&dv[tc_off]);
			tc_dst[0] = static_cast<float>(tc_src[0]) * kScale;
			tc_dst[1] = static_cast<float>(tc_src[1]) * kScale;

			// Copy any bytes after texcoord (rare, but safe)
			if (bytes_after_tc > 0)
				std::memcpy(&dv[tc_off + 8], &sv[tc_off + tc_size], bytes_after_tc);
		}

		if (bad_verts > 0)
			log("SKINVTX", std::format("total bad verts: {}/{} (base_vtx={} min={} num={})",
				bad_verts, total_verts, base_vtx, min_vtx, num_verts));

		entry.vb->Unlock();
		src_vb->Unlock();
		entry.expanded = total_verts;

		dev->SetVertexDeclaration(skinned_exp_decl_);
		dev->SetStreamSource(0, entry.vb, 0, new_stride);
		skinned_exp_active_ = true;
		return true;
	}

	// ---- Utility ----

	void ffp_state::mat4_transpose(float* dst, const float* src)
	{
		dst[0]  = src[0];  dst[1]  = src[4];  dst[2]  = src[8];  dst[3]  = src[12];
		dst[4]  = src[1];  dst[5]  = src[5];  dst[6]  = src[9];  dst[7]  = src[13];
		dst[8]  = src[2];  dst[9]  = src[6];  dst[10] = src[10]; dst[11] = src[14];
		dst[12] = src[3];  dst[13] = src[7];  dst[14] = src[11]; dst[15] = src[15];
	}

	bool ffp_state::mat4_is_interesting(const float* m)
	{
		bool all_zero = true;
		for (int i = 0; i < 16; i++)
		{
			if (m[i] != 0.0f) { all_zero = false; break; }
		}
		if (all_zero) return false;

		// Check for identity
		if (m[0] == 1.0f && m[1] == 0.0f && m[2] == 0.0f  && m[3] == 0.0f &&
			m[4] == 0.0f && m[5] == 1.0f && m[6] == 0.0f  && m[7] == 0.0f &&
			m[8] == 0.0f && m[9] == 0.0f && m[10] == 1.0f && m[11] == 0.0f &&
			m[12] == 0.0f && m[13] == 0.0f && m[14] == 0.0f && m[15] == 1.0f)
			return false;

		return true;
	}
}
