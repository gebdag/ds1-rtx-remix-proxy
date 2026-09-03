#include "std_include.hpp"
#include "ffp_state.hpp"
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
		// For skinned meshes the world matrix (c8-c10) IS bone 0. A single
		// SetVSConstantF(8, data, 114) covers the full palette: bone0 at c8,
		// bone1 at c11, ... bone37 at c119.  Partial updates (e.g. start=11
		// count=6) refresh specific bones without changing the palette layout.
		if (config::get().skinning.enabled)
		{
			UINT bone_thresh = static_cast<UINT>(vs_reg_bone_threshold_);
			UINT rpb = static_cast<UINT>(vs_regs_per_bone_);
			UINT min_regs = static_cast<UINT>(vs_bone_min_regs_);
			UINT world_start = static_cast<UINT>(vs_reg_world_start_);

			UINT pal_start, pal_count;
			if (start_reg >= bone_thresh)
			{
				// Write entirely past threshold (partial bone update)
				pal_start = start_reg;
				pal_count = count;
			}
			else if (end_reg > bone_thresh)
			{
				// Write spans world + bone region — palette starts at world_start
				pal_start = (start_reg <= world_start) ? world_start : start_reg;
				pal_count = end_reg - pal_start;
			}
			else
			{
				pal_start = 0;
				pal_count = 0;
			}

			if (pal_count >= min_regs && (pal_count % rpb) == 0)
			{
				int new_bones = static_cast<int>(pal_count) / vs_regs_per_bone_;
				// Only accept full palette writes or partial updates that don't
				// shrink the tracked palette (partial writes refresh a subset).
				if (new_bones >= num_bones_ || pal_start <= world_start)
				{
					bone_start_reg_ = static_cast<int>(pal_start);
					num_bones_ = new_bones;
				}
			}
		}
	}

	void ffp_state::on_set_ps_const_f(UINT start_reg, const float* data, UINT count)
	{
		if (!data || start_reg + count > 32) return;

		std::memcpy(&ps_const_[start_reg * 4], data, count * 4 * sizeof(float));
	}

	void ffp_state::on_set_vertex_shader(IDirect3DVertexShader9* shader)
	{
		if (shader) shader->AddRef();
		if (last_vs_) last_vs_->Release();
		last_vs_ = shader;
		ffp_active_ = false;
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
		if (shader)
		{
			if (auto it = ps_sampler_masks_.find(shader); it != ps_sampler_masks_.end())
				cur_ps_sampler_mask_ = it->second;
			if (auto it = ps_water_surface_.find(shader); it != ps_water_surface_.end())
				cur_ps_is_water_surface_ = it->second;
		}
	}

	void ffp_state::on_create_pixel_shader(IDirect3DPixelShader9* shader, const DWORD* bytecode)
	{
		if (!shader) return;
		ps_sampler_masks_[shader] = parse_ps_sampler_mask(bytecode);

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

	void ffp_state::on_set_texture(UINT stage, IDirect3DBaseTexture9* texture)
	{
		if (stage < 8)
			cur_texture_[stage] = texture;
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
	}

	void ffp_state::on_set_render_target(DWORD index, IDirect3DSurface9* rt)
	{
		if (index != 0) return;
		cur_rt0_ = rt;
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
		cur_decl_num_weights_ = 0;
		cur_decl_blend_weight_off_ = 0;
		cur_decl_blend_weight_type_ = 0;
		cur_decl_blend_indices_off_ = 0;
		cur_decl_blend_indices_type_ = -1;
		cur_decl_pos_off_ = 0;
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
				if (el.Stream == 0)
					cur_decl_pos_off_ = el.Offset;
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
		// Auto-enable/disable: track consecutive frames above/below the eligible draw threshold
		if (cfg_ && cfg_->auto_enable && cfg_->enabled)
		{
			int threshold = cfg_->auto_enable_threshold;
			int required = cfg_->auto_enable_frames;
			bool above = static_cast<int>(ffp_eligible_draws_) >= threshold;

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
					if (auto_consecutive_ >= required)
					{
						enabled_ = true;
						log("FFP", std::format("Auto-enabled ({}+ eligible draws for {} frames)", threshold, required));
					}
				}
			}
			else if (enabled_ && !above)
			{
				auto_consecutive_++;
				if (auto_consecutive_ >= required)
				{
					enabled_ = false;
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
			for (auto it = rigid_overlay_cache_.begin(); it != rigid_overlay_cache_.end(); )
			{
				if (frame_count_ - it->second.last_frame > 600)
				{
					if (it->second.vb) it->second.vb->Release();
					it = rigid_overlay_cache_.erase(it);
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
	}

	void ffp_state::on_reset()
	{
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
		cur_decl_num_weights_ = 0;
		cur_decl_blend_weight_off_ = 0;
		cur_decl_blend_weight_type_ = 0;
		cur_decl_blend_indices_off_ = 0;
		cur_decl_blend_indices_type_ = -1;
		cur_decl_pos_off_ = 0;
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
		cur_ps_is_water_surface_ = false;

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

		// Bind the per-mesh albedo and clear other stages
		dev->SetTexture(0, tex);
		for (DWORD ts = 1; ts < 8; ts++)
			dev->SetTexture(ts, nullptr);
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

		dev->SetTexture(0, chosen);
		for (DWORD ts = 1; ts < 8; ts++)
			dev->SetTexture(ts, nullptr);
		return true;
	}

	// ---- State consumers ----

	void ffp_state::engage(IDirect3DDevice9* dev)
	{
		if (!cfg_ || !enabled_ || !dev) return;

		if (!ffp_active_)
		{
			dev->SetVertexShader(nullptr);
			dev->SetPixelShader(nullptr);
			ffp_active_ = true;
		}

		apply_transforms(dev);
		setup_texture_stages(dev);

		if (!ffp_setup_)
		{
			setup_lighting(dev);
			ffp_setup_ = true;
		}
	}

	void ffp_state::disengage(IDirect3DDevice9* dev)
	{
		bool was_ffp = ffp_active_;

		if (ffp_active_ && dev)
		{
			dev->SetVertexShader(last_vs_);
			dev->SetPixelShader(last_ps_);
			dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
			ffp_active_ = false;
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
	}

	void ffp_state::restore_textures(IDirect3DDevice9* dev)
	{
		if (!dev) return;

		for (DWORD ts = 0; ts < 8; ts++)
			dev->SetTexture(ts, cur_texture_[ts]);
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
		const bool has_override_camera = camera_override_fn_ &&
			camera_override_fn_(override_cam_x, override_cam_y, override_cam_z);

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

			if (inject_view_pos_ && has_override_camera)
			{
				cam2world[12] = override_cam_x;
				cam2world[13] = override_cam_y;
				cam2world[14] = override_cam_z;
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
			float vp[16];
			mat4_transpose(vp, &vs_const_[vs_reg_view_start_ * 4]);
			mat4_mul(cached_proj_, cam2world_rot, vp);

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

		// UV scaling/offset is handled by CPU-side vertex expansion (expand_rigid_texcoords)
		// rather than D3DTS_TEXTURE0 — Remix doesn't support FFP texture coordinate transforms.
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

		const int overlay_stage = secondary_blend_stage();
		if (overlay_stage < 0)
			return false;

		auto* overlay_texture = cur_texture_[overlay_stage];
		if (!overlay_texture)
			return false;

		// Overlay VB with TEXCOORD0.zw as the UV set — its distinct texcoord data
		// gives the draw its own geometry hash (and thus its own BLAS/instance) in
		// Remix, so it cannot clobber the base draw's material.
		if (!expand_rigid_texcoords(dev, base_vtx, min_vtx, num_verts, true))
			return false;

		dev->SetTexture(0, overlay_texture);
		for (DWORD ts = 1; ts < 8; ts++)
			dev->SetTexture(ts, nullptr);

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
		return true;
	}

	bool ffp_state::apply_blended_decal_marker(IDirect3DDevice9* dev)
	{
		if (!dev || !cfg_ || !cfg_->texture_layer_blend)
			return false;

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

		dev->SetTextureStageState(0, D3DTSS_CONSTANT, OVERLAY_DECAL_MARKER);
		return true;
	}

	void ffp_state::clear_decal_marker(IDirect3DDevice9* dev)
	{
		if (dev)
			dev->SetTextureStageState(0, D3DTSS_CONSTANT, 0);
	}

	// ---- Rigid vertex expansion ----

	bool ffp_state::expand_rigid_texcoords(IDirect3DDevice9* dev, INT base_vtx, UINT min_vtx, UINT num_verts, bool overlay_uv)
	{
		if (num_verts == 0) return false;

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

		auto& entry = overlay_uv ? rigid_overlay_cache_[cache_key] : rigid_exp_cache_[cache_key];
		entry.last_frame = frame_count_;

		// Always re-expand from source — never serve cached data.
		// The game can destroy and re-create VBs at the same address (pointer
		// reuse), which makes stale cache hits return garbage vertex data.
		// The cache still provides allocation reuse to avoid create/release churn.

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
			entry.expanded = 0;
		}

		// Lock source VB
		unsigned char* src_data = nullptr;
		if (FAILED(src_vb->Lock(byte_offset, total_verts * stride,
			reinterpret_cast<void**>(&src_data), D3DLOCK_READONLY)))
			return false;

		// Lock destination VB
		unsigned char* dst_data = nullptr;
		if (FAILED(entry.vb->Lock(0, total_verts * rigid_vtx_size,
			reinterpret_cast<void**>(&dst_data), 0)))
		{
			src_vb->Unlock();
			return false;
		}

		constexpr float kScale = 1.0f / 1024.0f;

		int tc_type = cur_decl_texcoord_type_;
		int tc_off  = cur_decl_texcoord_off_;
		int n_off   = cur_decl_normal_off_;
		int n_type  = cur_decl_normal_type_;
		int p_off   = cur_decl_pos_off_;

		const bool rigid_chunk_layout = cur_decl_has_blend_indices_ && !cur_decl_is_skinned_ &&
			cur_decl_blend_indices_type_ == D3DDECLTYPE_UBYTE4 && cur_decl_pos_off_ == 0 &&
			cur_decl_has_normal_ && cur_decl_normal_off_ == 16 && cur_decl_normal_type_ == D3DDECLTYPE_UBYTE4N &&
			cur_decl_has_color_ && cur_decl_color_off_ == 20 && cur_decl_color_type_ == D3DDECLTYPE_UBYTE4N &&
			cur_decl_has_texcoord_ && cur_decl_texcoord_off_ == 24 && cur_decl_texcoord_type_ == D3DDECLTYPE_SHORT2 &&
			stride == 28;
		const bool debris_chunk_size = cur_ib_ && (num_verts == 58 || num_verts == 80);

		bool use_palette_indices = rigid_chunk_layout && debris_chunk_size && vs_regs_per_bone_ > 0;
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
		const bool has_override_camera = use_palette_indices && camera_override_fn_ &&
			camera_override_fn_(override_cam_x, override_cam_y, override_cam_z);

		auto apply_world_offset = [&](float* out)
		{
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
			if (use_palette_indices)
				mat4_invert_rigid(base_world_inv, base_world);
		}

		const int tc_src = overlay_uv ? 2 : 0;

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

		entry.vb->Unlock();
		src_vb->Unlock();
		entry.expanded = total_verts;

		dev->SetVertexDeclaration(rigid_exp_decl_);
		dev->SetStreamSource(0, entry.vb, 0, rigid_vtx_size);
		rigid_exp_active_ = true;
		return true;
	}

	void ffp_state::restore_rigid_source(IDirect3DDevice9* dev)
	{
		if (!rigid_exp_active_ && !skinned_exp_active_) return;

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
		if (!entry.vb || entry.capacity < total_verts)
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
