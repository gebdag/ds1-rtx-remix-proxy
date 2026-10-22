#pragma once

namespace comp::game::sun_light
{
	// Dark Souls' sun lives in two DrawParam banks and reaches the pixel shaders
	// as constants:
	//
	//   ShadowBank lightDegRotX/Y -> c175, the direction the shadow light travels.
	//     Every shadowed shader (Sdw/Csd variants, HemEnv and HemDir3 alike) takes
	//     dot(c175, N) to fade the shadow on surfaces facing away from it. This is
	//     where the game's sun shadows fall, and it can differ from LightBank's
	//     light 0 (Sen's Fortress row 0: shadow 27/-27 deg, light 0 50/-40 deg).
	//
	//   LightBank light 0..2 -> c92..c94 (directions of travel), c95..c97
	//     (colours), c98/c99 hemisphere. Only the *HemDir3 shaders read them; the
	//     HemEnv maps (Sen's Fortress among them) bake the key light into the
	//     irradiance cube (s11) and never read c92/c95.
	//
	// Each frame the draws that read these constants vote for the value bound
	// when they were drawn, weighted by primitive count:
	//   direction = c175, else c92 from a HemDir3 map draw, else from a character;
	//   colour    = c95 from a HemDir3 map draw, else from a character, else the
	//               last colour seen, else the configured fallback.

	enum class dir_source { none, shadow_c175, map_c92, chr_c92 };
	enum class colour_source { fallback, map_c95, chr_c95, probe, last_seen };

	struct light
	{
		float dir[3] = {};     // propagation direction (towards the ground), normalised
		float colour[3] = {};  // linear colour as the game's lambert term uses it
	};

	struct state
	{
		bool valid = false;         // a direction was voted for within the grace period
		light key;                  // the sun
		dir_source dir_from = dir_source::none;
		colour_source colour_from = colour_source::fallback;
		light fill[2];              // LightBank lights 1 and 2, when a HemDir3 draw was seen
		float hemi_up[3] = {};      // c98
		float hemi_down[3] = {};    // c99
		unsigned int frames_since_seen = 0;

		// Last completed frame's vote, for the F4 readout.
		unsigned int shadow_draws = 0;   // draws whose PS reads c175
		unsigned int map_dir3_draws = 0; // non-skinned draws whose PS reads c92/c95
		unsigned int chr_dir3_draws = 0; // skinned draws whose PS reads c92/c95
		int shadow_candidates = 0;       // distinct c175 values
		int dir3_candidates = 0;         // distinct c92/c95 values
	};

	// Called for every draw whose pixel shader reads c175 and/or c92+c95.
	// ps_const is the pixel shader constant shadow (256 float4 registers);
	// flags are ffp_state::PS_LIGHT_*.
	void observe(const float* ps_const, unsigned int frame, unsigned int prim_count,
		uint32_t flags, bool skinned);

	// The result of the last completed frame's vote. fallback_colour is used
	// when no draw has ever carried the key light's colour.
	const state& current(unsigned int frame, const float* fallback_colour);

	// The sun's colour as the area's irradiance probe holds it (game::atmosphere fits
	// it each frame). The HemEnv maps bake their key light into that probe and read no
	// c95, so when no map draw carries c95 this colour lights the sun, not the
	// characters' key light.
	void set_probe_sun(const float* colour);

	// The sun's propagation direction from the last completed vote, without
	// closing the current one. False while no direction is valid.
	bool key_direction(float* out_dir);

	// The live constant shadow, for the raw-register readout (null before the
	// first vote).
	const float* ps_constants();
}
