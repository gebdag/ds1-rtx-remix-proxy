#include "std_include.hpp"
#include "hitch_watch.hpp"

#include "shared/common/config.hpp"

#include <atomic>
#include <dbghelp.h>
#include <map>
#include <psapi.h>
#include <thread>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

namespace comp::hitch_watch
{
	namespace
	{
		constexpr int kMaxFrames = 28;
		constexpr int kShownFrames = 16;
		constexpr int kMaxSamples = 60;
		constexpr int kSampleEveryMs = 50;
		constexpr int kScanSlots = 4096; // 16 KB of stack above the stack pointer

		enum slot { render = 0, window = 1, slots = 2 };
		const char* const kSlotNames[slots] = { "render", "window" };

		struct sample
		{
			int slot = 0;
			int count = 0;
			uintptr_t frames[kMaxFrames] = {};
		};

		struct module_range
		{
			uintptr_t begin = 0, end = 0;
		};

		std::atomic<HANDLE> g_threads[slots] = {};
		std::atomic<DWORD> g_thread_ids[slots] = {};
		std::atomic<long long> g_last_present{ 0 };
		std::atomic<unsigned long long> g_presents{ 0 };
		std::atomic<bool> g_started{ false };
		std::atomic<bool> g_in_scene{ false };
		int g_limit_ms = 0;

		// The loaded modules' address ranges, read while no thread is suspended.
		module_range g_modules[256];
		int g_module_count = 0;

		long long now()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		double to_ms(long long ticks)
		{
			static const double ms_per_tick = []
			{
				LARGE_INTEGER f;
				QueryPerformanceFrequency(&f);
				return 1000.0 / static_cast<double>(f.QuadPart);
			}();
			return static_cast<double>(ticks) * ms_per_tick;
		}

		void read_modules()
		{
			HMODULE handles[256];
			DWORD needed = 0;
			if (!EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &needed))
				return;
			const int count = std::min<int>(static_cast<int>(needed / sizeof(HMODULE)), 256);
			int kept = 0;
			for (int i = 0; i < count; i++)
			{
				MODULEINFO info{};
				if (GetModuleInformation(GetCurrentProcess(), handles[i], &info, sizeof(info)))
				{
					g_modules[kept].begin = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
					g_modules[kept].end = g_modules[kept].begin + info.SizeOfImage;
					kept++;
				}
			}
			g_module_count = kept;
		}

		bool in_module(uintptr_t address)
		{
			for (int i = 0; i < g_module_count; i++)
				if (address >= g_modules[i].begin && address < g_modules[i].end)
					return true;
			return false;
		}

		// Whether the bytes before this address are a call: then it is where that call returns to.
		bool after_call(uintptr_t address)
		{
			const auto* p = reinterpret_cast<const unsigned char*>(address);
			if (p[-5] == 0xE8)
				return true; // call rel32
			// call r/m32 (FF /2), two to seven bytes long.
			for (int length = 2; length <= 7; length++)
				if (p[-length] == 0xFF && ((p[-length + 1] >> 3) & 7) == 2)
					return true;
			return false;
		}

