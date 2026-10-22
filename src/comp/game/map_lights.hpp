#pragma once

#include "point_lights.hpp"

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

	// A tunable group. Scales multiply whatever the table row already says, so 1.0 means
	// "exactly the map's own POINT_LIGHT_BANK values".
	struct category
	{
		std::string name;
		float intensity = 1.0f;
		float radius = 1.0f;
		bool enabled = true;
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
	void set_category(size_t index, float intensity, float radius, bool on);
	void reset_categories();

	// Persist to / restore from `<game>\rtx_comp\lights\categories.txt`. Loading happens
	// automatically on first use; saving is explicit.
	bool save_categories();
	const std::string& categories_path();

	// Row name and category name of a synthesized light (slot ids from get()).
	// False for any slot that is not a synthesized light on the current map.
	bool describe(int slot, std::string& name, std::string& category);

	// Path last read for the map table, and why it produced nothing if it did.
	const std::string& source_path();
	const std::string& status();
}
