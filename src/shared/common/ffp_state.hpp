#pragma once
#include "config.hpp"

#include <array>

namespace shared::common
{
	// What one of DS1's particle pixel shaders (FRPG_Sfx_*) computes, which decides
	// how its draws reach Remix. Generated per shader into sfx_shaders.inc by
	// scripts/gen_sfx_shader_table.py; the reasoning is in the script.
	enum class sfx_class : uint8_t
	{
		None,        // not a particle shader
		Sprite,      // rgb = tex * vcolor, a = tex.a * vcolor.a (fog, scatter, exposure on top)
		SoftSprite,  // Sprite, alpha also faded by the scene depth over the per-vertex feather
		LitSprite,   // normal-mapped lighting * tex * vcolor (smoke)
		DepthEncode, // writes the particle's depth encoded into rgb, no colour of its own
		Distortion,  // warps a copy of the frame (heat haze, refraction)
		Blur,        // screen-space depth copy / bright pass / blurs
	};

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
		// Record which constant registers a vertex shader takes its view-projection
		// from (the particle sprite path needs it). Called from CreateVertexShader.
		void on_create_vertex_shader(IDirect3DVertexShader9* shader, const DWORD* bytecode);

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
		// Invalidate the cached "FFP texture stage state is applied" latch used
		// by [FFPSteps] TextureStages=1. Called from the device wrapper, which
		// only ever sees the GAME's calls — the proxy talks to the real device
		// directly, so its own stage writes cannot falsely invalidate this.
		void on_set_texture_stage_state(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value);
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

		// DS1 particle sprites (FRPG_Sfx_* sprite shaders) are UP draws of world-space
		// quads the CPU rebuilds every frame, transformed by an ABSOLUTE view-projection
		// in their vertex shader (c8-c11, c12-c15 for point sprites), while map pieces use
		// a camera-relative one (c0-c3) plus a world matrix. This draws such a sprite
		// fixed-function in the converted geometry's space: World = translation from the
		// particle camera to the space ffp gives map pieces, vertices repacked to
		// position + colour + uv, stage 0 = texture x vertex colour (the sprite
		// shaders' own colour and alpha). Returns false, drawing nothing, when the draw
		// cannot be placed that way (unknown vertex shader, a different camera such as the
		// water reflection pass, no map camera yet, unusable vertex layout).
		// unlit: the sprite shader is unlit (Sprite / SoftSprite: colour = texture x vertex
		// colour); the draw then carries the unlit marker for the runtime
		// (rtx.fixedFunctionUnlitMarkerEnabled) and the masks the shader multiplies in
		// on samplers 1 and 3 (the "Multi" families) are folded into the vertex colour as
		// their average, since Remix only takes stage 0.
		bool draw_sprite_up(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, UINT prim_count,
			UINT min_index, UINT num_verts, const void* indices, D3DFORMAT index_format,
			const void* vertices, UINT stride, bool unlit, HRESULT& hr);

		// Particle shaders upload their own data into c0-c3 and c8-c11 (the camera and
		// world registers of map pieces) while c4-c6 still hold a valid camera basis, so
		// engage/disengage during a particle draw would decompose that data as the camera
		// and world and cache it for every following draw. This runs fn with the cached
		// transforms re-applied as they are, then restores the dirty flags so the next
		// map draw still picks up the constants it uploads.
		template <class Fn>
		void with_cached_transforms(Fn&& fn)
		{
			const bool vp_dirty = view_proj_dirty_, w_dirty = world_dirty_;
			view_proj_dirty_ = false;
			world_dirty_ = false;
			fn();
			view_proj_dirty_ = vp_dirty;
			world_dirty_ = w_dirty;
		}

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

		// The translation apply_transforms adds to every World matrix to turn the
		// main pass's camera-relative matrices into absolute world space. False
		// (and x/y/z untouched) when no offset is being applied.
		bool world_offset(float& x, float& y, float& z) const;

		// World-space right and forward axes of the last main camera handed to Remix
		// (from the cached View matrix). False before any camera has been seen.
		bool view_axes(float* right, float* forward) const;

		// --- Diagnostic data access ---

		const float* vs_const_data() const { return vs_const_; }
		const float* ps_const_data() const { return ps_const_; }

