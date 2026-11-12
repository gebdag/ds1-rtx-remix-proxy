#include "std_include.hpp"
#include <psapi.h>

#include "comp.hpp"
#include "d3d9_proxy.hpp"
#include "game/frustum_culling.hpp"
#include "modules/crash_context.hpp"
#include "modules/dependency_check.hpp"
#include "modules/mod_check.hpp"
#include "shared/common/flags.hpp"
#include "shared/common/config.hpp"

namespace comp
{
	std::unordered_set<HWND> wnd_class_list;

	#define WINDOW_CLASS_NAME "DARK SOULS"

	BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM lParam)
	{
		DWORD window_pid, target_pid = static_cast<DWORD>(lParam);
		GetWindowThreadProcessId(hwnd, &window_pid);

		if (window_pid == target_pid && IsWindowVisible(hwnd))
		{
			char class_name[256];
			GetClassNameA(hwnd, class_name, sizeof(class_name));

			if (!wnd_class_list.contains(hwnd))
			{
				char debug_msg[256];
				wsprintfA(debug_msg, "> HWND: %p, PID: %u, Class: %s, Visible: %d \n", hwnd, window_pid, class_name, IsWindowVisible(hwnd));
				shared::common::log("Main", debug_msg, shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
				wnd_class_list.insert(hwnd);
			}

			if (std::string_view(class_name).contains(WINDOW_CLASS_NAME))
			{
				shared::globals::main_window = hwnd;
				return FALSE;
			}
		}

		return TRUE;
	}

	DWORD WINAPI find_game_window([[maybe_unused]] LPVOID lpParam)
	{
		std::uint32_t T = 0;

		shared::common::log("Main", "Waiting for window with classname containing '" WINDOW_CLASS_NAME "' ...", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		{
			while (!shared::globals::main_window)
			{
				EnumWindows(enum_windows_proc, static_cast<LPARAM>(GetCurrentProcessId()));
				if (!shared::globals::main_window) {
					Sleep(1u); T += 1u;
				}

				if (T >= 30000)
				{
					Beep(300, 100); Sleep(100); Beep(200, 100);
					shared::common::log("Main", "Could not find '" WINDOW_CLASS_NAME "' Window. Not loading RTX Compatibility Mod.", shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
					return TRUE;
				}
			}
		}

		if (!shared::common::flags::has_flag("nobeep")) {
			Beep(523, 100);
		}

		// Overlays and injectors have attached by the time the window exists.
		if (!shared::globals::lookdev_host)
			mod_check::scan_loaded();
		if (shared::common::config::get().troubleshooting.verbose)
			dependency_check::log_loaded_modules();

		// Post-load DLLs (after window is found, game is running)
		d3d9_proxy::load_postload_dlls();

		comp::main();
		return 0;
	}

	// Off the loader lock: the mod check reads version resources (GetFileVersionInfo), which is not safe in DllMain,
	// and neither scan may delay the game's start.
	DWORD WINAPI startup_checks([[maybe_unused]] LPVOID lpParam)
	{
		mod_check::scan_folder();
		if (shared::common::config::get().troubleshooting.verbose)
			dependency_check::log_report();
		return 0;
	}

	void print_banner()
	{
		shared::common::set_console_color_blue(true);
		std::cout << "Launching RTX Remix Comp [" << COMP_MOD_VERSION_MAJOR << "." << COMP_MOD_VERSION_MINOR << "." << COMP_MOD_VERSION_PATCH << "]\n";
		std::cout << "> Compiled On : " + std::string(__DATE__) + " " + std::string(__TIME__) + "\n";
		std::cout << "> Based on xoxor4d/remix-comp-base\n";
		std::cout << "> Adapted by gabdeg for Dark Souls: Prepare to Die Edition\n";
		std::cout << "> Running as d3d9.dll proxy\n\n";
		shared::common::set_console_color_default();
	}
}

BOOL APIENTRY DllMain(HMODULE hmodule, const DWORD ul_reason_for_call, LPVOID)
{
	if (ul_reason_for_call == DLL_PROCESS_ATTACH)
	{
		comp::crash_context::install();

		shared::common::console();
		shared::globals::setup_dll_module(hmodule);
		shared::globals::setup_exe_module();
		shared::globals::setup_homepath();
		comp::print_banner();

		// The INIs live in rtx_comp\settings, not in the game's root folder. Files an earlier build left in the
		// old places move there; one already there wins, and a leftover stays where it is, unread.
		shared::globals::settings_path = shared::globals::root_path + "\\rtx_comp\\settings";
		CreateDirectoryA((shared::globals::root_path + "\\rtx_comp").c_str(), nullptr);
		CreateDirectoryA(shared::globals::settings_path.c_str(), nullptr);
		const std::pair<const char*, const char*> old_places[] = {
			{ "", "remix-comp-proxy.ini" },
			{ "", "remix-comp-proxy.lookdev.ini" },
			{ "", "remix-comp-proxy.host.ini" },
			{ "\\rtx_comp", "area_overrides.ini" },
		};
		for (const auto& [folder, name] : old_places)
			MoveFileA((shared::globals::root_path + folder + "\\" + name).c_str(), (shared::globals::settings_path + "\\" + name).c_str());

		// -lookdev (lookdev\lookdev.ps1) selects the lookdev INI: vanilla rendering plus frame capture.
		// The lookdev host (-lookdev-host) renders like the game, so it reads the normal INI.
		shared::globals::lookdev_host = shared::common::flags::has_flag("lookdev-host");
		const bool lookdev_game = shared::common::flags::has_flag("lookdev");
		const char* ini_name = lookdev_game
			? "\\remix-comp-proxy.lookdev.ini" : "\\remix-comp-proxy.ini";
		// Both lookdev modes are tools (captures, the live options file): dev mode on, whatever their INI says.
		if (lookdev_game || shared::globals::lookdev_host)
			shared::common::config::get().force_dev_mode();
		shared::common::config::get().load(shared::globals::settings_path + ini_name);
		if (shared::globals::lookdev_host)
			shared::common::config::get().set_overlay(shared::globals::settings_path + "\\remix-comp-proxy.host.ini");

		if (!shared::globals::lookdev_host)
		{
			if (const auto t = CreateThread(nullptr, 0, comp::startup_checks, nullptr, 0, nullptr); t)
				CloseHandle(t);
		}

		// Apply fixed-address game patches as early as possible.
		// Effect disabling lives in game/render_toggles.cpp now — schedule-level
		// flag writes asserted per frame from Present, no init-time code patches.
		// None of it exists in the lookdev host.
		if (!shared::globals::lookdev_host)
			comp::game::frustum_culling::init();

		// Pre-load DLLs (before the d3d9 chain is established)
		d3d9_proxy::load_preload_dlls();

		// Load the real d3d9 chain (Remix bridge or system d3d9.dll)
		if (!d3d9_proxy::init())
			return TRUE;

		if (const auto MH_INIT_STATUS = MH_Initialize(); MH_INIT_STATUS != MH_STATUS::MH_OK)
		{
			shared::common::log("Main", std::format("MinHook failed to initialize with code: {:d}", static_cast<int>(MH_INIT_STATUS)), shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return TRUE;
		}

		// Setup memory addresses (eg. patterns): the FPS unlock, intro skip, camera
		// and cheat patches. Pattern scans in the host would search the host exe.
		if (!shared::globals::lookdev_host)
		{
			comp::game::init_game_addresses();
			comp::game::frustum_culling::install_hooks();
		}
		else shared::common::log("Main", "Lookdev host: game patches and hooks skipped", shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);

		// Find game window thread (registers modules once window is found)
		if (const auto t = CreateThread(nullptr, 0, comp::find_game_window, nullptr, 0, nullptr); t) {
			CloseHandle(t);
		}
	}

	return TRUE;
}
