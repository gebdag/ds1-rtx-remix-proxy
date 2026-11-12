#include "std_include.hpp"
#include "mod_check.hpp"
#include "shared/common/config.hpp"
#include <psapi.h>
#pragma comment(lib, "version.lib")

namespace comp::mod_check
{
	namespace
	{
		using shared::common::LOG_TYPE;

		std::mutex g_mutex;
		std::vector<finding> g_findings;
		std::set<std::string> g_seen;  // lower-case paths already reported

		// Names Windows looks for in the game folder before its own copy: how DLL mods get themselves loaded.
		constexpr const char* PROXY_NAMES[] = {
			"dinput8.dll", "dxgi.dll", "d3d11.dll", "d3d10.dll", "d3d8.dll", "ddraw.dll", "opengl32.dll", "version.dll",
			"winmm.dll", "winhttp.dll", "wininet.dll", "dsound.dll", "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll",
			"msacm32.dll", "msvfw32.dll", "dbghelp.dll",
		};

		// Text found inside a known mod's DLL; a settings file name it opens identifies it across its versions.
		struct signature
		{
			const char* marker;
			const char* name;
			bool incompatible;
		};
		constexpr signature SIGNATURES[] = {
			{ "DSfixKeys.ini", "DSfix", true },
			{ "DSMfix", "DSMfix (mouse fix)", false },
			{ "DarkSoulsInputCustomizer", "Dark Souls Input Customizer", false },
			{ "Ultimate ASI Loader", "Ultimate ASI Loader", false },
			{ "ReShade", "ReShade", false },
			{ "SpecialK", "Special K", false },
		};

		std::string lower(const std::string& s)
		{
			return shared::utils::str_to_lower(s);
		}

