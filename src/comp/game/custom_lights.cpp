#include "std_include.hpp"
#include "custom_lights.hpp"
#include "area_overrides.hpp"
#include "atmosphere.hpp"
#include "map_lights.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

namespace comp::game::custom_lights
{
	namespace
	{
		std::vector<light> g_lights;
		std::vector<point_lights::light> g_out;
		std::string g_map;
		std::string g_path;
		std::string g_status = "no map loaded";
		bool g_loaded = false;
		unsigned int g_generation = 0;
		bool g_dirty = false;
		std::vector<light> g_saved; // the file's lights, by file_index

		// A map left with unsaved edits keeps them here until the player is back in it (or Save writes them, or they are
		// thrown away): crossing a map boundary that is easy to reach (Undead Parish to Firelink Shrine's lift) used to drop
		// lights placed and not yet saved (2026-11-12, three lights lost).
		struct kept_map
		{
			std::vector<light> lights, saved;
			std::string path;
		};
		std::map<std::string, kept_map> g_kept;

		std::string file_path(const std::string& map)
		{
			return shared::globals::root_path + "\\rtx_comp\\lights\\" + map + ".custom.txt";
		}

		void remember_saved()
		{
			for (size_t i = 0; i < g_lights.size(); ++i)
				g_lights[i].file_index = static_cast<int>(i);
			g_saved = g_lights;
		}

		bool parse(const std::string& line, light& l)
		{
			std::istringstream in(line);
			int r = 0, g = 0, b = 0;
			float pct = 0.0f;
			if (!(in >> l.pos[0] >> l.pos[1] >> l.pos[2] >> l.dwindle_begin >> l.dwindle_end >> r >> g >> b >> pct >> l.name))
				return false;
			if (!std::isfinite(l.pos[0] + l.pos[1] + l.pos[2]) || !(l.dwindle_end > 0.0f))
				return false;
			l.colour[0] = std::clamp(r, 0, 255) / 255.0f;
			l.colour[1] = std::clamp(g, 0, 255) / 255.0f;
			l.colour[2] = std::clamp(b, 0, 255) / 255.0f;
			l.intensity = pct * 0.01f;
			// A line without the remix tail predates Remix lights here: it keeps the game's falloff.
			l.game_falloff = true;
			std::string word;
			while (in >> word)
			{
				if (word == "remix")
				{
					l.game_falloff = !(in >> l.remix_intensity >> l.exposure >> l.radius >> l.volumetric);
				}
				else if (word == "game")
				{
					l.game_falloff = true;
				}
				else if (word == "spot")
				{
					l.spot = static_cast<bool>(in >> l.dir[0] >> l.dir[1] >> l.dir[2] >> l.cone_deg >> l.softness);
				}
				else if (word == "focus")
				{
					in >> l.focus;
				}
				else if (word == "off")
				{
					l.enabled = false;
				}
				else if (word == "dark")
				{
					l.when = 1;
				}
				else if (word == "light")
				{
					l.when = 2;
				}
			}
			return true;
		}

		// The game thread writes the map id: callers read the name once and pass it, so the g_kept lookup and the load
		// agree on the map.
		void load(const std::string& map)
		{
			g_lights.clear();
			g_saved.clear();
			g_dirty = false;
			g_loaded = true;
			++g_generation;
			g_map = map;
			if (g_map.empty())
			{
				g_path.clear();
				g_status = "no map loaded";
				return;
			}
			g_path = file_path(g_map);
			std::ifstream file(g_path);
			if (!file)
			{
				g_status = "no custom lights for this map";
				return;
			}
			int rejected = 0;
			std::string line;
			while (std::getline(file, line))
			{
				const auto first = line.find_first_not_of(" \t\r\n");
				if (first == std::string::npos || line[first] == '#')
					continue;
				light l;
				if (parse(line, l))
					g_lights.push_back(l);
				else
					++rejected;
			}
			remember_saved();
			g_status = rejected ? std::format("{} light(s), {} malformed line(s) skipped", g_lights.size(), rejected)
				: std::format("{} light(s)", g_lights.size());
			shared::common::log("CustomLights", std::format("{}: {}", g_path, g_status));
		}

		void ensure_current()
		{
			const std::string map = map_lights::current_map_name(); // once: the game thread writes it
			if (g_loaded && map == g_map)
				return;
			// Leaving a map with unsaved edits: they are kept for it, not dropped (they belong to its file, not the new one's).
			if (g_loaded && g_dirty && !g_map.empty())
			{
				g_kept[g_map] = { g_lights, g_saved, g_path };
				shared::common::log("CustomLights", std::format("{}: {} light(s) with unsaved changes kept until you are back or save",
					g_map, g_lights.size()));
			}
			const auto it = g_kept.find(map);
			if (map.empty() || it == g_kept.end())
			{
				load(map);
				return;
			}
			// Back in a map left with unsaved edits: those, not the file.
			g_lights = std::move(it->second.lights);
			g_saved = std::move(it->second.saved);
			g_path = std::move(it->second.path);
			g_kept.erase(it);
			g_map = map;
			g_dirty = true;
			g_loaded = true;
			++g_generation;
			g_status = std::format("{} light(s), unsaved changes kept from before", g_lights.size());
			shared::common::log("CustomLights", std::format("{}: {}", map, g_status));
		}

