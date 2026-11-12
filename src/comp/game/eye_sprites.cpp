#include "std_include.hpp"
#include "eye_sprites.hpp"

#include "texture_names.hpp"
#include "shared/common/config.hpp"

namespace comp::game::eye_sprites
{
	namespace
	{
		// The largest such quad: the effects' size is 0.03 to 0.09 m. Larger s01036 quads (138 effects use the sprite) are glows
		// of other kinds.
		constexpr float kMaxQuad = 0.12f;
		// An eye's glow summed over its quads: the game emits a particle every frame, and at 60 fps the sum pulses about three
		// times a second (one particle life, 0.33 s), 6 % either way (rtx_comp\\eye_log.txt, the Butcher). Each eye is held to
		// its own average over this many seconds. Eyes are told apart by where they are: the slot whose last position is
		// nearest within kSameEye, else the one used longest ago. By their order in the frame an eye took over another's
		// average whenever one was culled or an enemy came or went, and flashed.
		constexpr float kSameEye = 0.25f;
		constexpr double kSteadySeconds = 0.25;
		constexpr int kSlots = 8;
		constexpr int kLogLines = 40000;

		float g_gain = 1.6f;
		struct steady_slot
		{
			double average = 0.0;
			double last = 0.0;
			float where[3] = {};
		};
		steady_slot g_slots[kSlots];
		IDirect3DBaseTexture9* g_last_texture = nullptr;
		bool g_last_is_eye_sprite = false;
		FILE* g_log = nullptr;
		int g_log_lines = 0;

		// Diagnostic: what an eye's quads add up to per frame, to read how it flashes. The game's particles age in real time but
		// are emitted per frame, which nothing outside the frame can see. Lines stop at kLogLines. Only with dev mode or Verbose.
		void log_draw(unsigned frame, int live, float sum, float largest, const float* first)
		{
			static const bool wanted = shared::common::config::get().dev_logs();
			if (!wanted || shared::globals::lookdev_host || g_log_lines >= kLogLines)
				return;
			if (!g_log)
			{
				if (fopen_s(&g_log, "rtx_comp\\eye_log.txt", "w") != 0)
				{
					g_log_lines = kLogLines;
					return;
				}
				std::fputs("# seconds frame live_quads alpha_sum largest_quad x y z\n", g_log);
			}
			LARGE_INTEGER now, rate;
			QueryPerformanceCounter(&now);
			QueryPerformanceFrequency(&rate);
			std::fprintf(g_log, "%.4f %u %d %.3f %.3f %.2f %.2f %.2f\n", double(now.QuadPart) / double(rate.QuadPart), frame, live, sum, largest, first[0], first[1], first[2]);
			if (++g_log_lines % 500 == 0 || g_log_lines >= kLogLines)
				std::fflush(g_log);
		}
	}

	void set_gain(float gain)
	{
		g_gain = std::max(gain, 0.0f);
	}

	const void* adjust(IDirect3DBaseTexture9* texture, const void* vertices, unsigned stride, unsigned min_index, unsigned num_verts,
		unsigned frame, int colour_offset, std::vector<std::uint8_t>& copy)
	{
		if (texture != g_last_texture)
		{
			g_last_texture = texture;
			g_last_is_eye_sprite = texture && texture_names::name_of(texture) == "s01036";
		}
		if (!g_last_is_eye_sprite || g_gain == 1.0f || colour_offset < 12 || stride < unsigned(colour_offset) + 4 || num_verts < 4 || num_verts % 4 != 0)
			return vertices;
		const unsigned kColourOffset = unsigned(colour_offset);

		const auto* base = static_cast<const std::uint8_t*>(vertices) + size_t(min_index) * stride;
		float largest = 0.0f, sum = 0.0f;
		int live = 0;
		const float* first = nullptr;
		for (unsigned q = 0; q + 4 <= num_verts; q += 4)
		{
			const auto* a = reinterpret_cast<const float*>(base + size_t(q) * stride);
			const auto* b = reinterpret_cast<const float*>(base + size_t(q + 3) * stride);
			const DWORD colour = *reinterpret_cast<const DWORD*>(base + size_t(q) * stride + kColourOffset);
			const unsigned alpha = colour >> 24;
			if (!alpha || !std::isfinite(a[0]) || !std::isfinite(b[0]))
				continue;
			const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
			largest = std::max(largest, dx * dx + dy * dy + dz * dz);
			sum += float(alpha) / 255.0f;
			live++;
			if (!first)
				first = a;
		}
		largest = std::sqrt(largest);
		if (!live || largest > kMaxQuad)
			return vertices;

		log_draw(frame, live, sum, largest, first);

		LARGE_INTEGER now_count, rate;
		QueryPerformanceCounter(&now_count);
		QueryPerformanceFrequency(&rate);
		const double now = double(now_count.QuadPart) / double(rate.QuadPart);
		steady_slot* best = nullptr;
		float best_distance = kSameEye * kSameEye;
		for (auto& s : g_slots)
		{
			const float dx = s.where[0] - first[0], dy = s.where[1] - first[1], dz = s.where[2] - first[2];
			const float d = dx * dx + dy * dy + dz * dz;
			if (now - s.last < 0.5 && d < best_distance)
			{
				best_distance = d;
				best = &s;
			}
		}
		const bool same_eye = best != nullptr;
		if (!best)
		{
			best = &g_slots[0];
			for (auto& s : g_slots)
				if (s.last < best->last)
					best = &s;
		}
		auto& slot = *best;
		const double dt = same_eye ? now - slot.last : 1.0;
		std::memcpy(slot.where, first, sizeof(slot.where));
		slot.average = dt > 0.5 || slot.average <= 0.0 ? sum : slot.average + (sum - slot.average) * (1.0 - std::exp(-dt / kSteadySeconds));
		slot.last = now;
		const float gain = g_gain * std::clamp(float(slot.average) / std::max(sum, 1e-3f), 0.5f, 2.0f);
		if (gain == 1.0f)
			return vertices;

		const size_t start = size_t(min_index) * stride;
		copy.resize(start + size_t(num_verts) * stride);
		std::memcpy(copy.data() + start, base, size_t(num_verts) * stride);
		for (unsigned v = 0; v < num_verts; v++)
		{
			auto* colour = reinterpret_cast<DWORD*>(copy.data() + start + size_t(v) * stride + kColourOffset);
			const float alpha = float(*colour >> 24) * gain;
			*colour = (*colour & 0x00FFFFFF) | (static_cast<DWORD>(std::min(alpha, 255.0f)) << 24);
		}
		return copy.data();
	}
}
