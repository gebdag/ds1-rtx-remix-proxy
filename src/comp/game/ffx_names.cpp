#include "std_include.hpp"
#include "ffx_names.hpp"

#include <fstream>
#include <string>
#include <unordered_map>

namespace comp::game::ffx_names
{
	namespace
	{
		std::unordered_map<int, std::string> g_names;
		bool g_loaded = false;

		void load()
		{
			g_loaded = true;
			std::ifstream file(shared::globals::root_path + "\\rtx_comp\\ffx_names.tsv");
			std::string line;
			while (std::getline(file, line))
			{
				const size_t tab = line.find('\t');
				if (line.empty() || line[0] == '#' || tab == std::string::npos)
					continue;
				std::string text = line.substr(tab + 1);
				if (!text.empty() && text.back() == '\r')
					text.pop_back();
				g_names[std::atoi(line.c_str())] = std::move(text);
			}
		}
	}

	const char* name(int ffx_id)
	{
		if (!g_loaded)
			load();
		const auto it = g_names.find(ffx_id);
		return it != g_names.end() ? it->second.c_str() : nullptr;
	}
}
