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
	// c111 sun direction (w = distance scale), c135 exposure. The draw with the most
	// primitives that reads them supplies the frame's values.
	//
	// Ambient: the HemEnv shaders light a surface with irradianceProbe(N) * c86 (s11)
	// plus lerp(c99, c98, N.y * 0.5 + 0.5); the probe holds the area's sky and its
	// sun. The probe is split into a sky gradient and a sun lobe along the shadow
	// light (c175): the sky plus the hemisphere ambient becomes the sky of Remix's
	// bounce rays (rtx.ds1Ambient.*), the lobe the sun's colour in HemEnv maps
	// (sun_light::set_probe_sun). The probe-lit draw with the most primitives supplies it.
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

	// A probe-lit draw (PS_AMBIENT) and the cube bound on its stage 11.
	void observe_ambient(const float* ps_const, IDirect3DBaseTexture9* probe, unsigned int frame, unsigned int prim_count);

	// The ColAdj draw (PS_GRADE).
	void observe_grade(const float* ps_const);

	// Once per frame: hands the values seen since the last call to Remix when they changed.
	void frame_update(unsigned int frame);

	// The game's ambient light (gamma space: probe sky gradient plus hemisphere) on a
	// surface facing `normal`, from the last frame's ambient cube. False before one was seen.
	bool ambient_toward(const float* normal, float* out_gamma);
}
