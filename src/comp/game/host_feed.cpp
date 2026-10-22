#include "std_include.hpp"
#include "host_feed.hpp"
#include "texture_names.hpp"

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
		std::memcpy(s.camera, f.camera, sizeof(s.camera));
		std::memcpy(s.player, f.player, sizeof(s.player));
		s.heading = f.heading;
		s.map_id = f.map_id;
	}
	else if (type == lookdev::REC_GAME_LIGHTS && size >= sizeof(lookdev::game_lights))
	{
		lookdev::game_lights h;
		std::memcpy(&h, payload, sizeof(h));
		// The light struct is the proxy's own; both ends are this proxy, but a
		// capture from another build could differ.
		if (h.element_size != sizeof(point_lights::light) || sizeof(h) + size_t(h.count) * h.element_size > size)
			return;
		host_feed::s_lights.resize(h.count);
		if (h.count)
			std::memcpy(host_feed::s_lights.data(), static_cast<const uint8_t*>(payload) + sizeof(h), size_t(h.count) * h.element_size);
	}
}

extern "C" void __cdecl lookdev_host_texture_name(IDirect3DBaseTexture9* texture, const char* name)
{
	if (shared::globals::lookdev_host && texture && name)
		texture_names::set_name(texture, name);
}
