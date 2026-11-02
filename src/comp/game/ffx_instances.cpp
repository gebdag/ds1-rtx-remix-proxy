#include "std_include.hpp"
#include "ffx_instances.hpp"
#include "shared/common/ffp_state.hpp"

#include <algorithm>
#include <cmath>

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

		// Effect trees are small; the caps guard against a list changing under the walk.
		constexpr int kMaxInstances = 8192;
		constexpr int kMaxParents = 32;
		constexpr int kMaxTreeNodes = 256;

		std::vector<instance> g_instances;
		UINT g_walk_frame = 0;
		bool g_walked = false;

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
			for (DWORD group = read_dword(manager + kManagerGroups); group && g_instances.size() < kMaxInstances;
				group = read_dword(group + kGroupNext))
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

	const std::vector<instance>& current()
	{
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
		const DWORD bits = read_dword(address + kInstanceAge);
		return *reinterpret_cast<const float*>(&bits);
	}

	children child_state(unsigned address, int* working)
	{
		DWORD stack[kMaxTreeNodes];
		int depth = 0;
		if (const DWORD child = read_dword(address + kInstanceChild))
			stack[depth++] = child;
		else
			return children::none;
		for (int visited = 0; depth > 0 && visited < kMaxTreeNodes; ++visited)
		{
			const DWORD node = stack[--depth];
			if (const int id = effect_id(node); id != kLeftoverEffect)
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
}
