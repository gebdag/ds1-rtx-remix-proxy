#include "std_include.hpp"
#include "dlss_nr.hpp"

#include "shared/common/remix_api.hpp"

#include <atomic>
#include <fstream>

namespace comp::game::dlss_nr
{
	namespace
	{
		constexpr const char* kOption = "rtx.dlssNeuralRendering.enable";

		std::atomic<bool> g_toggle_requested { false };
		bool g_known = false;
		bool g_enabled = false;

		// The option's value in a Remix config file ("key = value" lines), if it sets it.
		bool read_config(const std::string& path, bool& value)
		{
			std::ifstream file(path);
			std::string line;
			bool found = false;
			while (std::getline(file, line))
			{
				const auto eq = line.find('=');
				if (eq == std::string::npos || line.find(kOption) == std::string::npos || line.find(kOption) > eq)
					continue;
				std::string v = line.substr(eq + 1);
				v.erase(0, v.find_first_not_of(" \t"));
				value = v.rfind("True", 0) == 0 || v.rfind("true", 0) == 0 || v.rfind("1", 0) == 0;
				found = true;
			}
			return found;
		}

		// user.conf is applied after rtx.conf, so it wins.
		bool starting_state()
		{
			bool value = false;
			if (read_config(shared::globals::root_path + "\\user.conf", value))
				return value;
			if (read_config(shared::globals::root_path + "\\rtx.conf", value))
				return value;
			return false;
		}
	}

	void request_toggle()
	{
		g_toggle_requested = true;
	}

	void frame_update()
	{
		if (!g_toggle_requested.exchange(false) || !shared::common::remix_api::is_initialized())
			return;
		if (!g_known)
		{
			g_enabled = starting_state();
			g_known = true;
		}

		auto& api = shared::common::remix_api::get();
		const bool enable = !g_enabled;
		if (!api.m_bridge.SetConfigVariable ||
			api.m_bridge.SetConfigVariable(kOption, enable ? "True" : "False") != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			shared::common::log("DlssNr", std::format("Remix refused {}", kOption), shared::common::LOG_TYPE::LOG_TYPE_WARN);
			return;
		}
		g_enabled = enable;
		shared::common::log("DlssNr", std::format("DLSS 3D-Guided Neural Rendering {}", enable ? "on" : "off"));
	}
}
