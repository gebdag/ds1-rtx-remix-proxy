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

	private:
		static constexpr int SKIN_VTX_SIZE = 48;
		// The overlay layout: SKIN_VTX_SIZE with the second uv set as TEXCOORD0, plus D3DCOLOR.
		static constexpr int SKIN_OVERLAY_VTX_SIZE = 52;
		static constexpr int SKIN_CACHE_SIZE = 64;

		IDirect3DVertexDeclaration9* skin_exp_decl_ = nullptr;
		IDirect3DVertexDeclaration9* skin_overlay_decl_ = nullptr;

		// Expanded vertex buffer cache
		IDirect3DVertexBuffer9* skin_exp_vb_[SKIN_CACHE_SIZE] = {};
		unsigned int skin_exp_key_[SKIN_CACHE_SIZE] = {};
		unsigned int skin_exp_nv_[SKIN_CACHE_SIZE] = {};
		// Which layout the entry holds (base or overlay vertices).
		bool skin_exp_overlay_[SKIN_CACHE_SIZE] = {};
		// Sampled content fingerprint of the source range: the game can free a VB
		// and create another at the same address, which a pointer key alone would
		// serve the old character's vertices for.
		uint64_t skin_exp_fp_[SKIN_CACHE_SIZE] = {};
		// Highest blend index the expanded vertices use: the bones to upload.
		int skin_exp_max_idx_[SKIN_CACHE_SIZE] = {};
		int cur_max_idx_ = 0;

		bool initialized_ = false;

		void create_expanded_decl(IDirect3DDevice9* dev);
		void release_cache();

		// Vertex expansion (overlay: the SKIN_OVERLAY_VTX_SIZE layout)
		IDirect3DVertexBuffer9* get_expanded_vb(IDirect3DDevice9* dev,
			IDirect3DVertexBuffer9* src_vb, INT base_vtx, UINT num_verts, UINT stride, bool overlay);
		void expand_skin_vertex(unsigned char* dst, const unsigned char* src, bool overlay);

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
