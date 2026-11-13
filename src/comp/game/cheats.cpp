#include "std_include.hpp"
#include "cheats.hpp"
#include "debug_tools.hpp"
#include "shared/common/config.hpp"
#include "map_lights.hpp"

#include <fstream>
#include <sstream>

using shared::common::log;
using shared::common::LOG_TYPE;

namespace comp::game::cheats
{
	struct bonfire_entry
	{
		int id;
		const char* name;
		bool has_coords;
		float x;
		float y;
		float z;
	};

	static constexpr bonfire_entry s_bonfires[] = {
		{ 1812100, "Undead Asylum (Prison Cell)", false, 0.0f, 0.0f, 0.0f },
		{ 1812900, "Undead Asylum (Prison Cell)", true, -13.750f, 184.750f, -44.500f },
		{ 1812960, "Undead Asylum (Bonfire #1)", true, 3.100f, 195.960f, 5.750f },
		{ 1812961, "Undead Asylum (Bonfire #2)", true, 33.900f, 193.150f, -25.200f },
		{ 1022960, "Firelink Shrine", true, -47.410f, -59.820f, 53.300f },
		{ 1802960, "Firelink Altar", true, 50.300f, -63.270f, 106.100f },
		{ 1012961, "Sunlight Altar", true, 23.100f, 9.500f, -24.400f },
		{ 1012962, "Undead Burg", true, 0.025f, -10.020f, -59.303f },
		{ 1012964, "Undead Parish", true, 88.000f, 15.000f, 106.000f },
		{ 1202961, "Forest Covenant", true, 255.500f, -3.350f, -12.150f },
		{ 1602961, "Darkroot Basin", true, 163.500f, -76.760f, -54.000f },
		{ 1002900, "Depths (Entrance)", false, 0.0f, 0.0f, 0.0f },
		{ 1002950, "Depths (Gaping Dragon's Room)", true, -170.990f, -99.650f, 0.000f },
		{ 1002960, "Depths", true, -120.500f, -74.140f, 14.200f },
		{ 1402962, "Blighttown (Upper)", true, -275.500f, -136.730f, 73.000f },
		{ 1402961, "Blighttown (Lower)", true, -199.500f, -214.710f, 98.500f },
		{ 1412961, "Demon Ruins (Upper)", true, 193.000f, -267.200f, 130.000f },
		{ 1412962, "Demon Ruins (Lower)", true, 253.900f, -334.030f, 23.000f },
		{ 1412963, "Demon Ruins (Even lower)", true, 117.000f, -356.590f, 140.300f },
		{ 1402960, "Quelagg's Domain (Daughter of Chaos)", true, 143.340f, -252.520f, 90.130f },
		{ 1322962, "Great Hollow", true, -316.500f, -236.030f, 123.000f },
		{ 1322961, "Ash Lake", true, -386.950f, -408.400f, 156.850f },
		{ 1322960, "Ash Lake  (Everlasting Dragon Bonfire)", true, -699.000f, -413.850f, -319.250f },
		{ 1502961, "Sen's Fortress", true, 73.500f, 60.000f, 301.000f },
		{ 1512970, "Anor Londo (Entrance)", false, 0.0f, 0.0f, 0.0f },
		{ 1512960, "Anor Londo  (1st bonfire)", true, 172.800f, 173.400f, 255.000f },
		{ 1512961, "Anor Londo (Solaire)", true, 506.000f, 135.000f, 176.600f },
		{ 1512950, "Chamber of the Princess", true, 591.000f, 161.000f, 255.000f },
		{ 1512962, "Darkmoon Tomb", true, 388.500f, 69.800f, 255.000f },
		{ 1102960, "Painted World of Ariamis", true, -24.000f, 51.760f, 946.500f },
		{ 1601354, "New Londo Ruins (Entrance)", false, 0.0f, 0.0f, 0.0f },
		{ 1602960, "New Londo Ruins (middle of the level)", true, 52.070f, -142.400f, 32.370f },
		{ 1602950, "The Abyss", true, 80.000f, -311.000f, 3.100f },
		{ 1412964, "Lost Izalith (First Bonfire)", true, 227.500f, -383.520f, 89.500f },
		{ 1412960, "Lost Izalith (Hidden Bonfire)", true, 455.800f, -380.030f, 171.600f },
		{ 1412950, "Bed Of Chaos (lord bonfire)", true, 580.838f, -443.939f, 444.344f },
		{ 1302960, "Catacombs (1st bonfire)", true, 45.500f, -119.080f, 205.850f },
		{ 1302961, "Catacombs (2nd bonfire)", true, 46.500f, -111.900f, 301.500f },
		{ 1312950, "The Catacombs-Lower Levels (Nito)", true, -157.000f, -264.900f, -34.000f },
		{ 1312961, "Tomb of the Giants (Patches Bonfire)", true, 98.000f, -199.860f, 104.000f },
		{ 1312960, "Tomb of the Giants (Deeper)", true, 76.300f, -214.310f, 45.750f },
		{ 1702960, "Dukes Archives (Bonfire 2)", true, 349.110f, 277.590f, 593.340f },
		{ 1702961, "Duke's Archives (Jail)", true, 377.000f, 269.780f, 555.250f },
		{ 1702900, "Duke's Archives (Jail)", true, 377.370f, 269.780f, 553.620f },
		{ 1702962, "Duke's Archives (Entrance lift)", true, 232.850f, 199.980f, 477.150f },
		{ 1702950, "Duke's Archives (Seath)", true, 100.000f, 134.050f, 861.000f },
		{ 1702950, "Crystal Cave", true, 100.000f, 134.050f, 861.000f },
		{ 1212963, "Sanctuary Garden", true, 894.500f, -329.330f, 451.500f },
		{ 1212961, "Oolacile Sanctuary", true, 971.700f, -313.700f, 580.300f },
		{ 1212962, "Oolacile Township", true, 1041.600f, -332.000f, 872.900f },
		{ 1212964, "Oolacile Township Dungeon", true, 863.320f, -447.500f, 913.130f },
		{ 1212950, "Chasm of the Abyss", true, 855.000f, -576.800f, 850.000f },
		{ 1801354, "Kiln of the First Flame", false, 0.0f, 0.0f, 0.0f },
		{ 1802130, "Lord Gwyn (Final boss)", true, 398.390f, -117.220f, 178.850f },
	};

