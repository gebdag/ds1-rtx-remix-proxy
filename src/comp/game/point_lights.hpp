#pragma once

namespace comp::game::point_lights
{
	// One live point light as the engine holds it. Map lights, character-attached
	// lights (torches, lanterns) and FX lights (pickups, spells) all register with
	// the same manager, so harvesting it covers every dynamic light in the scene.
	// Where a light's colour and falloff came from.
	enum class param_source : unsigned char
	{
		none = 0,   // neither a bank row nor a resolvable entity — unusable
		bank_row,   // POINT_LIGHT_BANK row on the slot (map lights placed by the MSB)
		entity,     // the entity's own GetLightParams (FX/SFX lights, item lights, lerp lights)
		synthetic,  // not from the engine at all — see game::map_lights
		effect,     // stands in for an effect the engine lights only briefly — see game::effect_lights
		ffx_table,  // an effect the game draws without a light, lit from the effect table — see game::ffx_lights
	};

	struct light
	{
		int slot;                // stable identity across frames — keys the Remix light hash
		float x;                 // world position, engine raw {X, Z, Y} order
		float y;
		float z;
		float r;                 // colour, already scaled by the row's intensity percentage
		float g;
		float b;
		float dwindle_begin;     // distance where falloff starts — a good emitter radius
		float dwindle_end;       // distance where the light stops contributing
		bool has_params;         // false when neither source produced usable parameters
		param_source source;     // which of the two produced them
		unsigned entity_handle;  // DLRF handle of the owning light entity (never a pointer)
		unsigned entity;         // entity address once the handle is resolved, else 0
		unsigned entity_vtable;  // its vftable, to tell Bank / Direct / LerpBank apart
		unsigned param_row;      // resolved LIGHT_BANK row address, 0 when the slot has none
		// The FFX effect that owns an effect (Direct) light, and the outermost effect that called it;
		// 0 when unknown (fx_owner). Last, so captures from before these fields still load.
		int ffx_id;
		int ffx_top_id;
	};

	// Human-readable class of a resolved entity ("Bank", "Direct", "LerpBank", "?").
	const char* entity_class_name(unsigned vtable);

	// Resolve DLRF handles and read parameters through the entity's own
	// GetLightParams. Off leaves only bank-row lights, i.e. MSB map lights.
	void set_resolve_entities(bool on);
	bool resolve_entities();

	// Diagnostics for the Lights tab: how many slots each source produced.
	int slots_from_bank();
	int slots_from_entity();
	int slots_unresolved();

	// One line per candidate walk to the entity table with the number of live handles it
	// validated, and which walk was picked. Separates "the chain is wrong" from "this map
	// genuinely has no FX lights registered", which need opposite fixes.
	std::string chain_diagnostics();

	// Append every harvested slot, with its raw LIGHT_BANK row fields, to
	// rtx_comp/lights_dump.txt. Answers "why does this torch have no light?"
	// without another guess: a slot with no row needs entity-side parameters,
	// a row with zero colour or zero intensity is a light the engine itself
	// draws dark. Returns the file path, or an empty string if it could not
	// be written.
	std::string dump_to_log();

	// Re-walks the manager. The returned reference is valid until the next call.
	const std::vector<light>& harvest();

	// Manager reachable (false in menus and before the world loads).
	bool is_available();

	// Live slots seen on the last harvest, and how many of those resolved a row.
	int slots_live();
	int slots_with_params();
}
