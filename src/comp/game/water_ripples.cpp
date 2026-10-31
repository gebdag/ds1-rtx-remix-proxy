#include "std_include.hpp"
#include "water_ripples.hpp"
#include "camera.hpp"

#include "shared/common/remix_api.hpp"

#include <cstring>
#include <format>

namespace comp::game::water_ripples
{
	namespace
	{
		float g_sent[3] = {};
		// A runtime without the option refuses it; asking again every frame would not change that.
		bool g_refused = false;
	}

	void frame_update()
	{
		if (g_refused || !shared::common::remix_api::is_initialized())
			return;
		auto& bridge = shared::common::remix_api::get().m_bridge;
		if (!bridge.SetConfigVariable)
			return;

		float x = 0.0f, depth = 0.0f, up = 0.0f;
		if (!camera::get_char_pos(x, depth, up) || (x == 0.0f && depth == 0.0f && up == 0.0f))
			return;
		const float position[3] = { x, up, depth };
		if (std::memcmp(position, g_sent, sizeof(position)) == 0)
			return;
		std::memcpy(g_sent, position, sizeof(position));
		g_refused = bridge.SetConfigVariable("rtx.ds1Ripple.playerPosition",
			std::format("{}, {}, {}", position[0], position[1], position[2]).c_str()) != REMIXAPI_ERROR_CODE_SUCCESS;
		if (g_refused)
			shared::common::log("WaterRipples", "Remix refused rtx.ds1Ripple.playerPosition (older runtime)");
	}
}
