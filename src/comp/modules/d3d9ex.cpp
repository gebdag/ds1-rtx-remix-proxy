// IDirect3DDevice9 proxy — thin delegation hub for all 119 vtable methods.
#include "std_include.hpp"
#include <tuple>
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
#include "../game/mesh_tables.hpp"
#include "../game/vanilla_view.hpp"
#include "../game/atmosphere.hpp"
#include "../game/env_reflection.hpp"
#include "../game/lightmap_sun.hpp"
#include "../game/ffx_instances.hpp"
#include "../game/fx_draw_owner.hpp"
#include "../game/hud_scale.hpp"
#include "../game/eye_sprites.hpp"
#include "../game/aspect_ratio.hpp"
#include "game_lights.hpp"
#include "lookdev_capture.hpp"

namespace {
	// Per-frame UP draw pass counter for individual post-FX control
	static int s_postfx_pass_idx = 0;

	// Where the game thread's frame goes, with [FFPSteps] Profile on: inside draw calls (the proxy's
	// routing and conversion plus the bridge client), inside Present (the bridge hands the frame to
	// the Remix server and may wait for it), and the rest (the game itself and every other device
	// call). Logged every 600 frames with the FFP step totals.
	struct frame_split
	{
		long long draw_ticks = 0, present_ticks = 0, limiter_ticks = 0, last_present = 0, frame_ticks = 0;
		unsigned long long tsc_start = 0;
		long long qpc_start = 0;
		long long skin_ticks = 0;
		double skin_draws = 0, skin_bones = 0;
		unsigned int draws = 0, frames = 0;
		double draw_ms = 0, present_ms = 0, frame_ms = 0, ffp_ms = 0;
	};
	frame_split s_split;

	long long split_now()
	{
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		return t.QuadPart;
	}

	double split_ms(long long ticks)
	{
		static const double inv = []
		{
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			return 1000.0 / static_cast<double>(f.QuadPart);
		}();
		return static_cast<double>(ticks) * inv;
	}

