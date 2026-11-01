#pragma once

namespace comp::game::estus
{
	// The game's Estus flask (the drinking effect's model, sfx s00902) is green whatever it holds, though the HUD
	// icon of a full flask is yellow. A flask with charges left is drawn yellow and glowing instead: the
	// texture rtx_comp\glass\s00902_full.dds (pbr_tools/gen_emission.py), whose own Remix hash gives it z_pbr's
	// emissive material. The game's green is what the player's empty flask shows; other characters' count as full.

	// Whether the inventory proves the local player's flask empty (in the lookdev host, what the game's frame
	// said). A failed read says not empty: yellow, the state nearly every drink is in.
	bool player_flask_empty();

	// The texture the flask's draw takes in place of the game's, or null to keep the game's (the player's empty
	// flask, or no such file). vs_const: the draw's vertex shader constants.
	IDirect3DBaseTexture9* flask_texture(IDirect3DDevice9* dev, const float* vs_const);

	// proxy.Estus.ForceEmpty: draws every flask as empty, to look at a state a capture does not record.
	void set_force_empty(bool force);
}
