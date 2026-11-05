// ds1_lookdev_host: renders the game's frames through the DS1 proxy and RTX
// Remix, exactly as the shipped game renders them.
//
//   ds1_lookdev_host.exe <capture.ds1cap> [--proxy <dll>] -lookdev-host
//       replays one captured frame over and over
//   ds1_lookdev_host.exe --live [--proxy <dll>] -lookdev-host
//       follows a game running in lookdev mode, frame by frame, through the
//       shared-memory stream (capture_format.hpp, stream_header)
//
// 32-bit, because it loads the game's own 32-bit proxy (which loads the Remix
// bridge client). It must run from the game's DATA folder: Remix resolves
// rtx.conf and the mods folder from the client exe's path. lookdev.py host
// copies it there along with a copy of the proxy (d3d9_lookdev.dll), so the
// d3d9.dll a running game holds is never touched.
//
// -lookdev-host on the command line puts the proxy in host mode (no game
// memory, no game hooks). The window class contains "DARK SOULS" because that
// is how the proxy finds its window and starts its modules, the F4 menu
// included. Alt+X opens the Remix menu as in game.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <memory>
#include <thread>

#include "../common/replay.hpp"

using namespace lookdev;
using namespace lookdev::replay;

namespace
{
	bool g_quit = false;

	LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		switch (msg)
		{
		case WM_CLOSE:
		case WM_DESTROY:
			g_quit = true;
			return 0;
		default:
			return DefWindowProcW(hwnd, msg, wp, lp);
		}
	}

	HWND create_window(uint32_t width, uint32_t height)
	{
		WNDCLASSW wc {};
		wc.lpfnWndProc = wnd_proc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wc.lpszClassName = L"DARK SOULS lookdev host";
		RegisterClassW(&wc);

		RECT work;
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
		const double scale = std::min({ 1.0, 0.9 * (work.right - work.left) / double(width), 0.9 * (work.bottom - work.top) / double(height) });
		RECT r { 0, 0, LONG(width * scale), LONG(height * scale) };
		AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
		HWND hwnd = CreateWindowW(wc.lpszClassName, L"DS1 lookdev - Remix", WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
		if (hwnd)
		{
			ShowWindow(hwnd, SW_SHOW);
			UpdateWindow(hwnd);
		}
		return hwnd;
	}

	bool pump()
	{
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		return !g_quit;
	}

	// ---- the proxy ----

	struct proxy_api
	{
		using create_fn = IDirect3D9*(WINAPI*)(UINT);
		using feed_fn = void(__cdecl*)(uint32_t, const void*, uint32_t);
		using name_fn = void(__cdecl*)(IDirect3DBaseTexture9*, const char*);

		IDirect3D9* d3d = nullptr;
		feed_fn feed = nullptr;
		name_fn name_texture = nullptr;

		bool load(const std::filesystem::path& path)
		{
			// The proxy imports d3dx9_43.dll, which imports d3d9.dll, and the loader
			// looks in this exe's folder first: that is the game's DATA\d3d9.dll, the
			// deployed proxy, which must never start inside the host. With the
			// system d3d9.dll already loaded, the import binds to it instead.
			// The same goes for the proxy's dinput8.dll import: DATA may hold an input
			// mod (Dark Souls Input Customizer), which refuses any exe but the game.
			for (const wchar_t* dll : { L"\\d3d9.dll", L"\\dinput8.dll" })
			{
				wchar_t system_dll[MAX_PATH];
				GetSystemDirectoryW(system_dll, MAX_PATH);
				wcscat_s(system_dll, dll);
				if (!LoadLibraryW(system_dll))
				{
					std::printf("could not load the system %ls\n", dll + 1);
					return false;
				}
			}

			HMODULE mod = LoadLibraryW(path.c_str());
			if (!mod)
			{
				std::printf("could not load the proxy %ls (error %lu)\n", path.c_str(), GetLastError());
				return false;
			}
			auto create = reinterpret_cast<create_fn>(GetProcAddress(mod, "Direct3DCreate9"));
			feed = reinterpret_cast<feed_fn>(GetProcAddress(mod, "lookdev_host_feed"));
			name_texture = reinterpret_cast<name_fn>(GetProcAddress(mod, "lookdev_host_texture_name"));
			if (!create)
			{
				std::printf("the proxy exports no Direct3DCreate9\n");
				return false;
			}
			if (!feed || !name_texture)
				std::printf("warning: this proxy build has no lookdev host feed: no game lights, camera or texture names\n");

			// Direct3DCreate9 before any window: it runs the bridge's SYN/ACK
			// handshake, and the proxy starts its modules (and the Remix API, which
			// talks to the bridge at once) as soon as it sees a "DARK SOULS" window.
			// API calls sent ahead of the handshake make the bridge server exit.
			d3d = create(D3D_SDK_VERSION);
			if (!d3d)
				std::printf("Direct3DCreate9 failed\n");
			return d3d != nullptr;
		}

		// Last game state seen, for the window title.
		uint32_t lights = 0;
		bool camera = false;
		unsigned map_id = 0;

		// Game state the proxy would read from game memory, from the capture.
		void attach(replayer& rp)
		{
			rp.on_game_state = [this, &rp](const record& rec)
			{
				if (rec.type == REC_GAME_LIGHTS && rec.size >= sizeof(lookdev::game_lights))
					lights = read<lookdev::game_lights>(rec.payload).count;
				else if (rec.type == REC_GAME_FRAME && rec.size >= sizeof(game_frame))
				{
					const auto f = read<game_frame>(rec.payload);
					camera = (f.flags & GAME_FRAME_CAMERA) != 0;
					map_id = f.map_id;
				}
				if (rec.type == REC_TEXTURE_NAME)
				{
					if (!name_texture || rec.size <= sizeof(id_only)) return;
					const std::string name(reinterpret_cast<const char*>(rec.payload) + sizeof(id_only), rec.size - sizeof(id_only));
					if (auto* t = rp.texture(read<id_only>(rec.payload).id))
						name_texture(t, name.c_str());
				}
				else if (feed)
					feed(rec.type, rec.payload, rec.size);
			};
		}
	};

	// The window and the device for a stream or capture header.
	IDirect3DDevice9* create_device(IDirect3D9* d3d, const file_header& h, HWND& hwnd)
	{
		hwnd = create_window(h.bb_width, h.bb_height);
		if (!hwnd)
			return nullptr;

		// Let the proxy's window thread find the window and register its modules
		// (renderer, F4 menu) before the device exists, as in the game.
		for (int i = 0; i < 30 && pump(); i++)
			Sleep(50);

		D3DPRESENT_PARAMETERS pp {};
		pp.BackBufferWidth = h.bb_width;
		pp.BackBufferHeight = h.bb_height;
		pp.BackBufferFormat = static_cast<D3DFORMAT>(h.bb_format);
		pp.BackBufferCount = 1;
		pp.MultiSampleType = static_cast<D3DMULTISAMPLE_TYPE>(h.multisample);
		pp.MultiSampleQuality = h.multisample_quality;
		pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
		pp.hDeviceWindow = hwnd;
		pp.Windowed = TRUE;
		pp.EnableAutoDepthStencil = FALSE; // the game's depth buffer is replayed as a plain surface
		pp.Flags = h.present_flags & ~DWORD(D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL);
		pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

		DWORD behavior = h.behavior_flags & ~DWORD(D3DCREATE_PUREDEVICE);
		if (!(behavior & (D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MIXED_VERTEXPROCESSING)))
			behavior |= D3DCREATE_HARDWARE_VERTEXPROCESSING;

		IDirect3DDevice9* dev = nullptr;
		if (const HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, behavior, &pp, &dev); FAILED(hr))
		{
			std::printf("CreateDevice failed (hr=0x%08lX)\n", static_cast<unsigned long>(hr));
			return nullptr;
		}
		// Another output size, for tests: DS1_HOST_SIZE=WxH. As the game does it: a device at one size, then a Reset to the setting.
		if (const char* size = std::getenv("DS1_HOST_SIZE"))
		{
			int width = 0, height = 0;
			if (std::sscanf(size, "%dx%d", &width, &height) == 2 && width > 0 && height > 0)
			{
				RECT rect { 0, 0, width, height };
				AdjustWindowRect(&rect, static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE)), FALSE);
				SetWindowPos(hwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER);
				pp.BackBufferWidth = width;
				pp.BackBufferHeight = height;
				const HRESULT reset = dev->Reset(&pp);
				std::printf("output size %dx%d: Reset hr=0x%08lX\n", width, height, static_cast<unsigned long>(reset));
			}
		}
		return dev;
	}

	struct fps_meter
	{
		uint64_t start = GetTickCount64(), last = start;
		uint64_t count = 0;
		double fps = 0.0;

		bool tick()
		{
			count++;
			const auto now = GetTickCount64();
			if (now - last < 500)
				return false;
			fps = 1000.0 * count / double(now - start);
			last = now;
			if (now - start > 3000) { start = now; count = 0; }
			return true;
		}
	};

	// ---- one captured frame ----

	// A replayed frame never changes, so running flat out only heats the GPU. Enough for the denoiser and
	// exposure to settle and the menus to stay usable; --fps 0 runs uncapped.
	constexpr int kReplayFps = 30;

	// A capture to change to without restarting Remix: its path in this file, in the game folder (shots.py). A capture
	// with another back buffer cannot share the device, so it is refused: restart the host for it.
	constexpr const wchar_t* kNextCaptureFile = L"lookdev_next_capture.txt";

	std::filesystem::path take_next_capture()
	{
		if (GetFileAttributesW(kNextCaptureFile) == INVALID_FILE_ATTRIBUTES)
			return {};
		std::string text;
		if (FILE* f = _wfopen(kNextCaptureFile, L"rb"))
		{
			char buffer[1024];
			const size_t n = fread(buffer, 1, sizeof(buffer), f);
			fclose(f);
			text.assign(buffer, n);
		}
		DeleteFileW(kNextCaptureFile);
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
			text.pop_back();
		return std::filesystem::path(std::u8string(text.begin(), text.end()));
	}

	int run_file(proxy_api& proxy, const std::filesystem::path& first_capture, const std::unordered_set<uint32_t>& drop_draws, int max_fps)
	{
		std::filesystem::path capture_path = first_capture;
		auto cap = std::make_unique<capture_file>();
		if (!cap->open(capture_path.wstring()))
		{
			std::printf("could not read %ls\n", capture_path.c_str());
			return 2;
		}
		const auto h = cap->header;
		std::printf("%ls: frame %llu, %ux%u, %zu records\n", capture_path.filename().c_str(),
			static_cast<unsigned long long>(h.frame_index), h.bb_width, h.bb_height, cap->records.size());

		HWND hwnd = nullptr;
		IDirect3DDevice9* dev = create_device(proxy.d3d, h, hwnd);
		if (!dev)
			return 2;

		replayer rp(*cap);
		rp.dev = dev;
		rp.reapply_gpu_targets = false;
		rp.drop_draws = drop_draws;
		proxy.attach(rp);

		fps_meter fps;
		bool first = true;
		while (pump())
		{
			if (const auto next = take_next_capture(); !next.empty())
			{
				auto opened = std::make_unique<capture_file>();
				if (!opened->open(next.wstring()))
					std::printf("could not read %ls\n", next.c_str());
				else if (opened->header.bb_width != h.bb_width || opened->header.bb_height != h.bb_height || opened->header.bb_format != h.bb_format)
					std::printf("%ls has another back buffer: restart the host for it\n", next.c_str());
				else
				{
					rp.reset();
					cap = std::move(opened);
					rp.cap = cap.get();
					rp.first_pass = true;
					capture_path = next;
					first = true;
					fps = fps_meter {};
					SetWindowTextW(hwnd, L"DS1 lookdev - Remix - changing capture");
				}
			}
			const auto t0 = GetTickCount64();
			rp.run();
			dev->Present(nullptr, nullptr, nullptr, nullptr);
			if (first)
			{
				std::printf("first pass: %u draws in %llu ms, %u call errors\n", rp.draws,
					static_cast<unsigned long long>(GetTickCount64() - t0), rp.errors);
				first = false;
			}
			if (fps.tick())
			{
				wchar_t title[256];
				swprintf_s(title, L"DS1 lookdev - Remix - %ls - %.1f fps - F4 proxy, Alt+X Remix",
					capture_path.filename().c_str(), fps.fps);
				SetWindowTextW(hwnd, title);
			}
			if (max_fps > 0)
			{
				const auto spent = GetTickCount64() - t0;
				const auto frame_ms = static_cast<uint64_t>(1000 / max_fps);
				if (spent < frame_ms)
					Sleep(static_cast<DWORD>(frame_ms - spent));
			}
		}

		rp.reset();
		rp.dev = nullptr;
		dev->Release();
		return 0;
	}

	// ---- live stream ----

	// A 64-bit position shared with the game: this is a 32-bit process, where a
	// plain 64-bit load can tear.
	int64_t load64(volatile int64_t* p) { return InterlockedCompareExchange64(p, 0, 0); }

	struct stream_reader
	{
		HANDLE mapping = nullptr;
		stream_header* hdr = nullptr;
		const uint8_t* ring = nullptr;
		uint32_t size = 0;
		uint32_t epoch = 0;
		uint32_t session = 0;         // the session being read, 0 = none yet
		uint32_t stale_session = 0;   // whatever ran before this host attached
		int64_t pos = 0;              // next byte to take from the ring
		std::vector<uint8_t> pending; // taken, not yet parsed
		size_t parsed = 0;
		// Frames in `pending`, indexed as they arrive: the REC_PRESENT offset of
		// the newest one worth drawing (full and complete), npos if none.
		static constexpr size_t npos = SIZE_MAX;
		size_t indexed = 0;
		size_t newest_drawable = npos;
		std::atomic<bool> stop = false;
		std::thread heartbeat;

		bool open()
		{
			const DWORD bytes = sizeof(stream_header) + STREAM_RING_SIZE;
			mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bytes, STREAM_NAME);
			const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
			if (!mapping)
				return false;
			hdr = static_cast<stream_header*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
			if (!hdr)
				return false;
			if (!existed || hdr->magic != STREAM_MAGIC || hdr->version != STREAM_VERSION)
			{
				// a game still holding an old mapping of another layout would never
				// match these, so it simply never attaches
				std::memset(hdr, 0, sizeof(stream_header));
				hdr->header_size = sizeof(stream_header);
				hdr->ring_size = STREAM_RING_SIZE;
				hdr->version = STREAM_VERSION;
				MemoryBarrier();
				hdr->magic = STREAM_MAGIC;
			}
			ring = reinterpret_cast<const uint8_t*>(hdr) + hdr->header_size;
			size = hdr->ring_size;

			hdr->consumer_pid = GetCurrentProcessId();
			hdr->consumer_heartbeat = GetTickCount();
			heartbeat = std::thread([this]
			{
				// Separate from the render loop: Remix can block for seconds (shader
				// compilation), and the game must not take that for a dead host.
				while (!stop)
				{
					hdr->consumer_heartbeat = GetTickCount();
					Sleep(250);
				}
			});

			// A new epoch makes the game start a new session for this host.
			stale_session = hdr->session;
			pos = load64(&hdr->read_pos);
			epoch = hdr->consumer_epoch + 1;
			MemoryBarrier();
			hdr->consumer_epoch = epoch;
			return true;
		}

		void close()
		{
			stop = true;
			if (heartbeat.joinable()) heartbeat.join();
			if (hdr)
			{
				hdr->consumer_pid = 0;
				hdr->consumer_heartbeat = 0;
				UnmapViewOfFile(hdr);
			}
			if (mapping) CloseHandle(mapping);
		}

		// A session the game started after this host attached. The game starts a
		// new one in the same epoch too, after it dropped a frame this host was too
		// far behind to take: whatever is left of the old one is discarded.
		bool new_session() const
		{
			const uint32_t s = hdr->session;
			return s != session && s != stale_session && hdr->producer_pid != 0;
		}

		void begin_session()
		{
			session = hdr->session;
			MemoryBarrier();
			const int64_t start = load64(&hdr->session_start);
			pending.clear();
			parsed = 0;
			indexed = 0;
			newest_drawable = npos;
			pos = std::max<int64_t>(pos, start);
			InterlockedExchange64(&hdr->read_pos, pos);
			hdr->frames_consumed = 0; // paces the game's full frames
		}

		static bool drawable(const present_info& info)
		{
			return !(info.flags & (PRESENT_INCOMPLETE | PRESENT_NO_DRAW));
		}

		void index_frames()
		{
			while (pending.size() - indexed >= sizeof(record_header))
			{
				const auto h = read<record_header>(pending.data() + indexed);
				if (pending.size() - indexed - sizeof(record_header) < h.size)
					break;
				if (h.type == REC_PRESENT)
				{
					present_info info {};
					if (h.size >= sizeof(info)) info = read<present_info>(pending.data() + indexed + sizeof(record_header));
					if (drawable(info))
						newest_drawable = indexed;
				}
				indexed += sizeof(record_header) + h.size;
			}
		}

		// Takes everything published, which frees the ring for the game.
		// Returns false when a new session started (the caller resets first).
		bool pull()
		{
			const int64_t end = load64(&hdr->write_pos);   // before the session check, see stream_header
			MemoryBarrier();
			if (new_session())
				return false;
			if (end <= pos)
				return true;

			if (parsed > (64u << 20) || (parsed && parsed == pending.size()))
			{
				pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(parsed));
				indexed -= parsed;
				newest_drawable = (newest_drawable != npos && newest_drawable >= parsed) ? newest_drawable - parsed : npos;
				parsed = 0;
			}
			while (pos < end)
			{
				const size_t offset = static_cast<size_t>(pos % size);
				const size_t chunk = static_cast<size_t>(std::min<int64_t>(end - pos, size - offset));
				pending.insert(pending.end(), ring + offset, ring + offset + chunk);
				pos += chunk;
			}
			InterlockedExchange64(&hdr->read_pos, pos);
			index_frames();
			return true;
		}

		// The next complete record; its payload stays valid until the next pull().
		bool next(record& out)
		{
			if (pending.size() - parsed < sizeof(record_header))
				return false;
			const auto h = read<record_header>(pending.data() + parsed);
			if (pending.size() - parsed - sizeof(record_header) < h.size)
				return false;
			out = { h.type, h.size, pending.data() + parsed + sizeof(record_header) };
			parsed += sizeof(record_header) + h.size;
			return true;
		}

		// The REC_PRESENT payload of the frame being read, looked up ahead, and
		// whether a later frame worth drawing is already here. The game publishes
		// whole frames only, so once a frame's first record is here, so is the
		// rest of it. Empty (complete, full) for a game that sends none.
		present_info frame_info(const record& first, bool& newer_drawable) const
		{
			present_info info {};
			size_t at = npos; // offset of this frame's REC_PRESENT header
			if (first.type == REC_PRESENT)
			{
				if (first.size >= sizeof(info)) info = read<present_info>(first.payload);
				at = parsed - sizeof(record_header) - first.size;
			}
			else
			{
				for (size_t p = parsed; pending.size() - p >= sizeof(record_header);)
				{
					const auto h = read<record_header>(pending.data() + p);
					if (pending.size() - p - sizeof(record_header) < h.size)
						break;
					if (h.type == REC_PRESENT)
					{
						if (h.size >= sizeof(info)) info = read<present_info>(pending.data() + p + sizeof(record_header));
						at = p;
						break;
					}
					p += sizeof(record_header) + h.size;
				}
			}
			newer_drawable = at != npos && newest_drawable != npos && newest_drawable > at;
			return info;
		}
	};

	int run_live(proxy_api& proxy)
	{
		stream_reader in;
		if (!in.open())
		{
			std::printf("could not create the live stream mapping\n");
			return 2;
		}
		std::printf("waiting for the game (start it with lookdev.ps1)...\n");

		HWND hwnd = nullptr;
		IDirect3DDevice9* dev = nullptr;
		replayer rp;
		proxy.attach(rp);
		rp.reapply_gpu_targets = false;

		uint32_t frames_done = 0;
		bool frame_start = true;
		fps_meter fps;
		uint32_t passed_over = 0; // frames applied without drawing since the last title update
		uint64_t last_title = 0;

		while (pump())
		{
			if (in.new_session())
			{
				in.begin_session();
				rp.reset();
				frames_done = 0;
				frame_start = true;
				std::printf("session %u: game pid %u\n", in.session, static_cast<uint32_t>(in.hdr->producer_pid));
			}
			if (!in.session)
			{
				Sleep(20);
				continue;
			}

			// At a frame boundary, take everything published: it frees the ring for
			// the game, and shows whether a newer frame is here to draw instead.
			if (frame_start && !in.pull())
				continue;              // a new session: picked up at the top

			record rec;
			if (!in.next(rec))
			{
				if (!in.pull())
					continue;          // a new session: picked up at the top
				if (!in.next(rec))
				{
					Sleep(1);
					continue;
				}
			}

			if (rec.type == REC_STREAM_BEGIN)
			{
				const auto h = read<file_header>(rec.payload);
				if (!dev)
				{
					dev = create_device(proxy.d3d, h, hwnd);
					if (!dev)
						break;
					rp.dev = dev;
				}
				frame_start = true;
				continue;
			}
			if (!dev)
				continue;

			if (frame_start)
			{
				// Draw only the newest frame worth drawing. The others (older ones,
				// the game's no-draw frames, and frames that use objects whose contents
				// have not arrived yet) are applied without drawing: resources and
				// their contents go to the device, state is kept in the replayer's
				// shadow and flushed before the next drawn frame. That makes passing
				// over a frame far cheaper than the game makes one, so the host always
				// catches up.
				bool newer = false;
				const auto info = in.frame_info(rec, newer);
				const bool incomplete = (info.flags & PRESENT_INCOMPLETE) != 0;
				rp.skip_commands = newer || !stream_reader::drawable(info);
				frame_start = false;
				if (incomplete && GetTickCount64() - last_title > 500)
				{
					last_title = GetTickCount64();
					wchar_t title[256];
					swprintf_s(title, L"DS1 lookdev - Remix - live, session %u - syncing: %u objects still to come - F4 proxy, Alt+X Remix",
						in.session, info.pending);
					SetWindowTextW(hwnd, title);
				}
			}

			if (rp.apply(rec))
			{
				if (!rp.skip_commands)
				{
					dev->Present(nullptr, nullptr, nullptr, nullptr);
					if (fps.tick())
					{
						wchar_t title[256];
						swprintf_s(title, L"DS1 lookdev - Remix - live, session %u - %.1f fps - %u game frames passed over - map %08X, %u game lights, camera %ls - F4 proxy, Alt+X Remix",
							in.session, fps.fps, passed_over, proxy.map_id, proxy.lights, proxy.camera ? L"ok" : L"MISSING");
						SetWindowTextW(hwnd, title);
						passed_over = 0;
					}
				}
				else passed_over++;
				in.hdr->frames_consumed = ++frames_done;
				frame_start = true;
			}
		}

		rp.reset();
		rp.dev = nullptr;
		if (dev) dev->Release();
		in.close();
		return 0;
	}
}

