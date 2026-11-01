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

	// True for a lava material's diffuse (mNN_n_lava_*), which atmosphere::tag_self_lit draws glowing.
	bool is_lava(IDirect3DBaseTexture9* texture);

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
