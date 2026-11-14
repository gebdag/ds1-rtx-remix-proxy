#include "std_include.hpp"
#include "game_ui.hpp"

#include "shared/common/remix_api.hpp"

#include <atomic>

namespace comp::game::game_ui
{
	namespace
	{
		constexpr const char* kOption = "rtx.showGameUi";

		std::atomic<bool> g_toggle_requested { false };
		bool g_hidden = false;
	}

	void request_toggle()
	{
		g_toggle_requested = true;
	}

	void frame_update()
	{
		if (!g_toggle_requested.exchange(false) || !shared::common::remix_api::is_initialized())
			return;

		auto& api = shared::common::remix_api::get();
		const bool hide = !g_hidden;
		if (!api.m_bridge.SetConfigVariable ||
			// Shown again: the value is taken off the runtime's Remix API layer, so the config's own applies (an empty value).
			api.m_bridge.SetConfigVariable(kOption, hide ? "False" : "") != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			shared::common::log("GameUi", std::format("Remix refused {}", kOption), shared::common::LOG_TYPE::LOG_TYPE_WARN);
			return;
		}
		g_hidden = hide;
		shared::common::log("GameUi", hide ? "game UI hidden" : "game UI shown");
	}
}
