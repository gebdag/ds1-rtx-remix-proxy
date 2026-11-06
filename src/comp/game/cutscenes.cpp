#include "std_include.hpp"
#include "cutscenes.hpp"
#include "debug_tools.hpp"
#include "fps_unlock.hpp"
#include "map_lights.hpp"

#include <fstream>
#include <map>
#include <sstream>

using shared::common::log;
using shared::common::LOG_TYPE;

namespace comp::game::cutscenes
{
	namespace
	{
		// RemoManStep: the singleton, and its request (thiscall: scene, flags, callback, user data, unused, player).
		constexpr DWORD kRemoMan = 0x01378490;
		constexpr DWORD kRequest = 0x00EE1950;
		constexpr BYTE kRequestStart[] = { 0x6A, 0xFF, 0x68, 0xFD, 0x5D, 0x05, 0x01, 0x64 };
		// What the game's own request from a script checks first, and the local player it plays for.
		constexpr DWORD kEventMan = 0x0137D790;
		constexpr DWORD kWorldChrMan = 0x0137D644;
		// The event scripts' update, in its vtable: the thread scripts request scenes in. It runs for a script that is waiting
		// as well, where the call of its instruction dispatcher inside (0xBBF468) is only reached when a script has work:
		// standing in a map with nothing to do, a scene asked for waited for ever.
		constexpr DWORD kScriptUpdateSlot = 0x011DCDAC;
		constexpr DWORD kScriptUpdate = 0x00BBF3B0;

		struct scene
		{
			int id;
			std::string map;
			int event;
			bool once;
		};

		std::map<std::string, std::vector<scene>> s_places;
		bool s_available = false;
		bool s_hooked = false;
		volatile LONG s_pending = 0;
		int s_manual = 100000;
		int s_last = 0;
		bool s_last_ok = true;
		DWORD s_script_update = kScriptUpdate;

