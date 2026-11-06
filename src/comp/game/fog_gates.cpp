#include "std_include.hpp"
#include "fog_gates.hpp"

#include "texture_names.hpp"
#include "vanilla_view.hpp"
#include "shared/common/config.hpp"
#include "shared/common/remix_api.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <vector>

namespace comp::game::fog_gates
{
	namespace
	{
		constexpr int kGates = 4;
		// The same gate in a later frame: its batch's centre wanders with its ribbons.
		constexpr float kSameGate = 3.0f;
		// The fit is kept while the batch is no larger than this share of what it was fitted to: ribbons come and go.
		constexpr float kRefit = 1.2f;
		constexpr float kGoneSeconds = 0.4f;   // not drawn for this long: the game has removed it
		constexpr float kFollowSeconds = 0.35f; // its strength, while the game draws it
		constexpr float kDissolveSeconds = 0.7f; // once it has gone (about 2 s to nothing)
		// The same door in a later frame: the quad does not wander. Two doors can stand closer than a gate's ribbons reach.
		constexpr float kSameDoor = 1.0f;
		// Ribbons above this few still belong to a gate there: a fading gate's batch shrinks. Under it, they may be anyone's.
		constexpr UINT kFewestRibbons = 16;
		// The volume thins out over its outer 0.25 m (dsFogGateRest), so it starts that far past the door the quad closes.
		constexpr float kPastDoor = 0.25f;
		constexpr DWORD kNotOurs = 0, kTaken = 1;
		// Stage 4's D3DTSS_CONSTANT on the quad: Surface::isDs1CameraPasses.
		constexpr DWORD kCameraPassesMarker = 0xD4000005;

		struct gate
		{
			bool used = false;
			bool has_wall = false; // the game's opaque quad gave its place and size
			bool ribbons = false;
			bool was_gone = false;
			float frame_energy = 0.0f; // ribbons so far this frame
			float centre[3] = {};
			float angle = 0.0f, half_width = 0.0f, half_height = 0.0f, half_thick = 0.5f;
			float fitted_to = 0.0f;
			float peak = 0.0f;
			float energy = 0.0f;
			float level = 0.0f;
			std::chrono::steady_clock::time_point seen {};
			float sent_a[4] = {};
			float sent_b[4] = {};
			bool sent = false;
		};
		std::array<gate, kGates> g_gates;
		bool g_refused = false;
		auto g_last = std::chrono::steady_clock::now();
		std::vector<float> g_values;

		// From the 2nd to the 98th percentile: stray ribbons do not make the slab larger.
		std::pair<float, float> range(std::vector<float>& v)
		{
			const size_t low = v.size() / 50, high = v.size() - 1 - v.size() / 50;
			std::nth_element(v.begin(), v.begin() + low, v.end());
			const float a = v[low];
			std::nth_element(v.begin(), v.begin() + high, v.end());
			return { a, v[high] };
		}

		// The nearest gate within reach, sideways and up.
		gate* find(double x, double y, double z, bool door)
		{
			gate* slot = nullptr;
			double best = 0.0;
			for (auto& g : g_gates)
			{
				if (!g.used)
					continue;
				const double dx = g.centre[0] - x, dy = g.centre[1] - y, dz = g.centre[2] - z;
				const double reach = door ? kSameDoor : kSameGate + g.half_width, reach_up = door ? kSameDoor : kSameGate + g.half_height;
				if (std::abs(dx) < reach && std::abs(dz) < reach && std::abs(dy) < reach_up && (!slot || dx * dx + dz * dz < best))
				{
					slot = &g;
					best = dx * dx + dz * dz;
				}
			}
			return slot;
		}

		// A free slot. Null: more gates than the runtime takes.
		gate* claim()
		{
			for (auto& g : g_gates)
				if (!g.used)
				{
					g = {};
					g.used = true;
					return &g;
				}
			return nullptr;
		}

