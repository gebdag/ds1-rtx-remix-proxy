#pragma once

namespace comp
{
	// Always-on check for other DLL mods: DLLs in the game folder under the names Windows loads in place of its own
	// (dinput8.dll, dxgi.dll, version.dll, ...), ASI plugins, and DLLs injected into the game from elsewhere. It never
	// stops the game and shows nothing on screen: what it finds goes to the log at startup and again, with the crash
	// report, after a crash. A mod known to break this one is named as incompatible, with the files to remove and any
	// DLL it chain-loads.
	namespace mod_check
	{
		struct finding
		{
			std::string path;       // the DLL (or plugin) on disk
			std::string name;       // what it is, when recognised ("ReShade"), else its version info
			std::string advice;     // one line for the player
			bool incompatible;      // known to break this mod
		};

		// Scans the game folder. Call once, off the loader lock (it reads version resources).
		void scan_folder();

		// Scans the modules loaded in the game for DLLs from outside Windows, Steam and the game folder. Call once the
		// game window exists, when overlays and injectors have attached.
		void scan_loaded();

		// Everything found so far, one line each; empty when nothing was found. Safe to call from the crash handler.
		std::string summary();
	}
}
