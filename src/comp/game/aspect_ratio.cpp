#include "std_include.hpp"
#include "aspect_ratio.hpp"
#include "game_build.hpp"
#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/globals.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <numeric>
#include <string>
#include <vector>

namespace comp::game::aspect_ratio
{
	bool g_active = false;

	namespace
	{
		constexpr float kDesigned = 16.0f / 9.0f;
		// 1366x768 and 1360x768 are 16:9 screens.
		constexpr float kSameShape = 0.01f;

		// Every camera's projection, for the shaders, the frustum and the UI's world positions: fov in xmm0, then
		// cdecl aspect, near, far, the matrix written to and returned in EAX.
		constexpr DWORD kProjection = 0x00453720;
		constexpr unsigned char kProjectionBytes[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };

		// The windowed sizes (FrpgWindow's constructor fills it, FUN_00F3B0F0 and the options menu read it): an object
		// with begin, end, capacity at +4, +8, +0xC. Its add takes the object in EAX and the entry on the stack, ret 4.
		constexpr DWORD kWindowList = 0x01378A14;
		constexpr DWORD kWindowListAdd = 0x0041BA00;
		constexpr unsigned char kWindowListAddBytes[] = { 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x0C };

		// The table's last entry in FrpgWindow's constructor: mov [esp+9Ch], 3840 and mov [esp+0A0h], 2160. The size it
		// starts at is the last entry that fits the desktop.
		constexpr DWORD kTableLastWidth = 0x00F7016C;
		constexpr DWORD kTableLastHeight = 0x00F70177;
		constexpr unsigned char kTableLastWidthBytes[] = { 0xC7, 0x84, 0x24, 0x9C, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00 };
		constexpr unsigned char kTableLastHeightBytes[] = { 0xC7, 0x84, 0x24, 0xA0, 0x00, 0x00, 0x00, 0x70, 0x08, 0x00, 0x00 };
		constexpr int kTableLastW = 3840, kTableLastH = 2160;

		struct window_size
		{
			int width, height, refresh, format;
			short ratio_x, ratio_y;
		};
		static_assert(sizeof(window_size) == 0x14);

		float g_fit_x = 1.0f, g_fit_y = 1.0f;
		bool g_enabled = false;
		bool g_config_read = false;
		bool g_hook_tried = false;
		bool g_warned = false;
		int g_position = 0; // 0 automatic, 1 centred, 2 at the edges

		bool g_table_patched = false;

		void* g_projection = nullptr;

		// The matrix of a 16:9 camera, with what the screen needs. Registers and xmm untouched: the game's own callers
		// of this function have private conventions (x87 only here, EAX stays the matrix).
		__declspec(naked) void projection_detour()
		{
			__asm
			{
				push dword ptr [esp + 0Ch]
				push dword ptr [esp + 0Ch]
				push dword ptr [esp + 0Ch]
				call g_projection
				add esp, 0Ch
				cmp dword ptr [esp + 4], 3FE38E39h // 16:9, as the game's cameras hold it
				jne done
				fld dword ptr [eax]
				fmul dword ptr g_fit_x
				fstp dword ptr [eax]
				fld dword ptr [eax + 14h]
				fmul dword ptr g_fit_y
				fstp dword ptr [eax + 14h]
			done:
				ret
			}
		}

