#include "std_include.hpp"
#include "waterfall.hpp"
#include "camera.hpp"
#include "ffx_instances.hpp"
#include "lightmap_sun.hpp"
#include "map_lights.hpp"
#include "texture_names.hpp"
#include "shared/common/config.hpp"
#include "shared/common/remix_api.hpp"

#include <array>
#include <unordered_map>
#include <unordered_set>

using shared::common::log;

namespace comp::game::waterfall
{
	namespace
	{
		// A body this close to falling water is in it, in metres. Measured from the waist up: a sheet ends in the pool
		// at the feet of anyone wading past it, and water that falls on someone reaches their head first.
		constexpr float kReach = 0.6f;
		constexpr float kBodyHeight = 1.8f;
		constexpr float kWaist = 0.9f;
		constexpr int kBodyPoints = 3;
		// A surface steeper than this falls: the flat pools under a fall share its texture.
		constexpr float kMostUpFacing = 0.7f;
		constexpr size_t kMaxTriangles = 60000;
		// Contact is checked every few frames and held a moment, as a sheet of water has gaps: under a fall the effect must not come and go.
		constexpr unsigned kEveryFrames = 3;
		constexpr DWORD kHoldMs = 1500;
		// A waterfall effect within this of the player makes its sprites count. The falling sprite (a triangle counted) around the body from
		// its hips to over its head, within this sideways: the Darkroot captures where the water visibly falls on the player have 2-11 in one
		// frame, up to 1.7 m from the mesh sheet, which carries a much narrower band of it; those standing 4.5 m off it have none.
		constexpr float kEffectReach = 25.0f;
		constexpr float kSpriteReach = 0.8f;
		constexpr float kSpriteLowest = 0.5f;
		constexpr float kSpriteHighest = 2.8f;
		constexpr int kSpritesInContact = 2;
		constexpr std::array<int, 25> kFallEffects = { 100096, 100099, 120010, 120012, 120013, 120014, 120034, 120035, 120036, 120037, 120040, 120041,
			120042, 120043, 120045, 120046, 120047, 120049, 120050, 120051, 120052, 120053, 120055, 130001, 130002 };

		using triangle = std::array<float, 9>;

		struct piece
		{
			std::vector<triangle> triangles;
			float lo[3], hi[3];
		};

		std::string g_map;
		std::vector<piece> g_pieces;
		std::unordered_set<uint64_t> g_read;
		std::unordered_map<IDirect3DBaseTexture9*, bool> g_is_fall;
		std::unordered_map<IDirect3DBaseTexture9*, bool> g_is_sprite;
		std::unordered_map<IDirect3DBaseTexture9*, bool> g_is_landing;
		// Landing sprites have been drawn on this map: from then on it is stirred under them alone.
		bool g_landing_seen = false;
		size_t g_triangle_count = 0;

		// The latest sprite contact, set from the sprite observer, and whether a live effect allows it.
		bool g_effect_near = false;
		float g_player[3] = {};
		bool g_have_player = false;
		int g_sprites_this_frame = 0;
		DWORD g_sprite_time = 0;

		DWORD g_last_contact = 0;
		float g_contact[3] = {};
		bool g_sent_on = false;
		float g_sent[3] = {};

		bool is_fall_texture(const std::string& name)
		{
			return name.find("waterfall") != std::string::npos || name.rfind("m13_n_water_", 0) == 0;
		}

		// The falling water itself, 61 of the waterfall effects' 80 sprite records, sitting on the sheet. Not their mist (s00309, drawn
		// all over the map: median 94 m from the sheet in Darkroot), the ripples on the pool (s05212, flat) or the splash at the bottom (s00027).
		bool is_fall_sprite(const std::string& name)
		{
			return name == "s00018";
		}

