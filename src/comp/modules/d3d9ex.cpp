// IDirect3DDevice9 proxy — thin delegation hub for all 119 vtable methods.
#include "std_include.hpp"
#include "d3d9ex.hpp"
#include "../d3d9_proxy.hpp"

#include "imgui.hpp"
#include "renderer.hpp"
#include "tracer.hpp"
#include "diagnostics.hpp"
#include "skinning.hpp"
#include "rt_tracker.hpp"
#include "benchmark.hpp"
#include "perf.hpp"
#include "crash_context.hpp"
#include "shared/common/shader_cache.hpp"

using comp::tracer;
#include "tracer_dispatch.inc"
#include "shared/common/ffp_state.hpp"
#include "shared/common/ffp_prof.hpp"
#include "shared/common/config.hpp"
#include "../game/fps_unlock.hpp"
#include "../game/render_toggles.hpp"
#include "../game/texture_names.hpp"
#include "../game/sky_meshes.hpp"
#include "../game/atmosphere.hpp"
#include "../game/lightmap_sun.hpp"

namespace {
	// Per-frame UP draw pass counter for individual post-FX control
	static int s_postfx_pass_idx = 0;

	// Lookdev mode is the vanilla reference: draws and render targets go to the
	// device untouched, past every Remix-oriented route (prepass drop, UV
	// expansion, sky/HUD markers, post-FX skip, category filters, RT redirects).
	bool lookdev_passthrough()
	{
		return shared::common::config::get().lookdev.enabled;
	}

	// Remix composites its frame at the first UI-marked draw, else at Present. DS1 draws its
	// scene, then the HUD and subtitles, into its scene target, and a final 2D pass copies that
	// target through the brightness LUT (s1, 256x1) to the back buffer. With UI on screen the
	// composite lands in the scene target and the final pass shows it, as in the game. Without
	// UI (cutscenes) nothing composited before Present, so the final pass reached Remix as scene
	// geometry: vertex capture made its full-screen quad a plane in front of the camera, which
	// darkened the view and cut across it near the camera. Before the frame's first draw into the
	// back buffer, a degenerate UI-marked triangle drawn into the texture that draw presents makes
	// Remix composite there; the final pass then runs after the composite, rasterized, as the
	// game's own pass on the game's own image.
	bool s_composite_requested = false; // this frame; reset at Present

	void composite_before_backbuffer(IDirect3DDevice9* dev, UINT stride)
	{
		// The same UI marking the HUD path uses; without it Remix composites at Present.
		if (s_composite_requested || !shared::common::config::get().hud.mark_ui_draws)
			return;
		IDirect3DSurface9* rt = nullptr;
		IDirect3DSurface9* back = nullptr;
		dev->GetRenderTarget(0, &rt);
		dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
		const bool to_back = rt && rt == back;
		if (back)
			back->Release();
		if (!to_back)
		{
			if (rt)
				rt->Release();
			return;
		}
		s_composite_requested = true;

		IDirect3DBaseTexture9* source = nullptr;
		IDirect3DSurface9* target = nullptr;
		if (SUCCEEDED(dev->GetTexture(0, &source)) && source && source->GetType() == D3DRTYPE_TEXTURE)
		{
			D3DSURFACE_DESC src_desc {}, rt_desc {};
			static_cast<IDirect3DTexture9*>(source)->GetLevelDesc(0, &src_desc);
			rt->GetDesc(&rt_desc);
			if ((src_desc.Usage & D3DUSAGE_RENDERTARGET) && src_desc.Width == rt_desc.Width && src_desc.Height == rt_desc.Height)
				static_cast<IDirect3DTexture9*>(source)->GetSurfaceLevel(0, &target);
		}
		if (target)
		{
			constexpr DWORD kUiMarker = 0xD15D0011;
			D3DVIEWPORT9 viewport {};
			RECT scissor {};
			DWORD constant = 0;
			dev->GetViewport(&viewport);
			dev->GetScissorRect(&scissor);
			dev->GetTextureStageState(0, D3DTSS_CONSTANT, &constant);
			dev->SetTexture(0, nullptr);
			dev->SetRenderTarget(0, target);
			dev->SetTextureStageState(0, D3DTSS_CONSTANT, kUiMarker);
			std::vector<BYTE> zero(3 * static_cast<size_t>(stride), 0);
			dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, zero.data(), stride);
			dev->SetTextureStageState(0, D3DTSS_CONSTANT, constant);
			// SetRenderTarget resets the viewport and the scissor rect to the whole target.
			dev->SetRenderTarget(0, rt);
			dev->SetViewport(&viewport);
			dev->SetScissorRect(&scissor);
			dev->SetTexture(0, source);
			target->Release();
		}
		if (source)
			source->Release();
		rt->Release();
	}

	// DS1's particles (FRPG_Sfx_* pixel shaders) reach Remix as shader draws, and
	// Remix builds their material from the stage-0 texture stage state, not the
	// shader. Every sprite shader computes colour = texture * vertex colour and
	// alpha = texture.a * vertex.a (fog, scattering and the game's exposure on
	// top, the same for the whole family), but the stage state at that point was
	// whatever the last converted draw left, and the pixel shader itself was still
	// swallowed by the FFP latch. Flames fade through vertex alpha on textures
	// whose alpha is 1, so an alpha without the vertex term drew every strip at
	// full strength. sfx_draw restores the game's shaders and transforms as any
	// passthrough draw does, states the shader's maths for the draw, and restores
	// the stage state after. The screen-space families, which Remix can only get
	// wrong (encoded depth, frame-copy distortion, blurs), are dropped.
	template <class Draw>
	HRESULT sfx_draw(IDirect3DDevice9* dev, shared::common::sfx_class cls, Draw&& draw)
	{
		using shared::common::sfx_class;
		const auto& cfg = shared::common::config::get().sfx;
		auto& ffp = shared::common::ffp_state::get();

		if (cfg.hide)
			return S_OK;
		if (cls == sfx_class::DepthEncode || cls == sfx_class::Distortion || cls == sfx_class::Blur)
		{
			if (cfg.drop_screen_effects)
				return S_OK;
			ffp.with_cached_transforms([&] { ffp.disengage(dev); });
			return draw();
		}

		constexpr D3DTEXTURESTAGESTATETYPE states[] = {
			D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2 };
		constexpr DWORD values[] = {
			D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE };
		DWORD saved[std::size(states)] = {};
		for (size_t i = 0; i < std::size(states); i++)
			dev->GetTextureStageState(0, states[i], &saved[i]);

		static bool logged = false;
		if (!logged)
		{
			logged = true;
			shared::common::log("Sfx", std::format("first particle draw: FFP latch {}, stage 0 colour op {} ({:#x}, {:#x}), alpha op {} ({:#x}, {:#x})",
				ffp.is_ffp_active() ? "engaged (pixel shader swallowed)" : "off",
				saved[0], saved[1], saved[2], saved[3], saved[4], saved[5]));
		}

		ffp.with_cached_transforms([&] { ffp.disengage(dev); });
		if (!cfg.translate)
			return draw();

		for (size_t i = 0; i < std::size(states); i++)
			dev->SetTextureStageState(0, states[i], values[i]);
		const auto hr = draw();
		for (size_t i = 0; i < std::size(states); i++)
			dev->SetTextureStageState(0, states[i], saved[i]);
		return hr;
	}

	shared::common::sfx_class current_sfx()
	{
		return shared::common::ffp_state::get().cur_ps_sfx_class();
	}

	// Sprite particles of UP draws go fixed-function (ffp_state::draw_sprite_up);
	// true when it drew. Everything else, and anything it cannot place, takes sfx_draw.
	bool sfx_sprite_up(IDirect3DDevice9* dev, shared::common::sfx_class cls, D3DPRIMITIVETYPE prim, UINT prim_count,
		UINT min_index, UINT num_verts, const void* indices, D3DFORMAT index_format,
		const void* vertices, UINT stride, HRESULT& hr)
	{
		using shared::common::sfx_class;
		const auto& cfg = shared::common::config::get().sfx;
		if (cfg.hide || !cfg.ffp_sprites)
			return false;
		if (cls != sfx_class::Sprite && cls != sfx_class::SoftSprite && cls != sfx_class::LitSprite)
			return false;

		auto& ffp = shared::common::ffp_state::get();
		static uint32_t drawn = 0, fallback = 0, frame = 0, frames_logged = 0;
		if (frame != ffp.frame_count())
		{
			if (frame && (drawn || fallback) && (frames_logged < 3 || frame % 600 == 0))
			{
				frames_logged++;
				shared::common::log("Sfx", std::format("frame {}: {} sprites drawn fixed-function, {} as shader draws", frame, drawn, fallback));
			}
			frame = ffp.frame_count();
			drawn = fallback = 0;
		}
		const bool ok = ffp.draw_sprite_up(dev, prim, prim_count, min_index, num_verts,
			indices, index_format, vertices, stride, cls != sfx_class::LitSprite, hr);
		(ok ? drawn : fallback)++;
		return ok;
	}

	// D3DFORMAT is either a small enum value or a FourCC ('DXT1', 'ATI2', ...).
	std::string format_tag(D3DFORMAT format)
	{
		const auto v = static_cast<DWORD>(format);
		if (v >= 0x20202020)
		{
			const char fourcc[5] = {
				static_cast<char>(v), static_cast<char>(v >> 8),
				static_cast<char>(v >> 16), static_cast<char>(v >> 24), '\0'
			};
			return fourcc;
		}
		return std::to_string(v);
	}

	// CheckDeviceFormat rejections are ordinary capability probing, so record
	// each distinct format once rather than flooding the log or the breadcrumb
	// ring. FourCC formats are the ones worth escalating: the Remix runtime maps
	// several the game uses (AL16, R16) to "unsupported", and D3DX turns that
	// into the D3DERR_NOTAVAILABLE the engine treats as fatal.
	void note_format_rejection(D3DFORMAT format, D3DRESOURCETYPE type, DWORD usage)
	{
		static std::mutex mutex;
		static std::set<std::pair<DWORD, DWORD>> seen;
		{
			std::lock_guard lock(mutex);
			if (!seen.emplace(static_cast<DWORD>(format), static_cast<DWORD>(type)).second)
				return;
		}

		const auto tag = format_tag(format);
		shared::common::log("FormatCheck", std::format("rejected {} rtype={} usage=0x{:X}",
			tag, static_cast<int>(type), usage));

		if (static_cast<DWORD>(format) >= 0x20202020)
			comp::crash_context::note("fmt rejected %s rt=%u", tag.c_str(), type);
	}

	// Draw-inspector mark: rebind all 8 stages to the placeholder texture
	// through the wrapper (so ffp_state tracking — and thus the FFP albedo
	// pick — sees it too), restore the tracked originals afterwards.
	struct inspector_mark_scope
	{
		IDirect3DDevice9* dev = nullptr;
		IDirect3DBaseTexture9* saved[8] = {};

		void apply(IDirect3DDevice9* wrapper)
		{
			dev = wrapper;
			auto& ffp = shared::common::ffp_state::get();
			auto* placeholder = comp::perf::inspector_placeholder_texture();
			for (UINT s = 0; s < 8; s++)
			{
				saved[s] = ffp.cur_texture(s);
				wrapper->SetTexture(s, placeholder);
			}
		}

		~inspector_mark_scope()
		{
			if (!dev) return;
			for (UINT s = 0; s < 8; s++)
				dev->SetTexture(s, saved[s]);
		}
	};
}

