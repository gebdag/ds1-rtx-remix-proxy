#include "std_include.hpp"
#include "debug_tools.hpp"

using shared::common::log;
using shared::common::LOG_TYPE;

namespace comp::game::debug_tools
{
	namespace
	{
		// ---------------------------------------------------------------- guarded memory access

		// The game's structures come and go with area loads, so every access is guarded.
		template <typename T>
		bool read(DWORD addr, T& out)
		{
			if (addr < 0x10000) return false;
			__try { out = *reinterpret_cast<const T*>(addr); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		template <typename T>
		bool write(DWORD addr, T value)
		{
			if (addr < 0x10000) return false;
			__try { *reinterpret_cast<T*>(addr) = value; return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		DWORD deref(DWORD addr)
		{
			DWORD v = 0;
			return read(addr, v) ? v : 0;
		}

		// Code and .rdata are not writable: unprotect around the write.
		bool write_protected(DWORD addr, const void* bytes, size_t size)
		{
			DWORD old = 0;
			if (!addr || !VirtualProtect(reinterpret_cast<void*>(addr), size, PAGE_EXECUTE_READWRITE, &old))
				return false;
			std::memcpy(reinterpret_cast<void*>(addr), bytes, size);
			VirtualProtect(reinterpret_cast<void*>(addr), size, old, &old);
			FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), size);
			return true;
		}

		// A static the code at an AOB references: the dword at hit + offset.
		DWORD resolve_static(const char* aob, DWORD offset, const char* name)
		{
			const DWORD hit = shared::utils::mem::find_pattern(aob, offset, name, true, 0);
			const DWORD value = hit ? deref(hit) : 0;
			if (value)
				log("Debug", std::format("{}: 0x{:08X}", name, value));
			else
				log("Debug", std::format("{}: AOB not found", name), LOG_TYPE::LOG_TYPE_WARN, true);
			return value;
		}

		// ---------------------------------------------------------------- statics

		DWORD s_flags = 0;             // debug flag block (0x013784D2)
		DWORD s_magic_qty_flag = 0;    // All No MagicQty Consume, outside the block (0x01376EE7)
		DWORD s_world_chr_man = 0;     // WorldChrMan (+0x3C local player ChrIns)
		DWORD s_game_man = 0;          // GameMan (area-load switches at +0xC2C..)
		DWORD s_game_data_man = 0;     // GameDataMan (+0x8 PlayerGameData)
		DWORD s_event_flag_man = 0;    // EventFlagMan
		DWORD s_fov_min = 0, s_fov_max = 0;   // the follow camera's FOV clamp floats (.rdata)
		float s_fov_original[2] = {};  // min, max as shipped (38, 48)
		bool s_fov_override = false;
		float s_fov_degrees = 60.0f;

		DWORD player_chr()
		{
			const DWORD w = deref(s_world_chr_man);
			return w ? deref(w + 0x3C) : 0;
		}

		DWORD player_game_data()
		{
			const DWORD g = deref(s_game_data_man);
			return g ? deref(g + 0x8) : 0;
		}

		// ---------------------------------------------------------------- code patches

		// `patched` over `original` at the AOB hit + offset. Installed only when the game's bytes
		// are `original`, so a different build leaves the patch unavailable instead of corrupt.
		struct code_patch
		{
			const char* name;
			const char* tip;
			const char* aob;
			DWORD offset;
			size_t size;
			BYTE original[8];
			BYTE patched[8];
			DWORD addr = 0;
			bool on = false;
		};

		code_patch s_patches[] = {
			{ "Never Framerate-Disconnect",
				"Online, the game drops the session when the framerate stays low. This always skips that\n"
				"branch (the session monitor's jz becomes jmp). Offline it is never reached.",
				"80 7C 24 13 00 74 41 8B C6 E8", 5, 1, { 0x74 }, { 0xEB } },
			{ "Weapons Never Break",
				"Equipment wear does nothing (the wear function returns success at once). Durability already\n"
				"lost stays lost.",
				"32 C0 53 8B 5C 24 0C 85 F6", 0, 5, { 0x32, 0xC0, 0x53, 0x8B, 0x5C }, { 0xB0, 0x01, 0xC2, 0x08, 0x00 } },
		};

		// The game thread may be executing a patched instruction, so a patch is one atomic store: a
		// single byte, or the aligned qword holding all of its bytes (compare-exchange), never a
		// sequence a thread could catch half-written.
		bool write_code_atomic(DWORD addr, const BYTE* bytes, size_t size)
		{
			if (size == 1)
				return write_protected(addr, bytes, 1);
			const DWORD base = addr & ~7u;
			if (addr + size > base + 8)
				return false;
			DWORD old = 0;
			if (!VirtualProtect(reinterpret_cast<void*>(base), 8, PAGE_EXECUTE_READWRITE, &old))
				return false;
			auto* target = reinterpret_cast<volatile LONG64*>(base);
			LONG64 current = *target, wanted;
			do
			{
				wanted = current;
				std::memcpy(reinterpret_cast<BYTE*>(&wanted) + (addr - base), bytes, size);
			} while (InterlockedCompareExchange64(target, wanted, current) != current && ((current = *target), true));
			VirtualProtect(reinterpret_cast<void*>(base), 8, old, &old);
			FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(base), 8);
			return true;
		}

		void install(code_patch& p)
		{
			const DWORD hit = shared::utils::mem::find_pattern(p.aob, p.offset, p.name, true, 0);
			if (!hit)
			{
				log("Debug", std::format("{}: AOB not found", p.name), LOG_TYPE::LOG_TYPE_WARN, true);
				return;
			}
			if (std::memcmp(reinterpret_cast<const void*>(hit), p.original, p.size) != 0)
			{
				log("Debug", std::format("{}: unexpected bytes at 0x{:08X}, left alone", p.name, hit), LOG_TYPE::LOG_TYPE_WARN, true);
				return;
			}
			if (p.size > 1 && (hit & ~7u) + 8 < hit + p.size)
			{
				log("Debug", std::format("{}: 0x{:08X} spans a qword, cannot patch it atomically", p.name, hit), LOG_TYPE::LOG_TYPE_WARN, true);
				return;
			}
			p.addr = hit;
		}

		void patch_checkbox(code_patch& p)
		{
			if (!p.addr)
			{
				ImGui::TextDisabled("%s unavailable (not found)", p.name);
				return;
			}
			bool on = p.on;
			if (ImGui::Checkbox(p.name, &on) && write_code_atomic(p.addr, on ? p.patched : p.original, p.size))
			{
				p.on = on;
				log("Debug", std::format("{}: {}", p.name, on ? "ON" : "off"));
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", p.tip);
		}

		// ---------------------------------------------------------------- debug flags

		struct flag_entry
		{
			unsigned offset;
			const char* name;
			const char* tip;
		};

		constexpr flag_entry kFlags[] = {
			{ 0x00, "Player No Dead", "HP can reach 0, but the player does not die." },
			{ 0x01, "Player Exterminate", "The player's hits kill." },
			{ 0x12, "No Stamina Consume", "Nobody uses stamina." },
			{ 0x13, "No MP Consume", "Nobody uses MP." },
			{ 0x14, "No Arrow Consume", "Arrows and bolts are not used up." },
			{ 0x17, "All No Dead", "No character dies: everyone, enemies included, holds at 1 HP." },
			{ 0x18, "All No Damage", "No character takes damage, the player included (also no fall damage)." },
			{ 0x19, "All No Hit", "No attack connects." },
			{ 0x1A, "All No Attack", "No character attacks, the player included." },
			{ 0x1B, "All No Move", "No character moves, the player included." },
		};

		void byte_checkbox(DWORD addr, const char* name, const char* tip)
		{
			BYTE value = 0;
			if (!read(addr, value))
			{
				ImGui::TextDisabled("%s unavailable", name);
				return;
			}
			bool on = value == 1;
			if (ImGui::Checkbox(name, &on))
			{
				const bool ok = write<BYTE>(addr, on ? 1 : 0);
				log("Debug", ok ? std::format("{}: {}", name, on ? "ON" : "off") : std::format("Could not write {}", name));
			}
			if (tip && ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", tip);
		}

		// ---------------------------------------------------------------- sections

		void flags_section()
		{
			if (!s_flags)
			{
				ImGui::TextDisabled("Unavailable (debug flag AOB not found)");
				return;
			}
			for (const auto& f : kFlags)
				flag_checkbox(f.offset, f.name, f.tip);
			if (s_magic_qty_flag)
				byte_checkbox(s_magic_qty_flag, "No Spell Uses Consume", "Attuned spells keep their uses.");
		}

		void patches_section()
		{
			for (auto& p : s_patches)
				patch_checkbox(p);
		}

		void player_section()
		{
			const DWORD chr = player_chr();
			if (!chr)
			{
				ImGui::TextDisabled("No player character loaded");
				return;
			}

			// Animation / movement rate (ChrIns+0x28 -> +0x14 -> +0x64), 1 = normal. Reset by area loads.
			if (const DWORD speed_addr = deref(deref(chr + 0x28) + 0x14); speed_addr)
			{
				float speed = 1.0f;
				if (read(speed_addr + 0x64, speed))
				{
					ImGui::SetNextItemWidth(200.0f);
					if (ImGui::SliderFloat("Player speed", &speed, 0.05f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic))
						write(speed_addr + 0x64, speed);
					ImGui::SameLine();
					if (ImGui::SmallButton("1.0##speed"))
						write(speed_addr + 0x64, 1.0f);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("The player's animation and movement rate; slow it down to judge fades and motion.\n"
							"Not game time. Resets on area load and death.");
				}
			}

			// Look: ChrType (+0x70) and TeamType (+0x74). The game rewrites them on its own transitions.
			int chr_type = 0, team_type = 0;
			if (read(chr + 0x70, chr_type) && read(chr + 0x74, team_type))
			{
				ImGui::Text("Character type %d, team type %d", chr_type, team_type);
				ImGui::SameLine();
				if (ImGui::SmallButton("Human"))
				{
					write(chr + 0x70, 0);
					write(chr + 0x74, 1);
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("Hollow"))
				{
					write(chr + 0x70, 8);
					write(chr + 0x74, 4);
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("White phantom"))
					write(chr + 0x70, 1);
				ImGui::SameLine();
				if (ImGui::SmallButton("Black phantom"))
					write(chr + 0x70, 2);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Character type 0 human, 1 white phantom, 2 black phantom, 8 hollow (team type 1 / 4\n"
						"for human / hollow). Offline only: the game rewrites them on death, summons and area loads.");
			}

			if (const DWORD pgd = player_game_data(); pgd)
			{
				int humanity = 0;
				if (read(pgd + 0x7C, humanity))
				{
					ImGui::SetNextItemWidth(120.0f);
					if (ImGui::InputInt("Humanity", &humanity))
						write(pgd + 0x7C, std::clamp(humanity, 0, 99));
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Saved to the character.");
				}

				// Gestures: 15 entries of {byte 2k+3 unlocked / 2k+2 locked, 0, u16 k} at [pgd+0x424]+8; only the
				// unlock byte is written.
				if (ImGui::Button("Unlock all gestures"))
				{
					const DWORD base = deref(pgd + 0x424);
					bool ok = base != 0;
					for (DWORD k = 0; ok && k < 15; k++)
						ok = write<BYTE>(base + 8 + 4 * k, static_cast<BYTE>(2 * k + 3));
					log("Debug", ok ? "Gestures unlocked" : "Could not unlock the gestures");
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Saved to the character.");
			}

			// Status build-up: current resistance points at +0x300.. (max at +0x310..). Build-up subtracts
			// and the status starts only inside the build-up path, so 1 point left makes the next hit start it.
			static const char* kStatus[] = { "Poison", "Toxin", "Bleed", "Curse" };
			ImGui::TextUnformatted("Prime status (the next hit of its kind starts it):");
			for (DWORD i = 0; i < 4; i++)
			{
				int current = 0, max = 0;
				read(chr + 0x300 + 4 * i, current);
				read(chr + 0x310 + 4 * i, max);
				ImGui::SameLine();
				if (ImGui::SmallButton(std::format("{} {}/{}###status{}", kStatus[i], current, max, i).c_str()))
					write(chr + 0x300 + 4 * i, 1);
			}
		}

		void camera_section()
		{
			if (!s_fov_min || !s_fov_max)
			{
				ImGui::TextDisabled("FOV override unavailable (not found)");
				return;
			}
			// The follow camera clamps its LockCamParam FOV to [min, max]; both set to one value makes it that.
			bool changed = ImGui::Checkbox("Override FOV", &s_fov_override);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(200.0f);
			changed |= ImGui::SliderFloat("##fov", &s_fov_degrees, 20.0f, 100.0f, "%.0f deg");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("The player follow camera's FOV (the game clamps it to %.0f-%.0f). Not cutscene cameras.",
					s_fov_original[0], s_fov_original[1]);
			if (changed)
			{
				const float min = s_fov_override ? s_fov_degrees : s_fov_original[0];
				const float max = s_fov_override ? s_fov_degrees : s_fov_original[1];
				write_protected(s_fov_min, &min, sizeof(min));
				write_protected(s_fov_max, &max, sizeof(max));
			}
		}

		void area_section()
		{
			const DWORD gm = deref(s_game_man);
			if (!gm)
			{
				ImGui::TextDisabled("Unavailable (GameMan not found)");
				return;
			}
			ImGui::TextDisabled("Take effect at the next area load (warp, quit-out); what is loaded stays.");
			byte_checkbox(gm + 0xC2C, "No enemies", "Map blocks load without their enemies.");
			byte_checkbox(gm + 0xC2E, "No map parts", nullptr);
			byte_checkbox(gm + 0xC2F, "No objects", nullptr);
			byte_checkbox(gm + 0xC34, "No SFX", nullptr);
			byte_checkbox(gm + 0xC2D, "No events", "Event scripts do not run: can break progression.");
		}

		void world_section()
		{
			// Event flag 11510400 (Anor Londo dark): byte [[EventFlagMan]] + 0x4631, bit 7 (the flag array,
			// not the 0x134-byte manager object).
			const DWORD efm = deref(deref(s_event_flag_man));
			BYTE value = 0;
			if (!efm || !read(efm + 0x4631, value))
			{
				ImGui::TextDisabled("World flags unavailable (no event flags loaded)");
				return;
			}
			bool dark = (value & 0x80) != 0;
			if (ImGui::Checkbox("Anor Londo dark", &dark))
				write<BYTE>(efm + 0x4631, static_cast<BYTE>(dark ? value | 0x80 : value & ~0x80));
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Event flag 11510400, set after Gwyndolin. Shows after an area reload, and is saved with\n"
					"the world progression: clear it again before saving if it should not stick.");
		}
	}

	void init()
	{
		s_flags = resolve_static("89 B5 10 01 00 00 85 F6", 0x11, "DebugFlags");
		s_magic_qty_flag = resolve_static("89 B5 10 01 00 00 85 F6 0F 84 ?? ?? ?? ?? 6A", 0x1B3, "NoMagicQtyConsume");
		s_world_chr_man = resolve_static("8B 15 ?? ?? ?? ?? 8B 42 3C 33 C9", 2, "WorldChrMan");
		s_game_man = resolve_static("A1 ?? ?? ?? ?? 53 56 57 8B F9 8B 88 ?? ?? ?? ?? 8B", 1, "GameMan");
		s_game_data_man = resolve_static("A1 ?? ?? ?? ?? 8B 40 34 53 32", 1, "GameDataMan");
		s_event_flag_man = resolve_static("A1 ?? ?? ?? ?? 68 CF 0F 00 00 32 C9 E8", 1, "EventFlagMan");

		// The follow camera's FOV clamp: movss xmm2, [max]; ...; movss xmm1, [min].
		if (const DWORD fov = shared::utils::mem::find_pattern(
			"F3 0F 10 15 ?? ?? ?? ?? F3 0F 11 8B 6C 01 00 00 F3 0F 10 0D ?? ?? ?? ?? 83 C0 14", 0, "FovClamp", true, 0))
		{
			s_fov_max = deref(fov + 4);
			s_fov_min = deref(fov + 0x14);
			if (!read(s_fov_min, s_fov_original[0]) || !read(s_fov_max, s_fov_original[1]))
				s_fov_min = s_fov_max = 0;
		}

		for (auto& p : s_patches)
			install(p);
	}

	bool online_session()
	{
		BYTE active = 0;
		return read(deref(s_game_man) + 0xB4E, active) && active != 0;
	}

	void online_warning()
	{
		if (online_session())
			ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
				"Online session active: phantoms and invaders see these changes. Use them offline.");
	}

	void tab_imgui()
	{
		online_warning();
		if (ImGui::CollapsingHeader("Game debug flags", ImGuiTreeNodeFlags_DefaultOpen))
			flags_section();
		if (ImGui::CollapsingHeader("Patches", ImGuiTreeNodeFlags_DefaultOpen))
			patches_section();
		if (ImGui::CollapsingHeader("Player", ImGuiTreeNodeFlags_DefaultOpen))
			player_section();
		if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
			camera_section();
		if (ImGui::CollapsingHeader("Area (next load)"))
			area_section();
		if (ImGui::CollapsingHeader("World"))
			world_section();
	}

	bool flags_available() { return s_flags != 0; }

	bool read_flag(unsigned offset)
	{
		BYTE v = 0;
		return s_flags && read(s_flags + offset, v) && v == 1;
	}

	bool write_flag(unsigned offset, bool on)
	{
		return s_flags && write<BYTE>(s_flags + offset, on ? 1 : 0);
	}

	void flag_checkbox(unsigned offset, const char* name, const char* tip)
	{
		byte_checkbox(s_flags + offset, name, tip);
	}

	unsigned world_chr_man() { return s_world_chr_man; }
}
