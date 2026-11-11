#include "std_include.hpp"
#include "area_overrides.hpp"
#include "atmosphere.hpp"
#include "camera.hpp"
#include "frustum_culling.hpp"
#include "map_lights.hpp"

#include "../modules/game_lights.hpp"
#include "shared/common/config.hpp"
#include "shared/common/remix_api.hpp"

#include <algorithm>
#include <cmath>
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
			// `yaw = <degrees>`: the box turned about the vertical through its centre, for rooms that do not run along the map's
			// axes (Duke's Archives' library halls are diamonds). A point is inside when, turned back by yaw about that centre
			// (x' = dx cos + dz sin, z' = -dx sin + dz cos), it lies in the box.
			float yaw_deg = 0.0f;

			bool contains(const float* position) const
			{
				if (!has_box)
					return true;
				float c[3], h[3];
				for (int i = 0; i < 3; i++)
				{
					c[i] = 0.5f * (box_min[i] + box_max[i]);
					h[i] = 0.5f * (box_max[i] - box_min[i]);
				}
				const float dx = position[0] - c[0], dy = position[1] - c[1], dz = position[2] - c[2];
				const float r = yaw_deg * 0.017453292f, cs = std::cos(r), sn = std::sin(r);
				const float x = dx * cs + dz * sn, z = -dx * sn + dz * cs;
				return std::abs(x) <= h[0] && std::abs(dy) <= h[1] && std::abs(z) <= h[2];
			}
			int when = 0; // 0 always, 1 only in the map's dark version, 2 only in its light one (atmosphere::ambient_is_cold), -1 never (a bad value)
			std::vector<std::string> light_prefixes; // `lights = <prefix>`: custom lights that exist only in this area
			std::vector<std::pair<std::string, std::string>> values;
		};

		std::vector<area> g_areas;
		FILETIME g_file_time {};
		bool g_file_seen = false;
		std::vector<std::string> g_active;
		// Custom light name prefixes claimed by this map's areas (`lights = `): with the claiming area's name, and whether the
		// player is in none of the areas that claim it.
		struct light_claim { std::string prefix; std::string area; bool away; };
		std::vector<light_claim> g_light_claims;
		std::string g_light_claims_map;   // the map they were built for
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
				else if (key == "yaw")
				{
					if (sscanf_s(value.c_str(), "%f", &a.yaw_deg) != 1)
						shared::common::log("AreaOverrides", std::format("line {}: yaw is one number, in degrees", number), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				}
				else if (key == "lights")
				{
					if (!value.empty())
						a.light_prefixes.push_back(value);
				}
				else if (key == "when")
				{
					// Anything else (a typo) leaves the section off rather than always on.
					a.when = value == "dark" ? 1 : value == "light" ? 2 : -1;
					if (a.when < 0)
						shared::common::log("AreaOverrides", std::format("line {}: when is dark or light; section ignored", number), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				}
				else if (key.rfind("rtx.", 0) == 0 || (key.rfind("proxy.", 0) == 0 && key.find('.', 6) != std::string::npos))
					a.values.emplace_back(key, value);
				else
					shared::common::log("AreaOverrides", std::format("line {}: {} is neither map, box, yaw, when, lights, rtx.<option> nor proxy.<Section>.<Key>", number, key), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
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

		void set_rtx(const std::string& key, const std::string& value)
		{
			auto& bridge = shared::common::remix_api::get().m_bridge;
			if (!bridge.SetConfigVariable || bridge.SetConfigVariable(key.c_str(), value.c_str()) != REMIXAPI_ERROR_CODE_SUCCESS)
				shared::common::log("AreaOverrides", std::format("Remix refused {} = {}", key, value), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		}

		// The areas of this map the player is in now.
		std::vector<std::string> areas_holding_player(const std::string& map)
		{
			float x = 0.0f, depth = 0.0f, up = 0.0f;
			const bool have_position = camera::get_char_pos(x, depth, up) && !(x == 0.0f && depth == 0.0f && up == 0.0f);
			const float position[3] = { x, up, depth };
			std::vector<std::string> names;
			for (const auto& a : g_areas)
			{
				if (a.map != map || a.when < 0 || (a.when && (a.when == 1) != atmosphere::ambient_is_cold()))
					continue;
				if ((!a.has_box || have_position) && a.contains(position))
					names.push_back(a.name);
			}
			return names;
		}

		// Which custom lights are away: a prefix claimed on this map, with the player in none of the areas claiming it.
		void build_light_claims(const std::string& map, const std::vector<std::string>& active)
		{
			std::vector<light_claim> claims;
			for (const auto& a : g_areas)
			{
				if (a.map != map)
					continue;
				const bool in = std::find(active.begin(), active.end(), a.name) != active.end();
				for (const auto& prefix : a.light_prefixes)
				{
					const auto known = std::find_if(claims.begin(), claims.end(), [&](const light_claim& c) { return c.prefix == prefix; });
					if (known == claims.end())
						claims.push_back({ prefix, a.name, !in });
					else if (in)
						known->away = false;
				}
			}
			g_light_claims = std::move(claims);
			g_light_claims_map = map;
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

	std::string hand_written_sun(const std::string& map)
	{
		std::string names;
		if (map.empty())
			return names;
		const auto panel = sun_section(map);
		for (const auto& a : g_areas)
		{
			if (a.map != map || a.name.rfind(panel, 0) == 0)
				continue;
			if (std::any_of(a.values.begin(), a.values.end(), [](const auto& kv) { return kv.first == "proxy.Sun.Colour" || kv.first == "proxy.Sun.Direction"; }))
				names += (names.empty() ? "[" : ", [") + a.name + "]";
		}
		return names;
	}

	bool saved_sun(const std::string& map, float colour[3], float direction[3], bool& in_box, std::string* by_hand)
	{
		if (by_hand)
			by_hand->clear();
		if (map.empty())
			return false;
		const auto wanted = sun_section(map);
		for (const auto& a : g_areas)
		{
			if (a.name.rfind(wanted, 0) != 0)
				continue;
			// A sun saved for a box is this map's saved sun only while the player is in that box. Counted everywhere, a Save made
			// anywhere else on the map (with the game's sun showing, as it should there) took it for switched off and deleted it.
			if (a.has_box && std::find(g_active.begin(), g_active.end(), a.name) == g_active.end())
				continue;
			bool have_colour = false, have_direction = false;
			for (const auto& [key, value] : a.values)
			{
				if (key == "proxy.Sun.Colour")
					have_colour = std::sscanf(value.c_str(), "%f , %f , %f", &colour[0], &colour[1], &colour[2]) == 3;
				else if (key == "proxy.Sun.Direction")
					have_direction = std::sscanf(value.c_str(), "%f , %f , %f", &direction[0], &direction[1], &direction[2]) == 3;
			}
			if (!have_colour || !have_direction)
				continue;
			const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
			for (int c = 0; c < 3; ++c)
				direction[c] /= std::max(length, 1e-6f);
			in_box = a.has_box;
			return true;
		}
		// No section of the panel's own: the file may still give this map a sun in sections written by hand (the Undead
		// Asylum's, in [Undead Asylum]). That is what the files hold, so it is no unsaved change: entering the Asylum listed
		// "Sun of m18_01_00_00: the game's -> 0.150,0.150,0.150" every time. Colour and direction each as the active areas set
		// them, in the file's order (the order they are applied in), so one may come from one section and one from another.
		float c[3] = {}, d[3] = {};
		bool have_colour = false, have_direction = false;
		std::string names;
		for (const auto& a : g_areas)
		{
			if (a.map != map || std::find(g_active.begin(), g_active.end(), a.name) == g_active.end())
				continue;
			bool used = false;
			for (const auto& [key, value] : a.values)
			{
				if (key == "proxy.Sun.Colour" && std::sscanf(value.c_str(), "%f , %f , %f", &c[0], &c[1], &c[2]) == 3)
					have_colour = used = true;
				else if (key == "proxy.Sun.Direction" && std::sscanf(value.c_str(), "%f , %f , %f", &d[0], &d[1], &d[2]) == 3)
					have_direction = used = true;
			}
			if (used && names.find("[" + a.name + "]") == std::string::npos)
				names += (names.empty() ? "[" : ", [") + a.name + "]";
		}
		if (!have_colour || !have_direction)
			return false;
		const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		for (int i = 0; i < 3; ++i)
		{
			colour[i] = c[i];
			direction[i] = d[i] / std::max(length, 1e-6f);
		}
		in_box = false;
		if (by_hand)
			*by_hand = names;
		return true;
	}

	namespace
	{
		const light_claim* claim_of(const std::string& light_name)
		{
			// A map just loaded, before frame_update has placed the player in it: its claimed lights are away until it has,
			// rather than all made for a few frames and taken down again.
			if (const auto map = map_lights::current_map_name(); map != g_light_claims_map)
				build_light_claims(map, areas_holding_player(map));
			// The longest prefix wins, so 'Hollow_top_' can have its own area inside 'Hollow_'.
			const light_claim* best = nullptr;
			for (const auto& c : g_light_claims)
				if (light_name.rfind(c.prefix, 0) == 0 && (!best || c.prefix.size() > best->prefix.size()))
					best = &c;
			return best;
		}
	}

	const char* custom_light_area(const std::string& light_name)
	{
		const auto* c = claim_of(light_name);
		return c ? c->area.c_str() : nullptr;
	}

	bool custom_light_away(const std::string& light_name)
	{
		const auto* c = claim_of(light_name);
		return c && c->away;
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
			if (a.when < 0 || (a.when && (a.when == 1) != atmosphere::ambient_is_cold()))
				continue;
			if (a.has_box)
			{
				// No player position (a menu, a load): the place is not known, so a box does not match.
				if (!have_position)
					continue;
				if (!a.contains(position))
					continue;
			}
			active.push_back(a.name);
			for (const auto& [key, value] : a.values)
				wanted[key] = value;
		}

		build_light_claims(map, active);
		if (active == g_active && wanted == g_applied)
			return;
		if (active != g_active)
			shared::common::log("AreaOverrides", std::format("area: {} (was {})", join(active), join(g_active)), shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		g_active = active;

		bool proxy_changed = false;
		for (const auto& [key, value] : g_applied)
			if (!wanted.contains(key))
			{
				// Empty: the runtime takes the value off its Remix API layer, and the config files' value applies again.
				if (key.rfind("rtx.", 0) == 0)
					set_rtx(key, "");
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
			frustum_culling::reread_light_blockers();
			if (auto* gl = comp::game_lights::get())
				gl->reread_settings();
		}
	}
}