		bool read_dword(DWORD addr, DWORD& out)
		{
			__try { out = *reinterpret_cast<const DWORD*>(addr); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		bool in_game()
		{
			DWORD chr = 0, player = 0, events = 0;
			return read_dword(kEventMan, events) && events && read_dword(kWorldChrMan, chr) && chr &&
				read_dword(chr + 0x3C, player) && player;
		}

		bool request(int id)
		{
			using request_fn = char(__thiscall*)(void* self, int scene, unsigned flags, void* callback, void* user, int unused, int player);
			__try
			{
				void* man = *reinterpret_cast<void**>(kRemoMan);
				return man && in_game() && reinterpret_cast<request_fn>(kRequest)(man, id, 0, nullptr, nullptr, 0, -1);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		void __stdcall on_event_instruction()
		{
			if (!s_pending)
				return;
			const int id = InterlockedExchange(&s_pending, 0);
			if (id)
			{
				s_last_ok = request(id);
				log("Cutscenes", std::format("Scene {} {}", id, s_last_ok ? "requested" : "refused"));
			}
		}

		__declspec(naked) void event_hook()
		{
			__asm {
				pushad
				pushfd
				call on_event_instruction
				popfd
				popad
				jmp s_script_update
			}
		}

		// Only the first scene asked for puts the hook in: without one, the game runs as shipped.
		bool hook()
		{
			if (s_hooked)
				return true;
			const DWORD target = reinterpret_cast<DWORD>(&event_hook);
			s_hooked = debug_tools::write_code(kScriptUpdateSlot, reinterpret_cast<const BYTE*>(&target), sizeof(target));
			return s_hooked;
		}

		void play(int id)
		{
			if (!hook())
			{
				log("Cutscenes", "Could not hook the event system", LOG_TYPE::LOG_TYPE_ERROR, true);
				return;
			}
			s_last = id;
			s_last_ok = true;
			InterlockedExchange(&s_pending, id);
		}

		void load_table()
		{
			std::ifstream file(shared::globals::root_path + "\\rtx_comp\\cutscenes.tsv");
			std::string line;
			while (std::getline(file, line))
			{
				if (line.empty() || line[0] == '#')
					continue;
				if (line.back() == '\r')
					line.pop_back();
				std::istringstream in(line);
				scene s {};
				std::string place;
				int once = 0;
				in >> s.id >> s.map >> std::ws;
				std::getline(in, place, '\t');
				in >> s.event >> once;
				if (in && s.id > 0)
				{
					s.once = once != 0;
					s_places[place].push_back(s);
				}
			}
			size_t count = 0;
			for (const auto& p : s_places)
				count += p.second.size();
			log("Cutscenes", std::format("{} scenes from rtx_comp\\cutscenes.tsv", count));
		}
	}

	void init()
	{
		if (shared::globals::lookdev_host)
			return;
		s_available = std::memcmp(reinterpret_cast<const void*>(kRequest), kRequestStart, sizeof(kRequestStart)) == 0 &&
			*reinterpret_cast<const DWORD*>(kScriptUpdateSlot) == kScriptUpdate;
		if (!s_available)
		{
			log("Cutscenes", "Unexpected bytes in the game: cutscene playback unavailable", LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		load_table();
	}

	void toggle_freeze()
	{
		fps_unlock::set_frozen(!fps_unlock::is_frozen());
		log("Cutscenes", fps_unlock::is_frozen() ? "Game frozen" : "Game running");
	}

	void tab_imgui()
	{
		bool frozen = fps_unlock::is_frozen();
		if (ImGui::Checkbox("Freeze Game (F7)", &frozen))
			fps_unlock::set_frozen(frozen);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Stops the game's time, in a cutscene too, while rendering goes on: to capture one frame with F9.\n"
				"Things the path tracer runs on its own clock (ripples, drips) keep going.");

		ImGui::Spacing();
		if (!s_available)
		{
			ImGui::TextDisabled("Cutscene playback unavailable (this build's code differs)");
			return;
		}

		ImGui::TextWrapped("Plays a scene wherever you are; nothing it plays is marked as seen. A scene shows the map that is loaded, so go to "
			"its map first (green in the list). Characters the event would set up may be missing.");
		if (!in_game())
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Load a character first.");
		if (s_pending)
			ImGui::TextDisabled("Scene %d waiting for the event system", static_cast<int>(s_pending));
		else if (s_last)
			ImGui::TextDisabled("Last: scene %d%s", s_last, s_last_ok ? "" : " (refused)");

		ImGui::SetNextItemWidth(160.0f);
		ImGui::InputInt("##scene", &s_manual, 0, 0);
		ImGui::SameLine();
		if (ImGui::Button("Play scene number"))
			play(s_manual);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Area, block and cut: 180130 is area 18, block 1, cut 30.");

		ImGui::Spacing();
		const std::string here = map_lights::current_map_name();
		for (const auto& [place, scenes] : s_places)
		{
			const bool on_this_map = std::any_of(scenes.begin(), scenes.end(), [&](const scene& s) { return s.map == here; });
			ImGui::PushID(place.c_str());
			if (on_this_map)
				ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
			if (ImGui::CollapsingHeader(std::format("{} ({})", place, scenes.size()).c_str()))
			{
				for (const auto& s : scenes)
				{
					ImGui::PushID(s.id);
					if (ImGui::SmallButton("Play"))
						play(s.id);
					ImGui::SameLine();
					const std::string text = std::format("{}  {}{}{}", s.id, s.map, s.once ? "  plays once" : "", s.event ? "" : "  no map script");
					if (s.map == here)
						ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.45f, 1.0f), "%s", text.c_str());
					else
						ImGui::TextUnformatted(text.c_str());
					if (s.event && ImGui::IsItemHovered())
						ImGui::SetTooltip("Event %d on %s", s.event, s.map.c_str());
					ImGui::PopID();
				}
			}
			ImGui::PopID();
		}
	}
}
