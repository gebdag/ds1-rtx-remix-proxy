#include "std_include.hpp"
#include "shaft_source.hpp"

#include <unordered_map>

namespace comp::game::shaft_source
{
	namespace
	{
		// A sky layer draw: its index range and diffuse.
		struct key
		{
			IDirect3DIndexBuffer9* ib;
			UINT start_index;
			UINT prim_count;
			IDirect3DBaseTexture9* texture;
			bool operator==(const key& o) const
			{
				return ib == o.ib && start_index == o.start_index && prim_count == o.prim_count && texture == o.texture;
			}
		};

		struct key_hash
		{
			size_t operator()(const key& k) const
			{
				size_t h = std::hash<const void*>()(k.ib);
				h ^= std::hash<UINT>()(k.start_index) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<UINT>()(k.prim_count) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<const void*>()(k.texture) + 0x9e3779b9 + (h << 6) + (h >> 2);
				return h;
			}
		};

		// Each redrawn layer and the last frame it went to the shaft buffer.
		std::unordered_map<key, unsigned int, key_hash> g_redrawn;
		// A layer redrawn within this many frames is a shaft source (areas switch the filter on and off).
		constexpr unsigned int kHoldFrames = 60;

		// The back buffer's size: the main pass draws there, the shaft pass into a smaller target.
		bool draws_to_back_buffer_size(IDirect3DDevice9* dev)
		{
			IDirect3DSurface9* rt = nullptr;
			if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt)
				return true;
			D3DSURFACE_DESC rt_desc = {};
			rt->GetDesc(&rt_desc);
			rt->Release();

			IDirect3DSurface9* bb = nullptr;
			if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
				return true;
			D3DSURFACE_DESC bb_desc = {};
			bb->GetDesc(&bb_desc);
			bb->Release();
			return rt_desc.Width == bb_desc.Width && rt_desc.Height == bb_desc.Height;
		}
	}

	bool observe(IDirect3DDevice9* dev, IDirect3DIndexBuffer9* ib, UINT start_index, UINT prim_count,
		IDirect3DBaseTexture9* texture, unsigned int frame)
	{
		const key k { ib, start_index, prim_count, texture };
		if (!draws_to_back_buffer_size(dev))
		{
			if (g_redrawn.size() > 256)
				std::erase_if(g_redrawn, [frame](const auto& e) { return frame - e.second > kHoldFrames; });
			g_redrawn[k] = frame;
			return false;
		}
		const auto it = g_redrawn.find(k);
		return it != g_redrawn.end() && frame - it->second <= kHoldFrames;
	}
}
