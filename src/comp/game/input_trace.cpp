#include "std_include.hpp"
#include "input_trace.hpp"

#include "shared/common/config.hpp"
#include "shared/common/dinput_hook_v2.hpp"

namespace comp::game::input_trace
{
	namespace
	{
		struct second
		{
			uint32_t pad_calls = 0, pad_calls_game = 0;
			DWORD pad_result = ERROR_DEVICE_NOT_CONNECTED;
			XINPUT_GAMEPAD pad_last {};
			WORD pad_buttons_seen = 0;
			SHORT pad_lx_min = 0, pad_lx_max = 0, pad_ly_min = 0, pad_ly_max = 0;
			uint32_t raw_key_press = 0, raw_key_release = 0, raw_key_dropped = 0;
			uint32_t raw_mouse = 0, raw_mouse_held = 0;
			int64_t raw_dx = 0, raw_dy = 0;
			uint32_t raw_buttons = 0;
			uint32_t di_keyboard = 0, di_mouse = 0, di_joystick = 0, di_other = 0, di_held = 0, di_from_game = 0;
			uint32_t key_state_calls = 0, key_state_held = 0, keys_down_max = 0;
		};

		std::mutex s_lock;
		second s_now;
		ULONGLONG s_next_report = 0;
	}

	bool enabled()
	{
		static const bool on = shared::common::config::get().dev_logs();
		return on;
	}

	bool is_game_address(const void* return_address)
	{
		static const auto range = []
		{
			const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
			return std::pair { base, base + nt->OptionalHeader.SizeOfImage };
		}();
		const auto a = reinterpret_cast<uintptr_t>(return_address);
		return a >= range.first && a < range.second;
	}

	void on_xinput(const bool from_game, const DWORD user, const DWORD result, const XINPUT_STATE* state)
	{
		if (!enabled() || user != 0)
			return;
		std::lock_guard lock(s_lock);
		auto& s = s_now;
		s.pad_calls++;
		s.pad_calls_game += from_game ? 1 : 0;
		s.pad_result = result;
		if (result != ERROR_SUCCESS || !state)
			return;
		const auto& g = state->Gamepad;
		if (s.pad_buttons_seen == 0 && s.pad_lx_min == 0 && s.pad_lx_max == 0 && s.pad_ly_min == 0 && s.pad_ly_max == 0)
		{
			s.pad_lx_min = s.pad_lx_max = g.sThumbLX;
			s.pad_ly_min = s.pad_ly_max = g.sThumbLY;
		}
		s.pad_last = g;
		s.pad_buttons_seen |= g.wButtons;
		s.pad_lx_min = std::min(s.pad_lx_min, g.sThumbLX);
		s.pad_lx_max = std::max(s.pad_lx_max, g.sThumbLX);
		s.pad_ly_min = std::min(s.pad_ly_min, g.sThumbLY);
		s.pad_ly_max = std::max(s.pad_ly_max, g.sThumbLY);
	}

	void on_raw_keyboard(const bool press, const bool dropped)
	{
		if (!enabled())
			return;
		std::lock_guard lock(s_lock);
		(press ? s_now.raw_key_press : s_now.raw_key_release)++;
		s_now.raw_key_dropped += dropped ? 1 : 0;
	}

	void on_raw_mouse(const LONG dx, const LONG dy, const USHORT buttons, const bool held)
	{
		if (!enabled())
			return;
		std::lock_guard lock(s_lock);
		s_now.raw_mouse++;
		s_now.raw_mouse_held += held ? 1 : 0;
		s_now.raw_dx += dx;
		s_now.raw_dy += dy;
		s_now.raw_buttons |= buttons;
	}

	void on_di_state(const bool from_game, const DWORD size, const bool held)
	{
		if (!enabled())
			return;
		std::lock_guard lock(s_lock);
		auto& s = s_now;
		if (size == 256)
			s.di_keyboard++;
		else if (size == sizeof(DIMOUSESTATE) || size == sizeof(DIMOUSESTATE2))
			s.di_mouse++;
		else if (size == sizeof(DIJOYSTATE) || size == sizeof(DIJOYSTATE2))
			s.di_joystick++;
		else
			s.di_other++;
		s.di_held += held ? 1 : 0;
		s.di_from_game += from_game ? 1 : 0;
	}

	void on_keyboard_state(const BYTE* keys, const bool held)
	{
		if (!enabled() || !keys)
			return;
		uint32_t down = 0;
		for (int i = 0; i < 256; i++)
			down += (keys[i] & 0x80) ? 1 : 0;
		std::lock_guard lock(s_lock);
		s_now.key_state_calls++;
		s_now.key_state_held += held ? 1 : 0;
		s_now.keys_down_max = std::max(s_now.keys_down_max, down);
	}

	void flush()
	{
		if (!enabled())
			return;
		const ULONGLONG now = GetTickCount64();
		if (now < s_next_report)
			return;
		s_next_report = now + 1000;

		second s;
		{
			std::lock_guard lock(s_lock);
			s = s_now;
			s_now = {};
		}
		if (!s.pad_calls && !s.raw_key_press && !s.raw_key_release && !s.raw_mouse && !s.di_keyboard && !s.di_mouse && !s.di_joystick && !s.key_state_calls)
			return;

		const auto& g = s.pad_last;
		shared::common::log("InputTrace", std::format(
			"pad0: {} reads ({} by the game), result {}, buttons seen 0x{:04X}, LX {}..{}, LY {}..{}, last RX {} RY {} LT {} RT {} | "
			"raw keys: {} presses {} releases, {} dropped | raw mouse: {} events, {} held, motion {} {}, buttons 0x{:X} | "
			"DirectInput reads: keyboard {} mouse {} joystick {} other {}, {} by the game, {} held | "
			"GetKeyboardState: {} calls, {} held, up to {} keys down | menus: F4 {} Remix {}",
			s.pad_calls, s.pad_calls_game, s.pad_result == ERROR_SUCCESS ? "connected" : (s.pad_result == ERROR_DEVICE_NOT_CONNECTED ? "not connected" : std::to_string(s.pad_result)),
			s.pad_buttons_seen, s.pad_lx_min, s.pad_lx_max, s.pad_ly_min, s.pad_ly_max, g.sThumbRX, g.sThumbRY, g.bLeftTrigger, g.bRightTrigger,
			s.raw_key_press, s.raw_key_release, s.raw_key_dropped, s.raw_mouse, s.raw_mouse_held, s.raw_dx, s.raw_dy, s.raw_buttons,
			s.di_keyboard, s.di_mouse, s.di_joystick, s.di_other, s.di_from_game, s.di_held,
			s.key_state_calls, s.key_state_held, s.keys_down_max,
			shared::globals::imgui_menu_open ? "open" : "closed", shared::globals::remix_menu_open ? "open" : "closed"));
	}
}