		// The game closes every door with an opaque quad (s00917, in 114 of the 119 fog wall effects) and draws its ribbons over it. Its first
		// rectangle is the door. The camera sees through it to the volume, which is what closes the door now.
		DWORD observe_wall(const unsigned char* src, UINT stride, UINT position_offset, UINT count)
		{
			if (count < 4)
				return kNotOurs;
			const auto at = [&](UINT i) { return reinterpret_cast<const float*>(src + static_cast<size_t>(i) * stride + position_offset); };
			const float* a = at(0);
			const float* b = at(1);
			const float* c = at(2);
			const float* d = at(3);
			const float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
			const float width = std::sqrt(ux * ux + uz * uz), height = std::abs(a[1] - c[1]);
			if (width < 0.5f || height < 1.0f || std::abs(uy) > 0.1f * width)
				return kNotOurs;
			const float x = 0.25f * (a[0] + b[0] + c[0] + d[0]), y = 0.25f * (a[1] + b[1] + c[1] + d[1]), z = 0.25f * (a[2] + b[2] + c[2] + d[2]);
			gate* slot = find(x, y, z, true);
			if (!slot)
				slot = claim();
			if (!slot)
				return kNotOurs;
			slot->has_wall = true;
			slot->centre[0] = x;
			slot->centre[1] = y;
			slot->centre[2] = z;
			slot->angle = std::atan2(uz, ux);
			slot->half_width = 0.5f * width + kPastDoor;
			slot->half_height = 0.5f * height + kPastDoor;
			// The door opens once the volume is there. A quad with no ribbons is no gate (the Kiln cutscene effects) and stays as the game draws it.
			if (slot->ribbons && slot->sent_a[3] > 0.0f)
				return kCameraPassesMarker;
			if (!slot->ribbons)
				slot->seen = std::chrono::steady_clock::now(); // kept while the quad stands, with no volume
			return kNotOurs;
		}

		void fit(gate& g, const unsigned char* src, UINT stride, UINT position_offset, UINT count)
		{
			double cx = 0.0, cz = 0.0;
			for (UINT i = 0; i < count; i++)
			{
				const auto* p = reinterpret_cast<const float*>(src + static_cast<size_t>(i) * stride + position_offset);
				cx += p[0];
				cz += p[2];
			}
			cx /= count;
			cz /= count;
			// The slab's long side on the ground, by the spread of its vertices. A gate stands upright.
			double xx = 0.0, xz = 0.0, zz = 0.0;
			for (UINT i = 0; i < count; i++)
			{
				const auto* p = reinterpret_cast<const float*>(src + static_cast<size_t>(i) * stride + position_offset);
				const double dx = p[0] - cx, dz = p[2] - cz;
				xx += dx * dx;
				xz += dx * dz;
				zz += dz * dz;
			}
			g.angle = static_cast<float>(0.5 * std::atan2(2.0 * xz, xx - zz));
			const float ux = std::cos(g.angle), uz = std::sin(g.angle);

			auto measure = [&](auto value)
			{
				g_values.resize(count);
				for (UINT i = 0; i < count; i++)
					g_values[i] = value(reinterpret_cast<const float*>(src + static_cast<size_t>(i) * stride + position_offset));
				return range(g_values);
			};
			const auto along = measure([&](const float* p) { return static_cast<float>((p[0] - cx) * ux + (p[2] - cz) * uz); });
			const auto through = measure([&](const float* p) { return static_cast<float>(-(p[0] - cx) * uz + (p[2] - cz) * ux); });
			const auto up = measure([&](const float* p) { return p[1]; });
			const float mid_along = 0.5f * (along.first + along.second), mid_through = 0.5f * (through.first + through.second);
			g.centre[0] = static_cast<float>(cx) + ux * mid_along - uz * mid_through;
			g.centre[1] = 0.5f * (up.first + up.second);
			g.centre[2] = static_cast<float>(cz) + uz * mid_along + ux * mid_through;
			// Past the ribbons' own reach: the door frame hides what goes too far, and what falls short would show what is behind the gate.
			g.half_width = 0.5f * (along.second - along.first) * 1.15f + 0.3f;
			g.half_height = 0.5f * (up.second - up.first) * 1.1f + 0.3f;
			// No deeper than this, whatever the ribbons reach: fog this dense is opaque long before, and a body has to be able to open it.
			g.half_thick = std::clamp(0.5f * (through.second - through.first), 0.2f, 0.4f);
		}
	}

