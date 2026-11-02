#include "std_include.hpp"
#include "hud_scale.hpp"
#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <format>
#include <mutex>
#include <set>
#include <string>

namespace comp::game::hud_scale
{
	namespace
	{
		// EzMenu: the draw of every dialog (153 classes share it) and of every object in one (20 share it),
		// both thiscall (this, draw context). Their first bytes, checked before hooking.
		constexpr DWORD kDialogDraw = 0x00787E90;
		constexpr DWORD kObjectDraw = 0x00788D80;
		constexpr unsigned char kDialogDrawBytes[] = { 0x83, 0xB9, 0xA0, 0x00, 0x00, 0x00, 0x00 };
		constexpr unsigned char kObjectDrawBytes[] = { 0x57, 0x8B, 0xF9, 0xF6, 0x47, 0x18, 0x02 };
		// A dialog's menu.drb record (+0, its name) and an object's name (+0, its text).
		constexpr DWORD kDialogRecord = 0x2C;
		constexpr DWORD kObjectName = 0x1C;
		// An object's rectangle in its dialog: float x0, y0, x1, y1 through this vtable slot.
		constexpr DWORD kObjectRectSlot = 0x54;
		// NS_FRPG::FrpgMenuDc and its command buffer: write cursor, size, base.
		constexpr DWORD kMenuDcVtable = 0x011D0404;
		constexpr DWORD kDcBuffer = 0x24;
		constexpr DWORD kBufferCursor = 0x14;
		constexpr DWORD kBufferSize = 0x18;
		constexpr DWORD kBufferBase = 0x1C;

		constexpr float kWidth = 1280.0f, kHeight = 720.0f;

		enum group { corners, boss, enemy, notices, banners, group_count };

		struct group_info { const char* key; const char* label; const char* help; };
		constexpr group_info kGroups[group_count] = {
			{ "ScaleCorners", "HUD", "Health and stamina, humanity, status icons, equipment slots, item names, souls" },
			{ "ScaleBoss", "Boss bar", "The boss's name and health, towards the bottom centre" },
			{ "ScaleEnemy", "Enemy bars and lock-on", "Stay on their target, smaller around their own centre" },
			{ "ScaleNotices", "Prompts and notices", "Action prompts, items received, status effect bars, invasion notices" },
			{ "ScaleBanners", "Banners", "YOU DIED, area names, bonfire lit: towards the centre of the screen" },
		};
		float g_scale[group_count] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		bool g_any_scaled = false;
		bool g_log_names = false;
		bool g_installed = false;

		// An object of a dialog: by name prefix, which group, and what it shrinks towards.
		struct element
		{
			const wchar_t* prefix;
			group g;
			float x, y;
			bool own_centre;
		};

