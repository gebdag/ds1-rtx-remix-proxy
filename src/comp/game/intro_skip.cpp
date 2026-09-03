#include "std_include.hpp"
#include "intro_skip.hpp"
#include "shared/common/config.hpp"

#include <cstring>

namespace comp::game::intro_skip
{
	namespace
	{
		// NS_FRPG::TitleStep_forWin32's STEP_NO_Logo: when the menus are ready it sets the logo dialog's flag
		// (menu man +0x10C, FrpgMenuDlgLogo clears it after its last logo) and moves on to STEP_NO_LogoWait, which
		// waits for that flag. Not setting it, the wait ends at once and the title follows the game's own steps.
		constexpr DWORD kSetLogoFlag = 0x00EE5919;
		constexpr unsigned char kSetLogoFlagBytes[] = { 0x89, 0x90, 0x0C, 0x01, 0x00, 0x00 }; // mov [eax+0x10C], edx
	}

	void init()
	{
		if (!shared::common::config::get().intro_skip.enabled)
			return;
		auto* code = reinterpret_cast<unsigned char*>(kSetLogoFlag);
		if (std::memcmp(code, kSetLogoFlagBytes, sizeof(kSetLogoFlagBytes)) != 0)
		{
			shared::common::log("IntroSkip", "the logo step is not where it should be: the logos will play",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}
		DWORD old_protect = 0;
		if (!VirtualProtect(code, sizeof(kSetLogoFlagBytes), PAGE_EXECUTE_READWRITE, &old_protect))
		{
			shared::common::log("IntroSkip", "the logo step cannot be written: the logos will play",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}
		std::memset(code, 0x90, sizeof(kSetLogoFlagBytes));
		VirtualProtect(code, sizeof(kSetLogoFlagBytes), old_protect, &old_protect);
		FlushInstructionCache(GetCurrentProcess(), code, sizeof(kSetLogoFlagBytes));
		shared::common::log("IntroSkip", "the logo dialog will not open");
	}
}
