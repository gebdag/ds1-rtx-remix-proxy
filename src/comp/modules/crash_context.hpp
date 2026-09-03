#pragma once

namespace comp
{
	// Crash breadcrumbs recoverable from any memory dump of the process.
	//
	// A magic-tagged global block records per-frame game state (frame counter,
	// character/camera position) and a ring of recent proxy events. WER
	// full-memory dumps and the proxy's own minidump both capture it;
	// scripts/crash_triage.py locates it by scanning the proxy module for the
	// magic and decodes it alongside the exception record.
	namespace crash_context
	{
		constexpr char BLOCK_MAGIC[16] = "DS1PROXYCTX.v01";
		constexpr uint32_t EVENT_COUNT = 64;
		constexpr uint32_t EVENT_MSG_LEN = 52;

		struct event_entry
		{
			uint32_t frame;
			uint32_t tick_ms;
			char msg[EVENT_MSG_LEN];
		};

		struct block
		{
			char magic[16];
			uint32_t version;
			uint32_t frame;                  // frames presented so far
			uint32_t tick_ms;                // GetTickCount at last Present
			uint32_t start_tick_ms;          // GetTickCount at install()
			float char_pos[3];               // last known character world position
			float cam_pos[3];                // last known camera world position
			uint32_t pos_frame;              // frame the positions were captured on
			uint32_t pos_valid;              // bit0 = char_pos, bit1 = cam_pos
			uint32_t draw_calls_last_frame;
			volatile LONG event_seq;         // total events written (ring index = seq % EVENT_COUNT)
			event_entry events[EVENT_COUNT];
		};

		// Installs the unhandled-exception filter and stamps the block header.
		void install();

		// Per-frame update from Present: refreshes game-state fields and
		// re-asserts the exception filter (the game replaces it after startup).
		void on_present(uint32_t draw_calls);

		// Appends a breadcrumb to the event ring. Cheap and thread-safe;
		// call before risky operations (code patching, token emission, resets).
		void note(const char* fmt, ...);

		// Wall time since construction. Distinguishes a bridge call that
		// genuinely blocked waiting on the server from one that failed
		// instantly because the command channel was already out of sync —
		// the two are indistinguishable from the returned HRESULT alone.
		struct stopwatch
		{
			stopwatch();
			uint32_t ms() const;

		private:
			int64_t start_;
		};

		const block& get();
	}
}
