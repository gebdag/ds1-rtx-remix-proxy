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
	// proxy's own draws included (its ImGui overlays excepted, see private_scope).
	// Outside a capture every hook passes straight through after one flag test.
	//
	// The freeze key (F8) also lives here: while shared::globals::input_frozen is
	// set the game gets no keyboard, mouse or gamepad input, so a camera lined up
	// for a capture or the live host stays put while the desktop is used.
	class lookdev_capture final : public shared::common::loader::component_module
	{
	public:
		lookdev_capture();
		~lookdev_capture();

		// Device calls made while one of these exists are not recorded: the proxy's
		// ImGui overlays drawn into the game's frame. Harmless outside lookdev mode.
		struct private_scope
		{
			private_scope();
			~private_scope();
		private:
			bool prev_;
		};

		// True while the frame being drawn goes into a capture file, whose
		// reference image must show only what the game and proxy rendered.
		static bool writing_file();

		// True for a few seconds after a capture file was written; name = its name.
		static bool recent_capture(std::string& name);

		// Before the game's device Reset: a stream session, capture or snap holds the
		// back buffer or DEFAULT-pool objects, and real D3D9 fails Reset while any are
		// alive (lookdev renders on the real device, which a fullscreen focus loss
		// makes lost). The stream resumes as a new session after the next Present.
		static void on_reset();
	};
}
