#pragma once

namespace comp::game::tex_override
{
	// DSfix-style texture packs: DATA\tex_override\<hash>.png or .dds (or any format D3DX reads) replaces the texture whose
	// source file hashes to <hash>, the 8 hex digits of SuperFastHash over the data the game hands D3DX. An empty or
	// missing folder costs nothing.
	void init();

	// The replacement file for this source data, read into `file`. False when there is none.
	bool read_replacement(const void* src, UINT src_size, std::vector<char>& file);

	// D3DX could not make a texture from a replacement: says so once, and the game keeps its own.
	void note_failed(const void* src, UINT src_size, HRESULT hr);
}