	// AOB for "mov [ebp+0x2D4], eax" — the HP write instruction.
	// Full pattern: 89 85 D4 02 00 00 83 ...
	// The game writes the updated HP value (in eax) to the entity struct at [ebp+0x2D4].
	// ebp = entity pointer; [ebp+4] == 0 identifies the player character.
	static constexpr const char* HP_WRITE_AOB = "89 85 D4 02 00 00 83";
	static constexpr const char* TELEPORT_BASE_AOB = "8B 76 0C 89 35 ?? ?? ?? ?? 33 C0";
	static constexpr DWORD TELEPORT_BASE_AOB_OFFSET = 5;
	static constexpr const char* LAST_BONFIRE_BASE_AOB = "A1 ?? ?? ?? ?? 53 56 57 8B F9 8B 88 ?? ?? ?? ?? 8B";
	static constexpr DWORD LAST_BONFIRE_BASE_AOB_OFFSET = 1;
	static constexpr DWORD LAST_BONFIRE_VALUE_OFFSET = 0xB04;
	static constexpr DWORD TELEPORT_BLOCK_OFFSET = 0xE0;
	static constexpr DWORD TELEPORT_SECONDARY_BLOCK_OFFSET = 0x10;

	// The only call of the game's fall-damage function (FUN_00e7fef0(ChrIns*), stdcall), in its landing
	// handler: push edi (the falling ChrIns); call rel32. No Fall Damage points the call at a stub that
	// returns at once when the ChrIns is the player, so enemies still take fall damage.
	static constexpr const char* FALL_DAMAGE_CALL_AOB = "57 E8 ?? ?? ?? ?? F3 0F 10 44 24 08 F3 0F 5A C0";

	static bool s_available = false;
	static bool s_enabled = false;
	static DWORD s_teleport_base_addr = 0;
	static DWORD s_last_bonfire_base_addr = 0;
	static int s_last_bonfire_index = 0;
	static int s_teleport_index = 0;

	// No Fall Damage: the call site, its original bytes and the cave.
	static DWORD s_fall_call_addr = 0;
	static BYTE s_fall_orig_bytes[6] = {};
	static BYTE* s_fall_cave = nullptr;
	static bool s_no_fall_damage = false;

	// The code cave and original bytes
	static BYTE* s_cave = nullptr;
	static DWORD s_patch_addr = 0;     // address of the original 6-byte instruction
	static BYTE  s_orig_bytes[6] = {}; // saved original bytes for disable

	// Toggle flag read by the code cave at runtime
	static volatile LONG s_active = 0;

	static size_t bonfire_count()
	{
		return sizeof(s_bonfires) / sizeof(s_bonfires[0]);
	}

