#include "std_include.hpp"
#include "free_cam.hpp"
#include "game_build.hpp"
#include "shared/common/config.hpp"
#include "../modules/lookdev_capture.hpp"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace comp::game::free_cam
{
	namespace
	{
		// GameRend::Update (stdcall GameRend*, float dt; ret 8). GameRend + 0x14: culling camera, + 0x18: FrpgPersCam.
		constexpr DWORD kGameRendUpdate = 0x00EB3420;
		constexpr unsigned char kGameRendUpdateBytes[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x44, 0x01, 0x00, 0x00 };
		constexpr float kPi = 3.14159265f;
		constexpr float kPitchLimit = 89.0f * kPi / 180.0f;

		using gamerend_update_t = void(__stdcall*)(int gamerend, float dt);
		gamerend_update_t s_update_original = nullptr;

		bool s_available = false;
		std::atomic_bool s_want_active = false; // asked for (F3, the tab, a live key)
		bool s_running = false;                 // the hook's own view: free camera state initialised from the game's camera
		bool s_saved_frozen = false;            // input_frozen before the free camera took the input

		// The free camera (game thread only).
		float s_pos[3] = {};
		float s_yaw = 0.0f, s_pitch = 0.0f;
		float s_right_sign = 1.0f, s_up_sign = 1.0f, s_yaw_sign = 1.0f; // the game matrix's handedness, taken when it starts
		LARGE_INTEGER s_last_tick {};
		bool s_recenter = true; // the next frame only centres the cursor (after a start, a menu, a focus change)
		POINT s_cursor_ref {};  // where the cursor was left last frame: the reference for this frame's motion

		float s_speed = 6.0f;
		float s_sensitivity = 0.0025f;

		// Field of view (vertical, radians; camera + 0x50). s_fov_override false = the game's own, which s_game_fov follows.
		constexpr float kFovMin = 5.0f * kPi / 180.0f, kFovMax = 120.0f * kPi / 180.0f;
		std::atomic<long> s_wheel = 0;   // wheel motion not applied yet (any thread)
		std::atomic<float> s_fov_request = -1.0f; // a live key's value, degrees (0 = the game's), applied on the game thread
		bool s_fov_override = false;
		float s_fov = 0.0f, s_game_fov = 0.75f;
		bool s_middle_down = false;

		std::mutex s_nudge_mutex;
		float s_nudge[5] = {};
		bool s_nudge_pending = false;

		struct vec3 { float x, y, z; };
		vec3 cross(const vec3& a, const vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
		float dot(const vec3& a, const vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		vec3 normalized(const vec3& v)
		{
			const float l = std::sqrt(dot(v, v));
			return l > 1e-6f ? vec3{ v.x / l, v.y / l, v.z / l } : vec3{ 0.0f, 0.0f, 0.0f };
		}

		vec3 forward_of(float yaw, float pitch)
		{
			return { std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
		}

		// Right and up for a forward, with the game matrix's handedness.
		void basis(const vec3& f, vec3& r, vec3& u)
		{
			r = normalized(cross({ 0.0f, 1.0f, 0.0f }, f));
			r = { r.x * s_right_sign, r.y * s_right_sign, r.z * s_right_sign };
			u = normalized(cross(f, r));
			u = { u.x * s_up_sign, u.y * s_up_sign, u.z * s_up_sign };
		}

		bool finite(const float* v, int n)
		{
			for (int i = 0; i < n; i++)
				if (!std::isfinite(v[i]))
					return false;
			return true;
		}

		// Takes position and direction from the game's finished matrix, and its handedness.
		bool start_from(const float* m)
		{
			if (!finite(m, 16))
				return false;
			const vec3 r{ m[0], m[1], m[2] }, u{ m[4], m[5], m[6] }, f = normalized({ m[8], m[9], m[10] });
			if (dot(f, f) < 0.5f)
				return false;
			s_pos[0] = m[12]; s_pos[1] = m[13]; s_pos[2] = m[14];
			s_pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));
			s_yaw = std::atan2(f.x, f.z);
			s_right_sign = dot(r, cross({ 0.0f, 1.0f, 0.0f }, f)) < 0.0f ? -1.0f : 1.0f;
			s_up_sign = 1.0f;
			vec3 rr, uu;
			basis(f, rr, uu);
			s_up_sign = dot(u, uu) < 0.0f ? -1.0f : 1.0f;
			// Turning right is the way the right vector points.
			const vec3 dfdyaw{ std::cos(s_yaw), 0.0f, -std::sin(s_yaw) };
			s_yaw_sign = dot(dfdyaw, rr) < 0.0f ? -1.0f : 1.0f;
			QueryPerformanceCounter(&s_last_tick);
			s_recenter = true;
			{
				std::lock_guard lock(s_nudge_mutex);
				s_nudge_pending = false; // one sent while it was off would jump the start
			}
			s_fov_override = false;
			s_wheel = 0;
			return true;
		}

		bool key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

		bool window_center(POINT& center)
		{
			RECT rc;
			if (!shared::globals::main_window || !GetClientRect(shared::globals::main_window, &rc))
				return false;
			center = { (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };
			return ClientToScreen(shared::globals::main_window, &center) != FALSE;
		}

		// One frame of flying: keys and mouse, only with the game window in front and no menu open.
		void fly()
		{
			LARGE_INTEGER now, freq;
			QueryPerformanceCounter(&now);
			QueryPerformanceFrequency(&freq);
			const float dt = std::clamp(static_cast<float>(now.QuadPart - s_last_tick.QuadPart) / static_cast<float>(freq.QuadPart), 0.0f, 0.1f);
			s_last_tick = now;

			float nudge[5] = {};
			bool nudged = false;
			{
				std::lock_guard lock(s_nudge_mutex);
				if (s_nudge_pending)
				{
					std::memcpy(nudge, s_nudge, sizeof(nudge));
					s_nudge_pending = false;
					nudged = true;
				}
			}

			float move_f = 0.0f, move_r = 0.0f, move_u = 0.0f;
			const bool in_front = GetForegroundWindow() == shared::globals::main_window;
			const bool menu = shared::globals::imgui_menu_open || shared::globals::remix_menu_open;
			if (in_front && !menu)
			{
				float speed = s_speed;
				if (key(VK_SHIFT)) speed *= 5.0f;
				if (key(VK_CONTROL)) speed *= 0.2f;
				move_f = (key('W') ? 1.0f : 0.0f) - (key('S') ? 1.0f : 0.0f);
				move_r = (key('D') ? 1.0f : 0.0f) - (key('A') ? 1.0f : 0.0f);
				move_u = (key('E') || key(VK_SPACE) ? 1.0f : 0.0f) - (key('Q') ? 1.0f : 0.0f);
				move_f *= speed * dt; move_r *= speed * dt; move_u *= speed * dt;

				// Wheel: 10 % narrower per notch up, wider down. Middle button: the game's own field of view again.
				if (const long wheel = s_wheel.exchange(0); wheel != 0)
				{
					if (!s_fov_override)
						s_fov = s_game_fov;
					s_fov = std::clamp(s_fov * std::pow(0.9f, static_cast<float>(wheel) / WHEEL_DELTA), kFovMin, kFovMax);
					s_fov_override = true;
				}
				const bool middle = key(VK_MBUTTON);
				if (middle && !s_middle_down)
					s_fov_override = false;
				s_middle_down = middle;

				POINT center, cursor;
				if (window_center(center) && GetCursorPos(&cursor))
				{
					if (!s_recenter)
					{
						// Zoomed in, the same hand motion turns less (in proportion to the field of view).
						const float zoom = s_fov_override && s_game_fov > 0.0f ? s_fov / s_game_fov : 1.0f;
						s_yaw += s_yaw_sign * static_cast<float>(cursor.x - s_cursor_ref.x) * s_sensitivity * zoom;
						s_pitch -= static_cast<float>(cursor.y - s_cursor_ref.y) * s_sensitivity * zoom;
					}
					SetCursorPos(center.x, center.y);
					// Read back where it landed: measuring from the intended centre would spin the view whenever it cannot get there.
					if (!GetCursorPos(&s_cursor_ref))
						s_cursor_ref = center;
					s_recenter = false;
				}
			}
			else
			{
				s_recenter = true;
				s_wheel = 0; // scrolled in a menu or another window: not a zoom
			}

			if (nudged)
			{
				move_f += nudge[0]; move_r += nudge[1]; move_u += nudge[2];
				s_yaw += s_yaw_sign * nudge[3] * kPi / 180.0f;
				s_pitch += nudge[4] * kPi / 180.0f;
			}
			s_pitch = std::clamp(s_pitch, -kPitchLimit, kPitchLimit);
			s_yaw = std::remainder(s_yaw, 2.0f * kPi);

			const vec3 f = forward_of(s_yaw, s_pitch);
			vec3 r, u;
			basis(f, r, u);
			s_pos[0] += f.x * move_f + r.x * move_r;
			s_pos[1] += f.y * move_f + r.y * move_r + move_u;
			s_pos[2] += f.z * move_f + r.z * move_r;
		}

		// Writes the free camera into the game's matrix; each row's fourth value stays the game's.
		void write_to(float* m)
		{
			const vec3 f = forward_of(s_yaw, s_pitch);
			vec3 r, u;
			basis(f, r, u);
			m[0] = r.x; m[1] = r.y; m[2] = r.z;
			m[4] = u.x; m[5] = u.y; m[6] = u.z;
			m[8] = f.x; m[9] = f.y; m[10] = f.z;
			m[12] = s_pos[0]; m[13] = s_pos[1]; m[14] = s_pos[2];
		}

		void __stdcall gamerend_update_hk(int gamerend, float dt)
		{
			const int cull_cam = gamerend ? *reinterpret_cast<int*>(gamerend + 0x14) : 0;
			const int pers_cam = gamerend ? *reinterpret_cast<int*>(gamerend + 0x18) : 0;
			const bool want = s_want_active.load();
			if (cull_cam && want && !s_running)
			{
				s_running = start_from(reinterpret_cast<float*>(cull_cam + 0x10));
				if (s_running)
					shared::common::log("FreeCam", std::format("on at ({:.2f}, {:.2f}, {:.2f})", s_pos[0], s_pos[1], s_pos[2]));
			}
			else if (!want && s_running)
			{
				s_running = false;
				shared::common::log("FreeCam", "off: the game's camera again");
			}
			if (s_running && cull_cam)
			{
				if (const float fov = *reinterpret_cast<const float*>(cull_cam + 0x50); std::isfinite(fov) && fov > 0.01f && fov < 3.0f)
					s_game_fov = fov;
				if (const float request = s_fov_request.exchange(-1.0f); request >= 0.0f)
				{
					s_fov_override = request > 0.0f;
					s_fov = std::clamp(request * kPi / 180.0f, kFovMin, kFovMax);
				}
				fly();
				write_to(reinterpret_cast<float*>(cull_cam + 0x10));
				if (pers_cam && pers_cam != cull_cam)
					write_to(reinterpret_cast<float*>(pers_cam + 0x10));
				// GameRend::Update copies these over both cameras after this point: the map-entry camera (flag +0xE0, matrix
				// +0xF0, set by a map's setup) and the camera the culling camera follows when +0x24C is set ([+0x254]).
				const bool map_entry = *reinterpret_cast<const BYTE*>(gamerend + 0xE0) != 0;
				const int follow = *reinterpret_cast<const int*>(gamerend + 0x24C) ? *reinterpret_cast<const int*>(gamerend + 0x254) : 0;
				if (map_entry)
					write_to(reinterpret_cast<float*>(gamerend + 0xF0));
				if (follow)
					write_to(reinterpret_cast<float*>(follow + 0x10));
				// The zoom: vertical field of view at camera + 0x50 (the map-entry path takes it from GameRend + 0xE4).
				if (s_fov_override)
				{
					*reinterpret_cast<float*>(cull_cam + 0x50) = s_fov;
					if (pers_cam)
						*reinterpret_cast<float*>(pers_cam + 0x50) = s_fov;
					if (map_entry)
						*reinterpret_cast<float*>(gamerend + 0xE4) = s_fov;
					if (follow)
						*reinterpret_cast<float*>(follow + 0x50) = s_fov;
				}
			}
			s_update_original(gamerend, dt);
		}
	}

	void init()
	{
		// Dev mode, the normal game only: the lookdev game's F8 freeze uses the same input flag.
		if (!shared::common::config::get().dev.enabled || shared::globals::lookdev_host || shared::common::config::get().lookdev.enabled)
			return;
		auto* code = reinterpret_cast<unsigned char*>(build::at(kGameRendUpdate));
		if (!code || std::memcmp(code, kGameRendUpdateBytes, sizeof(kGameRendUpdateBytes)) != 0)
		{
			shared::common::log("FreeCam", "the renderer's camera update is not where it should be: no free camera",
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		if (!shared::utils::hook::detour(reinterpret_cast<DWORD>(code), reinterpret_cast<void*>(&gamerend_update_hk),
			reinterpret_cast<void**>(&s_update_original)))
		{
			shared::common::log("FreeCam", "hooking the renderer's camera update failed: no free camera", shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		s_available = true;
		shared::common::log("FreeCam", "ready (F3)");
	}

	bool available() { return s_available; }
	bool active() { return s_want_active.load(); }

	void set_active(bool on)
	{
		if (!s_available || on == s_want_active.load())
			return;
		// The player stands still: the game reads no input while the free camera flies (F8's freeze). Its own state comes back after.
		if (on)
		{
			s_saved_frozen = shared::globals::input_frozen;
			if (!s_saved_frozen)
				lookdev_capture::freeze_cursor_here();
			shared::globals::input_frozen = true;
		}
		else
			shared::globals::input_frozen = s_saved_frozen;
		s_want_active = on;
	}

	void toggle() { set_active(!s_want_active.load()); }

	void nudge(float forward, float right, float up, float yaw_degrees, float pitch_degrees)
	{
		const float values[5] = { forward, right, up, yaw_degrees, pitch_degrees };
		if (!finite(values, 5))
			return;
		std::lock_guard lock(s_nudge_mutex);
		s_nudge[0] = forward; s_nudge[1] = right; s_nudge[2] = up; s_nudge[3] = yaw_degrees; s_nudge[4] = pitch_degrees;
		s_nudge_pending = true;
	}

	void on_mouse_wheel(long delta)
	{
		if (delta != 0 && s_want_active.load())
			s_wheel += delta;
	}

	void set_fov_degrees(float degrees)
	{
		if (std::isfinite(degrees) && degrees >= 0.0f)
			s_fov_request = degrees;
	}

	float& speed() { return s_speed; }
	float& mouse_sensitivity() { return s_sensitivity; }

	void tab_imgui()
	{
		if (!s_available)
		{
			ImGui::TextDisabled("Free camera unavailable (this build's code differs)");
			return;
		}
		bool on = active();
		if (ImGui::Checkbox("Free Camera (F3)", &on))
			set_active(on);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Fly the game's camera: WASD to move, mouse to look, E / Space up, Q down, Shift faster, Ctrl slower.\n"
				"Mouse wheel zooms (field of view), the middle button returns to the game's field of view.\n"
				"The player stands still (the game gets no input) while the world keeps running; F7 still stops time.\n"
				"The game draws, culls and lights from this camera. Off returns to the game's camera.");
		ImGui::SetNextItemWidth(200.0f);
		ImGui::SliderFloat("Speed (m/s)", &s_speed, 0.5f, 50.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
		ImGui::SetNextItemWidth(200.0f);
		ImGui::SliderFloat("Mouse sensitivity", &s_sensitivity, 0.0005f, 0.01f, "%.4f", ImGuiSliderFlags_Logarithmic);
		if (active())
		{
			const float shown = (s_fov_override ? s_fov : s_game_fov) * 180.0f / kPi;
			ImGui::TextDisabled("Field of view %.1f deg (vertical)%s", shown, s_fov_override ? ", zoomed" : ", the game's");
			if (s_fov_override)
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("Game's field of view"))
					set_fov_degrees(0.0f);
			}
		}
	}
}
