#pragma once

namespace comp::game::glass
{
	// DS1 paints a window into one texture: the leaded came (and frame) at alpha >= a cut, the glass
	// below it, its alpha the glass opacity in the game's blend. The renderer draws a listed glass
	// mesh (mesh_tables::glass_texture) twice: the came as an alpha-tested opaque surface, and the glass
	// 2 mm off it with a derived texture, the game texture with the came's alpha zeroed
	// (rtx_comp\glass\<name>.dds, written by scripts/gen_glass.py). Its own Remix hash lets the
	// ds1_glass mod make the glass translucent. Glass over an interior that was never built gets the
	// <name>_blend variant instead, which the mod leaves alone: it renders as the game's blend.

	// The derived glass texture rtx_comp\glass\<name>.dds, or null when there is none. Loaded on first use.
	IDirect3DBaseTexture9* derived_texture(IDirect3DDevice9* dev, const std::string& name);
}