		// The return addresses on a suspended thread's stack, innermost first. No C++ objects and no calls that take locks.
		int scan(HANDLE thread, uintptr_t* frames)
		{
			int count = 0;
			__try
			{
				CONTEXT context;
				context.ContextFlags = CONTEXT_CONTROL;
				if (!GetThreadContext(thread, &context))
					return 0;
				frames[count++] = context.Eip;
				const auto* stack = reinterpret_cast<const uintptr_t*>(context.Esp);
				for (int i = 0; i < kScanSlots && count < kMaxFrames; i++)
				{
					const uintptr_t value = stack[i];
					if (value > 0x10000 && in_module(value) && after_call(value))
						frames[count++] = value;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				// The end of the stack, or code that cannot be read: what was found is kept.
			}
			return count;
		}

		bool take(int which, sample& out)
		{
			const HANDLE thread = g_threads[which].load();
			if (!thread || SuspendThread(thread) == static_cast<DWORD>(-1))
				return false;
			out.slot = which;
			out.count = scan(thread, out.frames);
			ResumeThread(thread);
			return out.count > 0;
		}

		std::string name_of(uintptr_t address)
		{
			static bool initialised = false;
			const HANDLE process = GetCurrentProcess();
			if (!initialised)
			{
				initialised = true;
				SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
				SymInitialize(process, nullptr, TRUE);
			}
			HMODULE module = nullptr;
			char path[MAX_PATH] = {};
			std::string file = "?";
			uintptr_t base = 0;
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(address), &module) && module && GetModuleFileNameA(module, path, MAX_PATH))
			{
				const char* slash = strrchr(path, '\\');
				file = slash ? slash + 1 : path;
				base = reinterpret_cast<uintptr_t>(module);
			}
			// The game's exe: the address itself (no ASLR, no symbols).
			if (_stricmp(file.c_str(), "DARKSOULS.exe") == 0)
				return std::format("DARKSOULS.exe:0x{:08X}", address);
			char buffer[sizeof(SYMBOL_INFO) + 256] = {};
			auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
			symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
			symbol->MaxNameLen = 255;
			DWORD64 displacement = 0;
			if (SymFromAddr(process, address, &displacement, symbol))
				return std::format("{}!{}", file, symbol->Name);
			return std::format("{}+0x{:X}", file, address - base);
		}

		void report(double gap_ms, const std::vector<sample>& samples)
		{
			std::map<std::string, int> stacks[slots];
			int taken[slots] = {};
			std::map<uintptr_t, std::string> names;
			for (const auto& s : samples)
			{
				std::string line;
				for (int i = 0; i < s.count && i < kShownFrames; i++)
				{
					auto found = names.find(s.frames[i]);
					if (found == names.end())
						found = names.emplace(s.frames[i], name_of(s.frames[i])).first;
					line += (i ? " < " : "") + found->second;
				}
				stacks[s.slot][line]++;
				taken[s.slot]++;
			}
			std::string text = std::format("{:.0f} ms without a Present, {} + {} samples", gap_ms, taken[render], taken[window]);
			for (int which = 0; which < slots; which++)
				for (const auto& [line, count] : stacks[which])
					text += std::format("\n      {} {}x: {}", kSlotNames[which], count, line);
			shared::common::log("HitchStack", text);
		}

		void watchdog()
		{
			std::vector<sample> samples;
			unsigned long long seen = g_presents.load();
			long long late_since = 0, last_sample = 0, modules_read = 0;
			for (;;)
			{
				Sleep(10);
				const unsigned long long presents = g_presents.load();
				const long long t = now();
				if (presents != seen)
				{
					if (!samples.empty())
					{
						report(to_ms(t - late_since), samples);
						samples.clear();
					}
					seen = presents;
					// Modules come and go: read them again every few seconds, between hitches.
					if (!modules_read || to_ms(t - modules_read) > 5000.0)
					{
						read_modules();
						modules_read = t;
					}
					continue;
				}
				const long long last = g_last_present.load();
				if (!last || to_ms(t - last) < g_limit_ms || !g_in_scene.load())
					continue;
				if (samples.empty())
					late_since = last;
				if (samples.size() >= static_cast<size_t>(kMaxSamples) * slots || to_ms(t - last_sample) < kSampleEveryMs)
					continue;
				last_sample = t;
				for (int which = 0; which < slots; which++)
				{
					// The window's thread is often the render thread itself: one sample then.
					if (which == window && g_thread_ids[window].load() == g_thread_ids[render].load())
						continue;
					sample s;
					if (take(which, s))
						samples.push_back(s);
				}
			}
		}

		void set_thread(int which, DWORD id)
		{
			if (!id || g_thread_ids[which].load(std::memory_order_relaxed) == id)
				return;
			// The old handle stays open: the watchdog may be using it.
			if (const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id))
			{
				g_threads[which].store(handle);
				g_thread_ids[which].store(id);
			}
		}
	}

	void on_present(unsigned draws)
	{
		static const int limit_ms = shared::common::config::get().dev_logs() ?
			shared::common::config::get().get_int("Diagnostics", "HitchStackMs", 50): 0;
		if (limit_ms <= 0)
			return;
		set_thread(render, GetCurrentThreadId());
		if (shared::globals::main_window)
			set_thread(window, GetWindowThreadProcessId(shared::globals::main_window, nullptr));
		g_in_scene.store(draws >= 50);
		g_last_present.store(now());
		g_presents.fetch_add(1);
		bool expected = false;
		if (g_started.compare_exchange_strong(expected, true))
		{
			g_limit_ms = limit_ms;
			std::thread(watchdog).detach();
		}
	}
}
