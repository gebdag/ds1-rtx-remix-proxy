#pragma once

#include "point_lights.hpp"
#include "shared/common/ffp_state.hpp"

namespace comp::game::effect_lights
{
	// Lights for effects the game draws bright but lights only briefly, or not at all:
	// - Bolts: the drakes' lightning breath gets one DirectPointLightEntity on the jaw for the
	//   ignition flash (colour 2.55 x the bolts' tint, dwindle 0-2 m) and none while it flows. The
	//   bolt draws (additive, the game's shared lightning textures, also lightning miracles and
	//   weapons) are grouped per effect, each group lit like the game's flash light, reaching 2 m
	//   past the bolts' spread.
	// - Additive particles (fire breath, fireballs, flames): the light their sprites add to the
	//   frame, summed per cluster, as a point light. game_lights drops a particle light that an
	//   engine or table light already stands for (a bonfire, a torch).
	// game_lights draws them as the "Effect stand-ins" kind.

	// entity_vtable of a particle stand-in (point_lights::light has no other free field).
	constexpr unsigned kParticleMarker = 0xEF000001u;

	// The renderer, for each converted rigid draw, with the game's diffuse. Only a bolt texture's draw
	// reads the blend state; every other draw pays one texture-name flag lookup.
	void observe_draw(IDirect3DDevice9* dev, IDirect3DBaseTexture9* diffuse, const float* vs_const, const float* ps_const);

	// The skinning path, for a listed mesh of a crystal lizard (sky_meshes::is_emitter_draw): its faint blue light.
	void observe_emitter(const float* vs_const);

	// The sprite observer (ffp_state::set_sprite_observer): drawn particle triangles.
	void observe_sprites(const shared::common::ffp_state::sprite_point* points, UINT count);

	// Scale on the fire stand-ins ([Lights] FireLightScale, the Lights tab).
	void set_fire_scale(float scale);

	// Once per frame, from game_lights, before collect: the engine and table lights. A particle stand-in
	// near one of them is not drawn: the game lights that fire itself (a bonfire, a torch).
	void set_covers(const std::vector<const point_lights::light*>& lights);

	// Once per frame, from game_lights: the stand-ins for the effects drawn since the last call.
	const std::vector<point_lights::light>& collect();
}