namespace comp
{
#pragma region D3D9Device

	HRESULT d3d9ex::D3D9Device::QueryInterface(REFIID riid, void** ppvObj)
	{
		TRACE_IF_ACTIVE(trace_QueryInterface, &riid, ppvObj);
		*ppvObj = nullptr;
		HRESULT hRes = m_pIDirect3DDevice9->QueryInterface(riid, ppvObj);

		if (hRes == NOERROR) {
			*ppvObj = this;
		}

		return hRes;
	}

	ULONG d3d9ex::D3D9Device::AddRef()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_AddRef);
		return m_pIDirect3DDevice9->AddRef();
	}

	ULONG d3d9ex::D3D9Device::Release()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_Release);
		ULONG count = m_pIDirect3DDevice9->Release();
		if (!count) delete this;
		return count;
	}

	HRESULT d3d9ex::D3D9Device::TestCooperativeLevel()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_TestCooperativeLevel);
		const auto hr = m_pIDirect3DDevice9->TestCooperativeLevel();
		// Transitions only — a lost device is polled every frame. A lost or
		// not-reset device makes resource creation fail, which is what the
		// engine turns into an assert.
		static HRESULT s_last_level = D3D_OK;
		if (hr != s_last_level)
		{
			s_last_level = hr;
			crash_context::note("TestCooperativeLevel -> %08X", hr);
		}
		return hr;
	}

	UINT d3d9ex::D3D9Device::GetAvailableTextureMem()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_GetAvailableTextureMem);
		return m_pIDirect3DDevice9->GetAvailableTextureMem();
	}

	HRESULT d3d9ex::D3D9Device::EvictManagedResources()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_EvictManagedResources);
		return m_pIDirect3DDevice9->EvictManagedResources();
	}

	HRESULT d3d9ex::D3D9Device::GetDirect3D(IDirect3D9** ppD3D9)
	{
		TRACE_IF_ACTIVE(trace_GetDirect3D, ppD3D9);
		return m_pIDirect3DDevice9->GetDirect3D(ppD3D9);
	}

	HRESULT d3d9ex::D3D9Device::GetDeviceCaps(D3DCAPS9* pCaps)
	{
		TRACE_IF_ACTIVE(trace_GetDeviceCaps, pCaps);
		return m_pIDirect3DDevice9->GetDeviceCaps(pCaps);
	}

	HRESULT d3d9ex::D3D9Device::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode)
	{
		TRACE_IF_ACTIVE(trace_GetDisplayMode, iSwapChain, pMode);
		return m_pIDirect3DDevice9->GetDisplayMode(iSwapChain, pMode);
	}

	HRESULT d3d9ex::D3D9Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters)
	{
		TRACE_IF_ACTIVE(trace_GetCreationParameters, pParameters);
		return m_pIDirect3DDevice9->GetCreationParameters(pParameters);
	}

	HRESULT d3d9ex::D3D9Device::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap)
	{
		TRACE_IF_ACTIVE(trace_SetCursorProperties, XHotSpot, YHotSpot, pCursorBitmap);
		return m_pIDirect3DDevice9->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap);
	}

	void d3d9ex::D3D9Device::SetCursorPosition(int X, int Y, DWORD Flags)
	{
		TRACE_IF_ACTIVE(trace_SetCursorPosition, X, Y, Flags);
		return m_pIDirect3DDevice9->SetCursorPosition(X, Y, Flags);
	}

	BOOL d3d9ex::D3D9Device::ShowCursor(BOOL bShow)
	{
		TRACE_IF_ACTIVE(trace_ShowCursor, bShow);
		return m_pIDirect3DDevice9->ShowCursor(bShow);
	}

	HRESULT d3d9ex::D3D9Device::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain)
	{
		TRACE_IF_ACTIVE(trace_CreateAdditionalSwapChain, pPresentationParameters, pSwapChain);
		return m_pIDirect3DDevice9->CreateAdditionalSwapChain(pPresentationParameters, pSwapChain);
	}

	HRESULT d3d9ex::D3D9Device::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain)
	{
		TRACE_IF_ACTIVE(trace_GetSwapChain, iSwapChain, pSwapChain);
		return m_pIDirect3DDevice9->GetSwapChain(iSwapChain, pSwapChain);
	}

	UINT d3d9ex::D3D9Device::GetNumberOfSwapChains()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_GetNumberOfSwapChains);
		return m_pIDirect3DDevice9->GetNumberOfSwapChains();
	}

	HRESULT d3d9ex::D3D9Device::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters)
	{
		TRACE_IF_ACTIVE(trace_Reset, pPresentationParameters);
		crash_context::note("device Reset %ux%u",
			pPresentationParameters ? pPresentationParameters->BackBufferWidth : 0,
			pPresentationParameters ? pPresentationParameters->BackBufferHeight : 0);
		if (auto* t = tracer::get()) t->on_reset();
		shared::common::ffp_state::get().on_reset();
		rt_tracker::on_reset();
		if (auto* s = skinning::get()) s->on_reset();
		shared::common::g_shader_cache.clear_cache();
		tex_addons::init_texture_addons(true);
		ImGui_ImplDX9_InvalidateDeviceObjects();
		const auto& res_cfg = shared::common::config::get().resolution;
		if (res_cfg.render_width > 0 && res_cfg.render_height > 0)
		{
			shared::common::log("Resolution", std::format("Reset: BackBuffer {}x{} -> {}x{}",
				pPresentationParameters->BackBufferWidth, pPresentationParameters->BackBufferHeight,
				res_cfg.present_width, res_cfg.present_height));
			pPresentationParameters->BackBufferWidth = res_cfg.present_width;
			pPresentationParameters->BackBufferHeight = res_cfg.present_height;
		}
		const auto hr = m_pIDirect3DDevice9->Reset(pPresentationParameters);
		tex_addons::init_texture_addons();
		ImGui_ImplDX9_CreateDeviceObjects();
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::Present(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion)
	{
		TRACE_IF_ACTIVE(trace_Present, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
		auto& ffp = shared::common::ffp_state::get();
		auto& cfg = shared::common::config::get();
		cfg.postfx.pass_count = s_postfx_pass_idx;
		s_postfx_pass_idx = 0;
		s_composite_requested = false;
		if (auto* d = diagnostics::get()) d->on_present(ffp.frame_count(), ffp.draw_call_count(), ffp.scene_count());
		crash_context::on_present(ffp.draw_call_count());
		rt_tracker::on_present();
		perf::on_present();
		shared::common::ffp_prof::on_present();
		game::render_toggles::on_present();
		ffp.on_present();
		game::fps_unlock::on_present();
		auto hr = m_pIDirect3DDevice9->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
		benchmark::on_present();
		if (auto* t = tracer::get()) t->on_present();
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer)
	{
		TRACE_IF_ACTIVE(trace_GetBackBuffer, iSwapChain, iBackBuffer, Type, ppBackBuffer);
		return m_pIDirect3DDevice9->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer);
	}

	HRESULT d3d9ex::D3D9Device::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus)
	{
		TRACE_IF_ACTIVE(trace_GetRasterStatus, iSwapChain, pRasterStatus);
		return m_pIDirect3DDevice9->GetRasterStatus(iSwapChain, pRasterStatus);
	}

	HRESULT d3d9ex::D3D9Device::SetDialogBoxMode(BOOL bEnableDialogs)
	{
		TRACE_IF_ACTIVE(trace_SetDialogBoxMode, bEnableDialogs);
		return m_pIDirect3DDevice9->SetDialogBoxMode(bEnableDialogs);
	}

	void d3d9ex::D3D9Device::SetGammaRamp(UINT iSwapChain, DWORD Flags, CONST D3DGAMMARAMP* pRamp)
	{
		TRACE_IF_ACTIVE(trace_SetGammaRamp, iSwapChain, Flags, pRamp);
		return m_pIDirect3DDevice9->SetGammaRamp(iSwapChain, Flags, pRamp);
	}

	void d3d9ex::D3D9Device::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp)
	{
		TRACE_IF_ACTIVE(trace_GetGammaRamp, iSwapChain, pRamp);
		return m_pIDirect3DDevice9->GetGammaRamp(iSwapChain, pRamp);
	}

	HRESULT d3d9ex::D3D9Device::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle)
	{
		TRACE_IF_ACTIVE(trace_CreateTexture, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
		const auto& res = shared::common::config::get().resolution;
		if (res.render_width > 0 && res.render_height > 0)
		{
			if (Width == 1024 && Height == 720)
			{
				shared::common::log("Resolution", std::format("CreateTexture override 1024x720 -> {}x{} (render)", res.render_width, res.render_height));
				Width = res.render_width;
				Height = res.render_height;
			}
			else if (Width == 1280 && Height == 720)
			{
				shared::common::log("Resolution", std::format("CreateTexture override 1280x720 -> {}x{} (present)", res.present_width, res.present_height));
				Width = res.present_width;
				Height = res.present_height;
			}
		}
		const crash_context::stopwatch sw;
		const auto hr = m_pIDirect3DDevice9->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
		if (FAILED(hr))
			crash_context::note("Tex2D %ux%u L%u f=%s p=%u t=%u hr=%08X", Width, Height, Levels, format_tag(Format).c_str(), Pool, sw.ms(), hr);
		else if (ppTexture && *ppTexture)
			game::texture_names::on_device_created(*ppTexture);  // D3DX names its textures after this returns
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle)
	{
		TRACE_IF_ACTIVE(trace_CreateVolumeTexture, Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle);
		const crash_context::stopwatch sw;
		const auto hr = m_pIDirect3DDevice9->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle);
		if (FAILED(hr))
			crash_context::note("VolTex %ux%ux%u f=%s t=%u hr=%08X", Width, Height, Depth, format_tag(Format).c_str(), sw.ms(), hr);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle)
	{
		TRACE_IF_ACTIVE(trace_CreateCubeTexture, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
		const crash_context::stopwatch sw;
		const auto hr = m_pIDirect3DDevice9->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
		if (FAILED(hr))
			crash_context::note("CubeTex e=%u L%u f=%s p=%u t=%u hr=%08X", EdgeLength, Levels, format_tag(Format).c_str(), Pool, sw.ms(), hr);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle)
	{
		TRACE_IF_ACTIVE(trace_CreateVertexBuffer, Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
		const crash_context::stopwatch sw;
		const auto hr = m_pIDirect3DDevice9->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
		if (FAILED(hr))
			crash_context::note("VB len=%u u=%X p=%u t=%u hr=%08X", Length, Usage, Pool, sw.ms(), hr);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle)
	{
		TRACE_IF_ACTIVE(trace_CreateIndexBuffer, Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
		const crash_context::stopwatch sw;
		const auto hr = m_pIDirect3DDevice9->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
		if (FAILED(hr))
			crash_context::note("IB len=%u u=%X p=%u t=%u hr=%08X", Length, Usage, Pool, sw.ms(), hr);
		else if (ppIndexBuffer && *ppIndexBuffer)
			game::sky_meshes::on_index_buffer_created(*ppIndexBuffer);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
	{
		TRACE_IF_ACTIVE(trace_CreateRenderTarget, Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
		const auto& res = shared::common::config::get().resolution;
		if (res.render_width > 0 && res.render_height > 0 && Width == 1024 && Height == 720)
		{
			shared::common::log("Resolution", std::format("CreateRenderTarget override 1024x720 -> {}x{}", res.render_width, res.render_height));
			Width = res.render_width;
			Height = res.render_height;
		}
		const crash_context::stopwatch sw;
		const auto hr = m_pIDirect3DDevice9->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
		if (FAILED(hr))
			crash_context::note("RT %ux%u f=%s t=%u hr=%08X", Width, Height, format_tag(Format).c_str(), sw.ms(), hr);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
	{
		TRACE_IF_ACTIVE(trace_CreateDepthStencilSurface, Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle);
		const auto& res = shared::common::config::get().resolution;
		if (res.render_width > 0 && res.render_height > 0 && Width == 1024 && Height == 720)
		{
			shared::common::log("Resolution", std::format("CreateDepthStencilSurface override 1024x720 -> {}x{}", res.render_width, res.render_height));
			Width = res.render_width;
			Height = res.render_height;
		}
		const crash_context::stopwatch sw;
		const auto hr = m_pIDirect3DDevice9->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle);
		if (FAILED(hr))
			crash_context::note("DS %ux%u f=%s t=%u hr=%08X", Width, Height, format_tag(Format).c_str(), sw.ms(), hr);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::UpdateSurface(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestinationSurface, CONST POINT* pDestPoint)
	{
		TRACE_IF_ACTIVE(trace_UpdateSurface, pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint);
		return m_pIDirect3DDevice9->UpdateSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint);
	}

	HRESULT d3d9ex::D3D9Device::UpdateTexture(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture)
	{
		TRACE_IF_ACTIVE(trace_UpdateTexture, pSourceTexture, pDestinationTexture);
		return m_pIDirect3DDevice9->UpdateTexture(pSourceTexture, pDestinationTexture);
	}

	HRESULT d3d9ex::D3D9Device::GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface)
	{
		TRACE_IF_ACTIVE(trace_GetRenderTargetData, pRenderTarget, pDestSurface);
		return m_pIDirect3DDevice9->GetRenderTargetData(pRenderTarget, pDestSurface);
	}

	HRESULT d3d9ex::D3D9Device::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface)
	{
		TRACE_IF_ACTIVE(trace_GetFrontBufferData, iSwapChain, pDestSurface);
		return m_pIDirect3DDevice9->GetFrontBufferData(iSwapChain, pDestSurface);
	}

	HRESULT d3d9ex::D3D9Device::StretchRect(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter)
	{
		TRACE_IF_ACTIVE(trace_StretchRect, pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);
		return m_pIDirect3DDevice9->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);
	}

	HRESULT d3d9ex::D3D9Device::ColorFill(IDirect3DSurface9* pSurface, CONST RECT* pRect, D3DCOLOR color)
	{
		TRACE_IF_ACTIVE(trace_ColorFill, pSurface, pRect, color);
		return m_pIDirect3DDevice9->ColorFill(pSurface, pRect, color);
	}

	HRESULT d3d9ex::D3D9Device::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
	{
		TRACE_IF_ACTIVE(trace_CreateOffscreenPlainSurface, Width, Height, Format, Pool, ppSurface, pSharedHandle);
		const crash_context::stopwatch sw;
		const auto hr = m_pIDirect3DDevice9->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle);
		if (FAILED(hr))
			crash_context::note("Offscr %ux%u f=%s p=%u t=%u hr=%08X", Width, Height, format_tag(Format).c_str(), Pool, sw.ms(), hr);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget)
	{
		TRACE_IF_ACTIVE(trace_SetRenderTarget, RenderTargetIndex, pRenderTarget);
		shared::common::ffp_state::get().on_set_render_target(RenderTargetIndex, pRenderTarget);
		if (lookdev_passthrough())
			return m_pIDirect3DDevice9->SetRenderTarget(RenderTargetIndex, pRenderTarget);
		auto* effective = rt_tracker::on_set_render_target(RenderTargetIndex, pRenderTarget);
		return m_pIDirect3DDevice9->SetRenderTarget(RenderTargetIndex, effective);
	}

	HRESULT d3d9ex::D3D9Device::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget)
	{
		TRACE_IF_ACTIVE(trace_GetRenderTarget, RenderTargetIndex, ppRenderTarget);
		return m_pIDirect3DDevice9->GetRenderTarget(RenderTargetIndex, ppRenderTarget);
	}

	HRESULT d3d9ex::D3D9Device::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil)
	{
		TRACE_IF_ACTIVE(trace_SetDepthStencilSurface, pNewZStencil);
		return m_pIDirect3DDevice9->SetDepthStencilSurface(pNewZStencil);
	}

	HRESULT d3d9ex::D3D9Device::GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface)
	{
		TRACE_IF_ACTIVE(trace_GetDepthStencilSurface, ppZStencilSurface);
		return m_pIDirect3DDevice9->GetDepthStencilSurface(ppZStencilSurface);
	}

	HRESULT d3d9ex::D3D9Device::BeginScene()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_BeginScene);
		auto& ffp = shared::common::ffp_state::get();

		ffp.on_begin_scene();
		if (auto* d = diagnostics::get()) d->on_begin_scene(ffp.scene_count());

		if (renderer::is_initialized()) {
			on_begin_scene_cb();
		}

		return m_pIDirect3DDevice9->BeginScene();
	}

	HRESULT d3d9ex::D3D9Device::EndScene()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_EndScene);
		if (imgui::is_initialized()) {
			imgui::get()->on_present();
		}

		return m_pIDirect3DDevice9->EndScene();
	}

	HRESULT d3d9ex::D3D9Device::Clear(DWORD Count, CONST D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
	{
		TRACE_IF_ACTIVE(trace_Clear, Count, pRects, Flags, Color, Z, Stencil);
		shared::common::ffp_state::get().on_clear(Flags);
		return m_pIDirect3DDevice9->Clear(Count, pRects, Flags, Color, Z, Stencil);
	}

	HRESULT d3d9ex::D3D9Device::SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix)
	{
		TRACE_IF_ACTIVE(trace_SetTransform, State, pMatrix);
		return m_pIDirect3DDevice9->SetTransform(State, pMatrix);
	}

	HRESULT d3d9ex::D3D9Device::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix)
	{
		TRACE_IF_ACTIVE(trace_GetTransform, State, pMatrix);
		return m_pIDirect3DDevice9->GetTransform(State, pMatrix);
	}

	HRESULT d3d9ex::D3D9Device::MultiplyTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix)
	{
		TRACE_IF_ACTIVE(trace_MultiplyTransform, State, pMatrix);
		return m_pIDirect3DDevice9->MultiplyTransform(State, pMatrix);
	}

	HRESULT d3d9ex::D3D9Device::SetViewport(CONST D3DVIEWPORT9* pViewport)
	{
		TRACE_IF_ACTIVE(trace_SetViewport, pViewport);
		return m_pIDirect3DDevice9->SetViewport(pViewport);
	}

	HRESULT d3d9ex::D3D9Device::GetViewport(D3DVIEWPORT9* pViewport)
	{
		TRACE_IF_ACTIVE(trace_GetViewport, pViewport);
		return m_pIDirect3DDevice9->GetViewport(pViewport);
	}

	HRESULT d3d9ex::D3D9Device::SetMaterial(CONST D3DMATERIAL9* pMaterial)
	{
		TRACE_IF_ACTIVE(trace_SetMaterial, pMaterial);
		return m_pIDirect3DDevice9->SetMaterial(pMaterial);
	}

	HRESULT d3d9ex::D3D9Device::GetMaterial(D3DMATERIAL9* pMaterial)
	{
		TRACE_IF_ACTIVE(trace_GetMaterial, pMaterial);
		return m_pIDirect3DDevice9->GetMaterial(pMaterial);
	}

	HRESULT d3d9ex::D3D9Device::SetLight(DWORD Index, CONST D3DLIGHT9* pLight)
	{
		TRACE_IF_ACTIVE(trace_SetLight, Index, pLight);
		return m_pIDirect3DDevice9->SetLight(Index, pLight);
	}

	HRESULT d3d9ex::D3D9Device::GetLight(DWORD Index, D3DLIGHT9* pLight)
	{
		TRACE_IF_ACTIVE(trace_GetLight, Index, pLight);
		return m_pIDirect3DDevice9->GetLight(Index, pLight);
	}

	HRESULT d3d9ex::D3D9Device::LightEnable(DWORD Index, BOOL Enable)
	{
		TRACE_IF_ACTIVE(trace_LightEnable, Index, Enable);
		return m_pIDirect3DDevice9->LightEnable(Index, Enable);
	}

	HRESULT d3d9ex::D3D9Device::GetLightEnable(DWORD Index, BOOL* pEnable)
	{
		TRACE_IF_ACTIVE(trace_GetLightEnable, Index, pEnable);
		return m_pIDirect3DDevice9->GetLightEnable(Index, pEnable);
	}

	HRESULT d3d9ex::D3D9Device::SetClipPlane(DWORD Index, CONST float* pPlane)
	{
		TRACE_IF_ACTIVE(trace_SetClipPlane, Index, pPlane);
		return m_pIDirect3DDevice9->SetClipPlane(Index, pPlane);
	}

	HRESULT d3d9ex::D3D9Device::GetClipPlane(DWORD Index, float* pPlane)
	{
		TRACE_IF_ACTIVE(trace_GetClipPlane, Index, pPlane);
		return m_pIDirect3DDevice9->GetClipPlane(Index, pPlane);
	}

	HRESULT d3d9ex::D3D9Device::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
	{
		TRACE_IF_ACTIVE(trace_SetRenderState, State, Value);
		shared::common::ffp_state::get().on_set_render_state(State, Value);
		return m_pIDirect3DDevice9->SetRenderState(State, Value);
	}

	HRESULT d3d9ex::D3D9Device::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue)
	{
		TRACE_IF_ACTIVE(trace_GetRenderState, State, pValue);
		return m_pIDirect3DDevice9->GetRenderState(State, pValue);
	}

	HRESULT d3d9ex::D3D9Device::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB)
	{
		TRACE_IF_ACTIVE(trace_CreateStateBlock, Type, ppSB);
		return m_pIDirect3DDevice9->CreateStateBlock(Type, ppSB);
	}

	HRESULT d3d9ex::D3D9Device::BeginStateBlock()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_BeginStateBlock);
		return m_pIDirect3DDevice9->BeginStateBlock();
	}

	HRESULT d3d9ex::D3D9Device::EndStateBlock(IDirect3DStateBlock9** ppSB)
	{
		TRACE_IF_ACTIVE(trace_EndStateBlock, ppSB);
		return m_pIDirect3DDevice9->EndStateBlock(ppSB);
	}

	HRESULT d3d9ex::D3D9Device::SetClipStatus(CONST D3DCLIPSTATUS9* pClipStatus)
	{
		TRACE_IF_ACTIVE(trace_SetClipStatus, pClipStatus);
		return m_pIDirect3DDevice9->SetClipStatus(pClipStatus);
	}

	HRESULT d3d9ex::D3D9Device::GetClipStatus(D3DCLIPSTATUS9* pClipStatus)
	{
		TRACE_IF_ACTIVE(trace_GetClipStatus, pClipStatus);
		return m_pIDirect3DDevice9->GetClipStatus(pClipStatus);
	}

	HRESULT d3d9ex::D3D9Device::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture)
	{
		TRACE_IF_ACTIVE(trace_GetTexture, Stage, ppTexture);
		return m_pIDirect3DDevice9->GetTexture(Stage, ppTexture);
	}

	HRESULT d3d9ex::D3D9Device::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture)
	{
		TRACE_IF_ACTIVE(trace_SetTexture, Stage, pTexture);
		shared::common::ffp_state::get().on_set_texture(Stage, pTexture);
		return m_pIDirect3DDevice9->SetTexture(Stage, pTexture);
	}

	HRESULT d3d9ex::D3D9Device::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue)
	{
		TRACE_IF_ACTIVE(trace_GetTextureStageState, Stage, Type, pValue);
		return m_pIDirect3DDevice9->GetTextureStageState(Stage, Type, pValue);
	}

	HRESULT d3d9ex::D3D9Device::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
	{
		TRACE_IF_ACTIVE(trace_SetTextureStageState, Stage, Type, Value);
		// Only the GAME reaches this wrapper — the proxy's own conversion calls go
		// straight to m_pIDirect3DDevice9 — so this is a reliable signal that the
		// FFP stage configuration has been disturbed ([FFPSteps] TextureStages=1).
		shared::common::ffp_state::get().on_set_texture_stage_state(Stage, Type, Value);
		return m_pIDirect3DDevice9->SetTextureStageState(Stage, Type, Value);
	}

	HRESULT d3d9ex::D3D9Device::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue)
	{
		TRACE_IF_ACTIVE(trace_GetSamplerState, Sampler, Type, pValue);
		return m_pIDirect3DDevice9->GetSamplerState(Sampler, Type, pValue);
	}

	HRESULT d3d9ex::D3D9Device::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value)
	{
		TRACE_IF_ACTIVE(trace_SetSamplerState, Sampler, Type, Value);
		return m_pIDirect3DDevice9->SetSamplerState(Sampler, Type, Value);
	}

	HRESULT d3d9ex::D3D9Device::ValidateDevice(DWORD* pNumPasses)
	{
		TRACE_IF_ACTIVE(trace_ValidateDevice, pNumPasses);
		return m_pIDirect3DDevice9->ValidateDevice(pNumPasses);
	}

	HRESULT d3d9ex::D3D9Device::SetPaletteEntries(UINT PaletteNumber, CONST PALETTEENTRY* pEntries)
	{
		TRACE_IF_ACTIVE(trace_SetPaletteEntries, PaletteNumber, pEntries);
		return m_pIDirect3DDevice9->SetPaletteEntries(PaletteNumber, pEntries);
	}

	HRESULT d3d9ex::D3D9Device::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries)
	{
		TRACE_IF_ACTIVE(trace_GetPaletteEntries, PaletteNumber, pEntries);
		return m_pIDirect3DDevice9->GetPaletteEntries(PaletteNumber, pEntries);
	}

	HRESULT d3d9ex::D3D9Device::SetCurrentTexturePalette(UINT PaletteNumber)
	{
		TRACE_IF_ACTIVE(trace_SetCurrentTexturePalette, PaletteNumber);
		return m_pIDirect3DDevice9->SetCurrentTexturePalette(PaletteNumber);
	}

	HRESULT d3d9ex::D3D9Device::GetCurrentTexturePalette(UINT *PaletteNumber)
	{
		TRACE_IF_ACTIVE(trace_GetCurrentTexturePalette, PaletteNumber);
		return m_pIDirect3DDevice9->GetCurrentTexturePalette(PaletteNumber);
	}

	HRESULT d3d9ex::D3D9Device::SetScissorRect(CONST RECT* pRect)
	{
		TRACE_IF_ACTIVE(trace_SetScissorRect, pRect);
		return m_pIDirect3DDevice9->SetScissorRect(pRect);
	}

	HRESULT d3d9ex::D3D9Device::GetScissorRect(RECT* pRect)
	{
		TRACE_IF_ACTIVE(trace_GetScissorRect, pRect);
		return m_pIDirect3DDevice9->GetScissorRect(pRect);
	}

	HRESULT d3d9ex::D3D9Device::SetSoftwareVertexProcessing(BOOL bSoftware)
	{
		TRACE_IF_ACTIVE(trace_SetSoftwareVertexProcessing, bSoftware);
		return m_pIDirect3DDevice9->SetSoftwareVertexProcessing(bSoftware);
	}

	BOOL d3d9ex::D3D9Device::GetSoftwareVertexProcessing()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_GetSoftwareVertexProcessing);
		return m_pIDirect3DDevice9->GetSoftwareVertexProcessing();
	}

	HRESULT d3d9ex::D3D9Device::SetNPatchMode(float nSegments)
	{
		TRACE_IF_ACTIVE(trace_SetNPatchMode, nSegments);
		return m_pIDirect3DDevice9->SetNPatchMode(nSegments);
	}

	float d3d9ex::D3D9Device::GetNPatchMode()
	{
		TRACE_IF_ACTIVE_NOARGS(trace_GetNPatchMode);
		return m_pIDirect3DDevice9->GetNPatchMode();
	}

	HRESULT d3d9ex::D3D9Device::DrawPrimitive([[maybe_unused]] D3DPRIMITIVETYPE PrimitiveType, [[maybe_unused]] UINT StartVertex, [[maybe_unused]] UINT PrimitiveCount)
	{
		TRACE_IF_ACTIVE(trace_DrawPrimitive, PrimitiveType, StartVertex, PrimitiveCount);
		if (lookdev_passthrough())
			return m_pIDirect3DDevice9->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
		if (const auto sfx = current_sfx(); sfx != shared::common::sfx_class::None)
		{
			// Particles skip the renderer, where draws are tagged: no lightmap of their own.
			comp::game::lightmap_sun::untag(m_pIDirect3DDevice9);
			return sfx_draw(m_pIDirect3DDevice9, sfx, [&] { return m_pIDirect3DDevice9->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount); });
		}
		UINT num_verts = PrimitiveCount * 3;
		if (PrimitiveType == D3DPT_TRIANGLESTRIP || PrimitiveType == D3DPT_TRIANGLEFAN)
			num_verts = PrimitiveCount + 2;
		const auto insp = perf::inspector_on_draw("DP", PrimitiveType, PrimitiveCount, num_verts,
			static_cast<INT>(StartVertex), 0, shared::common::ffp_state::get().stream_stride(0));
		if (insp.drop)
			return S_OK;
		inspector_mark_scope mark;
		if (insp.mark) mark.apply(this);
		const auto hr = renderer::get()->on_draw_primitive(m_pIDirect3DDevice9, PrimitiveType, StartVertex, PrimitiveCount);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount)
	{
		TRACE_IF_ACTIVE(trace_DrawIndexedPrimitive, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
		if (lookdev_passthrough())
			return m_pIDirect3DDevice9->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
		if (const auto sfx = current_sfx(); sfx != shared::common::sfx_class::None)
		{
			// Particles skip the renderer, where draws are tagged: no lightmap of their own.
			comp::game::lightmap_sun::untag(m_pIDirect3DDevice9);
			return sfx_draw(m_pIDirect3DDevice9, sfx, [&] { return m_pIDirect3DDevice9->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount); });
		}
		const auto insp = perf::inspector_on_draw("DIP", PrimitiveType, primCount, NumVertices,
			BaseVertexIndex, startIndex, shared::common::ffp_state::get().stream_stride(0));
		if (insp.drop)
			return S_OK;
		inspector_mark_scope mark;
		if (insp.mark) mark.apply(this);
		const auto hr = renderer::get()->on_draw_indexed_prim(m_pIDirect3DDevice9, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
	{
		TRACE_IF_ACTIVE(trace_DrawPrimitiveUP, PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
		if (lookdev_passthrough())
			return m_pIDirect3DDevice9->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
		// UP draws (sprites, UI, post passes) carry no haze set or lightmap of their own.
		comp::game::atmosphere::tag_haze_set(m_pIDirect3DDevice9, false, nullptr, nullptr, 0);
		comp::game::lightmap_sun::untag(m_pIDirect3DDevice9);
		if (auto& ffp = shared::common::ffp_state::get(); ffp.cur_ps_light_flags() & ffp.PS_GRADE)
			comp::game::atmosphere::observe_grade(ffp.ps_const_data(), m_pIDirect3DDevice9);
		if (const auto sfx = current_sfx(); sfx != shared::common::sfx_class::None)
		{
			HRESULT hr = S_OK;
			if (sfx_sprite_up(m_pIDirect3DDevice9, sfx, PrimitiveType, PrimitiveCount, 0, 0, nullptr, D3DFMT_UNKNOWN,
				pVertexStreamZeroData, VertexStreamZeroStride, hr))
				return hr;
			return sfx_draw(m_pIDirect3DDevice9, sfx, [&] { return m_pIDirect3DDevice9->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride); });
		}
		{
			UINT up_verts = PrimitiveCount * 3;
			if (PrimitiveType == D3DPT_TRIANGLESTRIP || PrimitiveType == D3DPT_TRIANGLEFAN)
				up_verts = PrimitiveCount + 2;
			const auto insp = perf::inspector_on_draw("DPUP", PrimitiveType, PrimitiveCount, up_verts,
				0, 0, VertexStreamZeroStride);
			if (insp.drop)
				return S_OK;
			if (insp.mark)
			{
				inspector_mark_scope mark;
				mark.apply(this);
				return m_pIDirect3DDevice9->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
			}
		}
		// Fullscreen quad post-FX pass detection (stride=24 = pos+uv, 2 tris = single quad)
		if (PrimitiveCount <= 2 && VertexStreamZeroStride == 24)
		{
			auto& c = shared::common::config::get();
			int idx = s_postfx_pass_idx++;
			if (c.ffp.skip_postfx && (c.postfx.skip_passes.empty() || c.postfx.skip_passes.contains(idx)))
				return S_OK;
		}
		composite_before_backbuffer(m_pIDirect3DDevice9, VertexStreamZeroStride);
		const auto hr = m_pIDirect3DDevice9->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::DrawIndexedPrimitiveUP(
		[[maybe_unused]] D3DPRIMITIVETYPE PrimitiveType, 
		[[maybe_unused]] UINT MinVertexIndex, 
		[[maybe_unused]] UINT NumVertices, 
		[[maybe_unused]] UINT PrimitiveCount, 
		[[maybe_unused]] CONST void* pIndexData, 
		[[maybe_unused]] D3DFORMAT IndexDataFormat,
		[[maybe_unused]] CONST void* pVertexStreamZeroData,
		[[maybe_unused]] UINT VertexStreamZeroStride)
	{
		TRACE_IF_ACTIVE(trace_DrawIndexedPrimitiveUP, PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
		if (lookdev_passthrough())
			return m_pIDirect3DDevice9->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
		comp::game::atmosphere::tag_haze_set(m_pIDirect3DDevice9, false, nullptr, nullptr, 0);
		comp::game::lightmap_sun::untag(m_pIDirect3DDevice9);
		if (const auto sfx = current_sfx(); sfx != shared::common::sfx_class::None)
		{
			HRESULT hr = S_OK;
			if (sfx_sprite_up(m_pIDirect3DDevice9, sfx, PrimitiveType, PrimitiveCount, MinVertexIndex, NumVertices, pIndexData, IndexDataFormat,
				pVertexStreamZeroData, VertexStreamZeroStride, hr))
				return hr;
			return sfx_draw(m_pIDirect3DDevice9, sfx, [&] { return m_pIDirect3DDevice9->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride); });
		}
		{
			const auto insp = perf::inspector_on_draw("DIPUP", PrimitiveType, PrimitiveCount, NumVertices,
				0, 0, VertexStreamZeroStride);
			if (insp.drop)
				return S_OK;
			if (insp.mark)
			{
				inspector_mark_scope mark;
				mark.apply(this);
				// still classify/count as a HUD quad so the category shows up
				perf::filter_hud_quad(PrimitiveType, NumVertices, PrimitiveCount, IndexDataFormat, VertexStreamZeroStride);
				return m_pIDirect3DDevice9->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
			}
		}
		// HUD/menu quads (watertight P0 predicate — see findings.md 2026-07-26 §5).
		// The HUD path owns these draws entirely: they never reach the post-FX
		// quad skipper below (which used to swallow the 4-vertex fan quads when
		// SkipPostFX was on). When rendered (default), the draw carries the UI
		// marker in stage-0 D3DTSS_CONSTANT so the custom Remix runtime
		// (rtx.uiMarkerEnabled) classifies it as UI structurally — no
		// rtx.uiTextures hash lists required.
		{
			const auto hud = perf::filter_hud_quad(PrimitiveType, NumVertices, PrimitiveCount, IndexDataFormat, VertexStreamZeroStride);
			if (hud.drop)
				return S_OK;
			if (hud.is_hud)
			{
				const bool mark_ui = shared::common::config::get().hud.mark_ui_draws;
				// The stage-0 constant is only ever written by the proxy (decal
				// marker on FFP world draws), so restoring the default is exact.
				constexpr DWORD kUiMarker = 0xD15D0011;
				if (mark_ui)
					m_pIDirect3DDevice9->SetTextureStageState(0, D3DTSS_CONSTANT, kUiMarker);
				const auto hr = m_pIDirect3DDevice9->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
				if (mark_ui)
					m_pIDirect3DDevice9->SetTextureStageState(0, D3DTSS_CONSTANT, 0);
				return hr;
			}
		}
		// Fullscreen quad post-FX pass detection (stride=12 = pos-only, 2 tris + 4 verts = indexed quad)
		if (PrimitiveCount <= 2 && NumVertices <= 4 && VertexStreamZeroStride == 12)
		{
			auto& c = shared::common::config::get();
			int idx = s_postfx_pass_idx++;
			if (c.ffp.skip_postfx && (c.postfx.skip_passes.empty() || c.postfx.skip_passes.contains(idx)))
				return S_OK;
		}
		return m_pIDirect3DDevice9->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
	}

	HRESULT d3d9ex::D3D9Device::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags)
	{
		TRACE_IF_ACTIVE(trace_ProcessVertices, SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags);
		return m_pIDirect3DDevice9->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags);
	}

	HRESULT d3d9ex::D3D9Device::CreateVertexDeclaration(CONST D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl)
	{
		TRACE_IF_ACTIVE(trace_CreateVertexDeclaration, pVertexElements, ppDecl);
		auto hr = m_pIDirect3DDevice9->CreateVertexDeclaration(pVertexElements, ppDecl);
		if (SUCCEEDED(hr) && ppDecl && *ppDecl)
			if (auto* t = tracer::get(); t && t->is_capturing()) t->record_created_handle(*ppDecl);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl)
	{
		TRACE_IF_ACTIVE(trace_SetVertexDeclaration, pDecl);
		shared::common::ffp_state::get().on_set_vertex_declaration(pDecl);
		return m_pIDirect3DDevice9->SetVertexDeclaration(pDecl);
	}

	HRESULT d3d9ex::D3D9Device::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl)
	{
		TRACE_IF_ACTIVE(trace_GetVertexDeclaration, ppDecl);
		return m_pIDirect3DDevice9->GetVertexDeclaration(ppDecl);
	}

	HRESULT d3d9ex::D3D9Device::SetFVF(DWORD FVF)
	{
		TRACE_IF_ACTIVE(trace_SetFVF, FVF);
		return m_pIDirect3DDevice9->SetFVF(FVF);
	}

	HRESULT d3d9ex::D3D9Device::GetFVF(DWORD* pFVF)
	{
		TRACE_IF_ACTIVE(trace_GetFVF, pFVF);
		return m_pIDirect3DDevice9->GetFVF(pFVF);
	}

	HRESULT d3d9ex::D3D9Device::CreateVertexShader(CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader)
	{
		TRACE_IF_ACTIVE(trace_CreateVertexShader, pFunction, ppShader);
		auto hr = m_pIDirect3DDevice9->CreateVertexShader(pFunction, ppShader);
		if (SUCCEEDED(hr) && ppShader && *ppShader)
		{
			shared::common::ffp_state::get().on_create_vertex_shader(*ppShader, pFunction);
			if (auto* t = tracer::get(); t && t->is_capturing()) t->record_created_handle(*ppShader);
		}
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::SetVertexShader(IDirect3DVertexShader9* pShader)
	{
		TRACE_IF_ACTIVE(trace_SetVertexShader, pShader);
		shared::common::ffp_state::get().on_set_vertex_shader(pShader);
		if (auto* d = diagnostics::get()) d->on_set_vertex_shader(pShader);
		return m_pIDirect3DDevice9->SetVertexShader(pShader);
	}

	HRESULT d3d9ex::D3D9Device::GetVertexShader(IDirect3DVertexShader9** ppShader)
	{
		TRACE_IF_ACTIVE(trace_GetVertexShader, ppShader);
		return m_pIDirect3DDevice9->GetVertexShader(ppShader);
	}

	HRESULT d3d9ex::D3D9Device::SetVertexShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount)
	{
		TRACE_IF_ACTIVE(trace_SetVertexShaderConstantF, StartRegister, pConstantData, Vector4fCount);
		auto& ffp = shared::common::ffp_state::get();
		ffp.on_set_vs_const_f(StartRegister, pConstantData, Vector4fCount);
		if (auto* d = diagnostics::get()) d->on_set_vs_const_f(StartRegister, pConstantData, Vector4fCount);
		return m_pIDirect3DDevice9->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}

	HRESULT d3d9ex::D3D9Device::GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount)
	{
		TRACE_IF_ACTIVE(trace_GetVertexShaderConstantF, StartRegister, pConstantData, Vector4fCount);
		return m_pIDirect3DDevice9->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}

	HRESULT d3d9ex::D3D9Device::SetVertexShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount)
	{
		TRACE_IF_ACTIVE(trace_SetVertexShaderConstantI, StartRegister, pConstantData, Vector4iCount);
		return m_pIDirect3DDevice9->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}

	HRESULT d3d9ex::D3D9Device::GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount)
	{
		TRACE_IF_ACTIVE(trace_GetVertexShaderConstantI, StartRegister, pConstantData, Vector4iCount);
		return m_pIDirect3DDevice9->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}

	HRESULT d3d9ex::D3D9Device::SetVertexShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT  BoolCount)
	{
		TRACE_IF_ACTIVE(trace_SetVertexShaderConstantB, StartRegister, pConstantData, BoolCount);
		return m_pIDirect3DDevice9->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
	}

	HRESULT d3d9ex::D3D9Device::GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount)
	{
		TRACE_IF_ACTIVE(trace_GetVertexShaderConstantB, StartRegister, pConstantData, BoolCount);
		return m_pIDirect3DDevice9->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
	}

	HRESULT d3d9ex::D3D9Device::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride)
	{
		TRACE_IF_ACTIVE(trace_SetStreamSource, StreamNumber, pStreamData, OffsetInBytes, Stride);
		shared::common::ffp_state::get().on_set_stream_source(StreamNumber, pStreamData, OffsetInBytes, Stride);
		return m_pIDirect3DDevice9->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride);
	}

	HRESULT d3d9ex::D3D9Device::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* OffsetInBytes, UINT* pStride)
	{
		TRACE_IF_ACTIVE(trace_GetStreamSource, StreamNumber, ppStreamData, OffsetInBytes, pStride);
		return m_pIDirect3DDevice9->GetStreamSource(StreamNumber, ppStreamData, OffsetInBytes, pStride);
	}

	HRESULT d3d9ex::D3D9Device::SetStreamSourceFreq(UINT StreamNumber, UINT Divider)
	{
		TRACE_IF_ACTIVE(trace_SetStreamSourceFreq, StreamNumber, Divider);
		return m_pIDirect3DDevice9->SetStreamSourceFreq(StreamNumber, Divider);
	}

	HRESULT d3d9ex::D3D9Device::GetStreamSourceFreq(UINT StreamNumber, UINT* Divider)
	{
		TRACE_IF_ACTIVE(trace_GetStreamSourceFreq, StreamNumber, Divider);
		return m_pIDirect3DDevice9->GetStreamSourceFreq(StreamNumber, Divider);
	}

	HRESULT d3d9ex::D3D9Device::SetIndices(IDirect3DIndexBuffer9* pIndexData)
	{
		TRACE_IF_ACTIVE(trace_SetIndices, pIndexData);
		shared::common::ffp_state::get().on_set_indices(pIndexData);
		return m_pIDirect3DDevice9->SetIndices(pIndexData);
	}

	HRESULT d3d9ex::D3D9Device::GetIndices(IDirect3DIndexBuffer9** ppIndexData)
	{
		TRACE_IF_ACTIVE(trace_GetIndices, ppIndexData);
		return m_pIDirect3DDevice9->GetIndices(ppIndexData);
	}

	HRESULT d3d9ex::D3D9Device::CreatePixelShader(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader)
	{
		TRACE_IF_ACTIVE(trace_CreatePixelShader, pFunction, ppShader);
		auto hr = m_pIDirect3DDevice9->CreatePixelShader(pFunction, ppShader);
		if (SUCCEEDED(hr) && ppShader && *ppShader)
		{
			shared::common::ffp_state::get().on_create_pixel_shader(*ppShader, pFunction);
			if (auto* t = tracer::get(); t && t->is_capturing()) t->record_created_handle(*ppShader);
		}
		return hr;
	}

	// Swallowed during FFP so Remix sees fixed-function draws.
	// GetPixelShader will return the previous shader, not what the game set.
	HRESULT d3d9ex::D3D9Device::SetPixelShader(IDirect3DPixelShader9* pShader)
	{
		TRACE_IF_ACTIVE(trace_SetPixelShader, pShader);
		auto& ffp = shared::common::ffp_state::get();
		ffp.on_set_pixel_shader(pShader);
		if (ffp.is_ffp_active())
			return S_OK;
		return m_pIDirect3DDevice9->SetPixelShader(pShader);
	}

	HRESULT d3d9ex::D3D9Device::GetPixelShader(IDirect3DPixelShader9** ppShader)
	{
		TRACE_IF_ACTIVE(trace_GetPixelShader, ppShader);
		return m_pIDirect3DDevice9->GetPixelShader(ppShader);
	}

	HRESULT d3d9ex::D3D9Device::SetPixelShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount)
	{
		TRACE_IF_ACTIVE(trace_SetPixelShaderConstantF, StartRegister, pConstantData, Vector4fCount);
		shared::common::ffp_state::get().on_set_ps_const_f(StartRegister, pConstantData, Vector4fCount);
		return m_pIDirect3DDevice9->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}

	HRESULT d3d9ex::D3D9Device::GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount)
	{
		TRACE_IF_ACTIVE(trace_GetPixelShaderConstantF, StartRegister, pConstantData, Vector4fCount);
		return m_pIDirect3DDevice9->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}

	HRESULT d3d9ex::D3D9Device::SetPixelShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount)
	{
		TRACE_IF_ACTIVE(trace_SetPixelShaderConstantI, StartRegister, pConstantData, Vector4iCount);
		return m_pIDirect3DDevice9->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}

	HRESULT d3d9ex::D3D9Device::GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount)
	{
		TRACE_IF_ACTIVE(trace_GetPixelShaderConstantI, StartRegister, pConstantData, Vector4iCount);
		return m_pIDirect3DDevice9->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}

	HRESULT d3d9ex::D3D9Device::SetPixelShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT  BoolCount)
	{
		TRACE_IF_ACTIVE(trace_SetPixelShaderConstantB, StartRegister, pConstantData, BoolCount);
		return m_pIDirect3DDevice9->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
	}

	HRESULT d3d9ex::D3D9Device::GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount)
	{
		TRACE_IF_ACTIVE(trace_GetPixelShaderConstantB, StartRegister, pConstantData, BoolCount);
		return m_pIDirect3DDevice9->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
	}

	HRESULT d3d9ex::D3D9Device::DrawRectPatch(UINT Handle, CONST float* pNumSegs, CONST D3DRECTPATCH_INFO* pRectPatchInfo)
	{
		TRACE_IF_ACTIVE(trace_DrawRectPatch, Handle, pNumSegs, pRectPatchInfo);
		return m_pIDirect3DDevice9->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo);
	}

	HRESULT d3d9ex::D3D9Device::DrawTriPatch(UINT Handle, CONST float* pNumSegs, CONST D3DTRIPATCH_INFO* pTriPatchInfo)
	{
		TRACE_IF_ACTIVE(trace_DrawTriPatch, Handle, pNumSegs, pTriPatchInfo);
		return m_pIDirect3DDevice9->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo);
	}

	HRESULT d3d9ex::D3D9Device::DeletePatch(UINT Handle)
	{
		TRACE_IF_ACTIVE(trace_DeletePatch, Handle);
		return m_pIDirect3DDevice9->DeletePatch(Handle);
	}

	HRESULT d3d9ex::D3D9Device::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery)
	{
		TRACE_IF_ACTIVE(trace_CreateQuery, Type, ppQuery);
		return m_pIDirect3DDevice9->CreateQuery(Type, ppQuery);
	}

