#include "std_include.hpp"
#include "host_feed.hpp"
#include "texture_names.hpp"
#include "ffx_instances.hpp"
#include "fx_draw_owner.hpp"

#include "shared/lookdev/capture_format.hpp"

namespace comp::game::host_feed
{
	namespace
	{
		frame_state s_frame;
		std::vector<point_lights::light> s_lights;
	}

	const frame_state& frame() { return s_frame; }
	const std::vector<point_lights::light>& lights() { return s_lights; }
}

using namespace comp::game;

// Called by ds1_lookdev_host with the capture's game-state records (only ever
// on the host's render thread, between replayed calls).
extern "C" void __cdecl lookdev_host_feed(uint32_t type, const void* payload, uint32_t size)
{
	if (!shared::globals::lookdev_host || !payload)
		return;

	if (type == lookdev::REC_GAME_FRAME && size >= sizeof(lookdev::game_frame))
	{
		lookdev::game_frame f;
		std::memcpy(&f, payload, sizeof(f));
		auto& s = host_feed::s_frame;
		s.camera_valid = (f.flags & lookdev::GAME_FRAME_CAMERA) != 0;
		s.player_valid = (f.flags & lookdev::GAME_FRAME_PLAYER) != 0;
		s.heading_valid = (f.flags & lookdev::GAME_FRAME_HEADING) != 0;
		s.estus_empty = (f.flags & lookdev::GAME_FRAME_ESTUS_EMPTY) != 0;
		std::memcpy(s.camera, f.camera, sizeof(s.camera));
		std::memcpy(s.player, f.player, sizeof(s.player));
		s.heading = f.heading;
		s.map_id = f.map_id;
		// The first game-state record of a frame: a capture without effect instances (before 2026-11-11) leaves none behind
		// from the capture shown before it. REC_GAME_FFX, when there is one, follows.
		ffx_instances::set_snapshot(nullptr, 0, 0);
		fx_draw_owner::set_host_owner(0);
	}
	else if (type == lookdev::REC_GAME_FX_OWNER && size >= sizeof(lookdev::id_only))
	{
		lookdev::id_only owner;
		std::memcpy(&owner, payload, sizeof(owner));
		fx_draw_owner::set_host_owner(static_cast<unsigned>(owner.id));
	}
	else if (type == lookdev::REC_GAME_FFX && size >= sizeof(lookdev::game_ffx))
	{
		lookdev::game_ffx h;
		std::memcpy(&h, payload, sizeof(h));
		if (h.element_size && sizeof(h) + size_t(h.count) * h.element_size <= size)
		{
			ffx_instances::set_snapshot(static_cast<const uint8_t*>(payload) + sizeof(h), h.count, h.element_size);
			// Once per capture shown (the record comes with every replayed frame).
			static uint32_t logged_count = ~0u;
			if (h.count != logged_count)
			{
				logged_count = h.count;
				shared::common::log("HostFeed", std::format("the capture holds {} live effect instances", h.count));
			}
		}
	}
	else if (type == lookdev::REC_GAME_LIGHTS && size >= sizeof(lookdev::game_lights))
	{
		lookdev::game_lights h;
		std::memcpy(&h, payload, sizeof(h));
		// The light struct is the proxy's own; both ends are this proxy, but a
		// capture from another build could differ. Fields are only ever appended, so an
		// older, shorter record loads with its missing fields zero.
		if (h.element_size == 0 || h.element_size > sizeof(point_lights::light) ||
			sizeof(h) + size_t(h.count) * h.element_size > size)
			return;
		host_feed::s_lights.assign(h.count, point_lights::light {});
		const auto* src = static_cast<const uint8_t*>(payload) + sizeof(h);
		for (uint32_t i = 0; i < h.count; ++i)
			std::memcpy(&host_feed::s_lights[i], src + size_t(i) * h.element_size, h.element_size);
	}
}

extern "C" void __cdecl lookdev_host_texture_name(IDirect3DBaseTexture9* texture, const char* name)
{
	if (shared::globals::lookdev_host && texture && name)
		texture_names::set_name(texture, name);
}
