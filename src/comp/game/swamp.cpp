#include "std_include.hpp"
#include "swamp.hpp"
#include "map_lights.hpp"
#include "texture_names.hpp"

#include "shared/common/remix_api.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <unordered_map>
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
			float height_scale = 0.0f;   // PS c169.x, the height's weight on the slope (g_SnowHeight)
			float height_in_alpha = 0.0f; // 1: the height texture is not the colour texture (surface_texture)
			float metres_per_uv = 0.0f;  // the surface mesh's size per texture coordinate
			float scene_diffuse = 0.0f;  // c100.x of most of the frame's map geometry

			bool operator==(const surface_values& o) const { return std::memcmp(this, &o, sizeof(surface_values)) == 0; }
		};

		// The WaterWave ring mesh maps its local xy to ring texture UV 0.5 - 0.5455 * xy (an octagon of
		// local radius ~0.92 reaching UV 0 and 1; measured on the game's mesh).
		constexpr float kRingUvPerLocal = 0.5455f;
		constexpr size_t kMaxRings = 64; // the runtime's ds1SwampMaxRings

		// Ring texture coordinates from world x, z: u = u_x * x + u_z * z + u_1, v likewise.
		struct ring
		{
			float u[3], v[3], strength;
			int slot = -1;   // which footprint texture (kStampNames), -1: a wake ring, whose texture is the colour texture's alpha
		};

		// The footprint stamps Painted World's snow takes, by the texture they stamp with: the player's and Priscilla's. Bound by the game this frame only.
		constexpr const char* kStampNames[] = { "s00029", "s12730" };
		IDirect3DBaseTexture9* g_stamp_textures[std::size(kStampNames)] = {};
		// A stamp mesh's texture coordinates from its local x, z: u = m[0] x + m[1] z + m[2], v = m[3] x + m[4] z + m[5].
		struct stamp_mesh { float m[6]; bool ok; };
		std::unordered_map<uint64_t, stamp_mesh> g_stamp_meshes;

		int stamp_slot(IDirect3DBaseTexture9* texture)
		{
			if (!texture)
				return -1;
			const auto name = texture_names::name_of(texture);
			for (size_t i = 0; i < std::size(kStampNames); i++)
				if (name == kStampNames[i])
					return static_cast<int>(i);
			return -1;
		}

		// The frame being drawn.
		unsigned int g_frame = ~0u;
		surface_values g_current;
		unsigned int g_current_prims = 0;
		bool g_have_layers = false;
		IDirect3DSurface9* g_height_target = nullptr; // compared, never dereferenced
		std::vector<ring> g_rings;
		// Map materials' c100.x, weighted by primitives (a few distinct values per frame), over the whole
		// frame and per lightmap texture.
		std::vector<std::pair<float, unsigned int>> g_map_diffuse;
		std::unordered_map<IDirect3DBaseTexture9*, std::vector<std::pair<float, unsigned int>>> g_lightmap_diffuse;
		// The largest surface's lightmap texture (lightmapped FRPG_Snow_Lit*), else null.
		IDirect3DBaseTexture9* g_current_lightmap = nullptr;

		void tally(std::vector<std::pair<float, unsigned int>>& list, float value, unsigned int prims)
		{
			for (auto& [v, n] : list)
				if (v == value)
				{
					n += prims;
					return;
				}
			list.emplace_back(value, prims);
		}

		float dominant(const std::vector<std::pair<float, unsigned int>>& list)
		{
			float best_value = 0.0f;
			unsigned int best = 0;
			for (const auto& [value, prims] : list)
				if (prims > best)
				{
					best = prims;
					best_value = value;
				}
			return best_value;
		}
		// Each surface colour texture's height texture, from the height passes (pointers compared only).
		std::unordered_map<IDirect3DBaseTexture9*, IDirect3DBaseTexture9*> g_height_of;
		// Colour + height textures baked into one (surface_texture), per pair.
		struct baked
		{
			UINT colour_w = 0, height_w = 0;
			IDirect3DTexture9* texture = nullptr;
		};
		std::map<std::pair<IDirect3DBaseTexture9*, IDirect3DBaseTexture9*>, baked> g_baked;
		// Metres per texture coordinate of each surface mesh range, measured once.
		std::unordered_map<uint64_t, float> g_mesh_scale;

		// The last complete frame, and what Remix has.
		surface_values g_ready;
		bool g_have_ready = false;
		std::vector<ring> g_rings_ready;
		// The height layers' scroll (height pass VS c246.xy, c247.xy, c248.xy, wrapped to [0, 1)): it moves
		// every frame on a flowing surface, so it goes out on its own, like the rings.
		float g_offsets[6] = {};
		float g_offsets_ready[6] = {};
		std::string g_offsets_sent;
		surface_values g_sent;
		bool g_sent_once = false;
		std::string g_rings_sent;
		unsigned int g_updated_frame = ~0u;

		void close_frame()
		{
			if (g_current_prims > 0 && g_have_layers)
			{
				g_ready = g_current;
				// The map geometry around a lightmapped surface shares its lightmap texture (Gaping Dragon's
				// slime and its floor, c100 0.6, where the frame's walls are 0.39); else the frame's.
				const auto lit = g_current_lightmap ? g_lightmap_diffuse.find(g_current_lightmap) : g_lightmap_diffuse.end();
				g_ready.scene_diffuse = lit != g_lightmap_diffuse.end() ? dominant(lit->second) : dominant(g_map_diffuse);
				g_have_ready = true;
			}
			g_map_diffuse.clear();
			g_lightmap_diffuse.clear();
			g_rings_ready = g_rings;
			std::fill(std::begin(g_stamp_textures), std::end(g_stamp_textures), nullptr);
			std::memcpy(g_offsets_ready, g_offsets, sizeof(g_offsets));
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
			ok &= set("rtx.ds1Swamp.heightScale", std::format("{}", v.height_scale));
			ok &= set("rtx.ds1Swamp.heightInAlpha", v.height_in_alpha != 0.0f ? "True" : "False");
			if (v.metres_per_uv > 0.0f)
				ok &= set("rtx.ds1Swamp.meshMetresPerUv", std::format("{}", v.metres_per_uv));
			if (v.scene_diffuse > 0.0f)
				ok &= set("rtx.ds1Swamp.sceneDiffuseScale", std::format("{}", v.scene_diffuse));
			shared::common::log("Swamp", ok
				? std::format("game swamp sent: deep {}, crust {}, layers {} x {}, scene c100 {}, height in alpha {}, {} m per uv",
					vec3(v.deep), vec3(v.crust), vec3(v.layer_scale), vec3(v.layer_weight), v.scene_diffuse, v.height_in_alpha, v.metres_per_uv)
				: std::string("Remix refused the swamp values (SetConfigVariable)"));
		}

		IDirect3DBaseTexture9* bound_texture(IDirect3DDevice9* dev, DWORD stage)
		{
			IDirect3DBaseTexture9* t = nullptr;
			if (!dev || FAILED(dev->GetTexture(stage, &t)) || !t)
				return nullptr;
			t->Release();
			return t;
		}

		// A texture's top level as RGBA8 (r, g, b, a per texel): DXT1/3/5 and A8R8G8B8 / X8R8G8B8.
		bool read_rgba(IDirect3DBaseTexture9* texture, std::vector<uint8_t>& out, UINT& width, UINT& height)
		{
			if (!texture || texture->GetType() != D3DRTYPE_TEXTURE)
				return false;
			auto* t2d = static_cast<IDirect3DTexture9*>(texture);
			D3DSURFACE_DESC desc {};
			if (FAILED(t2d->GetLevelDesc(0, &desc)))
				return false;
			const bool dxt = desc.Format == D3DFMT_DXT1 || desc.Format == D3DFMT_DXT3 || desc.Format == D3DFMT_DXT5;
			if (!dxt && desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8)
				return false;
			D3DLOCKED_RECT lr {};
			if (FAILED(t2d->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits)
				return false;
			width = desc.Width;
			height = desc.Height;
			out.assign(static_cast<size_t>(width) * height * 4, 0);
			const auto* p = static_cast<const uint8_t*>(lr.pBits);
			if (!dxt)
			{
				for (UINT y = 0; y < height; y++)
					for (UINT x = 0; x < width; x++)
					{
						const uint8_t* t = p + static_cast<size_t>(y) * lr.Pitch + x * 4;
						uint8_t* o = &out[(static_cast<size_t>(y) * width + x) * 4];
						o[0] = t[2]; o[1] = t[1]; o[2] = t[0];
						o[3] = desc.Format == D3DFMT_A8R8G8B8 ? t[3] : 255;
					}
			}
			else
			{
				const size_t block = desc.Format == D3DFMT_DXT1 ? 8 : 16;
				const size_t colour_at = desc.Format == D3DFMT_DXT1 ? 0 : 8;
				const UINT bw = std::max(1u, (width + 3) / 4), bh = std::max(1u, (height + 3) / 4);
				for (UINT by = 0; by < bh; by++)
					for (UINT bx = 0; bx < bw; bx++)
					{
						const uint8_t* b = p + static_cast<size_t>(by) * lr.Pitch + bx * block;
						const uint8_t* cb = b + colour_at;
						const uint16_t c0 = static_cast<uint16_t>(cb[0] | cb[1] << 8), c1 = static_cast<uint16_t>(cb[2] | cb[3] << 8);
						uint8_t pal[4][4];
						auto expand = [](uint16_t c, uint8_t* o)
						{
							o[0] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
							o[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
							o[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
							o[3] = 255;
						};
						expand(c0, pal[0]);
						expand(c1, pal[1]);
						const bool four = c0 > c1 || desc.Format != D3DFMT_DXT1;
						for (int c = 0; c < 3; c++)
						{
							pal[2][c] = static_cast<uint8_t>(four ? (2 * pal[0][c] + pal[1][c]) / 3 : (pal[0][c] + pal[1][c]) / 2);
							pal[3][c] = static_cast<uint8_t>(four ? (pal[0][c] + 2 * pal[1][c]) / 3 : 0);
						}
						pal[2][3] = 255;
						pal[3][3] = four ? 255 : 0;
						const uint32_t bits = cb[4] | cb[5] << 8 | cb[6] << 16 | static_cast<uint32_t>(cb[7]) << 24;
						uint8_t alpha_pal[8] = {};
						uint64_t alpha_bits = 0;
						if (desc.Format == D3DFMT_DXT5)
						{
							alpha_pal[0] = b[0];
							alpha_pal[1] = b[1];
							for (int i = 2; i < 8; i++)
								alpha_pal[i] = static_cast<uint8_t>(b[0] > b[1] ? ((8 - i) * b[0] + (i - 1) * b[1]) / 7
									: i < 6 ? ((6 - i) * b[0] + (i - 1) * b[1]) / 5 : (i == 6 ? 0 : 255));
							for (int i = 0; i < 6; i++)
								alpha_bits |= static_cast<uint64_t>(b[2 + i]) << (8 * i);
						}
						for (int ty = 0; ty < 4; ty++)
							for (int tx = 0; tx < 4; tx++)
							{
								const UINT x = bx * 4 + tx, y = by * 4 + ty;
								if (x >= width || y >= height)
									continue;
								const int i = ty * 4 + tx;
								uint8_t* o = &out[(static_cast<size_t>(y) * width + x) * 4];
								const uint8_t* c = pal[(bits >> (2 * i)) & 3];
								o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = c[3];
								if (desc.Format == D3DFMT_DXT5)
									o[3] = alpha_pal[(alpha_bits >> (3 * i)) & 7];
								else if (desc.Format == D3DFMT_DXT3)
									o[3] = static_cast<uint8_t>(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
							}
					}
			}
			t2d->UnlockRect(0);
			return true;
		}

		// Bilinear, wrapping (the surface samplers wrap), of an RGBA8 image at uv.
		void sample_rgba(const std::vector<uint8_t>& img, UINT w, UINT h, float u, float v, float out[4])
		{
			const float fx = u * w - 0.5f, fy = v * h - 0.5f;
			const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
			const float ax = fx - x0, ay = fy - y0;
			auto at = [&](int x, int y, int c)
			{
				x = ((x % static_cast<int>(w)) + static_cast<int>(w)) % static_cast<int>(w);
				y = ((y % static_cast<int>(h)) + static_cast<int>(h)) % static_cast<int>(h);
				return img[(static_cast<size_t>(y) * w + x) * 4 + c] / 255.0f;
			};
			for (int c = 0; c < 4; c++)
				out[c] = (at(x0, y0, c) * (1 - ax) + at(x0 + 1, y0, c) * ax) * (1 - ay) +
					(at(x0, y0 + 1, c) * (1 - ax) + at(x0 + 1, y0 + 1, c) * ax) * ay;
		}

		// The colour texture with the height texture's (r + g + b) / 3 in its alpha, at the larger size of
		// the two, every mip box-filtered from the one above. Both tile, so each read at its own scale of
		// the same uv sees what the game's separate textures give there.
		// The Texture Lab's helper finds a baked surface's source by these dumps: width, height, then the top level as Remix hashes it (BGRA).
		// Only where the lab is set up (rtx_comp\\texlab exists), once per pair, at the bake (a load).
		void dump_for_texture_lab(IDirect3DBaseTexture9* colour, IDirect3DBaseTexture9* height, UINT w, UINT h, const std::vector<uint8_t>& rgba)
		{
			const std::string lab = shared::globals::root_path + "\\rtx_comp\\texlab";
			const auto colour_name = texture_names::name_of(colour), height_name = texture_names::name_of(height);
			if (GetFileAttributesA(lab.c_str()) == INVALID_FILE_ATTRIBUTES || colour_name.empty() || height_name.empty())
				return;
			const std::string dir = lab + "\\baked";
			CreateDirectoryA(dir.c_str(), nullptr);
			const std::string path = dir + "\\" + std::string(colour_name) + "__" + std::string(height_name) + ".bin";
			if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
				return;
			std::vector<uint8_t> bgra(rgba);
			for (size_t i = 0; i + 3 < bgra.size(); i += 4)
				std::swap(bgra[i], bgra[i + 2]);
			if (FILE* f = fopen(path.c_str(), "wb"))
			{
				fwrite(&w, sizeof(w), 1, f);
				fwrite(&h, sizeof(h), 1, f);
				fwrite(bgra.data(), 1, bgra.size(), f);
				fclose(f);
			}
		}

		IDirect3DTexture9* bake(IDirect3DDevice9* dev, IDirect3DBaseTexture9* colour, IDirect3DBaseTexture9* height)
		{
			std::vector<uint8_t> a, b;
			UINT aw = 0, ah = 0, bw = 0, bh = 0;
			if (!read_rgba(colour, a, aw, ah) || !read_rgba(height, b, bw, bh))
				return nullptr;
			const UINT w = std::max(aw, bw), h = std::max(ah, bh);
			IDirect3DTexture9* out = nullptr;
			if (FAILED(dev->CreateTexture(w, h, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &out, nullptr)) || !out)
				return nullptr;
			std::vector<uint8_t> level(static_cast<size_t>(w) * h * 4);
			for (UINT y = 0; y < h; y++)
				for (UINT x = 0; x < w; x++)
				{
					float ca[4], cb[4];
					const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
					sample_rgba(a, aw, ah, u, v, ca);
					sample_rgba(b, bw, bh, u, v, cb);
					uint8_t* o = &level[(static_cast<size_t>(y) * w + x) * 4];
					for (int c = 0; c < 3; c++)
						o[c] = static_cast<uint8_t>(std::lround(std::clamp(ca[c], 0.0f, 1.0f) * 255.0f));
					o[3] = static_cast<uint8_t>(std::lround(std::clamp((cb[0] + cb[1] + cb[2]) / 3.0f, 0.0f, 1.0f) * 255.0f));
				}
			dump_for_texture_lab(colour, height, w, h, level);
			UINT lw = w, lh = h;
			for (DWORD l = 0; l < out->GetLevelCount(); l++)
			{
				D3DLOCKED_RECT lr {};
				if (SUCCEEDED(out->LockRect(l, &lr, nullptr, 0)) && lr.pBits)
				{
					for (UINT y = 0; y < lh; y++)
						for (UINT x = 0; x < lw; x++)
						{
							const uint8_t* s = &level[(static_cast<size_t>(y) * lw + x) * 4];
							uint8_t* d = static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch + x * 4;
							d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
						}
					out->UnlockRect(l);
				}
				const UINT nw = std::max(1u, lw / 2), nh = std::max(1u, lh / 2);
				std::vector<uint8_t> next(static_cast<size_t>(nw) * nh * 4);
				for (UINT y = 0; y < nh; y++)
					for (UINT x = 0; x < nw; x++)
						for (int c = 0; c < 4; c++)
						{
							unsigned sum = 0;
							for (UINT dy = 0; dy < 2; dy++)
								for (UINT dx = 0; dx < 2; dx++)
									sum += level[(static_cast<size_t>(std::min(lh - 1, y * 2 + dy)) * lw + std::min(lw - 1, x * 2 + dx)) * 4 + c];
							next[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>(sum / 4);
						}
				level.swap(next);
				lw = nw;
				lh = nh;
			}
			shared::common::log("Swamp", std::format("height texture baked into the colour's alpha: {:#x} + {:#x} -> {}x{}",
				reinterpret_cast<uintptr_t>(colour), reinterpret_cast<uintptr_t>(height), w, h));
			return out;
		}

		UINT top_width(IDirect3DBaseTexture9* t)
		{
			D3DSURFACE_DESC desc {};
			return t && t->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &desc)) ? desc.Width : 0;
		}

		std::string rings_text(const std::vector<ring>& rings)
		{
			std::string text;
			for (const auto& r : rings)
				text += std::format("{:.5f},{:.5f},{:.4f},{:.5f},{:.5f},{:.4f},{:.5f},{};", r.u[0], r.u[1], r.u[2], r.v[0], r.v[1], r.v[2], r.strength, r.slot);
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
			v.height_scale = c(ps, 169, 0);
			// Blighttown's height pass reads the colour texture itself; Gaping Dragon's slime reads its own
			// noise texture (m10_water), which surface_texture bakes into the colour texture's alpha.
			IDirect3DBaseTexture9* colour = bound_texture(dev, 0);
			const auto it = colour ? g_height_of.find(colour) : g_height_of.end();
			v.height_in_alpha = it != g_height_of.end() && it->second && it->second != colour ? 1.0f : 0.0f;
			v.metres_per_uv = 0.0f;
			g_current_lightmap = (shared::common::ffp_state::get().cur_ps_sampler_mask() & (1u << 6)) ? bound_texture(dev, 6) : nullptr;
			g_current_prims = prim_count;
		}
		else if (kind == pass::HeightMap)
		{
			for (int k = 0; k < 3; k++)
			{
				g_current.layer_scale[k] = c(vs, 143, k);
				g_current.layer_weight[k] = c(ps, 171, k);
			}
			// Each layer at mesh uv * c143.i + c246 / c247 / c248.xy: the scroll, wrapped (the texture repeats).
			for (int k = 0; k < 3; k++)
			{
				g_offsets[k] = c(vs, 246 + k, 0) - std::floor(c(vs, 246 + k, 0));
				g_offsets[3 + k] = c(vs, 246 + k, 1) - std::floor(c(vs, 246 + k, 1));
			}
			// The height texture (s2) its surface's colour texture (still on s0) goes with.
			if (IDirect3DBaseTexture9* colour = bound_texture(dev, 0))
				g_height_of[colour] = bound_texture(dev, 2);
			g_have_layers = true;
			g_height_target = render_target(dev);
		}
		else if (kind == pass::WaveRing)
		{
			// Only the rings stamped into the swamp's height field (water has its own WaterWave rings).
			if (!g_height_target || render_target(dev) != g_height_target || g_rings.size() >= kMaxRings)
				return;
			// Footprints go by their own mesh (observe_stamp).
			if (stamp_slot(bound_texture(dev, 0)) >= 0)
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

	void observe_stamp(const float* vs, const float* ps, IDirect3DDevice9* dev, IDirect3DVertexBuffer9* vb, UINT stride, INT base_vertex, UINT min_vertex,
		UINT num_vertices, int pos_offset, int uv_offset, int uv_type, unsigned int frame)
	{
		begin(frame);
		if (!vs || !ps || !vb || !stride || num_vertices < 3 || pos_offset < 0 || uv_offset < 0 || !g_height_target || render_target(dev) != g_height_target ||
			g_rings.size() >= kMaxRings)
			return;
		IDirect3DBaseTexture9* texture = bound_texture(dev, 0);
		const int slot = stamp_slot(texture);
		if (slot < 0)
			return;
		const auto c = [](const float* regs, int r, int k) { return regs[r * 4 + k]; };
		// FRPG_WWS_Dif________________WaterWave: tex.r * tex.a * c100.x * c100.w * vertex x, w * c139.x * c139.w (the vertices are white). The stamp fades through c139.w.
		const float strength = c(ps, 100, 0) * c(ps, 100, 3) * c(ps, 139, 0) * c(ps, 139, 3);
		if (strength <= 0.0f)
			return;
		// Vertex buffers are freed and their addresses reused across maps.
		static unsigned stamp_map = 0;
		if (const unsigned map = map_lights::current_map_id(); map != stamp_map)
		{
			stamp_map = map;
			g_stamp_meshes.clear();
		}
		const uint64_t key = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(vb)) ^ (static_cast<uint64_t>(base_vertex + min_vertex) << 32) ^ num_vertices;
		auto [found, fresh] = g_stamp_meshes.try_emplace(key, stamp_mesh {});
		if (fresh)
		{
			unsigned char* data = nullptr;
			const UINT count = std::min(num_vertices, 16u);
			if (SUCCEEDED(vb->Lock((base_vertex + min_vertex) * stride, count * stride, reinterpret_cast<void**>(&data), D3DLOCK_READONLY)) && data)
			{
				// The mesh lies in its local x, z plane; the first three corners not on a line give its mapping.
				float p[16][3], t[16][2];
				for (UINT i = 0; i < count; i++)
				{
					const unsigned char* at = data + i * stride;
					std::memcpy(p[i], at + pos_offset, sizeof(p[i]));
					if (uv_type == D3DDECLTYPE_FLOAT2 || uv_type == D3DDECLTYPE_FLOAT4)
						std::memcpy(t[i], at + uv_offset, 2 * sizeof(float));
					else
					{
						const auto* s = reinterpret_cast<const int16_t*>(at + uv_offset);
						t[i][0] = s[0] / 1024.0f;
						t[i][1] = s[1] / 1024.0f;
					}
				}
				vb->Unlock();
				for (UINT i = 2; i < count && !found->second.ok; i++)
				{
					const float a = p[1][0] - p[0][0], b = p[1][2] - p[0][2], d = p[i][0] - p[0][0], e = p[i][2] - p[0][2];
					const float det = a * e - b * d;
					if (std::abs(det) < 1e-6f)
						continue;
					for (int k = 0; k < 2; k++)
					{
						const float du1 = t[1][k] - t[0][k], du2 = t[i][k] - t[0][k];
						float* m = found->second.m + k * 3;
						m[0] = (du1 * e - du2 * b) / det;
						m[1] = (du2 * a - du1 * d) / det;
						m[2] = t[0][k] - m[0] * p[0][0] - m[1] * p[0][2];
					}
					found->second.ok = true;
				}
			}
		}
		if (!found->second.ok)
			return;
		// Local x, z -> world x, z: rows c8 and c10 of bone 0, camera-relative like every world matrix here, then inverted.
		const float a = c(vs, 8, 0), b = c(vs, 8, 2), d = c(vs, 10, 0), e = c(vs, 10, 2);
		const float det = a * e - b * d;
		if (std::abs(det) < 1e-8f)
			return;
		float ox = 0.0f, oy = 0.0f, oz = 0.0f;
		shared::common::ffp_state::get().world_offset(ox, oy, oz);
		const float tx = c(vs, 8, 3) + ox, tz = c(vs, 10, 3) + oz;
		const float inv[2][2] = { { e / det, -b / det }, { -d / det, a / det } };
		ring r {};
		float* rows[2] = { r.u, r.v };
		const float* m = found->second.m;
		for (int k = 0; k < 2; k++)
		{
			const float* mk = m + k * 3;
			rows[k][0] = mk[0] * inv[0][0] + mk[1] * inv[1][0];
			rows[k][1] = mk[0] * inv[0][1] + mk[1] * inv[1][1];
			rows[k][2] = mk[2] + c(vs, 246, k) - rows[k][0] * tx - rows[k][1] * tz;
		}
		r.strength = strength;
		r.slot = slot;
		g_rings.push_back(r);
		g_stamp_textures[slot] = texture;
	}

	void stamp_textures(IDirect3DBaseTexture9** out)
	{
		std::copy(std::begin(g_stamp_textures), std::end(g_stamp_textures), out);
	}

	void observe_surface_mesh(IDirect3DVertexBuffer9* vb, UINT stride, INT base_vertex, UINT min_vertex, UINT num_vertices,
		int pos_offset, int uv_offset, int uv_type, unsigned int frame, unsigned int prim_count)
	{
		begin(frame);
		if (!vb || stride == 0 || pos_offset < 0 || uv_offset < 0 || base_vertex < 0 || num_vertices < 2 || prim_count != g_current_prims)
			return;
		const uint64_t key = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(vb)) ^ (static_cast<uint64_t>(base_vertex + min_vertex) << 32) ^ num_vertices;
		auto [it, fresh] = g_mesh_scale.try_emplace(key, 0.0f);
		if (fresh)
		{
			// The median of |dP| / |dUV| over neighbouring vertices: the mesh's metres per texture
			// coordinate (Blighttown's swamp 5, its UV mapping uniform across the surface).
			const UINT count = std::min(num_vertices, 512u);
			unsigned char* data = nullptr;
			if (SUCCEEDED(vb->Lock((base_vertex + min_vertex) * stride, count * stride, reinterpret_cast<void**>(&data), D3DLOCK_READONLY)) && data)
			{
				auto uv_of = [&](const unsigned char* v, float out[2])
				{
					if (uv_type == D3DDECLTYPE_FLOAT2 || uv_type == D3DDECLTYPE_FLOAT4)
						std::memcpy(out, v + uv_offset, 2 * sizeof(float));
					else
					{
						const auto* s = reinterpret_cast<const int16_t*>(v + uv_offset);
						out[0] = s[0] / 1024.0f;
						out[1] = s[1] / 1024.0f;
					}
				};
				std::vector<float> ratios;
				for (UINT i = 1; i < count; i++)
				{
					const unsigned char* a = data + (i - 1) * stride;
					const unsigned char* b = data + i * stride;
					float pa[3], pb[3], ua[2], ub[2];
					std::memcpy(pa, a + pos_offset, sizeof(pa));
					std::memcpy(pb, b + pos_offset, sizeof(pb));
					uv_of(a, ua);
					uv_of(b, ub);
					const float dp = std::sqrt((pa[0] - pb[0]) * (pa[0] - pb[0]) + (pa[1] - pb[1]) * (pa[1] - pb[1]) + (pa[2] - pb[2]) * (pa[2] - pb[2]));
					const float du = std::hypot(ua[0] - ub[0], ua[1] - ub[1]);
					if (du > 1e-4f && dp > 1e-3f && std::isfinite(dp / du))
						ratios.push_back(dp / du);
				}
				vb->Unlock();
				if (!ratios.empty())
				{
					std::nth_element(ratios.begin(), ratios.begin() + ratios.size() / 2, ratios.end());
					it->second = ratios[ratios.size() / 2];
				}
			}
		}
		g_current.metres_per_uv = it->second;
	}

	IDirect3DBaseTexture9* surface_texture(IDirect3DDevice9* dev, IDirect3DBaseTexture9* colour)
	{
		const auto it = colour ? g_height_of.find(colour) : g_height_of.end();
		if (!dev || it == g_height_of.end() || !it->second || it->second == colour)
			return nullptr;
		auto& b = g_baked[{ colour, it->second }];
		// The game frees and re-creates textures at the same addresses across loads: a pair whose sizes
		// changed is baked again.
		const UINT cw = top_width(colour), hw = top_width(it->second);
		if (b.texture && (b.colour_w != cw || b.height_w != hw))
		{
			b.texture->Release();
			b.texture = nullptr;
		}
		if (!b.texture && (b.colour_w != cw || b.height_w != hw || !b.colour_w))
		{
			b.colour_w = cw;
			b.height_w = hw;
			b.texture = bake(dev, colour, it->second);
		}
		return b.texture;
	}

	void observe_map_diffuse(const float* ps_const, unsigned int frame, unsigned int prim_count, IDirect3DBaseTexture9* lightmap)
	{
		if (!ps_const)
			return;
		begin(frame);
		const float value = ps_const[100 * 4];
		tally(g_map_diffuse, value, prim_count);
		if (lightmap)
			tally(g_lightmap_diffuse[lightmap], value, prim_count);
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
		if (std::string text = std::format("{}, {}, {}|{}, {}, {}", g_offsets_ready[0], g_offsets_ready[1], g_offsets_ready[2],
			g_offsets_ready[3], g_offsets_ready[4], g_offsets_ready[5]); g_have_ready && text != g_offsets_sent)
		{
			set("rtx.ds1Swamp.layerOffsetU", text.substr(0, text.find('|')));
			set("rtx.ds1Swamp.layerOffsetV", text.substr(text.find('|') + 1));
			g_offsets_sent = std::move(text);
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
