// Dark Souls Input Customizer (a dinput8.dll mod, "PRIC") under the proxy.
//
// Cursor and camera: the mod decides cursor capture inside its own detour of user32!ClipCursor,
// which ignores the caller's rect. That detour treats the game as being in the background unless
// GetActiveWindow() on the calling thread is the foreground window and its client area is at least
// 800x600, and the mod starts out in that state. While "in the background" it drops all mouse
// camera input and leaves the cursor shown. DS1 never calls ClipCursor (its DirectInput mouse is
// most likely non-exclusive, so DirectInput does not either) and nothing else under the proxy and
// Remix does, so the detour never ran: the camera stayed frozen and the cursor visible. A timer on
// the window thread now calls ClipCursor. The mod then hides and confines the cursor in game,
// releases it in the game's menus or when the window loses focus, and applies pending profile
// loads (also done in that detour). It stays hidden and confined while the F4 menu is open; ImGui
// draws its own cursor there.
//
// The mod confines the cursor to the game's client area, which the system clamps to the screen. A
// window as large as the screen reaches under the taskbar, so the hidden cursor could slide onto
// the taskbar and the camera spun. The pump narrows the mod's clip to the monitor's work area.
//
// F4 menu: the mod reads all of its input (camera mouse motion, bound keys and buttons) through
// its own import of GetRawInputData while messages are retrieved, before any window procedure
// runs, so the menu's WndProc block does not reach it. Its import is redirected so that presses
// and motion are dropped while the menu holds input; releases still pass, so nothing sticks.
//
// The Remix UI (Alt+X) is not covered: the proxy cannot see it, and the bridge's own raw input
// filter drops button releases too and leaves keys alone for DirectInput games.
#include "std_include.hpp"
#include "input_customizer.hpp"

namespace comp::game::input_customizer
{
	// Arbitrary, unlikely to collide with the game's own timers on its window.
	static constexpr UINT_PTR CLIP_PUMP_TIMER_ID = 0x50524943;
	static constexpr UINT CLIP_PUMP_INTERVAL_MS = 16;

	static HMODULE mod_ = nullptr;
	static bool pump_started_ = false;
	static bool pump_live_ = false;

	using get_raw_input_data_t = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
	static get_raw_input_data_t orig_get_raw_input_data_ = nullptr;

	static bool menu_holds_input()
	{
		return (shared::globals::imgui_menu_open && !shared::globals::imgui_allow_input_bypass)
			|| shared::globals::input_frozen || shared::globals::remix_menu_open;
	}

