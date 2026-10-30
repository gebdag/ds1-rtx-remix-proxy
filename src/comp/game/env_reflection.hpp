#pragma once

namespace comp::game::env_reflection
{
	// How strongly the game reflects a draw's surroundings. Its Phong shaders add
	//   _s * c101 * (EnvSpc cube s12(R) * c87 + point light highlights)
	// to a diffuse lit by the EnvDif probe s11(N) * c86, and the EnvSpc cube is the area's own: near black
	// in the Undead Asylum (0.005 against a probe of 0.37), bright elsewhere. The z_pbr mod's specular colour
	// scale is 0.5 * c101 / c100 times one typical cube-to-probe level per MTD slot (ptde_specular.py), so
	// this draw's weight on that scale is
	//   0.5 * (c101 / c100) * (c87 * mean(s12)) / (c86 * mean(s11)),
	// cube means decoded as the shader reads them (rgb / a). The runtime scales the reflections of the
	// draw's specular colour material by weight / scale (Surface::ds1EnvReflection, rtx.ds1Specular.envReflectionWeight).
	//
	// Tags the next converted draw with stage 1's D3DTSS_CONSTANT = 0xD7000000 | half(weight) when its pixel
	// shader samples both cubes, and clears it otherwise. Water draws use the same constant for their wave
	// strengths (texture_scroll), set after this and cleared by texture_scroll::clear, which calls forget().
	void tag(IDirect3DDevice9* dev, bool applies, const float* ps_const);

	// The device's stage 1 constant was changed behind tag's back.
	void forget();
}
