#include "std_include.hpp"
#include "dependency_check.hpp"
#include "shared/common/config.hpp"
#include <psapi.h>
#pragma comment(lib, "version.lib")

namespace comp::dependency_check
{
	namespace
	{
		using shared::common::LOG_TYPE;

		void log(const std::string& msg, LOG_TYPE type = LOG_TYPE::LOG_TYPE_DEFAULT)
		{
			shared::common::log("Dependencies", msg, type);
		}

		std::string lower(const std::string& s)
		{
			return shared::utils::str_to_lower(s);
		}

		bool exists(const std::string& path)
		{
			return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
		}

		std::string folder_of(const std::string& path)
		{
			return std::filesystem::path(path).parent_path().string();
		}

		// Windows' own DLL folder for a PE of the given bitness, as seen from this 32-bit process: SysWOW64 for x86
		// (System32 is redirected there), Sysnative for x64 (the real System32).
		std::string system_dir(bool x64)
		{
			char windows[MAX_PATH];
			GetWindowsDirectoryA(windows, MAX_PATH);
			if (!x64)
			{
				char sys[MAX_PATH];
				return GetSystemWow64DirectoryA(sys, MAX_PATH) ? std::string(sys) : std::string(windows) + "\\System32";
			}
			return std::string(windows) + "\\Sysnative";
		}

		struct pe_imports
		{
			bool valid = false;
			bool x64 = false;
			std::vector<std::string> normal, delayed;
		};

		// Reads parts of a file by offset. The Remix runtime DLL is hundreds of MB: it is never read whole.
		class file_reader
		{
		public:
			explicit file_reader(const std::string& path) : f_(path, std::ios::binary) {}
			explicit operator bool() const { return static_cast<bool>(f_); }

			template <typename T>
			bool read(size_t off, T& out)
			{
				f_.clear();
				f_.seekg(static_cast<std::streamoff>(off));
				return static_cast<bool>(f_.read(reinterpret_cast<char*>(&out), sizeof(T)));
			}

			std::string cstr(size_t off)
			{
				char buf[260] = {};
				f_.clear();
				f_.seekg(static_cast<std::streamoff>(off));
				f_.read(buf, sizeof(buf) - 1);
				return std::string(buf, strnlen(buf, sizeof(buf) - 1));
			}

		private:
			std::ifstream f_;
		};

