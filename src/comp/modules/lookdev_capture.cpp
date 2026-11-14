// Lookdev frame capture: records one presented frame from the real device.
// See lookdev_capture.hpp for the overview, capture_format.hpp for the file.
#include "std_include.hpp"
#include "lookdev_capture.hpp"

#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/lookdev/capture_format.hpp"
#include "../game/atmosphere.hpp"
#include "../game/estus.hpp"
#include "../game/camera.hpp"
#include "../game/map_lights.hpp"
#include "../game/point_lights.hpp"
#include "../game/ffx_instances.hpp"
#include "../game/ffx_lights.hpp"
#include "../game/fx_draw_owner.hpp"
#include "../game/texture_names.hpp"
#include "../game/vanilla_view.hpp"
#include "../game/input_trace.hpp"
#include "../game/free_cam.hpp"

#include "shared/common/dinput_hook_v2.hpp"
#include <Xinput.h>
#include <intrin.h>

#include <atomic>
#include <cctype>
#include <optional>
#include <tuple>

namespace comp
{
	namespace
	{
		using namespace lookdev;

		// IDirect3DDevice9 vtable slots (d3d9.h declaration order)
		constexpr int VT_PRESENT = 17;
		constexpr int VT_UPDATE_SURFACE = 30;
		constexpr int VT_UPDATE_TEXTURE = 31;
		constexpr int VT_STRETCH_RECT = 34;
		constexpr int VT_COLOR_FILL = 35;
		constexpr int VT_BEGIN_SCENE = 41;
		constexpr int VT_END_SCENE = 42;
		constexpr int VT_CLEAR = 43;
		constexpr int VT_DRAW_PRIMITIVE = 81;
		constexpr int VT_DRAW_INDEXED_PRIMITIVE = 82;
		constexpr int VT_DRAW_PRIMITIVE_UP = 83;
		constexpr int VT_DRAW_INDEXED_PRIMITIVE_UP = 84;
		constexpr int VT_CREATE_TEXTURE = 23;
		constexpr int VT_CREATE_VOLUME_TEXTURE = 24;
		constexpr int VT_CREATE_CUBE_TEXTURE = 25;
		constexpr int VT_CREATE_VERTEX_BUFFER = 26;
		constexpr int VT_CREATE_INDEX_BUFFER = 27;
		constexpr int VT_CREATE_RENDER_TARGET = 28;
		constexpr int VT_CREATE_DEPTH_STENCIL = 29;
		constexpr int VT_CREATE_OFFSCREEN_PLAIN = 36;
		constexpr int VT_CREATE_VERTEX_DECL = 86;
		constexpr int VT_CREATE_VERTEX_SHADER = 91;
		constexpr int VT_CREATE_PIXEL_SHADER = 106;
		// IDirect3DVertexBuffer9 / IDirect3DIndexBuffer9
		constexpr int VT_BUFFER_LOCK = 11;
		constexpr int VT_BUFFER_UNLOCK = 12;
		// IDirect3DTexture9
		constexpr int VT_TEXTURE_UNLOCK_RECT = 20;

		// Every settable D3DRENDERSTATETYPE except DEBUGMONITORTOKEN.
		constexpr DWORD RENDER_STATES[] = {
			7, 8, 9, 14, 15, 16, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 34, 35, 36, 37, 38, 48,
			52, 53, 54, 55, 56, 57, 58, 59, 60,
			128, 129, 130, 131, 132, 133, 134, 135,
			136, 137, 139, 140, 141, 142, 143, 145, 146, 147, 148, 151, 152,
			154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 166, 167, 168, 170, 171, 172, 173, 174, 175, 176,
			178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195,
			198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209,
		};
		constexpr DWORD TEXTURE_STAGE_STATES[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 22, 23, 24, 26, 27, 28, 32 };
		constexpr DWORD TRANSFORMS[] = { D3DTS_VIEW, D3DTS_PROJECTION,
			D3DTS_TEXTURE0, D3DTS_TEXTURE1, D3DTS_TEXTURE2, D3DTS_TEXTURE3,
			D3DTS_TEXTURE4, D3DTS_TEXTURE5, D3DTS_TEXTURE6, D3DTS_TEXTURE7,
			256, 257, 258, 259 }; // D3DTS_WORLDMATRIX(0..3)

		// 16 pixel samplers, then the 4 vertex texture samplers.
		constexpr DWORD SAMPLER_SLOTS = 20;
		DWORD sampler_index(DWORD slot) { return slot < 16 ? slot : D3DVERTEXTEXTURESAMPLER0 + (slot - 16); }

		constexpr DWORD MAX_RENDER_TARGETS = 4;
		constexpr DWORD MAX_STREAMS = 16;
		constexpr int LOCK_SLOTS = 6;

