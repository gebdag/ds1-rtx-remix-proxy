#pragma once

#include "point_lights.hpp"

namespace comp::game::fx_owner
{
	// Which FFX effect owns each effect light. The engine gives a light slot no link back to its
	// owner, so the game's live effect tree is walked instead: every effect instance whose current
	// draw host is a point light (FrpgFxDrawEntityHostPointLight) carries the slot it lit in the host,
	// and its effect definition carries the FFX id. A light found this way names the effect that
	// created it (ffx_id) and the outermost effect that called it (ffx_top_id): a Firebomb's light
	// belongs to its child effect 22819, called by 12541.
	//
	// Fills ffx_id / ffx_top_id of the DirectPointLightEntity lights in place (0 when not found).
	// Owners are cached per slot and re-checked every frame; the tree is walked only when a light
	// has no valid cached owner.
	void resolve(std::vector<point_lights::light>& lights);

	// How many instances the last walk visited, and how many point-light hosts it found (Lights tab).
	int last_walk_instances();
	int last_walk_hosts();
}
