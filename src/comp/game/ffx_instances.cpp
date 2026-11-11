#include "std_include.hpp"
#include "ffx_instances.hpp"
#include "shared/common/ffp_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace comp::game::ffx_instances
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
		// Its world matrix, row-major 3x4 at +0x30 (translation +0x3C / +0x4C / +0x5C), and its state
		// word, bit 30 set while the instance is valid.
		constexpr DWORD kInstanceTranslation[] = { 0x3C, 0x4C, 0x5C };
		constexpr DWORD kInstanceState = 0x68;
		constexpr DWORD kStateValid = 0x40000000;
		constexpr DWORD kInstanceAge = 0x7C;
		// The FXR1 root object {133, effectId}, its type word replaced by this vtable on load.
		constexpr DWORD kRootVtable = 0x011309CC;
		constexpr DWORD kRootId = 0x04;
		// Library effect 2101 has no appearance of its own, and stays in the tree after the effect has ended:
		// the Moonlight Butterfly's spear impacts (13237) keep one until the area unloads.
		constexpr int kLeftoverEffect = 2101;

		// Library effect 2118 likewise stays behind, in brief effects (Hellkite's fire). Not the other "no sprite" library effects: 2023 is a live emitter,
		// and nearly every brief effect holds nothing but such children.
		constexpr int kBriefLeftoverEffect = 2118;

		// Effect trees are small; the caps guard against a list changing under the walk.
		constexpr int kMaxInstances = 8192;
		constexpr int kMaxGroups = 4096;
		constexpr int kMaxParents = 32;
		constexpr int kMaxTreeNodes = 256;

		std::vector<instance> g_instances;
		UINT g_walk_frame = 0;
		bool g_walked = false;
		// The lookdev host: a capture's instances (set_snapshot), by address.
		std::vector<snapshot> g_snapshot;
		std::unordered_map<unsigned, size_t> g_snapshot_index;

		void read_matrix(DWORD address, float* out)
		{
			__try {
				std::memcpy(out, reinterpret_cast<const void*>(address + 0x30), sizeof(float) * 12);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				std::memset(out, 0, sizeof(float) * 12);
			}
		}

		DWORD read_dword(DWORD addr)
		{
			__try {
				return *reinterpret_cast<const DWORD*>(addr);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		int effect_id(DWORD address)
		{
			const DWORD root = read_dword(address + kInstanceRoot);
			return root && read_dword(root) == kRootVtable ? static_cast<int>(read_dword(root + kRootId)) : 0;
		}

		void walk_manager(DWORD manager)
		{
			const DWORD adapter = read_dword(manager + kManagerAdapter);
			if (!adapter || read_dword(adapter) != kAdapterVtable)
				return;

			std::vector<DWORD> stack;
			int groups = 0;
			for (DWORD group = read_dword(manager + kManagerGroups); group && g_instances.size() < kMaxInstances && groups < kMaxGroups;
				group = read_dword(group + kGroupNext), ++groups)
			{
				if (const DWORD first = read_dword(group + kGroupFirst))
					stack.push_back(first);
				while (!stack.empty() && g_instances.size() < kMaxInstances)
				{
					const DWORD address = stack.back();
					stack.pop_back();
					if (const DWORD next = read_dword(address + kInstanceNext))
						stack.push_back(next);
					if (const DWORD child = read_dword(address + kInstanceChild))
						stack.push_back(child);
					g_instances.push_back({ address, read_dword(address + kInstanceUser), effect_id(address) });
				}
				stack.clear();
			}
		}

		void walk()
		{
			g_instances.clear();
			const DWORD frpg = read_dword(kFrpgFx);
			if (!frpg)
				return;

			DWORD walked[std::size(kSubManagers)] = {};
			for (size_t i = 0; i < std::size(kSubManagers); ++i)
			{
				const DWORD sub = read_dword(frpg + kSubManagers[i]);
				const DWORD manager = sub ? read_dword(sub + kSubFxManager) : 0;
				if (!manager || std::find(std::begin(walked), std::end(walked), manager) != std::end(walked))
					continue;
				walked[i] = manager;
				walk_manager(manager);
			}
		}
	}

	std::vector<snapshot> take_snapshot(bool (*with_children)(int ffx_id))
	{
		std::vector<snapshot> out;
		const auto& live = current();
		out.reserve(live.size());
		for (const auto& inst : live)
		{
			snapshot s {};
			s.address = inst.address;
			s.user = inst.user;
			s.ffx_id = inst.ffx_id;
			s.top_id = top_effect_id(inst.address);
			if (world_position(inst.address, s.position))
				s.flags |= kSnapshotPosition;
			s.age = age(inst.address);
			if (with_children && with_children(inst.ffx_id))
			{
				s.children = static_cast<int>(child_state(inst.address, &s.working, false));
				s.children_brief = static_cast<int>(child_state(inst.address, &s.working_brief, true));
			}
			read_matrix(inst.address, s.matrix);
			s.level = -1.0f;
			s.parent = read_dword(inst.address + kInstanceParent);
			out.push_back(s);
		}
		return out;
	}

	void set_snapshot(const void* items, unsigned count, unsigned element_size)
	{
		g_snapshot.clear();
		g_snapshot_index.clear();
		g_instances.clear();
		if (!items || !element_size || element_size > sizeof(snapshot))
			return;
		// A capture from an older build holds shorter records: its missing fields stay zero, the level unknown.
		g_snapshot.assign(count, snapshot {});
		for (unsigned i = 0; i < count; ++i)
		{
			if (element_size < sizeof(snapshot))
				g_snapshot[i].level = -1.0f;
			std::memcpy(&g_snapshot[i], static_cast<const unsigned char*>(items) + static_cast<size_t>(i) * element_size, element_size);
			g_snapshot_index[g_snapshot[i].address] = i;
			g_instances.push_back({ g_snapshot[i].address, g_snapshot[i].user, g_snapshot[i].ffx_id });
		}
	}

	const snapshot* captured(unsigned address)
	{
		if (!shared::globals::lookdev_host)
			return nullptr;
		const auto it = g_snapshot_index.find(address);
		return it != g_snapshot_index.end() ? &g_snapshot[it->second] : nullptr;
	}

	const std::vector<instance>& current()
	{
		// The host has no game memory: the capture's instances.
		if (shared::globals::lookdev_host)
			return g_instances;
		const UINT frame = shared::common::ffp_state::get().frame_count();
		if (!g_walked || frame != g_walk_frame)
		{
			walk();
			g_walk_frame = frame;
			g_walked = true;
		}
		return g_instances;
	}

	int top_effect_id(unsigned address)
	{
		if (shared::globals::lookdev_host)
		{
			const snapshot* s = captured(address);
			return s ? s->top_id : 0;
		}
		DWORD top = address;
		for (int i = 0; i < kMaxParents; ++i)
		{
			const DWORD parent = read_dword(top + kInstanceParent);
			if (!parent)
				break;
			top = parent;
		}
		return effect_id(top);
	}

	bool world_position(unsigned address, float* out)
	{
		if (shared::globals::lookdev_host)
		{
			const snapshot* s = captured(address);
			if (!s || !(s->flags & kSnapshotPosition))
				return false;
			std::memcpy(out, s->position, sizeof(s->position));
			return true;
		}
		if (!(read_dword(address + kInstanceState) & kStateValid))
			return false;
		__try {
			for (int i = 0; i < 3; ++i)
				out[i] = *reinterpret_cast<const float*>(address + kInstanceTranslation[i]);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
		return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
	}

	float age(unsigned address)
	{
		if (shared::globals::lookdev_host)
		{
			const snapshot* s = captured(address);
			return s ? s->age : 0.0f;
		}
		const DWORD bits = read_dword(address + kInstanceAge);
		return *reinterpret_cast<const float*>(&bits);
	}

	children child_state(unsigned address, int* working, bool brief)
	{
		if (shared::globals::lookdev_host)
		{
			const snapshot* s = captured(address);
			if (!s)
				return children::none;
			if (working)
				*working = brief ? s->working_brief : s->working;
			return static_cast<children>(brief ? s->children_brief : s->children);
		}
		DWORD stack[kMaxTreeNodes];
		int depth = 0;
		if (const DWORD child = read_dword(address + kInstanceChild))
			stack[depth++] = child;
		else
			return children::none;
		for (int visited = 0; depth > 0 && visited < kMaxTreeNodes; ++visited)
		{
			const DWORD node = stack[--depth];
			if (const int id = effect_id(node); id != kLeftoverEffect && !(brief && id == kBriefLeftoverEffect))
			{
				if (working)
					*working = id;
				return children::running;
			}
			if (const DWORD next = read_dword(node + kInstanceNext); next && depth < kMaxTreeNodes)
				stack[depth++] = next;
			if (const DWORD child = read_dword(node + kInstanceChild); child && depth < kMaxTreeNodes)
				stack[depth++] = child;
		}
		return children::leftover;
	}

	int last_walk_count() { return static_cast<int>(g_instances.size()); }

	unsigned parent_of(unsigned address)
	{
		if (shared::globals::lookdev_host)
		{
			const snapshot* s = captured(address);
			return s ? s->parent : 0;
		}
		return read_dword(address + kInstanceParent);
	}
}