		// A path as the ANSI string the A-functions take; characters the code page lacks become '?'. Never throws,
		// unlike path::string(): a file name the code page cannot hold must not stop the game.
		std::string ansi(const std::filesystem::path& path)
		{
			const auto& w = path.native();
			const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
			std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
			WideCharToMultiByte(CP_ACP, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
			return out;
		}

		std::string file_name(const std::string& path)
		{
			return std::filesystem::path(path).filename().string();
		}

		// "Product - Description (Company)" from the version resource, empty when it has none.
		std::string version_info(const std::string& path)
		{
			DWORD handle = 0;
			const DWORD size = GetFileVersionInfoSizeA(path.c_str(), &handle);
			if (!size)
				return {};
			std::vector<char> block(size);
			if (!GetFileVersionInfoA(path.c_str(), 0, size, block.data()))
				return {};
			struct lang_cp { WORD lang, cp; }* tr = nullptr;
			UINT len = 0;
			if (!VerQueryValueA(block.data(), "\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &len) || len < sizeof(lang_cp))
				return {};
			const auto field = [&](const char* key) -> std::string {
				char* value = nullptr;
				UINT n = 0;
				const auto q = std::format("\\StringFileInfo\\{:04x}{:04x}\\{}", tr->lang, tr->cp, key);
				return VerQueryValueA(block.data(), q.c_str(), reinterpret_cast<void**>(&value), &n) && n > 1 ? std::string(value) : std::string();
			};
			std::string product = field("ProductName"), desc = field("FileDescription"), company = field("CompanyName");
			std::string out = product.empty() ? desc : (desc.empty() || desc == product ? product : product + " - " + desc);
			if (!company.empty())
				out += (out.empty() ? "" : " ") + std::string("(") + company + ")";
			return out;
		}

		const signature* identify(const std::string& path)
		{
			std::ifstream f(path, std::ios::binary);
			if (!f)
				return nullptr;
			std::error_code ec;
			const auto size = std::filesystem::file_size(path, ec);
			std::string data(ec ? 0 : static_cast<size_t>(std::min<uintmax_t>(size, 32u << 20)), '\0');
			f.read(data.data(), static_cast<std::streamsize>(data.size()));
			for (const auto& s : SIGNATURES)
				if (data.find(s.marker) != std::string::npos)
					return &s;
			return nullptr;
		}

		// The incompatible mod's dinput8dllWrapper setting: another dinput8 wrapper it loads itself ("none" when unused).
		std::string chained_wrapper(const std::string& folder)
		{
			std::ifstream ini(folder + "\\DSfix.ini");
			for (std::string line; std::getline(ini, line); )
			{
				if (!line.starts_with("dinput8dllWrapper"))
					continue;
				std::string v = line.substr(sizeof("dinput8dllWrapper") - 1);
				v.erase(0, v.find_first_not_of(" \t"));
				v.erase(v.find_last_not_of(" \t\r") + 1);
				return lower(v) == "none" ? std::string() : v;
			}
			return {};
		}

		void add(finding f)
		{
			{
				std::lock_guard lock(g_mutex);
				if (!g_seen.insert(lower(f.path)).second)
					return;
				g_findings.push_back(f);
			}
			shared::common::log("ModCheck", std::format("{}: {}. {}", f.path, f.name, f.advice),
				f.incompatible ? LOG_TYPE::LOG_TYPE_ERROR : LOG_TYPE::LOG_TYPE_WARN, f.incompatible);
		}

		void add_file(const std::string& path, const char* loaded_by = nullptr)
		{
			const auto* sig = identify(path);
			const auto info = version_info(path);
			std::string name = sig ? sig->name : (info.empty() ? "unknown DLL mod" : info);
			if (loaded_by)
				name += std::format(", loaded by {}", loaded_by);
			if (sig && sig->incompatible)
			{
				const auto folder = std::filesystem::path(path).parent_path().string();
				add({ path, name,
					std::format("INCOMPATIBLE with the RTX Remix mod: remove {}, DSfix.ini, DSfixKeys.ini and the dsfix folder", file_name(path)),
					true });
				// One level only: a chain pointing back at the same mod must not loop.
				if (const auto chained = chained_wrapper(folder); !chained.empty() && !loaded_by)
				{
					const auto chained_path = folder + "\\" + chained;
					if (lower(chained_path) != lower(path) && GetFileAttributesA(chained_path.c_str()) != INVALID_FILE_ATTRIBUTES)
						add_file(chained_path, "DSfix");
				}
				return;
			}
			add({ path, name, "DLL mod detected: remove it in case there are issues", false });
		}

		// Steam's folders (lower case, trailing backslash): its install from the registry, and the library the game
		// sits in (the part of the game's path before "\steamapps\"), which differ when the library is on another drive.
		std::vector<std::string> steam_folders()
		{
			std::vector<std::string> out;
			char buf[MAX_PATH] = {};
			DWORD size = sizeof(buf);
			if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
			{
				auto path = lower(buf);
				std::ranges::replace(path, '/', '\\');
				out.push_back(path + "\\");
			}
			const auto game = lower(shared::globals::root_path);
			if (const auto at = game.find("\\steamapps\\"); at != std::string::npos)
				out.push_back(game.substr(0, at + 1));
			return out;
		}

		// The proxy's own [Chain] PreLoad/PostLoad DLLs (';'-separated names or paths) are the user's choice, not mods.
		bool listed_in_chain(const std::string& name)
		{
			const auto& chain = shared::common::config::get().chain;
			const auto n = lower(name);
			for (const auto* list : { &chain.preload, &chain.postload })
			{
				std::stringstream ss(*list);
				for (std::string entry; std::getline(ss, entry, ';'); )
				{
					const auto slash = entry.find_last_of("\\/");
					auto file = entry.substr(slash == std::string::npos ? 0 : slash + 1);
					file.erase(0, file.find_first_not_of(" \t"));
					file.erase(file.find_last_not_of(" \t") + 1);
					if (!file.empty() && lower(file) == n)
						return true;
				}
			}
			return false;
		}
	}

	void scan_folder()
	{
		const auto& root = shared::globals::root_path;
		std::error_code ec;
		for (const auto& folder : { root, root + "\\scripts", root + "\\plugins" })
		{
			for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
			{
				if (!it->is_regular_file(ec))
					continue;
				const auto name = ansi(it->path().filename());
				const auto l = lower(name);
				const bool asi = l.ends_with(".asi");
				const bool proxy = folder == root && std::ranges::any_of(PROXY_NAMES, [&](const char* p) { return l == p; });
				if ((asi || proxy) && !listed_in_chain(name))
					add_file(ansi(it->path()));
			}
		}
	}

	void scan_loaded()
	{
		char windows[MAX_PATH];
		GetWindowsDirectoryA(windows, MAX_PATH);
		const auto win = lower(windows) + "\\";
		const auto game = lower(shared::globals::root_path) + "\\";
		const auto steam = steam_folders();

		HMODULE mods[1024];
		DWORD needed = 0;
		if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
			return;
		for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 1024); ++i)
		{
			char path[MAX_PATH];
			if (!GetModuleFileNameA(mods[i], path, MAX_PATH))
				continue;
			const auto l = lower(path);
			// Windows (system and driver folders), the game folder (its own DLLs; mods there are scan_folder's) and
			// Steam (client and overlay).
			if (l.starts_with(win) || l.starts_with(game) || std::ranges::any_of(steam, [&](const std::string& s) { return l.starts_with(s); }))
				continue;
			const auto info = version_info(path);
			if (lower(info).find("(microsoft") != std::string::npos)
				continue;  // Windows components installed outside C:\Windows (ink, text services)
			add({ path, info.empty() ? "DLL injected into the game" : info,
				"injected by another program (overlay or injector): disable it in case there are issues", false });
		}
	}

	std::string summary()
	{
		std::unique_lock lock(g_mutex, std::try_to_lock);  // never wait in the crash handler
		if (!lock.owns_lock())
			return {};
		std::string out;
		for (const auto& f : g_findings)
			out += std::format("- {}: {}. {}\r\n", f.path, f.name, f.advice);
		return out;
	}
}
