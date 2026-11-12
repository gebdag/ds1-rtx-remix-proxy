#include "std_include.hpp"
#include "fx_owner.hpp"
#include "game_build.hpp"
#include "ffx_instances.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace comp::game::fx_owner
{
	namespace
	{
		// The user object's current draw host, built in place at user + [0x0137E7A0].
		constexpr DWORD kUserHost = 0x08;
		constexpr DWORD kHostOffsetGlobal = 0x0137E7A0;
		// A live point-light host (its destructor swaps the vtable, leaving the slot id behind) and
		// the light slot it lit (int16, -1 until its first update).
		constexpr DWORD kPointLightHostVtable = 0x011A5254;
		constexpr DWORD kHostSlot = 0x0C;

		struct owner
		{
			DWORD host;
			DWORD instance;
			int ffx_id;
			int ffx_top_id;
		};

		std::unordered_map<int, owner> g_owners;
		// Lights the last search found no owner for, by slot, with their entity handle: such a light is
		// not looked for again until its slot holds another light.
		std::unordered_map<int, unsigned> g_unowned;
		int g_walk_instances = 0;
		int g_walk_hosts = 0;
		// The largest distance this period between an owned light and its instance's world position,
		// which the game places it at: a check on the instance position ffx_lights uses.
		float g_check_offset = -1.0f;
		int g_check_lights = 0;
		int g_check_calls = 0;
		constexpr int kCheckPeriod = 600;

		DWORD read_dword(DWORD addr)
		{
			__try {
				return *reinterpret_cast<const DWORD*>(addr);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		int read_short(DWORD addr, bool& ok)
		{
			__try {
				ok = true;
				return *reinterpret_cast<const short*>(addr);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				ok = false;
				return -1;
			}
		}

		// The light slot a live point-light host lit, or -1.
		int host_slot(DWORD host)
		{
			static const DWORD host_vtable = build::at(kPointLightHostVtable); // 2015-06-15 address -> running build's
			if (!host || !host_vtable || read_dword(host) != host_vtable)
				return -1;
			bool ok = false;
			const int slot = read_short(host + kHostSlot, ok);
			return ok ? slot : -1;
		}

		// Every point-light host among the live effect instances, by slot.
		void search()
		{
			g_owners.clear();
			g_walk_hosts = 0;
			// The lookdev host's instances are a capture's: their addresses are the game's, not memory to read here.
			if (shared::globals::lookdev_host)
				return;
			const auto& instances = ffx_instances::current();
			g_walk_instances = static_cast<int>(instances.size());
			const DWORD host_offset_global = build::at(kHostOffsetGlobal);
			const DWORD host_offset = host_offset_global ? read_dword(host_offset_global) : 0;
			if (!host_offset)
				return;
			for (const auto& inst : instances)
			{
				const DWORD host = inst.user ? read_dword(inst.user + kUserHost) : 0;
				if (!host || host != inst.user + host_offset)
					continue;
				const int slot = host_slot(host);
				if (slot < 0)
					continue;
				g_owners[slot] = { host, inst.address, inst.ffx_id, ffx_instances::top_effect_id(inst.address) };
				++g_walk_hosts;
			}
		}

		bool is_direct(const point_lights::light& l)
		{
			return l.entity && std::strcmp(point_lights::entity_class_name(l.entity_vtable), "Direct") == 0;
		}
	}

	void resolve(std::vector<point_lights::light>& lights)
	{
		bool searched = false;
		for (auto& l : lights)
		{
			l.ffx_id = 0;
			l.ffx_top_id = 0;
			if (!is_direct(l))
				continue;

			auto it = g_owners.find(l.slot);
			const bool cached = it != g_owners.end() && host_slot(it->second.host) == l.slot;
			const auto unowned = g_unowned.find(l.slot);
			const bool known_unowned = unowned != g_unowned.end() && unowned->second == l.entity_handle;
			if (!cached && !known_unowned && !searched)
			{
				search();
				searched = true;
				it = g_owners.find(l.slot);
			}
			if (it != g_owners.end() && host_slot(it->second.host) == l.slot)
			{
				l.ffx_id = it->second.ffx_id;
				l.ffx_top_id = it->second.ffx_top_id;
				g_unowned.erase(l.slot);
				float pos[3];
				if (ffx_instances::world_position(it->second.instance, pos))
				{
					const float dx = pos[0] - l.x, dy = pos[1] - l.y, dz = pos[2] - l.z;
					g_check_offset = std::max(g_check_offset, std::sqrt(dx * dx + dy * dy + dz * dz));
					++g_check_lights;
				}
			}
			else if (searched)
			{
				g_unowned[l.slot] = l.entity_handle;
			}
		}
		if (++g_check_calls >= kCheckPeriod)
		{
			if (g_check_lights)
				shared::common::log("FxOwner", std::format("instance position check: {} owned light samples, largest offset {:.3f} m",
					g_check_lights, g_check_offset));
			g_check_calls = 0;
			g_check_lights = 0;
			g_check_offset = -1.0f;
		}
	}

	int last_walk_instances() { return g_walk_instances; }
	int last_walk_hosts() { return g_walk_hosts; }
}
