#include "std_include.hpp"
#include "skinning.hpp"

#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/ffp_prof.hpp"
#include "../game/texture_scroll.hpp"

namespace comp
{
	skinning::skinning()
	{
		p_this = this;
		initialized_ = true;
		shared::common::log("Skinning", "Module initialized.", shared::common::LOG_TYPE::LOG_TYPE_WARN);
	}

	skinning::~skinning()
	{
		on_reset();
		p_this = nullptr;
	}

	HRESULT skinning::draw_skinned_dip(IDirect3DDevice9* dev,
		D3DPRIMITIVETYPE pt, INT base_vtx, UINT min_vtx, UINT num_verts,
		UINT start_idx, UINT prim_count)
	{
		auto& ffp = shared::common::ffp_state::get();

		struct stats_scope
		{
			LARGE_INTEGER start {};
			stats_scope() { if (shared::common::ffp_prof::profiling()) QueryPerformanceCounter(&start); }
			~stats_scope()
			{
				if (!start.QuadPart)
					return;
				LARGE_INTEGER end;
				QueryPerformanceCounter(&end);
				s_stats.ticks += end.QuadPart - start.QuadPart;
				++s_stats.draws;
			}
		} stats;

		// Create expanded declaration on first use
		if (!skin_exp_decl_)
			create_expanded_decl(dev);

		if (!skin_exp_decl_)
		{
			// Fallback: passthrough with shaders
			ffp.disengage(dev);
			return dev->DrawIndexedPrimitive(pt, base_vtx, min_vtx, num_verts, start_idx, prim_count);
		}

		// Get expanded vertex buffer from cache or create new
		auto* src_vb = ffp.stream_vb(0);
		UINT stride = ffp.stream_stride(0);
		UINT total_verts = min_vtx + num_verts;
		auto* exp_vb = get_expanded_vb(dev, src_vb, base_vtx, total_verts, stride, false);

		if (!exp_vb)
		{
			ffp.disengage(dev);
			return dev->DrawIndexedPrimitive(pt, base_vtx, min_vtx, num_verts, start_idx, prim_count);
		}

		// Engage FFP with skinning
		ffp.engage(dev);

		// Set albedo texture — skip the global-texture filter for skinned draws
		// (characters/enemies should never be filtered out).
		ffp.setup_albedo_texture(dev);

		upload_bones(dev);

		// Swap in expanded VB and declaration
		IDirect3DVertexDeclaration9* orig_decl = nullptr;
		dev->GetVertexDeclaration(&orig_decl);
		dev->SetVertexDeclaration(skin_exp_decl_);
		dev->SetStreamSource(0, exp_vb, 0, SKIN_VTX_SIZE);

		const bool tinted = ffp.apply_draw_tint(dev);
		const bool effect = ffp.apply_sp_effect(dev);
		const bool toned = ffp.apply_skin_tone(dev, false);
		const bool scrolled = game::texture_scroll::apply(dev, false, false, src_vb, ffp.cur_ib(),
			base_vtx, start_idx, prim_count, ffp.frame_count());
		auto hr = dev->DrawIndexedPrimitive(pt, 0, min_vtx, num_verts, start_idx, prim_count);
		if (scrolled)
			game::texture_scroll::clear(dev);
		if (toned)
			ffp.clear_skin_tone(dev);
		// Two-layer material (the FaceGen face: lerp(s0, s3 + c156, vertex alpha)): the second
		// diffuse as a coplanar decal with vertex alpha as its opacity, as on the rigid path.
		if (SUCCEEDED(hr) && ffp.overlay_texture())
		{
			const HRESULT overlay_hr = draw_overlay(dev, pt, src_vb, base_vtx, min_vtx, num_verts,
				start_idx, prim_count, stride);
			if (FAILED(overlay_hr))
				hr = overlay_hr;
		}
		if (tinted)
			ffp.clear_draw_tint(dev);
		if (effect)
			ffp.clear_sp_effect(dev);

		// Restore original state
		dev->SetVertexDeclaration(orig_decl);
		if (orig_decl) orig_decl->Release();
		dev->SetStreamSource(0, src_vb, ffp.stream_offset(0), stride);

		disable_skinning(dev);
		ffp.restore_textures(dev);
		ffp.disengage(dev);

		return hr;
	}