		// The closest point of a triangle to p (Ericson, Real-Time Collision Detection).
		void closest_on_triangle(const float* p, const triangle& t, float* out)
		{
			const auto sub = [](const float* a, const float* b, float* r) { r[0] = a[0] - b[0]; r[1] = a[1] - b[1]; r[2] = a[2] - b[2]; };
			const auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
			const float* a = &t[0];
			const float* b = &t[3];
			const float* c = &t[6];
			float ab[3], ac[3], ap[3], bp[3], cp[3];
			sub(b, a, ab);
			sub(c, a, ac);
			sub(p, a, ap);
			const float d1 = dot(ab, ap), d2 = dot(ac, ap);
			const auto set = [&](float u, float v) { for (int i = 0; i < 3; i++) out[i] = a[i] + ab[i] * u + ac[i] * v; };
			if (d1 <= 0.0f && d2 <= 0.0f) { set(0.0f, 0.0f); return; }
			sub(p, b, bp);
			const float d3 = dot(ab, bp), d4 = dot(ac, bp);
			if (d3 >= 0.0f && d4 <= d3) { set(1.0f, 0.0f); return; }
			const float vc = d1 * d4 - d3 * d2;
			if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) { set(d1 / (d1 - d3), 0.0f); return; }
			sub(p, c, cp);
			const float d5 = dot(ab, cp), d6 = dot(ac, cp);
			if (d6 >= 0.0f && d5 <= d6) { set(0.0f, 1.0f); return; }
			const float vb = d5 * d2 - d1 * d6;
			if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) { set(0.0f, d2 / (d2 - d6)); return; }
			const float va = d3 * d6 - d5 * d4;
			if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
			{
				const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
				set(1.0f - w, w);
				return;
			}
			const float denom = 1.0f / (va + vb + vc);
			set(vb * denom, vc * denom);
		}

		void read_draw(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, INT base_vertex, UINT start_index, UINT prim_count)
		{
			auto& ffp = shared::common::ffp_state::get();
			IDirect3DVertexBuffer9* vb = ffp.stream_vb(0);
			const UINT stride = ffp.stream_stride(0), vb_offset = ffp.stream_offset(0);
			IDirect3DIndexBuffer9* ib = ffp.cur_ib();
			const float* vs = ffp.vs_const_data();
			float ox, oy, oz;
			if (!vb || !ib || !stride || !vs || !ffp.world_offset(ox, oy, oz) || (prim != D3DPT_TRIANGLELIST && prim != D3DPT_TRIANGLESTRIP))
				return;
			IDirect3DVertexDeclaration9* decl = nullptr;
			if (FAILED(dev->GetVertexDeclaration(&decl)) || !decl)
				return;
			D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH];
			UINT count = 0;
			const HRESULT got = decl->GetDeclaration(elements, &count);
			decl->Release();
			if (FAILED(got))
				return;
			int pos_off = -1, bone_off = -1;
			for (UINT i = 0; i < count && elements[i].Stream != 0xFF; i++)
			{
				if (elements[i].Stream != 0 || elements[i].UsageIndex != 0)
					continue;
				if (elements[i].Usage == D3DDECLUSAGE_POSITION && elements[i].Type == D3DDECLTYPE_FLOAT3)
					pos_off = elements[i].Offset;
				else if (elements[i].Usage == D3DDECLUSAGE_BLENDINDICES)
					bone_off = elements[i].Offset;
			}
			if (pos_off < 0)
				return;
			D3DINDEXBUFFER_DESC idesc {};
			D3DVERTEXBUFFER_DESC vdesc {};
			if (FAILED(ib->GetDesc(&idesc)) || FAILED(vb->GetDesc(&vdesc)))
				return;
			// Write-only video memory reads back undefined data; the map's buffers are managed.
			if ((idesc.Pool == D3DPOOL_DEFAULT && (idesc.Usage & D3DUSAGE_WRITEONLY)) || (vdesc.Pool == D3DPOOL_DEFAULT && (vdesc.Usage & D3DUSAGE_WRITEONLY)))
				return;
			const UINT isz = idesc.Format == D3DFMT_INDEX32 ? 4 : 2;
			const UINT indices = prim == D3DPT_TRIANGLESTRIP ? prim_count + 2 : prim_count * 3;
			if (static_cast<uint64_t>(start_index + indices) * isz > idesc.Size)
				return;
			void* idata = nullptr;
			if (FAILED(ib->Lock(start_index * isz, indices * isz, &idata, D3DLOCK_READONLY)) || !idata)
				return;
			void* vdata = nullptr;
			if (FAILED(vb->Lock(0, 0, &vdata, D3DLOCK_READONLY)) || !vdata)
			{
				ib->Unlock();
				return;
			}
			const auto index = [&](UINT i) -> int64_t
			{
				return static_cast<int64_t>(isz == 4 ? static_cast<const uint32_t*>(idata)[i] : static_cast<const uint16_t*>(idata)[i]) + base_vertex;
			};
			const auto* bytes = static_cast<const unsigned char*>(vdata);
			const auto vertex = [&](int64_t v, float* out) -> bool
			{
				const int64_t at = static_cast<int64_t>(vb_offset) + v * stride;
				if (v < 0 || at + static_cast<int64_t>(stride) > static_cast<int64_t>(vdesc.Size))
					return false;
				float p[3];
				std::memcpy(p, bytes + at + pos_off, sizeof(p));
				// The rigid bone's three rows (VS c8 on), relative to the camera.
				const int bone = bone_off >= 0 ? bytes[at + bone_off] : 0;
				const float* m = vs + (8 + 3 * bone) * 4;
				out[0] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3] + ox;
				out[1] = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7] + oy;
				out[2] = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11] + oz;
				return true;
			};
			piece part;
			for (int c = 0; c < 3; c++)
			{
				part.lo[c] = FLT_MAX;
				part.hi[c] = -FLT_MAX;
			}
			const UINT triangles = prim == D3DPT_TRIANGLESTRIP ? indices - 2 : indices / 3;
			for (UINT t = 0; t < triangles; t++)
			{
				const UINT first = prim == D3DPT_TRIANGLESTRIP ? t : t * 3;
				triangle tri;
				if (!vertex(index(first), &tri[0]) || !vertex(index(first + 1), &tri[3]) || !vertex(index(first + 2), &tri[6]))
					continue;
				const float e1[3] = { tri[3] - tri[0], tri[4] - tri[1], tri[5] - tri[2] };
				const float e2[3] = { tri[6] - tri[0], tri[7] - tri[1], tri[8] - tri[2] };
				const float nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2], nz = e1[0] * e2[1] - e1[1] * e2[0];
				const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
				if (length <= 1e-6f || std::abs(ny) / length > kMostUpFacing)
					continue;
				for (int i = 0; i < 9; i++)
				{
					part.lo[i % 3] = std::min(part.lo[i % 3], tri[i]);
					part.hi[i % 3] = std::max(part.hi[i % 3], tri[i]);
				}
				part.triangles.push_back(tri);
			}
			vb->Unlock();
			ib->Unlock();
			if (!part.triangles.empty() && g_triangle_count + part.triangles.size() <= kMaxTriangles)
			{
				g_triangle_count += part.triangles.size();
				const auto& mid = part.triangles[part.triangles.size() / 2];
				log("Waterfall", std::format("{} falling triangles, x {:.1f}..{:.1f}, y {:.1f}..{:.1f}, z {:.1f}..{:.1f}, one at {:.2f},{:.2f},{:.2f}", part.triangles.size(),
					part.lo[0], part.hi[0], part.lo[1], part.hi[1], part.lo[2], part.hi[2], (mid[0] + mid[3] + mid[6]) / 3.0f, (mid[1] + mid[4] + mid[7]) / 3.0f, (mid[2] + mid[5] + mid[8]) / 3.0f));
				g_pieces.push_back(std::move(part));
			}
		}

		// The nearest falling mesh point to the body, by squared distance.
		float nearest_on_meshes(const float* player, float* point)
		{
			float best = FLT_MAX;
			for (const auto& part : g_pieces)
			{
				for (int i = 0; i < kBodyPoints; i++)
				{
					const float p[3] = { player[0], player[1] + kWaist + (kBodyHeight - kWaist) * static_cast<float>(i) / (kBodyPoints - 1), player[2] };
					float box = 0.0f;
					for (int c = 0; c < 3; c++)
					{
						const float d = std::max({ part.lo[c] - p[c], 0.0f, p[c] - part.hi[c] });
						box += d * d;
					}
					if (box >= best)
						continue;
					for (const auto& tri : part.triangles)
					{
						float q[3];
						closest_on_triangle(p, tri, q);
						const float dx = q[0] - p[0], dy = q[1] - p[1], dz = q[2] - p[2];
						const float d2 = dx * dx + dy * dy + dz * dz;
						if (d2 < best)
						{
							best = d2;
							std::memcpy(point, q, sizeof(q));
						}
					}
				}
			}
			return best;
		}

		void refused_log()
		{
			static int budget = 5;
			if (budget > 0 && budget--)
				log("Waterfall", "Remix refused an option; trying again");
		}

		void send(bool on, const float* at)
		{
			auto& bridge = shared::common::remix_api::get().m_bridge;
			if (!bridge.SetConfigVariable)
				return;
			if (on)
			{
				constexpr float kMove = 0.05f;
				const bool moved = !g_sent_on || std::abs(at[0] - g_sent[0]) > kMove || std::abs(at[1] - g_sent[1]) > kMove || std::abs(at[2] - g_sent[2]) > kMove;
				if (moved)
				{
					// A refused set is sent again next time: g_sent only moves when it went through.
					if (bridge.SetConfigVariable("rtx.ds1Wet.fallContact", std::format("{}, {}, {}", at[0], at[1], at[2]).c_str()) == REMIXAPI_ERROR_CODE_SUCCESS)
						std::memcpy(g_sent, at, sizeof(g_sent));
					else
						refused_log();
				}
			}
			if (on != g_sent_on)
			{
				if (bridge.SetConfigVariable("rtx.ds1Wet.fallStrength", on ? "1" : "0") != REMIXAPI_ERROR_CODE_SUCCESS)
				{
					refused_log();
					return;
				}
				log("Waterfall", on ? std::format("in falling water at {:.1f}, {:.1f}, {:.1f}", at[0], at[1], at[2]) : std::string("out of falling water"));
				g_sent_on = on;
			}
		}
	}

	void observe_draw(IDirect3DDevice9* dev, IDirect3DBaseTexture9* texture, D3DPRIMITIVETYPE prim, INT base_vertex, UINT start_index, UINT prim_count)
	{
		if (!texture)
			return;
		const auto known = g_is_fall.find(texture);
		bool is_fall;
		if (known != g_is_fall.end())
			is_fall = known->second;
		else
		{
			const auto name = texture_names::name_of(texture);
			// No name yet: asked again next time.
			if (name.empty())
				return;
			is_fall = g_is_fall[texture] = is_fall_texture(name);
		}
		if (!is_fall)
			return;
		const auto map = map_lights::current_map_name();
		if (map != g_map)
		{
			g_map = map;
			g_landing_seen = false;
			g_is_landing.clear();
			g_pieces.clear();
			g_read.clear();
			g_triangle_count = 0;
		}
		auto& ffp = shared::common::ffp_state::get();
		const uint64_t key = (reinterpret_cast<uintptr_t>(ffp.stream_vb(0)) * 0x9E3779B1ull) ^ (static_cast<uint64_t>(start_index) << 32) ^
			(static_cast<uint64_t>(prim_count) << 16) ^ static_cast<uint64_t>(static_cast<uint32_t>(base_vertex));
		if (g_read.insert(key).second)
			read_draw(dev, prim, base_vertex, start_index, prim_count);
	}

	void observe_sprites(const shared::common::ffp_state::sprite_point* points, UINT count)
	{
		if (!g_effect_near || !g_have_player)
			return;
		IDirect3DBaseTexture9* texture = shared::common::ffp_state::get().cur_texture(0);
		if (!texture)
			return;
		const auto known = g_is_sprite.find(texture);
		bool counts;
		if (known != g_is_sprite.end())
			counts = known->second;
		else
		{
			const auto name = texture_names::name_of(texture);
			if (name.empty())
				return;
			counts = g_is_sprite[texture] = is_fall_sprite(name);
		}
		if (!counts)
			return;
		for (UINT i = 0; i < count; i++)
		{
			const auto& s = points[i];
			if (s.alpha < 0.1f)
				continue;
			const float dx = s.x - g_player[0], dz = s.z - g_player[2];
			const float height = s.y - g_player[1];
			if (dx * dx + dz * dz < kSpriteReach * kSpriteReach && height > kSpriteLowest && height < kSpriteHighest)
			{
				g_sprites_this_frame++;
			}
		}
	}

	DWORD landing_tag(IDirect3DBaseTexture9* texture, const shared::common::ffp_state::sprite_point* points, UINT count)
	{
		// As far from a fall's sheet as its landing splashes come, sideways, and as far below its lowest point.
		constexpr float kLandingReach = 14.0f;
		constexpr float kLandingBelow = 4.0f;
		// A fall's landing is one draw of 120 triangles or more in every capture; a footstep or a hit on water, which use the same
		// texture, 18 at the most.
		constexpr UINT kLandingTriangles = 60;
		if (!texture || count < kLandingTriangles || g_pieces.empty())
			return 0;
		const auto known = g_is_landing.find(texture);
		bool is_landing;
		if (known != g_is_landing.end())
			is_landing = known->second;
		else
		{
			const auto name = texture_names::name_of(texture);
			if (name.empty())
				return 0;
			is_landing = g_is_landing[texture] = name == "s05212";
		}
		// The pieces are the last map's until a fall of this one is drawn.
		if (!is_landing || map_lights::current_map_name() != g_map)
			return 0;
		for (UINT i = 0; i < count; i++)
		{
			const auto& s = points[i];
			if (s.alpha < 0.1f)
				continue;
			for (const auto& part : g_pieces)
			{
				if (s.x > part.lo[0] - kLandingReach && s.x < part.hi[0] + kLandingReach && s.z > part.lo[2] - kLandingReach && s.z < part.hi[2] + kLandingReach &&
					s.y > part.lo[1] - kLandingBelow && s.y < part.hi[1])
				{
					g_landing_seen = true;
					return lightmap_sun::kFallLandingMarker;
				}
			}
		}
		return 0;
	}

	bool landing_seen()
	{
		return g_landing_seen;
	}

	namespace
	{
		// The player's current animation: [[[[0x12E29E8] + 0x10] + 0x38] + 0x46C] + 0x60 (the root global resolved by
		// its pattern). The bonfire interactions, rest and kindle, are 7700, 7710, 7720 and 7725 (FFX 90012/90015/90016 hang off them in
		// c0000's TAE); the range takes 7700 to 7729.
		constexpr DWORD kAnimBase = 0x012E29E8;
		constexpr int kBonfireAnimFirst = 7700, kBonfireAnimLast = 7729;

		bool read_dword(DWORD addr, DWORD& out)
		{
			__try { out = *reinterpret_cast<const DWORD*>(addr); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		int player_animation()
		{
			DWORD p = 0;
			if (!read_dword(kAnimBase, p) || !p || !read_dword(p + 0x10, p) || !p || !read_dword(p + 0x38, p) || !p ||
				!read_dword(p + 0x46C, p) || !p || !read_dword(p + 0x60, p))
				return -1;
			return static_cast<int>(p);
		}

		// Resting at a bonfire restores the player, and so does a warp or a load: the body is cleared of water, mud and lava. The
		// runtime clears on every new value of rtx.ds1Wet.clearEvent.
		void body_clear_update()
		{
			static bool was_at_bonfire = false;
			static int count = 0, sent = 0;
			static float last_pos[3] = {};
			static bool have_pos = false;
			// Not on a change of map id: that is the block the player stands in, which changes on walking over a seamless seam.
			// A warp, a load or a death moves the player far in one step, which the position check below sees.
			bool clear = false;
			if (!shared::globals::lookdev_host)
			{
				const int anim = player_animation();
				const bool at_bonfire = anim >= kBonfireAnimFirst && anim <= kBonfireAnimLast;
				if (at_bonfire && !was_at_bonfire)
				{
					clear = true;
					log("BodyClear", std::format("bonfire animation {}", anim));
				}
				was_at_bonfire = at_bonfire;
				float x = 0.0f, depth = 0.0f, up = 0.0f;
				if (camera::get_char_pos(x, depth, up) && !(x == 0.0f && depth == 0.0f && up == 0.0f))
				{
					// A warp inside the map: far further in one step than the player can move.
					constexpr float kWarp = 30.0f;
					if (have_pos && (x - last_pos[0]) * (x - last_pos[0]) + (depth - last_pos[2]) * (depth - last_pos[2]) + (up - last_pos[1]) * (up - last_pos[1]) > kWarp * kWarp)
						clear = true;
					last_pos[0] = x;
					last_pos[1] = up;
					last_pos[2] = depth;
					have_pos = true;
				}
			}
			if (clear)
				count++;
			auto& bridge = shared::common::remix_api::get().m_bridge;
			if (count != sent && bridge.SetConfigVariable &&
				bridge.SetConfigVariable("rtx.ds1Wet.clearEvent", std::to_string(count).c_str()) == REMIXAPI_ERROR_CODE_SUCCESS)
				sent = count;
		}
	}

	void frame_update(unsigned int frame)
	{
		body_clear_update();
		if (g_sprites_this_frame >= kSpritesInContact)
			g_sprite_time = GetTickCount();
		g_sprites_this_frame = 0;
		if (frame % kEveryFrames != 0)
			return;
		float x = 0.0f, depth = 0.0f, up = 0.0f;
		g_have_player = camera::get_char_pos(x, depth, up) && !(x == 0.0f && depth == 0.0f && up == 0.0f);
		g_player[0] = x;
		g_player[1] = up;
		g_player[2] = depth;
		// Test on a replay, where the player stays put.
		float test[3];
		const bool testing = shared::common::config::get().get_float3("Waterfall", "TestPlayer", test);
		if (testing)
		{
			std::memcpy(g_player, test, sizeof(test));
			g_have_player = true;
		}
		const DWORD now = GetTickCount();
		if (!g_have_player)
		{
			send(false, g_contact);
			return;
		}

		g_effect_near = false;
		if (!shared::globals::lookdev_host)
		{
			for (const auto& inst : ffx_instances::current())
			{
				if (std::find(kFallEffects.begin(), kFallEffects.end(), inst.ffx_id) == kFallEffects.end())
					continue;
				float at[3];
				if (ffx_instances::world_position(inst.address, at) &&
					(at[0] - g_player[0]) * (at[0] - g_player[0]) + (at[2] - g_player[2]) * (at[2] - g_player[2]) < kEffectReach * kEffectReach)
				{
					g_effect_near = true;
					break;
				}
			}
		}

		float point[3];
		const float d2 = nearest_on_meshes(g_player, point);
		if (d2 < kReach * kReach)
		{
			g_last_contact = now;
		}
		else if (g_sprite_time && now - g_sprite_time < kHoldMs)
			g_last_contact = now;
		// Falling water does not meet a body at a point: drops come off the whole of it (rtx.ds1Wet.splashRadius round this).
		g_contact[0] = g_player[0];
		g_contact[1] = g_player[1] + kBodyHeight;
		g_contact[2] = g_player[2];
		if (testing && frame % 300 == 0)
			log("Waterfall", std::format("test player {:.1f}, {:.1f}, {:.1f}: nearest falling mesh {:.2f} m, {} triangles", g_player[0], g_player[1], g_player[2], std::sqrt(d2), g_triangle_count));
		send(g_last_contact && now - g_last_contact < kHoldMs, g_contact);
	}
}
