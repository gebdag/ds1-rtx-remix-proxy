#pragma once
#include <shared_mutex>
#include <unordered_set>
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
		void release_menu_native();

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
		// Whether a map is loaded: FFP conversion stays on, whatever the frame draws (on_present).
		using map_loaded_fn = bool(*)();
		void set_map_loaded(map_loaded_fn fn) { map_loaded_fn_ = fn; }

		// Sprite observer: the comp layer sees where effects are drawn. Called with every sprite
		// batch draw_sprite_up draws: one point per triangle it draws, the triangle's centre in the
		// game's world space (the space of MSB positions and the light manager) and the strongest
		// vertex alpha of the three. Only the vertices the draw's indices reach: the game's batches
		// declare more, and the rest hold stale positions of earlier sprites.
		// One drawn particle triangle: its centre (world), strongest vertex alpha, the colour it adds
		// per unit area where it is additive (vertex colour * alpha * the texture's mean colour and
		// alpha), its area and whether it is drawn additively (SrcAlpha + One).
		struct sprite_point { float x, y, z, alpha; float r, g, b; float area; bool additive; };
		using sprite_observer_fn = void(*)(const sprite_point* points, UINT count);
		void set_sprite_observer(sprite_observer_fn fn) { sprite_observer_fn_ = fn; }
		// The bound indexed draw's area-weighted centre in world space (rigid draws: VS c8 rows and the world offset), from up to
		// 64 of its triangles. False when its buffers cannot be read. Call with the game's state, before the rigid expansion
		// replaces stream 0.
		bool draw_world_centre(D3DPRIMITIVETYPE prim, INT base_vertex, UINT start_index, UINT prim_count, float out[3]);

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
		// A sprite batch, to whoever would take it over instead of having it drawn (the fog gates).
		// What becomes of a sprite batch: as usual, taken (not drawn), or drawn with a marker for stage 4's D3DTSS_CONSTANT (returned, anything above 1).
		using sprite_batch_fn = DWORD(*)(IDirect3DBaseTexture9* texture, const void* vertices, UINT stride, UINT position_offset, UINT colour_offset, UINT count);
		void set_sprite_batch_taker(sprite_batch_fn fn) { sprite_batch_fn_ = fn; }
		// Cube textures the device created, by address: the game reaches its probes through handles of its own.
		void note_cube(IDirect3DBaseTexture9* cube)
		{
			std::unique_lock lock(known_cubes_mutex_);
			known_cubes_.insert(cube);
			known_cube_vtable_ = *reinterpret_cast<const uint32_t*>(cube);
			// A new cube at a freed one's address does not have its faces. Textures can be created off the render thread.
			cube_faces_.erase(cube);
		}
		// What a live cube's first word holds. The registry keeps cubes the game has since freed: a destroyed one holds its base class's table,
		// and a call through that is a pure call, which ends the process past any __except.
		uint32_t known_cube_vtable() const
		{
			std::shared_lock lock(known_cubes_mutex_);
			return known_cube_vtable_;
		}
		IDirect3DBaseTexture9* known_cube(uint32_t address) const
		{
			auto* cube = reinterpret_cast<IDirect3DBaseTexture9*>(static_cast<uintptr_t>(address));
			std::shared_lock lock(known_cubes_mutex_);
			return known_cubes_.contains(cube) ? cube : nullptr;
		}
		// A new map's cubes can sit at an old one's addresses.
		void forget_cube_faces()
		{
			std::unique_lock lock(known_cubes_mutex_);
			cube_faces_.clear();
		}

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
		// The vertex colour of a vertex in the current declaration as D3DCOLOR (white without one).
		DWORD cur_vertex_color(const unsigned char* vertex) const;

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
		bool cur_rt_is_menu_scene() const { return cur_rt_menu_scene_; }
		// proxy.MenuScene.PathTrace: this character view is converted for Remix, which writes its frame into the target.
		bool cur_rt_is_menu_path_traced() const { return cur_rt_menu_path_traced_; }
		// The menu's quad as it reaches the screen sizes the view's private target (the game's stays 512): square, its
		// larger side in pixels. The view's draws and the quad use that target in place of the game's.
		void note_menu_quad(IDirect3DDevice9* dev, const void* vertices, UINT stride, UINT count);
		// How much smaller the UI is drawn than its full-screen size (1 at 16:9).
		void set_ui_fit(float x, float y) { ui_fit_x_ = x; ui_fit_y_ = y; }
		IDirect3DSurface9* menu_native_surface() const { return cur_rt_menu_path_traced_ ? menu_native_surface_ : nullptr; }
		IDirect3DBaseTexture9* menu_native_texture_for(IDirect3DBaseTexture9* texture) const
		{
			return texture && (texture == menu_path_trace_target_ || menu_cell_of(texture) >= 0) && menu_is_path_traced() ? menu_native_texture_ : nullptr;
		}
		// Load game's small views share one camera, and Remix traces one frame: they stand in a row in one wide frame,
		// seen from far off so that the row keeps their view, and each quad shows its slice. -1: not one of them.
		int menu_cell_of(IDirect3DBaseTexture9* texture) const
		{
			for (int i = 0; i < menu_cell_count_; i++)
				if (texture && menu_cells_[i] == texture)
					return i;
			return -1;
		}
		bool menu_row_active() const { return menu_cell_count_ > 0 && menu_is_path_traced(); }
		void set_menu_row_distance(float distance);
		void turn_menu_cell(float* world) const;
		// The slice of the frame that a view's quad shows. False until the row's camera is known.
		bool menu_cell_uv(int cell, float& u0, float& v0, float& du, float& dv) const;
		bool menu_is_path_traced() const
		{
			return menu_path_trace_target_ && shared::common::config::get().ffp.menu_scene_path_trace;
		}
		// The menu's quad is showing that view.
		bool cur_texture_is_menu_path_traced() const
		{
			return menu_path_trace_target_ && (cur_texture_[0] == menu_path_trace_target_ || menu_cell_of(cur_texture_[0]) >= 0) &&
				!cur_rt_menu_scene_ && shared::common::config::get().ffp.menu_scene_path_trace;
		}

		bool is_depth_prepass() const
		{
			// A menu view writes Z in both of its passes; the depth pass's shader reads no light constants.
			if (cur_rt_menu_path_traced_)
				return !(cur_ps_light_flags_ & PS_AMBIENT);
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

		// True when the bound pixel shader is FRPG_Water_HeightMap: the pass that draws a water
		// mesh's wave height field from its scrolling layers (game/texture_scroll.hpp).
		bool cur_ps_is_water_heightmap() const { return cur_ps_is_water_heightmap_; }

		// Which UV scroll offsets the bound vertex shader reads: VS_TEX_SCROLL_0 = c246 (the
		// base uv, TEXCOORD0.xy / 1024 + c246.xy), VS_TEX_SCROLL_1 = c247 (the overlay uv,
		// TEXCOORD0.zw / 1024 + c247.xy). The game sets them from the material's g_TexScroll.
		static constexpr uint32_t VS_TEX_SCROLL_0 = 1u << 0;
		static constexpr uint32_t VS_TEX_SCROLL_1 = 1u << 1;
		uint32_t cur_vs_tex_scroll() const { return cur_vs_tex_scroll_; }

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
		//   PS_LIGHT_SHAFT  c69 and c70, FRPG_Fil_LightShaft's radial blur (see game/atmosphere.hpp).
		static constexpr uint32_t PS_LIGHT_SHAFT = 1u << 6;
		//   PS_UNLIT        a map or character shader with no light term (the FRPG_*_Non family
		//                   without a lightmap): texture * c100 * vertex colour * c139, then the
		//                   haze tail. c100 is its glow (Bed of Chaos's glyph shells: 10). See
		//                   game/atmosphere.hpp tag_self_lit.
		static constexpr uint32_t PS_UNLIT = 1u << 7;

		// A rigid mesh lit by three directional lights and a hemisphere with no probe (FRPG_Phn_*_HemDir3*). Among
		// blended draws, every one in 177 captures is an effect model (s####), never a surface of the map.
		bool cur_is_lit_effect_mesh() const
		{
			return (cur_ps_light_flags_ & PS_LIGHT_DIR3) && !(cur_ps_light_flags_ & PS_AMBIENT) && !cur_decl_is_skinned();
		}
		//   PS_SP_EFFECT    c136, c137 and c138: the FRPG_Gst_* family the game switches a character to while a
		//                   SpEffect colours it (phantoms, Iron Flesh). It adds a colour by view angle, from c136
		//                   (rim) to c137 (facing the camera) times c138.x, and overrides c101 / c102.
		static constexpr uint32_t PS_SP_EFFECT = 1u << 8;
		//   PS_SPEC_SCALE   c101, the specular map scale.
		static constexpr uint32_t PS_SPEC_SCALE = 1u << 9;
		uint32_t cur_ps_light_flags() const { return cur_ps_light_flags_; }

		// The main camera's camera-relative view-projection as the map shaders get it (VS c0..c3,
		// one register per row), nullptr before one was seen.
		const float* map_viewproj_rows() const { return have_map_viewproj_ ? map_viewproj_rows_ : nullptr; }

		// True when the current pixel shader reads c139, the per-draw colour and
		// fade every FRPG material shader multiplies its output by.
		bool cur_ps_reads_draw_tint() const { return cur_ps_draw_tint_; }

		// True when the current pixel shader reads the game's point lights (c112-c119: four lights'
		// camera-relative positions and colours, the *PntS* variants). Other shaders leave those
		// registers holding the last draw's lights.
		bool cur_ps_reads_point_lights() const { return cur_ps_point_lights_ > 0; }
		// How many of those lights it reads (c112+i / c116+i for i below it): the *PntS* map shaders read
		// one, and the registers past it still hold an earlier draw's lights.
		int cur_ps_point_light_count() const { return cur_ps_point_lights_; }

		// For a converted draw whose pixel shader reads c139 and whose c139 is not
		// (1,1,1,1): put c139 in D3DRS_TEXTUREFACTOR and enable stage 1 as
		// MODULATE(CURRENT, TFACTOR) for colour and alpha. Returns true when it did;
		// pair with clear_draw_tint after the draw(s). Call after engage().
		bool apply_draw_tint(IDirect3DDevice9* dev);
		void clear_draw_tint(IDirect3DDevice9* dev);

		// The character creator's skin tone (PS c156), which DS1's character shaders add to a
		// diffuse texture as stored: the base layer's, or for the two-layer "Mul" shaders (the
		// FaceGen face) the second layer's, lerp(s0, s3 + c156, vertex alpha). For the layer
		// being drawn (overlay = the second), puts c156 in stage 5's D3DTSS_CONSTANT as
		// 0xD3rrggbb, signed bytes in 1/255 (the creator's steps), for the runtime
		// (Surface::ds1AlbedoOffset). Returns true when it did; pair with clear_skin_tone.
		bool apply_skin_tone(IDirect3DDevice9* dev, bool overlay);
		// A PS_SP_EFFECT draw's colouring for the runtime (Surface::ds1Effect): stage 4's texture transform,
		// D3DTTFF_COUNT3, rows 1-2 = the rim and facing colours (c136 / c137 rgb * w * c138.x), _31 = the
		// specular scale over the material's own (c101 over what that specular map gets without the effect,
		// remembered from such draws; 1.5 when none was seen), _32 = the Phong exponent (c102.x). Returns
		// whether it set one, to be cleared after the draw. On other draws it remembers the specular scale.
		bool apply_sp_effect(IDirect3DDevice9* dev);
		void clear_sp_effect(IDirect3DDevice9* dev);

		// A skinned fur shell (the giant rats' fur, c1200-c1203): a bumped _Alp draw (blended over INVSRCALPHA,
		// alpha-tested, pixel shader samples s2) whose diffuse is DXT1 with punch-through texels, not fading out
		// (c139.w = 1). The game blends each shell by texture alpha x vertex alpha and darkens it towards the
		// roots by the vertex colour, so the shells are expanded with it (skinning's vertex_colour layout).
		// Without it each shell covered at its texture's 1-bit alpha: an opaque layer of the shell texture, its
		// holes showing the dark roots below. As a cutout (a hair card) only the high-alpha patches survived, as
		// hard-edged solid tufts. Lace, veils and wing membranes, the other bumped _Alp draws, have graded alpha.
		bool cur_draw_is_fur_shell(IDirect3DDevice9* dev);
		// Stage 0 = texture x vertex colour, for colour and alpha (a fur shell's expanded vertex colour).
		void apply_vertex_colour_stage(IDirect3DDevice9* dev);
		// Stage 0 shows the texture alone: the draw's vertex colour holds data, not a colour.
		void apply_texture_only_stage(IDirect3DDevice9* dev);
		// A new texture can reuse a freed one's address: forget what was learnt about it.
		void on_texture_created(IDirect3DBaseTexture9* texture);
		void clear_skin_tone(IDirect3DDevice9* dev);

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

		// A two-layer draw's second diffuse (null for single-layer draws), for callers that
		// expand the overlay vertices themselves (skinning).
		IDirect3DBaseTexture9* overlay_texture() const
		{
			const int stage = secondary_blend_stage();
			return stage >= 0 ? cur_texture_[stage] : nullptr;
		}

		// The overlay pass's texture, stage and blend state (see setup_overlay_decal_pass),
		// for a draw whose bound vertices already carry the overlay uv as TEXCOORD0 and the
		// vertex colour.
		void apply_overlay_decal_states(IDirect3DDevice9* dev, IDirect3DBaseTexture9* texture);

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

		// The glass pass of a window glass draw (game/glass.hpp), after the came pass (whose
		// stage-0 state it keeps): the vertices 2 mm off the came (their own Remix geometry, so the
		// two never merge or z-fight), the derived glass texture on stage 0 (its came is alpha 0)
		// and straight alpha blending, so without a replacement material only the glass shows,
		// blended as the game does.
		// No decal marker: the glass is a surface of its own. Returns true if the caller should
		// re-issue the draw; the caller saves and restores the render states it changes.
		bool setup_glass_pass(IDirect3DDevice9* dev, INT base_vtx, UINT min_vtx, UINT num_verts,
			IDirect3DBaseTexture9* glass_texture);

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
		// Alpha-blended unlit meshes (PS_UNLIT) take the sprites' unlit marker on stage 0's constant;
		// true when set. Cleared with clear_decal_marker.
		bool apply_unlit_layer_marker(IDirect3DDevice9* dev, bool light_shaft = false);

		// --- Rigid vertex expansion (CPU-side SHORT→FLOAT UV conversion) ---

		// Expand SHORT2/SHORT4 texcoords to FLOAT2 with UV scale (1/1024) + c246 offset.
		// Creates a temporary VB with FLOAT3 pos + FLOAT3 normal + D3DCOLOR diffuse.
		// With rigid_layer::overlay, the second UV pair (TEXCOORD0.zw of a SHORT4/FLOAT4 source)
		// is written as TEXCOORD0 instead, into a separate VB cache — the distinct
		// texcoord data gives Remix a distinct geometry hash for the overlay draw.
		// rigid_layer::glass moves the positions kGlassOffset along -normal, into a cache of
		// its own, for the same reason.
		// Returns true if expansion succeeded; caller should draw with BaseVertexIndex=0.
		enum class rigid_layer : uint8_t { base, overlay, glass };
		bool expand_rigid_texcoords(IDirect3DDevice9* dev, INT base_vtx, UINT min_vtx, UINT num_verts,
			rigid_layer layer = rigid_layer::base);

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
		static bool reads_float_const(const DWORD* bytecode, DWORD reg);
		static uint32_t hash_shader_bytecode(const DWORD* bytecode);

		bool enabled_ = true;

		// Camera override from comp layer
		camera_pos_fn camera_override_fn_ = nullptr;
		map_loaded_fn map_loaded_fn_ = nullptr;
		sprite_observer_fn sprite_observer_fn_ = nullptr;
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
		bool cur_rt_menu_scene_ = false;
		bool cur_rt_menu_path_traced_ = false;
		bool menu_cell_offset(float& x, float& y, float& z) const;
		IDirect3DBaseTexture9* menu_scene_targets_[8] = {};
		UINT menu_scene_widths_[8] = {};
		int menu_scene_count_ = 0;
		IDirect3DBaseTexture9* menu_shown_[8] = {};
		UINT menu_shown_widths_[8] = {};
		int menu_shown_count_ = 0;
		IDirect3DBaseTexture9* menu_cells_[4] = {};
		int menu_cell_count_ = 0;
		int cur_menu_cell_ = -1;
		float menu_view_half_x_ = 0.0f, menu_view_half_y_ = 0.0f; // the small views' frustum at the character
		IDirect3DBaseTexture9* menu_path_trace_target_ = nullptr;
		IDirect3DTexture9* menu_native_texture_ = nullptr;
		IDirect3DSurface9* menu_native_surface_ = nullptr;
		UINT menu_native_width_ = 0, menu_native_height_ = 0;
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
		// The device holds other transforms than the cached converted ones (disengage
		// set the raw projection, BeginScene an identity WORLD): re-apply on engage.
		bool device_transforms_stale_ = false;

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
		std::unordered_map<IDirect3DPixelShader9*, bool> ps_water_heightmap_;
		bool cur_ps_is_water_heightmap_ = false;

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
		// UV scroll registers each vertex shader reads (see cur_vs_tex_scroll).
		std::unordered_map<IDirect3DVertexShader9*, uint32_t> vs_tex_scroll_;
		uint32_t cur_vs_tex_scroll_ = 0;
		// The map camera's c0-c3 rows as of the last View/Proj decomposition, and the
		// sprite path's repacked vertices.
		float map_viewproj_rows_[16] = {};
		bool have_map_viewproj_ = false;
		std::vector<unsigned char> sprite_vertices_;
		std::vector<sprite_point> sprite_points_;
		// A soft sprite vertex pushed towards the camera by half its feather along its view ray, as the
		// sprite shaders' ZFeather does (draw_sprite_up: the camera and the w row of its view-projection).
		// What of a sprite vertex's alpha the game's fog leaves: 1 - sat((w - c2.x) * c2.y * c159.x) * c4.w. c159.x is 1 for additive and subtractive
		// draws and 0 for alpha ones, whose fog goes into their colour.
		float sprite_fog_alpha(const float* position) const;
		double sprite_cam_[3] = {};
		float sprite_w_row_[4] = {};
		void push_soft_sprite(float* position, float feather) const;
		// Average colour (rgba, 0-1) of textures used as sprite masks, from their smallest mip.
		std::unordered_map<IDirect3DBaseTexture9*, std::array<float, 4>> texture_average_;
		// draw_world_centre: up to 64 of a draw's triangles in object space, by draw (buffers and range).
		struct effect_mesh_triangle { float p[3][3]; uint8_t bone[3]; float rgba[4]; };
		struct effect_mesh_entry { std::vector<effect_mesh_triangle> triangles; float scale = 1.0f; };
		std::unordered_map<uint64_t, effect_mesh_entry> effect_meshes_;
		const effect_mesh_entry* sampled_triangles(D3DPRIMITIVETYPE prim, INT base_vertex, UINT start_index, UINT prim_count);
		std::array<float, 4> texture_average(IDirect3DBaseTexture9* texture);
		// A sprite texture's texels (rgba, 0-1) from its largest mip at most kMaskTexelSize wide, for sampling
		// the Multi sprites' textures per vertex (a small mip: the vertices of a subdivided sprite are several
		// top-level texels apart); empty when the format cannot be read. full_* is its top level's size, flat
		// whether every texel is the same colour.
		struct mask_mip
		{
			UINT width = 0, height = 0;
			std::vector<std::array<float, 4>> texels;
		};
		struct mask_texels
		{
			UINT width = 0, height = 0, full_width = 0, full_height = 0;
			bool flat = false;
			std::vector<std::array<float, 4>> texels;
			std::vector<mask_mip> mips;
		};
		std::unordered_map<IDirect3DBaseTexture9*, mask_texels> mask_texels_;
		const mask_texels* texture_texels(IDirect3DBaseTexture9* texture);
		// A Multi sprite draw as a subdivided triangle list in masked_sprite_vertices_: the textures other than
		// carrier_sampler's (the largest, which Remix samples at its uv) sampled per vertex into the colour (then
		// x colour_scale). The triangle count, 0 when the draw cannot be split so.
		UINT multi_sprite_triangles(D3DPRIMITIVETYPE prim, UINT prim_count, const void* indices, D3DFORMAT index_format,
			const void* vertices, UINT stride, UINT count, float colour_scale, DWORD& carrier_sampler);
		// The Multi sprites subdivided, their masks sampled per vertex (draw_sprite_up).
		std::vector<unsigned char> masked_sprite_vertices_;
		// A soft sprite draw's per-particle feathers (draw_sprite_up).
		std::vector<float> sprite_feathers_;


		// Pixel shaders that read c139 (see cur_ps_reads_draw_tint).
		std::unordered_map<IDirect3DPixelShader9*, bool> ps_draw_tint_;
		bool cur_ps_draw_tint_ = false;
		// Pixel shaders that read c116 (see cur_ps_reads_point_lights).
		std::unordered_map<IDirect3DPixelShader9*, int> ps_point_lights_;
		int cur_ps_point_lights_ = 0;
		// Pixel shaders that read c156 (see apply_skin_tone).
		std::unordered_map<IDirect3DPixelShader9*, bool> ps_skin_tone_;
		bool cur_ps_skin_tone_ = false;
		// c101 per specular map (sampler 1) on draws without a SpEffect.
		std::unordered_map<IDirect3DBaseTexture9*, float> spec_scale_;
		// Whether a texture is DXT1 with punch-through texels (cur_draw_is_fur_shell); unreadable ones are false.
		std::unordered_map<IDirect3DBaseTexture9*, bool> punch_through_;
		// The game's own D3DRS_TEXTUREFACTOR, restored by clear_draw_tint.
		DWORD game_tfactor_ = 0xFFFFFFFF;

		// Particle class of each pixel shader (see cur_ps_sfx_class).
		std::unordered_map<IDirect3DPixelShader9*, sfx_class> ps_sfx_class_;
		sfx_class cur_ps_sfx_class_ = sfx_class::None;
		// Its colour scale before fog (sfx_shaders.inc) and whether it applies the exposure c135.
		std::unordered_map<IDirect3DPixelShader9*, float> ps_sfx_colour_scale_;
		float cur_ps_sfx_colour_scale_ = 1.0f;
		// The SFX pixel shader reads the scene depth (soft sprites, lit soft smoke); sfx_shaders.inc.
		std::unordered_map<IDirect3DPixelShader9*, bool> ps_sfx_reads_depth_;
		bool cur_ps_sfx_reads_depth_ = false;
		std::unordered_map<IDirect3DPixelShader9*, bool> ps_reads_exposure_;
		bool cur_ps_reads_exposure_ = false;

		// Solid-color fallback albedo for water draws with no stable static
		// texture bound. Gives Remix a constant texture hash to remap.
		IDirect3DTexture9* water_fallback_tex_ = nullptr;
		IDirect3DTexture9* white_sprite_tex_ = nullptr;

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
		// TEXCOORD2: the soft sprite shaders' per-vertex feather distance, its last component (-1 = none).
		int cur_decl_feather_off_ = -1;
		// TEXCOORD1 as float2: the Multi sprite shaders' s3 mask uv (-1 = none).
		int cur_decl_texcoord1_off_ = -1;

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
	public:
		// The vertex colour's byte offset in the current declaration, -1 without one.
		int cur_decl_color_offset() const { return cur_decl_color_type_ == D3DDECLTYPE_D3DCOLOR ? cur_decl_color_off_ : -1; }
	private:
		int cur_decl_color_type_ = -1;

		// VS constant register layout — Dark Souls PTDE
		// c0-c3: ViewProj combined (column-major). No separate View/Proj upload.
		// c4-c6: camera rotation (.xyz) + world position (.w)
		// c8-c10: World/Bone matrix (4x3 column-major, 3 regs)
		// Proj range == View range so a single c0-c6 write marks both valid.
		int vs_reg_view_start_ = 0;
		float ui_fit_x_ = 1.0f, ui_fit_y_ = 1.0f;
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
		std::unordered_set<IDirect3DBaseTexture9*> known_cubes_;
		uint32_t known_cube_vtable_ = 0;
		sprite_batch_fn sprite_batch_fn_ = nullptr;
		mutable std::shared_mutex known_cubes_mutex_;

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
		// A blend skirt (apply_blended_decal_marker): a decal the runtime lights on its own surface, which
		// can lie a little above the one it covers (Painted World's rubble skirts over a 0.23 m trench).
		static constexpr DWORD SKIRT_DECAL_MARKER = 0xD15DECA2;
		// High byte of the stage-5 skin tone tag (apply_skin_tone).
		static constexpr DWORD SKIN_TONE_MARKER = 0xD3000000;
		IDirect3DVertexDeclaration9* rigid_exp_decl_ = nullptr;
		bool rigid_exp_active_ = false;

		struct rigid_vb_entry {
			IDirect3DVertexBuffer9* vb = nullptr;
			UINT capacity = 0;      // allocated size in vertices
			UINT vertex_size = 0;   // bytes per vertex the buffer was allocated for
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
			// Palette draws (index-only vertices on several bones): the bone
			// matrices relative to bone 0 that the expansion baked in, for each
			// bone the vertices used, so an unchanged palette can reuse it too.
			bool     palette_valid = false;
			uint64_t palette_fp = 0;
			std::vector<uint8_t> palette_used;
			std::vector<float>   palette_rel;
		};

		// Cheap fingerprint of everything a rigid expansion reads: evenly spaced
		// 8-byte samples of the source range, its dimensions, and the current
		// declaration's layout. Reads ~128 bytes instead of the whole range, so it
		// is far cheaper than re-converting, while still catching a buffer that was
		// recreated at the same address or drawn through a different declaration.
		uint64_t fingerprint_source(const unsigned char* src, UINT total_verts,
			UINT stride, rigid_layer layer) const;

		// True when the FFP texture stage state set by setup_texture_stages is
		// still resident, so TextureStages=1 can skip re-applying it.
		bool ffp_stages_applied_ = false;
		// True when this draw unbound stages 1-7. RestoreTextures=1 must restore
		// them if and only if something actually nulled them — the albedo path
		// does so only under AlbedoNullStages, but the overlay pass always does.
		bool nulled_high_stages_ = false;
		std::unordered_map<uint64_t, rigid_vb_entry> rigid_exp_cache_;
		std::unordered_map<uint64_t, rigid_vb_entry> rigid_overlay_cache_;
		std::unordered_map<uint64_t, rigid_vb_entry> rigid_glass_cache_;

		// Skinned vertex expansion — same cache structure but preserves full vertex layout.
		// Keyed by source VB and byte offset like the rigid caches. The expanded vertex size
		// follows the draw's declaration, and a VB the game frees can come back at the same
		// address with another one, so an entry's buffer is only reused at its own vertex size.
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
