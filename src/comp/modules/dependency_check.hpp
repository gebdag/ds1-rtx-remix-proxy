#pragma once

namespace comp
{
	// What the mod needs on disk and which of it is missing. Reads PE import tables straight from the files (no
	// LoadLibrary), so it is safe to call from DllMain and reports the 64-bit runtime's needs from this 32-bit process.
	namespace dependency_check
	{
		// The import of a DLL that is found neither beside it nor in Windows' system folder for its bitness.
		struct missing_import
		{
			std::string module;   // the file that imports it
			std::string dll;      // the import that does not resolve
			bool delay_load;      // only needed when first called
		};

		// Unresolved imports of `path`, an x86 or x64 PE. Empty when the file is missing (check that first) or fine.
		std::vector<missing_import> unresolved_imports(const std::string& path);

		// Logs why the Remix bridge could not be loaded: the file, the Windows error and every import that does not
		// resolve, with the redistributable that provides it where known. Always on: it only runs on that failure.
		void explain_load_failure(const std::string& dll_name, unsigned long error);

		// [Troubleshooting] Verbose: logs every file and library the mod needs and whether it is there (game exe and its
		// large-address flag, bridge client and server, runtime and their imports, configs, mods).
		void log_report();

		// [Troubleshooting] Verbose: logs every module loaded in the game with its path and version, once the game
		// window exists (overlays and injectors have attached by then).
		void log_loaded_modules();
	}
}
