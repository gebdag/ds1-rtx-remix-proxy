#pragma once
// Stand-in for src/comp/std_include.hpp so game_build.cpp builds into the offline test.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace shared::common
{
	enum class LOG_TYPE { LOG_TYPE_DEFAULT, LOG_TYPE_GREEN, LOG_TYPE_ERROR, LOG_TYPE_WARN, LOG_TYPE_STATUS };
	inline void log(const char* tag, const std::string& text, LOG_TYPE = LOG_TYPE::LOG_TYPE_DEFAULT, bool = false)
	{
		std::printf("[%s] %s\n", tag, text.c_str());
	}
}

namespace shared::globals
{
	inline bool lookdev_host = false;
	inline DWORD exe_module_addr = 0;
}
#define EXE_BASE shared::globals::exe_module_addr
