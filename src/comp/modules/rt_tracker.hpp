#pragma once

namespace comp
{
	namespace rt_tracker
	{
		enum class mode { passthrough, lock_selected, backbuffer_only, null_selected };

		struct rt_info
		{
			IDirect3DSurface9* surface = nullptr;
			UINT width = 0;
			UINT height = 0;
			D3DFORMAT format = D3DFMT_UNKNOWN;
			UINT draws_this_frame = 0;
			UINT draws_last_frame = 0;
			UINT set_count_this_frame = 0;
			UINT set_count_last_frame = 0;
			bool is_backbuffer = false;
			bool matches_backbuffer_size = false;
		};

		void init(IDirect3DDevice9* dev);
		void on_reset();

		// Called from d3d9ex proxy hooks
		IDirect3DSurface9* on_set_render_target(DWORD index, IDirect3DSurface9* requested);
		void on_draw();
		void on_present();

		// ImGui
		void tab_imgui();

		// State
		mode current_mode();
		const rt_info* current_info();
		int selected_index();
		const rt_info* get_info(int index);
		int count();
	}
}