		constexpr element kHudElements[] = {
			// Top left. The bars are 732 wide at any stat level (a control shortens them), so by name, not rect.
			{ L"plate_", corners, 0.0f, 0.0f, false },
			{ L"bar_", corners, 0.0f, 0.0f, false }, // bar_0_hp, bar_BG_0, bar_start ...
			{ L"mg_", corners, 0.0f, 0.0f, false },
			{ L"HP", corners, 0.0f, 0.0f, false }, // "HP icon", HP_endbar, HP_startbar
			{ L"SP", corners, 0.0f, 0.0f, false },
			{ L"MP", corners, 0.0f, 0.0f, false },
			{ L"icon_state", corners, 0.0f, 0.0f, false },
			{ L"icon_broke", corners, 0.0f, 0.0f, false },
			{ L"F20-02", corners, 0.0f, 0.0f, false }, // the humanity ring
			// Bottom left.
			{ L"F20-00_", corners, 0.0f, kHeight, false }, // magic, item, arms_l, arms_r, arrow_l, arrow_r
			{ L"text_magic_name", corners, 0.0f, kHeight, false },
			{ L"text_item_name", corners, 0.0f, kHeight, false },
			// Bottom right.
			{ L"soul_param", corners, kWidth, kHeight, false },
			// The rest.
			{ L"F20_boss_HP", boss, kWidth * 0.5f, kHeight, false },
			{ L"enemy_HP", enemy, 0.0f, 0.0f, true },
			{ L"F20_LockCursor", enemy, 0.0f, 0.0f, true },
			{ L"F20_state_abnormal", notices, kWidth * 0.5f, kHeight * 0.5f, false }, // and _bar_0-3
			{ L"map_name", notices, kWidth * 0.5f, kHeight * 0.5f, false },
		};
		// Holding a bow: the arrow panel goes with the slots, the aiming reticle stays.
		constexpr element kAimElements[] = {
			{ L"bg_parchment_0", corners, 0.0f, kHeight, false },
			{ L"F20-01_arrow", corners, 0.0f, kHeight, false },
			{ L"category_", corners, 0.0f, kHeight, false },
		};
		constexpr element kFloatingElements[] = {
			{ L"pc", enemy, 0.0f, 0.0f, true },
		};
		struct dialog_elements { const wchar_t* dialog; const element* elements; size_t count; };
		constexpr dialog_elements kDialogs[] = {
			{ L"F20-00", kHudElements, std::size(kHudElements) },
			{ L"F20-01", kAimElements, std::size(kAimElements) },
			{ L"F21", kFloatingElements, std::size(kFloatingElements) },
		};
		// Whole dialogs.
		struct whole_dialog { const wchar_t* name; group g; float x, y; };
		constexpr whole_dialog kWholeDialogs[] = {
			{ L"F03", notices, kWidth * 0.5f, kHeight },
			{ L"F22", notices, kWidth * 0.5f, kHeight },
			{ L"F25", notices, kWidth * 0.5f, kHeight * 0.5f },
			{ L"F23", banners, kWidth * 0.5f, kHeight * 0.5f },
		};

		// ----- Where the dialogs record -----

		struct walk_state
		{
			const wchar_t* dialog = nullptr;
			int depth = 0;       // objects open inside the dialog
			bool bracketed = false;
		};
		thread_local walk_state t_walk;

		using draw_fn = void(__fastcall*)(void* self, void* edx, void* dc);
		draw_fn g_dialog_draw = nullptr;
		draw_fn g_object_draw = nullptr;

		const wchar_t* dialog_name(const void* dialog)
		{
			__try {
				const auto* record = *reinterpret_cast<const wchar_t* const* const*>(static_cast<const char*>(dialog) + kDialogRecord);
				return record ? *record : nullptr;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return nullptr;
			}
		}

		const wchar_t* object_name(const void* object)
		{
			__try {
				const auto* name = *reinterpret_cast<const wchar_t* const* const*>(static_cast<const char*>(object) + kObjectName);
				return name ? *name : nullptr;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return nullptr;
			}
		}

