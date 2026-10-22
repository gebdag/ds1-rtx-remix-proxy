#include "std_include.hpp"
#include "ffp_prof.hpp"

namespace shared::common::ffp_prof
{
	namespace
	{
		struct step_info
		{
			const char* label;
			const char* describe;
			bool child;
		};

		const step_info k_steps[STEP_COUNT] =
		{
			{ "Route + dedup",      "Perf classification and dedup bookkeeping before routing.", false },
			{ "Z-write query",      "GetRenderState(D3DRS_ZWRITEENABLE) per draw for the prepass gate. Served from the bridge's client-side shadow, so cheap but not free. [FFP] ZWriteQueryPerDraw=0 uses the passively tracked value instead.", false },
			{ "Engage",             "SetVertexShader(null) + SetPixelShader(null). Latched, so it only costs on the transition into FFP.", false },
			{ "Transforms",         "SetTransform(VIEW/PROJECTION/WORLD). Re-applied on every draw even when nothing changed.", true },
			{ "Texture stages",     "29 SetTextureStageState calls per draw: stage 0 MODULATE plus disabling stages 1-7. Identical for almost every draw.", true },
			{ "Albedo bind",        "SetTexture(0, diffuse) then 7 x SetTexture(n, null).", false },
			{ "Expand rigid",       "Rebuild the draw's vertices as FLOAT3 pos + FLOAT3 normal + D3DCOLOR + FLOAT2 uv into a proxy-owned VB.", false },
			{ "  Lock",             "GetDesc + source Lock(READONLY) + destination Lock. Both read the bridge's client-side shadow memory.", true },
			{ "  Convert",          "The per-vertex CPU conversion loop (UBYTE4N normals, SHORT2 UVs, colour packing).", true },
			{ "  Unlock",           "The Unlock pair. The destination Unlock memcpy's the whole written range into the bridge IPC channel and marks the buffer for GPU re-upload. This is the step that pays for FFP on the Remix side.", true },
			{ "  Bind",             "SetVertexDeclaration + SetStreamSource onto the expanded VB.", true },
			{ "Decal query",        "3 x GetRenderState to detect the game's own alpha-blended edge-fade strips so they can be tagged as decals.", false },
			{ "Draw (base)",        "The base Draw(Indexed)Primitive that actually reaches Remix.", false },
			{ "Overlay save RS",    "7 x GetRenderState saving blend state for the overlay pass. Currently paid on EVERY converted draw, including the majority that have no overlay.", false },
			{ "Overlay setup",      "Second vertex expansion (overlay UV pair) + 8 SetTexture + 7 SetTextureStageState + 7 SetRenderState.", false },
			{ "Draw (overlay)",     "The coplanar decal draw for two-layer materials. Doubles the draw and the bridge traffic for those meshes.", false },
			{ "Restore textures",   "8 x SetTexture putting the game's original bindings back.", false },
			{ "Restore stream",     "SetVertexDeclaration + SetStreamSource back to the game's own VB.", false },
			{ "Disengage",          "Restore VS/PS and re-apply transforms for the next shader draw.", false },
			{ "Context restore",    "drawcall_mod_context::restore_all — replays saved render states.", false },
		};

		// Smoothing: the raw per-frame numbers jitter far too much to read.
		constexpr double EMA_ALPHA = 0.08;

		bool s_profiling = false;

		// Accumulators for the frame in flight.
		long long s_self_ticks[STEP_COUNT] = {};
		unsigned int s_calls[STEP_COUNT] = {};
		unsigned int s_converted = 0;
		unsigned int s_passthrough = 0;
		unsigned long long s_expand_bytes = 0;
		unsigned long long s_saved_bytes = 0;
		unsigned int s_reuse_hit = 0;
		unsigned int s_reuse_miss = 0;

		// Smoothed values published to the UI.
		double s_self_ms[STEP_COUNT] = {};
		unsigned int s_calls_last[STEP_COUNT] = {};
		unsigned int s_converted_last = 0;
		unsigned int s_passthrough_last = 0;
		double s_expand_mb = 0.0;
		double s_saved_mb = 0.0;
		unsigned int s_reuse_hit_last = 0;
		unsigned int s_reuse_miss_last = 0;
		double s_frame_ms = 0.0;

		long long s_last_present_qpc = 0;

		// Exclusive-time bookkeeping. The proxy issues draws from the game's
		// render thread only, so a plain static is sufficient and avoids the TLS
		// lookup in the hot path.
		long long s_child_ticks = 0;

		double qpc_freq()
		{
			static double freq = []
			{
				LARGE_INTEGER f{};
				QueryPerformanceFrequency(&f);
				return static_cast<double>(f.QuadPart);
			}();
			return freq;
		}

