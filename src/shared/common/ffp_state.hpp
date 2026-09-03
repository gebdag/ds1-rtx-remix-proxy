#pragma once
#include "config.hpp"

namespace shared::common
{
	/*
	 * FFP state tracker — captures D3D9 state needed for fixed-function pipeline conversion.
	 *
	 * The D3D9Device proxy calls on_xxx() methods when the game sets shaders, constants,
	 * textures, vertex declarations, etc. The renderer reads state via accessors to make
	 * draw routing decisions, then calls engage/disengage to switch between shader and FFP modes.
	 */
	class ffp_state
	{
	public:
		static ffp_state& get();

		void init(IDirect3DDevice9* real_device);

		// --- State mutators (called from D3D9Device interceptions) ---

		void on_set_vs_const_f(UINT start_reg, const float* data, UINT count);
		void on_set_ps_const_f(UINT start_reg, const float* data, UINT count);
		void on_set_vertex_shader(IDirect3DVertexShader9* shader);

		void on_set_pixel_shader(IDirect3DPixelShader9* shader);
		// Record the sampler usage of a freshly created pixel shader (parsed from
		// its DCL instructions). Called from the CreatePixelShader intercept.
		void on_create_pixel_shader(IDirect3DPixelShader9* shader, const DWORD* bytecode);

		void on_set_texture(UINT stage, IDirect3DBaseTexture9* texture);
		void on_set_stream_source(UINT stream, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride);
		void on_set_indices(IDirect3DIndexBuffer9* ib);
		void on_set_render_target(DWORD index, IDirect3DSurface9* rt);
		void on_set_vertex_declaration(IDirect3DVertexDeclaration9* decl);
		void on_clear(DWORD flags);
		// Query D3DRS_ZWRITEENABLE directly from the device and cache it.
		// Called at the start of each draw routing function so the prepass gate
		// reflects the true current state even when changed via StateBlock::Apply().
		// Gated by cfg.ffp.zwrite_query_per_draw — when off, the value tracked
		// via on_set_render_state() is used instead (one less call per draw).
		void sync_zwrite(IDirect3DDevice9* dev);
		// Passive render-state tracking (currently only D3DRS_ZWRITEENABLE).
		void on_set_render_state(D3DRENDERSTATETYPE state, DWORD value);
		void on_present();
		void on_begin_scene();
		void on_reset();

		// --- State consumers (called from renderer draw routing) ---

		void engage(IDirect3DDevice9* dev);
		void disengage(IDirect3DDevice9* dev);

		// Bind albedo texture to stage 0, NULL stages 1-7. Call before draw.
		void setup_albedo_texture(IDirect3DDevice9* dev);

		// Restore original texture bindings on all 8 stages. Call after draw.
		void restore_textures(IDirect3DDevice9* dev);

		// --- Read-only accessors for renderer ---

		bool is_enabled() const { return enabled_; }
		void set_enabled(bool e)
		{
			enabled_ = e;
			if (!e) manual_disable_ = true;
			else manual_disable_ = false;
		}

		// Called by renderer for each draw that would go through FFP engage (regardless of enabled_ state).
		void increment_eligible_draws() { ffp_eligible_draws_++; }

		bool view_proj_valid() const { return view_proj_valid_; }
		bool is_ffp_active() const { return ffp_active_; }
		bool cur_decl_is_skinned() const { return cur_decl_is_skinned_; }
		bool cur_decl_has_normal() const { return cur_decl_has_normal_; }
		bool cur_decl_has_pos_t() const { return cur_decl_has_pos_t_; }
		bool cur_decl_has_texcoord() const { return cur_decl_has_texcoord_; }
		bool cur_decl_has_color() const { return cur_decl_has_color_; }
		int cur_decl_texcoord_type() const { return cur_decl_texcoord_type_; }

		IDirect3DVertexShader9* last_vs() const { return last_vs_; }
		IDirect3DPixelShader9* last_ps() const { return last_ps_; }
		IDirect3DVertexDeclaration9* last_decl() const { return last_decl_; }

