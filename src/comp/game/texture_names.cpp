#include "std_include.hpp"
#include "texture_names.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace comp::game::texture_names
{
	namespace
	{
		struct entry
		{
			std::string name;
			bool atmospheric = false;
		};

		std::shared_mutex g_mutex;
		std::unordered_map<IDirect3DBaseTexture9*, entry> g_entries;
		std::atomic<uint32_t> g_named{ 0 }, g_unnamed{ 0 }, g_atmospheric{ 0 };

		// How far before the DDS to look for its TPF header. Map textures come
		// one per TPF, so their header is a few hundred bytes back; only the
		// DDS of a large multi-texture TPF (characters, objects) lies further,
		// and those never hold map layers. The cap bounds the cost of a DDS
		// that has no TPF around it at all.
		constexpr size_t kMaxScan = 1u << 20;

		// TPF (Dark Souls PC): "TPF\0", u32 data size, i32 count, u8 platform
		// (0 = PC), u8 flag2, u8 encoding (1 = UTF-16), u8 0; then per texture:
		// u32 data offset, i32 data size, u8 format, u8 type, u8 mipmaps,
		// u8 flags, u32 name offset, i32 has-float-struct, [float struct:
		// i32 unk, i32 byte length, bytes]. Offsets are from the header.
		constexpr uint32_t kTpfMagic = 0x00465054;

		struct readable_range
		{
			const uint8_t* begin;
			const uint8_t* end;

			bool contains(const uint8_t* p, size_t n) const
			{
				return p >= begin && p <= end && static_cast<size_t>(end - p) >= n;
			}
		};

		// The committed, readable region holding `p`: every byte in it can be
		// read without faulting, which bounds the backwards scan.
		std::optional<readable_range> region_of(const void* p)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
				return std::nullopt;

			constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
				PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			if (!(mbi.Protect & kReadable) || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
				return std::nullopt;

			const auto* base = static_cast<const uint8_t*>(mbi.BaseAddress);
			return readable_range{ base, base + mbi.RegionSize };
		}

		template <typename T>
		bool read(const readable_range& r, const uint8_t* p, T& out)
		{
			if (!r.contains(p, sizeof(T)))
				return false;
			memcpy(&out, p, sizeof(T));
			return true;
		}

		std::string read_name(const readable_range& r, const uint8_t* p, bool utf16)
		{
			std::string out;
			for (size_t i = 0; i < 260; i++)
			{
				if (utf16)
				{
					uint16_t c;
					if (!read(r, p + i * 2, c) || !c)
						break;
					out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
				}
				else
				{
					uint8_t c;
					if (!read(r, p + i, c) || !c)
						break;
					out.push_back(static_cast<char>(c));
				}
			}
			return out;
		}

		// The name of the texture whose DDS starts at `dds`, if `header` is
		// the TPF that holds it. The match is exact (the entry's offset and
		// size both equal what D3DX was given), so a stray "TPF\0" in other
		// data can never produce a wrong name.
		std::optional<std::string> name_from_tpf(const readable_range& r, const uint8_t* header,
			const uint8_t* dds, UINT dds_size)
		{
			int32_t count = 0;
			uint8_t platform = 0xFF, encoding = 0;
			if (!read(r, header + 8, count) || !read(r, header + 12, platform) || !read(r, header + 14, encoding))
				return std::nullopt;
			if (count < 1 || count > 4096 || platform != 0)
				return std::nullopt;

			const uint8_t* e = header + 0x10;
			for (int32_t i = 0; i < count; i++)
			{
				uint32_t offset = 0, name_offset = 0;
				int32_t size = 0, has_floats = 0;
				if (!read(r, e + 0, offset) || !read(r, e + 4, size) ||
					!read(r, e + 12, name_offset) || !read(r, e + 16, has_floats))
					return std::nullopt;
				if (has_floats != 0 && has_floats != 1)
					return std::nullopt;
				e += 0x14;
				if (has_floats)
				{
					int32_t length = 0;
					if (!read(r, e + 4, length) || length < 0 || length > 0x10000)
						return std::nullopt;
					e += 8 + length;
				}

				if (header + offset == dds && static_cast<UINT>(size) == dds_size)
				{
					auto name = read_name(r, header + name_offset, encoding == 1);
					if (name.empty())
						return std::nullopt;
					return name;
				}
			}
			return std::nullopt;
		}

		std::optional<std::string> find_name(const void* dds, UINT dds_size)
		{
			const auto* data = static_cast<const uint8_t*>(dds);
			const auto r = region_of(data);
			if (!r)
				return std::nullopt;

			const uint8_t* lowest = (data - r->begin > static_cast<ptrdiff_t>(kMaxScan)) ? data - kMaxScan : r->begin;
			// The header plus one entry sits before the first texture's data.
			const auto start = reinterpret_cast<uintptr_t>(data) - 0x24;
			for (auto* p = reinterpret_cast<const uint8_t*>(start & ~uintptr_t(3)); p >= lowest; p -= 4)
			{
				uint32_t magic;
				memcpy(&magic, p, sizeof(magic));
				if (magic == kTpfMagic)
				{
					if (auto name = name_from_tpf(*r, p, data, dds_size))
						return name;
				}
			}
			return std::nullopt;
		}

		// Lower-case file stem: "N:\...\m10_sky.tga" -> "m10_sky".
		std::string stem(std::string name)
		{
			if (const auto slash = name.find_last_of("\\/"); slash != std::string::npos)
				name.erase(0, slash + 1);
			if (const auto dot = name.find_last_of('.'); dot != std::string::npos)
				name.erase(dot);
			std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return name;
		}

		// Map textures are named "mNN_...": m10_sky, m10_01_cloud_01, m10_mist,
		// m10_bg_shaft, m11_light_shaft, m10_bg_giant_tree, m10_back_ground.
		// The "cloudshadow" textures are lit terrain overlays, not layers.
		bool classify_atmospheric(const std::string& s)
		{
			if (s.size() < 5 || s[0] != 'm' || !std::isdigit(static_cast<unsigned char>(s[1])) ||
				!std::isdigit(static_cast<unsigned char>(s[2])) || s[3] != '_')
				return false;
			if (s.find("shadow") != std::string::npos)
				return false;
			for (const char* word : { "sky", "cloud", "mist", "fog", "shaft", "_bg_", "back_ground" })
			{
				if (s.find(word) != std::string::npos)
					return true;
			}
			return false;
		}
	}

	void on_d3dx_created(IDirect3DBaseTexture9* texture, const void* dds, UINT dds_size)
	{
		if (!texture || !dds)
			return;

		auto name = find_name(dds, dds_size);
		if (!name)
		{
			const auto n = ++g_unnamed;
			if (n <= 4)
				shared::common::log("TexNames", std::format("no TPF header found for a {}-byte DDS", dds_size));
			std::unique_lock lock(g_mutex);
			g_entries.erase(texture);
			return;
		}

		entry e{ stem(*name), false };
		e.atmospheric = classify_atmospheric(e.name);
		++g_named;
		if (e.atmospheric)
		{
			++g_atmospheric;
			shared::common::log("TexNames", std::format("atmospheric layer texture: {}", e.name));
		}

		std::unique_lock lock(g_mutex);
		g_entries[texture] = std::move(e);
	}

	void on_device_created(IDirect3DBaseTexture9* texture)
	{
		if (!texture)
			return;
		std::unique_lock lock(g_mutex);
		g_entries.erase(texture);
	}

	bool is_atmospheric(IDirect3DBaseTexture9* texture)
	{
		if (!texture)
			return false;
		std::shared_lock lock(g_mutex);
		const auto it = g_entries.find(texture);
		return it != g_entries.end() && it->second.atmospheric;
	}

	std::string name_of(IDirect3DBaseTexture9* texture)
	{
		std::shared_lock lock(g_mutex);
		const auto it = g_entries.find(texture);
		return it != g_entries.end() ? it->second.name : std::string{};
	}

	stats get_stats()
	{
		return { g_named.load(), g_unnamed.load(), g_atmospheric.load() };
	}
}