		// Names come from the game's loaded menu.drb: guarded like every other read of its data.
		bool starts_with(const wchar_t* text, const wchar_t* prefix)
		{
			if (!text)
				return false;
			__try {
				return std::wcsncmp(text, prefix, std::wcslen(prefix)) == 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool same_text(const wchar_t* text, const wchar_t* other)
		{
			if (!text)
				return false;
			__try {
				return std::wcscmp(text, other) == 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool object_centre(void* object, float* x, float* y)
		{
			float rect[4] = {};
			__try {
				using rect_fn = float* (__thiscall*)(void*, float*);
				const auto fn = reinterpret_cast<rect_fn>((*static_cast<void***>(object))[kObjectRectSlot / 4]);
				fn(object, rect);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
			*x = (rect[0] + rect[2]) * 0.5f;
			*y = (rect[1] + rect[3]) * 0.5f;
			return std::isfinite(*x) && std::isfinite(*y);
		}

		// ----- The replay side: the render thread, as the buffer replays -----

		struct replay_state
		{
			float scale = 1.0f;
			float x = 0.0f, y = 0.0f;
			bool on = false;
			UINT frame = 0;
		};
		replay_state g_replay;

		// The buffer's words, as the game's own commands read their arguments. ECX is the buffer.
		float next_float(char* buffer)
		{
			auto& cursor = *reinterpret_cast<DWORD*>(buffer + kBufferCursor);
			const DWORD at = (cursor + 3) & ~3u;
			cursor = at + 4;
			return *reinterpret_cast<const float*>(at);
		}

		void __fastcall command_on(char* buffer)
		{
			g_replay.scale = next_float(buffer);
			g_replay.x = next_float(buffer);
			g_replay.y = next_float(buffer);
			g_replay.on = true;
			g_replay.frame = shared::common::ffp_state::get().frame_count();
		}

		void __fastcall command_off(char*)
		{
			g_replay.on = false;
		}

		// ----- Writing the commands -----

		// False when this is not the game's menu context, or its buffer has no room to spare.
		bool write_command(void* dc, void(__fastcall* command)(char*), const float* args, int count)
		{
			__try {
				auto* context = static_cast<char*>(dc);
				if (*reinterpret_cast<DWORD*>(context) != kMenuDcVtable)
					return false;
				auto* buffer = *reinterpret_cast<char**>(context + kDcBuffer);
				if (!buffer)
					return false;
				auto& cursor = *reinterpret_cast<DWORD*>(buffer + kBufferCursor);
				const DWORD end = *reinterpret_cast<DWORD*>(buffer + kBufferBase) + *reinterpret_cast<DWORD*>(buffer + kBufferSize);
				// Room for this, its closing command, and some of what is between them.
				if (((cursor + 3) & ~3u) + 4u * (count + 1) + 4096u >= end)
					return false;
				const DWORD at = (cursor + 3) & ~3u;
				*reinterpret_cast<void**>(at) = reinterpret_cast<void*>(command);
				for (int i = 0; i < count; i++)
					*reinterpret_cast<float*>(at + 4 + 4 * i) = args[i];
				cursor = at + 4u * (count + 1);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool open_bracket(void* dc, float scale, float x, float y)
		{
			const float args[3] = { scale, x, y };
			return write_command(dc, command_on, args, 3);
		}

		void close_bracket(void* dc)
		{
			// When the buffer has filled up the bracket stays open until the frame ends (draw_scope checks).
			write_command(dc, command_off, nullptr, 0);
		}

		// F4 > HUD > Log the UI's names: what is drawn where it would be looked up, once each.
		void log_name(const wchar_t* dialog, const wchar_t* object, const char* what, float x = 0.0f, float y = 0.0f)
		{
			if (!g_log_names)
				return;
			static std::mutex mutex;
			static std::set<std::wstring> seen;
			std::wstring key = std::wstring(dialog ? dialog : L"?") + L"/" + (object ? object : L"");
			std::lock_guard lock(mutex);
			if (seen.size() >= 600 || !seen.insert(key).second)
				return;
			std::string text;
			for (const wchar_t c : key)
				text.push_back(c < 128 ? static_cast<char>(c) : '?');
			shared::common::log("HudScale", std::format("{} [{}] at {:.0f},{:.0f}", text, what, x, y));
		}

		void __fastcall dialog_draw_detour(void* self, void* edx, void* dc)
		{
			if (!g_any_scaled && !g_log_names)
				return g_dialog_draw(self, edx, dc);

			const walk_state outer = t_walk;
			const wchar_t* name = dialog_name(self);
			t_walk.dialog = name;
			t_walk.depth = 0;

			bool bracket = false;
			if (!outer.bracketed)
				for (const auto& whole : kWholeDialogs)
					if (same_text(name, whole.name))
					{
						log_name(name, nullptr, kGroups[whole.g].label);
						const float scale = g_scale[whole.g];
						if (scale != 1.0f)
							bracket = open_bracket(dc, scale, whole.x, whole.y);
						break;
					}
			t_walk.bracketed = outer.bracketed || bracket;

			g_dialog_draw(self, edx, dc);

			if (bracket)
				close_bracket(dc);
			t_walk = outer;
		}

		const element* find_element(const wchar_t* dialog, const wchar_t* object)
		{
			for (const auto& entry : kDialogs)
			{
				if (!same_text(dialog, entry.dialog))
					continue;
				for (size_t i = 0; i < entry.count; i++)
					if (starts_with(object, entry.elements[i].prefix))
						return &entry.elements[i];
				return nullptr;
			}
			return nullptr;
		}

		void __fastcall object_draw_detour(void* self, void* edx, void* dc)
		{
			if (!g_any_scaled && !g_log_names)
				return g_object_draw(self, edx, dc);

			bool bracket = false;
			if (t_walk.depth == 0 && !t_walk.bracketed)
			{
				const wchar_t* name = object_name(self);
				if (const auto* part = find_element(t_walk.dialog, name))
				{
					float x = part->x, y = part->y;
					const float scale = g_scale[part->g];
					const bool placed = !part->own_centre || object_centre(self, &x, &y);
					log_name(t_walk.dialog, name, kGroups[part->g].label, x, y);
					if (scale != 1.0f && placed)
						bracket = open_bracket(dc, scale, x, y);
				}
				else
				{
					float x = 0.0f, y = 0.0f;
					if (g_log_names)
						object_centre(self, &x, &y);
					log_name(t_walk.dialog, name, "not scaled", x, y);
				}
			}
			const bool outer_bracketed = t_walk.bracketed;
			t_walk.bracketed = outer_bracketed || bracket;
			t_walk.depth++;

			g_object_draw(self, edx, dc);

			t_walk.depth--;
			t_walk.bracketed = outer_bracketed;
			if (bracket)
				close_bracket(dc);
		}

		void refresh_any()
		{
			g_any_scaled = std::any_of(std::begin(g_scale), std::end(g_scale), [](float s) { return s != 1.0f; });
		}
	}

	void install()
	{
		auto& cfg = shared::common::config::get();
		if (g_installed || shared::globals::lookdev_host)
			return;
		for (int g = 0; g < group_count; g++)
			g_scale[g] = std::clamp(cfg.get_float("Hud", kGroups[g].key, 1.0f), 0.3f, 1.5f);
		refresh_any();

		if (std::memcmp(reinterpret_cast<const void*>(kDialogDraw), kDialogDrawBytes, sizeof(kDialogDrawBytes)) != 0 ||
			std::memcmp(reinterpret_cast<const void*>(kObjectDraw), kObjectDrawBytes, sizeof(kObjectDrawBytes)) != 0)
		{
			shared::common::log("HudScale", "the dialog draws are not where they should be: HUD scaling is off",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}
		if (MH_CreateHook(reinterpret_cast<void*>(kDialogDraw), &dialog_draw_detour, reinterpret_cast<void**>(&g_dialog_draw)) != MH_OK ||
			MH_CreateHook(reinterpret_cast<void*>(kObjectDraw), &object_draw_detour, reinterpret_cast<void**>(&g_object_draw)) != MH_OK)
		{
			shared::common::log("HudScale", "hooking the dialog draws failed: HUD scaling is off",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}
		g_installed = true;
		shared::common::log("HudScale", "dialog draws hooked");
	}

	draw_scope::draw_scope(IDirect3DDevice9* dev)
	{
		if (!g_replay.on || g_replay.scale == 1.0f)
			return;
		const auto& ffp = shared::common::ffp_state::get();
		if (g_replay.frame != ffp.frame_count())
		{
			g_replay.on = false;
			return;
		}
		// The UI's c0 and c1: ndc = x * c.x + y * c.y + c.w, x and y in the virtual 1280x720. Anything else is no
		// UI draw of ours.
		std::memcpy(constants_, ffp.vs_const_data(), sizeof(constants_));
		if (!(std::abs(constants_[0]) > 1e-5f) || !(std::abs(constants_[5]) > 1e-5f))
			return;

		const float s = g_replay.scale, x = g_replay.x, y = g_replay.y;
		float scaled[8];
		std::memcpy(scaled, constants_, sizeof(scaled));
		for (int row = 0; row < 2; row++)
		{
			const float* c = &constants_[row * 4];
			float* out = &scaled[row * 4];
			out[0] = c[0] * s;
			out[1] = c[1] * s;
			out[3] = c[3] + (1.0f - s) * (x * c[0] + y * c[1]);
		}
		dev_ = dev;
		dev->SetVertexShaderConstantF(0, scaled, 2);

		// A scissor the game set for this part goes with it. In target pixels, and no use for the whole target.
		DWORD scissor_on = FALSE;
		D3DVIEWPORT9 viewport {};
		if (SUCCEEDED(dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor_on)) && scissor_on &&
			SUCCEEDED(dev->GetViewport(&viewport)) && SUCCEEDED(dev->GetScissorRect(&scissor_)))
		{
			const bool whole = scissor_.left <= static_cast<LONG>(viewport.X) && scissor_.top <= static_cast<LONG>(viewport.Y) &&
				scissor_.right >= static_cast<LONG>(viewport.X + viewport.Width) && scissor_.bottom >= static_cast<LONG>(viewport.Y + viewport.Height);
			if (!whole)
			{
				const float px = viewport.X + x * viewport.Width / kWidth, py = viewport.Y + y * viewport.Height / kHeight;
				RECT rect;
				rect.left = std::lround(px + (scissor_.left - px) * s);
				rect.right = std::lround(px + (scissor_.right - px) * s);
				rect.top = std::lround(py + (scissor_.top - py) * s);
				rect.bottom = std::lround(py + (scissor_.bottom - py) * s);
				dev->SetScissorRect(&rect);
				scissor_changed_ = true;
			}
		}
	}

	draw_scope::~draw_scope()
	{
		if (!dev_)
			return;
		dev_->SetVertexShaderConstantF(0, constants_, 2);
		if (scissor_changed_)
			dev_->SetScissorRect(&scissor_);
	}

	void tab_imgui()
	{
		if (!g_installed)
		{
			ImGui::TextUnformatted("HUD scaling is not available (lookdev host, or the game's dialog code was not found).");
			return;
		}
		auto& cfg = shared::common::config::get();
		ImGui::TextWrapped("Sizes of the in-game HUD. Each part shrinks towards its own corner. Pause menus, shops and conversations stay as they are.");
		ImGui::Spacing();
		bool changed = false;
		for (int g = 0; g < group_count; g++)
		{
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
			float percent = std::round(g_scale[g] * 100.0f);
			if (ImGui::SliderFloat(kGroups[g].label, &percent, 30.0f, 150.0f, "%.0f %%"))
			{
				g_scale[g] = std::round(percent / 5.0f) * 5.0f / 100.0f;
				changed = true;
			}
			if (ImGui::IsItemDeactivatedAfterEdit())
				cfg.set_string("Hud", kGroups[g].key, std::format("{:.2f}", g_scale[g]));
			ImGui::TextDisabled("%s", kGroups[g].help);
			ImGui::Spacing();
		}
		if (ImGui::Button("All at 100%"))
		{
			for (int g = 0; g < group_count; g++)
			{
				g_scale[g] = 1.0f;
				cfg.set_string("Hud", kGroups[g].key, "1.00");
			}
			changed = true;
		}
		if (changed)
			refresh_any();
		ImGui::Spacing();
		ImGui::Checkbox("Log the UI's names to console.log", &g_log_names);
	}
}