		// Camera world position extracted from the inverse of the VP matrix (c0-c3).
		void camera_world_pos(float& x, float& y, float& z) const;

		// True while c4-c6 hold an orthonormal basis, i.e. a real camera rather than
		// the 2D/menu transform that shares those registers.
		bool vs_camera_basis_is_orthonormal() const;

		UINT camera_basis_accepted() const { return camera_basis_accepted_; }
		UINT camera_basis_rejected() const { return camera_basis_rejected_; }
		bool have_valid_camera() const { return have_valid_camera_; }

		// Transform a point from view space into world space using c4-c6.
		void view_to_world_pos(float& x, float& y, float& z) const;

		// Camera override: comp layer can provide a real camera position (e.g. from game memory)
		// and control whether it gets injected into View/World transforms.
		using camera_pos_fn = bool(*)(float& x, float& y, float& z);
		void set_camera_override(camera_pos_fn fn) { camera_override_fn_ = fn; }

		// Per-frame flags — set by the comp layer's camera module
		void set_inject_view_pos(bool v) { inject_view_pos_ = v; }
		void set_offset_world(bool v) { offset_world_ = v; }
		bool inject_view_pos() const { return inject_view_pos_; }
		bool offset_world() const { return offset_world_; }

		// --- Diagnostic data access ---

		const float* vs_const_data() const { return vs_const_; }
		const float* ps_const_data() const { return ps_const_; }
		const int* vs_const_write_log() const { return vs_const_write_log_; }
		UINT draw_call_count() const { return draw_call_count_; }
		UINT frame_count() const { return frame_count_; }
		UINT scene_count() const { return scene_count_; }
		DWORD create_tick() const { return create_tick_; }

		// Texture tracking (stages 0-7)
		IDirect3DBaseTexture9* cur_texture(UINT stage) const { return stage < 8 ? cur_texture_[stage] : nullptr; }

		// Stream source tracking
		IDirect3DVertexBuffer9* stream_vb(UINT stream) const { return stream < 4 ? stream_vb_[stream] : nullptr; }
		UINT stream_offset(UINT stream) const { return stream < 4 ? stream_offset_[stream] : 0; }
		UINT stream_stride(UINT stream) const { return stream < 4 ? stream_stride_[stream] : 0; }
		IDirect3DIndexBuffer9* cur_ib() const { return cur_ib_; }

		// VS constant register layout (game-specific, edit defaults when porting)
		int reg_view_start() const { return vs_reg_view_start_; }
		int reg_view_end() const { return vs_reg_view_end_; }
		int reg_proj_start() const { return vs_reg_proj_start_; }
		int reg_proj_end() const { return vs_reg_proj_end_; }
		int reg_world_start() const { return vs_reg_world_start_; }
		int reg_world_end() const { return vs_reg_world_end_; }
		int reg_bone_threshold() const { return vs_reg_bone_threshold_; }
		int regs_per_bone() const { return vs_regs_per_bone_; }
		int bone_min_regs() const { return vs_bone_min_regs_; }

		// Skinning data (populated by skinning module via on_set_vs_const_f bone detection)
		int bone_start_reg() const { return bone_start_reg_; }
		int num_bones() const { return num_bones_; }
		int cur_decl_num_weights() const { return cur_decl_num_weights_; }
		int cur_decl_blend_weight_off() const { return cur_decl_blend_weight_off_; }
		int cur_decl_blend_weight_type() const { return cur_decl_blend_weight_type_; }
		int cur_decl_blend_indices_off() const { return cur_decl_blend_indices_off_; }
		int cur_decl_blend_indices_type() const { return cur_decl_blend_indices_type_; }
		int cur_decl_pos_off() const { return cur_decl_pos_off_; }
		int cur_decl_normal_off() const { return cur_decl_normal_off_; }
		int cur_decl_normal_type() const { return cur_decl_normal_type_; }
		int cur_decl_texcoord_off() const { return cur_decl_texcoord_off_; }

		void increment_draw_count()
		{
			draw_call_count_++;
		}