#pragma endregion

#pragma region _D3D9

	HRESULT __stdcall d3d9ex::_d3d9::QueryInterface(REFIID riid, void** ppvObj)
	{
		*ppvObj = nullptr;

		HRESULT hRes = m_pIDirect3D9->QueryInterface(riid, ppvObj);

		if (hRes == NOERROR) {
			*ppvObj = this;
		}

		return hRes;
	}

	ULONG __stdcall d3d9ex::_d3d9::AddRef()
	{
		return m_pIDirect3D9->AddRef();
	}

	ULONG __stdcall d3d9ex::_d3d9::Release()
	{
		ULONG count = m_pIDirect3D9->Release();
		if (!count) delete this;
		return count;
	}

	HRESULT __stdcall d3d9ex::_d3d9::RegisterSoftwareDevice(void* pInitializeFunction)
	{
		return m_pIDirect3D9->RegisterSoftwareDevice(pInitializeFunction);
	}

	UINT __stdcall d3d9ex::_d3d9::GetAdapterCount()
	{
		return m_pIDirect3D9->GetAdapterCount();
	}

	HRESULT __stdcall d3d9ex::_d3d9::GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier)
	{
		return m_pIDirect3D9->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
	}

	UINT __stdcall d3d9ex::_d3d9::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format)
	{
		return m_pIDirect3D9->GetAdapterModeCount(Adapter, Format);
	}

	HRESULT __stdcall d3d9ex::_d3d9::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode)
	{
		return m_pIDirect3D9->EnumAdapterModes(Adapter, Format, Mode, pMode);
	}

	HRESULT __stdcall d3d9ex::_d3d9::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode)
	{
		return m_pIDirect3D9->GetAdapterDisplayMode(Adapter, pMode);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CheckDeviceType(UINT iAdapter, D3DDEVTYPE DevType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed)
	{
		return m_pIDirect3D9->CheckDeviceType(iAdapter, DevType, DisplayFormat, BackBufferFormat, bWindowed);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat)
	{
		const auto hr = m_pIDirect3D9->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
		if (FAILED(hr))
			note_format_rejection(CheckFormat, RType, Usage);
		return hr;
	}

	HRESULT __stdcall d3d9ex::_d3d9::CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels)
	{
		return m_pIDirect3D9->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat)
	{
		return m_pIDirect3D9->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat)
	{
		return m_pIDirect3D9->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps)
	{
		return m_pIDirect3D9->GetDeviceCaps(Adapter, DeviceType, pCaps);
	}

	HMONITOR __stdcall d3d9ex::_d3d9::GetAdapterMonitor(UINT Adapter)
	{
		return m_pIDirect3D9->GetAdapterMonitor(Adapter);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice9** ppReturnedDeviceInterface)
	{
		const auto& res_cfg = shared::common::config::get().resolution;
		if (pPresentationParameters && res_cfg.render_width > 0 && res_cfg.render_height > 0)
		{
			shared::common::log("Resolution", std::format("CreateDevice: BackBuffer {}x{} -> {}x{}",
				pPresentationParameters->BackBufferWidth, pPresentationParameters->BackBufferHeight,
				res_cfg.present_width, res_cfg.present_height));
			pPresentationParameters->BackBufferWidth = res_cfg.present_width;
			pPresentationParameters->BackBufferHeight = res_cfg.present_height;
		}
		HRESULT hres = m_pIDirect3D9->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
		shared::common::log("d3d9", "m_pIDirect3D9->CreateDevice", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		if (SUCCEEDED(hres) && *ppReturnedDeviceInterface) {
			shared::globals::d3d_device_real = *ppReturnedDeviceInterface;
			*ppReturnedDeviceInterface = new d3d9ex::D3D9Device(*ppReturnedDeviceInterface);
			shared::globals::d3d_device = *ppReturnedDeviceInterface;
		}

		return hres;
	}


#pragma endregion

#pragma region _D3D9Ex

	HRESULT __stdcall d3d9ex::_d3d9ex::QueryInterface(REFIID riid, void** ppvObj)
	{
		*ppvObj = nullptr;
		HRESULT hRes = m_pIDirect3D9Ex->QueryInterface(riid, ppvObj);

		if (hRes == NOERROR) {
			*ppvObj = this;
		}

		return hRes;
	}

	ULONG __stdcall d3d9ex::_d3d9ex::AddRef()
	{
		return m_pIDirect3D9Ex->AddRef();
	}

	ULONG __stdcall d3d9ex::_d3d9ex::Release()
	{
		ULONG count = m_pIDirect3D9Ex->Release();
		if (!count) delete this;
		return count;
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::RegisterSoftwareDevice(void* pInitializeFunction)
	{
		return m_pIDirect3D9Ex->RegisterSoftwareDevice(pInitializeFunction);
	}

	UINT __stdcall d3d9ex::_d3d9ex::GetAdapterCount()
	{
		return m_pIDirect3D9Ex->GetAdapterCount();
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier)
	{
		return m_pIDirect3D9Ex->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
	}

	UINT __stdcall d3d9ex::_d3d9ex::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format)
	{
		return m_pIDirect3D9Ex->GetAdapterModeCount(Adapter, Format);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode)
	{
		return m_pIDirect3D9Ex->EnumAdapterModes(Adapter, Format, Mode, pMode);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode)
	{
		return m_pIDirect3D9Ex->GetAdapterDisplayMode(Adapter, pMode);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CheckDeviceType(UINT iAdapter, D3DDEVTYPE DevType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed)
	{
		return m_pIDirect3D9Ex->CheckDeviceType(iAdapter, DevType, DisplayFormat, BackBufferFormat, bWindowed);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat)
	{
		const auto hr = m_pIDirect3D9Ex->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
		if (FAILED(hr))
			note_format_rejection(CheckFormat, RType, Usage);
		return hr;
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels)
	{
		return m_pIDirect3D9Ex->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat)
	{
		return m_pIDirect3D9Ex->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat)
	{
		return m_pIDirect3D9Ex->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps)
	{
		return m_pIDirect3D9Ex->GetDeviceCaps(Adapter, DeviceType, pCaps);
	}

	HMONITOR __stdcall d3d9ex::_d3d9ex::GetAdapterMonitor(UINT Adapter)
	{
		return m_pIDirect3D9Ex->GetAdapterMonitor(Adapter);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice9** ppReturnedDeviceInterface)
	{
		const auto& res_cfg = shared::common::config::get().resolution;
		if (pPresentationParameters && res_cfg.render_width > 0 && res_cfg.render_height > 0)
		{
			shared::common::log("Resolution", std::format("CreateDeviceEx: BackBuffer {}x{} -> {}x{}",
				pPresentationParameters->BackBufferWidth, pPresentationParameters->BackBufferHeight,
				res_cfg.present_width, res_cfg.present_height));
			pPresentationParameters->BackBufferWidth = res_cfg.present_width;
			pPresentationParameters->BackBufferHeight = res_cfg.present_height;
		}
		HRESULT hres = m_pIDirect3D9Ex->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
		shared::common::log("d3d9", "m_pIDirect3D9Ex->CreateDevice", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);

		if (SUCCEEDED(hres) && *ppReturnedDeviceInterface) {
			shared::globals::d3d_device_real = *ppReturnedDeviceInterface;
			*ppReturnedDeviceInterface = new d3d9ex::D3D9Device(*ppReturnedDeviceInterface);
			shared::globals::d3d_device = *ppReturnedDeviceInterface;
		}

		return hres;
	}

	UINT __stdcall d3d9ex::_d3d9ex::GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter)
	{
		return (m_pIDirect3D9Ex->GetAdapterModeCountEx(Adapter, pFilter));
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::EnumAdapterModesEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter, UINT Mode, D3DDISPLAYMODEEX* pMode)
	{
		return (m_pIDirect3D9Ex->EnumAdapterModesEx(Adapter, pFilter, Mode, pMode));
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation)
	{
		return (m_pIDirect3D9Ex->GetAdapterDisplayModeEx(Adapter, pMode, pRotation));
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CreateDeviceEx(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode, IDirect3DDevice9Ex** ppReturnedDeviceInterface)
	{
		return (m_pIDirect3D9Ex->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, pFullscreenDisplayMode, ppReturnedDeviceInterface));
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::GetAdapterLUID(UINT Adapter, LUID* pLUID)
	{
		return (m_pIDirect3D9Ex->GetAdapterLUID(Adapter, pLUID));
	}
#pragma endregion

	d3d9ex::d3d9ex() {}
}

// ---- Exported entry points (linked via d3d9.def) ----

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion)
{
	shared::common::log("d3d9", "Direct3DCreate9 called. Creating proxy interface.");
	auto real_fn = d3d9_proxy::get_Direct3DCreate9();
	if (!real_fn) return nullptr;

	IDirect3D9* real_d3d9 = real_fn(SDKVersion);
	if (!real_d3d9) return nullptr;

	shared::globals::d3d9_interface = new comp::d3d9ex::_d3d9(real_d3d9);
	return shared::globals::d3d9_interface;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D)
{
	shared::common::log("d3d9", "Direct3DCreate9Ex called. Creating proxy interface.");
	auto real_fn = d3d9_proxy::get_Direct3DCreate9Ex();
	if (!real_fn || !ppD3D) return D3DERR_NOTAVAILABLE;

	IDirect3D9Ex* real_d3d9ex = nullptr;
	HRESULT hr = real_fn(SDKVersion, &real_d3d9ex);
	if (FAILED(hr) || !real_d3d9ex) return hr;

	auto* proxy = new comp::d3d9ex::_d3d9ex(real_d3d9ex);
	shared::globals::d3d9_interface = proxy;
	*ppD3D = proxy;
	return hr;
}
