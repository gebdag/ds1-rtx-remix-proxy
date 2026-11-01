#include "std_include.hpp"
#include "effect_lights.hpp"
#include "texture_names.hpp"
#include "shared/common/ffp_state.hpp"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <limits>

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
		// breath): clusters of fire-coloured additive particles get the game's own light for a fire of
		// that size, coloured by the particles. The game's fire lights are effect lights
		// (DirectPointLightEntity): a held torch's (1.97, 1.39, 0.77) to 1.9 m (frame_20261030_092329)
		// and a bonfire's (0.72, 0.52, 0.21) to 4.8 m (bonfire1, msg_scene), about the same energy
		// spread wider and dimmer. A flame drawing a torch flame's light gets the torch's light; a fire
		// drawing kBonfireSize times that gets the bonfire's, between them on the log of the drawn light,
		// and a smaller flame its square root of the torch's. Linear in the drawn light, a fire breath
		// came out at 17: that sum swings 10x between frames of one fire and is huge for big soft
		// sprites. Only fire: white, blue and green particles (item pickups, souls, motes, fog,
		// magic) are lit by no game light, and none is stood in for them.
		// A blue bonfire's light: its tint c139 (0.2, 0.39, 1), at twice the peak of a bonfire's (0.72, 0.52, 0.21) to
		// a bonfire's 4.8 m. It has no game light to match, and a bonfire's strength hardly showed in blue.
		constexpr float kBlueFlameLight[3] = { 0.3f, 0.6f, 1.5f };
		// A crystal lizard's light: faint, in the blue of its gem, about as far as the lizard is long.
		constexpr float kEmitterLight[3] = { 0.25f, 0.45f, 0.75f };
		constexpr float kEmitterReach = 3.5f;
		constexpr float kTorchPower = 0.14f;   // a torch's flame (torches1 0.139, torch_wall 0.168)
		constexpr float kTorchLight = 1.97f;   // a held torch's effect light, peak channel
		constexpr float kTorchReach = 1.9f;
		constexpr float kBonfireSize = 6.4f;   // drawn light of a bonfire-sized fire, in torch flames
		constexpr float kBonfireLight = 0.72f; // a bonfire's effect light, peak channel
		constexpr float kBonfireReach = 4.8f;
		constexpr float kSpriteClusterRadius = 4.0f;
		// Warm: red >= green >= blue and blue at most this share of red (fire breath 0.28, the
		// Burg's fires 0.3-0.36; item sparkles 1.17, motes and fog 0.8-1.2).
		constexpr float kWarmBlueShare = 0.6f;
		// Fire is orange: green at least this share of red. The flame textures' own colour is 0.52-0.62
		// (s00100 / s00103 fire breath, s00009 torch flame; their particles are white), torches and
		// fireballs 0.67-0.73 as drawn (msg_scene). The red glow of a message on the ground (s00906 at
		// 0.83 under vertex colour 255, 100, 70) draws 0.31-0.33 and a bloodstain less: the game lights
		// neither, and stood in as fires they lit the ground around them like a bonfire.
		constexpr float kFireGreenShare = 0.45f;
		// A cluster drawing less light than this (summed colour per unit area * area, max channel)
		// is embers or sparks; a torch's flame draws about 0.14.
		constexpr float kSpriteMinPower = 0.1f;

		constexpr size_t kMaxLights = 16;
		// A particle stand-in is suppressed where an engine or table light stands within this of it: the
		// game lights that fire itself (a bonfire, a torch). It comes back past kCoverRelease, so a fire
		// beside a moving torch-bearer does not blink at the edge, and a game light keeps covering for
		// kCoverHold seconds after it was last seen (the game's own fire lights flicker off and on).
		constexpr float kCoverRadius = 3.0f;
		constexpr float kCoverRelease = 3.6f;
		constexpr float kCoverHold = 0.5f;
		// A frame's samples beyond this are ignored (also bounds the list while nothing collects it).
		constexpr size_t kMaxSamples = 2048;
		// Samples are re-dealt every frame: each light eases towards its group, per 60 Hz frame.
		constexpr float kRise = 0.5f;
		constexpr float kFall = 0.15f;
		constexpr float kMove = 0.35f;
		// A particle light's colour and reach follow its particles slowly: their summed light swings
		// 25% between frames of one steady fire, and followed at kMove the light flickered with it.
		constexpr float kSteady = 0.05f;
		constexpr float kOff = 0.01f;
		constexpr int kFirstSlot = 0x20000;

		enum class kind { bolt, sprite, blue_flame, emitter };

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
			bool covered = false;
		};

		// Positions of the engine and table lights, and when each was last seen.
		struct cover { float x, y, z; std::chrono::steady_clock::time_point seen; };
		std::vector<cover> s_covers;

		std::vector<sample> s_samples;
		std::vector<tracked> s_lights;
		std::vector<point_lights::light> s_out;
		std::chrono::steady_clock::time_point s_last_collect {};
		bool s_logged[4] = {};
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
		if (!dev || !diffuse || !vs_const || !ps_const || s_samples.size() >= kMaxSamples)
			return;
		const bool blue_flame = texture_names::is_blue_flame(diffuse);
		if (!blue_flame && !texture_names::is_lit_effect(diffuse))
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
		if (blue_flame && !(tint[2] > tint[0]))
			return;
		const sample b { vs_const[8 * 4 + 3] + ox, vs_const[9 * 4 + 3] + oy, vs_const[10 * 4 + 3] + oz,
			blue_flame ? 1.0f : std::clamp(tint[3], 0.0f, 1.0f), tint[0], tint[1], tint[2], blue_flame ? kind::blue_flame : kind::bolt };
		if (!(b.fade > 0.0f) || !std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.z) ||
			!std::isfinite(b.r) || !std::isfinite(b.g) || !std::isfinite(b.b))
			return;
		s_samples.push_back(b);
	}

	void observe_emitter(const float* vs_const)
	{
		float ox = 0, oy = 0, oz = 0;
		if (!vs_const || s_samples.size() >= kMaxSamples || !shared::common::ffp_state::get().world_offset(ox, oy, oz))
			return;
		const sample e { vs_const[8 * 4 + 3] + ox, vs_const[9 * 4 + 3] + oy, vs_const[10 * 4 + 3] + oz, 1.0f, 1.0f, 1.0f, 1.0f, kind::emitter };
		if (std::isfinite(e.x) && std::isfinite(e.y) && std::isfinite(e.z))
			s_samples.push_back(e);
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

	void set_covers(const std::vector<const point_lights::light*>& lights)
	{
		const auto now = std::chrono::steady_clock::now();
		for (const auto* l : lights)
		{
			if (!l)
				continue;
			auto it = std::find_if(s_covers.begin(), s_covers.end(), [&](const cover& c)
				{ return dist2(c.x, c.y, c.z, l->x, l->y, l->z) < 0.25f * 0.25f; });
			if (it == s_covers.end())
				s_covers.push_back({ l->x, l->y, l->z, now });
			else
				*it = { l->x, l->y, l->z, now };
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
		// Particle groups that are not fire get no light. A faint one (sparks, embers) starts none either,
		// but keeps the light of a fire that dims for a frame or two (weak).
		std::erase_if(groups, [](const group& g)
		{
			if (g.k != kind::sprite)
				return false;
			return !(g.r >= g.g && g.g >= g.b && g.b <= kWarmBlueShare * g.r && g.g >= kFireGreenShare * g.r);
		});
		auto weak = [](const group& g) { return g.k == kind::sprite && std::max({ g.r, g.g, g.b }) < kSpriteMinPower; };

		// Each light follows the nearest group of its kind it can reach; a group no light took starts
		// a new one; a light without a group fades out.
		const float rise = rate(kRise, dt), fall = rate(kFall, dt), move = rate(kMove, dt), steady = rate(kSteady, dt);
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
				else if (g->k == kind::emitter)
				{
					r = kEmitterLight[0];
					gg = kEmitterLight[1];
					b = kEmitterLight[2];
					range = kEmitterReach;
					target = 1.0f;
				}
				else if (g->k == kind::blue_flame)
				{
					r = s_fire_scale * kBlueFlameLight[0];
					gg = s_fire_scale * kBlueFlameLight[1];
					b = s_fire_scale * kBlueFlameLight[2];
					range = kBonfireReach;
					target = 1.0f;
				}
				else
				{
					// The game's torch to bonfire light on the brightest channel, in the particles' colour.
					const double peak = std::max({ g->r, g->g, g->b });
					const float size = static_cast<float>(peak) / kTorchPower;
					const float growth = std::clamp(std::log(std::max(size, 1.0f)) / std::log(kBonfireSize), 0.0f, 1.0f);
					const float flame = std::min(1.0f, std::sqrt(size));
					const float strength = s_fire_scale * flame * (kTorchLight + (kBonfireLight - kTorchLight) * growth);
					r = strength * static_cast<float>(g->r / peak);
					gg = strength * static_cast<float>(g->g / peak);
					b = strength * static_cast<float>(g->b / peak);
					range = std::min(kMaxRange, spread + flame * (kTorchReach + (kBonfireReach - kTorchReach) * growth));
					target = 1.0f;
				}
				const float k = fresh ? 1.0f : move;
				const float kc = fresh ? 1.0f : (g->k == kind::sprite ? steady : move);
				t.x += (cx - t.x) * k;
				t.y += (cy - t.y) * k;
				t.z += (cz - t.z) * k;
				t.r += (r - t.r) * kc;
				t.g += (gg - t.g) * kc;
				t.b += (b - t.b) * kc;
				t.range += (range - t.range) * kc;
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
			if (g.taken || weak(g) || s_lights.size() >= kMaxLights)
				continue;
			tracked t;
			t.k = g.k;
			t.slot = free_slot();
			ease_to(t, &g, true);
			s_lights.push_back(t);
			if (bool& logged = s_logged[static_cast<int>(g.k)]; !logged)
			{
				logged = true;
				shared::common::log("EffectLights", std::format("{}: {} samples, light at ({:.1f}, {:.1f}, {:.1f}), colour ({:.3f}, {:.3f}, {:.3f}), range {:.1f} m",
					g.k == kind::bolt ? "lightning bolts" : g.k == kind::sprite ? "additive particles" : g.k == kind::emitter ? "crystal lizard" : "blue bonfire", g.samples.size(), t.x, t.y, t.z, t.r, t.g, t.b, t.range));
			}
		}
		s_samples.clear();
		std::erase_if(s_lights, [](const tracked& t) { return !(t.level >= kOff) || !std::isfinite(t.x + t.y + t.z + t.r + t.g + t.b); });

		// Particle lights an engine or table light stands for (covers, kCoverRadius and its release).
		std::erase_if(s_covers, [&](const cover& c) { return std::chrono::duration<float>(now - c.seen).count() > kCoverHold; });
		for (auto& t : s_lights)
		{
			if (t.k != kind::sprite)
				continue;
			float nearest = std::numeric_limits<float>::max();
			for (const auto& c : s_covers)
				nearest = std::min(nearest, dist2(t.x, t.y, t.z, c.x, c.y, c.z));
			const float edge = t.covered ? kCoverRelease : kCoverRadius;
			t.covered = nearest <= edge * edge;
		}

		for (const auto& t : s_lights)
		{
			if (t.covered)
				continue;
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
			l.entity_vtable = t.k != kind::bolt ? kParticleMarker : 0;
			s_out.push_back(l);
		}
		return s_out;
	}
}
