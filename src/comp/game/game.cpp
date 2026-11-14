#include "std_include.hpp"
#include "cutscenes.hpp"
#include "shared/common/flags.hpp"
#include "shared/common/config.hpp"
#include "camera.hpp"
#include "cheats.hpp"
#include "debug_tools.hpp"
#include "fps_unlock.hpp"
#include "free_cam.hpp"
#include "input_customizer.hpp"
#include "intro_skip.hpp"

namespace comp::game
{
	// --------------
	// game variables

	//DWORD* d3d_dev_sample_addr = nullptr;
	some_struct_containing_matrices* vp = nullptr; // example

	// --------------
	// game functions

	// SampleTemplate_t SampleTemplate = nullptr;


	// --------------
	// game asm offsets

	//uint32_t retn_addr__func1 = 0u;
	//uint32_t nop_addr__func2 = 0u;
	//uint32_t retn_addr__pre_draw_something = 0u;
	//uint32_t hk_addr__post_draw_something = 0u;


	// --------------

#define PATTERN_OFFSET_SIMPLE(var, pattern, byte_offset, static_addr) \
		if (const auto offset = shared::utils::mem::find_pattern(##pattern, byte_offset, #var, use_pattern, static_addr); offset) { \
			(var) = offset; found_pattern_count++; \
		} total_pattern_count++;

#define PATTERN_OFFSET_DWORD_PTR_CAST_TYPE(var, type, pattern, byte_offset, static_addr) \
		if (const auto offset = shared::utils::mem::find_pattern(##pattern, byte_offset, #var, use_pattern, static_addr); offset) { \
			(var) = (type)*(DWORD*)offset; found_pattern_count++; \
		} total_pattern_count++;

	// init any adresses here
	void init_game_addresses()
	{
		
		const bool use_pattern = !shared::common::flags::has_flag("no_pattern");
		if (use_pattern) {
			shared::common::log("Game", "Getting offsets ...", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		}

		std::uint32_t total_pattern_count = 0u;
		std::uint32_t found_pattern_count = 0u;


#pragma region GAME_VARIABLES

		// Find code that references the global var you are interested in, grab the address of the instruction + pattern
		// Figure out the byte offset that's needed until your global var address starts in the instruction 
		// -> 'mov eax, d3d_dev_sample_addr' == A1 D8 D8 7E 01 where A1 is the mov instruction and the following 4 bytes the addr of the global var -> so offset 1
		
		// Patterns are quite slow on DEBUG builds. The last argument in find_pattern allows you to declare a static offset which will be used
		// when the game gets started with `-no_pattern` in the commandline

		// ----

		// Example verbose
			//if (const auto offset = shared::utils::mem::find_pattern("? ? ? ? ?", 1, "d3d_dev_sample_addr", use_pattern, 0xDEADBEEF); offset) {
			//	d3d_dev_sample_addr = (DWORD*)*(DWORD*)offset; found_pattern_count++; // cast mem at offset
			//} total_pattern_count++;

		// Or via macro
			//PATTERN_OFFSET_DWORD_PTR_CAST_TYPE(d3d_dev_sample_addr, DWORD*, "? ? ? ? ?", 1, 0xDEADBEEF);


		// Another example with a structure object
			//PATTERN_OFFSET_DWORD_PTR_CAST_TYPE(vp, some_struct_containing_matrices*, "? ? ? ? ?", 0, 0xDEADBEEF);

		// end GAME_VARIABLES
#pragma endregion

		// ---


#pragma region GAME_FUNCTIONS

		// cast func template
		//PATTERN_OFFSET_DWORD_PTR_CAST_TYPE(SampleTemplate, SampleTemplate_t, "? ? ? ? ?", 0, 0xDEADBEEF);

		// end GAME_FUNCTIONS
#pragma endregion

		// ---


#pragma region GAME_ASM_OFFSETS

		// Assembly offsets are simple offsets that do not require additional casting

		// Example verbose
			//if (const auto offset = shared::utils::mem::find_pattern(" ? ? ? ", 0, "nop_addr__func2", use_pattern, 0xDEADBEEF); offset) {
			//	nop_addr__func2 = offset; found_pattern_count++;
			//} total_pattern_count++;

		// Or via macro
			//PATTERN_OFFSET_SIMPLE(retn_addr__pre_draw_something, "? ? ? ?", 0, 0xDEADBEEF);
			//PATTERN_OFFSET_SIMPLE(hk_addr__post_draw_something, "? ? ? ?", 0, 0xDEADBEEF); 

		// end GAME_ASM_OFFSETS
#pragma endregion


		if (use_pattern)
		{
			if (found_pattern_count == total_pattern_count) {
				shared::common::log("Game", std::format("Found all '{:d}' Patterns.", total_pattern_count));
			}
			else
			{
				shared::common::log("Game", std::format("Only found '{:d}' out of '{:d}' Patterns.", found_pattern_count, total_pattern_count), shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
				shared::common::log("Game", ">> Please create an issue on GitHub and attach this console log and information about your game (version, platform etc.)\n", shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
		}

		fps_unlock::init();
		intro_skip::init();
		camera::init();
		debug_tools::init();
		// The cheats patch the game's code (the god mode cave, the fall damage call): dev mode only, so a player's game runs
		// without them. debug_tools stays: it only finds addresses, and applies the framerate-disconnect fix.
		if (shared::common::config::get().dev.enabled)
			cheats::init();
		free_cam::init(); // dev mode only (checked inside)
		cutscenes::init();
		input_customizer::init();
	}

#undef PATTERN_OFFSET_SIMPLE

}