	// Per device method, with [FFPSteps] Profile on: time inside the wrapper (the proxy's own work
	// plus the bridge client), in TSC ticks, and calls. Every wrapped method opens with
	// TRACE_IF_ACTIVE, so the macro carries the scope.
	struct api_stat
	{
		const char* name;
		unsigned long long ticks = 0;
		unsigned int calls = 0;
	};
	std::vector<api_stat*>& api_stats()
	{
		static std::vector<api_stat*> stats;
		return stats;
	}
	api_stat* register_api_stat(const char* name)
	{
		auto* s = new api_stat { name };
		api_stats().push_back(s);
		return s;
	}
	struct api_scope
	{
		api_stat* stat;
		unsigned long long start;
		explicit api_scope(api_stat* s) : stat(s), start(shared::common::ffp_prof::profiling() ? __rdtsc() : 0) {}
		~api_scope()
		{
			if (start)
			{
				stat->ticks += __rdtsc() - start;
				++stat->calls;
			}
		}
	};

#undef TRACE_IF_ACTIVE
#undef TRACE_IF_ACTIVE_NOARGS
#define TRACE_IF_ACTIVE(fn, ...) \
	static api_stat* const api_stat_ = register_api_stat(#fn + 6); \
	api_scope api_scope_(api_stat_); \
	do { if (comp::tracer::is_active()) fn(comp::tracer::ref(), __VA_ARGS__); } while(0)
#define TRACE_IF_ACTIVE_NOARGS(fn) \
	static api_stat* const api_stat_ = register_api_stat(#fn + 6); \
	api_scope api_scope_(api_stat_); \
	do { if (comp::tracer::is_active()) fn(comp::tracer::ref()); } while(0)

	// The game creates and releases a handful of depth-stencil surfaces every frame (5 in
	// Blighttown). Over the bridge each Create waits for the Remix server, about 0.2 ms apiece, so
	// the proxy keeps one reference to each surface it handed out and hands it out again once the
	// game has released it: AddRef returning 2 means only the pool holds it. A depth-stencil
	// surface's contents are undefined on creation, so a reused one is as good as a new one.
	struct depth_pool_key
	{
		IDirect3DDevice9* device;
		UINT width, height;
		D3DFORMAT format;
		D3DMULTISAMPLE_TYPE multisample;
		DWORD quality;
		BOOL discard;
		bool operator==(const depth_pool_key&) const = default;
	};
	constexpr size_t kDepthPoolMax = 16;
	std::vector<std::pair<depth_pool_key, IDirect3DSurface9*>> s_depth_pool;

	IDirect3DSurface9* take_pooled_depth(const depth_pool_key& key)
	{
		for (auto& [k, surface] : s_depth_pool)
		{
			if (!(k == key))
				continue;
			if (surface->AddRef() == 2)
				return surface;
			surface->Release();
		}
		return nullptr;
	}

	void release_depth_pool()
	{
		for (auto& [k, surface] : s_depth_pool)
			surface->Release();
		s_depth_pool.clear();
	}

	// True when the game holds none of the pooled surfaces.
	bool depth_pool_unused()
	{
		for (auto& [k, surface] : s_depth_pool)
		{
			const ULONG refs = surface->AddRef();
			surface->Release();
			if (refs != 2)
				return false;
		}
		return true;
	}

	// A full pool drops one surface the game no longer holds, so sizes the game stopped
	// using do not keep the pool from taking new ones.
	bool make_depth_pool_room()
	{
		for (auto it = s_depth_pool.begin(); it != s_depth_pool.end(); ++it)
		{
			if (it->second->AddRef() == 2)
			{
				it->second->Release();
				it->second->Release();
				s_depth_pool.erase(it);
				return true;
			}
			it->second->Release();
		}
		return false;
	}

	struct draw_split_scope
	{
		long long start = shared::common::ffp_prof::profiling() ? split_now() : 0;
		~draw_split_scope()
		{
			if (start)
			{
				s_split.draw_ticks += split_now() - start;
				++s_split.draws;
			}
		}
	};

	// The same split every half second, with [FFPSteps] Profile on: a dip of a second or two (an attack's
	// effects) disappears in the 600 frame totals. The worst frame of the window shows single hitches.
	struct frame_timeline
	{
		long long start = 0, draw = 0, present = 0, seen_draw = 0, worst = 0;
		unsigned int draws = 0, seen_draws = 0, frames = 0;
	};
	frame_timeline s_timeline;

	void timeline_on_present(long long now, long long frame_ticks, long long present_ticks)
	{
		auto& t = s_timeline;
		t.draw += s_split.draw_ticks - t.seen_draw;
		t.draws += s_split.draws - t.seen_draws;
		t.seen_draw = s_split.draw_ticks;
		t.seen_draws = s_split.draws;
		t.present += present_ticks;
		t.worst = std::max(t.worst, frame_ticks);
		++t.frames;
		if (!t.start)
			t.start = now;
		const double span = split_ms(now - t.start);
		if (span < 500.0)
			return;
		const double n = static_cast<double>(t.frames), draw = split_ms(t.draw) / n, present = split_ms(t.present) / n;
		const auto* lights = comp::game_lights::get();
		shared::common::log("FrameTimeline", std::format(
			"{:.1f} ms/frame (worst {:.1f}) = draw calls {:.1f} ms ({:.0f} draws) + Present {:.1f} ms + game and other calls {:.1f} ms | FFX instances {}, lights {}/{}",
			span / n, split_ms(t.worst), draw, t.draws / n, present, span / n - draw - present,
			comp::game::ffx_instances::last_walk_count(), lights ? lights->active_light_count() : 0, lights ? lights->total_light_count() : 0));
		t.start = now;
		t.draw = t.present = t.worst = 0;
		t.draws = t.frames = 0;
	}

	void split_on_present(long long present_ticks)
	{
		if (!shared::common::ffp_prof::profiling())
		{
			s_split = {};
			s_timeline = {};
			return;
		}
		const long long now = split_now();
		if (!s_split.tsc_start)
		{
			s_split.tsc_start = __rdtsc();
			s_split.qpc_start = now;
		}
		if (s_split.last_present)
			timeline_on_present(now, now - s_split.last_present, present_ticks);
		if (s_split.last_present)
			s_split.frame_ticks += now - s_split.last_present;
		s_split.last_present = now;
		s_split.present_ticks += present_ticks;
		s_split.ffp_ms += shared::common::ffp_prof::total_ms();
		s_split.skin_ticks += comp::skinning::s_stats.ticks;
		s_split.skin_draws += comp::skinning::s_stats.draws;
		s_split.skin_bones += comp::skinning::s_stats.bones;
		comp::skinning::s_stats = {};
		if (++s_split.frames < 600)
			return;
		const double n = static_cast<double>(s_split.frames);
		const double frame = split_ms(s_split.frame_ticks) / n, draw = split_ms(s_split.draw_ticks) / n;
		const double present = split_ms(s_split.present_ticks) / n, limiter = split_ms(s_split.limiter_ticks) / n;
		shared::common::log("FrameSplit", std::format(
			"last {} frames: frame {:.2f} ms ({:.1f} fps) = draw calls {:.2f} ms ({:.0f} draws, FFP steps {:.2f} ms) + "
			"Present {:.2f} ms + FPS limiter wait {:.2f} ms + game and other calls {:.2f} ms",
			s_split.frames, frame, 1000.0 / frame, draw, s_split.draws / n, s_split.ffp_ms / n, present, limiter,
			frame - draw - present - limiter));
		// The device methods, ranked: TSC ticks to ms through the QPC span of the window.
		const double tsc_per_ms = static_cast<double>(__rdtsc() - s_split.tsc_start) / split_ms(now - s_split.qpc_start);
		std::vector<api_stat*> ranked = api_stats();
		std::sort(ranked.begin(), ranked.end(), [](const api_stat* a, const api_stat* b) { return a->ticks > b->ticks; });
		std::string methods;
		double methods_ms = 0.0;
		for (const auto* s : ranked)
			methods_ms += static_cast<double>(s->ticks) / tsc_per_ms / n;
		for (size_t i = 0; i < ranked.size() && i < 14 && ranked[i]->ticks; ++i)
			methods += std::format("{}{} {:.2f} ms/{:.0f}", i ? ", " : "", ranked[i]->name,
				static_cast<double>(ranked[i]->ticks) / tsc_per_ms / n, ranked[i]->calls / n);
		shared::common::log("FrameSplit", std::format("device methods {:.2f} ms per frame (draws included): {}", methods_ms, methods));
		{
			namespace prof = shared::common::ffp_prof;
			std::string steps;
			for (int i = 0; i < prof::STEP_COUNT; ++i)
			{
				const auto s = static_cast<prof::step>(i);
				if (prof::self_ms(s) >= 0.05)
					steps += std::format("{}{} {:.2f} ms/{}", steps.empty() ? "" : ", ", prof::label(s), prof::self_ms(s), prof::calls(s));
			}
			shared::common::log("FrameSplit", std::format(
				"FFP steps (last frames, smoothed): {} | converted {} passthrough {} | expand {:.2f} MB, reuse hits {} misses {} | "
				"skinned draws {:.2f} ms/{:.0f}, bones uploaded {:.0f}",
				steps, prof::converted_draws(), prof::passthrough_draws(), prof::expand_mb(), prof::reuse_hits(), prof::reuse_misses(),
				split_ms(s_split.skin_ticks) / n, s_split.skin_draws / n, s_split.skin_bones / n));
		}
		for (auto* s : api_stats())
		{
			s->ticks = 0;
			s->calls = 0;
		}
		const long long last = s_split.last_present;
		s_split = {};
		s_split.last_present = last;
		s_timeline.seen_draw = 0;
		s_timeline.seen_draws = 0;
	}

	// Lookdev mode is the vanilla reference: draws and render targets go to the
	// device untouched, past every Remix-oriented route (prepass drop, UV
	// expansion, sky/HUD markers, post-FX skip, category filters, RT redirects).
	UINT up_vertex_count(D3DPRIMITIVETYPE type, UINT primitives)
	{
		switch (type)
		{
		case D3DPT_POINTLIST: return primitives;
		case D3DPT_LINELIST: return primitives * 2;
		case D3DPT_LINESTRIP: return primitives + 1;
		case D3DPT_TRIANGLELIST: return primitives * 3;
		default: return primitives + 2; // strips and fans
		}
	}

	// The render targets the game set a remembered full viewport on (SetViewport changed it to the target's size): the ones
	// it draws its menus and HUD into; cleared at every device creation and Reset. The game's partial scissor rects there are
	// in its fixed 1280x720 UI space (a message's text box, (394, 144, 1264, 202), cut its text away at 1920x1080) and scale
	// to the target (scaled_scissor). Not by that viewport: on the menu target the game sets the desktop size it created the
	// device at (3840x2160), which halved the menus' scissors at 1920x1080 (the menu cut off, the fade dark in one quarter).
	// Also every render texture the game asked at its 1280x720 UI size and got at the present size (CreateTexture): with the
	// device created at the game's own size the game sets no remembered viewport there (frame_20261106_114841: the message
	// text cut away again), and its scissors are in the size it asked for.
	struct remembered_viewport
	{
		RECT last_scaled {}; // what the last scaled rect became: set again as it is (a Get and Set), it is not scaled twice
		bool have_last = false;
		LONG ui_width = 1280, ui_height = 720; // the size the game believes the target has
		// A UI render texture CreateTexture registered: the size and format it was made at, to know it again after a Reset.
		bool created_as_ui = false;
		UINT made_width = 0, made_height = 0;
		D3DFORMAT asked_format = D3DFMT_UNKNOWN;
	};
	std::unordered_map<IDirect3DSurface9*, remembered_viewport> s_remembered_viewports;

	// The UI render textures there were before a Reset. After it the game makes them again at the size they had (3840x2160,
	// not 1280x720) while its scissors there stay in 1280x720 space: the message text was cut away after the fullscreen
	// switch at 4K (console.log 2026-11-11 21:38, the first CreateTexture after the Reset is the UI target again). The
	// first render texture asked at such a size and format takes the entry back.
	std::vector<remembered_viewport> s_ui_targets_before_reset;

	IDirect3DSurface9* current_target(IDirect3DDevice9* dev)
	{
		IDirect3DSurface9* target = nullptr;
		if (FAILED(dev->GetRenderTarget(0, &target)) || !target)
			return nullptr;
		target->Release(); // only its address, as a key
		return target;
	}

	// The game window as the device is made or reset: whether the game asked for fullscreen, its styles and where it sits
	// on its monitor. Through the Remix bridge a fullscreen game stays a window covering the screen, so a frame style left
	// on it shows as a line round the image.
	void log_game_window(const D3DPRESENT_PARAMETERS* pp, const char* where)
	{
		const HWND window = pp->hDeviceWindow ? pp->hDeviceWindow : shared::globals::main_window;
		if (!window)
			return;
		RECT rect {};
		GetWindowRect(window, &rect);
		MONITORINFO monitor { sizeof(MONITORINFO) };
		GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
		shared::common::log("Window", std::format("{}: {} requested, style 0x{:08X} exstyle 0x{:08X}, window ({}, {})-({}, {}), monitor ({}, {})-({}, {})",
			where, pp->Windowed ? "windowed" : "fullscreen", static_cast<uint32_t>(GetWindowLongW(window, GWL_STYLE)),
			static_cast<uint32_t>(GetWindowLongW(window, GWL_EXSTYLE)), rect.left, rect.top, rect.right, rect.bottom,
			monitor.rcMonitor.left, monitor.rcMonitor.top, monitor.rcMonitor.right, monitor.rcMonitor.bottom));
	}

	// The game's back buffer request, at device creation and every Reset, is what the resolution follows.
	void follow_game_resolution(D3DPRESENT_PARAMETERS* pp, const char* where)
	{
		s_ui_targets_before_reset.clear();
		if (std::string_view(where) == "Reset")
			for (const auto& [surface, v] : s_remembered_viewports)
				if (v.created_as_ui)
					s_ui_targets_before_reset.push_back(v);
		s_remembered_viewports.clear();
		if (!pp)
			return;
		const UINT asked_width = pp->BackBufferWidth, asked_height = pp->BackBufferHeight;
		auto& res = shared::common::config::get().resolution;
		res.follow_game(pp->BackBufferWidth, pp->BackBufferHeight);
		shared::common::log("Resolution", std::format("{}: BackBuffer {}x{} -> {}x{}, render {}x{}", where, asked_width, asked_height,
			pp->BackBufferWidth, pp->BackBufferHeight, res.render_width, res.render_height));
		log_game_window(pp, where);
		comp::game::aspect_ratio::on_game_resolution(pp->BackBufferWidth, pp->BackBufferHeight, std::string_view(where).starts_with("Create"));
	}

	// After a Reset the game sets viewports and scissors at the full size it remembers. True when (width, height) is
	// such a size and the current target is another: this gives the target's.
	bool remembered_size(IDirect3DDevice9* dev, UINT width, UINT height, UINT& target_width, UINT& target_height)
	{
		const auto& res = shared::common::config::get().resolution;
		if (!res.was_render_size(width, height) && !res.was_present_size(width, height))
			return false;
		IDirect3DSurface9* target = nullptr;
		if (FAILED(dev->GetRenderTarget(0, &target)) || !target)
			return false;
		D3DSURFACE_DESC desc {};
		const bool known = SUCCEEDED(target->GetDesc(&desc));
		target->Release();
		if (!known || (desc.Width == width && desc.Height == height))
			return false;
		target_width = desc.Width;
		target_height = desc.Height;
		return true;
	}

	// A partial scissor in the game's UI space on one of those targets: the rect scaled to the target. A rect covering the
	// whole target, one outside the UI space, or the very rect the last scaling made pass as they are.
	bool scaled_scissor(IDirect3DDevice9* dev, const RECT& rect, RECT& scaled)
	{
		IDirect3DSurface9* target = current_target(dev);
		const auto it = s_remembered_viewports.find(target);
		if (it == s_remembered_viewports.end())
			return false;
		const LONG kUiWidth = it->second.ui_width, kUiHeight = it->second.ui_height;
		D3DSURFACE_DESC desc {};
		if (FAILED(target->GetDesc(&desc)) || (desc.Width == static_cast<UINT>(kUiWidth) && desc.Height == static_cast<UINT>(kUiHeight)))
			return false;
		if (rect.left <= 0 && rect.top <= 0 && rect.right >= static_cast<LONG>(desc.Width) && rect.bottom >= static_cast<LONG>(desc.Height))
			return false;
		auto& v = it->second;
		if (v.have_last && rect.left == v.last_scaled.left && rect.top == v.last_scaled.top &&
			rect.right == v.last_scaled.right && rect.bottom == v.last_scaled.bottom)
			return false;
		if (rect.left < 0 || rect.top < 0 || rect.right > kUiWidth || rect.bottom > kUiHeight)
			return false;
		const double sx = double(desc.Width) / kUiWidth, sy = double(desc.Height) / kUiHeight;
		scaled.left = static_cast<LONG>(std::lround(rect.left * sx));
		scaled.top = static_cast<LONG>(std::lround(rect.top * sy));
		scaled.right = static_cast<LONG>(std::lround(rect.right * sx));
		scaled.bottom = static_cast<LONG>(std::lround(rect.bottom * sy));
		v.last_scaled = scaled;
		v.have_last = true;
		return true;
	}

	// A partial scissor inside the game's 1280x720 UI space on a larger target that is not one of the UI targets above
	// stays where the game put it, which cuts away what it draws there (message text at 4K fullscreen). Each such case is
	// logged once, with the target, so the target a cut-off UI draw goes to shows in the log.
	void note_unscaled_ui_scissor(IDirect3DDevice9* dev, const RECT& rect)
	{
		if (rect.left < 0 || rect.top < 0 || rect.right > 1280 || rect.bottom > 720 ||
			(!rect.left && !rect.top && rect.right == 1280 && rect.bottom == 720))
			return;
		IDirect3DSurface9* target = current_target(dev);
		D3DSURFACE_DESC desc {};
		if (!target || FAILED(target->GetDesc(&desc)) || (desc.Width <= 1280 && desc.Height <= 720))
			return;
		static std::set<std::tuple<IDirect3DSurface9*, LONG, LONG, LONG, LONG>> seen;
		if (seen.size() >= 64 || !seen.emplace(target, rect.left, rect.top, rect.right, rect.bottom).second)
			return;
		IDirect3DSurface9* back_buffer = nullptr;
		bool is_back_buffer = false;
		if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer)) && back_buffer)
		{
			is_back_buffer = back_buffer == target;
			back_buffer->Release();
		}
		shared::common::log("Scissor", std::format("UI-space scissor ({}, {}, {}, {}) left as is on a {}x{} {} target {:p}, not a known UI target",
			rect.left, rect.top, rect.right, rect.bottom, desc.Width, desc.Height, is_back_buffer ? "back buffer" : "render",
			static_cast<void*>(target)));
	}

	bool lookdev_passthrough()
	{
		return shared::common::config::get().lookdev.enabled || comp::lookdev_capture::main_game_frame();
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

	// HDR10 output ([HDR] Enabled, config.hpp): the back buffer's size, which the scene target shares.
	bool back_buffer_size(IDirect3DDevice9* dev, UINT& width, UINT& height)
	{
		IDirect3DSurface9* back = nullptr;
		if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back)
			return false;
		D3DSURFACE_DESC desc {};
		const bool ok = SUCCEEDED(back->GetDesc(&desc));
		back->Release();
		width = desc.Width;
		height = desc.Height;
		return ok;
	}

	// HDR10 output: the final 2D pass (the scene target through the brightness LUT into the back buffer) becomes a
	// plain copy, since the LUT's dependent read clamps anything above 1. True when the draw was replaced. The LUT is
	// identity at the game's default brightness; HDR has its own white level (rtx.hdr.paperWhiteNits).
	bool hdr_final_pass_copy(IDirect3DDevice9* dev)
	{
		if (!shared::common::config::get().hdr.enabled)
			return false;
		IDirect3DSurface9* rt = nullptr;
		IDirect3DSurface9* back = nullptr;
		dev->GetRenderTarget(0, &rt);
		dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
		const bool to_back = rt && back && rt == back;
		if (back)
			back->Release();
		if (!to_back)
		{
			if (rt)
				rt->Release();
			return false;
		}
		bool copied = false;
		IDirect3DBaseTexture9* source = nullptr;
		if (SUCCEEDED(dev->GetTexture(0, &source)) && source && source->GetType() == D3DRTYPE_TEXTURE)
		{
			D3DSURFACE_DESC src_desc {}, rt_desc {};
			static_cast<IDirect3DTexture9*>(source)->GetLevelDesc(0, &src_desc);
			rt->GetDesc(&rt_desc);
			IDirect3DSurface9* level = nullptr;
			if ((src_desc.Usage & D3DUSAGE_RENDERTARGET) && src_desc.Width == rt_desc.Width && src_desc.Height == rt_desc.Height &&
				SUCCEEDED(static_cast<IDirect3DTexture9*>(source)->GetSurfaceLevel(0, &level)) && level)
			{
				copied = SUCCEEDED(dev->StretchRect(level, nullptr, rt, nullptr, D3DTEXF_POINT));
				level->Release();
			}
		}
		if (source)
			source->Release();
		rt->Release();
		return copied;
	}

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
			// The distortion (heat shimmer, the bonfire's rest) reads a copy of the frame. In the F8 vanilla view, which passes the
			// screen effects since d22cc0f (2026-11-02), that copy is black, and the distortion's mesh drew a black shape over the
			// frame, and over every capture's reference image taken in that view.
			if (cfg.drop_screen_effects || cls == sfx_class::Distortion)
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

	// The distortion (heat shimmer, the bonfire's rest) reads a copy of the frame, which is black in the F8 vanilla view: its mesh
	// drew a black shape over the frame and over F9 capture reference images (since d22cc0f passed screen effects there).
	bool drop_distortion_in_vanilla_view()
	{
		return comp::game::vanilla_view::active() && current_sfx() == shared::common::sfx_class::Distortion;
	}

	// The smoke the bonfire's rest effect throws round the camera as the player sits down: FFX 90010, three grey sheets of about
	// 2 m (s00313, alpha blended), gone after some three seconds. The game lays them over the effect's finished green streaks;
	// Remix sorts them in among the streaks by depth, triangle by triangle: dark hard-edged shards across the whole frame
	// (frame_20261111_142957; without the sheets the frame is the game's soft green burst). They are left out, and only they, by
	// the effect that draws them (fx_draw_owner): s00313 is also every fog gate's texture, the smoke of the Kiln's cutscene and the
	// mist of eight maps, so nothing can go by the texture. [Sfx] SitSmoke=1 (live proxy.Sfx.SitSmoke) draws them again.
	// Not in the vanilla view, nor before the fixed-function conversion runs: the game's own picture has them.
	bool sit_smoke_left_out()
	{
		constexpr int kRestEffect = 90010;
		auto& ffp = shared::common::ffp_state::get();
		if (shared::common::config::get().sfx.sit_smoke || !ffp.is_enabled() || comp::game::vanilla_view::active())
			return false;
		const unsigned owner = comp::game::fx_draw_owner::current();
		return owner && comp::game::texture_names::name_of(ffp.cur_texture(0)) == "s00313" &&
			comp::game::ffx_instances::top_effect_id(owner) == kRestEffect;
	}

	// Sprite particles of UP draws go fixed-function (ffp_state::draw_sprite_up);
	// true when it drew. Everything else, and anything it cannot place, takes sfx_draw.
	bool sfx_sprite_up(IDirect3DDevice9* dev, shared::common::sfx_class cls, D3DPRIMITIVETYPE prim, UINT prim_count,
		UINT min_index, UINT num_verts, const void* indices, D3DFORMAT index_format,
		const void* vertices, UINT stride, HRESULT& hr)
	{
		using shared::common::sfx_class;
		const auto& cfg = shared::common::config::get().sfx;
		if ((cls == sfx_class::Sprite || cls == sfx_class::SoftSprite) && sit_smoke_left_out())
		{
			hr = S_OK;
			return true;
		}
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
		static std::vector<std::uint8_t> eye_copy;
		vertices = comp::game::eye_sprites::adjust(ffp.cur_texture(0), vertices, stride, min_index, num_verts, ffp.frame_count(),
			ffp.cur_decl_color_offset(), eye_copy);
		const bool ok = ffp.draw_sprite_up(dev, prim, prim_count, min_index, num_verts,
			indices, index_format, vertices, stride, cls != sfx_class::LitSprite, hr);
		(ok ? drawn : fallback)++;
		// TEMPORARY: the Darksign's ground ring. Removed once it is explained.
		if (comp::game::texture_names::name_of(ffp.cur_texture(0)) == "s00050")
		{
			static uint32_t ring_frame = 0;
			if (ffp.frame_count() >= ring_frame)
			{
				ring_frame = ffp.frame_count() + 5;
				const auto* v = static_cast<const float*>(vertices);
				const auto* c = ffp.ps_const_data() + 139 * 4;
				DWORD blend = 0, src = 0, dst = 0, z = 0;
				dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
				dev->GetRenderState(D3DRS_SRCBLEND, &src);
				dev->GetRenderState(D3DRS_DESTBLEND, &dst);
				dev->GetRenderState(D3DRS_ZENABLE, &z);
				float ox = 0, oy = 0, oz = 0;
				const bool offset = ffp.world_offset(ox, oy, oz);
				const auto* bytes = static_cast<const unsigned char*>(vertices);
				const DWORD colour = stride == 48 ? *reinterpret_cast<const DWORD*>(bytes + 24) : 0;
				const float feather = stride == 48 ? *reinterpret_cast<const float*>(bytes + 40) : 0.0f;
				const auto* e = ffp.ps_const_data() + 135 * 4;
				shared::common::log("RingTest", std::format("frame {} class {} drawn {} hr {:#x} prims {} stride {} first vertex ({:.3f}, {:.3f}, {:.3f}) colour {:#010x} feather {:.2f} c135 ({:.3f}, {:.3f}) c139 ({:.2f}, {:.2f}, {:.2f}, {:.2f}) blend {} {} {} z {} world offset {} ({:.2f}, {:.2f}, {:.2f})",
					ffp.frame_count(), static_cast<int>(cls), ok, static_cast<uint32_t>(hr), prim_count, stride, v[0], v[1], v[2], colour, feather, e[0], e[1], c[0], c[1], c[2], c[3], blend, src, dst, z, offset, ox, oy, oz));
			}
		}
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
		// Each pooled depth surface keeps a reference to the device. When those are all that
		// is left, the game has let go of the device: release them, and with them the device.
		if (count && count == s_depth_pool.size() && depth_pool_unused())
		{
			release_depth_pool();
			count = 0;
		}
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
		release_depth_pool();
		shared::common::ffp_state::get().on_reset();
		rt_tracker::on_reset();
		if (auto* s = skinning::get()) s->on_reset();
		shared::common::g_shader_cache.clear_cache();
		tex_addons::init_texture_addons(true);
		ImGui_ImplDX9_InvalidateDeviceObjects();
		perf::on_reset();
		if (lookdev_passthrough())
			lookdev_capture::on_reset();
		follow_game_resolution(pPresentationParameters, "Reset");
		const auto hr = m_pIDirect3DDevice9->Reset(pPresentationParameters);
		shared::common::log("Resolution", std::format("Reset result {:#x}", static_cast<uint32_t>(hr)));
		if (FAILED(hr))
		{
			// The game polls TestCooperativeLevel and retries; nothing DEFAULT-pool can be
			// created until a Reset succeeds.
			crash_context::note("device Reset failed hr=%08X", hr);
			return hr;
		}
		tex_addons::init_texture_addons();
		ImGui_ImplDX9_CreateDeviceObjects();
		return hr;
	}

	namespace
	{
		// Per frame, for the hitch log in Present: what the game created between two Presents.
		struct frame_creates { unsigned textures = 0, cubes = 0, vbs = 0, ibs = 0, vs = 0, ps = 0, decls = 0; double create_ms = 0.0; };
		frame_creates s_creates;
		double s_last_present_call_ms = 0.0;
		struct create_timer
		{
			LARGE_INTEGER start {};
			create_timer() { QueryPerformanceCounter(&start); }
			~create_timer()
			{
				static LARGE_INTEGER freq {};
				if (!freq.QuadPart)
					QueryPerformanceFrequency(&freq);
				LARGE_INTEGER now {};
				QueryPerformanceCounter(&now);
				s_creates.create_ms += static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
			}
		};
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
		// A frame presented over [Diagnostics] HitchMs (300) after the last one is a hitch the player sees: logged with the wall clock
		// (the remix log's times), the frame count, how long the last Present call blocked (Remix's side) against the rest (the game and
		// the proxy between Presents), and what the game created in between, to match against what loaded around it.
		{
			static const double hitch_ms = shared::common::config::get().get_float("Diagnostics", "HitchMs", 300.0f);
			static LARGE_INTEGER s_last {}, s_freq {};
			LARGE_INTEGER now {};
			QueryPerformanceCounter(&now);
			if (!s_freq.QuadPart)
				QueryPerformanceFrequency(&s_freq);
			if (s_last.QuadPart)
			{
				const double ms = static_cast<double>(now.QuadPart - s_last.QuadPart) * 1000.0 / static_cast<double>(s_freq.QuadPart);
				if (ms > hitch_ms)
				{
					SYSTEMTIME t {};
					GetLocalTime(&t);
					const auto& c = s_creates;
					shared::common::log("Hitch", std::format("{:02}:{:02}:{:02}.{:03} frame {}: {:.0f} ms since the last Present (that Present {:.0f} ms, "
						"the rest {:.0f} ms; creates {:.0f} ms: tex {} cube {} vb {} ib {} vs {} ps {} decl {}; draws {})",
						t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, ffp.frame_count(), ms, s_last_present_call_ms, ms - s_last_present_call_ms,
						c.create_ms, c.textures, c.cubes, c.vbs, c.ibs, c.vs, c.ps, c.decls, ffp.draw_call_count()));
				}
			}
			s_last = now;
			s_creates = {};
		}
		rt_tracker::on_present();
		perf::on_present();
		shared::common::ffp_prof::on_present();
		game::render_toggles::on_present();
		ffp.on_present();
		{
			const long long limiter_start = shared::common::ffp_prof::profiling() ? split_now() : 0;
			game::fps_unlock::on_present();
			if (limiter_start)
				s_split.limiter_ticks += split_now() - limiter_start;
		}
		const long long present_start = shared::common::ffp_prof::profiling() ? split_now() : 0;
		LARGE_INTEGER call_start {}, call_end {}, call_freq {};
		QueryPerformanceCounter(&call_start);
		auto hr = m_pIDirect3DDevice9->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
		QueryPerformanceCounter(&call_end);
		QueryPerformanceFrequency(&call_freq);
		s_last_present_call_ms = static_cast<double>(call_end.QuadPart - call_start.QuadPart) * 1000.0 / static_cast<double>(call_freq.QuadPart);
		split_on_present(present_start ? split_now() - present_start : 0);
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
		s_creates.textures++;
		const create_timer creates_timer;
		TRACE_IF_ACTIVE(trace_CreateTexture, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
		const auto& res = shared::common::config::get().resolution;
		const UINT asked_width = Width, asked_height = Height;
		const D3DFORMAT asked_format = Format;
		bool ui_target = false;
		LONG ui_width = static_cast<LONG>(Width), ui_height = static_cast<LONG>(Height);
		// Only render and depth textures: the game fills the others (the 1280x720 video frames) itself at the size it
		// asked for, and writes past the end of a smaller one.
		if (res.render_width > 0 && res.render_height > 0 && Pool == D3DPOOL_DEFAULT && (Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)))
		{
			if ((Width == 1024 && Height == 720) || res.was_render_size(Width, Height))
			{
				shared::common::log("Resolution", std::format("CreateTexture override {}x{} -> {}x{} (render)", Width, Height, res.render_width, res.render_height));
				Width = res.render_width;
				Height = res.render_height;
			}
			else if ((Width == 1280 && Height == 720) || res.was_present_size(Width, Height))
			{
				shared::common::log("Resolution", std::format("CreateTexture override {}x{} -> {}x{} (present)", Width, Height, res.present_width, res.present_height));
				// The game's own 1280x720 request. After a Reset it asks again at the size it was given: s_ui_targets_before_reset.
				ui_target = (Usage & D3DUSAGE_RENDERTARGET) && Width == 1280 && Height == 720 &&
					(res.present_width != Width || res.present_height != Height);
				Width = res.present_width;
				Height = res.present_height;
			}
			if (!ui_target && (Usage & D3DUSAGE_RENDERTARGET))
			{
				const auto it = std::find_if(s_ui_targets_before_reset.begin(), s_ui_targets_before_reset.end(), [&](const remembered_viewport& v)
					{ return v.made_width == asked_width && v.made_height == asked_height && v.asked_format == asked_format; });
				if (it != s_ui_targets_before_reset.end())
				{
					ui_target = Width != static_cast<UINT>(it->ui_width) || Height != static_cast<UINT>(it->ui_height);
					ui_width = it->ui_width;
					ui_height = it->ui_height;
					shared::common::log("Resolution", std::format("CreateTexture {}x{}: the UI target again after the Reset, scissors scale from {}x{}",
						asked_width, asked_height, ui_width, ui_height));
					s_ui_targets_before_reset.erase(it);
				}
			}
		}
		// HDR10 output: the game's 8-bit render textures of the back buffer's size (its scene target, which Remix's
		// frame is composited into) hold 16-bit float, so what is brighter than SDR white survives to the back buffer.
		if (shared::common::config::get().hdr.enabled && Pool == D3DPOOL_DEFAULT && (Usage & D3DUSAGE_RENDERTARGET) &&
			(Format == D3DFMT_A8R8G8B8 || Format == D3DFMT_X8R8G8B8))
		{
			UINT bb_w = 0, bb_h = 0;
			if (back_buffer_size(m_pIDirect3DDevice9, bb_w, bb_h) && Width == bb_w && Height == bb_h)
			{
				shared::common::log("HDR", std::format("CreateTexture {}x{} {} render target -> A16B16G16R16F", Width, Height, format_tag(Format)));
				Format = D3DFMT_A16B16G16R16F;
			}
		}
		const crash_context::stopwatch sw;
		const auto hr = m_pIDirect3DDevice9->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
		if (FAILED(hr))
			crash_context::note("Tex2D %ux%u L%u f=%s p=%u t=%u hr=%08X", Width, Height, Levels, format_tag(Format).c_str(), Pool, sw.ms(), hr);
		else if (ppTexture && *ppTexture)
		{
			// Its scissors scale from the size the game asked for; any other render texture at a freed one's address does not.
			if (Usage & D3DUSAGE_RENDERTARGET)
			{
				IDirect3DSurface9* surface = nullptr;
				if (SUCCEEDED((*ppTexture)->GetSurfaceLevel(0, &surface)) && surface)
				{
					if (ui_target)
					{
						auto& v = s_remembered_viewports[surface];
						v = {};
						v.ui_width = ui_width;
						v.ui_height = ui_height;
						v.created_as_ui = true;
						v.made_width = Width;
						v.made_height = Height;
						v.asked_format = asked_format;
					}
					else
						s_remembered_viewports.erase(surface);
					surface->Release();
				}
			}
			game::texture_names::on_device_created(*ppTexture);  // D3DX names its textures after this returns
			shared::common::ffp_state::get().on_texture_created(*ppTexture);
		}
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
		s_creates.cubes++;
		const create_timer creates_timer;
		TRACE_IF_ACTIVE(trace_CreateCubeTexture, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
		const crash_context::stopwatch sw;
		const auto hr = m_pIDirect3DDevice9->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
		if (SUCCEEDED(hr) && ppCubeTexture && *ppCubeTexture)
			shared::common::ffp_state::get().note_cube(*ppCubeTexture);
		if (FAILED(hr))
			crash_context::note("CubeTex e=%u L%u f=%s p=%u t=%u hr=%08X", EdgeLength, Levels, format_tag(Format).c_str(), Pool, sw.ms(), hr);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle)
	{
		s_creates.vbs++;
		const create_timer creates_timer;
		TRACE_IF_ACTIVE(trace_CreateVertexBuffer, Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
		const crash_context::stopwatch sw;
		const auto hr = m_pIDirect3DDevice9->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
		if (FAILED(hr))
			crash_context::note("VB len=%u u=%X p=%u t=%u hr=%08X", Length, Usage, Pool, sw.ms(), hr);
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle)
	{
		s_creates.ibs++;
		const create_timer creates_timer;
		TRACE_IF_ACTIVE(trace_CreateIndexBuffer, Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
		const crash_context::stopwatch sw;
		const auto hr = m_pIDirect3DDevice9->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
		if (FAILED(hr))
			crash_context::note("IB len=%u u=%X p=%u t=%u hr=%08X", Length, Usage, Pool, sw.ms(), hr);
		else if (ppIndexBuffer && *ppIndexBuffer)
		{
			game::mesh_tables::on_index_buffer_created(*ppIndexBuffer);
		}
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
	{
		TRACE_IF_ACTIVE(trace_CreateRenderTarget, Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
		const auto& res = shared::common::config::get().resolution;
		if (res.render_width > 0 && res.render_height > 0 && ((Width == 1024 && Height == 720) || (shared::globals::lookdev_host && res.was_render_size(Width, Height))))
		{
			shared::common::log("Resolution", std::format("CreateRenderTarget override {}x{} -> {}x{}", Width, Height, res.render_width, res.render_height));
			Width = res.render_width;
			Height = res.render_height;
		}
		const crash_context::stopwatch sw;
		const auto hr = m_pIDirect3DDevice9->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
		if (FAILED(hr))
			crash_context::note("RT %ux%u f=%s t=%u hr=%08X", Width, Height, format_tag(Format).c_str(), sw.ms(), hr);
		else if (ppSurface && *ppSurface)
			s_remembered_viewports.erase(*ppSurface); // a new target at a freed one's address
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
	{
		TRACE_IF_ACTIVE(trace_CreateDepthStencilSurface, Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle);
		const auto& res = shared::common::config::get().resolution;
		if (res.render_width > 0 && res.render_height > 0 && ((Width == 1024 && Height == 720) || (shared::globals::lookdev_host && res.was_render_size(Width, Height))))
		{
			shared::common::log("Resolution", std::format("CreateDepthStencilSurface override {}x{} -> {}x{}", Width, Height, res.render_width, res.render_height));
			Width = res.render_width;
			Height = res.render_height;
		}
		const depth_pool_key key { m_pIDirect3DDevice9, Width, Height, Format, MultiSample, MultisampleQuality, Discard };
		const bool pool = shared::common::config::get().ffp_steps.pool_depth_surfaces && ppSurface && !pSharedHandle && !lookdev_passthrough();
		if (pool)
		{
			if (auto* pooled = take_pooled_depth(key))
			{
				*ppSurface = pooled;
				return D3D_OK;
			}
		}
		const crash_context::stopwatch sw;
		const auto hr = m_pIDirect3DDevice9->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle);
		if (FAILED(hr))
			crash_context::note("DS %ux%u f=%s t=%u hr=%08X", Width, Height, format_tag(Format).c_str(), sw.ms(), hr);
		else if (pool && *ppSurface && (s_depth_pool.size() < kDepthPoolMax || make_depth_pool_room()))
		{
			(*ppSurface)->AddRef();
			s_depth_pool.emplace_back(key, *ppSurface);
		}
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
		auto* native = shared::common::ffp_state::get().menu_native_surface();
		if (native && RenderTargetIndex == 0)
			effective = native;
		// The private menu target's marker (ffp_state::kMenuSceneMarker) while it is bound, cleared otherwise; each draw into it
		// sets it again (ffp_state::engage).
		if (RenderTargetIndex == 0)
			m_pIDirect3DDevice9->SetSamplerState(15, D3DSAMP_BORDERCOLOR, native ? shared::common::ffp_state::kMenuSceneMarker : 0);
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
		UINT target_width, target_height;
		if (pViewport && !pViewport->X && !pViewport->Y &&
			remembered_size(m_pIDirect3DDevice9, pViewport->Width, pViewport->Height, target_width, target_height))
		{
			D3DVIEWPORT9 viewport = *pViewport;
			viewport.Width = target_width;
			viewport.Height = target_height;
			if (auto* target = current_target(m_pIDirect3DDevice9))
				s_remembered_viewports.try_emplace(target);
			return m_pIDirect3DDevice9->SetViewport(&viewport);
		}
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
		if (auto* native = shared::common::ffp_state::get().menu_native_texture_for(pTexture); native && !lookdev_passthrough())
			return m_pIDirect3DDevice9->SetTexture(Stage, native);
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
		UINT target_width, target_height;
		if (pRect && !pRect->left && !pRect->top && pRect->right > 0 && pRect->bottom > 0 &&
			remembered_size(m_pIDirect3DDevice9, static_cast<UINT>(pRect->right), static_cast<UINT>(pRect->bottom), target_width, target_height))
		{
			const RECT rect { 0, 0, static_cast<LONG>(target_width), static_cast<LONG>(target_height) };
			return m_pIDirect3DDevice9->SetScissorRect(&rect);
		}
		if (RECT scaled; pRect && scaled_scissor(m_pIDirect3DDevice9, *pRect, scaled))
			return m_pIDirect3DDevice9->SetScissorRect(&scaled);
		if (pRect)
			note_unscaled_ui_scissor(m_pIDirect3DDevice9, *pRect);
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
		draw_split_scope split_scope;
		if (lookdev_passthrough())
		{
			// An F9 capture's reference frame is drawn in the vanilla view with the proxy out of the way: the distortion would
			// read the same black frame copy there (drop_distortion_in_vanilla_view).
			if (drop_distortion_in_vanilla_view())
				return S_OK;
				return m_pIDirect3DDevice9->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
		}
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
		draw_split_scope split_scope;
		if (lookdev_passthrough())
		{
			// An F9 capture's reference frame is drawn in the vanilla view with the proxy out of the way: the distortion would
			// read the same black frame copy there (drop_distortion_in_vanilla_view).
			if (drop_distortion_in_vanilla_view())
				return S_OK;
				return m_pIDirect3DDevice9->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
		}
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
		draw_split_scope split_scope;
		if (comp::game::aspect_ratio::active() && PrimitiveType == D3DPT_TRIANGLESTRIP)
			pVertexStreamZeroData = comp::game::aspect_ratio::final_quad(pVertexStreamZeroData, VertexStreamZeroStride, PrimitiveCount + 2, shared::common::ffp_state::get().last_decl());
		comp::game::hud_scale::draw_scope hud_scale_scope(m_pIDirect3DDevice9, pVertexStreamZeroData, VertexStreamZeroStride, up_vertex_count(PrimitiveType, PrimitiveCount));
		if (lookdev_passthrough())
		{
			// An F9 capture's reference frame is drawn in the vanilla view with the proxy out of the way: the distortion would
			// read the same black frame copy there (drop_distortion_in_vanilla_view).
			if (drop_distortion_in_vanilla_view())
				return S_OK;
				return m_pIDirect3DDevice9->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
		}
		// UP draws (sprites, UI, post passes) carry no haze set, self-lit ambient or lightmap of their own.
		comp::game::atmosphere::tag_haze_set(m_pIDirect3DDevice9, false, nullptr, nullptr, 0);
		comp::game::atmosphere::tag_self_lit(m_pIDirect3DDevice9, false, nullptr);
		comp::game::atmosphere::tag_view_fade(m_pIDirect3DDevice9, false, nullptr);
		comp::game::env_reflection::tag(m_pIDirect3DDevice9, false, nullptr);
		comp::game::lightmap_sun::untag(m_pIDirect3DDevice9);
		if (auto& ffp = shared::common::ffp_state::get(); ffp.cur_ps_light_flags() & ffp.PS_GRADE)
			comp::game::atmosphere::observe_grade(ffp.ps_const_data(), m_pIDirect3DDevice9);
		else if (ffp.cur_ps_light_flags() & ffp.PS_LIGHT_SHAFT)
			comp::game::atmosphere::observe_light_shaft(ffp.ps_const_data(), ffp.frame_count());
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
		// Fullscreen quad post-FX pass detection (stride=24 = pos+uv, 2 tris = single quad). The post passes
		// are triangle fans; a triangle list of this shape is a font draw holding one glyph or button icon.
		if (PrimitiveType == D3DPT_TRIANGLEFAN && PrimitiveCount <= 2 && VertexStreamZeroStride == 24)
		{
			auto& c = shared::common::config::get();
			int idx = s_postfx_pass_idx++;
			if (c.ffp.skip_postfx && (c.postfx.skip_passes.empty() || c.postfx.skip_passes.contains(idx)))
				return S_OK;
		}
		composite_before_backbuffer(m_pIDirect3DDevice9, VertexStreamZeroStride);
		// HDR10 output: the brightness LUT pass would clip the composited frame's headroom.
		if (hdr_final_pass_copy(m_pIDirect3DDevice9))
			return S_OK;
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
		draw_split_scope split_scope;
		// A path traced menu view has no alpha of its own: the quad adds its colour (over black where nothing was hit).
		if (!lookdev_passthrough() && shared::common::ffp_state::get().cur_texture_is_menu_path_traced())
		{
			// Only off 16:9: these quads never had a HUD scale of their own.
			std::optional<comp::game::hud_scale::draw_scope> menu_fit;
			if (comp::game::aspect_ratio::active())
				menu_fit.emplace(m_pIDirect3DDevice9, static_cast<const char*>(pVertexStreamZeroData) + size_t(MinVertexIndex) * VertexStreamZeroStride, VertexStreamZeroStride, NumVertices);
			shared::common::ffp_state::get().note_menu_quad(m_pIDirect3DDevice9, pVertexStreamZeroData, VertexStreamZeroStride, NumVertices);
			DWORD src_blend = D3DBLEND_SRCALPHA;
			m_pIDirect3DDevice9->GetRenderState(D3DRS_SRCBLEND, &src_blend);
			m_pIDirect3DDevice9->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
			// A view's quad: its S2 texture coordinates (4096 = 1), moved to its slice of the frame (a row's cell, or the single
			// view centred in the wider target).
			auto& menu_ffp = shared::common::ffp_state::get();
			short slice[4 * 6];
			const void* quad_vertices = pVertexStreamZeroData;
			float u0, v0, du, dv;
			if (VertexStreamZeroStride == 12 && NumVertices <= 4 && menu_ffp.menu_slice_uv(menu_ffp.cur_texture(0), u0, v0, du, dv))
			{
				std::memcpy(slice, pVertexStreamZeroData, 12 * NumVertices);
				for (UINT v = 0; v < NumVertices; v++)
				{
					slice[v * 6 + 4] = static_cast<short>(std::lround(4096.0f * (u0 + du * slice[v * 6 + 4] / 4096.0f)));
					slice[v * 6 + 5] = static_cast<short>(std::lround(4096.0f * (v0 + dv * slice[v * 6 + 5] / 4096.0f)));
				}
				quad_vertices = slice;
			}
			const auto hr = m_pIDirect3DDevice9->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, quad_vertices, VertexStreamZeroStride);
			m_pIDirect3DDevice9->SetRenderState(D3DRS_SRCBLEND, src_blend);
			return hr;
		}
		comp::game::hud_scale::draw_scope hud_scale_scope(m_pIDirect3DDevice9, static_cast<const char*>(pVertexStreamZeroData) + size_t(MinVertexIndex) * VertexStreamZeroStride, VertexStreamZeroStride, NumVertices);
		if (lookdev_passthrough())
		{
			// An F9 capture's reference frame is drawn in the vanilla view with the proxy out of the way: the distortion would
			// read the same black frame copy there (drop_distortion_in_vanilla_view).
			if (drop_distortion_in_vanilla_view())
				return S_OK;
				return m_pIDirect3DDevice9->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
		}
		comp::game::atmosphere::tag_haze_set(m_pIDirect3DDevice9, false, nullptr, nullptr, 0);
		comp::game::atmosphere::tag_self_lit(m_pIDirect3DDevice9, false, nullptr);
		comp::game::atmosphere::tag_view_fade(m_pIDirect3DDevice9, false, nullptr);
		comp::game::env_reflection::tag(m_pIDirect3DDevice9, false, nullptr);
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
		s_creates.decls++;
		const create_timer creates_timer;
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
		s_creates.vs++;
		const create_timer creates_timer;
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
		if (comp::game::aspect_ratio::active())
			pConstantData = comp::game::aspect_ratio::camera_constants(StartRegister, pConstantData, Vector4fCount);
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
		s_creates.ps++;
		const create_timer creates_timer;
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

	namespace
	{
		// An occlusion query whose sample count is reported at the game's native 1024x720.
		//
		// DS1's only occlusion query is the lens flare's sun test (LensFlareDrawEntity, 0x00FD3A20): a
		// 24x24 reference-pixel quad at the sun, visibility = samples / 460.8, and a count of 1.2x that
		// (691) or more is rejected as bogus, keeping the last accepted value. With [Resolution] at
		// 3840x2160 the same quad passes 5184 samples, so a fully visible sun was always rejected: the
		// flare latched at whatever partial count last got through (often full strength) and stayed,
		// through walls, until the sun left the screen.
		class native_scale_query final : public IDirect3DQuery9
		{
		public:
			native_scale_query(IDirect3DQuery9* query, IDirect3DDevice9* device)
				: query_(query), device_(device) {}

			HRESULT __stdcall QueryInterface(REFIID riid, void** ppv) override
			{
				if (!ppv)
					return E_POINTER;
				if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DQuery9))
				{
					*ppv = this;
					AddRef();
					return S_OK;
				}
				return query_->QueryInterface(riid, ppv);
			}

			ULONG __stdcall AddRef() override { return ++refs_; }

			ULONG __stdcall Release() override
			{
				const ULONG refs = --refs_;
				if (!refs)
				{
					query_->Release();
					delete this;
				}
				return refs;
			}

			HRESULT __stdcall GetDevice(IDirect3DDevice9** ppDevice) override
			{
				if (!ppDevice)
					return D3DERR_INVALIDCALL;
				device_->AddRef();
				*ppDevice = device_;
				return D3D_OK;
			}

			D3DQUERYTYPE __stdcall GetType() override { return query_->GetType(); }
			DWORD __stdcall GetDataSize() override { return query_->GetDataSize(); }
			HRESULT __stdcall Issue(DWORD flags) override { return query_->Issue(flags); }

			HRESULT __stdcall GetData(void* data, DWORD size, DWORD flags) override
			{
				const HRESULT hr = query_->GetData(data, size, flags);
				if (hr == S_OK && data && size >= sizeof(DWORD))
				{
					DWORD& samples = *static_cast<DWORD*>(data);
					const DWORD raw = samples;
					// Now's, not the size when the query was made: a Reset can change it.
					const auto& res = shared::common::config::get().resolution;
					const double scale = res.render_width && res.render_height
						? (1024.0 * 720.0) / (double(res.render_width) * double(res.render_height)) : 1.0;
					samples = static_cast<DWORD>(raw * scale + 0.5);
					// A few results for the log: a visible sun reads ~461 here, a hidden one 0.
					static std::atomic<int> logged{ 0 };
					if (raw && logged.fetch_add(1) < 8)
						shared::common::log("Query", std::format("occlusion samples {} -> {} at 1024x720", raw, samples));
				}
				return hr;
			}

		private:
			IDirect3DQuery9* query_;
			IDirect3DDevice9* device_;
			std::atomic<ULONG> refs_{ 1 };
		};
	}

	HRESULT d3d9ex::D3D9Device::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery)
	{
		TRACE_IF_ACTIVE(trace_CreateQuery, Type, ppQuery);
		const HRESULT hr = m_pIDirect3DDevice9->CreateQuery(Type, ppQuery);

		// ppQuery null only asks whether the type is supported.
		if (SUCCEEDED(hr) && Type == D3DQUERYTYPE_OCCLUSION && ppQuery && *ppQuery)
			*ppQuery = new native_scale_query(*ppQuery, this);
		return hr;
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
		follow_game_resolution(pPresentationParameters, "CreateDevice");
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
		follow_game_resolution(pPresentationParameters, "CreateDeviceEx");
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
	comp::game::aspect_ratio::before_game_window();
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
