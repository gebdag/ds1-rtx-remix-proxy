// Borderless window ([Window] Borderless=1).
//
// The game asks D3D9 for exclusive fullscreen or for a framed window. With the option on, both become a window without a
// frame that covers its monitor: the fullscreen request is turned into a windowed one at CreateDevice and Reset, and the
// window's frame styles are taken off and the window moved over the monitor. The back buffer keeps the game's resolution
// and is scaled to the window when presented.
//
// The window is checked again at every Present: in windowed mode the game sizes its window to the chosen resolution, and
// a change the game makes is undone once more. If the game keeps putting its frame back, the option gives up after
// kMaxApplies and says so in the log.
#include "std_include.hpp"
#include "borderless.hpp"
#include "shared/common/config.hpp"

namespace comp::game::borderless
{
	namespace
	{
		constexpr LONG kFrameStyles = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
		constexpr LONG kFrameExStyles = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;
		constexpr int kMaxApplies = 20;

		std::atomic<bool> s_window_proc_ready = false;
		std::atomic<bool> s_pending = false;
		std::atomic<int> s_applies = 0;
		std::atomic<bool> s_gave_up = false;

		bool monitor_rect(HWND window, RECT& rect)
		{
			MONITORINFO monitor { sizeof(MONITORINFO) };
			if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
				return false;
			rect = monitor.rcMonitor;
			return true;
		}

		bool needs_apply(HWND window)
		{
			if (!IsWindow(window) || IsIconic(window))
				return false;
			const LONG style = GetWindowLongW(window, GWL_STYLE);
			const LONG ex_style = GetWindowLongW(window, GWL_EXSTYLE);
			if ((style & kFrameStyles) || (ex_style & kFrameExStyles) || !(style & WS_POPUP))
				return true;
			RECT window_rect {}, target {};
			if (!GetWindowRect(window, &window_rect) || !monitor_rect(window, target))
				return false;
			return !EqualRect(&window_rect, &target);
		}
	}

	bool enabled()
	{
		return shared::common::config::get().window.borderless && !shared::globals::lookdev_host;
	}

	void adjust_present_params(D3DPRESENT_PARAMETERS* pp, const char* where)
	{
		if (!pp || !enabled() || pp->Windowed)
			return;
		pp->Windowed = TRUE;
		// Exclusive fullscreen only: a windowed device must leave the refresh rate at 0.
		pp->FullScreen_RefreshRateInHz = 0;
		shared::common::log("Window", std::format("{}: fullscreen requested, borderless window used instead", where),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS);
	}

	void set_window_proc_ready()
	{
		s_window_proc_ready = true;
	}

	void check(HWND window)
	{
		if (!window || !enabled() || s_gave_up || s_pending || !needs_apply(window))
			return;
		if (GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId())
		{
			apply(window);
			return;
		}
		// Not the window's thread: only once the proxy's window procedure is there to answer. Until then the next Present
		// checks again.
		if (!s_window_proc_ready)
			return;
		s_pending = true;
		if (!PostMessageW(window, kApplyMessage, 0, 0))
			s_pending = false;
	}

	void apply(HWND window)
	{
		s_pending = false;
		if (!enabled() || s_gave_up || !needs_apply(window))
			return;
		if (++s_applies > kMaxApplies)
		{
			s_gave_up = true;
			shared::common::log("Window", "Borderless: the game keeps restoring its window frame; the window is left as the game sets it",
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		RECT target {};
		if (!monitor_rect(window, target))
			return;
		const LONG style = GetWindowLongW(window, GWL_STYLE);
		const LONG ex_style = GetWindowLongW(window, GWL_EXSTYLE);
		SetWindowLongW(window, GWL_STYLE, (style & ~kFrameStyles) | WS_POPUP);
		SetWindowLongW(window, GWL_EXSTYLE, ex_style & ~kFrameExStyles);
		SetWindowPos(window, nullptr, target.left, target.top, target.right - target.left, target.bottom - target.top,
			SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
		shared::common::log("Window", std::format("Borderless: window covers the monitor ({}, {})-({}, {})",
			target.left, target.top, target.right, target.bottom), shared::common::LOG_TYPE::LOG_TYPE_STATUS);
	}
}
