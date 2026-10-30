#include "std_include.hpp"
#include "fx_owner.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace comp::game::fx_owner
{
	namespace
	{
		// FrpgFx system (created at boot, cleared only at shutdown) and its two effect managers:
		// frpgFxManagerBase at +0x10 and +0x14 (byte +0x18 picks the one that creates), each
		// holding the FFX::FXManager at +4. findings.db rows 256-263.
		constexpr DWORD kFrpgFx = 0x01378564;
		constexpr DWORD kSubManagers[] = { 0x10, 0x14 };
		constexpr DWORD kSubFxManager = 0x04;
		// FFX::FXManager: its adapter (+0x15C) is FrpgFxAdapterBase, a sanity check on the walk.
		constexpr DWORD kManagerAdapter = 0x15C;
		constexpr DWORD kAdapterVtable = 0x011A44F4;
		// Instance groups (newest first), each with its top-level instances.
		constexpr DWORD kManagerGroups = 0x14;
		constexpr DWORD kGroupNext = 0x30;
		constexpr DWORD kGroupFirst = 0x34;
		// An effect instance: siblings, children, parent, its user object and its effect definition.
		constexpr DWORD kInstanceNext = 0x88;
		constexpr DWORD kInstanceChild = 0x8C;
		constexpr DWORD kInstanceParent = 0x84;
		constexpr DWORD kInstanceUser = 0x64;
		constexpr DWORD kInstanceRoot = 0x90;
		// The user object's current draw host, built in place at user + [0x0137E7A0].
		constexpr DWORD kUserHost = 0x08;
		constexpr DWORD kHostOffsetGlobal = 0x0137E7A0;
		// A live point-light host (its destructor swaps the vtable, leaving the slot id behind) and
		// the light slot it lit (int16, -1 until its first update).
		constexpr DWORD kPointLightHostVtable = 0x011A5254;
		constexpr DWORD kHostSlot = 0x0C;
		// The FXR1 root object {133, effectId}, its type word replaced by this vtable on load.
		constexpr DWORD kRootVtable = 0x011309CC;
		constexpr DWORD kRootId = 0x04;

		// Effect trees are small; the cap guards against a list changing under the walk.
		constexpr int kMaxInstances = 8192;
		constexpr int kMaxParents = 32;

		struct owner
		{
			DWORD host;
			int ffx_id;
			int ffx_top_id;
		};

		std::unordered_map<int, owner> g_owners;
		// Lights the last walk found no owner for, by slot, with their entity handle: such a light is
		// not looked for again until its slot holds another light.
		std::unordered_map<int, unsigned> g_unowned;
		int g_walk_instances = 0;
		int g_walk_hosts = 0;

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
			if (!host || read_dword(host) != kPointLightHostVtable)
				return -1;
			bool ok = false;
			const int slot = read_short(host + kHostSlot, ok);
			return ok ? slot : -1;
		}

		int effect_id(DWORD instance)
		{
			const DWORD root = read_dword(instance + kInstanceRoot);
			return root && read_dword(root) == kRootVtable ? static_cast<int>(read_dword(root + kRootId)) : 0;
		}

		int top_effect_id(DWORD instance)
		{
			DWORD top = instance;
			for (int i = 0; i < kMaxParents; ++i)
			{
				const DWORD parent = read_dword(top + kInstanceParent);
				if (!parent)
					break;
				top = parent;
			}
			return effect_id(top);
		}

		// Every point-light host in one manager's effect tree, by slot.
		void walk_manager(DWORD manager, int host_offset, int& visited)
		{
			const DWORD adapter = read_dword(manager + kManagerAdapter);
			if (!adapter || read_dword(adapter) != kAdapterVtable)
				return;

			std::vector<DWORD> stack;
			for (DWORD group = read_dword(manager + kManagerGroups); group && visited < kMaxInstances;
				group = read_dword(group + kGroupNext))
			{
				if (const DWORD first = read_dword(group + kGroupFirst))
					stack.push_back(first);
				while (!stack.empty() && visited < kMaxInstances)
				{
					const DWORD instance = stack.back();
					stack.pop_back();
					++visited;
					if (const DWORD next = read_dword(instance + kInstanceNext))
						stack.push_back(next);
					if (const DWORD child = read_dword(instance + kInstanceChild))
						stack.push_back(child);

					const DWORD user = read_dword(instance + kInstanceUser);
					const DWORD host = user ? read_dword(user + kUserHost) : 0;
					if (!host || host != user + host_offset)
						continue;
					const int slot = host_slot(host);
					if (slot < 0)
						continue;
					g_owners[slot] = { host, effect_id(instance), top_effect_id(instance) };
					++g_walk_hosts;
				}
				stack.clear();
			}
		}

		void walk()
		{
			g_owners.clear();
			g_walk_instances = 0;
			g_walk_hosts = 0;

			const DWORD frpg = read_dword(kFrpgFx);
			const int host_offset = static_cast<int>(read_dword(kHostOffsetGlobal));
			if (!frpg || !host_offset)
				return;

			DWORD walked[std::size(kSubManagers)] = {};
			for (size_t i = 0; i < std::size(kSubManagers); ++i)
			{
				const DWORD sub = read_dword(frpg + kSubManagers[i]);
				const DWORD manager = sub ? read_dword(sub + kSubFxManager) : 0;
				if (!manager || std::find(std::begin(walked), std::end(walked), manager) != std::end(walked))
					continue;
				walked[i] = manager;
				walk_manager(manager, host_offset, g_walk_instances);
			}
		}

		bool is_direct(const point_lights::light& l)
		{
			return l.entity && std::strcmp(point_lights::entity_class_name(l.entity_vtable), "Direct") == 0;
		}
	}

	void resolve(std::vector<point_lights::light>& lights)
	{
		bool walked = false;
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
			if (!cached && !known_unowned && !walked)
			{
				walk();
				walked = true;
				it = g_owners.find(l.slot);
			}
			if (it != g_owners.end() && host_slot(it->second.host) == l.slot)
			{
				l.ffx_id = it->second.ffx_id;
				l.ffx_top_id = it->second.ffx_top_id;
				g_unowned.erase(l.slot);
			}
			else if (walked)
			{
				g_unowned[l.slot] = l.entity_handle;
			}
		}
	}

	int last_walk_instances() { return g_walk_instances; }
	int last_walk_hosts() { return g_walk_hosts; }
}
