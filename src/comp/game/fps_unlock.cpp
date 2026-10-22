// Frame rate above 30.
//
// The game holds 30 fps in two ways:
//   - Its swap-interval setter stores 2 (two vertical blanks a frame) into the render object at 0xFFB688; 5 takes the
//     wait away.
//   - Everything that advances per frame adds one float, 1/30 at 0x11E7E90 (37 code references). It is set to the
//     length of each frame, held between 1/TargetFPS and 1/10 s: real speed from 10 fps up to the cap.
//
// The frame length is measured on the draw thread: its dispatcher fetches one command at a time (the call at 0xBAC4ED
// to a getter that reads [ecx+0xC]); commands 2 and 5 each come once a frame and mark its start. The frame cap runs in
// Present.
#include "std_include.hpp"
#include "fps_unlock.hpp"
#include <algorithm>
#include <cstring>
#include "shared/common/config.hpp"

namespace comp::game::fps_unlock
{
	// 2015-06-15 VAs; the game's image has a fixed base.
	static constexpr DWORD kFrameStep = 0x011E7E90;       // float, 1/30 as shipped
	static constexpr DWORD kIntervalStore = 0x00FFB688;   // mov dword ptr [esi+0x248], 2
	static constexpr DWORD kCommandCall = 0x00BAC4ED;     // call <command getter>
	static constexpr unsigned char kIntervalStoreBytes[] = { 0xC7, 0x86, 0x48, 0x02, 0x00, 0x00 };
	static constexpr unsigned char kCommandGetterBytes[] = { 0x8B, 0x41, 0x0C, 0xC3 }; // mov eax, [ecx+0xC]; ret
	static constexpr DWORD kIntervalTwo = 2;
	static constexpr DWORD kIntervalNone = 5;
	static constexpr int kFrameStartCommand = 2;
	static constexpr int kLoadingFrameCommand = 5;
	static constexpr float kMinStepFps = 10.0f;            // a longer frame slows the game instead of taking a longer step
	static constexpr float kFrozenStep = 1e-5f;            // F7: time all but stops (the physics may divide by the step)

	static DWORD at(DWORD va) { return va; }

	static float* s_step = nullptr;
	static DWORD s_getter = 0; // the command getter the call site went to
	static bool s_active = false;
	static bool s_frozen = false;
	static bool s_cap_bypass = false;
	static int s_target_fps = 60;
	static float s_step_before_freeze = 1.0f / 30.0f;

	static LARGE_INTEGER s_freq {};
	static LARGE_INTEGER s_start {};
	static double s_last_frame_ms = 0.0;
	static double s_last_present_ms = 0.0;

	static double now_ms()
	{
		LARGE_INTEGER c;
		QueryPerformanceCounter(&c);
		return static_cast<double>(c.QuadPart - s_start.QuadPart) * 1000.0 / static_cast<double>(s_freq.QuadPart);
	}

	static bool unprotect(DWORD address, size_t size, DWORD protection)
	{
		DWORD old = 0;
		return VirtualProtect(reinterpret_cast<void*>(address), size, protection, &old) != 0;
	}

	// The step lives in read-only data: made writable once, then plain stores. Only the game has it (the lookdev host's
	// image is another exe).
	static float* find_step()
	{
		if (shared::globals::lookdev_host)
			return nullptr;
		const DWORD address = at(kFrameStep);
		if (!address || *reinterpret_cast<const float*>(address) != 1.0f / 30.0f || !unprotect(address, sizeof(float), PAGE_READWRITE))
			return nullptr;
		return reinterpret_cast<float*>(address);
	}

	static void write_step(float step)
	{
		*s_step = step;
	}

	static void on_frame_start()
	{
		const double now = now_ms();
		const double frame_ms = now - s_last_frame_ms;
		s_last_frame_ms = now;
		if (s_frozen)
			return;
		const float fps = std::clamp(static_cast<float>(1000.0 / frame_ms), kMinStepFps, static_cast<float>(s_target_fps));
		write_step(1.0f / fps);
	}