		// Mean RGB of each face of a cube texture's top level (+X, -X, +Y, -Y, +Z, -Z), cached
		// per texture. 8888 cubes only (DS1's irradiance probes); false for anything else.
		bool cube_face_averages(IDirect3DBaseTexture9* texture, std::array<std::array<float, 3>, 6>& faces);

		// The texture bound on stage 11: the irradiance probe of DS1's HemEnv shaders.
		IDirect3DBaseTexture9* probe_texture() const { return probe_texture_; }
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

		// DS1's FRPG_Snow passes (the Blighttown swamp), by pixel shader: the surface, the height
		// field it reads, and the wake rings stamped into that field (comp/game/swamp.hpp).
		enum class swamp_pass : uint8_t { None, Surface, HeightMap, WaveRing };
		swamp_pass cur_ps_swamp_pass() const { return cur_ps_swamp_pass_; }

		// Sampler-declaration bitmask of the current pixel shader (bit n = sn).
		uint32_t cur_ps_sampler_mask() const { return cur_ps_sampler_mask_; }

		// Particle class of the current pixel shader (sfx_class::None for all others).
		sfx_class cur_ps_sfx_class() const { return cur_ps_sfx_class_; }

		// Which of DS1's light constants the current pixel shader reads, parsed from
		// its instruction stream at creation:
		//   PS_LIGHT_DIR3   c92 and c95, directional light 0 direction and colour.
		//                   Every FRPG_Phn/Gst/Snow *HemDir3 variant; HemEnv lights
		//                   from an irradiance cube instead and reads neither.
		//   PS_LIGHT_SHADOW c175, the shadow light's direction of travel (ShadowBank
		//                   lightDegRotX/Y). Every shadowed variant (Sdw/Csd), HemEnv
		//                   included.
		static constexpr uint32_t PS_LIGHT_DIR3 = 1u << 0;
		static constexpr uint32_t PS_LIGHT_SHADOW = 1u << 1;
		//   PS_HAZE         c104 and c110, the fog/scattering tail every map and
		//                   character shader ends with (see game/haze.hpp).
		static constexpr uint32_t PS_HAZE = 1u << 2;
		//   PS_GRADE        c56 and c62, FRPG_Fil_HDR_ColAdj's grade (see game/atmosphere.hpp).
		static constexpr uint32_t PS_GRADE = 1u << 3;
		//   PS_AMBIENT      c86 and c98, the irradiance probe multiplier and hemisphere ambient
		//                   of the HemEnv shaders (see game/atmosphere.hpp).
		static constexpr uint32_t PS_AMBIENT = 1u << 4;
		//   PS_VIEW_FADE    c176, FRPG_NtoA's alpha fade by view angle (mist and light-shaft layers).
		static constexpr uint32_t PS_VIEW_FADE = 1u << 5;
		uint32_t cur_ps_light_flags() const { return cur_ps_light_flags_; }

		// True when the current pixel shader reads c139, the per-draw colour and
		// fade every FRPG material shader multiplies its output by.
		bool cur_ps_reads_draw_tint() const { return cur_ps_draw_tint_; }

		// For a converted draw whose pixel shader reads c139 and whose c139 is not
		// (1,1,1,1): put c139 in D3DRS_TEXTUREFACTOR and enable stage 1 as
		// MODULATE(CURRENT, TFACTOR) for colour and alpha. Returns true when it did;
		// pair with clear_draw_tint after the draw(s). Call after engage().
		bool apply_draw_tint(IDirect3DDevice9* dev);
		void clear_draw_tint(IDirect3DDevice9* dev);

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

		// True when the current draw is a two-layer material that will actually
		// run the overlay pass. Pure state inspection — no device calls — so the
		// caller can decide whether saving the overlay render states is worth it
		// ([FFPSteps] OverlaySaveLazy). Deliberately a conservative superset of
		// what setup_overlay_decal_pass modifies: it may still bail afterwards on
		// a failed expansion, which costs a redundant save but never a lost state.
		bool overlay_pass_applicable() const
		{
			const int stage = secondary_blend_stage();
			return stage >= 0 && cur_texture_[stage] != nullptr;
		}

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
		static uint32_t parse_ps_light_flags(const DWORD* bytecode);
		static bool ps_reads_float_const(const DWORD* bytecode, DWORD reg);
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
		float ps_const_[256 * 4] = {};

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

