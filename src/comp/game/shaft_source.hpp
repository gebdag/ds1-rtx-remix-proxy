#pragma once

namespace comp::game::shaft_source
{
	// The source of the game's light shaft filter (FRPG_Fil_LightShaft) is not the frame: after the
	// scene the game draws some of its sky layers again (Undead Parish: the dome m10_sky and the shaft
	// planes m10_bg_shaft, not the cloud layers) into a 512x360 buffer cleared to black and depth
	// tested against a copy of the scene's depth, and the shaft bright pass reads that buffer. Remix
	// only rasterizes that pass (its target is not the back buffer's size), so the proxy learns which
	// sky meshes it redraws and tags their main-pass draws (kMarker in place of the sky marker): the
	// runtime rasterizes those into its shaft source as well as its sky (rtx.ds1Look shafts).

	// Stage 0's D3DTSS_CONSTANT on a sky draw that is also a light shaft source.
	constexpr DWORD kMarker = 0xD15D05C2;

	// For a sky layer draw (indexed): records it when it goes to the game's shaft buffer, and says
	// whether a main-pass draw of it is a shaft source (redrawn there within the last second).
	bool observe(IDirect3DDevice9* dev, IDirect3DIndexBuffer9* ib, UINT start_index, UINT prim_count,
		IDirect3DBaseTexture9* texture, unsigned int frame);
}
