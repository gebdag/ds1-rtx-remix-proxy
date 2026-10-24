#include "std_include.hpp"
#include "swamp.hpp"

#include "shared/common/remix_api.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace comp::game::swamp
{
	namespace
	{
		// The surface's constants, from the frame's largest surface draw and its height pass.
		struct surface_values
		{
			float deep[3] = {};          // PS c170
			float crust[3] = {};         // PS c100
			float reflect[3] = {};       // PS c101
			float env[3] = {};           // PS c87
			float ramps[3] = {};         // PS c172.y, c172.z
			float reflect_ramps[3] = {}; // PS c173.xyz
			float uv[3] = {};            // VS c145.x, VS c144.x, PS c172.x
			float span_per_depth = 0.0f; // VS c142.x * 8 / c142.w
			float fade = 0.0f;           // PS c163.z
			float layer_scale[3] = {};   // height pass VS c143.xyz
			float layer_weight[3] = {};  // height pass PS c171.xyz
			float scene_diffuse = 0.0f;  // c100.x of most of the frame's map geometry

			bool operator==(const surface_values& o) const { return std::memcmp(this, &o, sizeof(surface_values)) == 0; }
		};

		// The WaterWave ring mesh maps its local xy to ring texture UV 0.5 - 0.5455 * xy (an octagon of
		// local radius ~0.92 reaching UV 0 and 1; measured on the game's mesh).
		constexpr float kRingUvPerLocal = 0.5455f;
		constexpr size_t kMaxRings = 16; // the runtime's ds1SwampMaxRings

		// Ring texture coordinates from world x, z: u = u_x * x + u_z * z + u_1, v likewise.
		struct ring
		{
			float u[3], v[3], strength;
		};

		// The frame being drawn.
		unsigned int g_frame = ~0u;
		surface_values g_current;
		unsigned int g_current_prims = 0;
		bool g_have_layers = false;
		IDirect3DSurface9* g_height_target = nullptr; // compared, never dereferenced
		std::vector<ring> g_rings;
		// Map materials' c100.x, weighted by primitives (a few distinct values per frame).
		std::vector<std::pair<float, unsigned int>> g_map_diffuse;

		// The last complete frame, and what Remix has.
		surface_values g_ready;
		bool g_have_ready = false;
		std::vector<ring> g_rings_ready;
		surface_values g_sent;
		bool g_sent_once = false;
		std::string g_rings_sent;
		unsigned int g_updated_frame = ~0u;

		void close_frame()
		{
			if (g_current_prims > 0 && g_have_layers)
			{
				g_ready = g_current;
				unsigned int best = 0;
				for (const auto& [value, prims] : g_map_diffuse)
					if (prims > best)
					{
						best = prims;
						g_ready.scene_diffuse = value;
					}
				g_have_ready = true;
			}
			g_map_diffuse.clear();
			g_rings_ready = g_rings;
			g_current_prims = 0;
			g_have_layers = false;
			g_height_target = nullptr;
			g_rings.clear();
		}

		void begin(unsigned int frame)
		{
			if (frame == g_frame)
				return;
			close_frame();
			g_frame = frame;
		}

		IDirect3DSurface9* render_target(IDirect3DDevice9* dev)
		{
			IDirect3DSurface9* rt = nullptr;
			if (!dev || FAILED(dev->GetRenderTarget(0, &rt)) || !rt)
				return nullptr;
			rt->Release();
			return rt;
		}

		bool set(const char* key, const std::string& value)
		{
			auto& api = shared::common::remix_api::get();
			return api.m_bridge.SetConfigVariable &&
				api.m_bridge.SetConfigVariable(key, value.c_str()) == REMIXAPI_ERROR_CODE_SUCCESS;
		}

		std::string vec3(const float* v) { return std::format("{}, {}, {}", v[0], v[1], v[2]); }

		void send(const surface_values& v)
		{
			bool ok = true;
			ok &= set("rtx.ds1Swamp.deepColour", vec3(v.deep));
			ok &= set("rtx.ds1Swamp.crustColour", vec3(v.crust));
			ok &= set("rtx.ds1Swamp.reflectColour", vec3(v.reflect));
			ok &= set("rtx.ds1Swamp.envColour", vec3(v.env));
			ok &= set("rtx.ds1Swamp.ramps", vec3(v.ramps));
			ok &= set("rtx.ds1Swamp.reflectRamps", vec3(v.reflect_ramps));
			ok &= set("rtx.ds1Swamp.uvScales", vec3(v.uv));
			ok &= set("rtx.ds1Swamp.layerScale", vec3(v.layer_scale));
			ok &= set("rtx.ds1Swamp.layerWeight", vec3(v.layer_weight));
			ok &= set("rtx.ds1Swamp.spanPerDepth", std::format("{}", v.span_per_depth));
			ok &= set("rtx.ds1Swamp.fadeDistance", std::format("{}", v.fade));
			if (v.scene_diffuse > 0.0f)
				ok &= set("rtx.ds1Swamp.sceneDiffuseScale", std::format("{}", v.scene_diffuse));
			shared::common::log("Swamp", ok
				? std::format("game swamp sent: deep {}, crust {}, layers {} x {}, scene c100 {}", vec3(v.deep), vec3(v.crust), vec3(v.layer_scale), vec3(v.layer_weight), v.scene_diffuse)
				: std::string("Remix refused the swamp values (SetConfigVariable)"));
		}

		std::string rings_text(const std::vector<ring>& rings)
		{
			std::string text;
			for (const auto& r : rings)
				text += std::format("{:.5f},{:.5f},{:.4f},{:.5f},{:.5f},{:.4f},{:.5f};", r.u[0], r.u[1], r.u[2], r.v[0], r.v[1], r.v[2], r.strength);
			return text;
		}
	}

	void observe(pass kind, const float* vs, const float* ps, IDirect3DDevice9* dev, unsigned int frame, unsigned int prim_count)
	{
		if (!vs || !ps)
			return;
		begin(frame);
		const auto c = [](const float* regs, int r, int k) { return regs[r * 4 + k]; };
		if (kind == pass::Surface)
		{
			if (prim_count < g_current_prims)
				return;
			surface_values& v = g_current;
			for (int k = 0; k < 3; k++)
			{
				v.deep[k] = c(ps, 170, k);
				v.crust[k] = c(ps, 100, k);
				v.reflect[k] = c(ps, 101, k);
				v.env[k] = c(ps, 87, k);
				v.reflect_ramps[k] = c(ps, 173, k);
			}
			v.ramps[0] = c(ps, 172, 1);
			v.ramps[1] = c(ps, 172, 2);
			v.uv[0] = c(vs, 145, 0);
			v.uv[1] = c(vs, 144, 0);
			v.uv[2] = c(ps, 172, 0);
			v.span_per_depth = c(vs, 142, 3) != 0.0f ? c(vs, 142, 0) * 8.0f / c(vs, 142, 3) : 0.0f;
			v.fade = c(ps, 163, 2);
			g_current_prims = prim_count;
		}
		else if (kind == pass::HeightMap)
		{
			for (int k = 0; k < 3; k++)
			{
				g_current.layer_scale[k] = c(vs, 143, k);
				g_current.layer_weight[k] = c(ps, 171, k);
			}
			g_have_layers = true;
			g_height_target = render_target(dev);
		}
		else if (kind == pass::WaveRing)
		{
			// Only the rings stamped into the swamp's height field (water has its own WaterWave rings).
			if (!g_height_target || render_target(dev) != g_height_target || g_rings.size() >= kMaxRings)
				return;
			// Local xy -> world x, z: rows c8 and c10 of bone 0 (the disc lies in its local xy plane).
			// Inverted, then the mesh's local -> texture mapping 0.5 - kRingUvPerLocal * xy, plus VS c246.
			const float a = c(vs, 8, 0), b = c(vs, 8, 1), d = c(vs, 10, 0), e = c(vs, 10, 1);
			const float det = a * e - b * d;
			const float strength = c(ps, 100, 0) * c(ps, 100, 3) * c(ps, 139, 0) * c(ps, 139, 3);
			if (std::abs(det) < 1e-8f || strength <= 0.0f)
				return;
			// The game's world matrices are camera-relative; Remix's world is absolute (ffp_state::world_offset).
			float ox = 0.0f, oy = 0.0f, oz = 0.0f;
			shared::common::ffp_state::get().world_offset(ox, oy, oz);
			const float tx = c(vs, 8, 3) + ox, tz = c(vs, 10, 3) + oz;
			const float inv[2][2] = { { e / det, -b / det }, { -d / det, a / det } };
			ring r {};
			float* rows[2] = { r.u, r.v };
			for (int k = 0; k < 2; k++)
			{
				rows[k][0] = -kRingUvPerLocal * inv[k][0];
				rows[k][1] = -kRingUvPerLocal * inv[k][1];
				rows[k][2] = 0.5f + kRingUvPerLocal * (inv[k][0] * tx + inv[k][1] * tz) + c(vs, 246, k);
			}
			r.strength = strength;
			g_rings.push_back(r);
		}
	}

	void observe_map_diffuse(const float* ps_const, unsigned int frame, unsigned int prim_count)
	{
		if (!ps_const)
			return;
		begin(frame);
		const float value = ps_const[100 * 4];
		for (auto& [v, prims] : g_map_diffuse)
			if (v == value)
			{
				prims += prim_count;
				return;
			}
		g_map_diffuse.emplace_back(value, prim_count);
	}

	void frame_update(unsigned int frame)
	{
		if (frame == g_updated_frame)
			return;
		g_updated_frame = frame;
		begin(frame);
		if (!shared::common::remix_api::is_initialized())
			return;
		if (g_have_ready && !(g_sent_once && g_sent == g_ready))
		{
			send(g_ready);
			g_sent = g_ready;
			g_sent_once = true;
		}
		if (std::string text = rings_text(g_rings_ready); text != g_rings_sent)
		{
			static bool logged = false;
			if (!logged && !text.empty())
			{
				logged = true;
				shared::common::log("Swamp", std::format("first wake rings sent: {}", text));
			}
			set("rtx.ds1Swamp.rings", text);
			g_rings_sent = std::move(text);
		}
	}
}
