#pragma once

namespace comp::game::lightmap_sun
{
	// Dark Souls' lightmapped map shaders (every FRPG_* probe-lit pixel shader that samples s6, the
	// "Lit" families) multiply the irradiance probe's light by the lightmap:
	//   light = probe(N) * c86 * lightmap + lerp(c99, c98, N.y * 0.5 + 0.5)
	// (LitSdw: min(lightmap, 1 - shadow)). The probe holds the area's sun (atmosphere.hpp), so the
	// lightmap decides how much of it a map surface gets. Darkroot's lightmaps sit near 0.2: the
	// game shows almost no sun there, while trees and other unlightmapped geometry get all of it.
	//
	// Remix lights map surfaces with the proxy's sun (the probe's lobe as a distant light), so each
	// lightmapped draw is tagged with its lightmap's level and the runtime scales the sun on the
	// draw's surfaces by it (Surface::ds1SunLightmap, rtx.ds1Ambient.sun*). The level is, per
	// channel, the 90th percentile of the lightmap at the draw's triangles weighted by their area:
	// the level of its unshadowed parts. Ray-traced shadows give the shadowed ones.
	//
	// The lightmap uv is TEXCOORD0.zw (TEXCOORD1.xy in the two-uv PINTT_DDL family) / 1024 + VS c248.xy,
	// as FRPG_Phn_*_DL*.vpo computes it.

	// Stage 6's D3DTSS_CONSTANT: kMarker | r << 16 | g << 8 | b (the level, 0-255), 0 = not lightmapped.
	constexpr DWORD kMarker = 0xD1000000;
	// The same constant on a skinned draw (characters, never lightmapped): the runtime's player light
	// clearance ray passes through characters, the player's own body and gear among them
	// (rtx_dark_souls_light_falloff.h in the runtime).
	constexpr DWORD kSkinnedMarker = 0xD2000000;
	// A skinned draw the game alpha-blends (over INVSRCALPHA) and alpha-tests with an unbumped pixel
	// shader: DS1's hair cards, which the runtime renders as hair cards (cutouts at full texture
	// detail) instead of stacked blends.
	constexpr DWORD kSkinnedCardMarker = 0xD2000001;
	// A lightmap level of nothing: no sun on this draw (Surface::ds1SunLightmap).
	constexpr DWORD kNoSunMarker = 0xD1000000;
	// A draw of a body part: its vertices carry their bind position in their colour, and it gets wet
	// (skinning::expand_skin_vertex, rtx_dark_souls_ripples.h in the runtime).
	constexpr DWORD kWetBodyMarker = 0xD2000002;
	// The same, for what armour covers (the head under a helmet, the bare body under a chest piece): it gets wet too,
	// but water does not reach it, so nothing bounces or drips off it.
	constexpr DWORD kWetBodyCoveredMarker = 0xD2000003;
	// A rigid part of a character, drawn on bones without weights: a weapon, a shield. Not a skinned draw to the
	// runtime, but it moves water like the body does (the ripple field's map pass, ds1_ripple_map in the runtime).
	constexpr DWORD kRigidPartMarker = 0xD2000010;
	// Added to any of the 0xD2 tags: the draw is another character's than the player's, an enemy with textures of
	// its own (c####). The runtime leaves those out of the ripples with rtx.ds1Ripple.enemies off. Characters built
	// from the player's parts (human NPCs, phantoms) cannot be told from the player here and carry no bit.
	constexpr DWORD kOtherCharacterBit = 0x00000020;
	// A waterfall's mesh (texture_names::kRippleWaterfall), no character: where its falling sheet meets water, the runtime's
	// ripple field stirs the water (rtx.ds1Ripple.fallStrength).
	constexpr DWORD kFallingWaterMarker = 0xD2000040;

	// Tags the next indexed draw: its lightmap level when its pixel shader is lightmapped,
	// kSkinnedMarker / kSkinnedCardMarker when it is skinned, kRigidPartMarker on a character's rigid part, else none.
	// Levels are worked out once per draw (vertex, index and lightmap data are read back on the CPU,
	// a few dozen draws per frame at most) and cached by the draw's buffers, range and lightmap.
	void tag_draw(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, INT base_vertex, UINT start_index,
		UINT prim_count, unsigned int frame);

	// Untags the next draw (non-indexed and UP draws: no map geometry).
	void untag(IDirect3DDevice9* dev);
}
