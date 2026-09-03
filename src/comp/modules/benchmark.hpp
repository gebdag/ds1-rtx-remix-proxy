#pragma once

namespace comp::benchmark
{
	// Automated option-cost sweep. start() snapshots the current option state,
	// bypasses the FPS-unlock frame cap, then steps through a phase plan:
	// all options off (baseline), each standalone option on top of the baseline,
	// each FFP sub-option on top of a bare-FFP phase, and finally the user's
	// original configuration. Each phase settles, then measures frame times.
	// Results are written to rtx_comp\benchmark_<n>.txt and the snapshot is
	// restored.
	void start();
	void cancel();
	bool is_running();

	// Called once per frame from D3D9Device::Present (after the real Present).
	void on_present();

	void tab_imgui();
}
