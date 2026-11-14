#pragma once
#include "shared/globals.hpp"
#include <filesystem>
#include <mutex>

namespace shared::common
{
	inline bool g_external_console_created = false;
    inline void console()
    {
        if (!g_external_console_created)
        {
			g_external_console_created = true;
            
            setvbuf(stdout, nullptr, _IONBF, 0);
            if (AllocConsole())
            {
                FILE* file = nullptr;
                freopen_s(&file, "CONIN$", "r", stdin);
                freopen_s(&file, "CONOUT$", "w", stdout);
                freopen_s(&file, "CONOUT$", "w", stderr);
                SetConsoleTitleA("RTX-Comp Debug Console");
            }

			HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
			CONSOLE_SCREEN_BUFFER_INFO info;
			GetConsoleScreenBufferInfo(hOut, &info);

			// Desired sizes
			SHORT newWidth = 500;
			SHORT newHeight = std::max((SHORT)(info.srWindow.Bottom + 1), (SHORT)300);

			// Step 1: Shrink window temporarily to avoid SetConsoleScreenBufferSize failure
			SMALL_RECT rect = { 0, 0, 1, 1 };
			SetConsoleWindowInfo(hOut, TRUE, &rect);

			// Step 2: Apply new buffer size
			COORD newSize = { newWidth, newHeight };
			SetConsoleScreenBufferSize(hOut, newSize);

			// Step 3: Resize visible window (optional)
			rect = { 0, 0, (SHORT)(120 - 1), (SHORT)(40 - 1) };
			SetConsoleWindowInfo(hOut, TRUE, &rect);
        }
    }

	inline void set_console_color_red(bool highlight = false)
	{
		if (g_external_console_created) 
		{
			WORD color = FOREGROUND_RED;
			if (highlight) {
				color |= FOREGROUND_INTENSITY;
			}
			SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
		}
	}

	inline void set_console_color_green(bool highlight = false)
	{
		if (g_external_console_created) 
		{
			WORD color = FOREGROUND_GREEN;
			if (highlight) {
				color |= FOREGROUND_INTENSITY;
			}
			SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
		}
	}

	inline void set_console_color_blue(bool highlight = false)
	{
		if (g_external_console_created) 
		{
			WORD color = FOREGROUND_BLUE;
			if (highlight) {
				color |= FOREGROUND_INTENSITY;
			}
			SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
		}
	}

	inline void set_console_color_yellow(bool highlight = false)
	{
		if (g_external_console_created)
		{
			WORD color = FOREGROUND_RED | FOREGROUND_GREEN;
			if (highlight) {
				color |= FOREGROUND_INTENSITY;
			}
			SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
		}
	}

	inline void set_console_color_default(bool highlight = false)
	{
		if (g_external_console_created)
		{
			WORD color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
			if (highlight) {
				color |= FOREGROUND_INTENSITY;
			}
			SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
		}
	}

	enum class LOG_TYPE
	{
		LOG_TYPE_DEFAULT,
		LOG_TYPE_STATUS,
		LOG_TYPE_GREEN,
		LOG_TYPE_WARN,
		LOG_TYPE_ERROR,
    };

	inline const char* log_type_to_string(LOG_TYPE type)
	{
		switch (type)
		{
		case LOG_TYPE::LOG_TYPE_DEFAULT: return "INFO";
		case LOG_TYPE::LOG_TYPE_STATUS:  return "STATUS";
		case LOG_TYPE::LOG_TYPE_GREEN:   return "OK";
		case LOG_TYPE::LOG_TYPE_WARN:    return "WARN";
		case LOG_TYPE::LOG_TYPE_ERROR:   return "ERROR";
		default:                         return "UNKNOWN";
		}
	}

	inline std::ofstream log_file;

	// Plain (INFO) lines are troubleshooting detail: written only with [Troubleshooting] Verbose=1 or dev mode (config sets this
	// as it loads). A player's log keeps the STATUS / OK / WARN / ERROR lines: what loaded, and what went wrong.
	inline bool g_debug_lines = false;
	inline void set_debug_lines(bool on) { g_debug_lines = on; }
	inline std::once_flag log_file_once;

