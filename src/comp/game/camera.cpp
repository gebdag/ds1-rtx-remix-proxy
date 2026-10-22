#include "std_include.hpp"
#include "camera.hpp"
#include "shared/common/ffp_state.hpp"
#include "frustum_culling.hpp"
#include "host_feed.hpp"

using shared::common::log;
using shared::common::LOG_TYPE;

namespace comp::game::camera
{
	namespace
	{
		constexpr const char* kOverrideSourceModes[] = {
			"Auto",
			"Camera",
			"Character",
		};

		bool is_nonzero_position(const float x, const float y, const float z)
		{
			return std::fabs(x) > 0.01f || std::fabs(y) > 0.01f || std::fabs(z) > 0.01f;
		}
	}

	// WorldChrMan AOB: `mov [static_addr], esi` — stores the WorldChrMan manager pointer.
	// Pattern: 8B 76 0C 89 35 ?? ?? ?? ?? 33 C0
	static constexpr const char* WORLD_CHR_MAN_AOB = "8B 76 0C 89 35 ?? ?? ?? ?? 33 C0";
	static constexpr DWORD WORLD_CHR_MAN_AOB_OFFSET = 5; // offset to the static address operand

	// Resolved static address of the WorldChrMan pointer slot (0 = not found)
	static DWORD world_chr_man_addr = 0;

	// Legacy AOBs (kept for diagnostics; positions now read via WorldChrMan chain)
	static DWORD cam_base_addr = 0;
	static DWORD char_base_addr = 0;

	static settings s_settings;
	static apex_lock_state s_lock = {};

	settings& get_settings() { return s_settings; }

	const apex_lock_state& lock_state() { return s_lock; }

	bool get_override_pos(float& x, float& y, float& z, const char** sourceLabel)
	{
		auto& s = s_settings;
		if (!s.use_game_camera)
		{
			return false;
		}

		// The lookdev host gets the game's result for this frame from the capture.
		if (shared::globals::lookdev_host)
		{
			const auto& f = host_feed::frame();
			if (!f.camera_valid) return false;
			x = f.camera[0]; y = f.camera[1]; z = f.camera[2];
			if (sourceLabel) *sourceLabel = "capture";
			return true;
		}

		// The consumer works in the engine's raw {X, Z, Y} order. Frustum apexes already
		// use it; gameplay entities are read as render order and need the swap. Doing it
		// here keeps every consumer of the override on one convention.
		auto accept = [&](const char* label, float px, float py, float pz, bool render_order)
		{
			if (render_order)
			{
				std::swap(py, pz);
			}

			x = px + s.manual_offset[0];
			y = py + s.manual_offset[1];
			z = pz + s.manual_offset[2];
			if (sourceLabel)
			{
				*sourceLabel = label;
			}
			return true;
		};

		float px = 0.0f, py = 0.0f, pz = 0.0f;

		// The camera-relative main pass never uploads the camera position, but the engine
		// builds culling frustums in world space, so the main camera's apex is it.
		if (s.use_frustum_apex && s.override_source_mode != 2 &&
			get_camera_apex_pos(px, py, pz) && is_nonzero_position(px, py, pz))
		{
			s_lock.used_player_fallback = false;
			return accept("frustum apex", px, py, pz, false);
		}

		// The player position is NOT the camera: it differs by the orbit offset, which
		// changes when the camera turns, so anything anchored to it slides through the
		// world as you look around. It only exists to keep start-up sane before the
		// camera has been acquired; lock_state() reports when it is in use.
		if (s.override_source_mode != 2 &&
			get_world_pos(px, py, pz) && is_nonzero_position(px, py, pz))
		{
			s_lock.used_player_fallback = true;
			return accept("player (approx)", px, py, pz, true);
		}

		if (s.override_source_mode != 1 &&
			get_char_pos(px, py, pz) && is_nonzero_position(px, py, pz))
		{
			return accept("character", px, py, pz, true);
		}

		return false;
	}

	// Safe pointer dereference — returns 0 if the address is not readable.
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

	static float safe_read_float(DWORD addr)
	{
		if (!addr) return 0.0f;
		__try {
			return *reinterpret_cast<float*>(addr);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return 0.0f;
		}
	}