		// Draw deduplication — record + skip for multi-pass draws.
		// record_draw_key: always count for next-frame learning.
		// check_dedup_skip: returns true if this pass should be skipped.
		//   skinned=false (rigid): uses dedup_pass_mode directly (1=first, 2=last).
		//   skinned=true: always keep first (characters look correct on first pass).
		void record_draw_key(IDirect3DVertexBuffer9* vb, IDirect3DIndexBuffer9* ib,
			INT base_vtx, UINT start_idx, UINT prim_count);
		bool check_dedup_skip(IDirect3DVertexBuffer9* vb, IDirect3DIndexBuffer9* ib,
			INT base_vtx, UINT start_idx, UINT prim_count, bool skinned = false);

		UINT dedup_multi_pass_count() const { return static_cast<UINT>(multi_pass_keys_.size()); }
		UINT dedup_skipped_count() const { return dedup_skipped_; }
		int dedup_max_passes() const { return dedup_max_passes_; }

		// Raw tracked render states (independent of any config gate).
		bool zwrite_enabled() const { return cur_zwrite_enabled_; }
		bool alpha_blend_enabled() const { return cur_alpha_blend_enabled_; }

		// Returns true when the current draw should be suppressed from FFP conversion.
		// DS1 uses a depth-prepass pattern: opaque geometry is drawn twice —
		//   pass 1: Z-write ON  (building the depth buffer, wrong/placeholder textures)
		//   pass 2: Z-write OFF (reading from pre-built depth, correct diffuse textures)
		// Additional effect passes also have Z-write ON (shadows, decals, etc.).
		// FFP conversion only belongs in pass 2 (Z-write OFF), where textures are correct.
		bool is_depth_prepass() const
		{
			return cur_zwrite_enabled_ && cfg_ && cfg_->suppress_aux_passes;
		}

		// Check if the current albedo texture is the globally-shared one that
		// Also records tex0 for global-texture learning.
		bool should_filter_texture();

		// Clear the peak counts (called when toggling tex filter off/on).
		void clear_global_textures();

		// Number of textures currently being filtered at the given threshold.
		UINT tex_filtered_count() const;

		// Set up the albedo texture for an FFP rigid draw.
		// Returns false if the draw should be skipped (NULL or global texture).
		bool setup_albedo_or_fallback(IDirect3DDevice9* dev);

		// True when the currently bound pixel shader is one of DS1's water
		// surface shaders (FRPG_Water_Env*/Reflect*, FRPG_WWS_Dif*).
		bool cur_ps_is_water_surface() const { return cur_ps_is_water_surface_; }

		// Sampler-declaration bitmask of the current pixel shader (bit n = sn).
		uint32_t cur_ps_sampler_mask() const { return cur_ps_sampler_mask_; }

		// Bind an albedo for a water-surface FFP draw: the first non-NULL,
		// non-render-target texture on stages 0-7, else a solid-color fallback
		// created on demand. Always succeeds unless the device is unavailable.
		bool setup_water_albedo(IDirect3DDevice9* dev);

		// Configure texture stage 0 for MODULATE and disable stages 1-7.
		// Call after setup_albedo_or_fallback when the pixel shader is nulled or when
		// clean TSS state is needed for Remix material identification.
		void setup_texture_stages(IDirect3DDevice9* dev);

		// Set up the overlay decal pass for a two-layer draw: expands a second VB
		// with the overlay UV pair (TEXCOORD0.zw) as TEXCOORD0, binds the overlay
		// diffuse on stage 0 with vertex alpha as opacity, enables alpha blending,
		// and tags the draw with the decal marker consumed by the custom RTX Remix
		// runtime (rtx.fixedFunctionDecalMarkerEnabled). Returns true if the caller
		// should re-issue the draw; call clear_decal_marker afterwards.
		bool setup_overlay_decal_pass(IDirect3DDevice9* dev, INT base_vtx, UINT min_vtx, UINT num_verts);