		uint64_t id_of(const void* p) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)); }

		UINT primitive_vertex_count(D3DPRIMITIVETYPE type, UINT count)
		{
			switch (type)
			{
			case D3DPT_POINTLIST: return count;
			case D3DPT_LINELIST: return count * 2;
			case D3DPT_LINESTRIP: return count + 1;
			case D3DPT_TRIANGLELIST: return count * 3;
			case D3DPT_TRIANGLESTRIP:
			case D3DPT_TRIANGLEFAN: return count + 2;
			default: return 0;
			}
		}

		rect to_rect(const RECT& r) { return { r.left, r.top, r.right, r.bottom }; }

		// ---- writer: a capture file, or the live stream's ring ----

		// Set while a capture or stream session records. Cleared early when a
		// stream frame overflows the ring: the rest of that frame passes through.
		std::atomic<bool> s_capturing = false;

		// A 64-bit position shared with the host. Both are 32-bit processes, where
		// a plain 64-bit load can tear.
		int64_t load64(volatile int64_t* p) { return InterlockedCompareExchange64(p, 0, 0); }

		// The game end of the live stream (capture_format.hpp, stream_header).
		// Never waits for the host: a frame is written past write_pos into the space
		// the host has freed and published at Present. A frame that does not fit
		// is dropped unpublished (overflow); the caller then ends the session.
		struct ring_sink
		{
			HANDLE mapping = nullptr;
			stream_header* hdr = nullptr;
			uint8_t* data = nullptr;
			uint32_t size = 0;
			int64_t published = 0;    // write_pos: everything before it is the host's to read
			int64_t pos = 0;          // written; the current frame is [published, pos)
			int64_t limit = 0;        // read_pos + size when last read: writes below it fit
			uint32_t epoch = 0;       // consumer_epoch this session serves
			bool overflow = false;    // the current frame did not fit

			void commit()
			{
				MemoryBarrier();
				published = pos;
				InterlockedExchange64(&hdr->write_pos, pos);
			}

			// Forgets the unpublished part of the frame.
			void rollback()
			{
				pos = published;
				overflow = false;
			}

			bool write(const void* src, size_t n)
			{
				if (overflow)
					return false;
				if (pos + int64_t(n) > limit)
				{
					// read_pos only grows, so the cached limit is safe; re-read it only
					// when it is not enough
					limit = load64(&hdr->read_pos) + size;
					if (pos + int64_t(n) > limit)
					{
						overflow = true;
						return false;
					}
				}
				const auto* p = static_cast<const uint8_t*>(src);
				const size_t offset = static_cast<size_t>(pos % size);
				const size_t first = std::min<size_t>(n, size - offset);
				std::memcpy(data + offset, p, first);
				if (first < n)
					std::memcpy(data, p + first, n - first);
				pos += n;
				return true;
			}
		};

		struct writer
		{
			FILE* file = nullptr;
			ring_sink* ring = nullptr;
			uint64_t bytes = 0;

			void raw(const void* data, size_t size)
			{
				if (!size) return;
				if (!ring) fwrite(data, 1, size, file);
				else if (!ring->write(data, size)) s_capturing = false; // dropped at Present
				bytes += size;
			}

			void begin(uint32_t type, size_t payload)
			{
				const record_header h { type, static_cast<uint32_t>(payload) };
				raw(&h, sizeof(h));
			}

			void rec(uint32_t type)
			{
				begin(type, 0);
			}

			template <class T>
			void rec(uint32_t type, const T& payload, const void* extra = nullptr, size_t extra_size = 0)
			{
				begin(type, sizeof(T) + extra_size);
				raw(&payload, sizeof(T));
				raw(extra, extra_size);
			}
		};

		// ---- capture state ----

		// How an object's contents are taken.
		enum contents_kind : uint8_t
		{
			CONTENTS_NONE,     // nothing to send: depth, unreadable, or (streams) a render target too large to read back
			CONTENTS_LOCK,     // a CPU-side copy (managed / system memory / dynamic)
			CONTENTS_READBACK, // GetRenderTargetData, or a lock on a default-pool buffer: waits for the GPU
		};

		struct texture_info
		{
			uint32_t kind; // REC_TEXTURE / REC_CUBE_TEXTURE / REC_VOLUME_TEXTURE
			D3DFORMAT format;
			D3DPOOL pool;
			DWORD usage;
			UINT levels;
			uint64_t bytes = 0;           // all faces and levels, packed
			uint8_t contents = CONTENTS_NONE;
			bool pending = false;         // stream: initial contents not sent yet
		};

		struct surface_info
		{
			IDirect3DBaseTexture9* texture = nullptr; // set for texture levels
			UINT face = 0;
			UINT level = 0;
			// standalone surfaces and the back buffer (levels go through their texture)
			uint64_t bytes = 0;
			uint8_t contents = CONTENTS_NONE;
			bool pending = false;
		};

		struct buffer_info
		{
			UINT size = 0;
			uint64_t bytes = 0;
			uint8_t contents = CONTENTS_LOCK;
			bool pending = false;
		};

		struct pending_lock
		{
			UINT offset;
			UINT size;
			const void* data;
		};

		template <class T>
		bool update(std::optional<T>& slot, const T& value)
		{
			if (slot && std::memcmp(&*slot, &value, sizeof(T)) == 0)
				return false;
			slot = value;
			return true;
		}

		struct const_shadow
		{
			std::vector<uint8_t> bytes;
			bool valid = false;
		};

		// Last value written for every piece of device state.
		struct shadow_state
		{
			std::optional<DWORD> rs[256];
			std::optional<DWORD> ss[SAMPLER_SLOTS][14];
			std::optional<DWORD> tss[8][33];
			std::optional<uint64_t> texture[SAMPLER_SLOTS];
			std::optional<uint64_t> vs, ps, decl;
			std::optional<DWORD> fvf;
			const_shadow vsf, psf, vsi, psi, vsb, psb;
			std::optional<stream_source> stream[MAX_STREAMS];
			std::optional<DWORD> stream_freq[MAX_STREAMS];
			std::optional<uint64_t> indices;
			std::optional<uint64_t> rt[MAX_RENDER_TARGETS];
			std::optional<uint64_t> ds;
			std::optional<D3DVIEWPORT9> viewport;
			std::optional<rect> scissor;
			std::optional<clip_plane> clip[6];
			std::optional<transform> xform[std::size(TRANSFORMS)];
			std::optional<D3DMATERIAL9> material;
			std::optional<DWORD> light_enable[8];
			std::optional<light> lights[8];
		};

		struct capture
		{
			IDirect3DDevice9* dev = nullptr;
			IDirect3DSurface9* backbuffer = nullptr;
			writer w;
			// Live stream session rather than a one-frame file: it holds no
			// references (the game frees resources as usual; a reused address is
			// sent as REC_DESTROY) and keeps its state from frame to frame.
			bool streaming = false;
			uint64_t contents = 0; // bytes of textures and buffers sent, which the host holds until the address is reused
			uint32_t frames = 0;
			std::string name;
			std::string part_path;
			std::string final_path;
			uint32_t vs_const_f = 256;
			uint32_t ps_const_f = 224;

			std::vector<IUnknown*> refs; // one reference on everything recorded
			std::unordered_map<const void*, texture_info> textures;
			std::unordered_map<const void*, surface_info> surfaces;
			std::unordered_map<const void*, buffer_info> buffers;
			std::unordered_set<const void*> objects;       // shaders, declarations
			std::unordered_map<const void*, pending_lock> locks;
			std::map<std::tuple<UINT, UINT, D3DFORMAT>, IDirect3DSurface9*> staging;

			shadow_state shadow;
			std::vector<uint8_t> scratch;
			bool game_state_written = false; // this frame's REC_GAME_FRAME / REC_GAME_LIGHTS

			// Live streams pace initial contents (pace()). Reset at every Present.
			int64_t budget = 0;          // content bytes left this frame
			int64_t sent = 0;            // content bytes sent this frame
			uint32_t pending = 0;        // objects whose initial contents are still to come
			bool frame_incomplete = false; // this frame used one of them
			bool in_sync = false;        // a complete frame has been published
			uint64_t contents_max = ~0ull; // larger objects are never sent (they might never fit in the ring)
			// Streams: whether this frame records its commands and state, or only
			// what changes resources (a "no-draw" frame, while the host is behind).
			// Captures always draw.
			bool draw_frame = true;
			uint32_t drawn_frames = 0;

			struct
			{
				uint32_t draws = 0, clears = 0, copies = 0;
				uint32_t textures = 0, buffers = 0, buffer_updates = 0, texture_updates = 0;
				uint32_t unreadable = 0, depth = 0, msaa = 0, unknown_format = 0;
				uint32_t not_sent = 0, deferred = 0;
			} stats;

			std::chrono::steady_clock::time_point started;
		};

		std::recursive_mutex s_mutex;
		capture* s_cap = nullptr;
		thread_local bool t_internal = false; // our own device/resource calls

		struct internal_scope
		{
			bool prev;
			internal_scope() : prev(t_internal) { t_internal = true; }
			~internal_scope() { t_internal = prev; }
		};

		uint64_t s_frame_index = 0;
		bool s_request = false;
		std::string s_request_name;
		std::string s_last_capture;                          // name of the last finished capture file
		std::chrono::steady_clock::time_point s_last_capture_time;
		ring_sink* s_ring = nullptr; // mapped live stream, while a host is attached

		// ---- hook trampolines ----

		using present_t = HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
		using update_surface_t = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const POINT*);
		using update_texture_t = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DBaseTexture9*, IDirect3DBaseTexture9*);
		using stretch_rect_t = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
		using color_fill_t = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, D3DCOLOR);
		using scene_t = HRESULT(__stdcall*)(IDirect3DDevice9*);
		using clear_t = HRESULT(__stdcall*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
		using draw_t = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
		using draw_indexed_t = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
		using draw_up_t = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
		using draw_indexed_up_t = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
		using buffer_lock_t = HRESULT(__stdcall*)(IUnknown*, UINT, UINT, void**, DWORD);
		using buffer_unlock_t = HRESULT(__stdcall*)(IUnknown*);
		using texture_unlock_t = HRESULT(__stdcall*)(IDirect3DTexture9*, UINT);

		present_t s_present_og = nullptr;
		update_surface_t s_update_surface_og = nullptr;
		update_texture_t s_update_texture_og = nullptr;
		stretch_rect_t s_stretch_rect_og = nullptr;
		color_fill_t s_color_fill_og = nullptr;
		scene_t s_begin_scene_og = nullptr;
		scene_t s_end_scene_og = nullptr;
		clear_t s_clear_og = nullptr;
		draw_t s_draw_og = nullptr;
		draw_indexed_t s_draw_indexed_og = nullptr;
		draw_up_t s_draw_up_og = nullptr;
		draw_indexed_up_t s_draw_indexed_up_og = nullptr;
		bool s_device_hooks = false;

		// Resource classes differ per pool (and VB / IB) in d3d9.dll, so each
		// distinct Lock / Unlock implementation seen gets its own slot and with it
		// its own detour instance and trampoline.
		template <class Fn>
		struct hook_slot_t
		{
			void* target = nullptr;
			Fn og = nullptr;
		};
		hook_slot_t<buffer_lock_t> s_buffer_lock_slots[LOCK_SLOTS];
		hook_slot_t<buffer_unlock_t> s_buffer_unlock_slots[LOCK_SLOTS];
		hook_slot_t<texture_unlock_t> s_texture_unlock_slots[LOCK_SLOTS];
		bool s_slots_full_logged = false;

		void log(const std::string& msg, shared::common::LOG_TYPE type = shared::common::LOG_TYPE::LOG_TYPE_DEFAULT)
		{
			shared::common::log("Lookdev", msg, type, true);
		}

		// The normal game, on the Remix bridge: one whole frame at a time (see the header).
		bool main_game()
		{
			return shared::common::config::get().remix.enabled;
		}
		std::atomic_bool s_main_frame = false;
		// The frame goes no further than the recording: no game image to take from it.
		bool s_main_drop = false;
		// The back buffer, once, for the game image.
		bool s_main_reference = false;

		// Waiting in vanilla graphics for the game's exposure (PS c135), which stood still in Remix.
		struct
		{
			bool waiting = false;
			bool switch_back = false;
			int frames = 0;
			int steady = 0;
			float exposure = 0.0f;
			std::chrono::steady_clock::time_point started;
		} s_settle;

		// Whether a requested frame can start now.
		bool main_game_ready()
		{
			if (!shared::common::config::get().lookdev.main_game_comparison)
				return true;
			auto& s = s_settle;
			if (!s.waiting)
			{
				if (game::vanilla_view::active())
					return true;
				game::vanilla_view::request_toggle();
				s = {};
				s.waiting = true;
				s.switch_back = true;
				s.started = std::chrono::steady_clock::now();
				return false;
			}
			const auto waited = std::chrono::duration<float>(std::chrono::steady_clock::now() - s.started).count();
			if (!game::vanilla_view::active())
			{
				if (waited < 2.0f)
					return false;
				s = {};
				return true;
			}
			constexpr float kSteadyChange = 0.001f;
			constexpr int kSteadyFrames = 15, kFewestFrames = 30;
			constexpr float kLongestWait = 5.0f;
			const float exposure = shared::common::ffp_state::get().ps_const_data()[135 * 4];
			s.steady = std::abs(exposure - s.exposure) <= kSteadyChange * std::abs(exposure) ? s.steady + 1 : 0;
			s.exposure = exposure;
			s.frames++;
			if ((s.frames < kFewestFrames || s.steady < kSteadyFrames) && waited < kLongestWait)
				return false;
			log(std::format("Vanilla graphics settled: {} frames, {:.1f} s, c135.x {}", s.frames, waited, exposure));
			s.waiting = false;
			return true;
		}

		void main_game_capture_over()
		{
			s_main_frame = false;
			s_main_drop = false;
			if (s_settle.switch_back)
				game::vanilla_view::request_toggle();
			s_settle = {};
		}

		bool recording(IDirect3DDevice9* dev)
		{
			return s_capturing.load(std::memory_order_relaxed) && !t_internal && s_cap && s_cap->dev == dev;
		}

		// Commands that only produce pixels (draws, clears, GPU copies) and the
		// state they read. Skipped in a stream's no-draw frames.
		bool recording_draws(IDirect3DDevice9* dev)
		{
			return recording(dev) && s_cap->draw_frame;
		}

		// ---- stream cost ----
		//
		// QueryPerformanceCounter around everything the recorder does on the game's
		// threads, summed per frame and logged as min / avg / max every 10 s.

		int64_t qpc()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		int64_t s_frame_ticks = 0; // recorder time in the current frame

		struct cost_timer
		{
			int64_t t0 = qpc();
			~cost_timer() { s_frame_ticks += qpc() - t0; }
		};

		// ---- contents ----

		// Writes one slice (or a volume level) of locked texel data, packed.
		void write_texels(uint32_t type, uint64_t id, uint32_t flags, UINT face, UINT level,
			D3DFORMAT format, UINT width, UINT height, UINT depth,
			const uint8_t* bits, UINT pitch, UINT slice_pitch)
		{
			auto& c = *s_cap;
			uint32_t rows, row_bytes;
			if (!slice_layout(format, width, height, rows, row_bytes))
			{
				rows = height;
				row_bytes = pitch;
				c.stats.unknown_format++;
			}
			row_bytes = std::min<uint32_t>(row_bytes, pitch);

			const texture_data td { id, flags, face, level, rows, row_bytes, depth };
			c.contents += uint64_t(rows) * row_bytes * depth;
			c.w.begin(type, sizeof(td) + size_t(rows) * row_bytes * depth);
			c.w.raw(&td, sizeof(td));
			for (UINT z = 0; z < depth; z++)
				for (uint32_t r = 0; r < rows; r++)
					c.w.raw(bits + size_t(z) * slice_pitch + size_t(r) * pitch, row_bytes);
		}

		IDirect3DSurface9* staging_surface(UINT width, UINT height, D3DFORMAT format)
		{
			auto& c = *s_cap;
			const auto key = std::make_tuple(width, height, format);
			if (const auto it = c.staging.find(key); it != c.staging.end())
				return it->second;
			IDirect3DSurface9* s = nullptr;
			if (FAILED(c.dev->CreateOffscreenPlainSurface(width, height, format, D3DPOOL_SYSTEMMEM, &s, nullptr)))
				s = nullptr;
			c.staging[key] = s;
			return s;
		}

		// Reads a render target through GetRenderTargetData (resolving MSAA
		// first) and hands the locked copy to fn.
		template <class Fn>
		bool read_render_target(IDirect3DSurface9* surface, Fn&& fn)
		{
			// It would wait on the bridge's server for a target Remix never drew.
			if (main_game() && !s_main_reference)
				return false;
			auto& c = *s_cap;
			D3DSURFACE_DESC sd;
			if (FAILED(surface->GetDesc(&sd)))
				return false;
			// The bridge's back buffer keeps the format the game asked for at Reset, which is "unknown" in a window: the display's.
			if (D3DDISPLAYMODE mode; sd.Format == D3DFMT_UNKNOWN && SUCCEEDED(c.dev->GetDisplayMode(0, &mode)))
				sd.Format = mode.Format;

			IDirect3DSurface9* source = surface;
			IDirect3DSurface9* resolved = nullptr;
			if (sd.MultiSampleType != D3DMULTISAMPLE_NONE)
			{
				if (FAILED(c.dev->CreateRenderTarget(sd.Width, sd.Height, sd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr))
					|| FAILED(c.dev->StretchRect(surface, nullptr, resolved, nullptr, D3DTEXF_NONE)))
				{
					if (resolved) resolved->Release();
					c.stats.msaa++;
					return false;
				}
				source = resolved;
			}

			bool ok = false;
			if (auto* tmp = staging_surface(sd.Width, sd.Height, sd.Format);
				tmp && SUCCEEDED(c.dev->GetRenderTargetData(source, tmp)))
			{
				D3DLOCKED_RECT lr;
				if (SUCCEEDED(tmp->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
				{
					fn(static_cast<const uint8_t*>(lr.pBits), static_cast<UINT>(lr.Pitch), sd);
					tmp->UnlockRect();
					ok = true;
				}
			}
			if (resolved) resolved->Release();
			return ok;
		}

		IDirect3DSurface9* texture_surface(IDirect3DBaseTexture9* texture, const texture_info& info, UINT face, UINT level)
		{
			IDirect3DSurface9* s = nullptr;
			if (info.kind == REC_TEXTURE)
				static_cast<IDirect3DTexture9*>(texture)->GetSurfaceLevel(level, &s);
			else if (info.kind == REC_CUBE_TEXTURE)
				static_cast<IDirect3DCubeTexture9*>(texture)->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(face), level, &s);
			return s;
		}

		void snapshot_texture_level(IDirect3DBaseTexture9* texture, const texture_info& info, UINT face, UINT level, uint32_t flags)
		{
			auto& c = *s_cap;
			const auto id = id_of(texture);

			if (info.kind == REC_VOLUME_TEXTURE)
			{
				auto* vol = static_cast<IDirect3DVolumeTexture9*>(texture);
				D3DVOLUME_DESC vd;
				D3DLOCKED_BOX lb;
				if (info.pool == D3DPOOL_DEFAULT && !(info.usage & D3DUSAGE_DYNAMIC))
				{
					c.stats.unreadable++;
					return;
				}
				if (SUCCEEDED(vol->GetLevelDesc(level, &vd)) && SUCCEEDED(vol->LockBox(level, &lb, nullptr, D3DLOCK_READONLY)))
				{
					write_texels(REC_TEXTURE_DATA, id, flags, 0, level, vd.Format, vd.Width, vd.Height, vd.Depth,
						static_cast<const uint8_t*>(lb.pBits), static_cast<UINT>(lb.RowPitch), static_cast<UINT>(lb.SlicePitch));
					vol->UnlockBox(level);
				}
				return;
			}

			if (info.usage & D3DUSAGE_DEPTHSTENCIL)
			{
				c.stats.depth++; // depth textures cannot be read back; the game clears them
				return;
			}

			auto* surface = texture_surface(texture, info, face, level);
			if (!surface)
				return;

			D3DSURFACE_DESC sd;
			surface->GetDesc(&sd);
			if (info.pool == D3DPOOL_DEFAULT && (info.usage & D3DUSAGE_RENDERTARGET))
			{
				read_render_target(surface, [&](const uint8_t* bits, UINT pitch, const D3DSURFACE_DESC& d)
				{
					write_texels(REC_TEXTURE_DATA, id, flags, face, level, d.Format, d.Width, d.Height, 1, bits, pitch, 0);
				});
			}
			else if (info.pool == D3DPOOL_DEFAULT && !(info.usage & D3DUSAGE_DYNAMIC))
			{
				c.stats.unreadable++;
			}
			else
			{
				D3DLOCKED_RECT lr;
				if (SUCCEEDED(surface->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
				{
					write_texels(REC_TEXTURE_DATA, id, flags, face, level, sd.Format, sd.Width, sd.Height, 1,
						static_cast<const uint8_t*>(lr.pBits), static_cast<UINT>(lr.Pitch), 0);
					surface->UnlockRect();
				}
				else c.stats.unreadable++;
			}
			surface->Release();
		}

		void snapshot_texture(IDirect3DBaseTexture9* texture, const texture_info& info, uint32_t flags)
		{
			const UINT faces = info.kind == REC_CUBE_TEXTURE ? 6 : 1;
			for (UINT f = 0; f < faces; f++)
				for (UINT l = 0; l < info.levels; l++)
					snapshot_texture_level(texture, info, f, l, flags);
		}

		// Standalone surfaces and the back buffer.
		void snapshot_surface(IDirect3DSurface9* surface, uint32_t flags)
		{
			auto& c = *s_cap;
			D3DSURFACE_DESC sd;
			if (FAILED(surface->GetDesc(&sd)))
				return;
			const auto id = id_of(surface);
			if (sd.Usage & D3DUSAGE_DEPTHSTENCIL)
			{
				c.stats.depth++;
				return;
			}
			if (sd.Usage & D3DUSAGE_RENDERTARGET || surface == c.backbuffer)
			{
				read_render_target(surface, [&](const uint8_t* bits, UINT pitch, const D3DSURFACE_DESC& d)
				{
					write_texels(REC_SURFACE_DATA, id, flags, 0, 0, d.Format, d.Width, d.Height, 1, bits, pitch, 0);
				});
				return;
			}
			D3DLOCKED_RECT lr;
			if (SUCCEEDED(surface->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
			{
				write_texels(REC_SURFACE_DATA, id, flags, 0, 0, sd.Format, sd.Width, sd.Height, 1,
					static_cast<const uint8_t*>(lr.pBits), static_cast<UINT>(lr.Pitch), 0);
				surface->UnlockRect();
			}
			else c.stats.unreadable++;
		}

		// ---- resource lock hooks ----

		void on_buffer_lock(IUnknown* self, UINT offset, UINT size, const void* data, DWORD flags)
		{
			std::lock_guard lock(s_mutex);
			cost_timer cost;
			if (!s_cap || (flags & D3DLOCK_READONLY) || !s_cap->buffers.contains(self))
				return;
			s_cap->locks[self] = { offset, size, data };
		}

		// The game's own write, copied from the pointer it wrote through: no lock of
		// ours, so nothing that could wait for the GPU.
		void on_buffer_unlock(IUnknown* self)
		{
			std::lock_guard lock(s_mutex);
			cost_timer cost;
			if (!s_cap)
				return;
			auto& c = *s_cap;
			const auto it = c.locks.find(self);
			if (it == c.locks.end())
				return;
			const auto [offset, size, data] = it->second;
			c.locks.erase(it);
			const UINT total = c.buffers[self].size;
			if (offset >= total)
				return;
			const UINT bytes = (size == 0 || offset + size > total) ? total - offset : size;
			c.w.rec(REC_BUFFER_DATA, buffer_data { id_of(self), 0, offset, bytes }, data, bytes);
			c.stats.buffer_updates++;
		}

		void on_texture_unlock(IDirect3DTexture9* self, UINT level)
		{
			std::lock_guard lock(s_mutex);
			cost_timer cost;
			if (!s_cap)
				return;
			const auto it = s_cap->textures.find(self);
			if (it == s_cap->textures.end())
				return;
			// Re-reading a default-pool (dynamic) texture can wait for the GPU: a
			// stream never does. DS1 captures contain none of these.
			if (s_cap->streaming && it->second.pool == D3DPOOL_DEFAULT)
				return;
			internal_scope in;
			snapshot_texture_level(self, it->second, 0, level, 0);
			s_cap->stats.texture_updates++;
		}

		template <int N>
		HRESULT __stdcall buffer_lock_hk(IUnknown* self, UINT offset, UINT size, void** data, DWORD flags)
		{
			const auto hr = s_buffer_lock_slots[N].og(self, offset, size, data, flags);
			if (SUCCEEDED(hr) && data && *data && s_capturing.load(std::memory_order_relaxed) && !t_internal)
				on_buffer_lock(self, offset, size, *data, flags);
			return hr;
		}

		template <int N>
		HRESULT __stdcall buffer_unlock_hk(IUnknown* self)
		{
			if (s_capturing.load(std::memory_order_relaxed) && !t_internal)
				on_buffer_unlock(self);
			return s_buffer_unlock_slots[N].og(self);
		}

		template <int N>
		HRESULT __stdcall texture_unlock_hk(IDirect3DTexture9* self, UINT level)
		{
			const auto hr = s_texture_unlock_slots[N].og(self, level);
			if (SUCCEEDED(hr) && s_capturing.load(std::memory_order_relaxed) && !t_internal)
				on_texture_unlock(self, level);
			return hr;
		}

		void* const BUFFER_LOCK_DETOURS[LOCK_SLOTS] = {
			reinterpret_cast<void*>(&buffer_lock_hk<0>), reinterpret_cast<void*>(&buffer_lock_hk<1>),
			reinterpret_cast<void*>(&buffer_lock_hk<2>), reinterpret_cast<void*>(&buffer_lock_hk<3>),
			reinterpret_cast<void*>(&buffer_lock_hk<4>), reinterpret_cast<void*>(&buffer_lock_hk<5>) };
		void* const BUFFER_UNLOCK_DETOURS[LOCK_SLOTS] = {
			reinterpret_cast<void*>(&buffer_unlock_hk<0>), reinterpret_cast<void*>(&buffer_unlock_hk<1>),
			reinterpret_cast<void*>(&buffer_unlock_hk<2>), reinterpret_cast<void*>(&buffer_unlock_hk<3>),
			reinterpret_cast<void*>(&buffer_unlock_hk<4>), reinterpret_cast<void*>(&buffer_unlock_hk<5>) };
		void* const TEXTURE_UNLOCK_DETOURS[LOCK_SLOTS] = {
			reinterpret_cast<void*>(&texture_unlock_hk<0>), reinterpret_cast<void*>(&texture_unlock_hk<1>),
			reinterpret_cast<void*>(&texture_unlock_hk<2>), reinterpret_cast<void*>(&texture_unlock_hk<3>),
			reinterpret_cast<void*>(&texture_unlock_hk<4>), reinterpret_cast<void*>(&texture_unlock_hk<5>) };

		void slots_full()
		{
			if (s_slots_full_logged) return;
			s_slots_full_logged = true;
			log("Out of Lock hook slots: in-frame writes to some resources will be missed", shared::common::LOG_TYPE::LOG_TYPE_WARN);
		}

		template <class Fn>
		void ensure_hook(hook_slot_t<Fn> (&slots)[LOCK_SLOTS], void* const (&detours)[LOCK_SLOTS], void* target)
		{
			for (const auto& slot : slots)
				if (slot.target == target)
					return;
			for (int n = 0; n < LOCK_SLOTS; n++)
			{
				auto& slot = slots[n];
				if (slot.target)
					continue;
				slot.target = target;
				if (MH_CreateHook(target, detours[n], reinterpret_cast<void**>(&slot.og)) != MH_OK || MH_EnableHook(target) != MH_OK)
					log("Could not hook a resource Lock/Unlock: its in-frame writes will be missed", shared::common::LOG_TYPE::LOG_TYPE_WARN);
				return;
			}
			slots_full();
		}

		void ensure_buffer_hooks(IUnknown* buffer)
		{
			auto** vt = *reinterpret_cast<void***>(buffer);
			ensure_hook(s_buffer_lock_slots, BUFFER_LOCK_DETOURS, vt[VT_BUFFER_LOCK]);
			ensure_hook(s_buffer_unlock_slots, BUFFER_UNLOCK_DETOURS, vt[VT_BUFFER_UNLOCK]);
		}

		void ensure_texture_hooks(IDirect3DBaseTexture9* texture)
		{
			auto** vt = *reinterpret_cast<void***>(texture);
			ensure_hook(s_texture_unlock_slots, TEXTURE_UNLOCK_DETOURS, vt[VT_TEXTURE_UNLOCK_RECT]);
		}

		// ---- resource registration ----

		// A one-frame capture holds everything it recorded so no address can be
		// reused mid-frame. A stream must not: the game would never free anything.
		void keep(IUnknown* obj)
		{
			if (s_cap->streaming)
				return;
			obj->AddRef();
			s_cap->refs.push_back(obj);
		}

		// Drops everything recorded at this address (a stream saw it reused).
		void forget(const void* obj)
		{
			auto& c = *s_cap;
			bool known = false;
			auto erase = [&](auto& map)
			{
				const auto it = map.find(obj);
				if (it == map.end())
					return false;
				if (it->second.pending)
					c.pending--;
				map.erase(it);
				return true;
			};
			std::vector<uint64_t> levels; // the texture's surfaces, gone with it
			if (erase(c.textures))
			{
				known = true;
				std::erase_if(c.surfaces, [obj, &levels](const auto& kv)
				{
					if (kv.second.texture != obj)
						return false;
					levels.push_back(id_of(kv.first));
					return true;
				});
			}
			known |= erase(c.surfaces);
			known |= erase(c.buffers);
			known |= c.objects.erase(obj) > 0;
			c.locks.erase(obj);
			if (!known)
				return;
			const auto id = id_of(obj);
			c.w.rec(REC_DESTROY, id_only { id });

			// Nothing is bound under these ids on the host any more: a new object at
			// the same address must be bound again even though its id is the same.
			auto& sh = c.shadow;
			auto gone = [&](uint64_t v) { return v == id || std::find(levels.begin(), levels.end(), v) != levels.end(); };
			auto drop = [&](auto& slot) { if (slot && gone(*slot)) slot.reset(); };
			for (auto& t : sh.texture) drop(t);
			for (auto& r : sh.rt) drop(r);
			drop(sh.ds);
			drop(sh.vs);
			drop(sh.ps);
			drop(sh.decl);
			drop(sh.indices);
			for (auto& s : sh.stream)
				if (s && s->id == id) s.reset();
		}

		// ---- contents pacing (live streams) ----
		//
		// A capture file takes every object's contents at its first use. A stream
		// session starts the same way, and its first frame uses everything at once:
		// some 300 MB at 4K, two thirds of it render targets read back from the GPU.
		// A stream never reads anything back from the GPU: render targets, the back
		// buffer and every other default-pool object are described but get no
		// contents (the host fills them by replaying frames; dynamic buffers get the
		// game's own writes). The rest, CPU-side copies of managed and system-memory
		// objects, is paced with a byte budget per frame: an object over budget is
		// described at once (its bindings resolve) and its contents follow at a
		// later use, usually the next frame. Each frame that used an object still
		// waiting for them is flagged PRESENT_INCOMPLETE, and the host does not
		// draw it.

		uint64_t packed_bytes(D3DFORMAT format, UINT width, UINT height, UINT depth)
		{
			uint32_t rows, row_bytes;
			if (!slice_layout(format, width, height, rows, row_bytes))
			{
				rows = height;
				row_bytes = width * 4; // unknown format: an estimate, for pacing only
			}
			return uint64_t(rows) * row_bytes * depth;
		}

		uint64_t texture_bytes(const texture_info& info, UINT width, UINT height, UINT depth)
		{
			uint64_t total = 0;
			for (UINT l = 0; l < info.levels; l++)
				total += packed_bytes(info.format, std::max(1u, width >> l), std::max(1u, height >> l),
					info.kind == REC_VOLUME_TEXTURE ? std::max(1u, depth >> l) : 1u);
			return total * (info.kind == REC_CUBE_TEXTURE ? 6 : 1);
		}

		// What snapshot_texture_level does with this texture.
		uint8_t texture_contents(const texture_info& info)
		{
			const bool default_static = info.pool == D3DPOOL_DEFAULT && !(info.usage & D3DUSAGE_DYNAMIC);
			if (info.kind == REC_VOLUME_TEXTURE)
				return default_static ? CONTENTS_NONE : info.pool == D3DPOOL_DEFAULT ? CONTENTS_READBACK : CONTENTS_LOCK;
			if (info.usage & D3DUSAGE_DEPTHSTENCIL)
				return CONTENTS_NONE;
			if (info.pool == D3DPOOL_DEFAULT && (info.usage & D3DUSAGE_RENDERTARGET))
				return main_game() ? CONTENTS_NONE : CONTENTS_READBACK;
			if (default_static)
				return CONTENTS_NONE;
			// a read-only lock of a default-pool dynamic texture can wait for the GPU, not with the bridge's copy
			return info.pool == D3DPOOL_DEFAULT && !main_game() ? CONTENTS_READBACK : CONTENTS_LOCK;
		}

		// Whether contents of this size may go out now. Always in a capture, and
		// when forced: the source of a copy (system memory), whose result the host
		// would otherwise get wrong in a frame that is not flagged. One object over
		// the budget still goes when it is the first of its frame, so everything
		// gets sent.
		bool pace(uint64_t bytes, bool force)
		{
			auto& c = *s_cap;
			if (!c.streaming)
				return true;
			if (c.w.ring->overflow)
				return false; // the frame is dropped anyway
			if (!force && c.sent && int64_t(bytes) > c.budget)
				return false;
			c.budget -= int64_t(bytes);
			c.sent += int64_t(bytes);
			return true;
		}

		// Sends a pending object's initial contents if the pace allows.
		template <class Info, class Snapshot>
		void send_contents(Info& info, bool force, Snapshot&& snapshot)
		{
			auto& c = *s_cap;
			if (!pace(info.bytes, force))
			{
				c.frame_incomplete = true;
				return;
			}
			snapshot();
			info.pending = false;
			c.pending--;
		}

		// An object seen for the first time: its contents go now, later, or never.
		template <class Info, class Snapshot>
		void first_contents(Info& info, bool force, Snapshot&& snapshot)
		{
			auto& c = *s_cap;
			if (info.contents == CONTENTS_NONE)
			{
				snapshot(); // counts what cannot be read
				return;
			}
			// A stream never waits for the GPU, and never sends what might not fit.
			if (c.streaming && (info.contents == CONTENTS_READBACK || info.bytes > c.contents_max))
			{
				info.contents = CONTENTS_NONE;
				c.stats.not_sent++;
				return;
			}
			info.pending = true;
			c.pending++;
			send_contents(info, force, snapshot);
			if (info.pending)
				c.stats.deferred++;
		}

		uint64_t ref_texture(IDirect3DBaseTexture9* texture, bool force = false)
		{
			if (!texture) return 0;
			auto& c = *s_cap;
			const auto id = id_of(texture);
			if (const auto it = c.textures.find(texture); it != c.textures.end())
			{
				if (it->second.pending)
					send_contents(it->second, force, [&] { snapshot_texture(texture, it->second, DATA_INITIAL); });
				return id;
			}

			texture_info info {};
			texture_desc d {};
			d.id = id;
			info.levels = texture->GetLevelCount();
			switch (texture->GetType())
			{
			case D3DRTYPE_TEXTURE:
			{
				D3DSURFACE_DESC sd;
				static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &sd);
				info.kind = REC_TEXTURE;
				d.width = sd.Width; d.height = sd.Height; d.depth = 1;
				info.format = sd.Format; info.pool = sd.Pool; info.usage = sd.Usage;
				break;
			}
			case D3DRTYPE_CUBETEXTURE:
			{
				D3DSURFACE_DESC sd;
				static_cast<IDirect3DCubeTexture9*>(texture)->GetLevelDesc(0, &sd);
				info.kind = REC_CUBE_TEXTURE;
				d.width = sd.Width; d.height = sd.Height; d.depth = 1;
				info.format = sd.Format; info.pool = sd.Pool; info.usage = sd.Usage;
				break;
			}
			case D3DRTYPE_VOLUMETEXTURE:
			{
				D3DVOLUME_DESC vd;
				static_cast<IDirect3DVolumeTexture9*>(texture)->GetLevelDesc(0, &vd);
				info.kind = REC_VOLUME_TEXTURE;
				d.width = vd.Width; d.height = vd.Height; d.depth = vd.Depth;
				info.format = vd.Format; info.pool = vd.Pool; info.usage = vd.Usage;
				break;
			}
			default:
				return 0;
			}
			d.levels = info.levels;
			d.usage = info.usage;
			d.format = static_cast<uint32_t>(info.format);
			d.pool = static_cast<uint32_t>(info.pool);

			info.contents = texture_contents(info);
			info.bytes = texture_bytes(info, d.width, d.height, d.depth);

			keep(texture);
			auto& stored = c.textures[texture] = info;
			c.stats.textures++;
			c.w.rec(info.kind, d);
			// the TPF name, which decides how the proxy treats atmospheric layers
			if (const auto name = game::texture_names::name_of(texture); !name.empty())
				c.w.rec(REC_TEXTURE_NAME, id_only { id }, name.data(), name.size());
			if (info.kind == REC_TEXTURE)
				ensure_texture_hooks(texture);
			first_contents(stored, force,
				[&] { snapshot_texture(texture, stored, DATA_INITIAL); });
			return id;
		}

		uint64_t ref_surface(IDirect3DSurface9* surface, bool force = false)
		{
			if (!surface) return 0;
			auto& c = *s_cap;
			const auto id = id_of(surface);
			if (const auto it = c.surfaces.find(surface); it != c.surfaces.end())
			{
				auto& known = it->second;
				if (!known.texture)
				{
					if (known.pending)
						send_contents(known, force, [&] { snapshot_surface(surface, DATA_INITIAL); });
					return id;
				}
				// A level of a texture freed since could be at this address now, as
				// a level of another texture. Streams only; captures hold their refs.
				if (!c.streaming)
					return id;
				void* container = nullptr;
				const bool same = (SUCCEEDED(surface->GetContainer(__uuidof(IDirect3DTexture9), &container)) ||
					SUCCEEDED(surface->GetContainer(__uuidof(IDirect3DCubeTexture9), &container))) && container == known.texture;
				if (container)
					static_cast<IUnknown*>(container)->Release();
				if (same)
				{
					ref_texture(known.texture, force); // the level's contents are the texture's
					return id;
				}
				c.surfaces.erase(it);
				c.w.rec(REC_DESTROY, id_only { id });
				for (auto& r : c.shadow.rt)
					if (r && *r == id) r.reset(); // bound again below, as the new surface
				if (c.shadow.ds && *c.shadow.ds == id) c.shadow.ds.reset();
			}

			surface_info info {};
			void* container = nullptr;
			if (SUCCEEDED(surface->GetContainer(__uuidof(IDirect3DTexture9), &container)) && container)
			{
				auto* tex = static_cast<IDirect3DTexture9*>(container);
				ref_texture(tex, force);
				info.texture = tex;
				for (UINT l = 0, n = tex->GetLevelCount(); l < n; l++)
				{
					IDirect3DSurface9* s = nullptr;
					if (SUCCEEDED(tex->GetSurfaceLevel(l, &s)) && s)
					{
						const bool match = s == surface;
						s->Release();
						if (match) { info.level = l; break; }
					}
				}
				tex->Release();
			}
			else if (SUCCEEDED(surface->GetContainer(__uuidof(IDirect3DCubeTexture9), &container)) && container)
			{
				auto* cube = static_cast<IDirect3DCubeTexture9*>(container);
				ref_texture(cube, force);
				info.texture = cube;
				bool found = false;
				for (UINT f = 0; f < 6 && !found; f++)
					for (UINT l = 0, n = cube->GetLevelCount(); l < n && !found; l++)
					{
						IDirect3DSurface9* s = nullptr;
						if (SUCCEEDED(cube->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(f), l, &s)) && s)
						{
							found = s == surface;
							s->Release();
							if (found) { info.face = f; info.level = l; }
						}
					}
				cube->Release();
			}

			keep(surface);
			auto& stored = c.surfaces[surface] = info;
			if (info.texture)
			{
				c.w.rec(REC_SURFACE_LEVEL, surface_level { id, id_of(info.texture), info.face, info.level });
				return id;
			}

			D3DSURFACE_DESC sd;
			if (FAILED(surface->GetDesc(&sd)))
				sd = {};
			if (surface == c.backbuffer)
				c.w.rec(REC_BACKBUFFER, id_only { id });
			else
				c.w.rec(REC_SURFACE, surface_desc { id, sd.Width, sd.Height, static_cast<uint32_t>(sd.Format),
					sd.Usage, static_cast<uint32_t>(sd.Pool), static_cast<uint32_t>(sd.MultiSampleType), sd.MultiSampleQuality });
			// as snapshot_surface takes them
			const bool render_target = surface == c.backbuffer || (sd.Usage & D3DUSAGE_RENDERTARGET);
			stored.contents = (sd.Usage & D3DUSAGE_DEPTHSTENCIL) || (render_target && main_game()) ? CONTENTS_NONE
				: (render_target || sd.Pool == D3DPOOL_DEFAULT) ? CONTENTS_READBACK : CONTENTS_LOCK;
			stored.bytes = packed_bytes(sd.Format, sd.Width, sd.Height, 1);
			first_contents(stored, force, [&] { snapshot_surface(surface, DATA_INITIAL); });
			return id;
		}

		// Re-reads a surface's contents (UpdateSurface sources are written
		// through surface locks, which are not hooked).
		void refresh_surface(IDirect3DSurface9* surface)
		{
			auto& c = *s_cap;
			auto& info = c.surfaces[surface];
			// a stream never re-reads what it does not send (GPU-side or too large)
			auto readable = [&c](uint8_t contents) { return !c.streaming || contents != CONTENTS_NONE; };
			if (info.texture)
			{
				if (const auto it = c.textures.find(info.texture); it != c.textures.end() && readable(it->second.contents))
					snapshot_texture_level(info.texture, it->second, info.face, info.level, 0);
			}
			else if (readable(info.contents))
			{
				snapshot_surface(surface, 0);
				if (info.pending)
				{
					info.pending = false;
					c.pending--;
				}
			}
		}

		template <class Buffer, class Desc>
		uint64_t ref_buffer(Buffer* buffer, uint32_t type)
		{
			if (!buffer) return 0;
			auto& c = *s_cap;
			const auto id = id_of(buffer);
			auto snapshot = [buffer, id](UINT size)
			{
				void* data = nullptr;
				if (SUCCEEDED(buffer->Lock(0, 0, &data, D3DLOCK_READONLY)) && data)
				{
					s_cap->contents += size;
					s_cap->w.rec(REC_BUFFER_DATA, buffer_data { id, DATA_INITIAL, 0, size }, data, size);
					buffer->Unlock();
				}
				else s_cap->stats.unreadable++;
			};
			if (const auto it = c.buffers.find(buffer); it != c.buffers.end())
			{
				if (it->second.pending)
					send_contents(it->second, false, [&] { snapshot(it->second.size); });
				return id;
			}

			Desc d;
			buffer->GetDesc(&d);
			uint32_t format;
			if constexpr (std::is_same_v<Desc, D3DVERTEXBUFFER_DESC>) format = d.FVF;
			else format = static_cast<uint32_t>(d.Format);

			keep(buffer);
			auto& stored = c.buffers[buffer];
			stored.size = d.Size;
			stored.bytes = d.Size;
			// reading a default-pool buffer can wait for the GPU like a read-back
			stored.contents = d.Pool == D3DPOOL_DEFAULT ? CONTENTS_READBACK : CONTENTS_LOCK;
			c.stats.buffers++;
			c.w.rec(type, buffer_desc { id, d.Size, d.Usage, format, static_cast<uint32_t>(d.Pool) });
			ensure_buffer_hooks(buffer);
			first_contents(stored, false, [&] { snapshot(d.Size); });
			return id;
		}

		uint64_t ref_vertex_buffer(IDirect3DVertexBuffer9* b) { return ref_buffer<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC>(b, REC_VERTEX_BUFFER); }
		uint64_t ref_index_buffer(IDirect3DIndexBuffer9* b) { return ref_buffer<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC>(b, REC_INDEX_BUFFER); }

		template <class Shader>
		uint64_t ref_shader(Shader* shader, uint32_t type)
		{
			if (!shader) return 0;
			auto& c = *s_cap;
			const auto id = id_of(shader);
			if (c.objects.contains(shader))
				return id;
			UINT size = 0;
			shader->GetFunction(nullptr, &size);
			c.scratch.resize(size);
			shader->GetFunction(c.scratch.data(), &size);
			keep(shader);
			c.objects.insert(shader);
			c.w.rec(type, id_only { id }, c.scratch.data(), size);
			return id;
		}

		uint64_t ref_decl(IDirect3DVertexDeclaration9* decl)
		{
			if (!decl) return 0;
			auto& c = *s_cap;
			const auto id = id_of(decl);
			if (c.objects.contains(decl))
				return id;
			UINT count = 0;
			decl->GetDeclaration(nullptr, &count);
			std::vector<D3DVERTEXELEMENT9> elements(count);
			decl->GetDeclaration(elements.data(), &count);
			keep(decl);
			c.objects.insert(decl);
			c.w.rec(REC_VERTEX_DECL, id_only { id }, elements.data(), count * sizeof(D3DVERTEXELEMENT9));
			return id;
		}

		// ---- state ----

		void emit_const_runs(uint32_t type, const_shadow& shadow, const void* current, uint32_t count, uint32_t reg_bytes)
		{
			auto& c = *s_cap;
			const auto* cur = static_cast<const uint8_t*>(current);
			shadow.bytes.resize(size_t(count) * reg_bytes);
			auto same = [&](uint32_t i)
			{
				return shadow.valid && std::memcmp(shadow.bytes.data() + size_t(i) * reg_bytes, cur + size_t(i) * reg_bytes, reg_bytes) == 0;
			};
			for (uint32_t i = 0; i < count;)
			{
				if (same(i)) { i++; continue; }
				const uint32_t start = i;
				while (i < count && !same(i)) i++;
				c.w.rec(type, const_range { start, i - start }, cur + size_t(start) * reg_bytes, size_t(i - start) * reg_bytes);
			}
			std::memcpy(shadow.bytes.data(), cur, size_t(count) * reg_bytes);
			shadow.valid = true;
		}

		uint64_t current_surface_id(IDirect3DSurface9* s)
		{
			const auto id = ref_surface(s);
			if (s) s->Release();
			return id;
		}

		// Reads the device's effective state and writes what changed. Only the
		// state that affects this command is read; anything skipped is caught
		// up the next time a command depends on it.
		void snapshot_state(IDirect3DDevice9* dev, bool clear_only)
		{
			auto& c = *s_cap;
			auto& sh = c.shadow;

			for (const auto rs : RENDER_STATES)
			{
				DWORD v;
				if (SUCCEEDED(dev->GetRenderState(static_cast<D3DRENDERSTATETYPE>(rs), &v)) && update(sh.rs[rs], v))
					c.w.rec(REC_RENDER_STATE, state_value { rs, v });
			}

			for (DWORD i = 0; i < MAX_RENDER_TARGETS; i++)
			{
				IDirect3DSurface9* s = nullptr;
				if (FAILED(dev->GetRenderTarget(i, &s))) s = nullptr;
				const auto id = current_surface_id(s);
				if (update(sh.rt[i], id))
					c.w.rec(REC_RENDER_TARGET, bind { i, id });
			}
			{
				IDirect3DSurface9* s = nullptr;
				if (FAILED(dev->GetDepthStencilSurface(&s))) s = nullptr;
				const auto id = current_surface_id(s);
				if (update(sh.ds, id))
					c.w.rec(REC_DEPTH_STENCIL, id_only { id });
			}
			if (D3DVIEWPORT9 vp; SUCCEEDED(dev->GetViewport(&vp)) && update(sh.viewport, vp))
				c.w.rec(REC_VIEWPORT, vp);
			if (RECT r; SUCCEEDED(dev->GetScissorRect(&r)) && update(sh.scissor, to_rect(r)))
				c.w.rec(REC_SCISSOR, to_rect(r));

			if (clear_only)
				return;

			if (const DWORD planes = sh.rs[D3DRS_CLIPPLANEENABLE].value_or(0))
				for (DWORD i = 0; i < 6; i++)
				{
					clip_plane cp { i, {} };
					if ((planes & (1u << i)) && SUCCEEDED(dev->GetClipPlane(i, cp.plane)) && update(sh.clip[i], cp))
						c.w.rec(REC_CLIP_PLANE, cp);
				}

			IDirect3DVertexShader9* vs = nullptr;
			if (FAILED(dev->GetVertexShader(&vs))) vs = nullptr;
			IDirect3DPixelShader9* ps = nullptr;
			if (FAILED(dev->GetPixelShader(&ps))) ps = nullptr;
			IDirect3DVertexDeclaration9* decl = nullptr;
			if (FAILED(dev->GetVertexDeclaration(&decl))) decl = nullptr;

			const auto vs_id = ref_shader(vs, REC_VERTEX_SHADER);
			const auto ps_id = ref_shader(ps, REC_PIXEL_SHADER);
			const auto decl_id = ref_decl(decl);
			if (vs) vs->Release();
			if (ps) ps->Release();
			if (decl) decl->Release();
			if (update(sh.vs, vs_id)) c.w.rec(REC_SET_VERTEX_SHADER, id_only { vs_id });
			if (update(sh.ps, ps_id)) c.w.rec(REC_SET_PIXEL_SHADER, id_only { ps_id });
			if (update(sh.decl, decl_id)) c.w.rec(REC_SET_VERTEX_DECL, id_only { decl_id });
			if (DWORD fvf; SUCCEEDED(dev->GetFVF(&fvf)) && update(sh.fvf, fvf))
				c.w.rec(REC_SET_FVF, state_value { 0, fvf });

			if (vs_id)
			{
				c.scratch.resize(size_t(c.vs_const_f) * 16);
				if (SUCCEEDED(dev->GetVertexShaderConstantF(0, reinterpret_cast<float*>(c.scratch.data()), c.vs_const_f)))
					emit_const_runs(REC_VS_CONST_F, sh.vsf, c.scratch.data(), c.vs_const_f, 16);
				int vi[16 * 4];
				if (SUCCEEDED(dev->GetVertexShaderConstantI(0, vi, 16)))
					emit_const_runs(REC_VS_CONST_I, sh.vsi, vi, 16, 16);
				BOOL vb[16];
				if (SUCCEEDED(dev->GetVertexShaderConstantB(0, vb, 16)))
					emit_const_runs(REC_VS_CONST_B, sh.vsb, vb, 16, 4);
			}
			if (ps_id)
			{
				c.scratch.resize(size_t(c.ps_const_f) * 16);
				if (SUCCEEDED(dev->GetPixelShaderConstantF(0, reinterpret_cast<float*>(c.scratch.data()), c.ps_const_f)))
					emit_const_runs(REC_PS_CONST_F, sh.psf, c.scratch.data(), c.ps_const_f, 16);
				int pi[16 * 4];
				if (SUCCEEDED(dev->GetPixelShaderConstantI(0, pi, 16)))
					emit_const_runs(REC_PS_CONST_I, sh.psi, pi, 16, 16);
				BOOL pb[16];
				if (SUCCEEDED(dev->GetPixelShaderConstantB(0, pb, 16)))
					emit_const_runs(REC_PS_CONST_B, sh.psb, pb, 16, 4);
			}

			for (DWORD slot = 0; slot < SAMPLER_SLOTS; slot++)
			{
				const DWORD stage = sampler_index(slot);
				IDirect3DBaseTexture9* t = nullptr;
				if (FAILED(dev->GetTexture(stage, &t))) t = nullptr;
				const auto id = ref_texture(t);
				if (t) t->Release();
				if (update(sh.texture[slot], id))
					c.w.rec(REC_SET_TEXTURE, bind { stage, id });
				if (!id)
					continue;
				for (DWORD type = D3DSAMP_ADDRESSU; type <= D3DSAMP_DMAPOFFSET; type++)
				{
					DWORD v;
					if (SUCCEEDED(dev->GetSamplerState(stage, static_cast<D3DSAMPLERSTATETYPE>(type), &v)) && update(sh.ss[slot][type], v))
						c.w.rec(REC_SAMPLER_STATE, stage_state { stage, type, v });
				}
			}

			if (!ps_id)
			{
				for (DWORD stage = 0; stage < 8; stage++)
					for (const auto type : TEXTURE_STAGE_STATES)
					{
						DWORD v;
						if (SUCCEEDED(dev->GetTextureStageState(stage, static_cast<D3DTEXTURESTAGESTATETYPE>(type), &v)) && update(sh.tss[stage][type], v))
							c.w.rec(REC_TEXTURE_STAGE_STATE, stage_state { stage, type, v });
					}
			}

			if (!vs_id)
			{
				for (size_t i = 0; i < std::size(TRANSFORMS); i++)
				{
					transform t { TRANSFORMS[i], {} };
					if (SUCCEEDED(dev->GetTransform(static_cast<D3DTRANSFORMSTATETYPE>(TRANSFORMS[i]), reinterpret_cast<D3DMATRIX*>(t.m))) && update(sh.xform[i], t))
						c.w.rec(REC_TRANSFORM, t);
				}
				if (sh.rs[D3DRS_LIGHTING].value_or(0))
				{
					if (D3DMATERIAL9 m; SUCCEEDED(dev->GetMaterial(&m)) && update(sh.material, m))
						c.w.rec(REC_MATERIAL, m);
					for (DWORD i = 0; i < 8; i++)
					{
						BOOL on = FALSE;
						if (FAILED(dev->GetLightEnable(i, &on)))
							continue;
						if (update(sh.light_enable[i], static_cast<DWORD>(on)))
							c.w.rec(REC_LIGHT_ENABLE, state_value { i, static_cast<DWORD>(on) });
						light l { i, {} };
						if (on && SUCCEEDED(dev->GetLight(i, reinterpret_cast<D3DLIGHT9*>(l.data))) && update(sh.lights[i], l))
							c.w.rec(REC_LIGHT, l);
					}
				}
			}

			for (DWORD i = 0; i < MAX_STREAMS; i++)
			{
				IDirect3DVertexBuffer9* vb = nullptr;
				UINT offset = 0, stride = 0;
				if (FAILED(dev->GetStreamSource(i, &vb, &offset, &stride))) vb = nullptr;
				const stream_source ss { i, ref_vertex_buffer(vb), vb ? offset : 0, vb ? stride : 0 };
				if (vb) vb->Release();
				if (update(sh.stream[i], ss))
					c.w.rec(REC_STREAM_SOURCE, ss);
				if (UINT div; SUCCEEDED(dev->GetStreamSourceFreq(i, &div)) && update(sh.stream_freq[i], static_cast<DWORD>(div)))
					c.w.rec(REC_STREAM_FREQ, state_value { i, div });
			}
			{
				IDirect3DIndexBuffer9* ib = nullptr;
				if (FAILED(dev->GetIndices(&ib))) ib = nullptr;
				const auto id = ref_index_buffer(ib);
				if (ib) ib->Release();
				if (update(sh.indices, id))
					c.w.rec(REC_INDICES, id_only { id });
			}
		}

		// ---- capture control ----

		void write_reference_image()
		{
			auto& c = *s_cap;
			if (!read_render_target(c.backbuffer, [&](const uint8_t* bits, UINT pitch, const D3DSURFACE_DESC& d)
			{
				uint32_t rows, row_bytes;
				if (!slice_layout(d.Format, d.Width, d.Height, rows, row_bytes)) { rows = d.Height; row_bytes = pitch; }
				const image img { d.Width, d.Height, static_cast<uint32_t>(d.Format), row_bytes };
				c.w.begin(REC_REFERENCE_IMAGE, sizeof(img) + size_t(rows) * row_bytes);
				c.w.raw(&img, sizeof(img));
				for (uint32_t r = 0; r < rows; r++)
					c.w.raw(bits + size_t(r) * pitch, row_bytes);
			}))
				log("Could not read the back buffer for the reference image", shared::common::LOG_TYPE::LOG_TYPE_WARN);
		}

		void ensure_device_hooks(IDirect3DDevice9* dev);

		std::string output_dir()
		{
			return shared::globals::root_path + "\\" + shared::common::config::get().lookdev.output_dir;
		}

		// Whether this device can be recorded at all; installs the device hooks.
		bool device_recordable(IDirect3DDevice9* dev, const char* what)
		{
			D3DDEVICE_CREATION_PARAMETERS cp;
			dev->GetCreationParameters(&cp);
			if (cp.BehaviorFlags & D3DCREATE_PUREDEVICE)
			{
				log(std::format("{} refused: pure device, state cannot be read back", what), shared::common::LOG_TYPE::LOG_TYPE_ERROR);
				return false;
			}
			ensure_device_hooks(dev);
			return s_device_hooks;
		}

		// Fills the capture's device facts and returns the file header for it.
		file_header describe_device(IDirect3DDevice9* dev, capture* c)
		{
			D3DDEVICE_CREATION_PARAMETERS cp;
			dev->GetCreationParameters(&cp);

			D3DCAPS9 caps;
			dev->GetDeviceCaps(&caps);
			c->vs_const_f = std::clamp<uint32_t>(caps.MaxVertexShaderConst, 16, 256);
			c->ps_const_f = caps.PixelShaderVersion >= D3DPS_VERSION(3, 0) ? 224 : 32;

			D3DPRESENT_PARAMETERS pp {};
			if (IDirect3DSwapChain9* sc = nullptr; SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc)
			{
				sc->GetPresentParameters(&pp);
				sc->Release();
			}
			if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &c->backbuffer)))
				c->backbuffer = nullptr;

			file_header h {};
			h.magic = CAPTURE_MAGIC;
			h.version = CAPTURE_VERSION;
			h.header_size = sizeof(h);
			h.adapter = cp.AdapterOrdinal;
			h.device_type = static_cast<uint32_t>(cp.DeviceType);
			h.behavior_flags = cp.BehaviorFlags;
			h.bb_width = pp.BackBufferWidth;
			h.bb_height = pp.BackBufferHeight;
			h.bb_format = static_cast<uint32_t>(pp.BackBufferFormat);
			h.bb_count = pp.BackBufferCount;
			h.multisample = static_cast<uint32_t>(pp.MultiSampleType);
			h.multisample_quality = pp.MultiSampleQuality;
			h.swap_effect = static_cast<uint32_t>(pp.SwapEffect);
			h.windowed = static_cast<uint32_t>(pp.Windowed);
			h.auto_depth_stencil = static_cast<uint32_t>(pp.EnableAutoDepthStencil);
			h.auto_depth_stencil_format = static_cast<uint32_t>(pp.AutoDepthStencilFormat);
			h.present_flags = pp.Flags;
			h.refresh_rate = pp.FullScreen_RefreshRateInHz;
			h.presentation_interval = pp.PresentationInterval;
			h.vs_const_f = c->vs_const_f;
			h.ps_const_f = c->ps_const_f;
			h.frame_index = s_frame_index;
			const auto ini = std::filesystem::path(shared::common::config::get().ini_path()).filename().string();
			strncpy_s(h.game_ini, ini.c_str(), _TRUNCATE);
			return h;
		}

		void release_staging(capture* c)
		{
			for (auto& [key, s] : c->staging) if (s) s->Release();
			c->staging.clear();
		}

		void start_capture(IDirect3DDevice9* dev, std::string name)
		{
			if (!device_recordable(dev, "Capture"))
				return;

			if (name.empty())
			{
				SYSTEMTIME st;
				GetLocalTime(&st);
				name = std::format("frame_{:04}{:02}{:02}_{:02}{:02}{:02}", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
			}

			const auto dir = output_dir();
			std::filesystem::create_directories(dir);
			auto* c = new capture;
			c->dev = dev;
			c->name = name;
			c->final_path = dir + "\\" + name + ".ds1cap";
			c->part_path = c->final_path + ".part";
			c->w.file = fopen(c->part_path.c_str(), "wb");
			if (!c->w.file)
			{
				log(std::format("Could not create {}", c->part_path), shared::common::LOG_TYPE::LOG_TYPE_ERROR);
				delete c;
				return;
			}
			setvbuf(c->w.file, nullptr, _IOFBF, 8 << 20);

			if (main_game())
			{
				// The last frame's conversion may have left the device in FFP state; the game only sets what it thinks changed.
				auto& ffp = shared::common::ffp_state::get();
				ffp.disengage(dev);
				ffp.restore_rigid_source(dev);
				ffp.restore_textures(dev);
				s_main_frame = true;
				s_main_drop = !game::vanilla_view::active();
			}

			const auto h = describe_device(dev, c);
			c->w.raw(&h, sizeof(h));

			c->started = std::chrono::steady_clock::now();
			s_cap = c;
			s_capturing = true;
			log(std::format("Capturing frame {} -> {}", s_frame_index, c->final_path));
		}

		void finish_capture()
		{
			auto* c = s_cap;
			s_capturing = false;
			s_cap = nullptr;

			const auto bytes = c->w.bytes;
			fclose(c->w.file);
			for (auto* obj : c->refs) obj->Release();
			release_staging(c);
			if (c->backbuffer) c->backbuffer->Release();

			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - c->started).count();
			if (MoveFileExA(c->part_path.c_str(), c->final_path.c_str(), MOVEFILE_REPLACE_EXISTING))
			{
				const auto& s = c->stats;
				log(std::format("Captured {}: {} draws, {} clears, {} copies, {} textures, {} buffers, "
					"{} buffer / {} texture updates, {:.1f} MB in {} ms. Unreadable: {} (+{} depth, {} msaa, {} unknown formats)",
					c->name, s.draws, s.clears, s.copies, s.textures, s.buffers, s.buffer_updates, s.texture_updates,
					double(bytes) / (1024.0 * 1024.0), ms, s.unreadable, s.depth, s.msaa, s.unknown_format));
				s_last_capture = c->name;
				s_last_capture_time = std::chrono::steady_clock::now();
			}
			else log(std::format("Could not finalise {}", c->final_path), shared::common::LOG_TYPE::LOG_TYPE_ERROR);
			delete c;
		}

		// A capture cut short by a device Reset: its references go, the partial file is dropped.
		void abort_capture(const char* why)
		{
			auto* c = s_cap;
			main_game_capture_over();
			s_capturing = false;
			s_cap = nullptr;
			fclose(c->w.file);
			for (auto* obj : c->refs) obj->Release();
			release_staging(c);
			if (c->backbuffer) c->backbuffer->Release();
			DeleteFileA(c->part_path.c_str());
			log(std::format("Capture {} abandoned ({})", c->name, why), shared::common::LOG_TYPE::LOG_TYPE_WARN);
			delete c;
		}

		// ---- live stream ----

		void close_ring()
		{
			if (!s_ring)
				return;
			if (s_ring->hdr)
			{
				s_ring->hdr->producer_pid = 0;
				UnmapViewOfFile(s_ring->hdr);
			}
			if (s_ring->mapping)
				CloseHandle(s_ring->mapping);
			delete s_ring;
			s_ring = nullptr;
		}

		double mb(int64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

		// Ends the session. Anything written since the last Present is dropped.
		void end_stream_session(const std::string& why)
		{
			auto* c = s_cap;
			s_capturing = false;
			s_cap = nullptr;
			if (s_ring)
				s_ring->rollback();
			if (c)
			{
				release_staging(c);
				if (c->backbuffer) c->backbuffer->Release();
				log(std::format("Live stream ended ({}) after {} frames ({} drawn), {:.1f} MB; {} objects' contents sent in later frames, {} GPU-side objects not read",
					why, c->frames, c->drawn_frames, mb(int64_t(c->w.bytes)), c->stats.deferred, c->stats.not_sent));
				delete c;
			}
		}

		// The host keeps what it was sent until the address is reused: the game frees a map's resources on a warp, unseen by
		// a stream that holds no references. The host is a 32-bit process, and Remix keeps its copies too. A new session
		// costs a few seconds' resync.
		constexpr uint64_t kMaxSessionContents = 1500ull << 20;

		// ---- back-off ----
		//
		// A host too slow for the game overflows the ring now and then. Each new
		// session costs a resync, so after an overflow the next session waits until
		// the host has been caught up for a while: 2 s, doubling per consecutive
		// overflow up to 60 s. A session that lasts 3 minutes clears the count, and
		// so does a new host.
		uint32_t s_overflows = 0;        // consecutive sessions ended by an overflow
		DWORD s_backoff_ms = 0;          // how long the host must be caught up before the next session
		DWORD s_caught_up_since = 0;     // tick it was first seen caught up, 0 = not yet

		void note_overflow()
		{
			s_overflows++;
			s_backoff_ms = std::min<DWORD>(2000u << std::min<uint32_t>(s_overflows - 1, 5), 60000u);
			s_caught_up_since = 0;
		}

		// ---- stream cost (see cost_timer) ----

		struct cost_window
		{
			uint32_t frames = 0;
			double sum = 0.0, min = 0.0, max = 0.0;

			void add(double ms)
			{
				if (!frames || ms < min) min = ms;
				if (!frames || ms > max) max = ms;
				sum += ms;
				frames++;
			}

			std::string text() const
			{
				return frames ? std::format("{} frames, {:.2f} / {:.2f} / {:.2f} ms", frames, min, sum / frames, max) : "none";
			}
		};

		cost_window s_cost_drawn, s_cost_nodraw;
		int64_t s_cost_bytes = 0;
		DWORD s_cost_since = 0;

		// Ends the frame's cost measurement (present_hk, after stream_present).
		void account_stream_cost(bool drawn, int64_t bytes)
		{
			static const double ms_per_tick = []
			{
				LARGE_INTEGER f;
				QueryPerformanceFrequency(&f);
				return 1000.0 / double(f.QuadPart);
			}();
			const double ms = double(s_frame_ticks) * ms_per_tick;
			s_frame_ticks = 0;
			(drawn ? s_cost_drawn : s_cost_nodraw).add(ms);
			s_cost_bytes += bytes;

			const DWORD now = GetTickCount();
			if (!s_cost_since)
				s_cost_since = now;
			if (now - s_cost_since < 10000)
				return;
			const double seconds = double(now - s_cost_since) / 1000.0;
			log(std::format("Live stream cost (min / avg / max per frame) over {:.0f} s: drawn {}; no-draw {}; {:.1f} MB/s",
				seconds, s_cost_drawn.text(), s_cost_nodraw.text(), mb(s_cost_bytes) / seconds));
			s_cost_drawn = {};
			s_cost_nodraw = {};
			s_cost_bytes = 0;
			s_cost_since = now;
		}

		void start_frame_budget(capture& c)
		{
			const auto& cfg = shared::common::config::get().lookdev;
			c.budget = int64_t(cfg.stream_budget_kb) << 10;
			c.sent = 0;
			c.frame_incomplete = false;
		}

		// A session starts from nothing, as a capture does: everything the frame
		// uses is sent at first use, its contents paced (pace()). The host skips
		// to session_start.
		void start_stream_session(IDirect3DDevice9* dev)
		{
			auto& ring = *s_ring;
			auto* c = new capture;
			c->dev = dev;
			c->streaming = true;
			c->w.ring = &ring;
			c->contents_max = ring.size / 4;
			start_frame_budget(*c);

			ring.epoch = ring.hdr->consumer_epoch;
			ring.published = ring.pos = load64(&ring.hdr->write_pos);
			ring.limit = 0; // read read_pos at the first write
			ring.overflow = false;
			ring.hdr->producer_pid = GetCurrentProcessId();
			ring.hdr->frames_written = 0;
			ring.hdr->frames_consumed = 0; // the host zeroes it too, when it takes the session
			InterlockedExchange64(&ring.hdr->session_start, ring.pos);
			MemoryBarrier();
			InterlockedIncrement(reinterpret_cast<volatile LONG*>(&ring.hdr->session));

			// published with the session's first frame
			const auto h = describe_device(dev, c);
			c->w.rec(REC_STREAM_BEGIN, h);

			c->started = std::chrono::steady_clock::now();
			s_cap = c;
			s_capturing = true;
			s_frame_ticks = 0;
			s_backoff_ms = 0;
			s_caught_up_since = 0;
			const uint32_t session = ring.hdr->session, host_pid = ring.hdr->consumer_pid;
			log(std::format("Live stream session {} started (host pid {})", session, host_pid));
		}

		// After a Present with no session running: whether one may start. The host
		// must be alive and have taken everything published, so the new session
		// finds the ring empty, and after an overflow it must have stayed caught up
		// for the back-off time. A host that attached since the last session
		// started (new epoch) skips to the new session's start and need not catch up.
		bool host_ready()
		{
			auto& ring = *s_ring;
			if (GetTickCount() - ring.hdr->consumer_heartbeat > 3000)
			{
				log("Live stream: the host is gone");
				close_ring();
				return false;
			}
			if (ring.hdr->consumer_epoch != ring.epoch)
			{
				s_overflows = 0;
				return true;
			}
			if (load64(&ring.hdr->read_pos) < ring.published)
			{
				s_caught_up_since = 0;
				return false;
			}
			if (!s_backoff_ms)
				return true;
			const DWORD now = GetTickCount();
			if (!s_caught_up_since)
				s_caught_up_since = now | 1;
			return now - s_caught_up_since >= s_backoff_ms;
		}

		// A host announces itself by creating the stream mapping. Checked every
		// half second or so while there is none.
		void poll_stream(IDirect3DDevice9* dev)
		{
			if (s_ring || s_frame_index % 30 != 0)
				return;
			HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, STREAM_NAME);
			if (!mapping)
				return;
			auto* hdr = static_cast<stream_header*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
			if (!hdr || hdr->magic != STREAM_MAGIC || hdr->version != STREAM_VERSION || hdr->header_size != sizeof(stream_header)
				|| GetTickCount() - hdr->consumer_heartbeat > 3000)
			{
				if (hdr) UnmapViewOfFile(hdr);
				CloseHandle(mapping);
				return;
			}
			if (!device_recordable(dev, "Live stream"))
			{
				UnmapViewOfFile(hdr);
				CloseHandle(mapping);
				return;
			}
			s_ring = new ring_sink;
			s_ring->mapping = mapping;
			s_ring->hdr = hdr;
			s_ring->data = reinterpret_cast<uint8_t*>(hdr) + hdr->header_size;
			s_ring->size = hdr->ring_size;
			s_ring->epoch = hdr->consumer_epoch;
			start_stream_session(dev);
		}

		// End of a streamed frame: publish it whole, or drop it when it did not fit
		// in the space the host has freed. A dropped frame ends the session (the
		// game's record of what the host has no longer holds); present_hk starts
		// the next one once the host has caught up. Then follow the host: a new
		// epoch means it restarted and needs everything again, a stale heartbeat
		// that it is gone. Nothing here waits for it.
		//
		// It also decides what the next frame records. While the host has more
		// than one published frame still to process, the game sends no-draw frames:
		// only what changes resources (creations, contents, the game's buffer
		// writes, CPU-side copies, REC_DESTROY), no per-draw state and no commands.
		// The state shadow keeps what the host has, so the next drawn frame sends
		// the net state changes. The game then records full frames at the rate the
		// host draws them, and does almost no work for the others.
		//
		// Returns the bytes this frame published (0 when dropped).
		int64_t stream_present()
		{
			auto* c = s_cap;
			auto& ring = *s_ring;
			// Read-back staging surfaces are only needed when something new is first
			// used; keeping one per size and format alive costs the game address space.
			release_staging(c);

			const present_info info { (c->frame_incomplete ? PRESENT_INCOMPLETE : 0u) | (c->draw_frame ? 0u : PRESENT_NO_DRAW), c->pending };
			c->w.rec(REC_PRESENT, info);
			if (ring.overflow)
			{
				const int64_t unread = ring.published - load64(&ring.hdr->read_pos);
				note_overflow();
				end_stream_session(std::format("frame {} did not fit: the host had {:.1f} MB still to read. "
					"The frame is dropped; a new session starts once the host has been caught up for {} s",
					c->frames + 1, mb(unread), s_backoff_ms / 1000));
				return 0;
			}
			const int64_t bytes = ring.pos - ring.published;
			ring.commit();
			InterlockedIncrement(reinterpret_cast<volatile LONG*>(&ring.hdr->frames_written));
			c->frames++;
			if (c->draw_frame)
				c->drawn_frames++;
			c->game_state_written = false;
			if (c->draw_frame && !c->frame_incomplete && !c->in_sync)
			{
				c->in_sync = true;
				log(std::format("Live stream in sync after {} frames ({} drawn), {:.1f} MB ({} objects' contents sent in later frames, {} GPU-side objects not read)",
					c->frames, c->drawn_frames, mb(int64_t(c->w.bytes)), c->stats.deferred, c->stats.not_sent));
			}
			start_frame_budget(*c);
			if (s_overflows && std::chrono::steady_clock::now() - c->started > std::chrono::minutes(3))
			{
				log("Live stream: 3 minutes without an overflow, back-off cleared");
				s_overflows = 0;
			}
			// the host is at most one frame behind: record the next frame in full
			c->draw_frame = int32_t(c->frames - ring.hdr->frames_consumed) <= 1;

			if (GetTickCount() - ring.hdr->consumer_heartbeat > 3000)
			{
				end_stream_session("host stopped");
				close_ring();
			}
			else if (ring.hdr->consumer_epoch != ring.epoch)
				end_stream_session("host restarted");
			else if (c->contents > kMaxSessionContents)
				end_stream_session(std::format("{:.0f} MB of objects sent: a new session lets the host free what the game no longer has", mb(int64_t(c->contents))));
			return bytes;
		}

		// ---- input freeze ----
		//
		// While shared::globals::input_frozen is set, every way the game reads input
		// comes back idle: DirectInput (ANSI and wide devices, state and buffered
		// data; the wide GetDeviceState is shared::common::dinput_v2's hook), the
		// user32 key and cursor queries, and XInput. Only calls from the game's own
		// code are blocked, so the proxy's key polling and ImGui keep working.
		//
		// In the game the same hooks hold the game's keyboard and mouse while the F4 menu (or the Remix menu) holds input:
		// user32 GetKeyboardState, and DirectInput keyboard and mouse reads (dinput_v2 only clears the wide GetDeviceState,
		// and the game may read the ANSI one). The cursor and the pad are left alone for the menu. That includes the game's
		// DirectInput joystick (it carries the DIJOYSTATE format): an all-zero DIJOYSTATE is both sticks pushed fully up and
		// left, so clearing it walked the character forward and left for as long as a menu was open.

		POINT s_frozen_cursor {};
		uintptr_t s_game_lo = 0, s_game_hi = 0;
		std::atomic<uint32_t> s_blocked_seen = 0;

		bool from_game(void* return_address)
		{
			const auto a = reinterpret_cast<uintptr_t>(return_address);
			return a >= s_game_lo && a < s_game_hi;
		}

		bool menu_holds_game_keys()
		{
			return shared::globals::remix_menu_open || (shared::globals::imgui_menu_open && !shared::globals::imgui_allow_input_bypass);
		}

		// menu: also while a menu holds input (not for the cursor and the pad).
		bool block_for_game(void* return_address, int api, const char* name, bool menu = true)
		{
			const bool frozen = shared::globals::input_frozen;
			if (!(frozen || (menu && menu_holds_game_keys())) || !from_game(return_address))
				return false;
			// Once per path and reason: which path the game reads shows in the log.
			const uint32_t bit = 1u << (api + (frozen ? 0 : 16));
			if (!(s_blocked_seen.fetch_or(bit) & bit))
				log(std::format("{}: blocked the game's {}", frozen ? "Input freeze" : "Menu open", name));
			return true;
		}

		using get_key_state_t = SHORT(WINAPI*)(int);
		using get_keyboard_state_t = BOOL(WINAPI*)(PBYTE);
		using get_cursor_pos_t = BOOL(WINAPI*)(LPPOINT);
		using xinput_get_state_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
		using di_get_state_t = HRESULT(__stdcall*)(IDirectInputDevice8A*, DWORD, LPVOID);
		using di_get_data_t = HRESULT(__stdcall*)(IDirectInputDevice8A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

		get_key_state_t s_get_async_key_state_og = nullptr;
		get_key_state_t s_get_key_state_og = nullptr;
		get_keyboard_state_t s_get_keyboard_state_og = nullptr;
		get_cursor_pos_t s_get_cursor_pos_og = nullptr;
		xinput_get_state_t s_xinput_get_state_og = nullptr;
		di_get_state_t s_di_get_state_og = nullptr;
		di_get_data_t s_di_get_data_og[2] = {};

		SHORT WINAPI get_async_key_state_hk(int vk)
		{
			return block_for_game(_ReturnAddress(), 0, "GetAsyncKeyState") ? 0 : s_get_async_key_state_og(vk);
		}

		SHORT WINAPI get_key_state_hk(int vk)
		{
			return block_for_game(_ReturnAddress(), 1, "GetKeyState") ? 0 : s_get_key_state_og(vk);
		}

		BOOL WINAPI get_keyboard_state_hk(PBYTE keys)
		{
			const BOOL r = s_get_keyboard_state_og(keys);
			if (!r || !keys)
				return r;
			const bool held = block_for_game(_ReturnAddress(), 2, "GetKeyboardState");
			if (from_game(_ReturnAddress()))
				game::input_trace::on_keyboard_state(keys, held);
			if (held)
				std::memset(keys, 0, 256);
			return r;
		}

		BOOL WINAPI get_cursor_pos_hk(LPPOINT pt)
		{
			if (pt && block_for_game(_ReturnAddress(), 3, "GetCursorPos", false))
			{
				*pt = s_frozen_cursor;
				return TRUE;
			}
			return s_get_cursor_pos_og(pt);
		}

		// The packet number is flipped so a game that only re-reads on a new packet
		// still sees the idle pad.
		DWORD WINAPI xinput_get_state_hk(DWORD user, XINPUT_STATE* state)
		{
			const DWORD r = s_xinput_get_state_og(user, state);
			if (r == ERROR_SUCCESS && state && block_for_game(_ReturnAddress(), 4, "XInputGetState", false))
			{
				state->dwPacketNumber ^= 0x40000000u;
				std::memset(&state->Gamepad, 0, sizeof(state->Gamepad));
			}
			game::input_trace::on_xinput(from_game(_ReturnAddress()), user, r, state);
			return r;
		}

		// Keyboard and mouse states are held by a menu, by their size; a joystick's never is (see above).
		bool is_keyboard_or_mouse_state(DWORD size)
		{
			return size == 256 || size == sizeof(DIMOUSESTATE) || size == sizeof(DIMOUSESTATE2);
		}

		HRESULT __stdcall di_get_state_hk(IDirectInputDevice8A* dev, DWORD size, LPVOID data)
		{
			const HRESULT hr = s_di_get_state_og(dev, size, data);
			if (FAILED(hr) || !data)
				return hr;
			const bool held = is_keyboard_or_mouse_state(size) && block_for_game(_ReturnAddress(), 5, "DirectInput GetDeviceState (ANSI)");
			game::input_trace::on_di_state(from_game(_ReturnAddress()), size, held);
			if (held)
				std::memset(data, 0, size);
			// A pad: held at rest only while input is frozen (block_for_game without the menu); a menu leaves it to the game.
			else if (!is_keyboard_or_mouse_state(size) && block_for_game(_ReturnAddress(), 9, "DirectInput joystick state", false))
				shared::common::neutral_joystick_state(dev, data, size);
			else if (!is_keyboard_or_mouse_state(size) && from_game(_ReturnAddress()))
				shared::common::note_joystick_state(dev, data, size);
			return hr;
		}

		// A DirectInput device's kind, asked once per device: buffered reads carry no size that tells.
		bool is_keyboard_or_mouse_device(IDirectInputDevice8A* dev)
		{
			static std::mutex lock;
			static std::unordered_map<void*, bool> kinds;
			std::lock_guard guard(lock);
			if (const auto it = kinds.find(dev); it != kinds.end())
				return it->second;
			DIDEVCAPS caps { sizeof(DIDEVCAPS) };
			const BYTE type = SUCCEEDED(dev->GetCapabilities(&caps)) ? GET_DIDEVICE_TYPE(caps.dwDevType) : 0;
			return kinds[dev] = type == DI8DEVTYPE_KEYBOARD || type == DI8DEVTYPE_MOUSE;
		}

		// Buffered keyboard and mouse input: drained as usual, then reported empty while a menu holds it.
		template <int Slot>
		HRESULT __stdcall di_get_data_hk(IDirectInputDevice8A* dev, DWORD object_size, LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags)
		{
			const HRESULT hr = s_di_get_data_og[Slot](dev, object_size, data, count, flags);
			if (SUCCEEDED(hr) && count && is_keyboard_or_mouse_device(dev) && block_for_game(_ReturnAddress(), 6 + Slot, "DirectInput GetDeviceData"))
				*count = 0;
			// A pad's buffered input: none while input is frozen.
			else if (SUCCEEDED(hr) && count && !is_keyboard_or_mouse_device(dev) && block_for_game(_ReturnAddress(), 10 + Slot, "DirectInput joystick data", false))
				*count = 0;
			return hr;
		}

		template <class Fn>
		void hook_target(void* target, Fn detour, Fn& og, const char* name)
		{
			if (!target)
			{
				log(std::format("Input freeze: {} not found", name));
				return;
			}
			const auto st = MH_CreateHook(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(&og));
			if (st == MH_ERROR_ALREADY_CREATED)
				return; // another module's hook on the same code (dinput_v2) covers it
			if (st != MH_OK || MH_EnableHook(target) != MH_OK)
				log(std::format("Input freeze: could not hook {}", name), shared::common::LOG_TYPE::LOG_TYPE_ERROR);
		}

		void* proc(const wchar_t* module, const char* name)
		{
			const auto mod = GetModuleHandleW(module);
			return mod ? reinterpret_cast<void*>(GetProcAddress(mod, name)) : nullptr;
		}

		void hook_input_freeze()
		{
			const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
			s_game_lo = base;
			s_game_hi = base + nt->OptionalHeader.SizeOfImage;

			hook_target(proc(L"user32.dll", "GetAsyncKeyState"), &get_async_key_state_hk, s_get_async_key_state_og, "GetAsyncKeyState");
			hook_target(proc(L"user32.dll", "GetKeyState"), &get_key_state_hk, s_get_key_state_og, "GetKeyState");
			hook_target(proc(L"user32.dll", "GetKeyboardState"), &get_keyboard_state_hk, s_get_keyboard_state_og, "GetKeyboardState");
			hook_target(proc(L"user32.dll", "GetCursorPos"), &get_cursor_pos_hk, s_get_cursor_pos_og, "GetCursorPos");
			hook_target(proc(L"xinput1_3.dll", "XInputGetState"), &xinput_get_state_hk, s_xinput_get_state_og, "XInputGetState");

			// DirectInput methods live in vtables; take them from throwaway devices.
			IDirectInput8A* di_a = nullptr;
			IDirectInput8W* di_w = nullptr;
			IDirectInputDevice8A* dev_a = nullptr;
			IDirectInputDevice8W* dev_w = nullptr;
			if (SUCCEEDED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, reinterpret_cast<void**>(&di_a), nullptr))
				&& SUCCEEDED(di_a->CreateDevice(GUID_SysKeyboard, &dev_a, nullptr)))
			{
				auto** vt = *reinterpret_cast<void***>(dev_a);
				hook_target(vt[9], &di_get_state_hk, s_di_get_state_og, "DirectInput GetDeviceState (ANSI)");
				hook_target(vt[10], &di_get_data_hk<0>, s_di_get_data_og[0], "DirectInput GetDeviceData (ANSI)");
			}
			if (SUCCEEDED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&di_w), nullptr))
				&& SUCCEEDED(di_w->CreateDevice(GUID_SysKeyboard, &dev_w, nullptr)))
			{
				auto** vt = *reinterpret_cast<void***>(dev_w);
				hook_target(vt[10], &di_get_data_hk<1>, s_di_get_data_og[1], "DirectInput GetDeviceData (wide)");
			}
			if (dev_a) dev_a->Release();
			if (di_a) di_a->Release();
			if (dev_w) dev_w->Release();
			if (di_w) di_w->Release();
		}

		// The freeze key (game window focused) toggles whether the game sees input.
		void poll_freeze_key()
		{
			static bool key_down = false;
			const int key = shared::common::config::get().lookdev.freeze_input_key;
			const bool down = key && (GetAsyncKeyState(key) & 0x8000) && GetForegroundWindow() == shared::globals::main_window;
			if (down && !key_down)
			{
				if (!shared::globals::input_frozen)
					GetCursorPos(&s_frozen_cursor);
				shared::globals::input_frozen = !shared::globals::input_frozen;
				log(shared::globals::input_frozen ? "Input frozen: the game ignores keyboard, mouse and gamepad" : "Input live");
			}
			key_down = down;
		}

		// A capture is requested with the capture key (game window focused) or
		// by creating <output dir>\capture.request; its first line, if any,
		// names the capture.
		void poll_request()
		{
			static bool key_down = false;
			// The key follows F4's dev view (a player's game has no capture key); the request file stays (the host's).
			const int key = shared::common::config::get().dev_view() ? shared::common::config::get().lookdev.capture_key : 0;
			const bool down = key && (GetAsyncKeyState(key) & 0x8000) && GetForegroundWindow() == shared::globals::main_window;
			if (down && !key_down)
			{
				s_request = true;
				s_request_name.clear();
			}
			key_down = down;

			if (s_frame_index % 15 == 0)
			{
				const auto path = output_dir() + "\\capture.request";
				if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
				{
					std::ifstream f(path);
					std::string line;
					std::getline(f, line);
					f.close();
					DeleteFileA(path.c_str());
					std::erase_if(line, [](char ch) { return !(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-'); });
					s_request = true;
					s_request_name = line;
				}
			}
		}

		// ---- game snapshot (lookdev snap.py) ----
		//
		// rtx_comp\lookdev\snap.request asks for the frame the game presents, without the
		// proxy's overlays, as snap_game.bmp. The back buffer is scaled down on the GPU and
		// read back only once an event query says the copy is done, so the game never
		// waits on it.

		enum class snap_stage { idle, armed, copying };
		snap_stage s_snap = snap_stage::idle;
		IDirect3DSurface9* s_snap_rt = nullptr;
		IDirect3DSurface9* s_snap_sys = nullptr;
		IDirect3DQuery9* s_snap_query = nullptr;
		UINT s_snap_w = 0, s_snap_h = 0;
		unsigned int s_snap_frames = 0;

		void snap_release()
		{
			if (s_snap_query) { s_snap_query->Release(); s_snap_query = nullptr; }
			if (s_snap_sys) { s_snap_sys->Release(); s_snap_sys = nullptr; }
			if (s_snap_rt) { s_snap_rt->Release(); s_snap_rt = nullptr; }
			s_snap = snap_stage::idle;
		}

		void snap_poll()
		{
			if (s_snap == snap_stage::idle && s_frame_index % 10 == 0
				&& GetFileAttributesA((output_dir() + "\\snap.request").c_str()) != INVALID_FILE_ATTRIBUTES)
				s_snap = snap_stage::armed;   // this frame's overlays are skipped (writing_file)
		}

		// Before Present: the armed frame's back buffer, scaled to at most 1920 wide.
		void snap_copy(IDirect3DDevice9* dev)
		{
			if (s_snap != snap_stage::armed)
				return;
			IDirect3DSurface9* bb = nullptr;
			D3DSURFACE_DESC desc {};
			if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb || FAILED(bb->GetDesc(&desc)))
			{
				if (bb) bb->Release();
				snap_release();
				return;
			}
			s_snap_w = std::min<UINT>(desc.Width, 1920);
			s_snap_h = desc.Height * s_snap_w / desc.Width;
			const bool ok = SUCCEEDED(dev->CreateRenderTarget(s_snap_w, s_snap_h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &s_snap_rt, nullptr))
				&& SUCCEEDED(dev->CreateOffscreenPlainSurface(s_snap_w, s_snap_h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &s_snap_sys, nullptr))
				&& SUCCEEDED(dev->StretchRect(bb, nullptr, s_snap_rt, nullptr, D3DTEXF_LINEAR))
				&& SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &s_snap_query))
				&& SUCCEEDED(s_snap_query->Issue(D3DISSUE_END));
			bb->Release();
			if (!ok)
			{
				snap_release();
				return;
			}
			s_snap = snap_stage::copying;
			s_snap_frames = 0;
		}

		// After each Present: read the copy back once the GPU has finished it.
		void snap_finish(IDirect3DDevice9* dev)
		{
			if (s_snap != snap_stage::copying)
				return;
			// Flush only if the query is still pending after a few frames (it never is in practice).
			const DWORD flags = ++s_snap_frames > 8 ? D3DGETDATA_FLUSH : 0;
			if (s_snap_query->GetData(nullptr, 0, flags) != S_OK)
				return;
			D3DLOCKED_RECT lr {};
			if (SUCCEEDED(dev->GetRenderTargetData(s_snap_rt, s_snap_sys)) && SUCCEEDED(s_snap_sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
			{
				const auto tmp = output_dir() + "\\snap_game.tmp";
				if (FILE* f = nullptr; !fopen_s(&f, tmp.c_str(), "wb") && f)
				{
					const uint32_t row = s_snap_w * 4, image = row * s_snap_h;
#pragma pack(push, 1)
					struct { uint16_t type; uint32_t size, reserved, offset; uint32_t hsize; int32_t w, h; uint16_t planes, bpp; uint32_t comp, isize; int32_t xr, yr; uint32_t used, important; } hdr
						{ 0x4D42, 54 + image, 0, 54, 40, int32_t(s_snap_w), -int32_t(s_snap_h), 1, 32, 0, image, 2835, 2835, 0, 0 };
#pragma pack(pop)
					fwrite(&hdr, sizeof(hdr), 1, f);
					for (UINT y = 0; y < s_snap_h; y++)
						fwrite(static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, 1, row, f);
					fclose(f);
					const auto out = output_dir() + "\\snap_game.bmp";
					MoveFileExA(tmp.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING);
				}
				s_snap_sys->UnlockRect();
			}
			DeleteFileA((output_dir() + "\\snap.request").c_str());
			snap_release();
		}

		// ---- device hooks ----

		HRESULT __stdcall present_hk(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty)
		{
			bool drop_present = false;
			// Not recording(): a stream frame that overflowed the ring stops
			// recording early but still ends here, where it is dropped.
			if (!t_internal && s_cap && s_cap->dev == dev)
			{
				std::lock_guard lock(s_mutex);
				internal_scope in;
				if (s_cap && s_cap->dev == dev)
				{
					if (s_cap->streaming)
					{
						const bool drawn = s_cap->draw_frame;
						int64_t bytes;
						{
							cost_timer cost;
							bytes = stream_present();
						}
						account_stream_cost(drawn, bytes);
					}
					else
					{
						if (main_game() && !s_main_drop)
						{
							s_main_reference = true;
							write_reference_image();
							s_main_reference = false;
						}
						else if (!main_game())
							write_reference_image();
						s_cap->w.rec(REC_PRESENT);
						finish_capture();
						drop_present = s_main_drop;
						if (main_game())
							main_game_capture_over();
					}
				}
			}

			if (!t_internal && !main_game())
			{
				internal_scope in;
				snap_copy(dev);
			}

			const auto hr = drop_present ? D3D_OK : s_present_og(dev, src, dst, wnd, dirty);

			if (!t_internal)
			{
				s_frame_index++;
				if (!main_game())
				{
					{
						internal_scope in;
						snap_finish(dev);
					}
					snap_poll();
					poll_freeze_key();
				}
				poll_request();
				// Nothing starts on a lost device: its references would keep the game's Reset
				// failing (on_reset ends what was already running).
				const bool presented = SUCCEEDED(hr);
				if (presented && s_request && !s_cap && (!main_game() || main_game_ready()))
				{
					// starts right after a Present so the file holds one whole frame
					s_request = false;
					std::lock_guard lock(s_mutex);
					start_capture(dev, s_request_name);
				}
				else if (presented && s_request && s_cap && s_cap->streaming)
				{
					// A capture file is one whole frame from a fresh recorder: the stream
					// pauses for it and resumes as a new session, which the host follows.
					s_request = false;
					std::lock_guard lock(s_mutex);
					end_stream_session("file capture");
					start_capture(dev, s_request_name);
				}
				if (presented && !s_cap && !main_game())
				{
					// Streams start (or resume after a file capture or a dropped frame)
					// right after a Present too, once the host is ready for them.
					std::lock_guard lock(s_mutex);
					if (!s_ring)
						poll_stream(dev);
					else if (host_ready())
						start_stream_session(dev);
				}
			}
			return hr;
		}

		HRESULT __stdcall update_surface_hk(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* src_rect, IDirect3DSurface9* dst, const POINT* dst_point)
		{
			if (recording(dev))
			{
				std::lock_guard lock(s_mutex);
				cost_timer cost;
				internal_scope in;
				const bool known = s_cap->surfaces.contains(src);
				update_surface u {};
				u.src = ref_surface(src, true); // a copy's source is never left for later
				if (known && src) refresh_surface(src);
				u.dst = ref_surface(dst);
				if (src_rect) { u.has_rect = 1; u.r = to_rect(*src_rect); }
				if (dst_point) { u.has_point = 1; u.x = dst_point->x; u.y = dst_point->y; }
				s_cap->w.rec(REC_UPDATE_SURFACE, u);
				s_cap->stats.copies++;
			}
			return s_update_surface_og(dev, src, src_rect, dst, dst_point);
		}

		HRESULT __stdcall update_texture_hk(IDirect3DDevice9* dev, IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst)
		{
			if (recording(dev))
			{
				std::lock_guard lock(s_mutex);
				cost_timer cost;
				internal_scope in;
				// A copy's source is never left for later: re-read in full if known
				// (which settles pending contents too), else sent in full now.
				if (const auto it = s_cap->textures.find(src); it != s_cap->textures.end()
					&& (!s_cap->streaming || it->second.contents != CONTENTS_NONE)) // a stream never re-reads what it does not send
				{
					snapshot_texture(src, it->second, 0);
					if (it->second.pending)
					{
						it->second.pending = false;
						s_cap->pending--;
					}
				}
				const update_texture u { ref_texture(src, true), ref_texture(dst) };
				s_cap->w.rec(REC_UPDATE_TEXTURE, u);
				s_cap->stats.copies++;
			}
			return s_update_texture_og(dev, src, dst);
		}

		HRESULT __stdcall stretch_rect_hk(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* src_rect, IDirect3DSurface9* dst, const RECT* dst_rect, D3DTEXTUREFILTERTYPE filter)
		{
			if (recording_draws(dev))
			{
				std::lock_guard lock(s_mutex);
				cost_timer cost;
				internal_scope in;
				stretch_rect s {};
				s.src = ref_surface(src);
				s.dst = ref_surface(dst);
				if (src_rect) { s.has_src_rect = 1; s.src_rect = to_rect(*src_rect); }
				if (dst_rect) { s.has_dst_rect = 1; s.dst_rect = to_rect(*dst_rect); }
				s.filter = static_cast<uint32_t>(filter);
				s_cap->w.rec(REC_STRETCH_RECT, s);
				s_cap->stats.copies++;
			}
			return s_stretch_rect_og(dev, src, src_rect, dst, dst_rect, filter);
		}

		HRESULT __stdcall color_fill_hk(IDirect3DDevice9* dev, IDirect3DSurface9* surface, const RECT* r, D3DCOLOR color)
		{
			if (recording_draws(dev))
			{
				std::lock_guard lock(s_mutex);
				cost_timer cost;
				internal_scope in;
				color_fill f {};
				f.surface = ref_surface(surface);
				if (r) { f.has_rect = 1; f.r = to_rect(*r); }
				f.color = color;
				s_cap->w.rec(REC_COLOR_FILL, f);
				s_cap->stats.copies++;
			}
			return s_color_fill_og(dev, surface, r, color);
		}

		// The effect instance whose particles a UP draw belongs to (game::fx_draw_owner), written before the draw when it
		// differs from the last one written this frame. The host puts it back before it replays the draw.
		unsigned s_fx_owner_written = 0;
		void write_fx_owner()
		{
			const unsigned owner = game::fx_draw_owner::current();
			if (owner != s_fx_owner_written)
			{
				s_cap->w.rec(REC_GAME_FX_OWNER, id_only { owner });
				s_fx_owner_written = owner;
			}
		}

		// What the proxy's modules read from game memory, for the lookdev host
		// (game::host_feed). Written at the frame's first BeginScene: in game the
		// proxy updates its lights there, after the engine has culled the frame.
		void write_game_state()
		{
			auto& c = *s_cap;
			game_frame f {};
			float x, y, z;
			if (game::camera::get_override_pos(x, y, z))
			{
				f.flags |= GAME_FRAME_CAMERA;
				f.camera[0] = x; f.camera[1] = y; f.camera[2] = z;
			}
			if (game::camera::get_char_pos(x, y, z))
			{
				f.flags |= GAME_FRAME_PLAYER;
				f.player[0] = x; f.player[1] = y; f.player[2] = z;
			}
			if (float heading; game::camera::get_char_heading(heading))
			{
				f.flags |= GAME_FRAME_HEADING;
				f.heading = heading;
			}
			if (game::estus::player_flask_empty())
				f.flags |= GAME_FRAME_ESTUS_EMPTY;
			if (game::atmosphere::sky_in_shot_scene())
				f.flags |= GAME_FRAME_SKY_IN_SHOT_SCENE;
			f.map_id = game::map_lights::current_map_id();
			c.w.rec(REC_GAME_FRAME, f);

			const auto& lights = game::point_lights::harvest();
			const lookdev::game_lights gl { static_cast<uint32_t>(lights.size()), sizeof(game::point_lights::light) };
			c.w.rec(REC_GAME_LIGHTS, gl, lights.data(), lights.size() * sizeof(game::point_lights::light));

			// The live effect instances, for the host's effect lights (game::ffx_lights): which effect, where, how old, whether its
			// emitters still run, and the level its light had. The child state only for effects that have a light row.
			auto effects = game::ffx_instances::take_snapshot([](int ffx_id) { return game::ffx_lights::find(ffx_id) != nullptr; });
			for (auto& e : effects)
				e.level = game::ffx_lights::level_of(e.address);
			const lookdev::game_ffx gf { static_cast<uint32_t>(effects.size()), sizeof(game::ffx_instances::snapshot) };
			c.w.rec(REC_GAME_FFX, gf, effects.data(), effects.size() * sizeof(game::ffx_instances::snapshot));
			// The host starts each frame with no owner (host_feed, REC_GAME_FRAME).
			s_fx_owner_written = 0;
			c.game_state_written = true;
		}

		HRESULT __stdcall begin_scene_hk(IDirect3DDevice9* dev)
		{
			if (recording_draws(dev))
			{
				std::lock_guard lock(s_mutex);
				cost_timer cost;
				internal_scope in;
				if (!s_cap->game_state_written)
					write_game_state();
				s_cap->w.rec(REC_BEGIN_SCENE);
			}
			return s_begin_scene_og(dev);
		}

		HRESULT __stdcall end_scene_hk(IDirect3DDevice9* dev)
		{
			if (recording_draws(dev))
			{
				std::lock_guard lock(s_mutex);
				cost_timer cost;
				s_cap->w.rec(REC_END_SCENE);
			}
			return s_end_scene_og(dev);
		}

		HRESULT __stdcall clear_hk(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
		{
			if (recording_draws(dev))
			{
				std::lock_guard lock(s_mutex);
				cost_timer cost;
				internal_scope in;
				snapshot_state(dev, true);
				const DWORD n = rects ? count : 0;
				s_cap->w.rec(REC_CLEAR, clear { n, flags, color, z, stencil }, rects, n * sizeof(D3DRECT));
				s_cap->stats.clears++;
			}
			return s_clear_og(dev, count, rects, flags, color, z, stencil);
		}

		HRESULT __stdcall draw_hk(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT start, UINT count)
		{
			if (recording_draws(dev))
			{
				std::lock_guard lock(s_mutex);
				cost_timer cost;
				internal_scope in;
				snapshot_state(dev, false);
				s_cap->w.rec(REC_DRAW, draw { static_cast<uint32_t>(type), start, count });
				s_cap->stats.draws++;
			}
			if (s_main_drop)
				return D3D_OK;
			return s_draw_og(dev, type, start, count);
		}

		HRESULT __stdcall draw_indexed_hk(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT base, UINT min_index, UINT num_vertices, UINT start_index, UINT count)
		{
			if (recording_draws(dev))
			{
				std::lock_guard lock(s_mutex);
				cost_timer cost;
				internal_scope in;
				snapshot_state(dev, false);
				s_cap->w.rec(REC_DRAW_INDEXED, draw_indexed { static_cast<uint32_t>(type), base, min_index, num_vertices, start_index, count });
				s_cap->stats.draws++;
			}
			if (s_main_drop)
				return D3D_OK;
			return s_draw_indexed_og(dev, type, base, min_index, num_vertices, start_index, count);
		}

		HRESULT __stdcall draw_up_hk(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT count, const void* vertices, UINT stride)
		{
			if (recording_draws(dev) && vertices)
			{
				std::lock_guard lock(s_mutex);
				cost_timer cost;
				internal_scope in;
				snapshot_state(dev, false);
				const UINT bytes = primitive_vertex_count(type, count) * stride;
				write_fx_owner();
				s_cap->w.rec(REC_DRAW_UP, draw_up { static_cast<uint32_t>(type), count, stride, bytes }, vertices, bytes);
				s_cap->stats.draws++;
				// D3D9 unbinds stream 0 after a UP draw, in the game and in the replay alike
				s_cap->shadow.stream[0].reset();
			}
			if (s_main_drop)
				return D3D_OK;
			return s_draw_up_og(dev, type, count, vertices, stride);
		}

		HRESULT __stdcall draw_indexed_up_hk(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT min_index, UINT num_vertices, UINT count,
			const void* indices, D3DFORMAT index_format, const void* vertices, UINT stride)
		{
			if (recording_draws(dev) && indices && vertices)
			{
				std::lock_guard lock(s_mutex);
				cost_timer cost;
				internal_scope in;
				snapshot_state(dev, false);
				const UINT index_bytes = primitive_vertex_count(type, count) * (index_format == D3DFMT_INDEX32 ? 4u : 2u);
				const UINT vertex_bytes = (min_index + num_vertices) * stride;
				auto& c = *s_cap;
				write_fx_owner();
				const draw_indexed_up d { static_cast<uint32_t>(type), min_index, num_vertices, count,
					static_cast<uint32_t>(index_format), stride, index_bytes, vertex_bytes };
				c.w.begin(REC_DRAW_INDEXED_UP, sizeof(d) + index_bytes + vertex_bytes);
				c.w.raw(&d, sizeof(d));
				c.w.raw(indices, index_bytes);
				c.w.raw(vertices, vertex_bytes);
				c.stats.draws++;
				// D3D9 unbinds stream 0 and the indices after a UP draw
				c.shadow.stream[0].reset();
				c.shadow.indices.reset();
			}
			if (s_main_drop)
				return D3D_OK;
			return s_draw_indexed_up_og(dev, type, min_index, num_vertices, count, indices, index_format, vertices, stride);
		}

		// ---- creation hooks (live streams) ----
		//
		// A stream holds no references, so the game frees resources as usual and
		// a new object can land on a freed address. Every creation is checked
		// against what was sent; a hit is sent as REC_DESTROY first, so the host
		// never mistakes the newcomer for the old object.

		void on_created(const void* obj)
		{
			if (!obj || !s_capturing.load(std::memory_order_relaxed) || t_internal)
				return;
			std::lock_guard lock(s_mutex);
			cost_timer cost;
			if (s_cap && s_cap->streaming)
				forget(obj);
		}

		using create_texture_t = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
		using create_volume_t = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9**, HANDLE*);
		using create_cube_t = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*);
		using create_vb_t = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
		using create_ib_t = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);
		using create_rt_t = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
		using create_plain_t = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9**, HANDLE*);
		using create_decl_t = HRESULT(__stdcall*)(IDirect3DDevice9*, const D3DVERTEXELEMENT9*, IDirect3DVertexDeclaration9**);
		using create_vs_t = HRESULT(__stdcall*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
		using create_ps_t = HRESULT(__stdcall*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);

		create_texture_t s_create_texture_og = nullptr;
		create_volume_t s_create_volume_og = nullptr;
		create_cube_t s_create_cube_og = nullptr;
		create_vb_t s_create_vb_og = nullptr;
		create_ib_t s_create_ib_og = nullptr;
		create_rt_t s_create_rt_og = nullptr;
		create_rt_t s_create_ds_og = nullptr; // same shape: Discard in place of Lockable
		create_plain_t s_create_plain_og = nullptr;
		create_decl_t s_create_decl_og = nullptr;
		create_vs_t s_create_vs_og = nullptr;
		create_ps_t s_create_ps_og = nullptr;

		template <class T>
		HRESULT created(HRESULT hr, T** out)
		{
			if (SUCCEEDED(hr) && out)
				on_created(*out);
			return hr;
		}

		HRESULT __stdcall create_texture_hk(IDirect3DDevice9* d, UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9** o, HANDLE* s)
		{ return created(s_create_texture_og(d, w, h, l, u, f, p, o, s), o); }
		HRESULT __stdcall create_volume_hk(IDirect3DDevice9* d, UINT w, UINT h, UINT dp, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DVolumeTexture9** o, HANDLE* s)
		{ return created(s_create_volume_og(d, w, h, dp, l, u, f, p, o, s), o); }
		HRESULT __stdcall create_cube_hk(IDirect3DDevice9* d, UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture9** o, HANDLE* s)
		{ return created(s_create_cube_og(d, e, l, u, f, p, o, s), o); }
		HRESULT __stdcall create_vb_hk(IDirect3DDevice9* d, UINT n, DWORD u, DWORD fvf, D3DPOOL p, IDirect3DVertexBuffer9** o, HANDLE* s)
		{ return created(s_create_vb_og(d, n, u, fvf, p, o, s), o); }
		HRESULT __stdcall create_ib_hk(IDirect3DDevice9* d, UINT n, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DIndexBuffer9** o, HANDLE* s)
		{ return created(s_create_ib_og(d, n, u, f, p, o, s), o); }
		HRESULT __stdcall create_rt_hk(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD q, BOOL b, IDirect3DSurface9** o, HANDLE* s)
		{ return created(s_create_rt_og(d, w, h, f, m, q, b, o, s), o); }
		HRESULT __stdcall create_ds_hk(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD q, BOOL b, IDirect3DSurface9** o, HANDLE* s)
		{ return created(s_create_ds_og(d, w, h, f, m, q, b, o, s), o); }
		HRESULT __stdcall create_plain_hk(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DPOOL p, IDirect3DSurface9** o, HANDLE* s)
		{ return created(s_create_plain_og(d, w, h, f, p, o, s), o); }
		HRESULT __stdcall create_decl_hk(IDirect3DDevice9* d, const D3DVERTEXELEMENT9* e, IDirect3DVertexDeclaration9** o)
		{ return created(s_create_decl_og(d, e, o), o); }
		HRESULT __stdcall create_vs_hk(IDirect3DDevice9* d, const DWORD* f, IDirect3DVertexShader9** o)
		{ return created(s_create_vs_og(d, f, o), o); }
		HRESULT __stdcall create_ps_hk(IDirect3DDevice9* d, const DWORD* f, IDirect3DPixelShader9** o)
		{ return created(s_create_ps_og(d, f, o), o); }

		template <class T>
		bool hook_slot(void** vt, int slot, void* detour, T& original)
		{
			return MH_CreateHook(vt[slot], detour, reinterpret_cast<void**>(&original)) == MH_OK
				&& MH_QueueEnableHook(vt[slot]) == MH_OK;
		}

		// Created at the first capture and left enabled: every detour passes
		// through unless a capture is running (another module's
		// MH_EnableHook(MH_ALL_HOOKS) would re-enable them anyway).
		void ensure_device_hooks(IDirect3DDevice9* dev)
		{
			if (s_device_hooks)
				return;
			auto** vt = *reinterpret_cast<void***>(dev);
			const bool ok =
				hook_slot(vt, VT_UPDATE_SURFACE, reinterpret_cast<void*>(&update_surface_hk), s_update_surface_og) &&
				hook_slot(vt, VT_UPDATE_TEXTURE, reinterpret_cast<void*>(&update_texture_hk), s_update_texture_og) &&
				hook_slot(vt, VT_STRETCH_RECT, reinterpret_cast<void*>(&stretch_rect_hk), s_stretch_rect_og) &&
				hook_slot(vt, VT_COLOR_FILL, reinterpret_cast<void*>(&color_fill_hk), s_color_fill_og) &&
				hook_slot(vt, VT_BEGIN_SCENE, reinterpret_cast<void*>(&begin_scene_hk), s_begin_scene_og) &&
				hook_slot(vt, VT_END_SCENE, reinterpret_cast<void*>(&end_scene_hk), s_end_scene_og) &&
				hook_slot(vt, VT_CLEAR, reinterpret_cast<void*>(&clear_hk), s_clear_og) &&
				hook_slot(vt, VT_DRAW_PRIMITIVE, reinterpret_cast<void*>(&draw_hk), s_draw_og) &&
				hook_slot(vt, VT_DRAW_INDEXED_PRIMITIVE, reinterpret_cast<void*>(&draw_indexed_hk), s_draw_indexed_og) &&
				hook_slot(vt, VT_DRAW_PRIMITIVE_UP, reinterpret_cast<void*>(&draw_up_hk), s_draw_up_og) &&
				hook_slot(vt, VT_DRAW_INDEXED_PRIMITIVE_UP, reinterpret_cast<void*>(&draw_indexed_up_hk), s_draw_indexed_up_og) &&
				hook_slot(vt, VT_CREATE_TEXTURE, reinterpret_cast<void*>(&create_texture_hk), s_create_texture_og) &&
				hook_slot(vt, VT_CREATE_VOLUME_TEXTURE, reinterpret_cast<void*>(&create_volume_hk), s_create_volume_og) &&
				hook_slot(vt, VT_CREATE_CUBE_TEXTURE, reinterpret_cast<void*>(&create_cube_hk), s_create_cube_og) &&
				hook_slot(vt, VT_CREATE_VERTEX_BUFFER, reinterpret_cast<void*>(&create_vb_hk), s_create_vb_og) &&
				hook_slot(vt, VT_CREATE_INDEX_BUFFER, reinterpret_cast<void*>(&create_ib_hk), s_create_ib_og) &&
				hook_slot(vt, VT_CREATE_RENDER_TARGET, reinterpret_cast<void*>(&create_rt_hk), s_create_rt_og) &&
				hook_slot(vt, VT_CREATE_DEPTH_STENCIL, reinterpret_cast<void*>(&create_ds_hk), s_create_ds_og) &&
				hook_slot(vt, VT_CREATE_OFFSCREEN_PLAIN, reinterpret_cast<void*>(&create_plain_hk), s_create_plain_og) &&
				hook_slot(vt, VT_CREATE_VERTEX_DECL, reinterpret_cast<void*>(&create_decl_hk), s_create_decl_og) &&
				hook_slot(vt, VT_CREATE_VERTEX_SHADER, reinterpret_cast<void*>(&create_vs_hk), s_create_vs_og) &&
				hook_slot(vt, VT_CREATE_PIXEL_SHADER, reinterpret_cast<void*>(&create_ps_hk), s_create_ps_og);
			MH_ApplyQueued();
			if (!ok)
			{
				log("Could not hook the device; captures disabled", shared::common::LOG_TYPE::LOG_TYPE_ERROR);
				return;
			}
			s_device_hooks = true;
		}

		// The input hooks, once: from the capture module's thread, or, without that module, from their own (install_input_hold).
		std::atomic_bool s_input_hold_installed = false;
		void hook_input_freeze_once()
		{
			if (!s_input_hold_installed.exchange(true))
				hook_input_freeze();
		}

		DWORD WINAPI install_input_hold_thread(LPVOID)
		{
			while (!shared::globals::d3d_device_real)
				Sleep(50);
			hook_input_freeze_once();
			return 0;
		}

		// The module is registered once the game window exists, which may be
		// before the device does.
		DWORD WINAPI install_present_hook(LPVOID)
		{
			while (!shared::globals::d3d_device_real)
				Sleep(50);
			hook_input_freeze_once(); // the lookdev's freeze, and the F4 menu's keyboard hold in the game, whatever becomes of Present
			auto** vt = *reinterpret_cast<void***>(shared::globals::d3d_device_real);
			if (MH_CreateHook(vt[VT_PRESENT], reinterpret_cast<void*>(&present_hk), reinterpret_cast<void**>(&s_present_og)) != MH_OK
				|| MH_EnableHook(vt[VT_PRESENT]) != MH_OK)
			{
				log("Could not hook Present; captures disabled", shared::common::LOG_TYPE::LOG_TYPE_ERROR);
				return 1;
			}
			const auto& cfg = shared::common::config::get().lookdev;
			log(std::format("Ready. Capture key 0x{:X}, or create {}\\capture.request; freeze-input key 0x{:X}",
				cfg.capture_key, output_dir(), cfg.freeze_input_key));
			return 0;
		}
	}

	void lookdev_capture::install_input_hold()
	{
		if (const auto t = CreateThread(nullptr, 0, install_input_hold_thread, nullptr, 0, nullptr); t)
			CloseHandle(t);
	}

	lookdev_capture::lookdev_capture()
	{
		if (const auto t = CreateThread(nullptr, 0, install_present_hook, nullptr, 0, nullptr); t)
			CloseHandle(t);
	}

	lookdev_capture::private_scope::private_scope() : prev_(t_internal) { t_internal = true; }
	lookdev_capture::private_scope::~private_scope() { t_internal = prev_; }

	void lookdev_capture::freeze_cursor_here()
	{
		GetCursorPos(&s_frozen_cursor);
	}

	bool lookdev_capture::recent_capture(std::string& name)
	{
		if (s_last_capture.empty() || std::chrono::steady_clock::now() - s_last_capture_time > std::chrono::seconds(4))
			return false;
		name = s_last_capture;
		return true;
	}

	bool lookdev_capture::main_game_frame()
	{
		return s_main_frame.load(std::memory_order_relaxed);
	}

	bool lookdev_capture::writing_file()
	{
		return (s_capturing.load(std::memory_order_relaxed) && s_cap && !s_cap->streaming) || s_snap == snap_stage::armed;
	}

	void lookdev_capture::on_reset()
	{
		std::lock_guard lock(s_mutex);
		if (s_cap && s_cap->streaming)
			end_stream_session("device reset");
		else if (s_cap)
			abort_capture("device reset");
		snap_release();
	}

	lookdev_capture::~lookdev_capture() = default;
}