		// FRPG_Snow passes, by FNV-1a bytecode hash at creation (see cur_ps_swamp_pass).
		std::unordered_map<IDirect3DPixelShader9*, swamp_pass> ps_swamp_pass_;
		swamp_pass cur_ps_swamp_pass_ = swamp_pass::None;

		// Light constants each pixel shader reads (see cur_ps_light_flags).
		std::unordered_map<IDirect3DPixelShader9*, uint32_t> ps_light_flags_;
		uint32_t cur_ps_light_flags_ = 0;

		// First register of each vertex shader's view-projection (see on_create_vertex_shader).
		std::unordered_map<IDirect3DVertexShader9*, int> vs_viewproj_base_;
		int cur_vs_viewproj_base_ = -1;
		// Vertex shaders that scale the sprite colour by c159.z / c159.w (the game's exposure).
		std::unordered_map<IDirect3DVertexShader9*, bool> vs_exposure_;
		bool cur_vs_exposure_ = false;
		// The map camera's c0-c3 rows as of the last View/Proj decomposition, and the
		// sprite path's repacked vertices.
		float map_viewproj_rows_[16] = {};
		bool have_map_viewproj_ = false;
		std::vector<unsigned char> sprite_vertices_;
		// Average colour (rgba, 0-1) of textures used as sprite masks, from their smallest mip.
		std::unordered_map<IDirect3DBaseTexture9*, std::array<float, 4>> texture_average_;
		std::array<float, 4> texture_average(IDirect3DBaseTexture9* texture);


		// Pixel shaders that read c139 (see cur_ps_reads_draw_tint).
		std::unordered_map<IDirect3DPixelShader9*, bool> ps_draw_tint_;
		bool cur_ps_draw_tint_ = false;
		// The game's own D3DRS_TEXTUREFACTOR, restored by clear_draw_tint.
		DWORD game_tfactor_ = 0xFFFFFFFF;

		// Particle class of each pixel shader (see cur_ps_sfx_class).
		std::unordered_map<IDirect3DPixelShader9*, sfx_class> ps_sfx_class_;
		sfx_class cur_ps_sfx_class_ = sfx_class::None;
		// Its colour scale before fog (sfx_shaders.inc) and whether it applies the exposure c135.
		std::unordered_map<IDirect3DPixelShader9*, float> ps_sfx_colour_scale_;
		float cur_ps_sfx_colour_scale_ = 1.0f;
		std::unordered_map<IDirect3DPixelShader9*, bool> ps_reads_exposure_;
		bool cur_ps_reads_exposure_ = false;

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
		int cur_decl_pos_type_ = -1;
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
		IDirect3DBaseTexture9* probe_texture_ = nullptr;
		std::unordered_map<IDirect3DBaseTexture9*, std::array<std::array<float, 3>, 6>> cube_faces_;

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
			// Reuse validation ([FFPSteps] ExpandReuse). The stock path never
			// serves cached data because a VB can be destroyed and re-created at
			// the same address; these fields let a hit be validated against the
			// source bytes instead of trusting the pointer.
			uint64_t fingerprint = 0;   // sampled hash of the source range
			UINT     src_verts = 0;     // vertex count the fingerprint covers
			UINT     src_stride = 0;    // source stride at expansion time
			bool     has_fingerprint = false;
		};

		// Cheap fingerprint of everything a rigid expansion reads: evenly spaced
		// 8-byte samples of the source range, its dimensions, and the current
		// declaration's layout. Reads ~128 bytes instead of the whole range, so it
		// is far cheaper than re-converting, while still catching a buffer that was
		// recreated at the same address or drawn through a different declaration.
		uint64_t fingerprint_source(const unsigned char* src, UINT total_verts,
			UINT stride, bool overlay_uv) const;

		// True when the FFP texture stage state set by setup_texture_stages is
		// still resident, so TextureStages=1 can skip re-applying it.
		bool ffp_stages_applied_ = false;
		// True when this draw unbound stages 1-7. RestoreTextures=1 must restore
		// them if and only if something actually nulled them — the albedo path
		// does so only under AlbedoNullStages, but the overlay pass always does.
		bool nulled_high_stages_ = false;
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
		// [FFPSteps] switches. A sibling of ffp_settings in config, so it needs
		// its own pointer rather than hanging off cfg_.
		const config::ffp_step_settings* steps_ = nullptr;

		// Internal helpers
		void apply_transforms(IDirect3DDevice9* dev);
		void setup_lighting(IDirect3DDevice9* dev);
	};
}
