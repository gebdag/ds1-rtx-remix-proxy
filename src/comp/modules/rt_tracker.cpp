#include "std_include.hpp"
#include "rt_tracker.hpp"
#include "shared/globals.hpp"

namespace comp::rt_tracker
{
	static IDirect3DDevice9* s_dev = nullptr;
	static bool s_initialized = false;
	static std::vector<rt_info> s_targets;
	static IDirect3DSurface9* s_backbuffer = nullptr;
	static UINT s_bb_width = 0;
	static UINT s_bb_height = 0;
	static int s_current_rt_idx = -1;   // index into s_targets of current RT0
	static int s_selected_idx = -1;      // user-selected RT index
	static mode s_mode = mode::passthrough;

	// Find or register a surface in s_targets. Returns index.
	static int find_or_register(IDirect3DSurface9* surface)
	{
		if (!surface) return -1;

		for (int i = 0; i < static_cast<int>(s_targets.size()); ++i)
		{
			if (s_targets[i].surface == surface)
				return i;
		}

		// New surface — query its properties
		rt_info info{};
		info.surface = surface;

		D3DSURFACE_DESC desc{};
		if (SUCCEEDED(surface->GetDesc(&desc)))
		{
			info.width = desc.Width;
			info.height = desc.Height;
			info.format = desc.Format;

			// First surface seen is the backbuffer — use its size as reference
			if (s_targets.empty())
			{
				s_backbuffer = surface;
				s_bb_width = desc.Width;
				s_bb_height = desc.Height;
				info.is_backbuffer = true;
				info.matches_backbuffer_size = true;
				shared::common::log("RT_Tracker",
					std::format("Backbuffer detected: 0x{:X} {}x{} fmt={}",
						reinterpret_cast<uintptr_t>(surface), desc.Width, desc.Height, static_cast<int>(desc.Format)));
			}
			else
			{
				info.matches_backbuffer_size = (desc.Width == s_bb_width && desc.Height == s_bb_height);
				info.is_backbuffer = false;
			}
		}

		s_targets.push_back(info);
		return static_cast<int>(s_targets.size()) - 1;
	}

	void init(IDirect3DDevice9* dev)
	{
		s_dev = dev;
		s_targets.clear();
		s_backbuffer = nullptr;
		s_bb_width = 0;
		s_bb_height = 0;
		s_current_rt_idx = -1;

		s_initialized = true;
		shared::common::log("RT_Tracker", "Init: deferred (lazy discovery)");
	}

	void on_reset()
	{
		// All surfaces are invalidated on Reset — clear tracking.
		s_targets.clear();
		s_backbuffer = nullptr;
		s_bb_width = 0;
		s_bb_height = 0;
		s_current_rt_idx = -1;
		s_selected_idx = -1;
		s_mode = mode::passthrough;
	}

	IDirect3DSurface9* on_set_render_target(DWORD index, IDirect3DSurface9* requested)
	{
		if (!s_initialized) return requested;

		// null_selected: intercept before per-slot tracking so it applies to all indices.
		if (s_mode == mode::null_selected && requested != nullptr)
		{
			if (s_selected_idx >= 0 && s_selected_idx < static_cast<int>(s_targets.size()) &&
				s_targets[s_selected_idx].surface == requested)
			{
				// RT0 cannot be nullptr — redirect to backbuffer. Slots 1+ can be truly nulled.
				return (index == 0 && s_backbuffer) ? s_backbuffer : nullptr;
			}
		}

		if (index != 0) return requested;

		int idx = find_or_register(requested);
		s_current_rt_idx = idx;

		if (idx >= 0)
			s_targets[idx].set_count_this_frame++;

		switch (s_mode)
		{
		case mode::lock_selected:
			if (s_selected_idx >= 0 && s_selected_idx < static_cast<int>(s_targets.size()))
				return s_targets[s_selected_idx].surface;
			break;

		case mode::backbuffer_only:
			if (s_backbuffer)
				return s_backbuffer;
			break;

		case mode::passthrough:
		case mode::null_selected:
		default:
			break;
		}

		return requested;
	}

	void on_draw()
	{
		if (!s_initialized) return;
		if (s_current_rt_idx >= 0 && s_current_rt_idx < static_cast<int>(s_targets.size()))
			s_targets[s_current_rt_idx].draws_this_frame++;
	}

	void on_present()
	{
		if (!s_initialized) return;
		for (auto& t : s_targets)
		{
			t.draws_last_frame = t.draws_this_frame;
			t.draws_this_frame = 0;
			t.set_count_last_frame = t.set_count_this_frame;
			t.set_count_this_frame = 0;
		}
	}

	mode current_mode() { return s_mode; }

	const rt_info* current_info()
	{
		if (s_current_rt_idx < 0 || s_current_rt_idx >= static_cast<int>(s_targets.size()))
		{
			return nullptr;
		}

		return &s_targets[s_current_rt_idx];
	}

	int selected_index() { return s_selected_idx; }

	const rt_info* get_info(int index)
	{
		if (index < 0 || index >= static_cast<int>(s_targets.size())) return nullptr;
		return &s_targets[index];
	}

	int count() { return static_cast<int>(s_targets.size()); }

