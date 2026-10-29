#pragma once

#include "point_lights.hpp"

namespace comp::game::effect_lights
{
	// Lights for effects the game draws bright but lights only briefly. The Valley of Drakes'
	// lightning breath gets one DirectPointLightEntity on the drake's jaw for the ignition flash
	// (colour 2.55 x the bolts' tint, dwindle 0-2 m) and none while the breath keeps flowing, so
	// under Remix the sustained bolts lit nothing. Each frame the bolt draws (additive, the game's
	// shared lightning-bolt textures, also used by lightning miracles and weapons) are collected
	// here and grouped by effect, and one light stands in for each group: at the bolts' origins
	// weighted by their fade (PS c139.w), coloured like the game's flash light, reaching the flash's
	// 2 m past the bolts' spread. game_lights draws them as the "Effect stand-ins" kind.

	// The renderer, for each converted rigid draw, with the game's diffuse. Only a bolt texture's draw
	// reads the blend state; every other draw pays one texture-name flag lookup.
	void observe_draw(IDirect3DDevice9* dev, IDirect3DBaseTexture9* diffuse, const float* vs_const, const float* ps_const);

	// Once per frame, from game_lights: the stand-ins for the effects drawn since the last call.
	const std::vector<point_lights::light>& collect();
}
