#include "std_include.hpp"

#include "modules/imgui.hpp"
#include "modules/renderer.hpp"
#include "modules/diagnostics.hpp"
#include "modules/skinning.hpp"
#include "modules/tracer.hpp"
#include "modules/game_lights.hpp"
#include "modules/lookdev_capture.hpp"
#include "shared/common/remix_api.hpp"
#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"

#include "shared/common/dinput_hook_v2.hpp"

#include "game/camera.hpp"
#include "game/atmosphere.hpp"
#include "game/swamp.hpp"
#include "game/vanilla_view.hpp"
#include "game/d3dx_texture_watch.hpp"
#include "game/frustum_culling.hpp"

namespace comp
{
	// Bridge: provides real camera position from game memory to the shared ffp_state layer
	static bool camera_pos_provider(float& x, float& y, float& z)
	{
		return game::camera::get_override_pos(x, y, z);
	}

	void on_begin_scene_cb()
	{
		if (!tex_addons::initialized) {
			tex_addons::init_texture_addons();
		}

		// Retry Remix API init — the call in main() fails with NOT_INITIALIZED (code 11)
		// because it runs before CreateDevice. By BeginScene the device exists.
		// Not without Remix (lookdev mode): each attempt would log a warning.
		if (!shared::common::remix_api::is_initialized() && shared::common::config::get().remix.enabled)
			shared::common::remix_api::initialize(nullptr, nullptr, nullptr);

		// Vanilla graphics on/off (F8 / F4 menu) switches here, before the frame's draws.
		game::vanilla_view::frame_update();

		// Update game lights each frame
		if (auto* gl = game_lights::get())
			gl->frame_update();

		// Hand the game's haze and grade to Remix (rtx.ds1Haze.*, rtx.ds1grading.game*)
		game::atmosphere::frame_update(shared::common::ffp_state::get().frame_count());

		// The swamp's constants and the last frame's wake rings (rtx.ds1Swamp.*).
		game::swamp::frame_update(shared::common::ffp_state::get().frame_count());

		// Sync camera override flags to ffp_state each frame
		{
			auto& ffp = shared::common::ffp_state::get();
			auto& cam_s = game::camera::get_settings();
			ffp.set_camera_override(camera_pos_provider);
			ffp.set_inject_view_pos(cam_s.inject_view_pos);
			ffp.set_offset_world(cam_s.offset_world);
		}

		const auto& im = imgui::get();
		if (im->m_dbg_use_fake_camera)
		{
			D3DXMATRIX view_matrix
			(
				1.0f, 0.0f, 0.0f, 0.0f,
				0.0f, 0.447f, 0.894f, 0.0f,
				0.0f, -0.894f, 0.447f, 0.0f,
				0.0f, 100.0f, -50.0f, 1.0f
			);

			D3DXMATRIX proj_matrix
			(
				1.359f, 0.0f, 0.0f, 0.0f,
				0.0f, 2.414f, 0.0f, 0.0f,
				0.0f, 0.0f, 1.001f, 1.0f,
				0.0f, 0.0f, -1.0f, 0.0f
			);

			// Construct view matrix
			D3DXMATRIX rotation, translation;
			D3DXMatrixRotationYawPitchRoll(&rotation,
				D3DXToRadian(im->m_dbg_camera_yaw),		// Yaw in radians
				D3DXToRadian(im->m_dbg_camera_pitch),	// Pitch in radians
				0.0f);									// No roll for simplicity

			D3DXMatrixTranslation(&translation,
				-im->m_dbg_camera_pos[0], // Negate for camera (moves world opposite)
				-im->m_dbg_camera_pos[1],
				-im->m_dbg_camera_pos[2]);

			D3DXMatrixMultiply(&view_matrix, &rotation, &translation);

			// Construct projection matrix
			D3DXMatrixPerspectiveFovLH(&proj_matrix,
				D3DXToRadian(im->m_dbg_camera_fov), // FOV in radians
				im->m_dbg_camera_aspect,
				im->m_dbg_camera_near_plane,
				im->m_dbg_camera_far_plane);

			shared::globals::d3d_device->SetTransform(D3DTS_WORLD, &shared::globals::IDENTITY);
			shared::globals::d3d_device->SetTransform(D3DTS_VIEW, &view_matrix);
			shared::globals::d3d_device->SetTransform(D3DTS_PROJECTION, &proj_matrix);
		}


		// Safe because ffp_state::apply_transforms overwrites WORLD per-draw when world_dirty_.
		// Games that write world constants every frame (the common case) will never see this identity.
		// Remove if your game sets world constants once and reuses across frames.
		shared::globals::d3d_device->SetTransform(D3DTS_WORLD, &shared::globals::IDENTITY);

		// Example: if you found a game struct with view/proj matrices, apply them here
		//if (const auto viewport = game::vp; viewport)
		//{
		//	shared::globals::d3d_device->SetTransform(D3DTS_VIEW, &viewport->view);
		//	shared::globals::d3d_device->SetTransform(D3DTS_PROJECTION, &viewport->proj);
		//}
	}


	void main()
	{
		// #Step 2: init remix api if you want to use it or comment it otherwise
		// Requires "exposeRemixApi = True" in the "bridge.conf" that is located in the .trex folder
		shared::common::remix_api::initialize(nullptr, nullptr, nullptr);

		// Core modules
		shared::common::loader::module_loader::register_module(std::make_unique<tracer>());
		shared::common::loader::module_loader::register_module(std::make_unique<imgui>());
		shared::common::loader::module_loader::register_module(std::make_unique<renderer>());

		auto& cfg = shared::common::config::get();

		// Diagnostics module (on-demand capture available from ImGui when loaded)
		if (cfg.diagnostics.enabled)
			shared::common::loader::module_loader::register_module(std::make_unique<diagnostics>());

		// Skinning module — always registered so the ImGui toggle works at runtime.
		shared::common::loader::module_loader::register_module(std::make_unique<skinning>());

		// Game lights module
		shared::common::loader::module_loader::register_module(std::make_unique<game_lights>());

		shared::common::loader::module_loader::register_module(std::make_unique<shared::common::dinput_v2>());

		// Lookdev frame capture: lookdev mode only (-lookdev, remix-comp-proxy.lookdev.ini)
		if (cfg.lookdev.enabled)
			shared::common::loader::module_loader::register_module(std::make_unique<lookdev_capture>());

		game::d3dx_texture_watch::install();

		MH_EnableHook(MH_ALL_HOOKS);
	}
}
