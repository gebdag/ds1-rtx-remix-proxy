#pragma once

#include "point_lights.hpp"
#include "shared/common/ffp_state.hpp"

namespace comp::game::map_lights
{
	// Stand-in point lights for flames the engine never lights.
	//
	// Blighttown places 99 torch effects and no MSB Light events, and the torch effect
	// declares no light node -- measured live, `KPointLightMan` holds two slots there and
	// both belong to the bonfire. So there is nothing to harvest and the lights come from
	// the shipped placement data instead: one line per flame SFX region origin, coloured
	// by that map's own POINT_LIGHT_BANK row.
	//
	// Tables live in `<game>\rtx_comp\lights\<map>.txt` and are re-read on map change.
	// Every light names a category (the effect it came from), and categories are global,
	// so tuning "torch_small" moves every torch in the game at once.
	//
	// Some effects only exist part of the time: Tomb of the Giants' 49 lantern-reactive
	// halos (FFX 540) are created one after another while the Skull Lantern is raised and
	// their emitters removed when it drops (m13_01 event 11315150). A category marked
	// "follows effect" scales each of its lights by how opaque its effect's sprites are drawn
	// at its spot (observe_sprites): the strongest alpha there, relative to the effect's full
	// alpha, so the light fades in, pulses and fades out with the halo. The game draws no
	// sprite off screen, so a light out of view keeps its last level until it is back in view.

	// A tunable group. Scales multiply whatever the table row already says, so 1.0 means
	// "exactly the map's own POINT_LIGHT_BANK values".
	struct category
	{
		std::string name;
		float intensity = 1.0f;
		float radius = 1.0f;
		bool enabled = true;
		bool follows_effect = false;  // lit only while its effect's sprites are drawn at the light
		float effect_alpha = 1.0f;    // the effect's sprite alpha at full strength (follows effect)
		float effect_alpha_seen = 0.0f; // the highest sprite alpha seen at its lights this session
		int count = 0;          // lights in this category on the current map
	};

	// Packed current-map id (byte 3 = area, byte 2 = block), 0 with no world loaded.
	unsigned current_map_id();

	// "m14_00_00_00" for the current map, empty when none is loaded.
	std::string current_map_name();

	// Lights for the current map with category scaling applied, reloading the table if
	// the map changed. Shares point_lights::light so the injector treats them like any
	// other light.
	const std::vector<point_lights::light>& get();

	void set_enabled(bool on);
	bool enabled();

	// Force a re-read of the map table on the next get(), for editing it live.
	void reload();

	// Categories seen so far, ordered as encountered. Mutate through set_category to
	// keep the emitted lights in step.
	const std::vector<category>& categories();
	void set_category(size_t index, float intensity, float radius, bool on, bool follows_effect);
	void reset_categories();
	// A category as categories.txt has it (when it was last read or saved), nullptr when the file has none.
	const category* saved_category(const std::string& name);

	// The sprite observer (ffp_state::set_sprite_observer): where effects were drawn, how opaque.
	void observe_sprites(const shared::common::ffp_state::sprite_point* points, UINT count);
	// Whether any light on this map follows its effect (else the observer is not needed).
	bool wants_sprites();

	// Once per frame before the lights are read: sets each follows-effect light's level from the
	// sprites drawn at it since the last call, and whether the game would have drawn its effect
	// (the spot inside the map camera's view).
	void update_effect_gate();

	// A light's brightness factor from its effect (0-1): 1 for lights that do not follow one.
	float effect_level(int slot);
	// The strongest sprite alpha drawn at a light in the last frame (the level's input).
	float effect_sprite_alpha(int slot);

	// Persist to / restore from `<game>\rtx_comp\lights\categories.txt`. Loading happens
	// automatically on first use; saving is explicit.
	bool save_categories();
	const std::string& categories_path();

	// Row name and category name of a synthesized light (slot ids from get()).
	// False for any slot that is not a synthesized light on the current map.
	bool describe(int slot, std::string& name, std::string& category);

	// Whether this map places a flame effect at this spot (rtx_comp\map_flames.tsv, the MSB's SFX events).
	bool is_flame(float x, float y, float z);

	// Path last read for the map table, and why it produced nothing if it did.
	const std::string& source_path();
	const std::string& status();
}
