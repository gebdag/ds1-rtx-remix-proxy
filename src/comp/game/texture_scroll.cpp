#include "std_include.hpp"
#include "texture_scroll.hpp"
#include "env_reflection.hpp"
#include "map_lights.hpp"

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
		// The game's horizontal field of view.
		constexpr double kFovX = 1.2218;
		// Remix's water normal tilts by this much per unit of the game's wave normal slope: the
		// reflection match c124 * fov_x / 2 (c124 = 0.4, the game's reflection shift).
		constexpr double kSlopeMatch = 0.4 * kFovX / 2.0;
		// The slope the normal map was authored with, in the units of c126 * c134.i * c141.i:
		// gen_water_material.py baked capture water1's river (c126 0.7) with the RMS share of its
		// height pass weights (0.18, 0.18, 0.1) per layer, kTiles tiles, and kSlopeMatch.
		constexpr double kAuthoredSlope = 0.7 * 0.193391 * kTiles * kSlopeMatch;

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
		// tiles (c246, c247.xy, c248.xy) and weights (c134).
		struct water_layers
		{
			float scale[3] = {};
			double offset1[2] = {}, offset2[2] = {}, offset3[2] = {};
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
		// The transform never reads _41, which carries payload for the runtime.
		void set_transform(IDirect3DDevice9* dev, DWORD stage, float scale, float u, float v, float payload = 0.0f)
		{
			D3DMATRIX m = {};
			m._11 = scale;
			m._22 = scale;
			m._31 = u;
			m._32 = v;
			m._33 = 1.0f;
			m._41 = payload;
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
			// How strongly the water reflects seen face-on (PS c130 * c131) and how much of it comes back
			// (c132), for the runtime's water Fresnel. c132 is a colour: mauve in the Depths, orange in the
			// Burg, teal at Ash Lake. It suits the game's reflection, a dim low-resolution image. Over a real
			// reflection of the lit scene it reads as a coloured sheen, so only its strength stays. At
			// c131 0 the game reflects nothing and c132 never shows: no strength from it then.
			const float face_on = ps[130 * 4] * ps[131 * 4];
			const float reflection_level = ps[131 * 4] > 0.0f ? 0.2126f * ps[132 * 4] + 0.7152f * ps[132 * 4 + 1] + 0.0722f * ps[132 * 4 + 2] : 1.0f;
			// Ash Lake (m13_02) shows the game's own reflection, tinted c132 and weighted c131, over what its bed and its murk (c127.w) leave.
			const bool game_look = ((map_lights::current_map_id() >> 16) & 0xFFFF) == ((13u << 8) | 2u);
			if (game_look)
			{
				const float r = ps[131 * 4];
				// [FFP] WaterReflectionScale, per area: see config.hpp.
				const float* scale = shared::common::config::get().ffp.water_reflection_scale;
				dev->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(to_byte(ps[127 * 4 + 3]), to_byte(r * ps[132 * 4] * std::max(scale[0], 0.0f)),
					to_byte(r * ps[132 * 4 + 1] * std::max(scale[1], 0.0f)), to_byte(r * ps[132 * 4 + 2] * std::max(scale[2], 0.0f))));
				dev->SetTextureStageState(3, D3DTSS_BUMPENVLOFFSET, 0xD9000001u);
			}
			else
				dev->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(to_byte(face_on), to_byte(reflection_level), to_byte(reflection_level), to_byte(reflection_level)));

			const float wave_height = ps[126 * 4];
			// The normal map repeats every texture coordinate, so whole coordinates drop out of the
			// offsets: they stay small however long the game has been scrolling.
			const double s1 = w.scale[0] / kTiles, s2 = w.scale[1] / kTiles;
			const double t1[2] = { wrap(w.offset1[0] / kTiles), wrap(w.offset1[1] / kTiles) };
			set_transform(dev, 0, static_cast<float>(s1), static_cast<float>(t1[0]), static_cast<float>(t1[1]));

			// The game's refraction smear: it reads the scene behind the water at the screen uv shifted by
			// its wave normal's xz * c125 (* vertex alpha), a view angle of 2 tan(fov_x / 2) per unit of uv.
			// Per unit of Remix's water normal tilt that is c125 * 2 tan(fov_x / 2) / kSlopeMatch.
			const double smear = ps[125 * 4] * 2.0 * std::tan(kFovX / 2.0) / kSlopeMatch;

			// Layers 2 and 3 from layer 1's uv: uv_i = uv * s_i + t_i = (uv1 - t1) * s_i / s1 + t_i.
			const double ratio = s2 / s1;
			const double o[2] = { wrap(w.offset2[0] / kTiles - t1[0] * ratio), wrap(w.offset2[1] / kTiles - t1[1] * ratio) };
			set_transform(dev, 1, static_cast<float>(ratio), static_cast<float>(o[0]), static_cast<float>(o[1]),
				static_cast<float>(smear));

			// The game sums all three layers: they share a scale but scroll apart, and the sparse ridges of
			// one fill the flat gaps of the others. Layer 3 goes in stage 2's transform, its strength in _41.
			const double s3 = w.scale[2] / kTiles;
			const double ratio3 = s3 / s1;
			const double o3[2] = { wrap(w.offset3[0] / kTiles - t1[0] * ratio3), wrap(w.offset3[1] / kTiles - t1[1] * ratio3) };
			set_transform(dev, 2, static_cast<float>(ratio3), static_cast<float>(o3[0]), static_cast<float>(o3[1]),
				static_cast<float>(wave_height * w.weight[2] * w.scale[2] / kAuthoredSlope));

			const float strength[2] = {
				static_cast<float>(wave_height * w.weight[0] * w.scale[0] / kAuthoredSlope),
				static_cast<float>(wave_height * w.weight[1] * w.scale[1] / kAuthoredSlope),
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
			w.offset3[i] = vs_const[248 * 4 + i];
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
			dev->SetTextureStageState(2, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
			dev->SetTextureStageState(1, D3DTSS_CONSTANT, 0);
			dev->SetTextureStageState(3, D3DTSS_BUMPENVLOFFSET, 0);
			env_reflection::forget();
			// The game's own texture factor back (and stage 1 as the FFP setup leaves it).
			ffp_state::get().clear_draw_tint(dev);
			g_layer2_set = false;
		}
	}
}
