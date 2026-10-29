#include "std_include.hpp"
#include "effect_lights.hpp"
#include "texture_names.hpp"
#include "shared/common/ffp_state.hpp"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>

namespace comp::game::effect_lights
{
	namespace
	{
		// Bolts: SFX models drawn with FRPG_Phn_PIN_D_Non + Dif_Non, SrcAlpha + One, with the game's
		// shared lightning textures (texture_names::is_lit_effect: s00502, s00504; the drakes' breath,
		// lightning miracles and weapons). Bonfire and torch flames share the shaders, so the texture
		// is the key. The game's own flash light for the breath: colour = this x the bolts' tint,
		// range 0-2 m.
		constexpr float kFlashGain = 2.55f;
		constexpr float kFlashRange = 2.0f;
		// The bolts reach about 10 m; a stand-in never reaches further than this.
		constexpr float kMaxRange = 15.0f;
		// Bolts closer than this to a group's first bolt belong to the same effect (the breath
		// spreads about 10 m); farther ones are another effect with its own light.
		constexpr float kClusterRadius = 12.0f;

		// Fire particles (ffp_state sprite points, SrcAlpha + One) the game gives no light (a fire
		// breath): clusters of warm additive particles get a light between the game's own fire
		// lights, coloured by the particles. A flame the size of a torch's lights like a torch's
		// engine light (1.2, reaching 5 m); bigger fires grow with the square root of the light they
		// draw, up to the game's bonfire light (3.04, 7 m). Linear in the drawn light, a fire breath
		// came out at 17: that sum swings 10x between frames of one fire and is huge for big soft
		// sprites. Only fire: white, blue and green particles (item pickups, souls, motes, fog,
		// magic) are lit by no game light, and none is stood in for them.
		constexpr float kTorchPower = 0.14f;   // a torch's flame (torches1 0.139, torch_wall 0.168)
		constexpr float kTorchLight = 1.2f;    // its engine light (item1, torches1: 1.2, 0.99, 0.75)
		constexpr float kTorchReach = 5.0f;
		constexpr float kBonfireLight = 3.04f; // bonfire1's engine light (3.04, 3.04, 2.35), to 7 m
		constexpr float kBonfireReach = 7.0f;
		constexpr float kSpriteClusterRadius = 4.0f;
		// Warm: red >= green >= blue and blue at most this share of red (fire breath 0.28, the
		// Burg's fires 0.3-0.36; item sparkles 1.17, motes and fog 0.8-1.2).
		constexpr float kWarmBlueShare = 0.6f;
		// A cluster drawing less light than this (summed colour per unit area * area, max channel)
		// is embers or sparks; a torch's flame draws about 0.14.
		constexpr float kSpriteMinPower = 0.1f;

		constexpr size_t kMaxLights = 8;
		// A frame's samples beyond this are ignored (also bounds the list while nothing collects it).
		constexpr size_t kMaxSamples = 2048;
		// Samples are re-dealt every frame: each light eases towards its group, per 60 Hz frame.
		constexpr float kRise = 0.5f;
		constexpr float kFall = 0.15f;
		constexpr float kMove = 0.35f;
		constexpr float kOff = 0.01f;
		constexpr int kFirstSlot = 0x20000;

		enum class kind { bolt, sprite };

		// A bolt: its origin, fade (c139.w) and tint (c139.rgb). A particle triangle: its centre,
		// weight 1, and the light it adds (rgb).
		struct sample { float x, y, z, fade, r, g, b; kind k; };

		struct group
		{
			kind k = kind::bolt;
			double w = 0, x = 0, y = 0, z = 0, r = 0, g = 0, b = 0;
			float ax = 0, ay = 0, az = 0; // its first sample, the clustering anchor
			float strongest = 0.0f;
			std::vector<const sample*> samples;
			bool taken = false;
		};

		struct tracked
		{
			kind k = kind::bolt;
			int slot = 0;
			float x = 0, y = 0, z = 0;
			float r = 0, g = 0, b = 0;
			float level = 0;
			float range = kFlashRange;
		};

		std::vector<sample> s_samples;
		std::vector<tracked> s_lights;
		std::vector<point_lights::light> s_out;
		std::chrono::steady_clock::time_point s_last_collect {};
		bool s_logged[2] = {};
		float s_fire_scale = 1.0f;

		float dist2(float ax, float ay, float az, float bx, float by, float bz)
		{
			return (ax - bx) * (ax - bx) + (ay - by) * (ay - by) + (az - bz) * (az - bz);
		}

		// A per-60 Hz-frame easing rate over dt seconds.
		float rate(float per_frame, float dt)
		{
			return 1.0f - std::pow(1.0f - per_frame, dt * 60.0f);
		}

