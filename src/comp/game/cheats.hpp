#pragma once

namespace comp::game::cheats
{
	void init();

	bool is_god_mode_available();
	bool& god_mode_enabled();

	void tab_imgui();
}
