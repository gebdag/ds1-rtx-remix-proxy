#include "std_include.hpp"
#include <DbgHelp.h>
#include <cstdarg>
#pragma comment(lib, "dbghelp.lib")

#include "crash_context.hpp"
#include "../game/camera.hpp"

namespace comp::crash_context
{
	namespace
	{
		block g_block = {};
		LONG g_in_handler = 0;

		std::string exe_dir()
		{
			char path[MAX_PATH];
			GetModuleFileNameA(nullptr, path, MAX_PATH);
			std::string dir(path);
			if (const auto pos = dir.find_last_of("\\/"); pos != std::string::npos)
				dir.resize(pos);
			return dir;
		}

		// Prefer the WER dump folder so all crash artifacts live in one place.
		std::string dump_dir()
		{
			const auto trex = exe_dir() + "\\.trex\\dumps";
			if (GetFileAttributesA(trex.c_str()) != INVALID_FILE_ATTRIBUTES)
				return trex;
			return exe_dir();
		}

		// Formats addr as "module.dll+0xRVA" (or the raw address when unmapped).
		const char* module_plus_offset(const void* addr, char* buf, size_t len)
		{
			MEMORY_BASIC_INFORMATION mbi = {};
			char path[MAX_PATH];
			if (VirtualQuery(addr, &mbi, sizeof(mbi)) && mbi.AllocationBase &&
				GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), path, MAX_PATH))
			{
				const char* base = strrchr(path, '\\');
				snprintf(buf, len, "%s+0x%X", base ? base + 1 : path,
					static_cast<unsigned>(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mbi.AllocationBase)));
			}
			else
			{
				snprintf(buf, len, "0x%p", addr);
			}
			return buf;
		}

		void append_line(HANDLE file, const char* fmt, ...)
		{
			char line[512];
			va_list args;
			va_start(args, fmt);
			int n = vsnprintf(line, sizeof(line), fmt, args);
			va_end(args);
			if (n < 0) return;
			n = std::min(n, static_cast<int>(sizeof(line) - 1));
			DWORD written = 0;
			WriteFile(file, line, static_cast<DWORD>(n), &written, nullptr);
			WriteFile(file, "\r\n", 2, &written, nullptr);
		}

		void write_report(const char* path, EXCEPTION_POINTERS* ep, const char* dump_path)
		{
			const auto file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return;

			const auto* rec = ep->ExceptionRecord;
			char mod_buf[MAX_PATH];
			append_line(file, "DS1 proxy crash report");
			append_line(file, "proxy build: %s %s", __DATE__, __TIME__);
			append_line(file, "dump: %s", dump_path);
			append_line(file, "thread: %u", GetCurrentThreadId());
			append_line(file, "exception: code=0x%08X at %s",
				rec->ExceptionCode,
				module_plus_offset(rec->ExceptionAddress, mod_buf, sizeof(mod_buf)));
			if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
			{
				append_line(file, "access violation: %s address 0x%08X",
					rec->ExceptionInformation[0] == 1 ? "writing" : (rec->ExceptionInformation[0] == 8 ? "executing" : "reading"),
					static_cast<unsigned>(rec->ExceptionInformation[1]));
			}

			const auto* ctx = ep->ContextRecord;
			append_line(file, "eax=%08X ebx=%08X ecx=%08X edx=%08X", ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx);
			append_line(file, "esi=%08X edi=%08X ebp=%08X esp=%08X eip=%08X", ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp, ctx->Eip);

			append_line(file, "");
			append_line(file, "frame=%u uptime_ms=%u draws_last_frame=%u",
				g_block.frame, g_block.tick_ms - g_block.start_tick_ms, g_block.draw_calls_last_frame);
			if (g_block.pos_valid & 1)
			{
				append_line(file, "char_pos: %.2f %.2f %.2f (frame %u)",
					g_block.char_pos[0], g_block.char_pos[1], g_block.char_pos[2], g_block.pos_frame);
			}
			if (g_block.pos_valid & 2)
			{
				append_line(file, "cam_pos:  %.2f %.2f %.2f",
					g_block.cam_pos[0], g_block.cam_pos[1], g_block.cam_pos[2]);
			}

			append_line(file, "");
			append_line(file, "recent events (oldest first):");
			const LONG seq = g_block.event_seq;
			const LONG first = seq > static_cast<LONG>(EVENT_COUNT) ? seq - EVENT_COUNT : 0;
			for (LONG i = first; i < seq; ++i)
			{
				const auto& ev = g_block.events[i % EVENT_COUNT];
				append_line(file, "  [frame %u, t+%ums] %s", ev.frame, ev.tick_ms - g_block.start_tick_ms, ev.msg);
			}

			CloseHandle(file);
		}

		LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep)
		{
			// A crash inside the handler (or a second faulting thread) must not recurse.
			if (InterlockedCompareExchange(&g_in_handler, 1, 0) != 0)
				return EXCEPTION_CONTINUE_SEARCH;

			SYSTEMTIME st;
			GetLocalTime(&st);
			const auto dir = dump_dir();
			char dump_path[MAX_PATH], report_path[MAX_PATH];
			wsprintfA(dump_path, "%s\\DARKSOULS_proxy_%04d%02d%02d_%02d%02d%02d.dmp",
				dir.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
			wsprintfA(report_path, "%s\\DARKSOULS_proxy_%04d%02d%02d_%02d%02d%02d.txt",
				dir.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

			const auto file = CreateFileA(dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file != INVALID_HANDLE_VALUE)
			{
				MINIDUMP_EXCEPTION_INFORMATION mei;
				mei.ThreadId = GetCurrentThreadId();
				mei.ExceptionPointers = ep;
				mei.ClientPointers = FALSE;
				// DataSegs captures the breadcrumb block; IndirectlyReferencedMemory
				// captures what crash-site registers/stack point at. The WER
				// LocalDumps full-memory dump remains the primary artifact.
				MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
					static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory),
					&mei, nullptr, nullptr);
				CloseHandle(file);
			}

			write_report(report_path, ep, dump_path);

			char mod_buf[MAX_PATH];
			char msg[512];
			snprintf(msg, sizeof(msg), "CRASH: code=0x%08X at %s frame=%u — report: %s",
				ep->ExceptionRecord->ExceptionCode,
				module_plus_offset(ep->ExceptionRecord->ExceptionAddress, mod_buf, sizeof(mod_buf)),
				g_block.frame, report_path);
			shared::common::log("CrashHandler", msg, shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);

			// Let WER continue so its full-memory dump is still written.
			InterlockedExchange(&g_in_handler, 0);
			return EXCEPTION_CONTINUE_SEARCH;
		}
	}

	void install()
	{
		memcpy(g_block.magic, BLOCK_MAGIC, sizeof(g_block.magic));
		g_block.version = 1;
		g_block.start_tick_ms = GetTickCount();
		g_block.tick_ms = g_block.start_tick_ms;
		SetUnhandledExceptionFilter(crash_handler);
	}

	void on_present(const uint32_t draw_calls)
	{
		g_block.frame++;
		g_block.tick_ms = GetTickCount();
		g_block.draw_calls_last_frame = draw_calls;

		float x, y, z;
		uint32_t valid = 0;
		if (game::camera::get_char_pos(x, y, z))
		{
			g_block.char_pos[0] = x; g_block.char_pos[1] = y; g_block.char_pos[2] = z;
			valid |= 1;
		}
		if (game::camera::get_world_pos(x, y, z))
		{
			g_block.cam_pos[0] = x; g_block.cam_pos[1] = y; g_block.cam_pos[2] = z;
			valid |= 2;
		}
		if (valid)
			g_block.pos_frame = g_block.frame;
		g_block.pos_valid = valid;

		// The game (or an overlay) replaces the unhandled-exception filter after
		// startup — proxy minidumps silently stopped appearing once it did. Keep
		// re-asserting, and record who had stolen it.
		if ((g_block.frame & 0xFF) == 0)
		{
			const auto prev = SetUnhandledExceptionFilter(crash_handler);
			if (prev && prev != crash_handler)
				note("exception filter was 0x%p, re-asserted", prev);
		}
	}

	void note(const char* fmt, ...)
	{
		const LONG seq = InterlockedIncrement(&g_block.event_seq) - 1;
		auto& ev = g_block.events[seq % EVENT_COUNT];
		ev.frame = g_block.frame;
		ev.tick_ms = GetTickCount();

		va_list args;
		va_start(args, fmt);
		vsnprintf(ev.msg, sizeof(ev.msg), fmt, args);
		va_end(args);
		ev.msg[sizeof(ev.msg) - 1] = '\0';
	}

	stopwatch::stopwatch()
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		start_ = now.QuadPart;
	}

	uint32_t stopwatch::ms() const
	{
		LARGE_INTEGER now, freq;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&freq);
		return static_cast<uint32_t>((now.QuadPart - start_) * 1000 / freq.QuadPart);
	}

	const block& get()
	{
		return g_block;
	}
}