		// Tag the current FFP draw as a decal when the game itself renders it
		// alpha-blended (terrain edge-fade strips blending onto the geometry
		// beneath). Without the tag Remix treats such draws as unordered
		// translucency, producing dark fringes around blended surfaces.
		// Returns true if the marker was applied; call clear_decal_marker after
		// the draw.
		bool apply_blended_decal_marker(IDirect3DDevice9* dev);

		// Clear the decal marker so subsequent draws are not classified as decals
		// by the runtime.
		void clear_decal_marker(IDirect3DDevice9* dev);

		// --- Rigid vertex expansion (CPU-side SHORT→FLOAT UV conversion) ---

		// Expand SHORT2/SHORT4 texcoords to FLOAT2 with UV scale (1/1024) + c246 offset.
		// Creates a temporary VB with FLOAT3 pos + FLOAT3 normal + D3DCOLOR diffuse.
		// With overlay_uv, the second UV pair (TEXCOORD0.zw of a SHORT4/FLOAT4 source)
		// is written as TEXCOORD0 instead, into a separate VB cache — the distinct
		// texcoord data gives Remix a distinct geometry hash for the overlay draw.
		// Returns true if expansion succeeded; caller should draw with BaseVertexIndex=0.
		bool expand_rigid_texcoords(IDirect3DDevice9* dev, INT base_vtx, UINT min_vtx, UINT num_verts, bool overlay_uv = false);

		// Expand texcoords for skinned vertices while preserving the full vertex layout
		// (blend indices, weights, etc.). Only the texcoord element is promoted from
		// SHORT2/SHORT4 to FLOAT2. Returns true if expansion succeeded.
		bool expand_skinned_texcoords(IDirect3DDevice9* dev, INT base_vtx, UINT min_vtx, UINT num_verts);

		// Restore original vertex declaration and stream source after expanded draw.
		void restore_rigid_source(IDirect3DDevice9* dev);

		// --- Utility ---

		static void mat4_transpose(float* dst, const float* src);
		static bool mat4_is_interesting(const float* m);

	private:
		int secondary_blend_stage() const;
		static uint32_t parse_ps_sampler_mask(const DWORD* bytecode);
		static uint32_t hash_shader_bytecode(const DWORD* bytecode);

		bool enabled_ = true;

		// Camera override from comp layer
		camera_pos_fn camera_override_fn_ = nullptr;
		bool inject_view_pos_ = false;
		bool offset_world_ = false;

		// Auto-enable tracking
		UINT ffp_eligible_draws_ = 0;
		int auto_consecutive_ = 0;
		bool manual_disable_ = false; // user toggled off via F4; cleared when auto-disable fires (menu)

		// VS/PS constant capture
		float vs_const_[256 * 4] = {};
		float ps_const_[32 * 4] = {};

		// Dirty tracking
		bool world_dirty_ = false;
		bool view_proj_dirty_ = false;
		bool view_proj_valid_ = false;
		bool have_valid_camera_ = false;
		UINT camera_basis_accepted_ = 0;
		UINT camera_basis_rejected_ = 0;
		bool ffp_active_ = false;
		bool ffp_setup_ = false;

		// Cached FFP transforms — used by FFP engage (cleaned proj) and disengage (raw proj)
		float cached_view_[16] = {};
		float cached_proj_[16] = {};     // cleaned (off-diags zeroed) — used by FFP pipeline
		float cached_proj_raw_[16] = {}; // uncleaned: cam2world × VP — used by disengage for vertex capture
		float cached_world_[16] = {};
		bool transforms_cached_ = false;

		// Shader tracking
		IDirect3DVertexShader9* last_vs_ = nullptr;
		IDirect3DPixelShader9* last_ps_ = nullptr;

		// Sampler usage per pixel shader (bit n = shader declares sampler sn),
		// parsed from DCL instructions at creation. Used to verify a draw actually
		// samples a second diffuse before running the overlay decal pass — DS1
		// leaves stale textures bound on s3+ across material changes, so a bound
		// texture alone is not a reliable signal.
		std::unordered_map<IDirect3DPixelShader9*, uint32_t> ps_sampler_masks_;
		uint32_t cur_ps_sampler_mask_ = 0;

