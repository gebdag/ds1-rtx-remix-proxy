#pragma once

namespace comp::game::atmosphere
{
	// Dark Souls' atmosphere as the runtime reproduces it (rtx.ds1Haze.*, rtx.ds1grading.game*):
	//
	// Haze: every map and character pixel shader ends with the same fog and
	// sky-scattering tail, driven by per-area DrawParam values that reach it as
	// constants: PS c103 fog colour (w = fog strength), VS c128 fog start / 1 / range,
	// PS c104 extinction, c105 surface tint (w = in-scatter scale), c106 1 / extinction,
	// c107 Mie phase, c108 Rayleigh, c109 Mie, c110 sun colour (w = blend),
	// c111 sun direction (w = distance scale), c135 exposure. These are per-region
	// DrawParams, applied per draw, so Remix gets them per draw too: each distinct set
	// takes a slot (1-15), each draw is tagged with its set's slot (tag_haze_set), and
	// the runtime hazes a pixel with its surface's set (rtx.ds1Haze.sets). The single
	// set most of the frame's geometry uses (weighted by primitives, held against brief
	// challengers; atmosphere.cpp dominant_set) remains for untagged surfaces.
	//
	// Ambient: the HemEnv shaders light a surface with irradianceProbe(N) * c86 (s11)
	// plus lerp(c99, c98, N.y * 0.5 + 0.5); the probe holds the area's sky and its
	// sun. The probe is split into a sky gradient and a sun lobe along the shadow
	// light (c175): the sky plus the hemisphere ambient becomes the sky of Remix's
	// bounce rays (rtx.ds1Ambient.*), the lobe the sun's colour in HemEnv maps
	// (sun_light::set_probe_sun). Chosen like the haze: the set most probe-lit geometry uses.
	//
	// The game lights in gamma space: no lit draw reads or writes sRGB, so a light
	// value multiplies the texture's stored (gamma-encoded) colour. Remix lights in
	// linear space, so the ambient cube goes out as value^2.2.
	//
	// Grade: FRPG_Fil_HDR_ColAdj scales the frame by c56.z and applies the colour
	// matrix c62-c64 (the area's ToneCorrectBank row, built by the game).
	//
	// Values go to Remix once per frame, and only when they change.

	// A draw whose pixel shader reads the haze constants (PS_HAZE).
	void observe_haze(const float* ps_const, const float* vs_const, unsigned int frame, unsigned int prim_count);

	// Tags the next draw with the slot of its haze set (stage 7 D3DTSS_CONSTANT, 0xD15DA000 | slot),
	// or untags it when it has no haze tail (has_haze false). Every draw goes through here
	// (renderer for DIP/DP, d3d9ex for the UP draws), so no draw inherits another's tag.
	void tag_haze_set(IDirect3DDevice9* dev, bool has_haze, const float* ps_const, const float* vs_const, unsigned int frame);

	// Self-lit ambient: a few region light sets switch the probe off (c86 = 0) and give both
	// hemispheres one bright colour, so the HemEnv shaders draw their surfaces as texture * c100 *
	// that colour however the scene is lit. That is how the game makes a plain rock plane its
	// glowing lava (Tomb of the Giants, s13 LightBank row 9: (10, 0, 0)). Remix lights those
	// surfaces itself, so the draw is tagged with c100 * colour (gamma space) in stage 3's
	// D3DTSS_CONSTANT (0xD5rrggbb) and the runtime adds albedo * value^2.2 as emission
	// (Surface::ds1SelfLit), the game's own output in linear light. Every draw goes through here,
	// as through tag_haze_set; `ambient` is false for draws without the HemEnv lighting (PS_AMBIENT).
	// Lava (`lava`, its diffuse is a lava texture) is lit the same way but probe-lit: its region sets
	// give it ten times a map surface's light (Lost Izalith: c100 2.6 * (c86 3 * probe + c98 (2, 2, 0)),
	// about 13), so the game draws even its crust saturated yellow; it is tagged with
	// c100 * (c86 * the probe's upward face + c98), its own region's value.
	void tag_self_lit(IDirect3DDevice9* dev, bool ambient, const float* ps_const,
		IDirect3DBaseTexture9* probe = nullptr, bool lava = false);

	// A probe-lit draw (PS_AMBIENT) and the cube bound on its stage 11.
	void observe_ambient(const float* ps_const, IDirect3DBaseTexture9* probe, unsigned int frame, unsigned int prim_count);

	// The ColAdj draw (PS_GRADE), with the bright pass constant c11 still bound and the
	// bloom buffer on stage 1 (its size gives the game's bloom resolution).
	void observe_grade(const float* ps_const, IDirect3DDevice9* dev);

	// Light shafts: FRPG_Fil_LightShaft blurs a bright pass of the frame (threshold c71.x for the
	// sky) radially towards c69.xy, and ColAdj adds the result times c56.y. c69 is the shaft sun's
	// direction T through the camera's view-projection without the perspective divide:
	// xy = (VP * (T, 0)).xy as uv, w = its clip w, z = w - 0.05 (fitted exactly on six captures).
	// The runtime path traces the shafts from T (rtx.ds1Look.shaft*), so T is recovered from c69
	// with the frame's view-projection. T is per area and is not the lighting sun (c175).
	// Called on each LightShaft pass; the frame's first one is read.
	void observe_light_shaft(const float* ps_const, unsigned int frame);

	// Once per frame: hands the values seen since the last call to Remix when they changed.
	void frame_update(unsigned int frame);

	// The game's ambient light (gamma space: probe sky gradient plus hemisphere) on a
	// surface facing `normal`, from the last frame's ambient cube. False before one was seen.
	bool ambient_toward(const float* normal, float* out_gamma);

	// The sun as game_lights::sync_sun submits it, in gamma space: the ambient on a surface facing
	// it and its own colour. Sent as rtx.ds1Ambient.sunBase / sunLobe: lightmapped surfaces add
	// lightmap * lobe to that ambient in the game, and the runtime scales the sun on them to match
	// (lightmap_sun.hpp).
	void set_sun_gamma(const float* base, const float* lobe);
}
