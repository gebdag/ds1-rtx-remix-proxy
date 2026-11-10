#include "std_include.hpp"
#include "lightmap_sun.hpp"
#include "texture_names.hpp"
#include "waterfall.hpp"

#include "shared/common/ffp_state.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace comp::game::lightmap_sun
{
	namespace
	{
		using shared::common::ffp_state;

		// Triangles sampled per draw: evenly spaced over its index range. A large terrain draw
		// has tens of thousands; a few hundred give its level to well within a step of 1/255.
		constexpr UINT kMaxSamples = 512;
		// Draws measured per frame at most: an area streaming in brings hundreds of new draws, and
		// the game must not hitch. The rest stay untagged for the few frames until their turn.
		constexpr int kMeasuresPerFrame = 24;
		// A draw whose data could not be read (a lightmap still streaming in) is tried again this
		// many frames later.
		constexpr unsigned int kRetryFrames = 60;
		// Cached levels of draws not seen for this many frames go (their area was unloaded).
		constexpr unsigned int kPruneFrames = 36000;

		// Where the draw's lightmap uv and position are in its stream-0 vertex.
		struct layout
		{
			bool valid = false;
			int pos_off = -1;     // FLOAT3
			int uv_off = -1;      // first of the two lightmap uv components
			BYTE uv_type = 0;     // SHORT2/SHORT4 (raw / 1024 in the vertex shader) or FLOAT2/FLOAT4
		};

		// Read from the declaration at measuring time only (a released declaration's address can come
		// back with another layout), and checked against the stream's stride.
		layout layout_of(IDirect3DVertexDeclaration9* decl, UINT stride)
		{
			layout l;
			D3DVERTEXELEMENT9 els[MAXD3DDECLLENGTH + 1] = {};
			UINT n = 0;
			if (decl && SUCCEEDED(decl->GetDeclaration(nullptr, &n)) && n <= MAXD3DDECLLENGTH + 1 &&
				SUCCEEDED(decl->GetDeclaration(els, &n)))
			{
				const D3DVERTEXELEMENT9* tc0 = nullptr;
				const D3DVERTEXELEMENT9* tc1 = nullptr;
				for (UINT i = 0; i < n && els[i].Stream != 0xFF; i++)
				{
					const auto& e = els[i];
					if (e.Stream != 0)
						continue;
					if (e.Usage == D3DDECLUSAGE_POSITION && e.UsageIndex == 0 && e.Type == D3DDECLTYPE_FLOAT3)
						l.pos_off = e.Offset;
					else if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex == 0)
						tc0 = &e;
					else if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex == 1)
						tc1 = &e;
				}
				// PINTT_DDL: TEXCOORD0 holds both diffuse uvs, TEXCOORD1 the lightmap's.
				// The others: TEXCOORD0.xy diffuse, .zw lightmap.
				if (tc1 && (tc1->Type == D3DDECLTYPE_SHORT2 || tc1->Type == D3DDECLTYPE_SHORT4 ||
					tc1->Type == D3DDECLTYPE_FLOAT2 || tc1->Type == D3DDECLTYPE_FLOAT4))
				{
					l.uv_off = tc1->Offset;
					l.uv_type = tc1->Type;
				}
				else if (tc0 && tc0->Type == D3DDECLTYPE_SHORT4)
				{
					l.uv_off = tc0->Offset + 4;
					l.uv_type = D3DDECLTYPE_SHORT2;
				}
				else if (tc0 && tc0->Type == D3DDECLTYPE_FLOAT4)
				{
					l.uv_off = tc0->Offset + 8;
					l.uv_type = D3DDECLTYPE_FLOAT2;
				}
				const int uv_bytes = l.uv_type == D3DDECLTYPE_FLOAT2 || l.uv_type == D3DDECLTYPE_FLOAT4 ? 8 : 4;
				l.valid = l.pos_off >= 0 && l.uv_off >= 0 &&
					l.pos_off + 12 <= static_cast<int>(stride) && l.uv_off + uv_bytes <= static_cast<int>(stride);
			}
			return l;
		}

		struct draw_key
		{
			uintptr_t vb, ib, lightmap, decl;
			UINT vb_offset, stride;
			INT base_vertex;
			UINT start_index, prim_count;
			DWORD prim;

			bool operator==(const draw_key&) const = default;
		};

		struct draw_key_hash
		{
			size_t operator()(const draw_key& k) const
			{
				size_t h = 1469598103u;
				auto mix = [&](size_t v) { h ^= v + 0x9e3779b9 + (h << 6) + (h >> 2); };
				mix(k.vb); mix(k.ib); mix(k.lightmap); mix(k.decl); mix(k.vb_offset); mix(k.stride);
				mix(static_cast<size_t>(k.base_vertex)); mix(k.start_index); mix(k.prim_count); mix(k.prim);
				return h;
			}
		};

		struct entry
		{
			DWORD tag = 0;
			unsigned int last_frame = 0;
			bool measured = false;
			unsigned int retry_frame = 0; // when unmeasured: the frame to try again
		};

		std::unordered_map<draw_key, entry, draw_key_hash> g_levels;
		unsigned int g_last_prune = 0;
		DWORD g_tag_set = ~0u; // the stage 6 constant as last set; ~0 = unknown (set it)
		unsigned int g_tag_frame = 0;
		int g_measures_left = kMeasuresPerFrame;

		void set_tag(IDirect3DDevice9* dev, DWORD tag)
		{
			if (tag != g_tag_set)
			{
				dev->SetTextureStageState(6, D3DTSS_CONSTANT, tag);
				g_tag_set = tag;
			}
		}

		// The lightmap's top level, locked for point reads (DXT1/3/5 colour, 8888).
		struct texels
		{
			IDirect3DTexture9* tex = nullptr;
			D3DSURFACE_DESC desc {};
			D3DLOCKED_RECT lr {};

			bool lock(IDirect3DTexture9* t)
			{
				if (FAILED(t->GetLevelDesc(0, &desc)))
					return false;
				if (desc.Format != D3DFMT_DXT1 && desc.Format != D3DFMT_DXT3 && desc.Format != D3DFMT_DXT5 &&
					desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8)
					return false;
				if (FAILED(t->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits)
					return false;
				tex = t;
				return true;
			}

			~texels()
			{
				if (tex)
					tex->UnlockRect(0);
			}

			// rgb 0-1 at uv, nearest texel, wrapped.
			void at(float u, float v, float* rgb) const
			{
				const float fu = u - std::floor(u), fv = v - std::floor(v);
				const UINT x = std::min(desc.Width - 1, static_cast<UINT>(fu * desc.Width));
				const UINT y = std::min(desc.Height - 1, static_cast<UINT>(fv * desc.Height));
				const auto* p = static_cast<const unsigned char*>(lr.pBits);
				if (desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8)
				{
					const unsigned char* t = p + static_cast<size_t>(y) * lr.Pitch + x * 4;
					rgb[0] = t[2] / 255.0f; rgb[1] = t[1] / 255.0f; rgb[2] = t[0] / 255.0f;
					return;
				}
				const size_t block = desc.Format == D3DFMT_DXT1 ? 8 : 16;
				const unsigned char* b = p + static_cast<size_t>(y / 4) * lr.Pitch + (x / 4) * block +
					(desc.Format == D3DFMT_DXT1 ? 0 : 8);
				const uint16_t c0 = *reinterpret_cast<const uint16_t*>(b);
				const uint16_t c1 = *reinterpret_cast<const uint16_t*>(b + 2);
				const uint32_t bits = *reinterpret_cast<const uint32_t*>(b + 4);
				const uint32_t index = (bits >> (2 * ((y % 4) * 4 + (x % 4)))) & 3;
				float e0[3], e1[3];
				auto rgb565 = [](uint16_t c, float* out)
				{
					out[0] = ((c >> 11) & 31) / 31.0f;
					out[1] = ((c >> 5) & 63) / 63.0f;
					out[2] = (c & 31) / 31.0f;
				};
				rgb565(c0, e0);
				rgb565(c1, e1);
				// DXT1 with c0 <= c1 is the 3-colour + transparent-black mode; DXT3/5 colour is always 4-colour.
				const bool four = desc.Format != D3DFMT_DXT1 || c0 > c1;
				for (int c = 0; c < 3; c++)
				{
					switch (index)
					{
					case 0: rgb[c] = e0[c]; break;
					case 1: rgb[c] = e1[c]; break;
					case 2: rgb[c] = four ? (2.0f * e0[c] + e1[c]) / 3.0f : 0.5f * (e0[c] + e1[c]); break;
					default: rgb[c] = four ? (e0[c] + 2.0f * e1[c]) / 3.0f : 0.0f; break;
					}
				}
			}
		};

		// The draw's level as a tag, or 0 when its data cannot be read.
		DWORD measure(const draw_key& k, const layout& l, IDirect3DVertexBuffer9* vb, IDirect3DIndexBuffer9* ib,
			IDirect3DTexture9* lightmap, const float* uv_offset)
		{
			D3DINDEXBUFFER_DESC idesc {};
			D3DVERTEXBUFFER_DESC vdesc {};
			if (FAILED(ib->GetDesc(&idesc)) || FAILED(vb->GetDesc(&vdesc)))
				return 0;
			// Write-only video memory reads back undefined data; the map's buffers are managed.
			if ((idesc.Pool == D3DPOOL_DEFAULT && (idesc.Usage & D3DUSAGE_WRITEONLY)) ||
				(vdesc.Pool == D3DPOOL_DEFAULT && (vdesc.Usage & D3DUSAGE_WRITEONLY)))
				return 0;
			const UINT isz = idesc.Format == D3DFMT_INDEX32 ? 4 : 2;
			const bool strip = k.prim == D3DPT_TRIANGLESTRIP;
			const UINT count = strip ? k.prim_count + 2 : k.prim_count * 3;
			if (static_cast<uint64_t>(k.start_index + count) * isz > idesc.Size)
				return 0;

			texels lm;
			if (!lm.lock(lightmap))
				return 0;
			void* idata = nullptr;
			if (FAILED(ib->Lock(k.start_index * isz, count * isz, &idata, D3DLOCK_READONLY)) || !idata)
				return 0;
			void* vdata = nullptr;
			if (FAILED(vb->Lock(0, 0, &vdata, D3DLOCK_READONLY)) || !vdata)
			{
				ib->Unlock();
				return 0;
			}

			auto index = [&](UINT i) -> int64_t
			{
				const int64_t raw = isz == 4 ? static_cast<const uint32_t*>(idata)[i] : static_cast<const uint16_t*>(idata)[i];
				return raw + k.base_vertex;
			};
			const auto* vbytes = static_cast<const unsigned char*>(vdata);
			auto vertex = [&](int64_t v) -> const unsigned char*
			{
				const int64_t at = static_cast<int64_t>(k.vb_offset) + v * k.stride;
				if (v < 0 || at + static_cast<int64_t>(k.stride) > static_cast<int64_t>(vdesc.Size))
					return nullptr;
				return vbytes + at;
			};
			auto lightmap_uv = [&](const unsigned char* v, float* uv)
			{
				const unsigned char* t = v + l.uv_off;
				if (l.uv_type == D3DDECLTYPE_SHORT2 || l.uv_type == D3DDECLTYPE_SHORT4)
				{
					uv[0] = reinterpret_cast<const int16_t*>(t)[0] / 1024.0f;
					uv[1] = reinterpret_cast<const int16_t*>(t)[1] / 1024.0f;
				}
				else
				{
					uv[0] = reinterpret_cast<const float*>(t)[0] / 1024.0f;
					uv[1] = reinterpret_cast<const float*>(t)[1] / 1024.0f;
				}
				uv[0] += uv_offset[0];
				uv[1] += uv_offset[1];
			};

			struct sample { float rgb[3]; float area; };
			std::vector<sample> samples;
			const UINT tris = k.prim_count;
			const UINT step = std::max(1u, tris / kMaxSamples);
			samples.reserve(std::min(tris, kMaxSamples + 1));
			for (UINT t = 0; t < tris; t += step)
			{
				const UINT first = strip ? t : t * 3;
				const unsigned char* v[3];
				bool ok = true;
				for (int c = 0; c < 3 && ok; c++)
					ok = (v[c] = vertex(index(first + c))) != nullptr;
				if (!ok)
					continue;
				float p[3][3], uv[3][2];
				for (int c = 0; c < 3; c++)
				{
					std::memcpy(p[c], v[c] + l.pos_off, sizeof(p[c]));
					lightmap_uv(v[c], uv[c]);
				}
				const float a[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
				const float b[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
				const float cx = a[1] * b[2] - a[2] * b[1], cy = a[2] * b[0] - a[0] * b[2], cz = a[0] * b[1] - a[1] * b[0];
				const float area = 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
				if (!(area > 0.0f) || !std::isfinite(area))
					continue; // strip joins and degenerate triangles
				sample s;
				s.area = area;
				lm.at((uv[0][0] + uv[1][0] + uv[2][0]) / 3.0f, (uv[0][1] + uv[1][1] + uv[2][1]) / 3.0f, s.rgb);
				samples.push_back(s);
			}
			vb->Unlock();
			ib->Unlock();
			if (samples.empty())
				return 0;

			// Per channel: the area-weighted 90th percentile.
			double total = 0.0;
			for (const auto& s : samples)
				total += s.area;
			DWORD level[3] = {};
			std::vector<std::pair<float, float>> channel(samples.size());
			for (int c = 0; c < 3; c++)
			{
				for (size_t i = 0; i < samples.size(); i++)
					channel[i] = { samples[i].rgb[c], samples[i].area };
				std::sort(channel.begin(), channel.end());
				double acc = 0.0;
				float value = channel.back().first;
				for (const auto& [v, w] : channel)
				{
					acc += w;
					if (acc >= 0.9 * total)
					{
						value = v;
						break;
					}
				}
				level[c] = static_cast<DWORD>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
			}
			return kMarker | level[0] << 16 | level[1] << 8 | level[2];
		}
	}

	namespace
	{
		// Whether armour covers this body draw. The game draws the head (FaceGen face and scalp: no normal map, so no s2) and the bare
		// body (bd_m_body_*) under what is worn. Armour that goes by no name does not count: then these stay as they are.
		unsigned int g_helmet_frame = 0, g_chest_frame = 0;

		// texture_names::ripple_class of the draw's diffuse, remembered per texture: tag_draw runs for every draw, on the game's
		// render thread, and the lookup takes a lock.
		uint32_t ripple_class_of(IDirect3DBaseTexture9* texture)
		{
			struct slot { IDirect3DBaseTexture9* texture = nullptr; uint32_t bits = 0; };
			static std::array<slot, 256> cache;
			static uint32_t generation = ~0u;
			if (!texture)
				return 0;
			const uint32_t now = texture_names::generation();
			if (now != generation)
			{
				generation = now;
				cache.fill(slot{});
			}
			slot& s = cache[(reinterpret_cast<uintptr_t>(texture) >> 5) & 255u];
			if (s.texture != texture)
			{
				s.texture = texture;
				s.bits = texture_names::ripple_class(texture);
			}
			return s.bits;
		}

		bool is_covered_body(ffp_state& ffp, unsigned int frame)
		{
			const auto name = texture_names::name_of(ffp.cur_texture(0));
			const bool bare_body = name.find("_body") != std::string::npos;
			if (name.rfind("hd_", 0) == 0)
				g_helmet_frame = frame;
			else if (name.rfind("bd_", 0) == 0 && !bare_body)
				g_chest_frame = frame;
			const auto recent = [&](unsigned int seen) { return seen && frame - seen <= 2; };
			return bare_body ? recent(g_chest_frame) : !(ffp.cur_ps_sampler_mask() & (1u << 2)) && recent(g_helmet_frame);
		}
	}

	void tag_draw(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, INT base_vertex, UINT start_index,
		UINT prim_count, unsigned int frame)
	{
		auto& ffp = ffp_state::get();
		// Re-assert the tag once per frame (a device reset clears texture stage states).
		if (frame != g_tag_frame)
		{
			g_tag_frame = frame;
			g_tag_set = ~0u;
			g_measures_left = kMeasuresPerFrame;
		}
		DWORD tag = 0;
		const uint32_t ripple = ripple_class_of(ffp.cur_texture(0));
		if (ffp.cur_decl_is_skinned())
		{
			// Read from the device: the game applies blend state through state blocks.
			DWORD blend = FALSE, test = FALSE, dest = D3DBLEND_ZERO;
			dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
			dev->GetRenderState(D3DRS_ALPHATESTENABLE, &test);
			dev->GetRenderState(D3DRS_DESTBLEND, &dest);
			// MTD g_BlendMode 2 (_Alp) blends over INVSRCALPHA, mode 4 (Add: ghosts, fire) over ONE, both
			// alpha-tested. Only unbumped _Alp draws are cards: hair, lashes, the FaceGen iris, fringes.
			// The bumped ones (pixel shader samples s2, every *Bmp* shader and FRPG_Ghost) are the
			// see-through ones: fur shells, lace, veils, wing membranes. A character fading out (PS c139.w,
			// the per-draw fade, below 1: a death) is no card either: its hair fades with the body.
			const float* ps = ffp.ps_const_data();
			const bool fading = ps && ps[139 * 4 + 3] < 0.999f;
			const bool card = blend && test && dest == D3DBLEND_INVSRCALPHA &&
				!(ffp.cur_ps_sampler_mask() & (1u << 2)) && !fading;
			tag = card ? kSkinnedCardMarker : !blend && texture_names::is_body_part(ffp.cur_texture(0)) ? (is_covered_body(ffp, frame) ? kWetBodyCoveredMarker : kWetBodyMarker) : kSkinnedMarker;
			if (ripple & texture_names::kRippleEnemy)
				tag |= kOtherCharacterBit;
		}
		else if (ffp.cur_decl_has_blend_indices() && (ripple & texture_names::kRippleCharacter))
		{
			// A weapon or a shield (wp_), or a rigid piece of an enemy: on bones, without weights. A lightmapped draw
			// (below) is never one.
			tag = kRigidPartMarker | ((ripple & texture_names::kRippleEnemy) ? kOtherCharacterBit : 0);
		}
		IDirect3DBaseTexture9* lightmap = ffp.cur_texture(6);
		IDirect3DVertexBuffer9* vb = ffp.stream_vb(0);
		IDirect3DIndexBuffer9* ib = ffp.cur_ib();
		const float* vs = ffp.vs_const_data();
		if (ffp.view_proj_valid() && !ffp.cur_decl_is_skinned() &&
			(ffp.cur_ps_light_flags() & ffp_state::PS_AMBIENT) && (ffp.cur_ps_sampler_mask() & (1u << 6)) &&
			lightmap && lightmap->GetType() == D3DRTYPE_TEXTURE && vb && ib && vs &&
			(prim == D3DPT_TRIANGLELIST || prim == D3DPT_TRIANGLESTRIP) && prim_count)
		{
			draw_key k {};
			k.vb = reinterpret_cast<uintptr_t>(vb);
			k.ib = reinterpret_cast<uintptr_t>(ib);
			k.lightmap = reinterpret_cast<uintptr_t>(lightmap);
			k.decl = reinterpret_cast<uintptr_t>(ffp.last_decl());
			k.vb_offset = ffp.stream_offset(0);
			k.stride = ffp.stream_stride(0);
			k.base_vertex = base_vertex;
			k.start_index = start_index;
			k.prim_count = prim_count;
			k.prim = prim;
			auto& e = g_levels[k];
			e.last_frame = frame;
			// c248 (the lightmap uv offset) is read when measuring, not part of the key: a draw's
			// lightmap sits at one place in its atlas, and a scrolling offset must not re-measure it.
			if (!e.measured && static_cast<int>(frame - e.retry_frame) >= 0 && g_measures_left > 0)
			{
				g_measures_left--;
				const layout l = layout_of(ffp.last_decl(), k.stride);
				e.tag = l.valid ? measure(k, l, vb, ib, static_cast<IDirect3DTexture9*>(lightmap), &vs[248 * 4]) : 0;
				// An unreadable layout will not get better; anything else that failed is tried again.
				e.measured = e.tag != 0 || !l.valid;
				e.retry_frame = frame + kRetryFrames;
			}
			tag = e.tag;
			// FRPG_Snow with a key light lobe (PS c169.z, the slime) scales that lobe by a near-black lightmap: it gets none of
			// that light. Remix's sun, through a gap in the roof, took a full reflection (148 against the game's 50).
			if (ffp.cur_ps_swamp_pass() == ffp_state::swamp_pass::Surface && ffp.ps_const_data() && ffp.ps_const_data()[169 * 4 + 2] != 0.0f)
				tag = kNoSunMarker;
		}
		// A waterfall's mesh, when nothing above took the channel.
		if (!tag && (ripple & texture_names::kRippleWaterfall) && !waterfall::landing_seen())
			tag = kFallingWaterMarker;
		set_tag(dev, tag);

		if (frame - g_last_prune > kPruneFrames)
		{
			g_last_prune = frame;
			std::erase_if(g_levels, [&](const auto& kv) { return frame - kv.second.last_frame > kPruneFrames; });
		}
	}

	void untag(IDirect3DDevice9* dev)
	{
		set_tag(dev, 0);
	}
}
