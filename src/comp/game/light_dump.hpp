#pragma once

// F6: records every frame's light data, complete and unfiltered, to rtx_comp\light_dumps\lights_<date>_<time>.log, until F6
// again. For lights that flicker (the first seconds of the intro after making a new character).
//
// Every line starts with the frame number and the milliseconds since recording started:
//   f<frame> t<ms> <SECTION> ...
// Sections:
//   FRAME     once per frame: camera world offset, player position, map
//   SLOT      the engine's point light manager, every slot with any non-zero byte: the raw 0x30 bytes, its bank row's raw
//             fields, and the entity's own GetLightParams output (point_lights::harvest, before anything is skipped)
//   HARVEST   each light point_lights::harvest returned, every field
//   SOURCE    each light game_lights went through this frame (engine, placed flames, effect stand-ins, effect table,
//             custom, mesh), every field, before any rule
//   ROW       game_lights' decision for each source: kind, state (on, or why not), key, colour, range, radiance sent
//   BIND      the positions the game was seen binding to map, character and lightmapped draws (the characters-only rule)
//   LATCH     the per-light decisions kept for the map (lightmapped / map / character), with where each was latched
//   FALLOFF   the rtx.ds1Lights.falloffTable string built this frame
//   DRAW      every draw whose pixel shader reads the game's point lights: c112-c119 raw (4 positions, 4 colours), the
//             light count, skinned, lightmapped, texture
//   API       every Remix API CreateLight (all fields of the info and its extension chain), DestroyLight,
//             DrawLightInstance and SetConfigVariable call, with its result
namespace comp::game::light_dump
{
	bool active();
	void toggle();
	// The file being written, or the last one.
	std::string path();
	unsigned long long line_count();

	// One line, prefixed with the frame and time. Thread safe. Does nothing while not recording.
	void write(const std::string& text);

	// Routes the interface's CreateLight / DestroyLight / DrawLightInstance / SetConfigVariable through the recorder (once).
	void hook_api(remixapi_Interface& api);

	// Writes the FRAME line when the frame number changed since the last one.
	void frame_header();

	// A draw whose pixel shader reads the game's point lights (renderer.cpp).
	void game_draw(const float* ps_const, int light_count, bool skinned, bool lightmapped, int owner, const void* texture, UINT prim_count);

	// While recording: a line at the top of the screen.
	void draw_overlay();
}
