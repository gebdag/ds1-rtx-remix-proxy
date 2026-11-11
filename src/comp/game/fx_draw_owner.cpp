#include "std_include.hpp"
#include "fx_draw_owner.hpp"
#include "shared/common/config.hpp"

#include <atomic>
#include <cstring>

namespace comp::game::fx_draw_owner
{
	namespace
	{
		// The draw functions of the nine particle host classes (the slot before 0x6F0F40 in each vtable at 0x11E7310..0x11E73B0).
		constexpr DWORD kDraws[] = { 0x0054E150, 0x0054E820, 0x0054F060, 0x005508A0, 0x00551560, 0x00551BB0, 0x005523C0, 0x00552930, 0x00553AC0 };
		constexpr size_t kDrawCount = std::size(kDraws);
		// Each starts push ebp; mov ebp, esp; and esp, -16 ...
		constexpr unsigned char kPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
		// ... and is the slot before 0x6F0F40 of its host class's vtable: the test that this is the executable they were found in.
		constexpr DWORD kSlots[] = { 0x011E7318, 0x011E7328, 0x011E7338, 0x011E7348, 0x011E735C, 0x011E7370, 0x011E7384, 0x011E7398, 0x011E73AC };
		constexpr DWORD kSlotAfter = 0x006F0F40;

		// thiscall as fastcall: ecx the host, edx unused, three stack arguments the callee removes (ret 0xC). The result registers
		// pass through whatever the function leaves in them.
		using draw_fn = unsigned long long(__fastcall*)(void* host, void* edx, void* context, void* cluster, void* arg);
		draw_fn g_original[kDrawCount] = {};
		bool g_installed = false;
		std::atomic<unsigned long long> g_draws { 0 };

		thread_local unsigned t_owner = 0;
		unsigned g_host_owner = 0;

		unsigned owner_of(const void* cluster)
		{
			if (!cluster)
				return 0;
			__try {
				return *static_cast<const unsigned*>(cluster);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		bool prologue_matches(size_t n)
		{
			__try {
				const DWORD* slot = reinterpret_cast<const DWORD*>(kSlots[n]);
				return slot[0] == kDraws[n] && slot[1] == kSlotAfter &&
					std::memcmp(reinterpret_cast<const void*>(kDraws[n]), kPrologue, sizeof(kPrologue)) == 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		template <size_t N>
		unsigned long long __fastcall draw_detour(void* host, void* edx, void* context, void* cluster, void* arg)
		{
			// A host draw does not draw another effect's cluster inside itself, but restoring the value costs nothing.
			const unsigned previous = t_owner;
			t_owner = owner_of(cluster);
			g_draws.fetch_add(1, std::memory_order_relaxed);
			const unsigned long long result = g_original[N](host, edx, context, cluster, arg);
			t_owner = previous;
			return result;
		}

		template <size_t... N>
		bool create_hooks(std::index_sequence<N...>)
		{
			return ((MH_CreateHook(reinterpret_cast<void*>(kDraws[N]), &draw_detour<N>, reinterpret_cast<void**>(&g_original[N])) == MH_OK) && ...);
		}
	}

	void install()
	{
		if (g_installed || shared::globals::lookdev_host)
			return;
		// [Lights] EffectDrawLink=0 leaves the game's particle draws alone (the link is then missing and effect lights keep their table sizes).
		if (shared::common::config::get().get_int("Lights", "EffectDrawLink", 1) == 0)
		{
			shared::common::log("FxDraw", "[Lights] EffectDrawLink is 0: the particle draws are not hooked");
			return;
		}
		for (size_t n = 0; n < kDrawCount; ++n)
		{
			if (!prologue_matches(n))
			{
				shared::common::log("FxDraw", std::format("the particle draw at {:#x} is not where it should be: effect lights keep their table sizes", kDraws[n]),
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
				return;
			}
		}
		if (!create_hooks(std::make_index_sequence<kDrawCount>()))
		{
			shared::common::log("FxDraw", "hooking the particle draws failed: effect lights keep their table sizes",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}
		g_installed = true;
		shared::common::log("FxDraw", std::format("{} particle draws hooked: every sprite batch carries its effect instance", kDrawCount));
	}

	unsigned current()
	{
		return shared::globals::lookdev_host ? g_host_owner : t_owner;
	}

	void set_host_owner(unsigned owner)
	{
		g_host_owner = owner;
	}

	unsigned long long draws_seen()
	{
		return g_draws.load(std::memory_order_relaxed);
	}
}
