#pragma once

namespace comp::game::texture_names
{
	// DS1 creates every TPF texture with D3DXCreateTextureFromFileInMemoryEx,
	// handing D3DX a pointer into the TPF file it just read. The TPF header
	// that owns that DDS carries the texture's name, so the name can be
	// recovered from memory at creation time without any engine hook.
	//
	// Called by d3dx_texture_watch after a successful D3DX create.
	void on_d3dx_created(IDirect3DBaseTexture9* texture, const void* dds, UINT dds_size);

	// Called by the device wrapper after CreateTexture: a texture the game
	// creates directly (render targets, dynamic textures) can reuse the
	// address of a freed named texture, so its stale entry must go.
	void on_device_created(IDirect3DBaseTexture9* texture);

	// True when the texture is a map atmospheric or backdrop layer: its name is
	// a map texture ("mNN_...") containing sky, cloud, mist, fog, shaft,
	// "_bg_" or "back_ground" (cloud shadows on terrain excluded). These are
	// free-standing layers the game blends over whatever lies behind them, not
	// overlays on a surface.
	bool is_atmospheric(IDirect3DBaseTexture9* texture);

	// True for an effect texture the game lights only briefly (the lightning breath's bolts), which
	// game::effect_lights stands a light in for.
	bool is_lit_effect(IDirect3DBaseTexture9* texture);
	// Whether a skinned draw with this diffuse can be part of the player: an armour or body part (hd_, bd_, am_, lg_,
	// fg_, hr_) or an unnamed texture. Many of the parts' textures have no name: their DDS lies too deep in a large
	// multi-texture TPF to find its header (legs and gauntlets among them). Enemies (c####), effects (s####) and
	// objects (o####) do have names.
	bool is_body_part(IDirect3DBaseTexture9* texture);

	// What a draw with this diffuse is to the runtime's ripple field, in one lookup. kRippleCharacter: a character's or its
	// equipment's (owner::character). kRippleEnemy: an enemy's own (c####, but not c0000, the player's model), a character
	// that is certainly not the player; characters built from the player's parts have no such texture. kRippleWaterfall:
	// a waterfall mesh's diffuse (*waterfall*, m13_n_water_*), the falling sheets and the flat pools that share the texture.
	constexpr uint32_t kRippleCharacter = 1, kRippleEnemy = 2, kRippleWaterfall = 4;
	uint32_t ripple_class(IDirect3DBaseTexture9* texture);

	// Changes whenever a texture is named, renamed or forgotten: what a caller remembered about a texture pointer holds
	// only as long as this does.
	uint32_t generation();

	// True for the Estus flask's diffuse (the drinking effect's model), see game/estus.hpp.
	bool is_estus_flask(IDirect3DBaseTexture9* texture);

	// True for the Moonlight Butterfly's diffuse (c3230): parts of its wings are two single-sided faces whose
	// fronts face each other: the renderer tags its skinned draws with the thin-wall marker (renderer.cpp, thin_wall_skin).
	bool is_butterfly_wing(IDirect3DBaseTexture9* texture);

	// A light shaft drawn as an alpha-blended map mesh the game lights by the area's ambient (m14_light by the Bed of
	// Chaos): a self-lit layer in Remix (atmosphere tag_self_lit), not a decal, which hangs in the air with nothing behind.
	bool is_light_shaft(IDirect3DBaseTexture9* texture);

	// True for a lava material's diffuse (mNN_n_lava_*), which atmosphere::tag_self_lit draws glowing.
	bool is_lava(IDirect3DBaseTexture9* texture);

	// True for a plant whose alpha-tested leaf cards the renderer tags with the foliage marker (in Firelink only): indirect
	// light passes them in part (rtx.ds1Foliage.transmission). Only the plants seen wrong on a capture: Firelink's spiky rosettes, m10_grass_02
	// and m10_grass_03 (black hearts and hard leaf shadows, frame_20261107_083232 and 083353).
	bool is_foliage(IDirect3DBaseTexture9* texture);

	// Whose texture it is, by name: a character's or its equipment's (c#### bodies, wp_ weapons and
	// shields, bd_/hd_/lg_/am_ armour, fg_ faces), anything else named (the map and its objects), or
	// unknown (no name recovered).
	enum class owner : unsigned char { unknown, map, character };
	owner owner_of(IDirect3DBaseTexture9* texture);

	// Name recovered for the texture, or empty.
	std::string name_of(IDirect3DBaseTexture9* texture);

	// Names a texture directly (the lookdev host, from the capture's name records).
	void set_name(IDirect3DBaseTexture9* texture, const std::string& name);

	struct stats
	{
		uint32_t named = 0;     // D3DX textures whose TPF name was recovered
		uint32_t unnamed = 0;   // D3DX textures with no TPF header found around the DDS
		uint32_t atmospheric = 0;  // named textures classified as atmospheric layers
	};
	stats get_stats();
}