	static const char* format_name(D3DFORMAT fmt)
	{
		switch (fmt)
		{
		case D3DFMT_A8R8G8B8:   return "A8R8G8B8";
		case D3DFMT_X8R8G8B8:   return "X8R8G8B8";
		case D3DFMT_R5G6B5:     return "R5G6B5";
		case D3DFMT_A1R5G5B5:   return "A1R5G5B5";
		case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
		case D3DFMT_A32B32G32R32F: return "A32B32G32R32F";
		case D3DFMT_R16F:       return "R16F";
		case D3DFMT_R32F:       return "R32F";
		case D3DFMT_G16R16F:    return "G16R16F";
		case D3DFMT_G32R32F:    return "G32R32F";
		case D3DFMT_D24S8:      return "D24S8";
		case D3DFMT_D24X8:      return "D24X8";
		case D3DFMT_D16:        return "D16";
		case D3DFMT_L8:         return "L8";
		case D3DFMT_A8:         return "A8";
		default:                return "???";
		}
	}

	void tab_imgui()
	{
		ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Render Target Router");
		ImGui::Separator();
		ImGui::Spacing();

		ImGui::TextWrapped(
			"Remix raytraces draws to RTs matching the backbuffer size (%ux%u). "
			"RTs of different size are rasterized only (invisible to RT).",
			s_bb_width, s_bb_height);
		ImGui::Spacing();

		// Mode selector
		int mode_int = static_cast<int>(s_mode);
		ImGui::Text("Routing Mode:");
		ImGui::RadioButton("Passthrough (observe only)", &mode_int, 0);
		ImGui::RadioButton("Lock to Selected RT", &mode_int, 1);
		ImGui::RadioButton("Backbuffer Only", &mode_int, 2);
		ImGui::RadioButton("Null Selected RT", &mode_int, 3);
		if (mode_int == 3)
			ImGui::TextDisabled("  Selected surface -> nullptr (slots 1+) or backbuffer (slot 0)");
		s_mode = static_cast<mode>(mode_int);

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		// RT table
		ImGui::Text("Render Targets (%d surfaces seen):", static_cast<int>(s_targets.size()));
		ImGui::Spacing();

		if (ImGui::BeginTable("rt_table", 7,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
		{
			ImGui::TableSetupColumn("Sel");
			ImGui::TableSetupColumn("#");
			ImGui::TableSetupColumn("Surface Ptr");
			ImGui::TableSetupColumn("Size");
			ImGui::TableSetupColumn("Format");
			ImGui::TableSetupColumn("Draws/F");
			ImGui::TableSetupColumn("Sets/F");
			ImGui::TableHeadersRow();

			for (int i = 0; i < static_cast<int>(s_targets.size()); ++i)
			{
				const auto& t = s_targets[i];
				ImGui::TableNextRow();

				// Highlight current RT
				if (i == s_current_rt_idx)
				{
					ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
						ImGui::GetColorU32(ImVec4(0.2f, 0.4f, 0.2f, 0.5f)));
				}

				// Selection radio button
				ImGui::TableNextColumn();
				ImGui::PushID(i);
				if (ImGui::RadioButton("##sel", s_selected_idx == i))
					s_selected_idx = i;
				ImGui::PopID();

				// Index
				ImGui::TableNextColumn();
				ImGui::Text("%d", i);

				// Surface pointer + flags
				ImGui::TableNextColumn();
				char buf[64];
				snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(t.surface)));
				ImGui::Text("%s", buf);
				if (t.is_backbuffer)
				{
					ImGui::SameLine();
					ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "(BB)");
				}
				else if (t.matches_backbuffer_size)
				{
					ImGui::SameLine();
					ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "(=BB)");
				}

				// Size
				ImGui::TableNextColumn();
				ImGui::Text("%ux%u", t.width, t.height);

				// Format
				ImGui::TableNextColumn();
				ImGui::Text("%s", format_name(t.format));

				// Draws per frame
				ImGui::TableNextColumn();
				if (t.draws_last_frame > 0)
					ImGui::Text("%u", t.draws_last_frame);
				else
					ImGui::TextDisabled("0");

				// SetRT calls per frame
				ImGui::TableNextColumn();
				if (t.set_count_last_frame > 0)
					ImGui::Text("%u", t.set_count_last_frame);
				else
					ImGui::TextDisabled("0");
			}

			ImGui::EndTable();
		}

		ImGui::Spacing();

		// Selected info
		if (s_selected_idx >= 0 && s_selected_idx < static_cast<int>(s_targets.size()))
		{
			const auto& sel = s_targets[s_selected_idx];
			ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f),
				"Selected: #%d  %ux%u  %s  %u draws/frame",
				s_selected_idx, sel.width, sel.height, format_name(sel.format), sel.draws_last_frame);

			if (sel.matches_backbuffer_size)
				ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
					"Size matches backbuffer -- Remix will raytrace draws to this RT");
			else
				ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
					"Size does NOT match backbuffer (%ux%u) -- Remix will rasterize only",
					s_bb_width, s_bb_height);
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		// Legend
		ImGui::TextDisabled("(BB) = Backbuffer  (=BB) = Same size as backbuffer");
		ImGui::TextDisabled("Green row = currently active RT0");
		ImGui::TextDisabled("Lock mode: all SetRenderTarget(0) redirected to selected surface");
		ImGui::TextDisabled("Null mode: selected surface replaced with nullptr (or BB for slot 0)");
	}
}