		bool write_file(const std::string& path, const std::string& map, const std::vector<light>& lights)
		{
			std::ofstream file(path, std::ios::trunc);
			if (!file)
				return false;
			file << "# Hand-placed lights for " << map << " (F4 > Lights > Custom lights writes this file).\n"
				<< "# x y z  dwindleBegin dwindleEnd  r g b  intensityPct  name  [remix intensity exposure radius volumetric]\n"
				<< "#   [spot dx dy dz coneDeg softness]  [focus exponent]  [off]\n"
				<< "# With 'remix' a plain Remix sphere light; without, the game's falloff (dwindle begin/end, intensityPct).\n";
			for (const auto& l : lights)
			{
				auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
				std::string name = l.name.empty() ? "light" : l.name;
				std::replace(name.begin(), name.end(), ' ', '_');
				file << std::format("{:.3f} {:.3f} {:.3f}  {:.3f} {:.3f}  {} {} {}  {:.0f}  {}", l.pos[0], l.pos[1], l.pos[2],
					l.dwindle_begin, l.dwindle_end, byte(l.colour[0]), byte(l.colour[1]), byte(l.colour[2]), l.intensity * 100.0f, name);
				if (!l.game_falloff)
					file << std::format("  remix {:.3f} {:.2f} {:.3f} {:.2f}", l.remix_intensity, l.exposure, l.radius, l.volumetric);
				if (l.spot)
					file << std::format("  spot {:.4f} {:.4f} {:.4f} {:.1f} {:.2f}", l.dir[0], l.dir[1], l.dir[2], l.cone_deg, l.softness);
				if (l.spot && !l.game_falloff && l.focus > 0.0f)
					file << std::format("  focus {:.2f}", l.focus);
				if (!l.enabled)
					file << "  off";
				if (l.when)
					file << (l.when == 1 ? "  dark" : "  light");
				file << '\n';
			}
			return static_cast<bool>(file);
		}
	}

	std::vector<light>& edit()
	{
		ensure_current();
		return g_lights;
	}

	const std::vector<point_lights::light>& get()
	{
		ensure_current();
		g_out.clear();
		for (size_t i = 0; i < g_lights.size(); ++i)
		{
			const auto& l = g_lights[i];
			if (!l.enabled || (l.when && (l.when == 1) != atmosphere::ambient_is_cold()))
				continue;
			// A light that belongs to an area of this map (area_overrides.ini, `lights = <name prefix>`) is there only while the player is.
			if (area_overrides::custom_light_away(l.name))
				continue;
			point_lights::light p {};
			p.slot = kFirstSlot + static_cast<int>(i);
			p.x = l.pos[0]; p.y = l.pos[1]; p.z = l.pos[2];
			const float scale = l.game_falloff ? l.intensity : l.remix_intensity * std::exp2(l.exposure);
			p.r = l.colour[0] * scale;
			p.g = l.colour[1] * scale;
			p.b = l.colour[2] * scale;
			p.dwindle_begin = l.dwindle_begin;
			p.dwindle_end = l.dwindle_end;
			p.physical = !l.game_falloff;
			p.emitter_radius = std::max(0.001f, l.radius);
			p.spot_focus = std::max(0.0f, l.focus);
			p.volumetric_scale = std::max(0.0f, l.volumetric);
			p.has_params = true;
			p.source = point_lights::param_source::custom;
			if (l.spot)
			{
				const float len = std::sqrt(l.dir[0] * l.dir[0] + l.dir[1] * l.dir[1] + l.dir[2] * l.dir[2]);
				if (len > 0.0f)
				{
					for (int c = 0; c < 3; ++c)
						p.spot_dir[c] = l.dir[c] / len;
					p.spot_cone_deg = std::clamp(l.cone_deg, 1.0f, 89.0f);
					p.spot_softness = std::clamp(l.softness, 0.0f, 1.0f);
				}
			}
			g_out.push_back(p);
		}
		return g_out;
	}

	bool save()
	{
		ensure_current();
		if (g_path.empty())
			return false;
		if (!write_file(g_path, g_map, g_lights))
		{
			g_status = "could not write " + g_path;
			return false;
		}
		g_dirty = false;
		remember_saved();
		g_status = std::format("{} light(s) saved", g_lights.size());
		return true;
	}

	void reload()
	{
		ensure_current(); // a map we are back in leaves g_kept first, so the reload below drops it for good
		load(g_map);
	}

	std::vector<std::pair<std::string, size_t>> kept()
	{
		std::vector<std::pair<std::string, size_t>> out;
		for (const auto& [map, k] : g_kept)
			out.emplace_back(map, k.lights.size());
		return out;
	}

	bool save_kept(const std::string& map)
	{
		const auto it = g_kept.find(map);
		if (it == g_kept.end())
			return true;
		if (!write_file(it->second.path, map, it->second.lights))
		{
			shared::common::log("CustomLights", "could not write " + it->second.path, shared::common::LOG_TYPE::LOG_TYPE_ERROR);
			return false;
		}
		shared::common::log("CustomLights", std::format("{}: {} light(s) saved from another map", map, it->second.lights.size()));
		g_kept.erase(it);
		return true;
	}

	void discard_kept(const std::string& map) { g_kept.erase(map); }
	unsigned int generation() { ensure_current(); return g_generation; }

	const light* saved(const light& l)
	{
		return l.file_index >= 0 && l.file_index < static_cast<int>(g_saved.size()) ? &g_saved[l.file_index] : nullptr;
	}
	const std::string& path() { ensure_current(); return g_path; }
	const std::string& status() { return g_status; }
	bool dirty() { return g_dirty; }
	void mark_dirty() { g_dirty = true; }
}
