#pragma once

namespace comp::game::debug_tools
{
	// F4 > Debug: the game's own debug flags, a few small code patches and player / world fields,
	// for testing without interruption and for bringing out rare looks. Every address is found by
	// AOB at init, and every
	// control reads the game's value back each frame, so it shows the game's state.
	void init();
	void tab_imgui();

	// The game's debug flag block (one byte per flag, 1 = on); false / no-op when it was not found.
	bool flags_available();
	bool read_flag(unsigned offset);
	bool write_flag(unsigned offset, bool on);
	// A checkbox on one flag byte, read back every frame.
	void flag_checkbox(unsigned offset, const char* name, const char* tip);

	// Debug flag offsets the Cheats tab uses.
	constexpr unsigned kFlagPlayerHide = 0x15;     // enemies do not see the player
	constexpr unsigned kFlagPlayerSilence = 0x16;  // enemies do not hear the player
	constexpr unsigned kFlagAllNoUpdateAi = 0x1C;  // every AI character stands still

	// One atomic store of a few code bytes, for code the game thread may be running: false if they cross an aligned qword.
	bool write_code(DWORD addr, const BYTE* bytes, size_t size);

	// The game's WorldChrMan static (its +0x3C is the local player's ChrIns), 0 if not found.
	unsigned world_chr_man();
}