	void skinning::on_reset()
	{
		release_cache();
		for (auto** decl : { &skin_exp_decl_, &skin_overlay_decl_ })
		{
			if (*decl)
			{
				(*decl)->Release();
				*decl = nullptr;
			}
		}
	}

	void skinning::create_expanded_decl(IDirect3DDevice9* dev)
	{
		// Expanded skinned vertex layout: FLOAT3 pos + FLOAT3 weights + UBYTE4 idx + FLOAT3 normal + FLOAT2 uv
		const D3DVERTEXELEMENT9 base[] = {
			{ 0,  0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
			{ 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0 },
			{ 0, 24, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0 },
			{ 0, 28, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
			{ 0, 40, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
			D3DDECL_END()
		};
		// The overlay pass: the same, the uv being the second set, plus the vertex colour.
		const D3DVERTEXELEMENT9 overlay[] = {
			{ 0,  0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
			{ 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0 },
			{ 0, 24, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0 },
			{ 0, 28, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
			{ 0, 40, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
			{ 0, 48, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
			D3DDECL_END()
		};

		if (!skin_exp_decl_ && FAILED(dev->CreateVertexDeclaration(base, &skin_exp_decl_)))
			skin_exp_decl_ = nullptr;
		if (!skin_overlay_decl_ && FAILED(dev->CreateVertexDeclaration(overlay, &skin_overlay_decl_)))
			skin_overlay_decl_ = nullptr;
		if (!skin_exp_decl_ || !skin_overlay_decl_)
			shared::common::log("Skinning", "Failed to create expanded vertex declaration!",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
	}

	void skinning::release_cache()
	{
		for (auto& [key, entry] : skin_cache_)
		{
			if (entry.vb)
				entry.vb->Release();
		}
		skin_cache_.clear();
	}

	void skinning::prune_cache(UINT frame)
	{
		for (auto it = skin_cache_.begin(); it != skin_cache_.end(); )
		{
			if (frame - it->second.last_frame > SKIN_CACHE_MAX_AGE)
			{
				if (it->second.vb)
					it->second.vb->Release();
				it = skin_cache_.erase(it);
			}
			else
			{
				++it;
			}
		}
		last_prune_frame_ = frame;
	}

	uint64_t skinning::fingerprint(const unsigned char* src, UINT num_verts, UINT stride)
	{
		// FNV-1a over the full bytes of up to 64 vertices spread across the range,
		// always including the first and the last.
		uint64_t h = 0xCBF29CE484222325ull;
		auto mix = [&h](const unsigned char* p, UINT n)
		{
			for (UINT i = 0; i < n; ++i)
			{
				h ^= p[i];
				h *= 0x100000001B3ull;
			}
		};
		mix(reinterpret_cast<const unsigned char*>(&num_verts), sizeof(num_verts));

		constexpr UINT kSamples = 64;
		const UINT step = num_verts > kSamples ? num_verts / kSamples : 1;
		for (UINT v = 0; v < num_verts; v += step)
			mix(&src[v * stride], stride);
		mix(&src[(num_verts - 1) * stride], stride);
		return h;
	}

	IDirect3DVertexBuffer9* skinning::get_expanded_vb(IDirect3DDevice9* dev,
		IDirect3DVertexBuffer9* src_vb, INT base_vtx, UINT num_verts, UINT stride, bool overlay)
	{
		if (!src_vb || stride == 0 || num_verts == 0) return nullptr;
		const UINT vtx_size = overlay ? SKIN_OVERLAY_VTX_SIZE : SKIN_VTX_SIZE;

		const UINT frame = shared::common::ffp_state::get().frame_count();
		if (frame - last_prune_frame_ >= SKIN_CACHE_MAX_AGE)
			prune_cache(frame);

		auto& entry = skin_cache_[skin_key{ src_vb, base_vtx, num_verts, stride, overlay }];
		entry.last_frame = frame;

		// Lock source VB
		unsigned char* src_data = nullptr;
		UINT read_off = static_cast<UINT>(base_vtx) * stride;
		if (FAILED(src_vb->Lock(read_off, num_verts * stride, reinterpret_cast<void**>(&src_data), D3DLOCK_READONLY)))
			return nullptr;

		const uint64_t fp = fingerprint(src_data, num_verts, stride);

		// Cache hit: still the same vertices
		if (entry.vb && entry.fp == fp)
		{
			src_vb->Unlock();
			cur_max_idx_ = entry.max_idx;
			cur_raw_max_idx_ = entry.raw_max_idx;
			return entry.vb;
		}

		// Other vertices in the range re-expand into the entry's buffer, which the key sizes exactly:
		// a new buffer would cost Remix the draw's instance pairing. A new range creates one.
		IDirect3DVertexBuffer9* exp_vb = entry.vb;
		if (!exp_vb && FAILED(dev->CreateVertexBuffer(num_verts * vtx_size, 0,
			0, D3DPOOL_MANAGED, &exp_vb, nullptr)))
		{
			src_vb->Unlock();
			return nullptr;
		}
		entry.vb = exp_vb;

		// Lock and expand
		unsigned char* dst_data = nullptr;
		if (FAILED(exp_vb->Lock(0, num_verts * vtx_size, reinterpret_cast<void**>(&dst_data), 0)))
		{
			src_vb->Unlock();
			exp_vb->Release();
			entry.vb = nullptr;
			return nullptr;
		}

		cur_max_idx_ = 0;
		cur_raw_max_idx_ = 0;
		for (UINT v = 0; v < num_verts; v++)
			expand_skin_vertex(&dst_data[v * vtx_size], &src_data[v * stride], overlay);

		exp_vb->Unlock();
		src_vb->Unlock();

		entry.fp = fp;
		entry.max_idx = cur_max_idx_;
		entry.raw_max_idx = cur_raw_max_idx_;

		return exp_vb;
	}

	void skinning::expand_skin_vertex(unsigned char* dst, const unsigned char* src, bool overlay)
	{
		auto& ffp = shared::common::ffp_state::get();
		auto* out = reinterpret_cast<float*>(dst);

		// Position (FLOAT3, always at pos_off)
		auto* pos = reinterpret_cast<const float*>(&src[ffp.cur_decl_pos_off()]);
		out[0] = pos[0]; out[1] = pos[1]; out[2] = pos[2];

		// Blend weights (FLOAT3 at offset 12). DS1's skinned vertex shaders
		// (FRPG_*_PIWN*) read all four SHORT4N weights and divide by their sum
		// before blending; FFP 3WEIGHTS derives the fourth as 1 - (w0 + w1 + w2),
		// so the stored three must already be normalised for the two to agree.
		auto bw_off = ffp.cur_decl_blend_weight_off();
		auto bw_type = ffp.cur_decl_blend_weight_type();

		float w[4] = {};
		int w_count = 0;
		switch (bw_type)
		{
		case D3DDECLTYPE_FLOAT1: w_count = 1; break;
		case D3DDECLTYPE_FLOAT2: w_count = 2; break;
		case D3DDECLTYPE_FLOAT3: w_count = 3; break;
		case D3DDECLTYPE_FLOAT4: w_count = 4; break;
		default: break;
		}
		if (w_count > 0)
		{
			auto* f = reinterpret_cast<const float*>(&src[bw_off]);
			for (int i = 0; i < w_count; i++) w[i] = f[i];
		}
		else if (bw_type == D3DDECLTYPE_UBYTE4N)
		{
			for (int i = 0; i < 4; i++) w[i] = src[bw_off + i] / 255.0f;
		}
		else if (bw_type == D3DDECLTYPE_SHORT4N || bw_type == D3DDECLTYPE_SHORT2N)
		{
			auto* bw = reinterpret_cast<const short*>(&src[bw_off]);
			const int n = bw_type == D3DDECLTYPE_SHORT4N ? 4 : 2;
			for (int i = 0; i < n; i++) w[i] = std::max(-1.0f, bw[i] / 32767.0f);
		}

		const float w_sum = w[0] + w[1] + w[2] + w[3];
		if (std::fabs(w_sum) > 1e-6f)
		{
			for (int i = 0; i < 4; i++) w[i] /= w_sum;
		}
		else
		{
			// No usable weights: bind rigidly to the first index.
			w[0] = 1.0f; w[1] = w[2] = w[3] = 0.0f;
		}
		out[3] = w[0];
		out[4] = w[1];
		out[5] = w[2];

		// Blend indices (UBYTE4 at offset 24)
		const unsigned char* bi = &src[ffp.cur_decl_blend_indices_off()];
		std::memcpy(&dst[24], bi, 4);
		for (int i = 0; i < 4; i++)
		{
			// Indices whose weight is zero never reach the output.
			if (w[i] != 0.0f && bi[i] > cur_max_idx_)
				cur_max_idx_ = bi[i];
		}
		// Remix sizes and hashes its palette by the raw indices of every bone the vertex blend
		// mode reads (upload_bones: 2, 3 or 4), zero weights included.
		const int nw = ffp.cur_decl_num_weights();
		const int raw_count = nw <= 1 ? 2 : (nw == 2 ? 3 : 4);
		for (int i = 0; i < raw_count; i++)
		{
			if (bi[i] > cur_raw_max_idx_)
				cur_raw_max_idx_ = bi[i];
		}

		// Normal (FLOAT3 at offset 28)
		if (ffp.cur_decl_has_normal())
		{
			decode_normal(&src[ffp.cur_decl_normal_off()], ffp.cur_decl_normal_type(), &out[7]);
		}
		else
		{
			out[7] = 0.0f; out[8] = 1.0f; out[9] = 0.0f;
		}

		// Texcoord (FLOAT2 at offset 40). The overlay takes the second pair of a four-component
		// texcoord (TEXCOORD0.zw, the second layer's uv): only SHORT4/FLOAT4 draws have an overlay.
		const int first = overlay ? 2 : 0;
		if (ffp.cur_decl_has_texcoord())
		{
			int tc_type = ffp.cur_decl_texcoord_type();
			int tc_off = ffp.cur_decl_texcoord_off();
			if (tc_type == D3DDECLTYPE_FLOAT2 || tc_type == D3DDECLTYPE_FLOAT3 || tc_type == D3DDECLTYPE_FLOAT4)
			{
				auto* tc = reinterpret_cast<const float*>(&src[tc_off]);
				out[10] = tc[first]; out[11] = tc[first + 1];
			}
			else if (tc_type == D3DDECLTYPE_FLOAT16_2)
			{
				auto* h = reinterpret_cast<const unsigned short*>(&src[tc_off]);
					out[10] = half_to_float(h[0]);
				out[11] = half_to_float(h[1]);
			}
			else if (tc_type == D3DDECLTYPE_SHORT2 || tc_type == D3DDECLTYPE_SHORT4)
			{
				// Dark Souls stores UVs as SHORT * 1024; convert to normalized FLOAT
				constexpr float kScale = 1.0f / 1024.0f;
				auto* tc = reinterpret_cast<const short*>(&src[tc_off]);
				out[10] = static_cast<float>(tc[first]) * kScale;
				out[11] = static_cast<float>(tc[first + 1]) * kScale;
			}
			else
			{
				out[10] = out[11] = 0.0f;
			}
		}
		else
		{
			out[10] = out[11] = 0.0f;
		}

		// Vertex colour (D3DCOLOR at offset 48, overlay only): its alpha is the second layer's weight.
		// Its rgb stays white, as on the base layer, which carries no vertex colour.
		if (overlay)
		{
			const DWORD argb = (ffp.cur_vertex_color(src) & 0xFF000000u) | 0x00FFFFFFu;
			std::memcpy(&dst[48], &argb, sizeof(argb));
		}
	}

	HRESULT skinning::draw_overlay(IDirect3DDevice9* dev, D3DPRIMITIVETYPE pt, IDirect3DVertexBuffer9* src_vb,
		INT base_vtx, UINT min_vtx, UINT num_verts, UINT start_idx, UINT prim_count, UINT stride)
	{
		auto& ffp = shared::common::ffp_state::get();
		auto* texture = ffp.overlay_texture();
		auto* overlay_vb = skin_overlay_decl_ ? get_expanded_vb(dev, src_vb, base_vtx, min_vtx + num_verts, stride, true) : nullptr;
		if (!texture || !overlay_vb)
			return S_OK;

		// The render states the overlay pass changes, restored after it.
		static constexpr D3DRENDERSTATETYPE kStates[] = {
			D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,
			D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE,
		};
		DWORD saved[std::size(kStates)] = {};
		for (size_t i = 0; i < std::size(kStates); i++)
			dev->GetRenderState(kStates[i], &saved[i]);

		dev->SetVertexDeclaration(skin_overlay_decl_);
		dev->SetStreamSource(0, overlay_vb, 0, SKIN_OVERLAY_VTX_SIZE);
		ffp.apply_overlay_decal_states(dev, texture);
		const bool toned = ffp.apply_skin_tone(dev, true);
		const bool scrolled = game::texture_scroll::apply(dev, true, false, src_vb, ffp.cur_ib(),
			base_vtx, start_idx, prim_count, ffp.frame_count());
		const HRESULT hr = dev->DrawIndexedPrimitive(pt, 0, min_vtx, num_verts, start_idx, prim_count);
		if (scrolled)
			game::texture_scroll::clear(dev);
		if (toned)
			ffp.clear_skin_tone(dev);
		ffp.clear_decal_marker(dev);

		for (size_t i = 0; i < std::size(kStates); i++)
			dev->SetRenderState(kStates[i], saved[i]);
		return hr;
	}

	void skinning::upload_bones(IDirect3DDevice9* dev)
	{
		auto& ffp = shared::common::ffp_state::get();

		int bone_start = ffp.bone_start_reg();
		int num_bones = ffp.num_bones();
		if (num_bones <= 0) return;

		const float* vs_const = ffp.vs_const_data();
		int regs_per_bone = ffp.regs_per_bone();
		if (regs_per_bone != 3 && regs_per_bone != 4) return;

		// The vertex shader indexes the palette with the vertex's blend indices,
		// whatever the last constant write covered: upload every bone a vertex of
		// this draw can reach, from the same constants the GPU would read.
		const int palette_capacity = (256 - bone_start) / regs_per_bone;
		// num_bones is the palette's high-water mark over the whole session (every bone any draw's
		// constant writes ever reached, 83 in Blighttown); a mesh's vertices only index up to
		// cur_raw_max_idx_, the bones Remix reads and hashes (zero weights included).
		int max_bones = shared::common::config::get().ffp_steps.skin_upload_used_bones
			? std::max(cur_max_idx_, cur_raw_max_idx_) + 1 : std::max(num_bones, cur_max_idx_ + 1);
		max_bones = std::min({ max_bones, palette_capacity, 256 });
		if (shared::common::ffp_prof::profiling())
			s_stats.bones += static_cast<unsigned int>(std::max(max_bones, 0));

		// The main pass draws camera-relative; apply_transforms moves World into
		// absolute space by adding the camera position, and every bone needs the
		// same shift or the character renders offset by the camera position.
		float off_x = 0.0f, off_y = 0.0f, off_z = 0.0f;
		const bool offset = ffp.world_offset(off_x, off_y, off_z);

		for (int i = 0; i < max_bones; i++)
		{
			// Each bone is three rows of a row-major 3x4 matrix, out = M * [p, 1];
			// D3D wants the row-vector 4x4 form, i.e. its transpose.
			const float* src = &vs_const[(bone_start + i * regs_per_bone) * 4];
			float bone44[16];

			bone44[0]  = src[0]; bone44[1]  = src[4]; bone44[2]  = src[8];  bone44[3]  = 0.0f;
			bone44[4]  = src[1]; bone44[5]  = src[5]; bone44[6]  = src[9];  bone44[7]  = 0.0f;
			bone44[8]  = src[2]; bone44[9]  = src[6]; bone44[10] = src[10]; bone44[11] = 0.0f;
			bone44[12] = src[3]; bone44[13] = src[7]; bone44[14] = src[11]; bone44[15] = 1.0f;
			if (regs_per_bone == 4)
			{
				bone44[3] = src[12]; bone44[7] = src[13]; bone44[11] = src[14]; bone44[15] = src[15];
			}

			if (offset)
			{
				bone44[12] += off_x;
				bone44[13] += off_y;
				bone44[14] += off_z;
			}

			dev->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_WORLDMATRIX(i)),
				reinterpret_cast<const D3DMATRIX*>(bone44));
		}

		// Enable indexed vertex blending
		D3DVERTEXBLENDFLAGS blend_flag;
		int nw = ffp.cur_decl_num_weights();
		if (nw <= 1)      blend_flag = D3DVBF_1WEIGHTS;
		else if (nw == 2) blend_flag = D3DVBF_2WEIGHTS;
		else              blend_flag = D3DVBF_3WEIGHTS;

		dev->SetRenderState(D3DRS_VERTEXBLEND, blend_flag);
		dev->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE);
	}

	void skinning::disable_skinning(IDirect3DDevice9* dev)
	{
		dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
		dev->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
	}

	float skinning::half_to_float(unsigned short h)
	{
		unsigned int sign = (h >> 15) & 0x1;
		unsigned int exp = (h >> 10) & 0x1F;
		unsigned int mant = h & 0x3FF;

		if (exp == 0)
		{
			if (mant == 0) {
				unsigned int f = sign << 31;
				float result;
				std::memcpy(&result, &f, sizeof(float));
				return result;
			}
			// Denormalized
			while (!(mant & 0x400)) { mant <<= 1; exp--; }
			exp++; mant &= ~0x400u;
		}
		else if (exp == 31)
		{
			unsigned int f = (sign << 31) | 0x7F800000u | (mant << 13);
			float result;
			std::memcpy(&result, &f, sizeof(float));
			return result;
		}

		exp += (127 - 15);
		unsigned int f = (sign << 31) | (exp << 23) | (mant << 13);
		float result;
		std::memcpy(&result, &f, sizeof(float));
		return result;
	}

	void skinning::decode_normal(const unsigned char* src, int type, float* out)
	{
		switch (type)
		{
		case D3DDECLTYPE_FLOAT3:
		{
			auto* fp = reinterpret_cast<const float*>(src);
			out[0] = fp[0]; out[1] = fp[1]; out[2] = fp[2];
			break;
		}
		case D3DDECLTYPE_FLOAT16_2:
		{
			auto* h = reinterpret_cast<const unsigned short*>(src);
			out[0] = half_to_float(h[0]);
			out[1] = half_to_float(h[1]);
			out[2] = 0.0f; // reconstruct Z if needed
			break;
		}
		case D3DDECLTYPE_DEC3N:
		{
			unsigned int packed = *reinterpret_cast<const unsigned int*>(src);
			int x = static_cast<int>((packed >>  0) & 0x3FF); if (x & 0x200) x |= ~0x3FF;
			int y = static_cast<int>((packed >> 10) & 0x3FF); if (y & 0x200) y |= ~0x3FF;
			int z = static_cast<int>((packed >> 20) & 0x3FF); if (z & 0x200) z |= ~0x3FF;
			out[0] = x / 511.0f;
			out[1] = y / 511.0f;
			out[2] = z / 511.0f;
			break;
		}
		case D3DDECLTYPE_UBYTE4N:
		{
			// FLVER normals are (b - 127) / 127. The skinned vertex shaders decode
			// the UBYTE4N input with (v - 0.49804) * 2.00787, which is exactly that.
			out[0] = (src[0] - 127.0f) / 127.0f;
			out[1] = (src[1] - 127.0f) / 127.0f;
			out[2] = (src[2] - 127.0f) / 127.0f;
			break;
		}
		default:
			out[0] = 0.0f; out[1] = 1.0f; out[2] = 0.0f;
			break;
		}
	}
}
