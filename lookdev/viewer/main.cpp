// ds1_lookdev_viewer: replays a lookdev frame capture (.ds1cap) on a plain
// x64 D3D9 device and compares the result with the frame the game presented.
//
//   ds1_lookdev_viewer.exe <capture.ds1cap> [--compare] [--out <dir>]
//
// The replay is written next to the capture (or to --out) as vanilla.bmp,
// replay.bmp, diff.bmp and result.txt. --compare exits after that with 0 when
// the two images are identical, 1 when they differ, 2 on error. Without it the
// window keeps replaying the frame: 1 replay, 2 vanilla, 3 difference, Esc quits.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>

#include "../common/replay.hpp"

#pragma comment(lib, "d3d9.lib")

using namespace lookdev;
using namespace lookdev::replay;

namespace
{
	// ---- window ----

	int g_view = 1;
	bool g_quit = false;

	LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		switch (msg)
		{
		case WM_KEYDOWN:
			if (wp == VK_ESCAPE) g_quit = true;
			else if (wp >= '1' && wp <= '3') g_view = static_cast<int>(wp - '0');
			return 0;
		case WM_CLOSE:
		case WM_DESTROY:
			g_quit = true;
			return 0;
		default:
			return DefWindowProcW(hwnd, msg, wp, lp);
		}
	}

	HWND create_window(uint32_t width, uint32_t height, bool visible)
	{
		WNDCLASSW wc {};
		wc.lpfnWndProc = wnd_proc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wc.lpszClassName = L"ds1_lookdev_viewer";
		RegisterClassW(&wc);

		// fit the work area, keeping the aspect
		RECT work;
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
		const double scale = std::min({ 1.0, 0.9 * (work.right - work.left) / double(width), 0.9 * (work.bottom - work.top) / double(height) });
		RECT r { 0, 0, LONG(width * scale), LONG(height * scale) };
		AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
		HWND hwnd = CreateWindowW(wc.lpszClassName, L"DS1 lookdev viewer", WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
		if (hwnd && visible)
			ShowWindow(hwnd, SW_SHOW);
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
}

int wmain(int argc, wchar_t** argv)
{
	std::filesystem::path capture_path, out_dir;
	bool compare_only = false;
	for (int i = 1; i < argc; i++)
	{
		const std::wstring a = argv[i];
		if (a == L"--compare") compare_only = true;
		else if (a == L"--out" && i + 1 < argc) out_dir = argv[++i];
		else capture_path = a;
	}
	if (capture_path.empty())
	{
		std::printf("usage: ds1_lookdev_viewer <capture.ds1cap> [--compare] [--out <dir>]\n");
		return 2;
	}
	if (out_dir.empty())
		out_dir = capture_path.parent_path() / (capture_path.stem().wstring() + L"_replay");
	std::filesystem::create_directories(out_dir);

	capture_file cap;
	if (!cap.open(capture_path.wstring()))
	{
		std::printf("could not read %ls\n", capture_path.c_str());
		return 2;
	}
	const auto& h = cap.header;
	std::printf("%ls: frame %llu, %ux%u fmt %u, %zu records, %.1f MB (game INI %s)\n", capture_path.filename().c_str(),
		static_cast<unsigned long long>(h.frame_index), h.bb_width, h.bb_height, h.bb_format, cap.records.size(),
		double(cap.size) / (1024.0 * 1024.0), h.game_ini);

	HWND hwnd = create_window(h.bb_width, h.bb_height, !compare_only);
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	if (!hwnd || !d3d)
	{
		std::printf("no window / Direct3D9\n");
		return 2;
	}

	D3DPRESENT_PARAMETERS pp {};
	pp.BackBufferWidth = h.bb_width;
	pp.BackBufferHeight = h.bb_height;
	pp.BackBufferFormat = static_cast<D3DFORMAT>(h.bb_format);
	pp.BackBufferCount = 1;
	pp.MultiSampleType = static_cast<D3DMULTISAMPLE_TYPE>(h.multisample);
	pp.MultiSampleQuality = h.multisample_quality;
	pp.SwapEffect = h.multisample ? D3DSWAPEFFECT_DISCARD : static_cast<D3DSWAPEFFECT>(h.swap_effect ? h.swap_effect : D3DSWAPEFFECT_DISCARD);
	pp.hDeviceWindow = hwnd;
	pp.Windowed = TRUE;
	pp.EnableAutoDepthStencil = FALSE; // the game's auto depth buffer is replayed as a plain surface
	pp.Flags = h.present_flags & ~DWORD(D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL);
	pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

	DWORD behavior = h.behavior_flags & ~DWORD(D3DCREATE_PUREDEVICE);
	if (!(behavior & (D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MIXED_VERTEXPROCESSING)))
		behavior |= D3DCREATE_HARDWARE_VERTEXPROCESSING;

	IDirect3DDevice9* dev = nullptr;
	if (const HRESULT hr = d3d->CreateDevice(h.adapter < d3d->GetAdapterCount() ? h.adapter : D3DADAPTER_DEFAULT,
		D3DDEVTYPE_HAL, hwnd, behavior, &pp, &dev); FAILED(hr))
	{
		std::printf("CreateDevice failed (hr=0x%08lX)\n", static_cast<unsigned long>(hr));
		return 2;
	}

	replayer rp(cap);
	rp.dev = dev;
	const auto t0 = GetTickCount64();
	rp.run();
	const auto t1 = GetTickCount64();

	image_buf replay;
	if (!rp.read_backbuffer(replay))
	{
		std::printf("could not read the replayed back buffer\n");
		return 2;
	}
	std::printf("replayed %u draws in %llu ms, %u call errors\n", rp.draws, static_cast<unsigned long long>(t1 - t0), rp.errors);

	compare_result cmp;
	if (rp.reference)
		cmp = compare(*rp.reference, replay);

	write_bmp(out_dir / L"replay.bmp", replay);
	FILE* rf = _wfopen((out_dir / L"result.txt").c_str(), L"w");
	if (rp.reference && cmp.comparable)
	{
		write_bmp(out_dir / L"vanilla.bmp", *rp.reference);
		write_bmp(out_dir / L"diff.bmp", cmp.diff);
		const double pct = 100.0 * double(cmp.differing) / double(cmp.pixels);
		std::printf("%s: %llu of %llu pixels differ (%.4f%%), max channel diff %u, mean %.5f\n",
			cmp.differing ? "MISMATCH" : "IDENTICAL",
			static_cast<unsigned long long>(cmp.differing), static_cast<unsigned long long>(cmp.pixels), pct, cmp.max_diff, cmp.mean_diff);
		if (rf)
			std::fprintf(rf, "identical=%d\ndiffering_pixels=%llu\npixels=%llu\nmax_channel_diff=%u\nmean_channel_diff=%.6f\ndraws=%u\ncall_errors=%u\n",
				cmp.differing ? 0 : 1, static_cast<unsigned long long>(cmp.differing), static_cast<unsigned long long>(cmp.pixels),
				cmp.max_diff, cmp.mean_diff, rp.draws, rp.errors);
	}
	else
	{
		std::printf("no comparable reference image in the capture\n");
		if (rf) std::fprintf(rf, "identical=-1\ndraws=%u\ncall_errors=%u\n", rp.draws, rp.errors);
	}
	if (rf) fclose(rf);
	std::printf("output: %ls\n", out_dir.c_str());

	if (compare_only)
		return (rp.reference && cmp.comparable && !cmp.differing) ? 0 : 1;

	dev->Present(nullptr, nullptr, nullptr, nullptr);
	wchar_t title[256];
	while (pump())
	{
		if (g_view == 1)
			rp.run();
		else if (g_view == 2 && rp.reference)
			rp.blit_image(*rp.reference);
		else if (g_view == 3 && cmp.comparable)
			rp.blit_image(cmp.diff);
		swprintf_s(title, L"DS1 lookdev viewer - %s - %llu px differ (max %u) - 1 replay, 2 vanilla, 3 diff",
			g_view == 1 ? L"replay" : g_view == 2 ? L"vanilla" : L"diff x16",
			static_cast<unsigned long long>(cmp.differing), cmp.max_diff);
		SetWindowTextW(hwnd, title);
		dev->Present(nullptr, nullptr, nullptr, nullptr);
		if (g_view != 1) Sleep(16);
	}
	return 0;
}
