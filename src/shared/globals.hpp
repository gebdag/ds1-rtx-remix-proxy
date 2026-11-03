#pragma once

// Written once at init, read from the single D3D9 thread — no locks needed.
namespace shared::globals
{
	extern D3DXMATRIX IDENTITY;
	
	extern std::string root_path;
	extern HWND main_window;

#define EXE_BASE shared::globals::exe_module_addr

	extern HMODULE exe_hmodule;
	extern DWORD exe_module_addr; // x64: use uintptr_t
	extern DWORD exe_size;
	extern void setup_exe_module();

	extern HMODULE dll_hmodule;
	extern DWORD dll_module_addr; // x64: use uintptr_t
	extern void setup_dll_module(const HMODULE mod);

	extern void setup_homepath();

	extern IDirect3DDevice9* d3d_device;
	// The device the wrapper above forwards to (Remix bridge or system d3d9).
	extern IDirect3DDevice9* d3d_device_real;

	// Loaded by the lookdev host (-lookdev-host), not by DARKSOULS.exe: the game's
	// memory and code do not exist in this process, so nothing may read, patch or
	// hook it. Game state the modules need arrives through game::host_feed.
	extern bool lookdev_host;
	// <game>\rtx_comp\settings: the INIs a player edits. Set before the config loads.
	extern std::string settings_path;

	// Lookdev mode: the game sees no keyboard, mouse or gamepad input while set,
	// so the camera stays where it was lined up (toggled by [Lookdev] FreezeInputKey).
	extern bool input_frozen;
	extern IDirect3D9* d3d9_interface;

	// Module handle of the real d3d9 chain (Remix bridge or system d3d9.dll).
	// Set by d3d9_proxy::init(), consumed by remix_api for API lookups.
	extern HMODULE d3d9_chain_module;

	extern bool imgui_is_rendering;
	extern bool imgui_menu_open;
	// A tab of the F4 menu to bring to the front on the next frame (live option proxy.Menu.Tab), else empty.
	extern std::string imgui_select_tab;
	// Open every section of the Lights tab on the next frame (live option proxy.Menu.Tab=Lights+).
	extern bool imgui_open_light_sections;
	extern bool imgui_allow_input_bypass;
	extern bool imgui_wants_text_input;
	extern uint32_t imgui_allow_input_bypass_timeout;
}
