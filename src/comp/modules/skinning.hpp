#pragma once

namespace comp
{
	/*
	 * Optional skinning module for FFP conversion.
	 *
	 * When enabled via [Skinning] Enabled=1 in remix-comp-proxy.ini, this module
	 * handles skinned mesh rendering by expanding compressed vertex data,
	 * uploading bone matrices via SetTransform(WORLDMATRIX(i)), and drawing
	 * with D3D FFP indexed vertex blending.
	 *
	 * Only enable after rigid FFP geometry works correctly.
	 */
	class skinning final : public shared::common::loader::component_module
	{
	public:
		skinning();
		~skinning();

		static inline skinning* p_this = nullptr;
		static skinning* get() { return p_this; }

		static bool is_available()
		{
			return p_this != nullptr;
		}

		// Called from renderer when cur_decl_is_skinned
		HRESULT draw_skinned_dip(IDirect3DDevice9* dev,
			D3DPRIMITIVETYPE pt, INT base_vtx, UINT min_vtx, UINT num_verts,
			UINT start_idx, UINT prim_count);

		// Called on device reset
		void on_reset();

		// With [FFPSteps] Profile on: this frame's skinned draws, their time (QPC ticks) and the
		// bone matrices they uploaded. d3d9ex's frame split reads and clears it at Present.
		struct frame_stats
		{
			long long ticks = 0;
			unsigned int draws = 0;
			unsigned int bones = 0;
		};
		static inline frame_stats s_stats {};

	private:
		static constexpr int SKIN_VTX_SIZE = 48;
		// The overlay layout: SKIN_VTX_SIZE with the second uv set as TEXCOORD0, plus D3DCOLOR.
		static constexpr int SKIN_OVERLAY_VTX_SIZE = 52;
		// Entries no draw has used for this many frames are released.
		static constexpr UINT SKIN_CACHE_MAX_AGE = 600;

		IDirect3DVertexDeclaration9* skin_exp_decl_ = nullptr;
		IDirect3DVertexDeclaration9* skin_overlay_decl_ = nullptr;

		// Expanded vertex buffer cache. Every source range keeps one buffer for as long as it is drawn:
		// Remix pairs a draw with last frame's instance only through the same vertex buffer, and a
		// skinned mesh bound to a single bone becomes a rigid instance whose motion vectors come from
		// that pairing alone (hair, eyes, parts of the face).
		// base: SKIN_VTX_SIZE; overlay: the second uv set and the vertex alpha (a two-layer draw's second
		// diffuse); vertex_colour: the first uv set and the whole vertex colour (fur shells).
		// body: the SKIN_OVERLAY_VTX_SIZE layout, the colour being the vertex's bind position (a body part's draw).
		// fur_layer_1..3: a fur shell's in-between copies (expand_skin_vertex).
		enum class skin_layout : uint8_t { base, overlay, vertex_colour, body, fur_layer_1, fur_layer_2, fur_layer_3 };

		struct skin_key {
			IDirect3DVertexBuffer9* src_vb;
			INT base_vtx;
			UINT num_verts;
			UINT stride;
			skin_layout layout;   // which layout the entry holds
			bool operator==(const skin_key&) const = default;
		};
		struct skin_key_hash {
			size_t operator()(const skin_key& k) const {
				size_t h = std::hash<uintptr_t>{}(reinterpret_cast<uintptr_t>(k.src_vb));
				h ^= std::hash<INT>{}(k.base_vtx) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<UINT>{}(k.num_verts) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<UINT>{}(k.stride) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<int>{}(static_cast<int>(k.layout)) + 0x9e3779b9 + (h << 6) + (h >> 2);
				return h;
			}
		};
		struct skin_entry {
			IDirect3DVertexBuffer9* vb = nullptr;
			// Sampled content fingerprint of the source range: the game can free a VB
			// and create another at the same address, which a pointer key alone would
			// serve the old character's vertices for.
			uint64_t fp = 0;
			// Highest blend index the expanded vertices use: the bones to upload.
			int max_idx = 0;
			// Highest blend index any vertex lists among the bones the blend mode reads.
			int raw_max_idx = 0;
			UINT last_frame = 0;
		};
		std::unordered_map<skin_key, skin_entry, skin_key_hash> skin_cache_;
		UINT last_prune_frame_ = 0;
		int cur_max_idx_ = 0;
		int cur_raw_max_idx_ = 0;

		bool initialized_ = false;

		void create_expanded_decl(IDirect3DDevice9* dev);
		void release_cache();
		void prune_cache(UINT frame);

		// Vertex expansion (overlay and vertex_colour: the SKIN_OVERLAY_VTX_SIZE layout)
		IDirect3DVertexBuffer9* get_expanded_vb(IDirect3DDevice9* dev,
			IDirect3DVertexBuffer9* src_vb, INT base_vtx, UINT num_verts, UINT stride, skin_layout layout);
		void expand_skin_vertex(unsigned char* dst, const unsigned char* src, skin_layout layout);
		// A shell mesh's own layer spacing and how much alpha a layer loses, from its vertices.
		void measure_fur_shells(const unsigned char* src, UINT num_verts, UINT stride);
		float fur_step_ = 0.015f;
		float fur_alpha_step_ = 0.0f;
		int fur_layer_count_ = 1;

		// A two-layer draw's second diffuse, re-drawn as a coplanar decal (see draw_skinned_dip).
		HRESULT draw_overlay(IDirect3DDevice9* dev, D3DPRIMITIVETYPE pt, IDirect3DVertexBuffer9* src_vb,
			INT base_vtx, UINT min_vtx, UINT num_verts, UINT start_idx, UINT prim_count, UINT stride);
		static uint64_t fingerprint(const unsigned char* src, UINT num_verts, UINT stride);

		// Bone upload
		void upload_bones(IDirect3DDevice9* dev);
		void disable_skinning(IDirect3DDevice9* dev);

		// Format decoders
		static float half_to_float(unsigned short h);
		static void decode_normal(const unsigned char* src, int type, float* out);
	};
}