		// Import and delay-import DLL names of an x86 or x64 PE, read from the file.
		pe_imports read_imports(const std::string& path)
		{
			pe_imports out;
			file_reader f(path);
			IMAGE_DOS_HEADER dos{};
			if (!f || !f.read(0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE)
				return out;
			const size_t nt = static_cast<size_t>(dos.e_lfanew);
			DWORD sig = 0;
			IMAGE_FILE_HEADER fh{};
			WORD magic = 0;
			const size_t opt = nt + 4 + sizeof(IMAGE_FILE_HEADER);
			if (!f.read(nt, sig) || sig != IMAGE_NT_SIGNATURE || !f.read(nt + 4, fh) || !f.read(opt, magic))
				return out;
			out.x64 = magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;

			IMAGE_DATA_DIRECTORY import_dir{}, delay_dir{};
			if (out.x64)
			{
				IMAGE_OPTIONAL_HEADER64 oh{};
				if (!f.read(opt, oh)) return out;
				if (oh.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT) import_dir = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
				if (oh.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT) delay_dir = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
			}
			else
			{
				IMAGE_OPTIONAL_HEADER32 oh{};
				if (!f.read(opt, oh)) return out;
				if (oh.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT) import_dir = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
				if (oh.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT) delay_dir = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
			}

			std::vector<IMAGE_SECTION_HEADER> sections(fh.NumberOfSections);
			for (WORD i = 0; i < fh.NumberOfSections; ++i)
				if (!f.read(opt + fh.SizeOfOptionalHeader + i * sizeof(IMAGE_SECTION_HEADER), sections[i]))
					return out;
			const auto rva_to_off = [&](DWORD rva) -> size_t {
				for (const auto& s : sections)
				{
					const DWORD size = std::max(s.Misc.VirtualSize, s.SizeOfRawData);
					if (rva >= s.VirtualAddress && rva < s.VirtualAddress + size)
						return rva - s.VirtualAddress + s.PointerToRawData;
				}
				return SIZE_MAX;
			};

			if (import_dir.VirtualAddress)
			{
				IMAGE_IMPORT_DESCRIPTOR d{};
				for (size_t off = rva_to_off(import_dir.VirtualAddress); off != SIZE_MAX && f.read(off, d) && d.Name; off += sizeof(d))
					if (const size_t name = rva_to_off(d.Name); name != SIZE_MAX)
						out.normal.push_back(f.cstr(name));
			}
			if (delay_dir.VirtualAddress)
			{
				IMAGE_DELAYLOAD_DESCRIPTOR d{};
				for (size_t off = rva_to_off(delay_dir.VirtualAddress); off != SIZE_MAX && f.read(off, d) && d.DllNameRVA; off += sizeof(d))
					if (const size_t name = rva_to_off(d.DllNameRVA); name != SIZE_MAX)
						out.delayed.push_back(f.cstr(name));
			}
			out.valid = true;
			return out;
		}

		// API sets (api-ms-win-*, ext-ms-*) are virtual: Windows maps them, and an ext- set may legitimately be absent.
		bool is_api_set(const std::string& dll)
		{
			const auto l = lower(dll);
			return l.starts_with("api-ms-") || l.starts_with("ext-ms-");
		}

		const char* provider_hint(const std::string& dll, bool x64)
		{
			const auto l = lower(dll);
			if (l.starts_with("msvcp140") || l.starts_with("vcruntime140") || l.starts_with("concrt140") || l.starts_with("vccorlib140"))
				return x64 ? "Microsoft Visual C++ 2015-2022 Redistributable (x64): https://aka.ms/vs/17/release/vc_redist.x64.exe"
				           : "Microsoft Visual C++ 2015-2022 Redistributable (x86): https://aka.ms/vs/17/release/vc_redist.x86.exe";
			if (l.starts_with("d3dx9_") || l == "xinput1_3.dll" || l.starts_with("d3dcompiler_4") || l.starts_with("xaudio2_"))
				return "DirectX End-User Runtime (June 2010)";
			if (l == "nvcuda.dll" || l == "nvapi64.dll" || l == "nvapi.dll" || l.starts_with("nvoglv"))
				return "the NVIDIA graphics driver";
			if (l == "vulkan-1.dll")
				return "a current graphics driver (it installs the Vulkan loader)";
			return nullptr;
		}

		void log_file(const char* what, const std::string& path, bool required = true)
		{
			const bool ok = exists(path);
			log(std::format("{:<26} {}  {}", what, ok ? "found  " : (required ? "MISSING" : "absent "), path),
				ok ? LOG_TYPE::LOG_TYPE_DEFAULT : (required ? LOG_TYPE::LOG_TYPE_ERROR : LOG_TYPE::LOG_TYPE_DEFAULT));
		}

		void log_imports(const char* what, const std::string& path)
		{
			if (!exists(path))
				return;
			const auto missing = unresolved_imports(path);
			if (missing.empty())
			{
				log(std::format("{:<26} every import resolves", what));
				return;
			}
			const bool x64 = read_imports(path).x64;
			for (const auto& m : missing)
			{
				const auto* hint = provider_hint(m.dll, x64);
				log(std::format("{:<26} {} import {} not found{}", what, m.delay_load ? "delay-load" : "required", m.dll,
					hint ? std::format(" (install {})", hint) : ""),
					m.delay_load ? LOG_TYPE::LOG_TYPE_WARN : LOG_TYPE::LOG_TYPE_ERROR);
			}
		}

		bool large_address_aware(const std::string& exe)
		{
			std::ifstream f(exe, std::ios::binary);
			IMAGE_DOS_HEADER dos{};
			if (!f.read(reinterpret_cast<char*>(&dos), sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE)
				return false;
			f.seekg(dos.e_lfanew + 4);
			IMAGE_FILE_HEADER fh{};
			return f.read(reinterpret_cast<char*>(&fh), sizeof(fh)) && (fh.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE);
		}
	}

	std::vector<missing_import> unresolved_imports(const std::string& path)
	{
		std::vector<missing_import> out;
		const auto pe = read_imports(path);
		if (!pe.valid)
			return out;
		const auto here = folder_of(path);
		const auto sys = system_dir(pe.x64);
		const auto check = [&](const std::string& dll, bool delayed) {
			if (dll.empty() || is_api_set(dll) || exists(here + "\\" + dll) || exists(sys + "\\" + dll))
				return;
			out.push_back({ std::filesystem::path(path).filename().string(), dll, delayed });
		};
		for (const auto& d : pe.normal) check(d, false);
		for (const auto& d : pe.delayed) check(d, true);
		return out;
	}

	void explain_load_failure(const std::string& dll_name, unsigned long error)
	{
		const auto path = shared::globals::root_path + "\\" + dll_name;
		if (!exists(path))
		{
			log(std::format("{} is missing from {}: reinstall the mod's DATA folder", dll_name, shared::globals::root_path), LOG_TYPE::LOG_TYPE_ERROR);
			return;
		}
		log(std::format("{} is there but did not load (Windows error {})", dll_name, error), LOG_TYPE::LOG_TYPE_ERROR);
		const auto missing = unresolved_imports(path);
		for (const auto& m : missing)
		{
			const auto* hint = provider_hint(m.dll, false);
			log(std::format("  {} needs {}, which is not installed{}", m.module, m.dll, hint ? std::format(": install {}", hint) : ""),
				LOG_TYPE::LOG_TYPE_ERROR);
		}
		if (missing.empty())
			log("  every library it imports directly is present: one of those may miss a library of its own, or the file is "
				"damaged or blocked (antivirus); reinstall it", LOG_TYPE::LOG_TYPE_ERROR);
	}

	void log_report()
	{
		const auto& root = shared::globals::root_path;
		const auto& cfg = shared::common::config::get();
		log("--- what the mod needs ---", LOG_TYPE::LOG_TYPE_STATUS);

		char exe[MAX_PATH];
		GetModuleFileNameA(nullptr, exe, MAX_PATH);
		log(std::format("{:<26} {}", "game", exe));
		if (large_address_aware(exe))
			log(std::format("{:<26} yes (the game may use 4 GB)", "large address aware"));
		else
			log(std::format("{:<26} NO: run DATA\\rtx_comp\\tools\\enable_large_address_aware.bat once", "large address aware"), LOG_TYPE::LOG_TYPE_ERROR);

		const auto client = root + "\\" + cfg.remix.dll_name;
		log_file("Remix bridge client", client, cfg.remix.enabled);
		log_imports("  bridge client", client);
		log_file("Remix bridge server", root + "\\.trex\\NvRemixBridge.exe", cfg.remix.enabled);
		log_imports("  bridge server", root + "\\.trex\\NvRemixBridge.exe");
		log_file("Remix runtime", root + "\\.trex\\d3d9.dll", cfg.remix.enabled);
		log_imports("  runtime", root + "\\.trex\\d3d9.dll");
		log_file("bridge.conf", root + "\\.trex\\bridge.conf", false);
		log_file("rtx.conf", root + "\\rtx.conf");
		log_file("user.conf", root + "\\user.conf", false);
		log_file("proxy settings", shared::globals::settings_path + "\\remix-comp-proxy.ini");
		for (const char* table : { "mesh_tables", "lights", "sky" })
			log_file(std::format("rtx_comp\\{}", table).c_str(), root + "\\rtx_comp\\" + table);

		const auto mods = root + "\\rtx-remix\\mods";
		if (!exists(mods))
		{
			log(std::format("{:<26} MISSING  {}", "Remix mods", mods), LOG_TYPE::LOG_TYPE_ERROR);
		}
		else
		{
			std::error_code ec;
			for (std::filesystem::directory_iterator it(mods, ec), end; !ec && it != end; it.increment(ec))
			{
				if (!it->is_directory(ec))
					continue;
				const auto& d = it->path();
				const bool usd = std::filesystem::exists(d / "mod.usda", ec) || std::filesystem::exists(d / "mod.usd", ec) ||
					std::filesystem::exists(d / "mod.usdc", ec);
				const bool pkg = std::filesystem::exists(d / "mod.pkg", ec);
				// Wide to UTF-8 by hand: path::string() throws on names the ANSI code page cannot hold.
				const auto file = d.filename();
				const auto& w = file.native();
				std::string name(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr)), '\0');
				WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), name.data(), static_cast<int>(name.size()), nullptr, nullptr);
				log(std::format("{:<26} {}{}{}", "Remix mod", name,
					usd ? "" : "  (no mod.usd: Remix skips it)", pkg ? "  (RTX IO package: needs rtx.io.enabled = True)" : ""),
					usd ? LOG_TYPE::LOG_TYPE_DEFAULT : LOG_TYPE::LOG_TYPE_WARN);
			}
		}
		log("--- end of report ---", LOG_TYPE::LOG_TYPE_STATUS);
	}

	void log_loaded_modules()
	{
		HMODULE mods[1024];
		DWORD needed = 0;
		if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
			return;
		const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), 1024);
		log(std::format("--- {} modules loaded in the game ---", count), LOG_TYPE::LOG_TYPE_STATUS);
		for (DWORD i = 0; i < count; ++i)
		{
			char path[MAX_PATH];
			if (!GetModuleFileNameA(mods[i], path, MAX_PATH))
				continue;
			DWORD handle = 0;
			std::string version;
			if (const DWORD size = GetFileVersionInfoSizeA(path, &handle))
			{
				std::vector<char> block(size);
				VS_FIXEDFILEINFO* fixed = nullptr;
				UINT len = 0;
				if (GetFileVersionInfoA(path, 0, size, block.data()) && VerQueryValueA(block.data(), "\\", reinterpret_cast<void**>(&fixed), &len) && fixed)
					version = std::format("{}.{}.{}.{}", HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
						HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
			}
			log(std::format("  0x{:08X} {:<16} {}", reinterpret_cast<uintptr_t>(mods[i]), version, path));
		}
	}
}