		int free_slot()
		{
			for (int slot = kFirstSlot;; slot++)
				if (std::none_of(s_lights.begin(), s_lights.end(), [&](const tracked& t) { return t.slot == slot; }))
					return slot;
		}

		float cluster_radius(kind k) { return k == kind::bolt ? kClusterRadius : kSpriteClusterRadius; }
	}

	void observe_draw(IDirect3DDevice9* dev, IDirect3DBaseTexture9* diffuse, const float* vs_const, const float* ps_const)
	{
		if (!dev || !diffuse || !vs_const || !ps_const || s_samples.size() >= kMaxSamples || !texture_names::is_lit_effect(diffuse))
			return;
		DWORD blend = FALSE, src = 0, dst = 0;
		dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
		dev->GetRenderState(D3DRS_SRCBLEND, &src);
		dev->GetRenderState(D3DRS_DESTBLEND, &dst);
		if (!blend || src != D3DBLEND_SRCALPHA || dst != D3DBLEND_ONE)
			return;
		// The bolt's first bone (VS c8-c10, 3x4 rows) holds its origin, relative to the camera; the
		// proxy's world offset is that camera, in the same space as the engine's light positions.
		float ox = 0, oy = 0, oz = 0;
		if (!shared::common::ffp_state::get().world_offset(ox, oy, oz))
			return;
		const float* tint = &ps_const[139 * 4];
		const sample b { vs_const[8 * 4 + 3] + ox, vs_const[9 * 4 + 3] + oy, vs_const[10 * 4 + 3] + oz,
			std::clamp(tint[3], 0.0f, 1.0f), tint[0], tint[1], tint[2], kind::bolt };
		if (!(b.fade > 0.0f) || !std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.z) ||
			!std::isfinite(b.r) || !std::isfinite(b.g) || !std::isfinite(b.b))
			return;
		s_samples.push_back(b);
	}

	void set_fire_scale(float scale)
	{
		s_fire_scale = std::isfinite(scale) ? std::max(scale, 0.0f) : 1.0f;
	}

	void observe_sprites(const shared::common::ffp_state::sprite_point* points, UINT count)
	{
		if (!points)
			return;
		for (UINT i = 0; i < count && s_samples.size() < kMaxSamples; i++)
		{
			const auto& p = points[i];
			if (!p.additive)
				continue;
			const float r = p.r * p.area, g = p.g * p.area, b = p.b * p.area;
			if (!(r + g + b > 0.0f) || !std::isfinite(r + g + b))
				continue;
			s_samples.push_back({ p.x, p.y, p.z, 1.0f, r, g, b, kind::sprite });
		}
	}

	const std::vector<point_lights::light>& collect()
	{
		s_out.clear();
		const auto now = std::chrono::steady_clock::now();
		const float dt = s_last_collect == std::chrono::steady_clock::time_point {} ? 1.0f / 60.0f :
			std::clamp(std::chrono::duration<float>(now - s_last_collect).count(), 0.0f, 0.25f);
		s_last_collect = now;

		// This frame's samples in groups, one per effect and kind. A bolt group weighs its bolts by
		// their fade; a particle group sums its triangles' light.
		std::vector<group> groups;
		for (const auto& s : s_samples)
		{
			const float radius = cluster_radius(s.k);
			auto it = std::find_if(groups.begin(), groups.end(), [&](const group& g)
				{ return g.k == s.k && dist2(g.ax, g.ay, g.az, s.x, s.y, s.z) <= radius * radius; });
			if (it == groups.end())
			{
				if (groups.size() >= 4 * kMaxLights)
					continue;
				groups.push_back({});
				it = groups.end() - 1;
				it->k = s.k;
				it->ax = s.x; it->ay = s.y; it->az = s.z;
			}
			const float w = s.k == kind::bolt ? s.fade : std::max({ s.r, s.g, s.b });
			it->w += w; it->x += s.x * w; it->y += s.y * w; it->z += s.z * w;
			if (s.k == kind::bolt)
			{
				it->r += s.r * s.fade; it->g += s.g * s.fade; it->b += s.b * s.fade;
				it->strongest = std::max(it->strongest, s.fade);
			}
			else
			{
				it->r += s.r; it->g += s.g; it->b += s.b;
			}
			it->samples.push_back(&s);
		}
		// Particle groups that are faint (sparks, embers) or not fire get no light.
		std::erase_if(groups, [](const group& g)
		{
			if (g.k != kind::sprite)
				return false;
			const bool warm = g.r >= g.g && g.g >= g.b && g.b <= kWarmBlueShare * g.r;
			return !warm || std::max({ g.r, g.g, g.b }) < kSpriteMinPower;
		});

		// Each light follows the nearest group of its kind it can reach; a group no light took starts
		// a new one; a light without a group fades out.
		const float rise = rate(kRise, dt), fall = rate(kFall, dt), move = rate(kMove, dt);
		auto centre = [](const group& g, float& cx, float& cy, float& cz)
		{
			cx = static_cast<float>(g.x / g.w); cy = static_cast<float>(g.y / g.w); cz = static_cast<float>(g.z / g.w);
		};
		auto ease_to = [&](tracked& t, const group* g, bool fresh)
		{
			float target = 0.0f;
			if (g)
			{
				float cx, cy, cz;
				centre(*g, cx, cy, cz);
				float spread = 0.0f;
				for (const auto* s : g->samples)
					spread = std::max(spread, std::sqrt(dist2(s->x, s->y, s->z, cx, cy, cz)));
				float r, gg, b, range;
				if (g->k == kind::bolt)
				{
					r = kFlashGain * static_cast<float>(g->r / g->w);
					gg = kFlashGain * static_cast<float>(g->g / g->w);
					b = kFlashGain * static_cast<float>(g->b / g->w);
					range = std::min(kMaxRange, spread + kFlashRange);
					target = g->strongest;
				}
				else
				{
					// Torch to bonfire strength on the brightest channel, in the particles' colour.
					const double peak = std::max({ g->r, g->g, g->b });
					const float size = static_cast<float>(peak) / kTorchPower;
					const float strength = s_fire_scale * std::min(kBonfireLight, kTorchLight * std::sqrt(size));
					r = strength * static_cast<float>(g->r / peak);
					gg = strength * static_cast<float>(g->g / peak);
					b = strength * static_cast<float>(g->b / peak);
					range = std::min(kBonfireReach, spread + kTorchReach * std::min(1.0f, std::sqrt(size)));
					target = 1.0f;
				}
				const float k = fresh ? 1.0f : move;
				t.x += (cx - t.x) * k;
				t.y += (cy - t.y) * k;
				t.z += (cz - t.z) * k;
				t.r += (r - t.r) * k;
				t.g += (gg - t.g) * k;
				t.b += (b - t.b) * k;
				t.range += (range - t.range) * k;
			}
			t.level += (target - t.level) * (target > t.level ? rise : fall);
		};
		for (auto& t : s_lights)
		{
			group* best = nullptr;
			const float radius = cluster_radius(t.k);
			float best_d = radius * radius;
			for (auto& g : groups)
			{
				if (g.taken || g.k != t.k)
					continue;
				float cx, cy, cz;
				centre(g, cx, cy, cz);
				const float d = dist2(t.x, t.y, t.z, cx, cy, cz);
				if (d <= best_d)
				{
					best = &g;
					best_d = d;
				}
			}
			if (best)
				best->taken = true;
			ease_to(t, best, false);
		}
		for (auto& g : groups)
		{
			if (g.taken || s_lights.size() >= kMaxLights)
				continue;
			tracked t;
			t.k = g.k;
			t.slot = free_slot();
			ease_to(t, &g, true);
			s_lights.push_back(t);
			if (bool& logged = s_logged[g.k == kind::bolt ? 0 : 1]; !logged)
			{
				logged = true;
				shared::common::log("EffectLights", std::format("{}: {} samples, light at ({:.1f}, {:.1f}, {:.1f}), colour ({:.3f}, {:.3f}, {:.3f}), range {:.1f} m",
					g.k == kind::bolt ? "lightning bolts" : "additive particles", g.samples.size(), t.x, t.y, t.z, t.r, t.g, t.b, t.range));
			}
		}
		s_samples.clear();
		std::erase_if(s_lights, [](const tracked& t) { return !(t.level >= kOff) || !std::isfinite(t.x + t.y + t.z + t.r + t.g + t.b); });

		for (const auto& t : s_lights)
		{
			point_lights::light l {};
			l.slot = t.slot;
			l.x = t.x; l.y = t.y; l.z = t.z;
			l.r = t.r * t.level;
			l.g = t.g * t.level;
			l.b = t.b * t.level;
			l.dwindle_begin = 0.0f;
			l.dwindle_end = t.range;
			l.has_params = true;
			l.source = point_lights::param_source::effect;
			// Particle stand-ins are told apart by their entity_vtable (game_lights hides those an
			// engine or table light already stands for).
			l.entity_vtable = t.k == kind::sprite ? kParticleMarker : 0;
			s_out.push_back(l);
		}
		return s_out;
	}
}