		void install_projection_hook()
		{
			if (g_hook_tried || shared::globals::lookdev_host)
				return;
			g_hook_tried = true;
			// Addresses here are the 2015-06-15 build's; build::at() gives the running build's (0 = not available).
			const DWORD projection = build::at(kProjection);
			if (!projection || std::memcmp(reinterpret_cast<const void*>(projection), kProjectionBytes, sizeof(kProjectionBytes)) != 0)
			{
				shared::common::log("Aspect", "the camera projection is not where it should be: the view stays 16:9 for the game's own logic",
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
				return;
			}
			if (MH_CreateHook(reinterpret_cast<void*>(projection), reinterpret_cast<void*>(&projection_detour), &g_projection) != MH_OK ||
				MH_EnableHook(reinterpret_cast<void*>(projection)) != MH_OK)
			{
				shared::common::log("Aspect", "hooking the camera projection failed", shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
				return;
			}
			shared::common::log("Aspect", "camera projection hooked");
		}

		bool same_shape(float aspect) { return std::abs(aspect - kDesigned) < kSameShape; }

		bool add_entry(window_size* entry)
		{
			DWORD list = build::at(kWindowList), function = build::at(kWindowListAdd);
			if (!list || !function)
				return false;
			__try
			{
				__asm
				{
					mov eax, list
					push entry
					mov ecx, function
					call ecx
				}
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool list_range(window_size*& first, window_size*& last)
		{
			const DWORD list = build::at(kWindowList);
			if (!list)
				return false;
			__try
			{
				first = *reinterpret_cast<window_size**>(list + 4);
				last = *reinterpret_cast<window_size**>(list + 8);
				return first && last > first;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		std::string ini_path()
		{
			char base[MAX_PATH] {};
			const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
			return n == 0 || n >= MAX_PATH ? std::string() : std::string(base) + "\\NBGI\\DarkSouls\\DarkSouls.ini";
		}

		// A fullscreen game has its own list, from the adapter's real modes: window sizes mean nothing to it.
		bool ini_says_fullscreen()
		{
			const std::string path = ini_path();
			return !path.empty() && GetPrivateProfileIntA("DisplaySetting", "WindowMode", 1, path.c_str()) == 0;
		}

		// The window size in the game's ini, 0 when there is none.
		void ini_window_size(int& width, int& height)
		{
			width = height = 0;
			const std::string path = ini_path();
			if (path.empty())
				return;
			width = static_cast<int>(GetPrivateProfileIntA("DisplaySettingWindow", "Width", 0, path.c_str()));
			height = static_cast<int>(GetPrivateProfileIntA("DisplaySettingWindow", "Height", 0, path.c_str()));
		}

		bool desktop_size(int& width, int& height)
		{
			RECT desktop {};
			if (!GetWindowRect(GetDesktopWindow(), &desktop))
				return false;
			width = desktop.right - desktop.left;
			height = desktop.bottom - desktop.top;
			return width > 0 && height > 0;
		}

		bool usable_ini_size(int width, int height, int desktop_width, int desktop_height)
		{
			return width >= 640 && height >= 360 && width <= desktop_width && height <= desktop_height && !same_shape(float(width) / float(height));
		}

		// The sizes this screen's owner could want that the game's 16:9 table lacks: the size set in the game's ini
		// and, on a screen that is not 16:9, the desktop's size and the usual ones of its shape. None on a 16:9 screen
		// whose ini holds a 16:9 size, which leaves the game's list as it was.
		void add_window_sizes()
		{
			window_size* first = nullptr, * last = nullptr;
			const DWORD list_add = build::at(kWindowListAdd);
			if (!list_add || std::memcmp(reinterpret_cast<const void*>(list_add), kWindowListAddBytes, sizeof(kWindowListAddBytes)) != 0 || !list_range(first, last))
				return;

			int desktop_width = 0, desktop_height = 0;
			if (!desktop_size(desktop_width, desktop_height))
				return;

			std::vector<std::pair<int, int>> wanted;
			// The place the ini's size took in the table.
			if (g_table_patched && kTableLastW <= desktop_width && kTableLastH <= desktop_height)
				wanted.emplace_back(kTableLastW, kTableLastH);
			const float desktop_aspect = float(desktop_width) / float(desktop_height);
			if (!same_shape(desktop_aspect))
			{
				wanted.emplace_back(desktop_width, desktop_height);
				constexpr int kUsual[][2] = {
					{ 1024, 768 }, { 1280, 960 }, { 1600, 1200 }, { 1920, 1440 }, { 2048, 1536 },
					{ 1280, 800 }, { 1440, 900 }, { 1680, 1050 }, { 1920, 1200 }, { 2560, 1600 }, { 3840, 2400 },
					{ 1720, 720 }, { 1920, 800 }, { 2560, 1080 }, { 3440, 1440 }, { 3840, 1600 }, { 5120, 2160 },
					{ 2560, 720 }, { 3840, 1080 }, { 5120, 1440 },
				};
				for (const auto& size : kUsual)
					if (size[0] <= desktop_width && size[1] <= desktop_height && std::abs(float(size[0]) / float(size[1]) - desktop_aspect) < 0.04f)
						wanted.emplace_back(size[0], size[1]);
			}
			int ini_width = 0, ini_height = 0;
			ini_window_size(ini_width, ini_height);
			if (!ini_says_fullscreen() && usable_ini_size(ini_width, ini_height, desktop_width, desktop_height))
				wanted.emplace_back(ini_width, ini_height);

			const int refresh = first->refresh, format = first->format;
			int added = 0;
			for (const auto& [width, height] : wanted)
			{
				if (!list_range(first, last))
					return;
				if (std::any_of(first, last, [&, w = width, h = height](const window_size& s) { return s.width == w && s.height == h; }))
					continue;
				const int divisor = std::gcd(width, height);
				window_size entry { width, height, refresh, format, static_cast<short>(width / divisor), static_cast<short>(height / divisor) };
				if (!add_entry(&entry))
					return;
				shared::common::log("Aspect", std::format("window size {}x{} added to the game's list", width, height));
				added++;
			}
			// The options menu lists them in this order.
			if ((added || g_table_patched) && list_range(first, last))
				std::sort(first, last, [](const window_size& a, const window_size& b) { return a.width * a.height < b.width * b.height; });
		}

		void read_config()
		{
			if (g_config_read)
				return;
			g_config_read = true;
			auto& cfg = shared::common::config::get();
			g_enabled = cfg.get_bool("Aspect", "Enable", true) && !cfg.lookdev.enabled;
			g_position = std::clamp(cfg.get_int("Aspect", "HudPosition", 0), 0, 2);
		}
	}

	void before_game_window()
	{
		static bool done = false;
		if (done)
			return;
		done = true;
		read_config();
		if (!g_enabled || shared::globals::lookdev_host || ini_says_fullscreen())
			return;
		int desktop_width = 0, desktop_height = 0, width = 0, height = 0;
		if (!desktop_size(desktop_width, desktop_height))
			return;
		ini_window_size(width, height);
		// No setting yet, on a screen that is not 16:9: its whole size, as a 16:9 screen gets.
		if (width == 0 && height == 0 && !same_shape(float(desktop_width) / float(desktop_height)))
		{
			width = desktop_width;
			height = desktop_height;
		}
		if (!usable_ini_size(width, height, desktop_width, desktop_height))
			return;
		const DWORD last_width = build::at(kTableLastWidth), last_height = build::at(kTableLastHeight);
		if (!last_width || !last_height ||
			std::memcmp(reinterpret_cast<const void*>(last_width), kTableLastWidthBytes, sizeof(kTableLastWidthBytes)) != 0 ||
			std::memcmp(reinterpret_cast<const void*>(last_height), kTableLastHeightBytes, sizeof(kTableLastHeightBytes)) != 0)
		{
			shared::common::log("Aspect", "the game's window size table is not where it should be: it starts at a 16:9 size", shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}
		const auto write_int = [](DWORD address, int value)
		{
			DWORD old = 0;
			if (!VirtualProtect(reinterpret_cast<void*>(address), sizeof(value), PAGE_EXECUTE_READWRITE, &old))
				return false;
			std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
			VirtualProtect(reinterpret_cast<void*>(address), sizeof(value), old, &old);
			FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), sizeof(value));
			return true;
		};
		constexpr DWORD kImmediate = 7;
		if (write_int(last_width + kImmediate, width) && write_int(last_height + kImmediate, height))
		{
			g_table_patched = true;
			shared::common::log("Aspect", std::format("the game starts at {}x{}", width, height));
		}
	}

	void on_game_resolution(UINT width, UINT height, bool creating_device)
	{
		read_config();
		if (!g_enabled)
			return;
		if (creating_device && !shared::globals::lookdev_host)
			add_window_sizes();
		const float aspect = width && height ? float(width) / float(height) : kDesigned;
		const bool wanted = !same_shape(aspect);
		g_fit_x = wanted && aspect > kDesigned ? kDesigned / aspect : 1.0f;
		g_fit_y = wanted && aspect < kDesigned ? aspect / kDesigned : 1.0f;
		shared::common::ffp_state::get().set_ui_fit(g_fit_x, g_fit_y);
		if (wanted)
			install_projection_hook();
		if (wanted != g_active)
			shared::common::log("Aspect", wanted ? std::format("output {}x{}: view and HUD follow its shape (experimental)", width, height) : std::string("output is 16:9: nothing to do"));
		g_active = wanted;
	}

	float fit_x() { return g_fit_x; }
	float fit_y() { return g_fit_y; }

	const float* camera_constants(UINT start, const float* data, UINT count)
	{
		if (start != 0 || count < 4 || !data)
			return data;
		// A camera: row 3 the view direction with w 0, x and y rows in the proportion of a 16:9 projection.
		const auto length = [](const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
		const float x = length(data), y = length(data + 4);
		if (std::abs(data[15]) > 1e-5f || std::abs(length(data + 12) - 1.0f) > 0.01f || y < 0.5f || std::abs(x / y - 9.0f / 16.0f) > 0.001f)
			return data;
		if (!g_warned && !shared::globals::lookdev_host)
		{
			g_warned = true;
			shared::common::log("Aspect", "a 16:9 camera reached the shaders past the projection hook: corrected here, but culling may still be 16:9",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}
		thread_local std::vector<float> copy;
		copy.assign(data, data + static_cast<size_t>(count) * 4);
		for (int i = 0; i < 4; i++)
		{
			copy[i] *= g_fit_x;
			copy[4 + i] *= g_fit_y;
		}
		return copy.data();
	}

	const void* final_quad(const void* vertices, UINT stride, UINT count, IDirect3DVertexDeclaration9* declaration)
	{
		// Position float x, y, then u, v.
		constexpr UINT kStride = 16, kCount = 4;
		if (!vertices || stride != kStride || count != kCount || !declaration)
			return vertices;
		D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH] {};
		UINT element_count = 0;
		if (FAILED(declaration->GetDeclaration(elements, &element_count)) || elements[0].Usage != D3DDECLUSAGE_POSITION || elements[0].Type != D3DDECLTYPE_FLOAT2)
			return vertices;
		thread_local float quad[4 * 4];
		std::memcpy(quad, vertices, sizeof(quad));
		const float height = std::abs(quad[1]);
		int above = 0;
		for (UINT i = 0; i < kCount; i++)
		{
			const float x = quad[i * 4], y = quad[i * 4 + 1];
			// As wide as the target and centred on it, two corners at the image's top and two at its bottom: anything else is
			// another draw.
			if (std::abs(x) != 1.0f || !std::isfinite(y) || y == 0.0f || std::abs(y) != height)
				return vertices;
			// Up or down by the texture coordinate, not by the game's y: on a wider screen the game's y is a huge value whose
			// sign is upside down (3440x1440: the image's top row at y -2982610), and keeping that sign showed the whole frame
			// upside down. The image's top row (v near 0) goes to the top of the target.
			const float v = quad[i * 4 + 3];
			if (!std::isfinite(v))
				return vertices;
			const bool top = v < 0.5f;
			above += top ? 1 : 0;
			quad[i * 4 + 1] = top ? 1.0f : -1.0f;
		}
		{
			// Dev log, once per height the game draws its final quad at: each corner's position and texture coordinate.
			static float logged_height = 0.0f;
			if (above == 2 && height != 1.0f && height != logged_height)
			{
				logged_height = height;
				const auto* raw = static_cast<const float*>(vertices);
				std::string corners;
				for (UINT i = 0; i < kCount; i++)
					corners += std::format(" ({:g}, {:g} | {:g}, {:g})", raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], raw[i * 4 + 3]);
				shared::common::log("Aspect", std::format("final quad, x y | u v as the game draws it:{}", corners));
			}
		}
		return above == 2 && height != 1.0f ? quad : vertices;
	}

	bool hud_at_edges()
	{
		// Automatic: an ultrawide keeps the HUD where the eye is; a narrower screen has no room to lose.
		return g_position == 2 || (g_position == 0 && g_fit_y < 1.0f);
	}

	void hud_position_imgui()
	{
		if (!g_active)
			return;
		static const char* kNames[] = { "Automatic", "Centred", "At the screen's edges" };
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		if (ImGui::Combo("HUD position", &g_position, kNames, IM_ARRAYSIZE(kNames)))
			shared::common::config::get().set_string("Aspect", "HudPosition", std::to_string(g_position));
		ImGui::TextDisabled("This screen isn't 16:9. Automatic: centred on ultrawide, at the edges on narrower screens.");
		ImGui::Spacing();
	}
}