	static DWORD safe_read_ptr(DWORD addr)
	{
		if (!addr) return 0;
		__try {
			return *reinterpret_cast<DWORD*>(addr);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
	}

	static int safe_read_int(DWORD addr, int fallback = 0)
	{
		if (!addr) return fallback;
		__try {
			return *reinterpret_cast<int*>(addr);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return fallback;
		}
	}

	static bool safe_write_int(DWORD addr, int value)
	{
		if (!addr) return false;
		__try {
			*reinterpret_cast<int*>(addr) = value;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Enemies ignore the player: the game's own Player Hide (sight) and Player Silence (hearing) flags.
	static bool enemies_ignore_player()
	{
		return debug_tools::read_flag(debug_tools::kFlagPlayerHide) && debug_tools::read_flag(debug_tools::kFlagPlayerSilence);
	}

	static bool set_enemies_ignore_player(bool on)
	{
		return debug_tools::write_flag(debug_tools::kFlagPlayerHide, on) && debug_tools::write_flag(debug_tools::kFlagPlayerSilence, on);
	}

	// The No Fall Damage stub, called in place of the fall-damage function with the same stdcall
	// argument (the falling ChrIns, pushed by the call site):
	//   mov eax, [WorldChrMan]; test eax, eax; jz go
	//   mov eax, [eax+0x3C]; cmp eax, [esp+4]; jne go
	//   ret 4                                  ; the local player falls: no damage
	// go: jmp <fall damage>
	static bool build_fall_cave()
	{
		const DWORD world_chr_man = debug_tools::world_chr_man();
		if (!s_fall_call_addr || !world_chr_man)
			return false;
		s_fall_cave = static_cast<BYTE*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!s_fall_cave)
			return false;
		const DWORD fall_damage = s_fall_call_addr + 6 + *reinterpret_cast<const DWORD*>(&s_fall_orig_bytes[2]);
		const DWORD cave = reinterpret_cast<DWORD>(s_fall_cave);
		const BYTE code[26] = {
			0xA1, 0, 0, 0, 0,      // mov eax, [WorldChrMan]
			0x85, 0xC0,            // test eax, eax
			0x74, 0x0C,            // jz go
			0x8B, 0x40, 0x3C,      // mov eax, [eax+0x3C]
			0x3B, 0x44, 0x24, 0x04,// cmp eax, [esp+4]
			0x75, 0x03,            // jne go
			0xC2, 0x04, 0x00,      // ret 4
			0xE9, 0, 0, 0, 0,      // go: jmp <fall damage>
		};
		std::memcpy(s_fall_cave, code, sizeof(code));
		*reinterpret_cast<DWORD*>(&s_fall_cave[1]) = world_chr_man;
		*reinterpret_cast<DWORD*>(&s_fall_cave[22]) = fall_damage - (cave + 26);
		return true;
	}

	// Retargets only the call's rel32: one aligned 4-byte store, so a game thread at the call site
	// sees the old target or the new one, never half of each.
	static bool set_no_fall_damage(bool on)
	{
		const DWORD rel_addr = s_fall_call_addr + 2;
		if (!s_fall_cave || (rel_addr & 3))
			return false;
		const DWORD original = *reinterpret_cast<const DWORD*>(&s_fall_orig_bytes[2]);
		const DWORD to_stub = reinterpret_cast<DWORD>(s_fall_cave) - (s_fall_call_addr + 6);
		DWORD old_protect;
		if (!VirtualProtect(reinterpret_cast<void*>(rel_addr), 4, PAGE_EXECUTE_READWRITE, &old_protect))
			return false;
		InterlockedExchange(reinterpret_cast<volatile LONG*>(rel_addr), static_cast<LONG>(on ? to_stub : original));
		VirtualProtect(reinterpret_cast<void*>(rel_addr), 4, old_protect, &old_protect);
		FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(rel_addr), 4);
		s_no_fall_damage = on;
		return true;
	}

	static bool safe_write_vec3(DWORD addr, float x, float y, float z)
	{
		if (!addr) return false;
		__try {
			auto* out = reinterpret_cast<float*>(addr);
			out[0] = x;
			out[1] = y;
			out[2] = z;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	static bool safe_read_vec3(DWORD addr, float& x, float& y, float& z)
	{
		if (!addr) return false;
		__try {
			const auto* in = reinterpret_cast<const float*>(addr);
			x = in[0];
			y = in[1];
			z = in[2];
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Saved teleport spots: one per line in rtx_comp\teleport_spots.txt, `name|mNN_NN|x y z` (name without '|').
	struct saved_spot
	{
		std::string name;
		std::string map;
		float x = 0.0f, y = 0.0f, z = 0.0f;
	};
	static std::vector<saved_spot> s_spots;
	static bool s_spots_loaded = false;
	static bool s_show_coordinates = false;
	static char s_spot_name[64] = {};

	static std::string spots_path()
	{
		return shared::globals::root_path + "\\rtx_comp\\teleport_spots.txt";
	}

	static void load_spots()
	{
		s_spots_loaded = true;
		s_spots.clear();
		std::ifstream file(spots_path());
		std::string line;
		while (std::getline(file, line))
		{
			if (line.empty() || line[0] == '#')
				continue;
			const auto a = line.find('|'), b = line.find('|', a == std::string::npos ? a : a + 1);
			if (a == std::string::npos || b == std::string::npos)
				continue;
			saved_spot s;
			s.name = line.substr(0, a);
			s.map = line.substr(a + 1, b - a - 1);
			std::istringstream in(line.substr(b + 1));
			if (in >> s.x >> s.y >> s.z)
				s_spots.push_back(s);
		}
	}

	static void save_spots()
	{
		std::ofstream file(spots_path(), std::ios::trunc);
		file << "# Teleport spots (F4 > Cheats): name|map|x y z. proxy.Cheats.TeleportSpot = <name> teleports from a script.\n";
		for (const auto& s : s_spots)
			file << s.name << '|' << s.map << '|' << std::format("{:.3f} {:.3f} {:.3f}", s.x, s.y, s.z) << '\n';
	}

	static std::string current_map_name()
	{
		const unsigned id = (map_lights::current_map_id() >> 16) & 0xFFFF;
		return id ? std::format("m{:02}_{:02}", id >> 8, id & 0xFF) : std::string("m??_??");
	}

	static int find_bonfire_index_by_id(int id)
	{
		for (size_t i = 0; i < bonfire_count(); ++i)
		{
			if (s_bonfires[i].id == id)
			{
				return static_cast<int>(i);
			}
		}

		return -1;
	}

	static int find_first_teleportable_index()
	{
		for (size_t i = 0; i < bonfire_count(); ++i)
		{
			if (s_bonfires[i].has_coords)
			{
				return static_cast<int>(i);
			}
		}

		return 0;
	}

	static const bonfire_entry* bonfire_at(int index)
	{
		if (index < 0 || static_cast<size_t>(index) >= bonfire_count())
		{
			return nullptr;
		}

		return &s_bonfires[index];
	}

	static DWORD get_last_bonfire_value_addr()
	{
		if (!s_last_bonfire_base_addr)
		{
			return 0;
		}

		const DWORD base = safe_read_ptr(s_last_bonfire_base_addr);
		return base ? base + LAST_BONFIRE_VALUE_OFFSET : 0;
	}

	static DWORD get_teleport_primary_addr()
	{
		if (!s_teleport_base_addr)
		{
			return 0;
		}

		DWORD p = safe_read_ptr(s_teleport_base_addr);
		if (!p) return 0;
		p = safe_read_ptr(p + 0x18);
		if (!p) return 0;
		p = safe_read_ptr(p + 0x44);
		if (!p) return 0;
		p = safe_read_ptr(p + 0x10);
		if (!p) return 0;

		return p + TELEPORT_BLOCK_OFFSET;
	}

	static DWORD get_teleport_secondary_addr()
	{
		if (!s_teleport_base_addr)
		{
			return 0;
		}

		DWORD p = safe_read_ptr(s_teleport_base_addr);
		if (!p) return 0;
		p = safe_read_ptr(p + 0x18);
		if (!p) return 0;

		return p + TELEPORT_SECONDARY_BLOCK_OFFSET;
	}

	static void sync_last_bonfire_selection_from_game()
	{
		const int current_id = safe_read_int(get_last_bonfire_value_addr(), 0);
		const int current_index = find_bonfire_index_by_id(current_id);
		if (current_index >= 0)
		{
			s_last_bonfire_index = current_index;
			if (s_bonfires[current_index].has_coords)
			{
				s_teleport_index = current_index;
			}
		}
	}

	static bool set_last_bonfire(const bonfire_entry& entry)
	{
		const DWORD addr = get_last_bonfire_value_addr();
		return safe_write_int(addr, entry.id);
	}

	static bool teleport_to_bonfire(const bonfire_entry& entry)
	{
		if (!entry.has_coords)
		{
			return false;
		}

		const DWORD primary = get_teleport_primary_addr();
		const DWORD secondary = get_teleport_secondary_addr();

		bool wrote = false;
		if (primary)
		{
			wrote = safe_write_vec3(primary, entry.x, entry.y, entry.z) || wrote;
		}

		if (secondary)
		{
			wrote = safe_write_vec3(secondary, entry.x, entry.y, entry.z) || wrote;
		}

		return wrote;
	}

	static bool player_position(float& x, float& y, float& z)
	{
		return safe_read_vec3(get_teleport_primary_addr(), x, y, z);
	}

	static bool teleport_to(const char* what, float x, float y, float z)
	{
		const DWORD primary = get_teleport_primary_addr();
		const DWORD secondary = get_teleport_secondary_addr();
		float fx = 0.0f, fy = 0.0f, fz = 0.0f;
		const bool had = player_position(fx, fy, fz);
		bool wrote = false;
		if (primary)
			wrote = safe_write_vec3(primary, x, y, z) || wrote;
		if (secondary)
			wrote = safe_write_vec3(secondary, x, y, z) || wrote;
		log("Cheats", wrote ? std::format("Teleported to {} ({:.3f}, {:.3f}, {:.3f}) from {} ({})", what, x, y, z,
				had ? std::format("{:.3f}, {:.3f}, {:.3f}", fx, fy, fz) : std::string("unknown"), current_map_name())
			: std::format("Teleport to {} failed (no player position)", what), wrote ? LOG_TYPE::LOG_TYPE_GREEN : LOG_TYPE::LOG_TYPE_ERROR, true);
		return wrote;
	}

	bool teleport_to_xyz(float x, float y, float z)
	{
		return teleport_to("coordinates", x, y, z);
	}

	bool teleport_to_spot(const std::string& name)
	{
		if (!s_spots_loaded)
			load_spots();
		for (const auto& s : s_spots)
			if (s.name == name)
				return teleport_to(s.name.c_str(), s.x, s.y, s.z);
		log("Cheats", std::format("No saved teleport spot named '{}'", name), LOG_TYPE::LOG_TYPE_ERROR, true);
		return false;
	}

	void draw_coordinates_overlay()
	{
		float x, y, z;
		if (!s_show_coordinates || !player_position(x, y, z))
			return;
		ImGui::GetForegroundDrawList()->AddText(ImVec2(20.0f, 60.0f), IM_COL32(255, 255, 160, 255),
			std::format("{}  X {:.2f}  Y {:.2f}  Z {:.2f}", current_map_name(), x, y, z).c_str());
	}

	void init()
	{
		const DWORD teleport_ptr = shared::utils::mem::find_pattern(
			TELEPORT_BASE_AOB, TELEPORT_BASE_AOB_OFFSET, "TeleportBase", true, 0);
		if (teleport_ptr)
		{
			s_teleport_base_addr = *reinterpret_cast<DWORD*>(teleport_ptr);
			log("Cheats", std::format("Teleport base static address: 0x{:08X}", s_teleport_base_addr));
		}
		else
		{
			log("Cheats", "Teleport base AOB not found", LOG_TYPE::LOG_TYPE_WARN, true);
		}

		const DWORD bonfire_ptr = shared::utils::mem::find_pattern(
			LAST_BONFIRE_BASE_AOB, LAST_BONFIRE_BASE_AOB_OFFSET, "LastBonfireBase", true, 0);
		if (bonfire_ptr)
		{
			s_last_bonfire_base_addr = *reinterpret_cast<DWORD*>(bonfire_ptr);
			log("Cheats", std::format("Last bonfire static address: 0x{:08X}", s_last_bonfire_base_addr));
		}
		else
		{
			log("Cheats", "Last bonfire AOB not found", LOG_TYPE::LOG_TYPE_WARN, true);
		}

		s_teleport_index = find_first_teleportable_index();
		sync_last_bonfire_selection_from_game();

		if (debug_tools::flags_available() && shared::common::config::get().cheats.enemies_ignore_player)
		{
			const bool ok = set_enemies_ignore_player(true);
			log("Cheats", ok ? "Enemies ignore the player (from INI)" : "Could not set the hide / silence flags",
				ok ? LOG_TYPE::LOG_TYPE_WARN : LOG_TYPE::LOG_TYPE_ERROR, true);
		}

		if (const DWORD fall_call = shared::utils::mem::find_pattern(FALL_DAMAGE_CALL_AOB, 0, "FallDamageCall", true, 0))
		{
			s_fall_call_addr = fall_call;
			std::memcpy(s_fall_orig_bytes, reinterpret_cast<const void*>(fall_call), sizeof(s_fall_orig_bytes));
		}
		if (!build_fall_cave())
			log("Cheats", "Fall damage call not found — No Fall Damage unavailable", LOG_TYPE::LOG_TYPE_WARN, true);
		else if (shared::common::config::get().cheats.no_fall_damage && set_no_fall_damage(true))
			log("Cheats", "No fall damage (from INI)", LOG_TYPE::LOG_TYPE_WARN, true);

		DWORD found = shared::utils::mem::find_pattern(HP_WRITE_AOB, 0, "HP_Write", true, 0);
		if (!found)
		{
			log("Cheats", "HP write AOB not found — god mode unavailable", LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}

		s_patch_addr = found;
		log("Cheats", std::format("HP write instruction at 0x{:08X}", s_patch_addr));

		// Save original 6 bytes: 89 85 D4 02 00 00 = mov [ebp+0x2D4], eax
		std::memcpy(s_orig_bytes, reinterpret_cast<void*>(s_patch_addr), 6);

		// Allocate executable code cave
		s_cave = static_cast<BYTE*>(
			VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!s_cave)
		{
			log("Cheats", "Failed to allocate code cave", LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}

		// Build the code cave:
		//   cmp dword ptr [s_active], 0
		//   je .skip
		//   cmp byte ptr [ebp+4], 0     ; is player?
		//   jne .skip
		//   mov eax, 0x7D0              ; 2000 HP
		// .skip:
		//   mov [ebp+0x2D4], eax        ; original instruction
		//   jmp <return_addr>           ; jump back past the patched bytes
		int i = 0;

		// cmp dword ptr [&s_active], 0
		s_cave[i++] = 0x81; // cmp r/m32, imm32
		s_cave[i++] = 0x3D; // ModRM: [disp32]
		*reinterpret_cast<DWORD*>(&s_cave[i]) = reinterpret_cast<DWORD>(&s_active);
		i += 4;
		*reinterpret_cast<DWORD*>(&s_cave[i]) = 0; // imm32 = 0
		i += 4;

		// je .skip (short jump, offset filled below)
		s_cave[i++] = 0x74; // JE rel8
		int je_offset_pos = i;
		s_cave[i++] = 0; // placeholder

		// cmp byte ptr [ebp+4], 0
		s_cave[i++] = 0x80; // CMP r/m8, imm8
		s_cave[i++] = 0x7D; // ModRM: [ebp+disp8]
		s_cave[i++] = 0x04; // disp8 = +4
		s_cave[i++] = 0x00; // imm8 = 0

		// jne .skip (short jump)
		s_cave[i++] = 0x75; // JNE rel8
		int jne_offset_pos = i;
		s_cave[i++] = 0; // placeholder

		// mov eax, 0x7D0
		s_cave[i++] = 0xB8; // MOV EAX, imm32
		*reinterpret_cast<DWORD*>(&s_cave[i]) = 0x7D0; // 2000
		i += 4;

		// .skip:
		int skip_target = i;
		s_cave[je_offset_pos] = static_cast<BYTE>(skip_target - (je_offset_pos + 1));
		s_cave[jne_offset_pos] = static_cast<BYTE>(skip_target - (jne_offset_pos + 1));

		// mov [ebp+0x2D4], eax  (original instruction: 89 85 D4 02 00 00)
		std::memcpy(&s_cave[i], s_orig_bytes, 6);
		i += 6;

		// jmp <return_addr>  (E9 + rel32)
		DWORD return_addr = s_patch_addr + 6;
		s_cave[i++] = 0xE9;
		*reinterpret_cast<DWORD*>(&s_cave[i]) =
			static_cast<DWORD>(return_addr - (reinterpret_cast<DWORD>(&s_cave[i]) + 4));
		i += 4;

		// Patch original: overwrite 6 bytes with JMP to cave + NOP
		// JMP rel32 = 5 bytes (E9 xx xx xx xx) + 1 NOP (90)
		DWORD old_protect;
		VirtualProtect(reinterpret_cast<void*>(s_patch_addr), 6, PAGE_EXECUTE_READWRITE, &old_protect);

		BYTE patch[6];
		patch[0] = 0xE9; // JMP rel32
		*reinterpret_cast<DWORD*>(&patch[1]) =
			static_cast<DWORD>(reinterpret_cast<DWORD>(s_cave) - (s_patch_addr + 5));
		patch[5] = 0x90; // NOP

		std::memcpy(reinterpret_cast<void*>(s_patch_addr), patch, 6);
		VirtualProtect(reinterpret_cast<void*>(s_patch_addr), 6, old_protect, &old_protect);
		FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(s_patch_addr), 6);

		s_available = true;
		log("Cheats", std::format("God mode hook installed at 0x{:08X}, cave at 0x{:p}",
			s_patch_addr, static_cast<void*>(s_cave)), LOG_TYPE::LOG_TYPE_GREEN, true);

		// Apply INI setting
		auto& cfg = shared::common::config::get();
		if (cfg.cheats.god_mode)
		{
			s_enabled = true;
			InterlockedExchange(&s_active, 1);
			log("Cheats", "God mode enabled from INI", LOG_TYPE::LOG_TYPE_WARN, true);
		}
	}

	bool is_god_mode_available() { return s_available; }

	bool& god_mode_enabled()
	{
		return s_enabled;
	}

	void tab_imgui()
	{
		if (!s_available)
		{
			ImGui::TextDisabled("God mode unavailable (AOB not found)");
		}
		else if (ImGui::Checkbox("God Mode (Infinite Health)", &s_enabled))
		{
			InterlockedExchange(&s_active, s_enabled ? 1 : 0);
			log("Cheats", std::format("God mode {}", s_enabled ? "ENABLED" : "disabled"));
		}

		if (s_available && s_enabled)
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "ACTIVE");
		}

		if (!debug_tools::flags_available())
		{
			ImGui::TextDisabled("Enemies Ignore Player / Freeze Enemies unavailable (AOB not found)");
		}
		else
		{
			// Read back every frame: the checkbox shows the game's flags, not a remembered choice.
			bool ignore = enemies_ignore_player();
			if (ImGui::Checkbox("Enemies Ignore Player", &ignore))
			{
				const bool ok = set_enemies_ignore_player(ignore);
				log("Cheats", ok ? std::format("Enemies ignore the player: {}", ignore ? "ON" : "off")
					: std::string("Could not write the hide / silence flags"));
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("The game's own debug flags Player Hide (enemies do not see you) and\n"
					"Player Silence (they do not hear you). On at start with [Cheats] EnemiesIgnorePlayer=1.");
			debug_tools::flag_checkbox(debug_tools::kFlagAllNoUpdateAi, "Freeze Enemies",
				"The game's All No Update AI debug flag: every AI character stands still (event-scripted\n"
				"actions and projectiles in flight still happen).");
		}

		if (!s_fall_cave)
		{
			ImGui::TextDisabled("No Fall Damage unavailable (not found)");
		}
		else
		{
			bool no_fall = s_no_fall_damage;
			if (ImGui::Checkbox("No Fall Damage", &no_fall))
			{
				const bool ok = set_no_fall_damage(no_fall);
				log("Cheats", ok ? std::format("No fall damage: {}", no_fall ? "ON" : "off") : std::string("Could not patch the fall damage call"));
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("The player takes no fall damage (enemies still do). Bottomless pits and kill planes\n"
					"still kill. On at start with [Cheats] NoFallDamage=1.");
		}

		ImGui::Separator();
		ImGui::Spacing();

		ImGui::Text("Bonfires:");

		if (!s_last_bonfire_base_addr)
		{
			ImGui::TextDisabled("Last bonfire write unavailable (AOB not found)");
		}
		else
		{
			const bonfire_entry* selected = bonfire_at(s_last_bonfire_index);
			const char* preview = selected ? selected->name : "<invalid>";

			if (ImGui::BeginCombo("Last Bonfire", preview))
			{
				for (size_t i = 0; i < bonfire_count(); ++i)
				{
					const bool is_selected = (s_last_bonfire_index == static_cast<int>(i));
					// Some destinations share a name, and a label alone is the row's ImGui ID.
					ImGui::PushID(static_cast<int>(i));
					if (ImGui::Selectable(s_bonfires[i].name, is_selected))
					{
						s_last_bonfire_index = static_cast<int>(i);
					}
					ImGui::PopID();

					if (is_selected)
					{
						ImGui::SetItemDefaultFocus();
					}
				}

				ImGui::EndCombo();
			}

			const int current_id = safe_read_int(get_last_bonfire_value_addr(), 0);
			const int current_index = find_bonfire_index_by_id(current_id);
			if (current_index >= 0)
			{
				ImGui::TextDisabled("Current: %s (%d)", s_bonfires[current_index].name, current_id);
			}
			else
			{
				ImGui::TextDisabled("Current: Unknown ID %d", current_id);
			}

			if (ImGui::Button("Set Last Bonfire"))
			{
				const bonfire_entry* target = bonfire_at(s_last_bonfire_index);
				if (target && set_last_bonfire(*target))
				{
					log("Cheats", std::format("Last bonfire set to {} ({})", target->name, target->id), LOG_TYPE::LOG_TYPE_GREEN, true);
				}
				else
				{
					log("Cheats", "Failed to write last bonfire value", LOG_TYPE::LOG_TYPE_ERROR, true);
				}
			}
		}

		ImGui::Spacing();

		if (!s_teleport_base_addr)
		{
			ImGui::TextDisabled("Teleport unavailable (AOB not found)");
		}
		else
		{
			const bonfire_entry* selected = bonfire_at(s_teleport_index);
			const char* preview = selected ? selected->name : "<invalid>";

			if (ImGui::BeginCombo("Teleport Destination", preview))
			{
				for (size_t i = 0; i < bonfire_count(); ++i)
				{
					const bool is_selected = (s_teleport_index == static_cast<int>(i));
					// Some destinations share a name, and a label alone is the row's ImGui ID.
					ImGui::PushID(static_cast<int>(i));
					if (ImGui::Selectable(s_bonfires[i].name, is_selected))
					{
						s_teleport_index = static_cast<int>(i);
					}
					ImGui::PopID();

					if (is_selected)
					{
						ImGui::SetItemDefaultFocus();
					}
				}

				ImGui::EndCombo();
			}

			if (selected)
			{
				if (selected->has_coords)
				{
					ImGui::TextDisabled("XYZ: %.3f  %.3f  %.3f", selected->x, selected->y, selected->z);
				}
				else
				{
					ImGui::TextDisabled("Coordinates unavailable for this bonfire");
				}

				if (selected->has_coords && ImGui::Button("Teleport"))
				{
					if (teleport_to_bonfire(*selected))
					{
						log("Cheats", std::format("Teleported to {} ({:.3f}, {:.3f}, {:.3f})",
							selected->name, selected->x, selected->y, selected->z), LOG_TYPE::LOG_TYPE_GREEN, true);
					}
					else
					{
						log("Cheats", "Failed to write teleport coordinates", LOG_TYPE::LOG_TYPE_ERROR, true);
					}
				}
			}
		}

		// Coordinates and saved spots.
		ImGui::Separator();
		ImGui::Spacing();
		ImGui::Text("Position and saved spots:");
		if (!s_teleport_base_addr)
		{
			ImGui::TextDisabled("Unavailable (teleport AOB not found)");
			return;
		}
		if (!s_spots_loaded)
			load_spots();
		float px = 0.0f, py = 0.0f, pz = 0.0f;
		const bool have_position = player_position(px, py, pz);
		if (have_position)
			ImGui::Text("%s  X %.3f  Y %.3f  Z %.3f", current_map_name().c_str(), px, py, pz);
		else
			ImGui::TextDisabled("No player position (not in the world)");
		ImGui::Checkbox("Show coordinates on screen", &s_show_coordinates);

		ImGui::SetNextItemWidth(220.0f);
		ImGui::InputTextWithHint("##spot_name", "spot name", s_spot_name, sizeof(s_spot_name));
		ImGui::SameLine();
		const std::string typed = s_spot_name;
		const bool name_ok = !typed.empty() && typed.find('|') == std::string::npos;
		ImGui::BeginDisabled(!have_position || !name_ok);
		if (ImGui::Button("Save this spot"))
		{
			auto it = std::find_if(s_spots.begin(), s_spots.end(), [&](const saved_spot& s) { return s.name == typed; });
			if (it == s_spots.end())
				it = s_spots.insert(s_spots.end(), saved_spot{ typed });
			it->map = current_map_name();
			it->x = px;
			it->y = py;
			it->z = pz;
			save_spots();
			log("Cheats", std::format("Saved teleport spot '{}' at {} ({:.3f}, {:.3f}, {:.3f})", typed, it->map, px, py, pz),
				LOG_TYPE::LOG_TYPE_GREEN, true);
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextDisabled("(same name: overwritten)");

		int remove = -1;
		for (int i = 0; i < static_cast<int>(s_spots.size()); ++i)
		{
			const auto& s = s_spots[i];
			ImGui::PushID(i);
			if (ImGui::Button("Teleport"))
				teleport_to(s.name.c_str(), s.x, s.y, s.z);
			ImGui::SameLine();
			if (ImGui::Button("Delete"))
				remove = i;
			ImGui::SameLine();
			ImGui::Text("%s  (%s  %.2f  %.2f  %.2f)", s.name.c_str(), s.map.c_str(), s.x, s.y, s.z);
			ImGui::PopID();
		}
		if (remove >= 0)
		{
			s_spots.erase(s_spots.begin() + remove);
			save_spots();
		}
		if (s_spots.empty())
			ImGui::TextDisabled("No saved spots yet. Live keys for scripts: proxy.Cheats.TeleportSpot = <name>, proxy.Cheats.TeleportXYZ = x, y, z");
	}
}
