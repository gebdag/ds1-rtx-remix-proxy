#include "std_include.hpp"
#include "sky_meshes.hpp"

#include "shared/common/ffp_state.hpp"

#include <atomic>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace comp::game::sky_meshes
{
	namespace
	{
		enum class kind : uint8_t { none, sky, shell, glass, thin_wall, emitter, no_shadow, glow, painted, ground };

		struct range_key
		{
			UINT start;
			UINT count;
			bool operator==(const range_key&) const = default;
		};

		struct entry
		{
			std::string source;  // for the log
			kind k;
			uint8_t cut = 0;     // glass: the alpha that separates came (>=) from glass
			std::string texture; // glass: the diffuse the mesh is drawn with
			std::string derived; // glass: the glass pass texture (rtx_comp\glass\<derived>.dds)
			float above_y = -std::numeric_limits<float>::infinity(); // sky: only while the camera is above this height
			bool hide = false;    // no-shadow: prims hide_first..hide_last of the draw are left out
			UINT hide_first = 0;
			UINT hide_last = 0;
			std::vector<uint16_t> heights_mm; // ground card: each vertex's height above the ground, mm (0xFFFF: none)
		};

		// Entries live in g_hashes for good (a node map: pointers stay valid).
		struct match
		{
			kind k = kind::none;
			const entry* e = nullptr;
		};

		struct range_result
		{
			range_key key;
			match m;
		};

		std::mutex g_mutex;
		bool g_loaded = false;
		std::unordered_map<uint64_t, entry> g_hashes;      // index FNV-1a 64 -> table entry
		std::unordered_set<UINT> g_counts;                  // index counts present in the tables
		std::unordered_map<IDirect3DIndexBuffer9*, std::vector<range_result>> g_cache;
		std::unordered_set<uint64_t> g_logged;
		bool g_any_hide = false;                            // some no-shadow entry lists hidden prims

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
				std::string hash_hex, source, texture, derived;
				UINT count = 0, cut = 0;
				if (!(in >> hash_hex >> count >> source))
					continue;
				if (k == kind::glass && !(in >> texture >> cut >> derived && cut > 0 && cut < 256))
					continue;
				float above_y = -std::numeric_limits<float>::infinity();
				if (std::string word; k == kind::sky && in >> texture >> word && word == "above")
					in >> above_y;
				entry e{ source, k, static_cast<uint8_t>(cut), texture, derived, above_y };
				// A ground card: '<texture> <vertex count> <height mm> ...'.
				if (k == kind::ground)
				{
					std::string tex;
					size_t n = 0;
					if (!(in >> tex >> n) || n == 0 || n > 65536)
						continue;
					e.heights_mm.resize(n);
					size_t got = 0;
					for (unsigned h = 0; got < n && in >> h; got++)
						e.heights_mm[got] = static_cast<uint16_t>(std::min(h, 0xFFFFu));
					if (got != n)
						continue;
				}
				// A no-shadow mesh may name prims of its draw that no view shows: 'hide <first> <last>'.
				if (std::string word; k == kind::no_shadow && in >> word && word == "hide")
				{
					long long first = -1, last = -1;
					if (in >> first >> last && first >= 0 && last >= first)
					{
						e.hide = true;
						e.hide_first = static_cast<UINT>(first);
						e.hide_last = static_cast<UINT>(last);
						g_any_hide = true;
					}
					else
						shared::common::log("SkyMeshes", std::format("{}: bad 'hide <first> <last>' for {}, ignored", file_name, source),
							shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
				}
				g_hashes.emplace(std::stoull(hash_hex, nullptr, 16), std::move(e));
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
			load_table_locked("glass_meshes.txt", kind::glass, "window glass", "window glass is not split");
			// Before the thin walls: a face listed in both casts no shadow at all.
			load_table_locked("no_shadow_meshes.txt", kind::no_shadow, "no-shadow", "every map mesh casts shadows");
			// Last: a face set already listed as another kind keeps it.
			load_table_locked("thin_wall_meshes.txt", kind::thin_wall, "thin wall", "no map mesh gets the thin-wall tag");
			load_table_locked("emitter_meshes.txt", kind::emitter, "light emitter", "no character lights");
			load_table_locked("glow_meshes.txt", kind::glow, "glow", "nothing glows of its own");
			load_table_locked("painted_meshes.txt", kind::painted, "painted colour", "vertex colours stay normalised");
			load_table_locked("ground_cards.txt", kind::ground, "ground card", "ground cards keep their lift");
		}

		uint64_t fnv1a64(const uint8_t* data, size_t size)
		{
			uint64_t h = 0xCBF29CE484222325ull;
			for (size_t i = 0; i < size; i++)
				h = (h ^ data[i]) * 0x100000001B3ull;
			return h;
		}

		match classify(IDirect3DIndexBuffer9* ib, UINT start, UINT count)
		{
			D3DINDEXBUFFER_DESC desc{};
			if (FAILED(ib->GetDesc(&desc)) || desc.Format != D3DFMT_INDEX16)
				return {};
			const UINT offset = start * 2, bytes = count * 2;
			if (offset + bytes > desc.Size)
				return {};

			void* data = nullptr;
			if (FAILED(ib->Lock(offset, bytes, &data, D3DLOCK_READONLY)) || !data)
				return {};
			const auto hash = fnv1a64(static_cast<const uint8_t*>(data), bytes);
			ib->Unlock();

			const auto it = g_hashes.find(hash);
			if (it == g_hashes.end())
				return {};
			if (g_logged.insert(hash).second)
			{
				const char* what = it->second.k == kind::sky ? "sky mesh" :
					it->second.k == kind::shell ? "blended shell" :
					it->second.k == kind::glass ? "window glass" :
					it->second.k == kind::thin_wall ? "thin wall" : it->second.k == kind::no_shadow ? "no-shadow mesh" : it->second.k == kind::glow ? "glow mesh" : it->second.k == kind::painted ? "painted colour mesh" : it->second.k == kind::ground ? "ground card" : "light emitter";
				shared::common::log("SkyMeshes", std::format("{} drawn: {}", what, it->second.source));
			}
			return { it->second.k, &it->second };
		}

		match lookup(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count)
		{
			UINT count = 0;
			if (type == D3DPT_TRIANGLESTRIP)
				count = prim_count + 2;
			else if (type == D3DPT_TRIANGLELIST)
				count = prim_count * 3;
			if (!ib || !count)
				return {};

			std::lock_guard lock(g_mutex);
			if (!g_loaded)
				load_locked();
			if (!g_counts.contains(count))
				return {};

			auto& ranges = g_cache[ib];
			const range_key key{ start_index, count };
			for (const auto& r : ranges)
			{
				if (r.key == key)
					return r.m;
			}
			const match m = classify(ib, start_index, count);
			ranges.push_back({ key, m });
			return m;
		}
	}

	bool is_sky_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count)
	{
		const match m = lookup(ib, type, start_index, prim_count);
		if (m.k != kind::sky)
			return false;
		if (m.e->above_y == -std::numeric_limits<float>::infinity())
			return true;
		float x, y, z;
		return shared::common::ffp_state::get().world_offset(x, y, z) && y > m.e->above_y;
	}

	bool is_shell_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count)
	{
		return lookup(ib, type, start_index, prim_count).k == kind::shell;
	}

	bool is_thin_wall_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count)
	{
		return lookup(ib, type, start_index, prim_count).k == kind::thin_wall;
	}

	bool is_no_shadow_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count)
	{
		return lookup(ib, type, start_index, prim_count).k == kind::no_shadow;
	}

	bool any_hidden_prims()
	{
		// Once loaded the tables never change: read the flag without the lock after that.
		static std::atomic<int> state{ -1 };
		const int s = state.load(std::memory_order_acquire);
		if (s >= 0)
			return s > 0;
		std::lock_guard lock(g_mutex);
		if (!g_loaded)
			load_locked();
		state.store(g_any_hide ? 1 : 0, std::memory_order_release);
		return g_any_hide;
	}

	bool no_shadow_hidden_prims(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count,
		UINT& first, UINT& last)
	{
		const match m = lookup(ib, type, start_index, prim_count);
		if (m.k != kind::no_shadow || !m.e->hide || m.e->hide_last >= prim_count)
			return false;
		first = m.e->hide_first;
		last = m.e->hide_last;
		return true;
	}

	bool is_glow_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count)
	{
		return lookup(ib, type, start_index, prim_count).k == kind::glow;
	}

	bool is_painted_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count)
	{
		return lookup(ib, type, start_index, prim_count).k == kind::painted;
	}

	const std::vector<uint16_t>* ground_card_heights(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count)
	{
		const match m = lookup(ib, type, start_index, prim_count);
		return m.k == kind::ground ? &m.e->heights_mm : nullptr;
	}

	bool is_emitter_draw(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count)
	{
		return lookup(ib, type, start_index, prim_count).k == kind::emitter;
	}

	const std::string* glass_texture(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT start_index, UINT prim_count,
		uint8_t& cut, const std::string*& derived)
	{
		const match m = lookup(ib, type, start_index, prim_count);
		if (m.k != kind::glass)
			return nullptr;
		cut = m.e->cut;
		derived = &m.e->derived;
		return &m.e->texture;
	}

	void on_index_buffer_created(IDirect3DIndexBuffer9* ib)
	{
		std::lock_guard lock(g_mutex);
		g_cache.erase(ib);
	}
}
