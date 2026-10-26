#include "std_include.hpp"
#include "texture_scroll.hpp"

#include "shared/common/ffp_state.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace comp::game::texture_scroll
{
	namespace
	{
		using shared::common::ffp_state;

		// Wave tiles per texture coordinate in the ds1_water normal map (scripts/gen_water_material.py).
		constexpr double kTiles = 8.0;
		// The slope the normal map was authored with, in the units of c126 * c134.i * c141.i:
		// gen_water_material.py baked capture water1's river (c126 0.7) with the RMS share of its
		// height pass weights (0.18, 0.18, 0.1) per layer, kTiles tiles, and the reflection match
		// c124 * fov_x / 2 = 0.4 * 1.2218 / 2.
		constexpr double kAuthoredSlope = 0.7 * 0.193391 * kTiles * 0.2443669;

		struct mesh_key
		{
			uintptr_t vb, ib;
			INT base_vertex;
			UINT start_index, prim_count;

			bool operator==(const mesh_key&) const = default;
		};

		struct mesh_key_hash
		{
			size_t operator()(const mesh_key& k) const
			{
				size_t h = 1469598103u;
				auto mix = [&](size_t v) { h ^= v + 0x9e3779b9 + (h << 6) + (h >> 2); };
				mix(k.vb); mix(k.ib); mix(static_cast<size_t>(k.base_vertex)); mix(k.start_index); mix(k.prim_count);
				return h;
			}
		};

		// A water mesh's wave layers from its HeightMap draw: tiles per mesh uv (c141), offsets in
		// tiles (c246, c247.xy) and weights (c134).
		struct water_layers
		{
			float scale[3] = {};
			double offset1[2] = {}, offset2[2] = {};
			float weight[3] = {};
			unsigned int frame = 0;
		};

		std::unordered_map<mesh_key, water_layers, mesh_key_hash> g_water;
		unsigned int g_water_frame = 0;
		bool g_layer2_set = false;

		mesh_key key_of(IDirect3DVertexBuffer9* vb, IDirect3DIndexBuffer9* ib, INT base_vertex, UINT start_index, UINT prim_count)
		{
			return { reinterpret_cast<uintptr_t>(vb), reinterpret_cast<uintptr_t>(ib), base_vertex, start_index, prim_count };
		}

		// D3D's 2D texture transform: (u, v, 1) times the matrix, uniform scale and translation in _31 / _32.
		void set_transform(IDirect3DDevice9* dev, DWORD stage, float scale, float u, float v)
		{
			D3DMATRIX m = {};
			m._11 = scale;
			m._22 = scale;
			m._31 = u;
			m._32 = v;
			m._33 = 1.0f;
			m._44 = 1.0f;
			dev->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + stage), &m);
			dev->SetTextureStageState(stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
		}

		double wrap(double x) { return x - std::floor(x); }

		DWORD to_byte(float v)
		{
			return static_cast<DWORD>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
		}

		void apply_water(IDirect3DDevice9* dev, const water_layers& w, const float* ps)
		{
			// How strongly the water reflects seen face-on (PS c130 * c131) and the tint of its
			// reflection (c132), for the runtime's water Fresnel.
			const float face_on = ps[130 * 4] * ps[131 * 4];
			const float* tint = &ps[132 * 4];
			dev->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(to_byte(face_on), to_byte(tint[0]), to_byte(tint[1]), to_byte(tint[2])));

			const float wave_height = ps[126 * 4];
			// The normal map repeats every texture coordinate, so whole coordinates drop out of the
			// offsets: they stay small however long the game has been scrolling.
			const double s1 = w.scale[0] / kTiles, s2 = w.scale[1] / kTiles;
			const double t1[2] = { wrap(w.offset1[0] / kTiles), wrap(w.offset1[1] / kTiles) };
			set_transform(dev, 0, static_cast<float>(s1), static_cast<float>(t1[0]), static_cast<float>(t1[1]));

			// Layer 2 from layer 1's uv: uv2 = uv * s2 + t2 = (uv1 - t1) * s2 / s1 + t2.
			const double ratio = s2 / s1;
			const double o[2] = { wrap(w.offset2[0] / kTiles - t1[0] * ratio), wrap(w.offset2[1] / kTiles - t1[1] * ratio) };
			set_transform(dev, 1, static_cast<float>(ratio), static_cast<float>(o[0]), static_cast<float>(o[1]));

			const float strength[2] = {
				static_cast<float>(wave_height * w.weight[0] * w.scale[0] / kAuthoredSlope),
				static_cast<float>(wave_height * std::hypot(w.weight[1] * w.scale[1], w.weight[2] * w.scale[2]) / kAuthoredSlope),
			};
			D3DXFLOAT16 halfs[2];
			D3DXFloat32To16Array(halfs, strength, 2);
			WORD bits[2];
			std::memcpy(bits, halfs, sizeof(bits));
			dev->SetTextureStageState(1, D3DTSS_CONSTANT, static_cast<DWORD>(bits[0]) | static_cast<DWORD>(bits[1]) << 16);
			g_layer2_set = true;
		}
	}

	void observe_water_heightmap(const float* vs_const, const float* ps_const, IDirect3DVertexBuffer9* vb,
		IDirect3DIndexBuffer9* ib, INT base_vertex, UINT start_index, UINT prim_count, unsigned int frame)
	{
		if (!vs_const || !ps_const || !vb || !ib)
			return;
		if (frame != g_water_frame)
		{
			// Meshes not drawn last frame are gone or out of view.
			std::erase_if(g_water, [&](const auto& kv) { return kv.second.frame + 1 < frame; });
			g_water_frame = frame;
		}
		if (vs_const[141 * 4 + 0] == 0.0f || vs_const[141 * 4 + 1] == 0.0f)
			return;
		auto& w = g_water[key_of(vb, ib, base_vertex, start_index, prim_count)];
		for (int i = 0; i < 3; i++)
		{
			w.scale[i] = vs_const[141 * 4 + i];
			w.weight[i] = ps_const[134 * 4 + i];
		}
		for (int i = 0; i < 2; i++)
		{
			w.offset1[i] = vs_const[246 * 4 + i];
			w.offset2[i] = vs_const[247 * 4 + i];
		}
		w.frame = frame;
	}

	bool apply(IDirect3DDevice9* dev, bool overlay, bool water_surface, IDirect3DVertexBuffer9* vb,
		IDirect3DIndexBuffer9* ib, INT base_vertex, UINT start_index, UINT prim_count, unsigned int frame)
	{
		auto& ffp = ffp_state::get();
		if (water_surface)
		{
			// Water with a height pass takes its layers. Without one (the WaterWave wake rings) its
			// own uv scroll applies, as for any other surface.
			const auto it = g_water.find(key_of(vb, ib, base_vertex, start_index, prim_count));
			const float* ps = ffp.ps_const_data();
			if (it != g_water.end() && it->second.frame == frame && ps)
			{
				apply_water(dev, it->second, ps);
				return true;
			}
		}

		const uint32_t reg_flag = overlay ? ffp_state::VS_TEX_SCROLL_1 : ffp_state::VS_TEX_SCROLL_0;
		const float* vs = ffp.vs_const_data();
		if (!vs || !(ffp.cur_vs_tex_scroll() & reg_flag))
			return false;
		const float* offset = &vs[(overlay ? 247 : 246) * 4];
		if (offset[0] == 0.0f && offset[1] == 0.0f)
			return false;
		set_transform(dev, 0, 1.0f, offset[0], offset[1]);
		return true;
	}

	void clear(IDirect3DDevice9* dev)
	{
		dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		if (g_layer2_set)
		{
			dev->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
			dev->SetTextureStageState(1, D3DTSS_CONSTANT, 0);
			// The game's own texture factor back (and stage 1 as the FFP setup leaves it).
			ffp_state::get().clear_draw_tint(dev);
			g_layer2_set = false;
		}
	}
}
