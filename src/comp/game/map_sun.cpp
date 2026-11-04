#include "std_include.hpp"
#include "map_sun.hpp"

#include "atmosphere.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/globals.hpp"

#include <array>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace comp::game::map_sun
{
	namespace
	{
		// The game's DrawParam manager (exe: FUN_00d0cff0 registers a bank in it through FUN_00d0b580 / FUN_00d0b200).
		// +8: the list of areas, +4 count, +8 array of 0x24-byte entries, the area number at +7.
		// Its banks: (2 + area index * 27 + slot) * 12 from the manager: { param, ?, the LIGHT_BANK's per-row array }.
		// A param: +0x20 the loaded file, laid out as on disk (type at 0xC, rows at 0x30: id, data offset, name offset).
		constexpr uintptr_t kDrawParamMan = 0x0137D99C;
		constexpr size_t kAreaEntry = 0x24, kLightRowInfo = 0x2C, kLightRowCube = 0x18;
		constexpr int kSlots = 13, kMaxAreas = 12;

		// Outdoor suns measure 0.31 and up, against an ambient of about their own size. Enclosed places reach 0.31 at an ambient of 2.5: no sun.
		constexpr float kMinSun = 0.25f, kMinSunShare = 0.25f;

		bool read_bytes(uintptr_t address, void* out, size_t size)
		{
			__try
			{
				std::memcpy(out, reinterpret_cast<const void*>(address), size);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		template <class T>
		bool read(uintptr_t address, T& out)
		{
			return read_bytes(address, &out, sizeof(T));
		}

		struct bank_row
		{
			int id = 0;
			uintptr_t data = 0;
			std::string name;
			int index = 0;
		};

		struct bank
		{
			uintptr_t info = 0; // the per-row array, LIGHT_BANK only
			std::vector<bank_row> rows;
		};

		bool read_rows(uintptr_t file, bank& out)
		{
			uint16_t count = 0;
			if (!read(file + 0xA, count) || count > 1024)
				return false;
			for (int i = 0; i < count; i++)
			{
				int head[3] = {};
				if (!read_bytes(file + 0x30 + static_cast<uintptr_t>(i) * 12, head, sizeof(head)))
					return false;
				bank_row row;
				row.id = head[0];
				row.data = file + static_cast<uint32_t>(head[1]);
				row.index = i;
				char name[64] = {};
				if (head[2] && read_bytes(file + static_cast<uint32_t>(head[2]), name, sizeof(name) - 1))
					row.name = name;
				out.rows.push_back(std::move(row));
			}
			return true;
		}

		bool find_banks(unsigned area, bank& light, bank& shadow)
		{
			uintptr_t man = 0, list = 0, entries = 0;
			int count = 0;
			if (!read(kDrawParamMan, man) || !man || !read(man + 8, list) || !list || !read(list + 4, count) || !read(list + 8, entries) || !entries)
				return false;
			for (int i = 0; i < std::min(count, kMaxAreas); i++)
			{
				uint8_t number = 0;
				if (!read(entries + static_cast<uintptr_t>(i) * kAreaEntry + 7, number) || number != area)
					continue;
				for (int slot = 0; slot < kSlots; slot++)
				{
					const uintptr_t at = man + static_cast<uintptr_t>(2 + i * 27 + slot) * 12;
					uintptr_t param = 0, file = 0;
					char type[0x20] = {};
					if (!read(at, param) || !param || !read(param + 0x20, file) || !file || !read_bytes(file + 0xC, type, sizeof(type) - 1))
						continue;
					if (!std::strcmp(type, "LIGHT_BANK"))
					{
						read(at + 8, light.info);
						read_rows(file, light);
					}
					else if (!std::strcmp(type, "SHADOW_BANK"))
						read_rows(file, shadow);
				}
				return !light.rows.empty() && !shadow.rows.empty() && light.info;
			}
			return false;
		}

		// The cube behind a bank's texture handle: within two pointers of it, one the device created.
		IDirect3DBaseTexture9* cube_of(uintptr_t handle)
		{
			auto& ffp = shared::common::ffp_state::get();
			uint32_t first[11] = {};
			if (!handle || !read_bytes(handle, first, sizeof(first)))
				return nullptr;
			for (const uint32_t a : first)
			{
				if (!a)
					continue;
				if (auto* cube = ffp.known_cube(a))
					return cube;
				uint32_t second[16] = {};
				if (!read_bytes(a, second, sizeof(second)))
					continue;
				for (const uint32_t b : second)
					if (b)
						if (auto* cube = ffp.known_cube(b))
							return cube;
			}
			return nullptr;
		}

		// A pointer that only looks like one of the device's cubes must not take the game down.
		bool safe_probe_lobe(IDirect3DBaseTexture9* cube, const float* scale, const float* direction, float* up, float* down, float* lobe)
		{
			__try
			{
				return atmosphere::probe_lobe(cube, scale, direction, up, down, lobe);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

		// Shift-JIS, as the banks have them.
		const char* const kBasic = "\x8A\xEE\x96\x7B";
		const char* const kOutdoors = "\x89\xAE\x8A\x4F";
		const char* const kNotMapLight[] = { "\x83\x4C\x83\x83\x83\x89", "\xB7\xAC\xD7", "\x93\x56\x8B\x85", "\x89\x93\x8C\x69", "\x92\x86\x8C\x69" };

		float luminance(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

		// False while the banks or the cubes are not there yet.
		bool from_game(unsigned map_id, values& out)
		{
			const unsigned area = (map_id >> 24) & 0xFF, block = (map_id >> 16) & 0xFF;
			bank light, shadow;
			if (!find_banks(area, light, shadow))
				return false;

			const std::string tag = std::format("B{}", block);
			const bank_row* direction_row = nullptr;
			std::vector<std::string> wanted = { tag + kBasic, tag + kOutdoors };
			if (block == 0)
				wanted.emplace_back(kBasic);
			int16_t rotation[2] = {};
			for (const auto& name : wanted)
			{
				for (const auto& row : shadow.rows)
					if (row.name == name && read_bytes(row.data, rotation, sizeof(rotation)) && (rotation[0] || rotation[1]))
					{
						direction_row = &row;
						break;
					}
				if (direction_row)
					break;
			}
			if (!direction_row)
			{
				out = {};
				out.authored = false;
				return true;
			}

			constexpr float kRadians = 3.14159265358979f / 180.0f;
			const float rx = rotation[0] * kRadians, ry = rotation[1] * kRadians;
			const float direction[3] = { std::cos(rx) * std::sin(ry), -std::sin(rx), std::cos(rx) * std::cos(ry) };

			bool any = false, cube_missing = false;
			float best = -1.0f;
			values found;
			for (const auto& row : light.rows)
			{
				if (row.name.rfind(tag, 0) != 0)
					continue;
				bool skip = false;
				for (const char* part : kNotMapLight)
					skip = skip || has(row.name, part);
				if (skip)
					continue;
				int16_t s[46] = {};
				uintptr_t handle = 0;
				if (!read_bytes(row.data, s, sizeof(s)) || !read(light.info + static_cast<uintptr_t>(row.index) * kLightRowInfo + kLightRowCube, handle))
					continue;
				auto* cube = cube_of(handle);
				if (!cube)
				{
					// The named row is the sun: no other row stands in while its cube is not there yet.
					if (row.name == direction_row->name)
						return false;
					cube_missing = true;
					continue;
				}
				const float scale[3] = { s[32] / 255.0f * s[35] / 100.0f, s[33] / 255.0f * s[35] / 100.0f, s[34] / 255.0f * s[35] / 100.0f };
				float up[3], down[3], lobe[3];
				if (!safe_probe_lobe(cube, scale, direction, up, down, lobe))
					continue;
				const float lum = luminance(lobe);
				const bool same_name = row.name == direction_row->name;
				if (!same_name && lum <= best)
					continue;
				any = true;
				best = lum;
				found.has_sun = true;
				for (int c = 0; c < 3; c++)
				{
					found.direction[c] = direction[c];
					found.lobe[c] = lobe[c];
					const float hemi_up = s[18 + c] / 255.0f * s[21] / 100.0f, hemi_down = s[22 + c] / 255.0f * s[25] / 100.0f;
					found.base[c] = 0.0f;
					for (int axis = 0; axis < 3; axis++)
					{
						const float v = -direction[axis];
						const float face = axis == 1 ? (v >= 0.0f ? up[c] + hemi_up : down[c] + hemi_down) : 0.5f * (up[c] + down[c] + hemi_up + hemi_down);
						found.base[c] += v * v * face;
					}
				}
				if (same_name)
					break;
			}
			if (!any)
				return false;
			if (cube_missing && best < kMinSun)
				return false;
			found.has_sun = best >= kMinSun && best >= kMinSunShare * luminance(found.base);
			out = found;
			return true;
		}

		// For the lookdev host, which has no game memory, and if the game's own values cannot be read (scripts/gen_map_suns.py, the same rule).
		bool from_table(unsigned map_id, values& out)
		{
			static const std::unordered_map<unsigned, values> table = []
			{
				std::unordered_map<unsigned, values> loaded;
				std::ifstream file("rtx_comp\\sky\\map_suns.txt");
				std::string line;
				while (std::getline(file, line))
				{
					if (line.empty() || line[0] == '#')
						continue;
					std::istringstream in(line);
					std::string key;
					values v;
					if (in >> key >> v.direction[0] >> v.direction[1] >> v.direction[2] >> v.lobe[0] >> v.lobe[1] >> v.lobe[2] >> v.base[0] >> v.base[1] >> v.base[2])
					{
						v.has_sun = v.lobe[0] > 0.0f || v.lobe[1] > 0.0f || v.lobe[2] > 0.0f;
						loaded[static_cast<unsigned>(std::stoul(key, nullptr, 16))] = v;
					}
				}
				return loaded;
			}();
			const auto found = table.find(map_id >> 16);
			if (found == table.end())
				return false;
			out = found->second;
			return true;
		}
	}

	const values& get(unsigned map_id)
	{
		static unsigned cached_map = 0;
		static values cached;
		static int tries = 0;
		static bool settled = false;
		static unsigned frame = 0;
		if (map_id != cached_map)
		{
			cached_map = map_id;
			shared::common::ffp_state::get().forget_cube_faces();
			tries = 0;
			settled = false;
			cached = {};
			cached.authored = false;
			if (from_table(map_id, cached))
				cached.authored = true;
		}
		// The banks load with the map: a look now and then until they are there, then never again.
		if (!settled && map_id && !shared::globals::lookdev_host && (frame++ % 30) == 0 && tries < 40)
		{
			tries++;
			values game;
			if (from_game(map_id, game))
			{
				const bool same = game.has_sun == cached.has_sun && std::abs(game.lobe[0] - cached.lobe[0]) < 0.01f;
				shared::common::log("MapSun", std::format("map {:08X}: from the game's banks, authored {} sun {} dir {:.3f},{:.3f},{:.3f} lobe {:.3f},{:.3f},{:.3f} ({} the table)",
					map_id, game.authored, game.has_sun, game.direction[0], game.direction[1], game.direction[2], game.lobe[0], game.lobe[1], game.lobe[2], same ? "as" : "NOT as"));
				cached = game;
				settled = true;
			}
			else if (tries == 40)
				shared::common::log("MapSun", std::format("map {:08X}: the game's banks could not be read, keeping the table", map_id), shared::common::LOG_TYPE::LOG_TYPE_WARN);
		}
		return cached;
	}
}
