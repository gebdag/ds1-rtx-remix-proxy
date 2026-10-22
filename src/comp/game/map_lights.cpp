#include "std_include.hpp"
#include "map_lights.hpp"
#include "host_feed.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace comp::game::map_lights
{
	// Which map the player is actually IN, via FieldArea's active entry:
	//
	//   entry    = *(ptr*)0x01378520
	//   vec_hdr  = *(ptr*)(entry + 0x08)
	//   arr      = *(ptr*)(vec_hdr + 0x10)     // array of 60-byte MapEntry
	//   index    = *(u32*)(entry + 0x14)
	//   mapDword = *(u32*)(arr + index * 60 + 4)
	//
	// packed as (area << 24) | (block << 16) | (sub << 8) | sub2, so Blighttown is
	// 0x0E000000. Taken from tools/ds1_scene_monitor, which has been driving Remix's
	// per-area configs off this chain.
	//
	// The obvious-looking alternative — the packed id cached at [0x013784A0 + 0x14] by
	// the map-path builder at 0x00EEFBC0 — is the *last map path built*, not the active
	// one, so with neighbouring blocks streamed in it names whichever loaded last.
	static constexpr DWORD FIELD_AREA_ACTIVE_RVA = 0x00F78520;   // VA 0x01378520
	static constexpr DWORD ENTRY_VECTOR = 0x08;
	static constexpr DWORD ENTRY_INDEX = 0x14;
	static constexpr DWORD VECTOR_ARRAY = 0x10;
	static constexpr DWORD MAP_ENTRY_STRIDE = 60;
	static constexpr DWORD MAP_ENTRY_DWORD = 0x04;

	// Synthesized lights are given slot ids past the manager's 512 so they cannot
	// collide with a harvested slot in the injector's per-slot light hash. Ids are
	// assigned at parse time, so toggling a category off and on keeps each light's
	// identity and Remix sees a light disappear and return rather than a new one.
	static constexpr int SYNTHETIC_SLOT_BASE = 0x10000;

	// A table row before category scaling.
	struct raw_light
	{
		point_lights::light base;
		size_t category = 0;
		std::string name;       // last column of the table row, e.g. "o4404_1000"
	};

	static std::vector<raw_light> s_raw;
	static std::vector<point_lights::light> s_lights;
	static std::vector<category> s_categories;
	static std::string s_loaded_map;
	static std::string s_source_path;
	static std::string s_categories_path;
	static std::string s_status = "not loaded";
	static bool s_enabled = true;
	static bool s_force_reload = false;
	static bool s_categories_loaded = false;

	static DWORD safe_read_dword(DWORD address)
	{
		if (!address) return 0;
		__try {
			return *reinterpret_cast<DWORD*>(address);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
	}

	unsigned current_map_id()
	{
		if (shared::globals::lookdev_host)
			return host_feed::frame().map_id;

		const DWORD entry = safe_read_dword(EXE_BASE + FIELD_AREA_ACTIVE_RVA);
		if (!entry) return 0;

		const DWORD vector_header = safe_read_dword(entry + ENTRY_VECTOR);
		if (!vector_header) return 0;

		const DWORD array = safe_read_dword(vector_header + VECTOR_ARRAY);
		if (!array) return 0;

		const DWORD index = safe_read_dword(entry + ENTRY_INDEX);
		if (index > 0xFFFF) return 0;   // a plausible entry count, not a wild value

		return safe_read_dword(array + index * MAP_ENTRY_STRIDE + MAP_ENTRY_DWORD);
	}

	std::string current_map_name()
	{
		const unsigned id = current_map_id();
		const unsigned area = (id >> 24) & 0xFF;

		// The engine's own map-path builder treats area >= 99 as "no real map", so the
		// same bound rejects uninitialised and menu states here.
		if (id == 0 || area >= 99) return {};

		return std::format("m{:02}_{:02}_{:02}_{:02}",
			area, (id >> 16) & 0xFF, (id >> 8) & 0xFF, id & 0xFF);
	}

	// ------------------------------------------------------------------ categories

	static std::string lights_dir()
	{
		return shared::globals::root_path + "\\rtx_comp\\lights\\";
	}

	static size_t category_index(const std::string& name)
	{
		for (size_t i = 0; i < s_categories.size(); ++i)
		{
			if (s_categories[i].name == name)
				return i;
		}
		s_categories.push_back({ name });
		return s_categories.size() - 1;
	}

	static void load_categories()
	{
		s_categories_loaded = true;
		s_categories_path = lights_dir() + "categories.txt";

		std::ifstream file(s_categories_path);
		if (!file) return;

		std::string line;
		while (std::getline(file, line))
		{
			const auto first = line.find_first_not_of(" \t\r\n");
			if (first == std::string::npos || line[first] == '#')
				continue;

			std::istringstream in(line);
			std::string name;
			float intensity = 1.0f, radius = 1.0f;
			int on = 1;
			if (!(in >> name >> intensity >> radius >> on))
				continue;

			auto& c = s_categories[category_index(name)];
			c.intensity = intensity;
			c.radius = radius;
			c.enabled = on != 0;
		}
	}

	bool save_categories()
	{
		if (s_categories_path.empty())
			s_categories_path = lights_dir() + "categories.txt";

		std::ofstream file(s_categories_path, std::ios::trunc);
		if (!file) return false;

		file << "# Per-category light tuning for the synthesized map lights.\n"
			 << "# Scales multiply the values in the per-map tables, so 1.0 is exactly the\n"
			 << "# map's own POINT_LIGHT_BANK row. Categories are global: torch_small covers\n"
			 << "# every torch in the game.\n"
			 << "#\n"
			 << "# category  intensityScale  radiusScale  enabled\n";
		for (const auto& c : s_categories)
			file << std::format("{}  {:.3f}  {:.3f}  {}\n",
				c.name, c.intensity, c.radius, c.enabled ? 1 : 0);
		return true;
	}

	// ----------------------------------------------------------------- map tables

	// Emit the lights the current category settings ask for. Rebuilt rather than
	// mutated so a disabled category simply stops appearing and the injector's own
	// per-frame cleanup releases its Remix handles.
	static void rebuild()
	{
		s_lights.clear();
		s_lights.reserve(s_raw.size());
		for (const auto& raw : s_raw)
		{
			const auto& c = s_categories[raw.category];
			if (!c.enabled)
				continue;

			point_lights::light light = raw.base;
			light.r *= c.intensity;
			light.g *= c.intensity;
			light.b *= c.intensity;
			light.dwindle_begin *= c.radius;
			light.dwindle_end *= c.radius;
			s_lights.push_back(light);
		}
	}

	// "x y z  dwindleBegin dwindleEnd  r g b  intensityPct  [category]  name"
	//
	// The category column is optional so a hand-written table without one still loads;
	// those lights land in "uncategorised" and can still be toggled as a group.
	static bool parse_line(const std::string& line, int slot, raw_light& out)
	{
		std::istringstream in(line);
		float begin = 0.0f, end = 0.0f;
		int r = 0, g = 0, b = 0, intensity_pct = 0;

		auto& light = out.base;
		if (!(in >> light.x >> light.y >> light.z >> begin >> end >> r >> g >> b >> intensity_pct))
			return false;
		if (!std::isfinite(light.x) || !std::isfinite(light.y) || !std::isfinite(light.z))
			return false;
		if (!std::isfinite(begin) || !std::isfinite(end) || end <= 0.0f)
			return false;

		std::string first, second;
		in >> first >> second;
		out.category = category_index(second.empty() ? "uncategorised" : first);
		out.name = second.empty() ? first : second;

		// The conversion the engine's own row evaluator applies (VA 0x00FC83D0):
		// 0-255 components scaled by a percentage.
		const float intensity = static_cast<float>(intensity_pct) * 0.01f;
		light.slot = slot;
		light.r = (static_cast<float>(r) / 255.0f) * intensity;
		light.g = (static_cast<float>(g) / 255.0f) * intensity;
		light.b = (static_cast<float>(b) / 255.0f) * intensity;
		light.dwindle_begin = begin;
		light.dwindle_end = end;
		light.has_params = true;
		light.source = point_lights::param_source::synthetic;
		return true;
	}

	static void load(const std::string& map_name)
	{
		s_raw.clear();
		s_lights.clear();
		s_loaded_map = map_name;
		s_source_path = lights_dir() + map_name + ".txt";
		for (auto& c : s_categories)
			c.count = 0;

		if (map_name.empty())
		{
			s_status = "no map loaded";
			return;
		}

		std::ifstream file(s_source_path);
		if (!file)
		{
			s_status = "no table for this map";
			return;
		}

		int rejected = 0;
		std::string line;
		while (std::getline(file, line))
		{
			const auto first = line.find_first_not_of(" \t\r\n");
			if (first == std::string::npos || line[first] == '#')
				continue;

			raw_light light = {};
			if (parse_line(line, SYNTHETIC_SLOT_BASE + static_cast<int>(s_raw.size()), light))
			{
				++s_categories[light.category].count;
				s_raw.push_back(light);
			}
			else
			{
				++rejected;
			}
		}

		rebuild();
		s_status = rejected == 0
			? std::format("{} light(s)", s_raw.size())
			: std::format("{} light(s), {} malformed line(s) skipped", s_raw.size(), rejected);
	}

	// Every map change reports what it found, so a wrong id or a missing table is
	// visible in the log instead of just being zero lights.
	static void log_load()
	{
		const unsigned id = current_map_id();
		if (s_raw.empty())
			shared::common::log("MapLights",
				std::format("map 0x{:08X} '{}' -> {} ({})", id,
					s_loaded_map.empty() ? "?" : s_loaded_map, s_status, s_source_path),
				shared::common::LOG_TYPE::LOG_TYPE_WARN);
		else
			shared::common::log("MapLights",
				std::format("map 0x{:08X} '{}' -> {}", id, s_loaded_map, s_status),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS);
	}

	const std::vector<point_lights::light>& get()
	{
		if (!s_categories_loaded)
			load_categories();

		if (!s_enabled)
		{
			s_lights.clear();
			s_raw.clear();
			s_loaded_map.clear();
			return s_lights;
		}

		const std::string map_name = current_map_name();
		if (s_force_reload || map_name != s_loaded_map)
		{
			s_force_reload = false;
			load(map_name);
			log_load();
		}
		return s_lights;
	}

	void set_enabled(bool on)
	{
		if (s_enabled == on) return;
		s_enabled = on;
		s_loaded_map.clear();   // force a reload when switched back on
	}

	bool enabled() { return s_enabled; }

	bool describe(int slot, std::string& name, std::string& category)
	{
		const int index = slot - SYNTHETIC_SLOT_BASE;
		if (index < 0 || index >= static_cast<int>(s_raw.size()))
			return false;
		name = s_raw[index].name;
		category = s_categories[s_raw[index].category].name;
		return true;
	}
	void reload() { s_force_reload = true; }

	const std::vector<category>& categories() { return s_categories; }

	void set_category(size_t index, float intensity, float radius, bool on)
	{
		if (index >= s_categories.size()) return;
		auto& c = s_categories[index];
		c.intensity = std::max(0.0f, intensity);
		c.radius = std::max(0.01f, radius);
		c.enabled = on;
		rebuild();
	}

	void reset_categories()
	{
		for (auto& c : s_categories)
		{
			c.intensity = 1.0f;
			c.radius = 1.0f;
			c.enabled = true;
		}
		rebuild();
	}

	const std::string& categories_path() { return s_categories_path; }
	const std::string& source_path() { return s_source_path; }
	const std::string& status() { return s_status; }
}
