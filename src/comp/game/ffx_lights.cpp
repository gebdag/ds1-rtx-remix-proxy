#include "std_include.hpp"
#include "ffx_lights.hpp"
#include "ffx_instances.hpp"
#include "shared/common/ffp_state.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace comp::game::ffx_lights
{
	namespace
	{
		// Slots of these lights (game_lights keys a Remix light by its slot): the engine's slots are
		// below 0x10000, map_lights' stand-ins from 0x10000, effect_lights' from 0x20000.
		constexpr int kFirstSlot = 0x30000;
		// Lights past this distance from the camera (beyond their own reach) are left out, and at most
		// kMaxLights of the nearest are drawn: a map can hold dozens of messages and bloodstains.
		constexpr float kReach = 60.0f;
		constexpr size_t kMaxLights = 64;
		// A map light this close to a persistent effect stands for it (a wall torch's MSB light).
		constexpr float kCoverRadius = 3.0f;
		// A brief effect past its visual lifetime stays lit while its emitters run, then fades with this
		// time constant and goes when it is too faint to matter.
		constexpr float kFadeSeconds = 0.25f;
		constexpr float kFadeOff = 0.01f;
		// Seconds past its visual life after which what keeps a brief effect lit is logged, once per effect.
		constexpr float kLingerLog = 5.0f;
		std::unordered_set<int> g_lingering, g_ended;

		std::unordered_map<int, entry> g_table;
		bool g_loaded = false;

		// A live instance's slot, kept while the same instance runs the same effect.
		struct tracked
		{
			int ffx_id;
			int slot;
			UINT seen;
		};
		std::unordered_map<unsigned, tracked> g_slots;
		std::vector<point_lights::light> g_out;

		// An instance's brightness: 1 while it shows, fading after.
		struct fade
		{
			int ffx_id;
			float level;
			UINT seen;
			float first_seen;   // seconds since g_origin: an instance's own age can stand at 0 (seen with state 0xFFFFFFFE)
		};
		std::unordered_map<unsigned, fade> g_fades;
		std::chrono::steady_clock::time_point g_fade_time = std::chrono::steady_clock::now();
		const std::chrono::steady_clock::time_point g_origin = g_fade_time;

		void load()
		{
			g_loaded = true;
			std::ifstream file(shared::globals::root_path + "\\rtx_comp\\ffx_lights.tsv");
			std::string line;
			while (std::getline(file, line))
			{
				if (line.empty() || line[0] == '#')
					continue;
				if (line.back() == '\r')
					line.pop_back();
				std::istringstream in(line);
				int id = 0, planar = 0, persistent = 0;
				entry e {};
				std::string confidence;
				in >> id >> e.r >> e.g >> e.b >> e.dwindle_begin >> e.dwindle_end >> planar >> persistent >> confidence >> e.visual_life;
				if (!in || id <= 0)
					continue;
				in >> std::ws;
				std::getline(in, e.category);
				e.planar = planar != 0;
				e.persistent = persistent != 0;
				e.confidence = confidence.empty() ? 'l' : confidence[0];
				g_table[id] = std::move(e);
			}
			shared::common::log("FfxLights", std::format("{} effect lights from rtx_comp\\ffx_lights.tsv", g_table.size()));
		}

		int free_slot()
		{
			for (int slot = kFirstSlot;; ++slot)
				if (std::none_of(g_slots.begin(), g_slots.end(), [&](const auto& t) { return t.second.slot == slot; }))
					return slot;
		}
	}

	const entry* find(int ffx_id)
	{
		if (!g_loaded)
			load();
		const auto it = g_table.find(ffx_id);
		return it != g_table.end() ? &it->second : nullptr;
	}

	int table_size()
	{
		if (!g_loaded)
			load();
		return static_cast<int>(g_table.size());
	}

	const std::vector<point_lights::light>& collect(const std::vector<const point_lights::light*>& covers)
	{
		g_out.clear();
		if (!g_loaded)
			load();
		float cam[3] = {};
		if (g_table.empty() || !shared::common::ffp_state::get().world_offset(cam[0], cam[1], cam[2]))
			return g_out;

		const UINT frame = shared::common::ffp_state::get().frame_count();
		const auto time_now = std::chrono::steady_clock::now();
		const float dt = std::clamp(std::chrono::duration<float>(time_now - g_fade_time).count(), 0.0f, 0.5f);
		g_fade_time = time_now;
		const float decay = std::exp(-dt / kFadeSeconds);
		const float now = std::chrono::duration<float>(time_now - g_origin).count();
		struct candidate { const ffx_instances::instance* inst; const entry* e; float pos[3]; float distance; float level; };
		std::vector<candidate> candidates;
		for (const auto& inst : ffx_instances::current())
		{
			const auto it = g_table.find(inst.ffx_id);
			if (it == g_table.end())
				continue;
			candidate c { &inst, &it->second };
			if (!ffx_instances::world_position(inst.address, c.pos))
				continue;
			// How bright it is now: it ends with what it shows, not with the instance.
			auto [f, fresh_fade] = g_fades.try_emplace(inst.address, fade { inst.ffx_id, 1.0f, frame, now });
			if (fresh_fade || f->second.ffx_id != inst.ffx_id)
				f->second = { inst.ffx_id, 1.0f, frame, now };
			f->second.seen = frame;
			if (const float age = std::max(ffx_instances::age(inst.address), now - f->second.first_seen); age >= c.e->visual_life)
			{
				// "Persistent" is inferred for many effects (a character's event effects), which then play once and
				// stay behind like the brief ones. One with no children at all draws by itself: it stays lit.
				int working = 0;
				const auto children = ffx_instances::child_state(inst.address, &working);
				const bool lit = c.e->persistent ? children != ffx_instances::children::leftover
					: f->second.level >= kFadeOff && children == ffx_instances::children::running;
				f->second.level = lit ? 1.0f : f->second.level * decay;
				if (lit && !c.e->persistent && age > c.e->visual_life + kLingerLog && g_lingering.insert(inst.ffx_id).second)
					shared::common::log("FfxLights", std::format("effect {} (under {}) lit {:.0f} s past its visual life {:.2f} s, kept on by child {}",
						inst.ffx_id, ffx_instances::top_effect_id(inst.address), age - c.e->visual_life, c.e->visual_life, working));
				if (!lit && c.e->persistent && f->second.level >= kFadeOff && g_ended.insert(inst.ffx_id).second)
					shared::common::log("FfxLights", std::format("persistent effect {} (under {}) ends at age {:.1f} s: only leftovers remain",
						inst.ffx_id, ffx_instances::top_effect_id(inst.address), age));
			}
			c.level = f->second.level;
			if (c.level < kFadeOff)
				continue;
			if (c.e->persistent && std::any_of(covers.begin(), covers.end(), [&](const point_lights::light* l)
				{
					const float dx = l->x - c.pos[0], dy = l->y - c.pos[1], dz = l->z - c.pos[2];
					return dx * dx + dy * dy + dz * dz <= kCoverRadius * kCoverRadius;
				}))
				continue;
			const float dx = c.pos[0] - cam[0], dy = c.pos[1] - cam[1], dz = c.pos[2] - cam[2];
			c.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (c.distance - c.e->dwindle_end <= kReach)
				candidates.push_back(c);
		}
		std::erase_if(g_fades, [&](const auto& t) { return t.second.seen != frame; });
		if (candidates.size() > kMaxLights)
		{
			std::nth_element(candidates.begin(), candidates.begin() + kMaxLights, candidates.end(),
				[](const candidate& a, const candidate& b) { return a.distance < b.distance; });
			candidates.resize(kMaxLights);
		}

		for (const auto& c : candidates)
		{
			auto [it, fresh] = g_slots.try_emplace(c.inst->address, tracked { c.inst->ffx_id, 0, frame });
			if (fresh || it->second.ffx_id != c.inst->ffx_id)
				it->second = { c.inst->ffx_id, free_slot(), frame };
			it->second.seen = frame;

			point_lights::light l {};
			l.slot = it->second.slot;
			l.x = c.pos[0]; l.y = c.pos[1]; l.z = c.pos[2];
			l.r = c.e->r * c.level; l.g = c.e->g * c.level; l.b = c.e->b * c.level;
			l.dwindle_begin = c.e->dwindle_begin;
			l.dwindle_end = c.e->dwindle_end;
			l.has_params = true;
			l.source = point_lights::param_source::ffx_table;
			l.ffx_id = c.inst->ffx_id;
			l.ffx_top_id = ffx_instances::top_effect_id(c.inst->address);
			g_out.push_back(l);
		}
		std::erase_if(g_slots, [&](const auto& t) { return t.second.seen != frame; });
		return g_out;
	}
}
