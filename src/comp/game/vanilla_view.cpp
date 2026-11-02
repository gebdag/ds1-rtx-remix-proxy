#include "std_include.hpp"
#include "vanilla_view.hpp"

#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/remix_api.hpp"

#include <atomic>

namespace comp::game::vanilla_view
{
	namespace
	{
		std::atomic<bool> g_toggle_requested { false };
		bool g_active = false;

		// What vanilla mode turned off, restored when it ends.
		bool g_saved_ffp = true;
		bool g_saved_skip_postfx = true;
		bool g_saved_suppress_aux = true;
		bool g_saved_drop_screen_effects = true;

		bool set_raytracing(bool on)
		{
			if (!shared::common::remix_api::is_initialized())
				return false;
			auto& api = shared::common::remix_api::get();
			return api.m_bridge.SetConfigVariable &&
				api.m_bridge.SetConfigVariable("rtx.enableRaytracing", on ? "True" : "False") == REMIXAPI_ERROR_CODE_SUCCESS;
		}
	}

	bool active()
	{
		return g_active;
	}

	void request_toggle()
	{
		g_toggle_requested = true;
	}

	void frame_update()
	{
		if (!g_toggle_requested.exchange(false))
			return;

		auto& ffp = shared::common::ffp_state::get();
		auto& cfg = shared::common::config::get();
		if (!g_active)
		{
			if (!set_raytracing(false))
			{
				shared::common::log("Vanilla", "Remix refused rtx.enableRaytracing; staying in Remix", shared::common::LOG_TYPE::LOG_TYPE_WARN);
				return;
			}
			g_saved_ffp = ffp.is_enabled();
			g_saved_skip_postfx = cfg.ffp.skip_postfx;
			g_saved_suppress_aux = cfg.ffp.suppress_aux_passes;
			g_saved_drop_screen_effects = cfg.sfx.drop_screen_effects;
			ffp.set_enabled(false);
			cfg.ffp.skip_postfx = false;
			cfg.ffp.suppress_aux_passes = false;
			cfg.sfx.drop_screen_effects = false;
			g_active = true;
			shared::common::log("Vanilla", "vanilla graphics on");
		}
		else
		{
			ffp.set_enabled(g_saved_ffp);
			cfg.ffp.skip_postfx = g_saved_skip_postfx;
			cfg.ffp.suppress_aux_passes = g_saved_suppress_aux;
			cfg.sfx.drop_screen_effects = g_saved_drop_screen_effects;
			set_raytracing(true);
			g_active = false;
			shared::common::log("Vanilla", "vanilla graphics off (Remix)");
		}
	}
}