int wmain(int argc, wchar_t** argv)
{
	std::filesystem::path capture_path;
	std::filesystem::path proxy_path = L"d3d9_lookdev.dll";
	bool live = false;
	int max_fps = kReplayFps;
	std::unordered_set<uint32_t> drop_draws;
	for (int i = 1; i < argc; i++)
	{
		const std::wstring a = argv[i];
		if (a == L"--proxy" && i + 1 < argc) proxy_path = argv[++i];
		else if (a == L"--live") live = true;
		else if (a == L"--fps" && i + 1 < argc) max_fps = _wtoi(argv[++i]);
		else if (a == L"--drop" && i + 1 < argc)
		{
			// Comma-separated draw indices, e.g. --drop 240,343
			for (const wchar_t* s = argv[++i]; *s; )
			{
				wchar_t* end = nullptr;
				const auto v = std::wcstoul(s, &end, 10);
				if (end == s) break;
				drop_draws.insert(static_cast<uint32_t>(v));
				s = (*end == L',') ? end + 1 : end;
			}
		}
		else if (!a.empty() && a[0] != L'-') capture_path = a;
	}
	if (capture_path.empty() && !live)
	{
		std::printf("usage: ds1_lookdev_host (<capture.ds1cap> [--drop i,j,...] [--fps n] | --live) [--proxy <dll>] -lookdev-host\n");
		return 2;
	}
	if (!std::wstring(GetCommandLineW()).contains(L"-lookdev-host"))
	{
		std::printf("refusing to start without -lookdev-host: the proxy would treat this process as the game\n");
		return 2;
	}

	proxy_api proxy;
	if (!proxy.load(proxy_path))
		return 2;

	const int rc = live ? run_live(proxy) : run_file(proxy, capture_path, drop_draws, max_fps);
	proxy.d3d->Release();
	return rc;
}