		// Water-surface pixel shaders (FRPG_Water_Env*/Reflect*/WWS_Dif*),
		// identified by FNV-1a bytecode hash at creation. Their draws never
		// survive the standard rigid gates (no NORMAL in the WWS decl, no
		// stage-0 diffuse on Env, reflection RT on stage 0 for Reflect), so the
		// renderer routes them through a dedicated water path.
		std::unordered_map<IDirect3DPixelShader9*, bool> ps_water_surface_;
		bool cur_ps_is_water_surface_ = false;

		// Solid-color fallback albedo for water draws with no stable static
		// texture bound. Gives Remix a constant texture hash to remap.
		IDirect3DTexture9* water_fallback_tex_ = nullptr;

		// Vertex declaration tracking
		IDirect3DVertexDeclaration9* last_decl_ = nullptr;
		bool cur_decl_is_skinned_ = false;
		bool cur_decl_has_blend_indices_ = false;
		bool cur_decl_has_texcoord_ = false;
		bool cur_decl_has_normal_ = false;
		bool cur_decl_has_color_ = false;
		bool cur_decl_has_pos_t_ = false;
		int cur_decl_texcoord_type_ = -1;
		int cur_decl_texcoord_off_ = 0;

		// Skinning-related declaration data
		int cur_decl_num_weights_ = 0;
		int cur_decl_blend_weight_off_ = 0;
		int cur_decl_blend_weight_type_ = 0;
		int cur_decl_blend_indices_off_ = 0;
		int cur_decl_blend_indices_type_ = -1;
		int cur_decl_pos_off_ = 0;
		int cur_decl_normal_off_ = 0;
		int cur_decl_normal_type_ = -1;
		int cur_decl_color_off_ = 0;
		int cur_decl_color_type_ = -1;

		// VS constant register layout — Dark Souls PTDE
		// c0-c3: ViewProj combined (column-major). No separate View/Proj upload.
		// c4-c6: camera rotation (.xyz) + world position (.w)
		// c8-c10: World/Bone matrix (4x3 column-major, 3 regs)
		// Proj range == View range so a single c0-c6 write marks both valid.
		int vs_reg_view_start_ = 0;
		int vs_reg_view_end_ = 4;
		int vs_reg_proj_start_ = 0;    // same as view — VP is combined
		int vs_reg_proj_end_ = 4;
		int vs_reg_world_start_ = 8;
		int vs_reg_world_end_ = 11;
		int vs_reg_bone_threshold_ = 11;
		int vs_regs_per_bone_ = 3;
		int vs_bone_min_regs_ = 3;

		// Bone detection
		int bone_start_reg_ = 0;
		int num_bones_ = 0;

		// Texture tracking
		IDirect3DBaseTexture9* cur_texture_[8] = {};

		// Stream source tracking
		IDirect3DVertexBuffer9* stream_vb_[4] = {};
		UINT stream_offset_[4] = {};
		UINT stream_stride_[4] = {};
		IDirect3DIndexBuffer9* cur_ib_ = nullptr;

		// Frame/draw counters
		UINT frame_count_ = 0;
		UINT draw_call_count_ = 0;
		UINT scene_count_ = 0;
		DWORD create_tick_ = 0;

		// VS constant write log (for diagnostics: which registers have been written)
		int vs_const_write_log_[256] = {};

		// Rigid vertex expansion — per-draw MANAGED VB cache
		// Each unique (source_vb, base_offset) gets its own MANAGED VB that persists
		// across frames, giving Remix stable vertex data for hashing.
		static constexpr UINT RIGID_VTX_SIZE = 36; // FLOAT3 pos + FLOAT3 normal + D3DCOLOR diffuse + FLOAT2 uv
		// Decal tag for overlay draws — matched by the custom RTX Remix runtime
		// (rtx.fixedFunctionDecalMarkerEnabled) in the stage-0 D3DTSS_CONSTANT.
		static constexpr DWORD OVERLAY_DECAL_MARKER = 0xD15DECA1;
		IDirect3DVertexDeclaration9* rigid_exp_decl_ = nullptr;
		bool rigid_exp_active_ = false;