	// The last session's logs, kept before this one overwrites them: where the folder rtx_comp\logs exists (made by hand on a
	// development machine; a player's game has none and keeps nothing), console.log and Remix's remix-dxvk.log, bridge32.log
	// and bridge64.log are copied to rtx_comp\logs\<when that session's console.log was last written>\. The newest 30 sessions
	// stay. A hitch seen while playing can then be looked up afterwards ([Hitch] and [DS1 hitch] lines), whatever was started since.
	inline void keep_last_session_logs(const std::string& dir_path, const std::string& console_path)
	{
		namespace fs = std::filesystem;
		std::error_code ec;
		const fs::path keep = fs::path(dir_path) / "logs";
		if (!fs::is_directory(keep, ec) || !fs::exists(console_path, ec))
			return;
		WIN32_FILE_ATTRIBUTE_DATA info {};
		SYSTEMTIME utc {}, local {};
		if (!GetFileAttributesExA(console_path.c_str(), GetFileExInfoStandard, &info) ||
			!FileTimeToSystemTime(&info.ftLastWriteTime, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
			return;
		char name[32];
		sprintf_s(name, "%04u%02u%02u_%02u%02u%02u", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);
		const fs::path session = keep / name;
		if (fs::exists(session, ec) || !fs::create_directories(session, ec))
			return;
		fs::copy_file(console_path, session / "console.log", ec);
		const fs::path remix_logs = fs::path(shared::globals::root_path) / "rtx-remix" / "logs";
		for (const char* file : { "remix-dxvk.log", "bridge32.log", "bridge64.log" })
			fs::copy_file(remix_logs / file, session / file, ec);

		std::vector<fs::path> sessions;
		for (const auto& entry : fs::directory_iterator(keep, ec))
			if (entry.is_directory(ec))
				sessions.push_back(entry.path());
		std::sort(sessions.begin(), sessions.end());
		for (size_t i = 0; i + 30 < sessions.size(); i++)
			fs::remove_all(sessions[i], ec);
	}

	inline void init_log_file()
	{
		std::call_once(log_file_once, []() {
			const std::string dir_path = shared::globals::root_path + "\\rtx_comp";
			std::filesystem::create_directories(dir_path);
			// The lookdev host shares the game's folder and may run beside it.
			const bool host = std::wstring_view(GetCommandLineW()).find(L"-lookdev-host") != std::wstring_view::npos;
			const std::string file_path = dir_path + (host ? "\\console_host.log" : "\\console.log");
			if (!host)
				keep_last_session_logs(dir_path, file_path);
			log_file.open(file_path, std::ios::out | std::ios::trunc);
		});
	}

	inline void log(const std::string_view& module_str, const std::string_view& msg, LOG_TYPE type = LOG_TYPE::LOG_TYPE_DEFAULT, bool highlight = false, bool newline_infront = false)
	{
		if (type == LOG_TYPE::LOG_TYPE_DEFAULT && !g_debug_lines)
			return;
		auto colorize = [](const LOG_TYPE& t, const bool h)
			{
				switch (t)
				{
				case LOG_TYPE::LOG_TYPE_DEFAULT:
					set_console_color_default(h);
					break;
				case LOG_TYPE::LOG_TYPE_STATUS:
					set_console_color_blue(h);
					break;
				case LOG_TYPE::LOG_TYPE_GREEN:
					set_console_color_green(h);
					break;
				case LOG_TYPE::LOG_TYPE_WARN:
					set_console_color_yellow(h);
					break;
				case LOG_TYPE::LOG_TYPE_ERROR:
					set_console_color_red(h);
					break;
				default:
					break;
				}
			};

		// width of the inner module field
		constexpr int inner_width = 14;

		std::cout << (newline_infront ? "\n" : "")
			<< std::setw(2) << (type == LOG_TYPE::LOG_TYPE_ERROR ? "!" : " ") << "[ ";

		colorize(type, true);
		std::cout << std::format("{:>{}}", module_str, inner_width);
		set_console_color_default();

		std::cout << " ]  ";

		colorize(type, highlight);
		std::cout << msg << '\n';
		set_console_color_default();

		init_log_file();
		if (log_file.is_open())
		{
			log_file
				<< "[" << log_type_to_string(type) << "] "
				<< "[" << module_str << "] "
				<< msg << std::endl; // auto flush
		}
	}
}