	DWORD observe_batch(IDirect3DBaseTexture9* texture, const void* vertices, UINT stride, UINT position_offset, UINT colour_offset, UINT count)
	{
		if (g_refused || !shared::common::config::get().fog_gate_enable || !texture || !shared::common::remix_api::is_initialized() || vanilla_view::active())
			return kNotOurs;
		const auto& name = texture_names::name_of(texture);
		const auto* src = static_cast<const unsigned char*>(vertices);
		if (name == "s00917")
			return observe_wall(src, stride, position_offset, count);
		if (count < kFewestRibbons || name != "s00313")
			return kNotOurs;

		double x = 0.0, y = 0.0, z = 0.0;
		float energy = 0.0f;
		for (UINT i = 0; i < count; i++)
		{
			const auto* v = src + static_cast<size_t>(i) * stride;
			const auto* p = reinterpret_cast<const float*>(v + position_offset);
			x += p[0];
			y += p[1];
			z += p[2];
			energy += v[colour_offset + 3] / 255.0f;
		}
		x /= count;
		y /= count;
		z /= count;

		gate* slot = find(x, y, z, false);
		// A small batch only joins a gate already there.
		if (slot && !slot->ribbons && count < 64)
			slot = nullptr;
		if (!slot && count >= 64)
			slot = claim();
		// More gates in view than the runtime takes: this one stays as the game draws it.
		if (!slot)
			return kNotOurs;

		if (count >= 64 && static_cast<float>(count) > slot->fitted_to * kRefit)
		{
			gate fitted = *slot;
			fit(fitted, src, stride, position_offset, count);
			// The quad knows the door exactly. Only how deep the fog is comes from the ribbons.
			if (slot->has_wall)
				slot->half_thick = fitted.half_thick;
			else
				*slot = fitted;
			slot->fitted_to = static_cast<float>(count);
		}
		slot->ribbons = true;
		slot->frame_energy += energy;
		slot->seen = std::chrono::steady_clock::now();
		// The game draws it until the volume is there.
		return slot->sent_a[3] > 0.0f ? kTaken : kNotOurs;
	}

	void frame_update()
	{
		if (g_refused || !shared::common::remix_api::is_initialized())
			return;
		auto& bridge = shared::common::remix_api::get().m_bridge;
		if (!bridge.SetConfigVariable)
			return;
		const auto now = std::chrono::steady_clock::now();
		const float dt = std::clamp(std::chrono::duration<float>(now - g_last).count(), 0.0f, 0.1f);
		g_last = now;
		for (int i = 0; i < kGates; i++)
		{
			auto& g = g_gates[i];
			if (!g.used && !g.sent)
				continue;
			if (g.frame_energy > 0.0f)
			{
				g.energy = g.frame_energy;
				g.peak = std::max(g.peak, g.energy);
				g.frame_energy = 0.0f;
			}
			const bool gone = !g.used || std::chrono::duration<float>(now - g.seen).count() > kGoneSeconds;
			const float target = gone || g.peak <= 0.0f ? 0.0f : std::min(g.energy / g.peak, 1.0f);
			// A new gate is there at once: it was there before we saw it. So is one coming back into view: the game does not draw what is out of it.
			if (!gone && (g.level == 0.0f || g.was_gone))
				g.level = target;
			g.was_gone = gone;
			g.level += (target - g.level) * (1.0f - std::exp(-dt / (gone ? kDissolveSeconds : kFollowSeconds)));
			const bool over = gone && g.level < 0.01f;
			const float a[4] = { g.centre[0], g.centre[1], g.centre[2], over ? 0.0f : g.level };
			const float b[4] = { g.angle, g.half_width, g.half_height, g.half_thick };
			if (!g.sent || std::memcmp(b, g.sent_b, sizeof(b)) != 0)
			{
				const auto value = std::format("{}, {}, {}, {}", b[0], b[1], b[2], b[3]);
				if (bridge.SetConfigVariable(std::format("rtx.ds1FogGate.gate{}B", i).c_str(), value.c_str()) != REMIXAPI_ERROR_CODE_SUCCESS)
				{
					g_refused = true;
					shared::common::log("FogGates", "Remix refused rtx.ds1FogGate (older runtime): the game's fog gates stay");
					return;
				}
				std::memcpy(g.sent_b, b, sizeof(b));
			}
			if (!g.sent || std::abs(a[3] - g.sent_a[3]) > 0.005f || (a[3] == 0.0f) != (g.sent_a[3] == 0.0f) || std::memcmp(a, g.sent_a, 3 * sizeof(float)) != 0)
			{
				bridge.SetConfigVariable(std::format("rtx.ds1FogGate.gate{}A", i).c_str(), std::format("{}, {}, {}, {}", a[0], a[1], a[2], a[3]).c_str());
				std::memcpy(g.sent_a, a, sizeof(a));
			}
			g.sent = true;
			if (over)
				g = {};
		}
	}
}