		struct rigid_vb_entry {
			IDirect3DVertexBuffer9* vb = nullptr;
			UINT capacity = 0;      // allocated size in vertices
			UINT expanded = 0;      // vertices with valid data
			UINT last_frame = 0;
		};
		std::unordered_map<uint64_t, rigid_vb_entry> rigid_exp_cache_;
		std::unordered_map<uint64_t, rigid_vb_entry> rigid_overlay_cache_;

		// Skinned vertex expansion — same cache structure but preserves full vertex layout.
		// Key: (tc_type << 48) | (stride << 32) | (src_vb ^ byte_offset) to differentiate
		// from rigid entries and handle multiple skinned declaration variants.
		IDirect3DVertexDeclaration9* skinned_exp_decl_ = nullptr;
		int skinned_exp_tc_type_ = -1;     // D3DDECLTYPE of last skinned expansion
		int skinned_exp_stride_ = 0;        // stride of last skinned decl source
		bool skinned_exp_active_ = false;
		UINT skinned_exp_vtx_size_ = 0;     // expanded vertex size (stride with FLOAT2 tc)
		std::unordered_map<uint64_t, rigid_vb_entry> skinned_exp_cache_;

		// Draw dedup — keyed on (VB, IB, BaseVertex, startIndex, primCount).
		// Uses frame learning: tracks which geometry keys are multi-pass (drawn 2+
		// times per frame), then on subsequent frames skips passes based on
		// dedup_pass_mode: 1=keep first, 2=keep last, 3=keep all.

		// Render target tracking.
		IDirect3DSurface9* cur_rt0_ = nullptr;

		// Z-write state: D3DRS_ZWRITEENABLE tracked via on_set_render_state().
		// TRUE  = depth-building pass (prepass/shadow) — suppress FFP.
		// FALSE = depth-reading pass (main render with pre-built depth) — allow FFP.
		bool cur_zwrite_enabled_ = true;
		// Alpha-blend state tracked the same passive way (perf classification).
		bool cur_alpha_blend_enabled_ = false;
		struct draw_key {
			uintptr_t vb;
			uintptr_t ib;
			INT base_vtx;
			UINT start_idx;
			UINT prim_count;
			bool operator==(const draw_key&) const = default;
		};
		struct draw_key_hash {
			size_t operator()(const draw_key& k) const {
				size_t h = std::hash<uintptr_t>{}(k.vb);
				h ^= std::hash<uintptr_t>{}(k.ib) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<INT>{}(k.base_vtx) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<UINT>{}(k.start_idx) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<UINT>{}(k.prim_count) + 0x9e3779b9 + (h << 6) + (h >> 2);
				return h;
			}
		};

		// Current frame: count appearances of each draw key
		std::unordered_map<draw_key, int, draw_key_hash> draw_key_counts_;
		// Learned from previous frame: keys that appeared more than once, with total pass count
		std::unordered_map<draw_key, int, draw_key_hash> multi_pass_keys_;
		// Current frame: per-key occurrence index (0-based, incremented each time seen)
		std::unordered_map<draw_key, int, draw_key_hash> seen_this_frame_;
		UINT dedup_skipped_ = 0;
		int dedup_log_budget_ = 20;
		int dedup_max_passes_ = 0;

		// Global texture filter: skip textures that appear on many draws per frame.
		// tex0_counts_: per-frame draw counts (cleared each frame).
		// tex0_peak_: highest single-frame count ever observed per texture.
		// Filtering: peak >= threshold. Slider changes take effect immediately.
		std::unordered_map<uintptr_t, UINT> tex0_counts_;
		std::unordered_map<uintptr_t, UINT> tex0_peak_;

		// Cached config
		const config::ffp_settings* cfg_ = nullptr;

		// Internal helpers
		void apply_transforms(IDirect3DDevice9* dev);
		void setup_lighting(IDirect3DDevice9* dev);
	};
}