	static DWORD get_char_object_ptr()
	{
		DWORD wcm = safe_read_ptr(world_chr_man_addr);
		if (!wcm) return 0;
		return safe_read_ptr(wcm + 0x18);
	}

	void init()
	{
		// Resolve WorldChrMan: AOB `mov [addr], esi` — addr is the static slot holding WorldChrMan ptr.
		DWORD wcm_aob = shared::utils::mem::find_pattern(WORLD_CHR_MAN_AOB, WORLD_CHR_MAN_AOB_OFFSET, "WorldChrMan", true, 0);
		if (wcm_aob)
		{
			world_chr_man_addr = *reinterpret_cast<DWORD*>(wcm_aob);
			log("Camera", std::format("WorldChrMan slot: 0x{:08X}", world_chr_man_addr));
		}
		else
		{
			log("Camera", "WorldChrMan AOB not found", LOG_TYPE::LOG_TYPE_ERROR, true);
		}
	}

	bool is_available()
	{
		return world_chr_man_addr != 0;
	}

	// Read player world position via WorldChrMan → player entity → position block.
	// DS1 stores position as {X, Z, Y} at +0x10 from the player entity (Y-up, Z = depth).
	static bool read_player_pos(float& x, float& y, float& z)
	{
		if (!world_chr_man_addr) return false;

		DWORD wcm = safe_read_ptr(world_chr_man_addr);
		if (!wcm) return false;

		DWORD player_entity = safe_read_ptr(wcm + 0x18);
		if (!player_entity) return false;

		// Position block layout (3 × float32 at +0x10): X_world, Z_world, Y_world
		x = safe_read_float(player_entity + 0x10);
		z = safe_read_float(player_entity + 0x14);
		y = safe_read_float(player_entity + 0x18);
		return true;
	}

	// Bootstrap only: the main camera orbits the character at a measured 1.6-5.4 units,
	// and the water-reflection frustum (the same camera mirrored through the water plane)
	// lands 15-20 away, so this separates them with margin at the moment of acquisition.
	static constexpr float BOOTSTRAP_ORBIT_DISTANCE = 12.0f;

	// Once acquired the main camera is followed by its frustum OBJECT, so it may travel
	// any distance from the character — cutscenes, wide rooms and elevators all move the
	// camera well past orbit range. Only a gap this absurd means the lock is stale (the
	// engine recycled the frustum, or a map block changed the coordinate space) and the
	// camera has to be acquired again.
	static constexpr float LOCK_SANITY_DISTANCE = 300.0f;

	bool get_camera_apex_pos(float& x, float& y, float& z)
	{
		frustum_culling::apex_sample apexes[frustum_culling::MAX_FRAME_APEXES];
		const int count = frustum_culling::frame_apexes(apexes, static_cast<int>(std::size(apexes)));
		s_lock.frustums_seen = count;
		if (count == 0)
		{
			s_lock.frustum = nullptr;
			return false;
		}

		// read_player_pos returns render order (X, Y-up, Z-depth); apexes come straight
		// from the engine's planes in its raw {X, Z, Y} order.
		float px = 0.0f, py = 0.0f, pz = 0.0f;
		const bool has_char = read_player_pos(px, py, pz);
		const float ref_x = px;
		const float ref_y = pz;
		const float ref_z = py;

		const auto distance_to_char = [&](const frustum_culling::apex_sample& a)
		{
			const float dx = a.x - ref_x;
			const float dy = a.y - ref_y;
			const float dz = a.z - ref_z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		};

		const auto accept = [&](const frustum_culling::apex_sample& a, float distance)
		{
			s_lock.frustum = a.frustum;
			s_lock.distance_to_char = distance;
			x = a.x;
			y = a.y;
			z = a.z;
			return true;
		};

		// Follow the frustum we already identified. This is what survives the camera
		// leaving orbit range: picking the nearest apex to the character every frame
		// silently loses the camera the moment it pulls back, and the caller then
		// substitutes the player position — which differs from the camera by the orbit
		// offset and therefore changes whenever the camera TURNS. That substitution is
		// exactly the "lights slide when I look around" symptom.
		if (s_lock.frustum)
		{
			for (int i = 0; i < count; ++i)
			{
				if (apexes[i].frustum != s_lock.frustum)
				{
					continue;
				}
				const float distance = has_char ? distance_to_char(apexes[i]) : -1.0f;
				if (has_char && distance > LOCK_SANITY_DISTANCE)
				{
					break; // stale lock — fall through and re-acquire
				}
				return accept(apexes[i], distance);
			}
			s_lock.frustum = nullptr;
			++s_lock.relocks;
		}

		if (!has_char)
		{
			return false;
		}

		int best = -1;
		float best_distance = 0.0f;
		for (int i = 0; i < count; ++i)
		{
			const float distance = distance_to_char(apexes[i]);
			if (best < 0 || distance < best_distance)
			{
				best = i;
				best_distance = distance;
			}
		}

		if (best < 0 || best_distance > BOOTSTRAP_ORBIT_DISTANCE)
		{
			return false;
		}

		return accept(apexes[best], best_distance);
	}

