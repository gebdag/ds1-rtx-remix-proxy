#include "std_include.hpp"
#include "sky_meshes.hpp"

#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace comp::game::sky_meshes
{
	namespace
	{
		enum class kind : uint8_t { none, sky, shell };

		struct range_key
		{
			UINT start;
			UINT count;
			bool operator==(const range_key&) const = default;
		};

		struct range_result
		{
			range_key key;
			kind k;
		};

		struct entry
		{
			std::string source; // for the log
			kind k;
		};

		std::mutex g_mutex;
		bool g_loaded = false;
		std::unordered_map<uint64_t, entry> g_hashes;      // index FNV-1a 64 -> table entry
		std::unordered_set<UINT> g_counts;                  // index counts present in the tables
		std::unordered_map<IDirect3DIndexBuffer9*, std::vector<range_result>> g_cache;
		std::unordered_set<uint64_t> g_logged;

		void load_table_locked(const char* file_name, kind k, const char* what, const char* missing)
		{
			const auto path = shared::globals::root_path + "\\rtx_comp\\sky\\" + file_name;
			std::ifstream file(path);
			if (!file)
			{
				shared::common::log("SkyMeshes", std::format("{} not found - {}", path, missing),
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
				return;
			}

			size_t loaded = 0;
			std::string line;
			while (std::getline(file, line))
			{
				if (line.empty() || line[0] == '#')
					continue;
				std::istringstream in(line);
				std::string hash_hex, source;
				UINT count = 0;
				if (!(in >> hash_hex >> count >> source))
					continue;
				g_hashes.emplace(std::stoull(hash_hex, nullptr, 16), entry{ source, k });
				g_counts.insert(count);
				loaded++;
			}
			shared::common::log("SkyMeshes", std::format("{} {} face sets loaded", loaded, what));
		}

		void load_locked()
		{
			g_loaded = true;
			load_table_locked("sky_meshes.txt", kind::sky, "sky mesh", "sky layers stay world geometry");
			load_table_locked("shell_meshes.txt", kind::shell, "blended shell", "blended shells stay decals");
		}

		uint64_t fnv1a64(const uint8_t* data, size_t size)
		{
			uint64_t h = 0xCBF29CE484222325ull;
			for (size_t i = 0; i < size; i++)
				h = (h ^ data[i]) * 0x100000001B3ull;
			return h;
		}

		kind classify(IDirect3DIndexBuffer9* ib, UINT start, UINT count)
		{
			D3DINDEXBUFFER_DESC desc{};
			if (FAILED(ib->GetDesc(&desc)) || desc.Format != D3DFMT_INDEX16)
				return kind::none;
			const UINT offset = start * 2, bytes = count * 2;
			if (offset + bytes > desc.Size)
				return kind::none;

			void* data = nullptr;
			if (FAILED(ib->Lock(offset, bytes, &data, D3DLOCK_READONLY)) || !data)
				return kind::none;
			const auto hash = fnv1a64(static_cast<const uint8_t*>(data), bytes);
			ib->Unlock();

			const auto it = g_hashes.find(hash);
			if (it == g_hashes.end())
				return kind::none;
			if (g_logged.insert(hash).second)
				shared::common::log("SkyMeshes", std::format("{} drawn: {}",
					it->second.k == kind::sky ? "sky mesh" : "blended shell", it->second.source));
			return it->second.k;
		}

		kind lookup(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count)
		{
			UINT count = 0;
			if (type == D3DPT_TRIANGLESTRIP)
				count = prim_count + 2;
			else if (type == D3DPT_TRIANGLELIST)
				count = prim_count * 3;
			if (!ib || !count)
				return kind::none;

			std::lock_guard lock(g_mutex);
			if (!g_loaded)
				load_locked();
			if (!g_counts.contains(count))
				return kind::none;

			auto& ranges = g_cache[ib];
			const range_key key{ start_index, count };
			for (const auto& r : ranges)
			{
				if (r.key == key)
					return r.k;
			}
			const kind k = classify(ib, start_index, count);
			ranges.push_back({ key, k });
			return k;
		}
	}

	bool is_sky_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count)
	{
		return lookup(ib, type, start_index, prim_count) == kind::sky;
	}

	bool is_shell_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count)
	{
		return lookup(ib, type, start_index, prim_count) == kind::shell;
	}

	void on_index_buffer_created(IDirect3DIndexBuffer9* ib)
	{
		std::lock_guard lock(g_mutex);
		g_cache.erase(ib);
	}
}
