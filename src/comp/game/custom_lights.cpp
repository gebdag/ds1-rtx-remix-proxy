#include "std_include.hpp"
#include "custom_lights.hpp"
#include "map_lights.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
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
		bool g_dirty = false;

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
			std::string word;
			while (in >> word)
			{
				if (word == "spot")
				{
					l.spot = static_cast<bool>(in >> l.dir[0] >> l.dir[1] >> l.dir[2] >> l.cone_deg >> l.softness);
				}
				else if (word == "off")
				{
					l.enabled = false;
				}
			}
			return true;
		}

		void load()
		{
			g_lights.clear();
			g_dirty = false;
			g_loaded = true;
			g_map = map_lights::current_map_name();
			if (g_map.empty())
			{
				g_path.clear();
				g_status = "no map loaded";
				return;
			}
			g_path = shared::globals::root_path + "\\rtx_comp\\lights\\" + g_map + ".custom.txt";
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
			g_status = rejected ? std::format("{} light(s), {} malformed line(s) skipped", g_lights.size(), rejected)
				: std::format("{} light(s)", g_lights.size());
			shared::common::log("CustomLights", std::format("{}: {}", g_path, g_status));
		}

		void ensure_current()
		{
			// A map change drops unsaved edits of the old map: they belong to its file, not the new one's.
			if (!g_loaded || map_lights::current_map_name() != g_map)
				load();
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
			if (!l.enabled)
				continue;
			point_lights::light p {};
			p.slot = kFirstSlot + static_cast<int>(i);
			p.x = l.pos[0]; p.y = l.pos[1]; p.z = l.pos[2];
			p.r = l.colour[0] * l.intensity;
			p.g = l.colour[1] * l.intensity;
			p.b = l.colour[2] * l.intensity;
			p.dwindle_begin = l.dwindle_begin;
			p.dwindle_end = l.dwindle_end;
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
		std::ofstream file(g_path, std::ios::trunc);
		if (!file)
		{
			g_status = "could not write " + g_path;
			return false;
		}
		file << "# Hand-placed lights for " << g_map << " (F4 > Lights > Custom lights writes this file).\n"
			<< "# x y z  dwindleBegin dwindleEnd  r g b  intensityPct  name  [spot dx dy dz coneDeg softness]  [off]\n";
		for (const auto& l : g_lights)
		{
			auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
			std::string name = l.name.empty() ? "light" : l.name;
			std::replace(name.begin(), name.end(), ' ', '_');
			file << std::format("{:.3f} {:.3f} {:.3f}  {:.3f} {:.3f}  {} {} {}  {:.0f}  {}", l.pos[0], l.pos[1], l.pos[2],
				l.dwindle_begin, l.dwindle_end, byte(l.colour[0]), byte(l.colour[1]), byte(l.colour[2]), l.intensity * 100.0f, name);
			if (l.spot)
				file << std::format("  spot {:.4f} {:.4f} {:.4f} {:.1f} {:.2f}", l.dir[0], l.dir[1], l.dir[2], l.cone_deg, l.softness);
			if (!l.enabled)
				file << "  off";
			file << '\n';
		}
		g_dirty = false;
		g_status = std::format("{} light(s) saved", g_lights.size());
		return true;
	}

	void reload() { load(); }
	const std::string& path() { ensure_current(); return g_path; }
	const std::string& status() { return g_status; }
	bool dirty() { return g_dirty; }
	void mark_dirty() { g_dirty = true; }
}