	bool get_world_pos(float& x, float& y, float& z)
	{
		// Camera world position approximated by player position — camera orbits the player
		// and is typically within 3 units of the player origin.
		return read_player_pos(x, y, z);
	}

	bool get_char_pos(float& x, float& y, float& z)
	{
		if (shared::globals::lookdev_host)
		{
			const auto& f = host_feed::frame();
			x = f.player[0]; y = f.player[1]; z = f.player[2];
			return f.player_valid;
		}
		return read_player_pos(x, y, z);
	}

	bool get_char_heading(float& radians)
	{
		if (shared::globals::lookdev_host)
		{
			radians = host_feed::frame().heading;
			return host_feed::frame().heading_valid;
		}
		if (!world_chr_man_addr) return false;
		const DWORD wcm = safe_read_ptr(world_chr_man_addr);
		const DWORD player_entity = wcm ? safe_read_ptr(wcm + 0x18) : 0;
		if (!player_entity) return false;
		radians = safe_read_float(player_entity + 0x04);
		return std::isfinite(radians) && std::fabs(radians) <= 7.0f;
	}

	// --- ImGui tab ---

	void tab_imgui()
	{
		auto& s = s_settings;
		auto& ffp = shared::common::ffp_state::get();

		// Status
		if (is_available())
			ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "WorldChrMan: FOUND (0x%08X)", world_chr_man_addr);
		else
			ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "WorldChrMan: NOT FOUND (AOB failed)");

		if (world_chr_man_addr)
		{
			DWORD wcm = safe_read_ptr(world_chr_man_addr);
			DWORD player = wcm ? safe_read_ptr(wcm + 0x18) : 0;
			ImGui::TextDisabled("  WCM ptr: 0x%08X  player entity: 0x%08X", wcm, player);
		}

		ImGui::Separator();
		ImGui::Spacing();

		// Position readouts — diagnostic, always shown
		ImGui::Text("Position Sources:");
		ImGui::Spacing();

		// c0-c6 are shared with the 2D/menu pass, so the basis is only a camera while
		// it stays orthonormal. Accepted/rejected counts show how often each layout wins.
		{
			const bool basis_ok = ffp.vs_camera_basis_is_orthonormal();
			ImGui::TextColored(basis_ok ? ImVec4(0.2f, 0.9f, 0.2f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
				"Camera basis: %s   accepted=%u  rejected=%u  latched=%s",
				basis_ok ? "ORTHONORMAL (camera)" : "NOT A CAMERA (menu/2D block)",
				ffp.camera_basis_accepted(), ffp.camera_basis_rejected(),
				ffp.have_valid_camera() ? "yes" : "no");
		}

		// c4-c6.w from VS constants (what apply_transforms actually uses)
		if (ffp.view_proj_valid())
		{
			float vx, vy, vz;
			ffp.camera_world_pos(vx, vy, vz);
			ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
				"VS Constants (c4-c6.w):  X=%.2f  Y=%.2f  Z=%.2f", vx, vy, vz);
		}
		else
		{
			ImGui::TextDisabled("VS Constants:  (VP not valid yet)");
		}

		// Game memory camera (AOB pointer chain)
		{
			float gx = 0, gy = 0, gz = 0;
			bool ok = get_world_pos(gx, gy, gz);
			if (ok)
				ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
"Game Memory (player pos):  X=%.2f  Y=%.2f  Z=%.2f", gx, gy, gz);
			else
				ImGui::TextDisabled("Game Memory (player pos):  (unavailable)");
		}

		// Character position (same path as above since both use WorldChrMan chain)
		{
			float cx = 0, cy = 0, cz = 0;
			bool ok = get_char_pos(cx, cy, cz);
			if (ok)
				ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f),
				"Character (WorldChrMan):  X=%.2f  Y=%.2f  Z=%.2f", cx, cy, cz);
			else
				ImGui::TextDisabled("Character (WorldChrMan):  (unavailable)");
		}

		// Frustum apexes: the main pass never uploads the camera's absolute position, but
		// the engine builds culling frustums in world space, so each apex is a camera
		// position. The main camera's sits within the orbit distance of the character.
		{
			frustum_culling::apex_sample apexes[frustum_culling::MAX_FRAME_APEXES];
			const int count = frustum_culling::frame_apexes(apexes, static_cast<int>(std::size(apexes)));
			ImGui::Spacing();

			const auto& lk = lock_state();
			if (lk.used_player_fallback)
				ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
					"MAIN CAMERA NOT LOCKED — using the player position. Lights WILL slide as the camera turns.");
			else if (lk.frustum)
				ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
					"Main camera locked: frustum %p, %.1f units from the character, %u relock(s)",
					lk.frustum, lk.distance_to_char, lk.relocks);

			ImGui::Text("Frustum apexes this frame (%d):", count);
			if (count == 0)
			{
				ImGui::TextDisabled("  (none — culling hooks not running)");
			}
			float px = 0.0f, py = 0.0f, pz = 0.0f;
			const bool has_player = get_char_pos(px, py, pz);
			for (int i = 0; i < count; ++i)
			{
				const auto& a = apexes[i];
				const float dx = a.x - px, dy = a.y - py, dz = a.z - pz;
				const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
				const bool is_locked = a.frustum == lk.frustum;
				ImGui::TextColored(is_locked ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
					"  [%d] X=%.2f Y=%.2f Z=%.2f  tests=%u  dist to char=%.1f%s",
					i, a.x, a.y, a.z, a.tests, has_player ? dist : -1.0f, is_locked ? "  <-- main camera" : "");
			}
		}

		ImGui::Separator();
		ImGui::Spacing();

		// World-state controls (absolute coordinates for light placement and Remix path tracing)
		ImGui::Text("Absolute World State:");
		ImGui::Spacing();

		ImGui::Checkbox("Use Game Memory Camera", &s.use_game_camera);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Read camera position from game memory AOB.\nRequired for absolute world coordinates.");

		ImGui::Combo("Source", &s.override_source_mode, kOverrideSourceModes, static_cast<int>(std::size(kOverrideSourceModes)));

		ImGui::Checkbox("Camera From Frustum Apex", &s.use_frustum_apex);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(
				"The main pass renders camera-relative and never uploads the camera position.\n"
				"The engine builds culling frustums in world space, so the main camera's apex\n"
				"is that position. Falls back to the player position when unavailable.");

		ImGui::Checkbox("Inject into View Matrix", &s.inject_view_pos);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Place camera at its absolute world position in the View matrix.\nEnabling this without Use Game Memory Camera has no effect.");

		ImGui::Checkbox("Offset World Matrices", &s.offset_world);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Add camera world position to all object World matrix translations.\nConverts camera-relative object positions to absolute world space.\nRequired alongside Inject into View Matrix for correct results.");

		ImGui::DragFloat3("Manual Offset", s.manual_offset, 1.0f, -10000.0f, 10000.0f, "%.1f");

		ImGui::Spacing();

		float ox = 0.0f, oy = 0.0f, oz = 0.0f;
		const char* source_label = nullptr;
		if (get_override_pos(ox, oy, oz, &source_label))
		{
			ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
				"Active source (%s): %.2f %.2f %.2f", source_label ? source_label : "unknown", ox, oy, oz);
		}
		else if (s.use_game_camera)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
				"Game camera requested but unavailable or zero — check AOB status above.");
		}
		else
		{
			ImGui::TextDisabled("World state disabled — camera at (0,0,0), positions camera-relative.");
		}
	}
}
