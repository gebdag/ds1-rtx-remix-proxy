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
		// The bolts are SFX models drawn with FRPG_Phn_PIN_D_Non + Dif_Non, SrcAlpha + One. Bonfire
		// and torch flames share the shaders, so their textures are the key (texture_names::
		// is_lit_effect: s00502, s00504). Those are the game's shared lightning-bolt meshes: about
		// 60 effects use them (the drakes' breath, lightning miracles and weapons), and each gets its
		// own stand-in.
		// The game's own flash light for the breath: colour = this x the bolts' tint, range 0-2 m.
		constexpr float kFlashGain = 2.55f;
		constexpr float kFlashRange = 2.0f;
		// The bolts reach about 10 m; a stand-in never reaches further than this.
		constexpr float kMaxRange = 15.0f;
		// Bolts closer than this to a group's first bolt belong to the same effect (the breath
		// spreads about 10 m); farther ones are another effect with its own light.
		constexpr float kClusterRadius = 12.0f;
		constexpr size_t kMaxLights = 8;
		// A frame's bolts beyond this are ignored (also bounds the list while nothing collects it).
		constexpr size_t kMaxBolts = 512;
		// Bolts are re-dealt every frame: each light eases towards its group, per 60 Hz frame.
		constexpr float kRise = 0.5f;
		constexpr float kFall = 0.15f;
		constexpr float kMove = 0.35f;
		constexpr float kOff = 0.01f;
		constexpr int kFirstSlot = 0x20000;

		struct bolt { float x, y, z, fade, r, g, b; };

		struct group
		{
			double w = 0, x = 0, y = 0, z = 0, r = 0, g = 0, b = 0;
			float ax = 0, ay = 0, az = 0; // its first bolt, the clustering anchor
			float strongest = 0.0f;
			std::vector<const bolt*> bolts;
			bool taken = false;
		};

		struct tracked
		{
			int slot = 0;
			float x = 0, y = 0, z = 0;
			float r = 0, g = 0, b = 0;
			float level = 0;
			float range = kFlashRange;
		};

		std::vector<bolt> s_bolts;
		std::vector<tracked> s_lights;
		std::vector<point_lights::light> s_out;
		std::chrono::steady_clock::time_point s_last_collect {};
		bool s_logged = false;

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
	}

	void observe_draw(IDirect3DDevice9* dev, IDirect3DBaseTexture9* diffuse, const float* vs_const, const float* ps_const)
	{
		if (!dev || !diffuse || !vs_const || !ps_const || s_bolts.size() >= kMaxBolts || !texture_names::is_lit_effect(diffuse))
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
		const bolt b { vs_const[8 * 4 + 3] + ox, vs_const[9 * 4 + 3] + oy, vs_const[10 * 4 + 3] + oz,
			std::clamp(tint[3], 0.0f, 1.0f), tint[0], tint[1], tint[2] };
		if (!(b.fade > 0.0f) || !std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.z) ||
			!std::isfinite(b.r) || !std::isfinite(b.g) || !std::isfinite(b.b))
			return;
		s_bolts.push_back(b);
	}

	const std::vector<point_lights::light>& collect()
	{
		s_out.clear();
		const auto now = std::chrono::steady_clock::now();
		const float dt = s_last_collect == std::chrono::steady_clock::time_point {} ? 1.0f / 60.0f :
			std::clamp(std::chrono::duration<float>(now - s_last_collect).count(), 0.0f, 0.25f);
		s_last_collect = now;

		// This frame's bolts in groups, one per effect.
		std::vector<group> groups;
		for (const auto& b : s_bolts)
		{
			auto it = std::find_if(groups.begin(), groups.end(), [&](const group& g)
				{ return dist2(g.ax, g.ay, g.az, b.x, b.y, b.z) <= kClusterRadius * kClusterRadius; });
			if (it == groups.end())
			{
				if (groups.size() >= kMaxLights)
					continue;
				groups.push_back({});
				it = groups.end() - 1;
				it->ax = b.x; it->ay = b.y; it->az = b.z;
			}
			it->w += b.fade; it->x += b.x * b.fade; it->y += b.y * b.fade; it->z += b.z * b.fade;
			it->r += b.r * b.fade; it->g += b.g * b.fade; it->b += b.b * b.fade;
			it->strongest = std::max(it->strongest, b.fade);
			it->bolts.push_back(&b);
		}

		// Each light follows the nearest group it can reach; a group no light took starts a new one;
		// a light without a group fades out.
		const float rise = rate(kRise, dt), fall = rate(kFall, dt), move = rate(kMove, dt);
		auto ease_to = [&](tracked& t, const group* g, bool fresh)
		{
			float target = 0.0f;
			if (g)
			{
				const float cx = static_cast<float>(g->x / g->w), cy = static_cast<float>(g->y / g->w), cz = static_cast<float>(g->z / g->w);
				float spread = 0.0f;
				for (const auto* b : g->bolts)
					spread = std::max(spread, std::sqrt(dist2(b->x, b->y, b->z, cx, cy, cz)));
				const float k = fresh ? 1.0f : move;
				t.x += (cx - t.x) * k;
				t.y += (cy - t.y) * k;
				t.z += (cz - t.z) * k;
				t.r += (static_cast<float>(g->r / g->w) - t.r) * k;
				t.g += (static_cast<float>(g->g / g->w) - t.g) * k;
				t.b += (static_cast<float>(g->b / g->w) - t.b) * k;
				t.range += (std::min(kMaxRange, spread + kFlashRange) - t.range) * k;
				target = g->strongest;
			}
			t.level += (target - t.level) * (target > t.level ? rise : fall);
		};
		for (auto& t : s_lights)
		{
			group* best = nullptr;
			float best_d = kClusterRadius * kClusterRadius;
			for (auto& g : groups)
			{
				const float d = g.taken ? FLT_MAX : dist2(t.x, t.y, t.z,
					static_cast<float>(g.x / g.w), static_cast<float>(g.y / g.w), static_cast<float>(g.z / g.w));
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
			t.slot = free_slot();
			ease_to(t, &g, true);
			s_lights.push_back(t);
			if (!s_logged)
			{
				s_logged = true;
				shared::common::log("EffectLights", std::format("lightning bolts: {} bolts, light at ({:.1f}, {:.1f}, {:.1f}), range {:.1f} m",
					g.bolts.size(), t.x, t.y, t.z, t.range));
			}
		}
		s_bolts.clear();
		std::erase_if(s_lights, [](const tracked& t) { return !(t.level >= kOff) || !std::isfinite(t.x + t.y + t.z + t.r + t.g + t.b); });

		for (const auto& t : s_lights)
		{
			point_lights::light l {};
			l.slot = t.slot;
			l.x = t.x; l.y = t.y; l.z = t.z;
			l.r = kFlashGain * t.r * t.level;
			l.g = kFlashGain * t.g * t.level;
			l.b = kFlashGain * t.b * t.level;
			l.dwindle_begin = 0.0f;
			l.dwindle_end = t.range;
			l.has_params = true;
			l.source = point_lights::param_source::effect;
			s_out.push_back(l);
		}
		return s_out;
	}
}