	// Only the mod's own import comes here; DirectInput and the bridge keep the real data.
	static UINT WINAPI hooked_get_raw_input_data(HRAWINPUT raw, UINT command, LPVOID data, PUINT size, UINT header_size)
	{
		const UINT res = orig_get_raw_input_data_(raw, command, data, size, header_size);
		if (command != RID_INPUT || !data || res == static_cast<UINT>(-1) || res < sizeof(RAWINPUTHEADER) || !menu_holds_input())
			return res;

		auto* input = static_cast<RAWINPUT*>(data);
		if (input->header.dwType == RIM_TYPEMOUSE && res >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE))
		{
			constexpr USHORT releases = RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_UP
				| RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;

			// The mod takes lLastX/Y as deltas even in absolute mode, so zero both kinds.
			input->data.mouse.lLastX = 0;
			input->data.mouse.lLastY = 0;
			input->data.mouse.usButtonFlags &= releases;
			input->data.mouse.usButtonData = 0;
		}
		else if (input->header.dwType == RIM_TYPEKEYBOARD && res >= offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD)
			&& !(input->data.keyboard.Flags & RI_KEY_BREAK))
		{
			return static_cast<UINT>(-1); // the mod skips reads that fail
		}

		return res;
	}

	// ClipCursor(nullptr) for real would release any clip, other programs' included, so the pump
	// only calls it while the mod's detour (a jmp into its image at the top of ClipCursor) is in place.
	// The mod hooks some time after the game window exists, so this is checked on every tick.
	static bool mod_hooks_clip_cursor()
	{
		static const auto* code = reinterpret_cast<const BYTE*>(GetProcAddress(GetModuleHandleA("user32.dll"), "ClipCursor"));
		if (!code || code[0] != 0xE9)
			return false;

		const auto target = reinterpret_cast<std::uintptr_t>(code) + 5 + *reinterpret_cast<const std::int32_t*>(code + 1);
		const auto base = reinterpret_cast<std::uintptr_t>(mod_);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(mod_)->e_lfanew);
		return target >= base && target < base + nt->OptionalHeader.SizeOfImage;
	}

	// Keeps the mod's clip off the taskbar and anything else outside the monitor's work area.
	// user32!ClipCursor is only a jump to win32u!NtUserClipCursor; calling that directly sets the
	// clip without going through the mod's detour, which would replace the rect with its own.
	static void keep_clip_in_work_area(HWND window)
	{
		using clip_cursor_t = BOOL(WINAPI*)(const RECT*);
		static const auto real_clip_cursor = reinterpret_cast<clip_cursor_t>(
			GetProcAddress(GetModuleHandleA("win32u.dll"), "NtUserClipCursor"));
		if (!real_clip_cursor || GetForegroundWindow() != window)
			return;

		RECT clip;
		const RECT screen = { GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
			GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
			GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
		if (!GetClipCursor(&clip) || EqualRect(&clip, &screen))
			return; // the mod has the cursor released

		MONITORINFO monitor = { sizeof(MONITORINFO) };
		if (!GetMonitorInfoA(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
			return;

		RECT inside;
		if (IntersectRect(&inside, &clip, &monitor.rcWork) && !EqualRect(&inside, &clip))
			real_clip_cursor(&inside);
	}

	static void CALLBACK clip_pump(HWND window, UINT, UINT_PTR, DWORD)
	{
		if (!mod_hooks_clip_cursor())
			return;

		if (!pump_live_)
		{
			pump_live_ = true;
			shared::common::log("InputCustomizer", "The mod's ClipCursor hook is in; cursor pump live.");
		}
		ClipCursor(nullptr);
		keep_clip_in_work_area(window);
	}

	void init()
	{
		// The mod ships as dinput8.dll next to the game, with its config beside it.
		char exe_path[MAX_PATH] = {};
		std::string game_dir(exe_path, GetModuleFileNameA(nullptr, exe_path, MAX_PATH));
		const auto slash = game_dir.find_last_of("\\/");
		if (slash == std::string::npos)
			return;
		game_dir.resize(slash);

		const HMODULE mod = GetModuleHandleA((game_dir + "\\dinput8.dll").c_str());
		if (!mod || GetFileAttributesA((game_dir + "\\DarkSoulsInputCustomizer.ini").c_str()) == INVALID_FILE_ATTRIBUTES)
			return;

		mod_ = mod;

		if (const DWORD thunk_addr = shared::utils::mem::find_import_addr(mod_, "user32.dll", "GetRawInputData"); thunk_addr)
		{
			auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(thunk_addr);
			orig_get_raw_input_data_ = reinterpret_cast<get_raw_input_data_t>(thunk->u1.Function);

			DWORD old_protect;
			VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), PAGE_READWRITE, &old_protect);
			thunk->u1.Function = reinterpret_cast<DWORD>(&hooked_get_raw_input_data);
			VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), old_protect, &old_protect);
		}

		shared::common::log("InputCustomizer", std::format("Dark Souls Input Customizer found; F4 menu input gate {}.",
			orig_get_raw_input_data_ ? "on" : "NOT installed (GetRawInputData import not found)"));
	}

	void on_window_message(HWND window)
	{
		if (!mod_ || pump_started_)
			return;

		pump_started_ = true;

		// SetTimer needs the window's own thread, which is where window procedures run. A timer with a
		// callback is dispatched straight to it, never to a window procedure (the bridge would forward
		// WM_TIMER to the server; it only sees retrieved messages with client.hookMessagePump on).
		if (!SetTimer(window, CLIP_PUMP_TIMER_ID, CLIP_PUMP_INTERVAL_MS, clip_pump))
			shared::common::log("InputCustomizer", std::format("SetTimer failed ({}); the mod will not capture the cursor.", GetLastError()),
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
	}
}
