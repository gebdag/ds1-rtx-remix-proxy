#include "std_include.hpp"
#include "area_overrides.hpp"
#include "camera.hpp"
#include "map_lights.hpp"

#include "../modules/game_lights.hpp"
#include "shared/common/config.hpp"
#include "shared/common/remix_api.hpp"

#include <algorithm>
#include <fstream>
#include <format>
#include <map>
#include <unordered_map>

namespace comp::game::area_overrides
{
	namespace
	{
		struct area
		{
			std::string name;
			std::string map;
			bool has_box = false;
			float box_min[3] = {};
			float box_max[3] = {};
			std::vector<std::pair<std::string, std::string>> values;
		};

		std::vector<area> g_areas;
		FILETIME g_file_time {};
		bool g_file_seen = false;
		std::vector<std::string> g_active;
		std::map<std::string, std::string> g_applied;

		std::string trim(const std::string& t)
		{
			const auto a = t.find_first_not_of(" \t\r\n"), b = t.find_last_not_of(" \t\r\n");
			return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
		}

		std::string file_path()
		{
			return shared::globals::settings_path + "\\area_overrides.ini";
		}

		void parse_file()
		{
			g_areas.clear();
			std::ifstream in(file_path());
			std::string line;
			int number = 0;
			while (std::getline(in, line))
			{
				++number;
				line = trim(line);
				if (line.empty() || line[0] == ';' || line[0] == '#')
					continue;
				if (line.front() == '[' && line.back() == ']')
				{
					g_areas.push_back({});
					g_areas.back().name = trim(line.substr(1, line.size() - 2));
					continue;
				}
				const auto eq = line.find('=');
				if (eq == std::string::npos || g_areas.empty())
				{
					shared::common::log("AreaOverrides", std::format("line {} ignored: {}", number, line), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
					continue;
				}
				auto& a = g_areas.back();
				const auto key = trim(line.substr(0, eq));
				const auto value = trim(line.substr(eq + 1));
				if (key == "map")
					a.map = value;
				else if (key == "box")
				{
					float b[6];
					if (sscanf_s(value.c_str(), "%f %f %f %f %f %f", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6)
					{
						a.has_box = true;
						for (int i = 0; i < 3; i++)
						{
							a.box_min[i] = std::min(b[i], b[i + 3]);
							a.box_max[i] = std::max(b[i], b[i + 3]);
						}
					}
					else
						shared::common::log("AreaOverrides", std::format("line {}: a box is six numbers: x y z x y z", number), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				}
				else if (key.rfind("rtx.", 0) == 0 || (key.rfind("proxy.", 0) == 0 && key.find('.', 6) != std::string::npos))
					a.values.emplace_back(key, value);
				else
					shared::common::log("AreaOverrides", std::format("line {}: {} is neither map, box, rtx.<option> nor proxy.<Section>.<Key>", number, key), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			}
			shared::common::log("AreaOverrides", std::format("{}: {} area(s)", file_path(), g_areas.size()), shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}

		void reload_if_changed()
		{
			WIN32_FILE_ATTRIBUTE_DATA info;
			const bool exists = GetFileAttributesExA(file_path().c_str(), GetFileExInfoStandard, &info) != 0;
			if (!exists)
			{
				if (g_file_seen)
					g_areas.clear();
				g_file_seen = false;
				return;
			}
			if (g_file_seen && CompareFileTime(&info.ftLastWriteTime, &g_file_time) == 0)
				return;
			g_file_seen = true;
			g_file_time = info.ftLastWriteTime;
			parse_file();
		}

		// What the option was before any area touched it: user.conf, then rtx.conf (user.conf wins in Remix too).
		// Empty when neither sets it: Remix then takes the value off and falls back to its default.
		std::string rtx_baseline(const std::string& key)
		{
			for (const char* name : { "\\user.conf", "\\rtx.conf" })
			{
				std::ifstream in(shared::globals::root_path + name);
				std::string line;
				while (std::getline(in, line))
				{
					const auto eq = line.find('=');
					if (eq != std::string::npos && trim(line.substr(0, eq)) == key)
						return trim(line.substr(eq + 1));
				}
			}
			return {};
		}

		void set_rtx(const std::string& key, const std::string& value)
		{
			auto& bridge = shared::common::remix_api::get().m_bridge;
			if (!bridge.SetConfigVariable || bridge.SetConfigVariable(key.c_str(), value.c_str()) != REMIXAPI_ERROR_CODE_SUCCESS)
				shared::common::log("AreaOverrides", std::format("Remix refused {} = {}", key, value), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		}

		std::string join(const std::vector<std::string>& names)
		{
			std::string out;
			for (const auto& n : names)
				out += (out.empty() ? "" : ", ") + n;
			return out.empty() ? "none" : out;
		}
	}

	namespace
	{
		std::string sun_section(const std::string& map) { return "F4 sun " + map; }

		// The file's lines without the panel's sections of this map.
		std::vector<std::string> lines_without_sun(const std::string& map, bool& ok)
		{
			std::vector<std::string> lines;
			std::ifstream in(file_path());
			// No file yet is fine. One that is there and cannot be read must not be written over.
			ok = in.is_open() || GetFileAttributesA(file_path().c_str()) == INVALID_FILE_ATTRIBUTES;
			std::string line;
			bool dropping = false;
			const auto wanted = "[" + sun_section(map);
			while (std::getline(in, line))
			{
				const auto t = trim(line);
				if (!t.empty() && t.front() == '[')
					dropping = t.rfind(wanted, 0) == 0;
				// The panel writes no comments: one here belongs to what follows.
				else if (!t.empty() && (t.front() == ';' || t.front() == '#'))
					dropping = false;
				if (!dropping)
					lines.push_back(line);
			}
			while (!lines.empty() && trim(lines.back()).empty())
				lines.pop_back();
			return lines;
		}

		bool write_lines(const std::vector<std::string>& lines)
		{
			// To the side first, so a failure leaves the file as it was.
			const auto side = file_path() + ".tmp";
			{
				std::ofstream out(side, std::ios::trunc);
				for (const auto& l : lines)
					out << l << "\n";
				if (!out.good())
					return false;
			}
			return MoveFileExA(side.c_str(), file_path().c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
		}
	}

	bool save_sun(const std::string& map, const float* box_min, const float* box_max, const float* colour, const float* direction)
	{
		bool ok = false;
		if (map.empty())
			return false;
		auto lines = lines_without_sun(map, ok);
		if (!ok)
			return false;
		lines.emplace_back("");
		lines.push_back(std::format("[{}]", sun_section(map)));
		lines.push_back("map = " + map);
		if (box_min && box_max)
			lines.push_back(std::format("box = {:.1f} {:.1f} {:.1f} {:.1f} {:.1f} {:.1f}", box_min[0], box_min[1], box_min[2], box_max[0], box_max[1], box_max[2]));
		lines.push_back(std::format("proxy.Sun.Colour = {:.3f},{:.3f},{:.3f}", colour[0], colour[1], colour[2]));
		lines.push_back(std::format("proxy.Sun.Direction = {:.4f},{:.4f},{:.4f}", direction[0], direction[1], direction[2]));
		return write_lines(lines);
	}

	bool remove_sun(const std::string& map)
	{
		bool ok = false;
		if (map.empty())
			return false;
		const auto lines = lines_without_sun(map, ok);
		return ok && write_lines(lines);
	}

	bool has_saved_sun(const std::string& map)
	{
		if (map.empty())
			return false;
		const auto wanted = sun_section(map);
		return std::any_of(g_areas.begin(), g_areas.end(), [&](const area& a) { return a.name.rfind(wanted, 0) == 0; });
	}

	void frame_update(unsigned int frame)
	{
		if (frame % 15 != 0 || !shared::common::remix_api::is_initialized())
			return;
		reload_if_changed();

		const auto map = map_lights::current_map_name();
		float x = 0.0f, depth = 0.0f, up = 0.0f;
		const bool have_position = camera::get_char_pos(x, depth, up) && !(x == 0.0f && depth == 0.0f && up == 0.0f);
		const float position[3] = { x, up, depth };

		std::vector<std::string> active;
		std::map<std::string, std::string> wanted;
		for (const auto& a : g_areas)
		{
			if (a.map != map)
				continue;
			if (a.has_box)
			{
				// No player position (a menu, a load): the place is not known, so a box does not match.
				if (!have_position)
					continue;
				bool inside = true;
				for (int i = 0; i < 3; i++)
					inside = inside && position[i] >= a.box_min[i] && position[i] <= a.box_max[i];
				if (!inside)
					continue;
			}
			active.push_back(a.name);
			for (const auto& [key, value] : a.values)
				wanted[key] = value;
		}
		if (active == g_active && wanted == g_applied)
			return;
		if (active != g_active)
			shared::common::log("AreaOverrides", std::format("area: {} (was {})", join(active), join(g_active)), shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		g_active = active;

		bool proxy_changed = false;
		for (const auto& [key, value] : g_applied)
			if (!wanted.contains(key))
			{
				if (key.rfind("rtx.", 0) == 0)
					set_rtx(key, rtx_baseline(key));
				else
					proxy_changed = true;
				shared::common::log("AreaOverrides", std::format("{} back from {}", key, value), shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
		std::unordered_map<std::string, std::string> proxy_values;
		for (const auto& [key, value] : wanted)
		{
			const auto was = g_applied.find(key);
			const bool changed = was == g_applied.end() || was->second != value;
			if (key.rfind("rtx.", 0) == 0)
			{
				if (changed)
					set_rtx(key, value);
			}
			else
			{
				proxy_changed = proxy_changed || changed;
				proxy_values[key.substr(6)] = value;
			}
			if (changed)
				shared::common::log("AreaOverrides", std::format("{} = {}", key, value), shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}
		g_applied = wanted;

		if (proxy_changed)
		{
			shared::common::config::get().set_area_values(std::move(proxy_values));
			if (auto* gl = comp::game_lights::get())
				gl->reread_settings();
		}
	}
}
