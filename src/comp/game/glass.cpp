#include "std_include.hpp"
#include "glass.hpp"

#include <mutex>
#include <unordered_map>

namespace comp::game::glass
{
	namespace
	{
		std::mutex g_mutex;
		IDirect3DDevice9* g_device = nullptr;
		std::unordered_map<std::string, IDirect3DTexture9*> g_textures;  // texture name -> derived (null: none)

		// A BC3 DDS as written by gen_glass.py (the game's own DDS with alpha blocks changed): the
		// mips are created as the game's D3DX would, so Remix hashes mip 0 exactly as the script did.
		IDirect3DTexture9* load(IDirect3DDevice9* dev, const std::string& name)
		{
			const auto path = shared::globals::root_path + "\\rtx_comp\\glass\\" + name + ".dds";
			std::ifstream file(path, std::ios::binary);
			if (!file)
				return nullptr;
			const std::vector<uint8_t> dds((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

			uint32_t magic = 0, height = 0, width = 0, mips = 0, fourcc = 0;
			if (dds.size() < 128)
				return nullptr;
			std::memcpy(&magic, &dds[0], 4);
			std::memcpy(&height, &dds[12], 4);
			std::memcpy(&width, &dds[16], 4);
			std::memcpy(&mips, &dds[28], 4);
			std::memcpy(&fourcc, &dds[84], 4);
			if (magic != MAKEFOURCC('D', 'D', 'S', ' ') || fourcc != MAKEFOURCC('D', 'X', 'T', '5') ||
				!width || !height || width > 8192 || height > 8192)
				return nullptr;
			mips = std::max(1u, mips);

			IDirect3DTexture9* texture = nullptr;
			if (FAILED(dev->CreateTexture(width, height, mips, 0, D3DFMT_DXT5, D3DPOOL_MANAGED, &texture, nullptr)))
				return nullptr;

			size_t offset = 128;
			for (UINT level = 0; level < mips; level++)
			{
				const UINT bw = (std::max(1u, width >> level) + 3) / 4, bh = (std::max(1u, height >> level) + 3) / 4;
				const size_t row = size_t(bw) * 16;
				D3DLOCKED_RECT rect{};
				if (offset + row * bh > dds.size() || FAILED(texture->LockRect(level, &rect, nullptr, 0)))
				{
					texture->Release();
					return nullptr;
				}
				for (UINT y = 0; y < bh; y++)
					std::memcpy(static_cast<uint8_t*>(rect.pBits) + size_t(y) * rect.Pitch, &dds[offset + y * row], row);
				texture->UnlockRect(level);
				offset += row * bh;
			}
			return texture;
		}
	}

	IDirect3DBaseTexture9* derived_texture(IDirect3DDevice9* dev, const std::string& name)
	{
		if (!dev || name.empty())
			return nullptr;

		std::lock_guard lock(g_mutex);
		// Textures belong to the device that made them. Should the device ever change, the old
		// one's textures cannot be used or safely released, so they are dropped.
		if (dev != g_device)
		{
			g_device = dev;
			g_textures.clear();
		}
		const auto it = g_textures.find(name);
		if (it != g_textures.end())
			return it->second;

		auto* texture = load(dev, name);
		shared::common::log("Glass", texture
			? std::format("derived glass texture loaded: {}", name)
			: std::format("no derived glass texture for {} (rtx_comp\\glass\\{}.dds) - drawn as the game does", name, name),
			texture ? shared::common::LOG_TYPE::LOG_TYPE_DEFAULT : shared::common::LOG_TYPE::LOG_TYPE_ERROR, !texture);
		g_textures.emplace(name, texture);
		return texture;
	}
}
