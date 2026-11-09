#include "std_include.hpp"
#include "hit_sparks.hpp"
#include "ffx_instances.hpp"
#include "shared/common/config.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_map>

namespace comp::game::hit_sparks
{
	namespace
	{
		struct record { unsigned streaks; float r, g, b; float speed; };
		struct effect { int id; int records; record lines[3]; };
		const effect kEffects[] = {
#include "hit_spark_table.inc"
		};

		// An instance first seen later than this has been running a while (an area just came into the walk, a save loaded): its burst is over.
		constexpr float kNewAge = 0.25f;
		// An instance that emits as long as it runs stops counting after this long, should it stay behind in the tree.
		constexpr float kLongestEmission = 6.0f;
		// A second instance this close in the same frame is the same hit.
		constexpr float kSameHit = 0.1f;

		struct seen { UINT frame; float age; bool fired; };
		std::unordered_map<unsigned, seen> g_seen;
		int g_logged = 0;

		const effect* find_effect(int id)
		{
			static const auto table = []
			{
				std::unordered_map<int, const effect*> m;
				for (const auto& e : kEffects)
					m[e.id] = &e;
				return m;
			}();
			const auto it = table.find(id);
			return it == table.end() ? nullptr : it->second;
		}

		void add(std::vector<shared::common::ffp_state::spark_emission>& out, const effect& e, const float* at, bool running)
		{
			for (int i = 0; i < e.records; ++i)
			{
				const record& r = e.lines[i];
				if (running != (r.streaks == 0))
					continue;
				out.push_back({ { at[0], at[1], at[2] }, { r.r, r.g, r.b }, r.speed, r.streaks ? r.streaks : 1u });
			}
		}

		// Replays have no game memory: hits at given places, every TestInterval frames. proxy.Sparks.TestHit and TestHit2 to TestHit4.
		void test_hits(std::vector<shared::common::ffp_state::spark_emission>& out, UINT frame)
		{
			const auto& cfg = shared::common::config::get();
			const int interval = std::max(cfg.get_int("Sparks", "TestInterval", 30), 1);
			if (frame % interval != 0)
				return;
			static const char* const keys[] = { "TestHit", "TestHit2", "TestHit3", "TestHit4" };
			const effect* stone = find_effect(190012000);
			for (const char* key : keys)
			{
				float at[3];
				if (stone && cfg.get_float3("Sparks", key, at))
					add(out, *stone, at, false);
			}
		}
	}

	void collect(std::vector<shared::common::ffp_state::spark_emission>& out)
	{
		const UINT frame = shared::common::ffp_state::get().frame_count();
		if (shared::globals::lookdev_host)
		{
			test_hits(out, frame);   // reads the INI, which is for the host and never the game
			return;
		}

		const size_t first = out.size();
		for (const auto& inst : ffx_instances::current())
		{
			const effect* e = find_effect(inst.ffx_id);
			if (!e)
				continue;
			const float age = ffx_instances::age(inst.address);
			if (!std::isfinite(age))
				continue;
			auto [it, fresh] = g_seen.try_emplace(inst.address, seen { frame, age, false });
			seen& s = it->second;
			// A new instance at a freed one's address: its age starts again.
			if (!fresh && age + 0.05f < s.age)
				s.fired = false;
			s.frame = frame;
			s.age = age;
			// Not before the game has updated it once: only then does its matrix hold this hit's place, and not a dead instance's left in the block.
			float at[3];
			if (!(age > 0.0f) || !ffx_instances::world_position(inst.address, at))
				continue;
			if (!s.fired)
			{
				s.fired = true;
				bool same = age >= kNewAge;
				for (size_t i = first; i < out.size() && !same; ++i)
				{
					const float dx = out[i].pos[0] - at[0], dy = out[i].pos[1] - at[1], dz = out[i].pos[2] - at[2];
					same = dx * dx + dy * dy + dz * dz < kSameHit * kSameHit;
				}
				if (!same)
				{
					add(out, *e, at, false);
					if (g_logged < 256)
					{
						++g_logged;
						shared::common::log("Sparks", std::format("hit: effect {} at ({:.2f}, {:.2f}, {:.2f}), age {:.3f}, frame {}", e->id, at[0], at[1], at[2], age, frame));
					}
				}
			}
			if (age < kLongestEmission)
				add(out, *e, at, true);
		}
		std::erase_if(g_seen, [&](const auto& kv) { return kv.second.frame != frame; });
	}
}