		inline long long now_qpc()
		{
			LARGE_INTEGER t{};
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		inline double ticks_to_ms(long long ticks)
		{
			return (static_cast<double>(ticks) * 1000.0) / qpc_freq();
		}
	}

	const char* label(step s)    { return k_steps[static_cast<int>(s)].label; }
	const char* describe(step s) { return k_steps[static_cast<int>(s)].describe; }
	bool is_child(step s)        { return k_steps[static_cast<int>(s)].child; }

	bool profiling() { return s_profiling; }

	void set_profiling(bool e)
	{
		if (e == s_profiling) return;
		s_profiling = e;
		reset();
	}

	scope::scope(step s)
	{
		if (!s_profiling) return;
		active_ = true;
		step_ = s;
		// Stash the parent's child-tick accumulator and start a fresh one, so
		// this scope's own children are attributed to this scope, not the parent.
		saved_child_ = s_child_ticks;
		s_child_ticks = 0;
		start_ = now_qpc();
	}

	scope::~scope()
	{
		if (!active_) return;
		const long long elapsed = now_qpc() - start_;
		const int idx = static_cast<int>(step_);
		s_self_ticks[idx] += elapsed - s_child_ticks; // exclusive
		s_calls[idx]++;
		// Restore the parent's accumulator and charge it our full inclusive time.
		s_child_ticks = saved_child_ + elapsed;
	}

	void count_converted_draw()   { if (s_profiling) s_converted++; }
	void count_passthrough_draw() { if (s_profiling) s_passthrough++; }
	void add_expand_bytes(unsigned int b) { if (s_profiling) s_expand_bytes += b; }
	void add_saved_bytes(unsigned int b)  { if (s_profiling) s_saved_bytes += b; }
	void count_expand_reuse_hit()  { if (s_profiling) s_reuse_hit++; }
	void count_expand_reuse_miss() { if (s_profiling) s_reuse_miss++; }

	void on_present()
	{
		const long long now = now_qpc();
		if (s_last_present_qpc != 0)
		{
			const double dt = ticks_to_ms(now - s_last_present_qpc);
			s_frame_ms = (s_frame_ms == 0.0) ? dt : s_frame_ms + (dt - s_frame_ms) * EMA_ALPHA;
		}
		s_last_present_qpc = now;

		if (!s_profiling) return;

		for (int i = 0; i < STEP_COUNT; i++)
		{
			const double ms = ticks_to_ms(s_self_ticks[i]);
			s_self_ms[i] += (ms - s_self_ms[i]) * EMA_ALPHA;
			s_calls_last[i] = s_calls[i];
			s_self_ticks[i] = 0;
			s_calls[i] = 0;
		}

		const double emb = static_cast<double>(s_expand_bytes) / (1024.0 * 1024.0);
		const double smb = static_cast<double>(s_saved_bytes) / (1024.0 * 1024.0);
		s_expand_mb += (emb - s_expand_mb) * EMA_ALPHA;
		s_saved_mb  += (smb - s_saved_mb) * EMA_ALPHA;

		s_converted_last = s_converted;
		s_passthrough_last = s_passthrough;
		s_reuse_hit_last = s_reuse_hit;
		s_reuse_miss_last = s_reuse_miss;

		s_expand_bytes = 0;
		s_saved_bytes = 0;
		s_converted = 0;
		s_passthrough = 0;
		s_reuse_hit = 0;
		s_reuse_miss = 0;
		s_child_ticks = 0;
	}

	void reset()
	{
		for (int i = 0; i < STEP_COUNT; i++)
		{
			s_self_ticks[i] = 0;
			s_calls[i] = 0;
			s_self_ms[i] = 0.0;
			s_calls_last[i] = 0;
		}
		s_converted = s_passthrough = 0;
		s_converted_last = s_passthrough_last = 0;
		s_expand_bytes = s_saved_bytes = 0;
		s_expand_mb = s_saved_mb = 0.0;
		s_reuse_hit = s_reuse_miss = 0;
		s_reuse_hit_last = s_reuse_miss_last = 0;
		s_child_ticks = 0;
	}

	double       self_ms(step s)  { return s_self_ms[static_cast<int>(s)]; }
	unsigned int calls(step s)    { return s_calls_last[static_cast<int>(s)]; }
	double       frame_ms()       { return s_frame_ms; }
	unsigned int converted_draws()   { return s_converted_last; }
	unsigned int passthrough_draws() { return s_passthrough_last; }
	double       expand_mb()      { return s_expand_mb; }
	double       saved_mb()       { return s_saved_mb; }
	unsigned int reuse_hits()     { return s_reuse_hit_last; }
	unsigned int reuse_misses()   { return s_reuse_miss_last; }

	double total_ms()
	{
		double sum = 0.0;
		for (int i = 0; i < STEP_COUNT; i++)
			sum += s_self_ms[i];
		return sum;
	}
}
