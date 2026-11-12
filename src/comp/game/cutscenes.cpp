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

		// --- Smooth cutscene camera ---
		// A cutscene's camera (.sibcam) has one key per 30 Hz frame and remo samples it at its continuous time, but through the
		// nearest-key sampler 0xD09DA0: at 60 fps every camera pose is held for two frames while the characters move every frame
		// (judder in slow pans). The engine's own Hermite sampler 0xD0A1F0 (same registers and stack: EDX track, XMM0 time,
		// stack out and dat) interpolates with the authored tangents; at 30 fps its samples land on the keys, the same poses.
		// Some cameras cut between shots on two consecutive keys: there the original snap is kept. The four keys around the time
		// are sampled first, with every register as at the original call (this code's convention is the compiler's own); the
		// step between the middle two is a cut when it is far larger than the steps either side of it (a fast camera move
		// changes speed gradually, a cut jumps once).
		constexpr DWORD kCameraCall = 0x00FC2084;   // in the SibcamIns sample 0xFC2060, used by remo cut start and update only
		constexpr BYTE kCameraCallBytes[] = { 0xE8, 0x17, 0x7D, 0xD4, 0xFF };
		constexpr DWORD kNearestSampler = 0x00D09DA0;
		constexpr DWORD kHermiteSampler = 0x00D0A1F0;
		constexpr float kKeysPerSecond = 30.0f;
		constexpr int kCameraKeys = 4;            // the key before, the two around the time, the key after
		constexpr float kCutRatio = 4.0f;         // a cut's step against the larger of its neighbours'
		constexpr float kCutMinDistance = 0.5f;   // metres: smaller steps are never cuts
		constexpr float kCutMinTurn = 0.03f;      // 1 - cos of an axis' turn (about 14 degrees): smaller turns are never cuts

		bool s_smooth_camera = true;
		bool s_camera_patched = false;
		// Shared with the naked thunk (game thread only).
		DWORD s_camera_target = kNearestSampler;
		DWORD s_nearest_ptr = kNearestSampler;
		DWORD s_sample_track = 0, s_sample_dat = 0, s_sample_esp = 0, s_sample_index = 0;
		float s_sample_time = 0.0f;
		float s_key_times[kCameraKeys] = {};
		alignas(16) float s_poses[kCameraKeys][16] = {};
		// Samples of the current scene and of the one before (a second without a sample ends a scene), for the F4 tab.
		unsigned s_interpolated = 0, s_snapped = 0, s_last_interpolated = 0, s_last_snapped = 0;
		ULONGLONG s_last_sample_ms = 0;

		void __cdecl camera_key_times()
		{
			const float lo = std::floor(s_sample_time * kKeysPerSecond) / kKeysPerSecond;
			for (int k = 0; k < kCameraKeys; ++k)
				s_key_times[k] = lo + static_cast<float>(k - 1) / kKeysPerSecond;
		}

		// How far a pose moves to the next: its position step, and the largest 1 - cos of an axis' turn (rows 0-2 axes, row 3
		// position: the matrix 0x415910 builds). A degenerate axis counts as a full turn.
		void pose_step(const float* a, const float* b, float& distance, float& turn)
		{
			const float dx = a[12] - b[12], dy = a[13] - b[13], dz = a[14] - b[14];
			distance = std::sqrt(dx * dx + dy * dy + dz * dz);
			turn = 0.0f;
			for (int row = 0; row < 3; ++row)
			{
				const float* u = a + row * 4;
				const float* v = b + row * 4;
				const float lu = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
				const float lv = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
				const float c = lu > 1e-6f && lv > 1e-6f ? (u[0] * v[0] + u[1] * v[1] + u[2] * v[2]) / (lu * lv) : -1.0f;
				turn = std::max(turn, 1.0f - c);
			}
		}

		void __cdecl camera_choose()
		{
			float distance[kCameraKeys - 1], turn[kCameraKeys - 1];
			for (int k = 0; k + 1 < kCameraKeys; ++k)
				pose_step(s_poses[k], s_poses[k + 1], distance[k], turn[k]);
			const bool cut = distance[1] > std::max(kCutMinDistance, kCutRatio * std::max(distance[0], distance[2])) ||
				turn[1] > std::max(kCutMinTurn, kCutRatio * std::max(turn[0], turn[2]));
			s_camera_target = cut ? kNearestSampler : kHermiteSampler;

			const ULONGLONG now = GetTickCount64();
			if (s_last_sample_ms && now - s_last_sample_ms > 1000)
			{
				s_last_interpolated = s_interpolated;
				s_last_snapped = s_snapped;
				s_interpolated = s_snapped = 0;
			}
			s_last_sample_ms = now;
			++(cut ? s_snapped : s_interpolated);
		}

		// Replaces the call at 0xFC2084. Entry: EDX track, XMM0 time, [esp+4] out, [esp+8] dat. The four keys go through the
		// original sampler into our own poses, each call with the entry registers restored (pushad block at +8+16 after its two
		// pushes) and ESP put back after it whatever it pops; then everything is restored and the chosen sampler is jumped to with
		// the original stack, so it returns straight to the caller.
		__declspec(naked) void camera_thunk()
		{
			__asm
			{
				pushad
				sub esp, 16
				movups [esp], xmm0
				movss s_sample_time, xmm0
				mov s_sample_track, edx
				mov eax, [esp + 56]
				mov s_sample_dat, eax
				call camera_key_times

				mov s_sample_esp, esp
				mov s_sample_index, 0
			next_key:
				mov eax, s_sample_index
				shl eax, 6
				lea eax, s_poses[eax]
				push s_sample_dat
				push eax
				mov eax, s_sample_index
				movss xmm0, s_key_times[eax * 4]
				mov edi, [esp + 24]
				mov esi, [esp + 28]
				mov ebp, [esp + 32]
				mov ebx, [esp + 40]
				mov ecx, [esp + 48]
				mov eax, [esp + 52]
				mov edx, s_sample_track
				call dword ptr [s_nearest_ptr]
				mov esp, s_sample_esp
				inc s_sample_index
				cmp s_sample_index, 4
				jb next_key

				call camera_choose

				movups xmm0, [esp]
				add esp, 16
				popad
				jmp dword ptr [s_camera_target]
			}
		}

		bool write_call(DWORD target)
		{
			BYTE bytes[5] = { 0xE8 };
			const DWORD rel = target - (kCameraCall + 5);
			std::memcpy(bytes + 1, &rel, sizeof(rel));
			DWORD old = 0;
			if (!VirtualProtect(reinterpret_cast<void*>(kCameraCall), sizeof(bytes), PAGE_EXECUTE_READWRITE, &old))
				return false;
			std::memcpy(reinterpret_cast<void*>(kCameraCall), bytes, sizeof(bytes));
			VirtualProtect(reinterpret_cast<void*>(kCameraCall), sizeof(bytes), old, &old);
			FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(kCameraCall), sizeof(bytes));
			return true;
		}

		// Patched at start-up, before any scene plays: the call's displacement spans two 8-byte blocks, so it cannot be written
		// in one atomic store while the game might run it. Switching it later only takes effect from the next start.
		void apply_smooth_camera()
		{
			if (s_camera_patched || !s_smooth_camera)
				return;
			if (std::memcmp(reinterpret_cast<const void*>(kCameraCall), kCameraCallBytes, sizeof(kCameraCallBytes)) != 0)
			{
				log("Cutscenes", "Unexpected bytes at the cutscene camera call: smooth camera unavailable", LOG_TYPE::LOG_TYPE_WARN, true);
				return;
			}
			s_camera_patched = write_call(reinterpret_cast<DWORD>(&camera_thunk));
			log("Cutscenes", s_camera_patched ? "Smooth cutscene camera on" : "Could not patch the cutscene camera call");
		}
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
		s_smooth_camera = shared::common::config::get().get_bool("Cutscenes", "SmoothCamera", true);
		apply_smooth_camera();
		s_available = std::memcmp(reinterpret_cast<const void*>(kRequest), kRequestStart, sizeof(kRequestStart)) == 0 &&
			*reinterpret_cast<const DWORD*>(kScriptUpdateSlot) == kScriptUpdate;
		if (!s_available)
		{
			log("Cutscenes", "Unexpected bytes in the game: cutscene playback unavailable", LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		load_table();
	}

	bool is_playing(int id)
	{
		// RemoMan: +0x28 four slots of 0x18 bytes, a queue filled by the request 0xEE1950. slot[0] is the scene playing (its
		// start 0xEE1EF0 reads only slot[0]), later slots wait; a finished scene leaves and the rest move up (0xEE1D20). Each
		// slot is a task whose +0x18 is the scene instance, its id at +0x3C. +0xE8 is set from the scene's load to its
		// restore step and cleared with the slots (0xEE1C30); the event conditions read it (0xBB1AB0).
		constexpr DWORD kBusy = 0xE8, kPlayingSlot = 0x28, kTaskScene = 0x18, kSceneId = 0x3C;
		if (!s_available)
			return false;
		__try
		{
			const auto man = *reinterpret_cast<const BYTE* const*>(kRemoMan);
			if (!man || !man[kBusy])
				return false;
			const auto task = *reinterpret_cast<const BYTE* const*>(man + kPlayingSlot);
			const auto scene = task ? *reinterpret_cast<const BYTE* const*>(task + kTaskScene) : nullptr;
			return scene && *reinterpret_cast<const int*>(scene + kSceneId) == id;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
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

		if (ImGui::Checkbox("Smooth Cutscene Camera (from the next start)", &s_smooth_camera))
			shared::common::config::get().set_string("Cutscenes", "SmoothCamera", s_smooth_camera ? "1" : "0");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Cutscene cameras have 30 keys a second; at 60 fps the game holds each for two frames (judder in slow pans).\n"
				"On: the game's own interpolating camera sampler between keys, hard cuts kept. Saved as [Cutscenes] SmoothCamera;\n"
				"a change takes effect when the game starts.");
		ImGui::SameLine();
		ImGui::TextDisabled(s_camera_patched ? "(in use this session)" : "(not in use this session)");
		if (s_camera_patched && (s_interpolated || s_snapped || s_last_interpolated || s_last_snapped))
			ImGui::TextDisabled("Camera samples: this scene %u interpolated, %u kept on a key (cuts); last scene %u / %u",
				s_interpolated, s_snapped, s_last_interpolated, s_last_snapped);

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