	static void __stdcall on_command(int command)
	{
		if (command == kFrameStartCommand || command == kLoadingFrameCommand)
			on_frame_start();
	}

	// Stands in for the getter at the dispatcher's call: the command comes back in eax as before, and ecx/edx as the getter
	// leaves them, so the call behaves exactly like the getter's.
	static __declspec(naked) void command_fetch()
	{
		__asm
		{
			call dword ptr [s_getter]
			push ecx
			push edx
			push eax
			push eax
			call on_command
			pop eax
			pop edx
			pop ecx
			ret
		}
	}

	// The stored interval's immediate, if the setter is as shipped.
	static DWORD find_interval_value()
	{
		const DWORD store = at(kIntervalStore);
		if (!store || std::memcmp(reinterpret_cast<const void*>(store), kIntervalStoreBytes, sizeof(kIntervalStoreBytes)) != 0)
			return 0;
		const DWORD value = store + sizeof(kIntervalStoreBytes);
		return *reinterpret_cast<const DWORD*>(value) == kIntervalTwo ? value : 0;
	}

	// The dispatcher's call to the command getter, if both are as shipped.
	static DWORD find_command_call()
	{
		const DWORD call = at(kCommandCall);
		if (!call || *reinterpret_cast<const unsigned char*>(call) != 0xE8)
			return 0;
		const DWORD getter = call + 5 + *reinterpret_cast<const DWORD*>(call + 1);
		if (std::memcmp(reinterpret_cast<const void*>(getter), kCommandGetterBytes, sizeof(kCommandGetterBytes)) != 0)
			return 0;
		s_getter = getter;
		return call;
	}

	static void write_code_dword(DWORD address, DWORD value)
	{
		unprotect(address, sizeof(DWORD), PAGE_EXECUTE_READWRITE);
		*reinterpret_cast<DWORD*>(address) = value;
		FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), sizeof(DWORD));
	}

	void init()
	{
		const auto& cfg = shared::common::config::get();
		if (!cfg.fps_unlock.enabled)
			return;
		set_target_fps(cfg.fps_unlock.target_fps);
		QueryPerformanceFrequency(&s_freq);
		QueryPerformanceCounter(&s_start);

		// All three are checked before any game byte changes: the interval alone would run the game uncapped at the 1/30 step.
		s_step = find_step();
		const DWORD interval = find_interval_value();
		const DWORD call = find_command_call();
		if (!s_step || !interval || !call)
		{
			shared::common::log("FPSUnlock", "the game's frame timing is not where it should be on this build: the game stays at 30 fps",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}
		write_code_dword(interval, kIntervalNone);
		write_code_dword(call + 1, reinterpret_cast<DWORD>(&command_fetch) - (call + 5));
		s_active = true;
		shared::common::log("FPSUnlock", std::format("frame rate unlocked, cap {} fps", s_target_fps), shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
	}

	bool is_active() { return s_active; }

	void set_frozen(bool frozen)
	{
		if (frozen == s_frozen)
			return;
		if (!s_step)
			s_step = find_step(); // the freeze works with the unlock off too
		if (!s_step)
			return;
		if (frozen)
		{
			s_step_before_freeze = *s_step;
			s_frozen = true;
			write_step(kFrozenStep);
		}
		else
		{
			write_step(s_step_before_freeze);
			s_frozen = false;
		}
	}

	bool is_frozen() { return s_frozen; }

	int target_fps() { return s_target_fps; }

	void set_cap_bypass(bool bypass) { s_cap_bypass = bypass; }

	void set_target_fps(int fps) { s_target_fps = std::clamp(fps, 30, 300); }

	void on_present()
	{
		if (!s_active)
			return;
		if (!s_cap_bypass)
		{
			// A tenth of a millisecond early: Present itself takes a moment.
			const double frame_ms = 1000.0 / s_target_fps - 0.1;
			while (now_ms() - s_last_present_ms < frame_ms)
				SwitchToThread();
		}
		s_last_present_ms = now_ms();
	}
}
