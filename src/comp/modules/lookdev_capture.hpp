#pragma once

namespace comp
{
	// Lookdev frame capture. Registered only in lookdev mode ([Lookdev] Enabled=1,
	// which lookdev\lookdev.ps1 writes into remix-comp-proxy.lookdev.ini before it
	// starts the game with -lookdev; the normal INI never has it).
	//
	// F9, or a rtx_comp\lookdev\capture.request file, records the next presented
	// frame into rtx_comp\lookdev\<name>.ds1cap along with the image the game
	// presented; the lookdev viewer replays the file (see capture_format.hpp).
	//
	// The recording hooks sit on the REAL device and its buffers, below this
	// proxy's wrapper, so the file holds exactly what the GPU was given, the
	// proxy's own draws included. Outside a capture every hook passes straight
	// through after one flag test.
	class lookdev_capture final : public shared::common::loader::component_module
	{
	public:
		lookdev_capture();
		~lookdev_capture();
	};
}
