#pragma once

namespace comp::game::d3dx_texture_watch
{
	// DS1 creates its TPF textures through D3DX, not through the device
	// directly, so a failure there never reaches the proxy's device wrapper.
	// The engine responds to that failure with its retail assert (a store to
	// address 0), which leaves no trace of the real cause — the HRESULT these
	// two entry points return is that cause.
	void install();
}
