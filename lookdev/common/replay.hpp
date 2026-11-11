#pragma once

// Replay of a lookdev frame capture (.ds1cap) onto any IDirect3DDevice9: the
// plain x64 device of ds1_lookdev_viewer, or the proxy's wrapper device in the
// 32-bit ds1_lookdev_host, where it goes through the proxy and Remix. Shared,
// header-only, so both bitnesses build it from the same source.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../src/shared/lookdev/capture_format.hpp"

namespace lookdev::replay
{
	template <class T>
	T read(const uint8_t* p)
	{
		T v;
		std::memcpy(&v, p, sizeof(T));
		return v;
	}

	// ---- capture file ----

	struct record
	{
		uint32_t type;
		uint32_t size;
		const uint8_t* payload;
	};

	struct capture_file
	{
		HANDLE file = INVALID_HANDLE_VALUE;
		HANDLE mapping = nullptr;
		const uint8_t* data = nullptr;
		size_t size = 0;
		file_header header {};
		std::vector<record> records;

		bool open(const std::wstring& path)
		{
			file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return false;
			LARGE_INTEGER li;
			GetFileSizeEx(file, &li);
			size = static_cast<size_t>(li.QuadPart);
			mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
			if (!mapping)
				return false;
			data = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
			if (!data || size < sizeof(file_header))
				return false;
			header = read<file_header>(data);
			if (header.magic != CAPTURE_MAGIC || header.version != CAPTURE_VERSION)
			{
				std::printf("not a version %u lookdev capture\n", CAPTURE_VERSION);
				return false;
			}
			for (size_t pos = header.header_size; pos + sizeof(record_header) <= size;)
			{
				const auto h = read<record_header>(data + pos);
				pos += sizeof(record_header);
				if (pos + h.size > size)
				{
					std::printf("truncated capture at offset %zu\n", pos);
					break;
				}
				records.push_back({ h.type, h.size, data + pos });
				pos += h.size;
			}
			return true;
		}
	};

	// ---- images ----

	struct image_buf
	{
		uint32_t width = 0, height = 0, format = 0, row_bytes = 0;
		std::vector<uint8_t> bytes;
	};

	inline bool is_bgra8(uint32_t format) { return format == D3DFMT_A8R8G8B8 || format == D3DFMT_X8R8G8B8; }

	inline bool write_bmp(const std::filesystem::path& path, const image_buf& img)
	{
		if (!is_bgra8(img.format))
			return false;
		FILE* f = _wfopen(path.c_str(), L"wb");
		if (!f)
			return false;
		const uint32_t data_size = img.width * img.height * 4;
		BITMAPFILEHEADER fh {};
		BITMAPINFOHEADER ih {};
		fh.bfType = 0x4D42;
		fh.bfOffBits = sizeof(fh) + sizeof(ih);
		fh.bfSize = fh.bfOffBits + data_size;
		ih.biSize = sizeof(ih);
		ih.biWidth = static_cast<LONG>(img.width);
		ih.biHeight = -static_cast<LONG>(img.height); // top-down
		ih.biPlanes = 1;
		ih.biBitCount = 32;
		ih.biCompression = BI_RGB;
		fwrite(&fh, sizeof(fh), 1, f);
		fwrite(&ih, sizeof(ih), 1, f);
		std::vector<uint8_t> row(img.width * 4);
		for (uint32_t y = 0; y < img.height; y++)
		{
			std::memcpy(row.data(), img.bytes.data() + size_t(y) * img.row_bytes, row.size());
			for (uint32_t x = 0; x < img.width; x++)
				row[x * 4 + 3] = 255;
			fwrite(row.data(), 1, row.size(), f);
		}
		fclose(f);
		return true;
	}

	struct compare_result
	{
		bool comparable = false;
		uint64_t pixels = 0;
		uint64_t differing = 0;
		uint32_t max_diff = 0;
		double mean_diff = 0.0;
		image_buf diff;
	};

	// Per-pixel comparison of the colour channels (alpha is ignored: the
	// back buffer's is undefined for X8R8G8B8 and never shown).
	inline compare_result compare(const image_buf& a, const image_buf& b)
	{
		compare_result r;
		if (a.width != b.width || a.height != b.height || a.format != b.format || !is_bgra8(a.format))
			return r;
		r.comparable = true;
		r.pixels = uint64_t(a.width) * a.height;
		r.diff = a;
		uint64_t total = 0;
		for (uint32_t y = 0; y < a.height; y++)
		{
			const uint8_t* pa = a.bytes.data() + size_t(y) * a.row_bytes;
			const uint8_t* pb = b.bytes.data() + size_t(y) * b.row_bytes;
			uint8_t* pd = r.diff.bytes.data() + size_t(y) * r.diff.row_bytes;
			for (uint32_t x = 0; x < a.width; x++)
			{
				uint32_t px_max = 0;
				for (int ch = 0; ch < 3; ch++)
				{
					const uint32_t d = static_cast<uint32_t>(std::abs(int(pa[x * 4 + ch]) - int(pb[x * 4 + ch])));
					total += d;
					px_max = std::max(px_max, d);
					// amplified so single-step differences are visible
					pd[x * 4 + ch] = static_cast<uint8_t>(std::min<uint32_t>(255, d * 16));
				}
				if (px_max)
				{
					r.differing++;
					r.max_diff = std::max(r.max_diff, px_max);
				}
			}
		}
		r.mean_diff = double(total) / double(r.pixels * 3);
		return r;
	}

	// ---- replay ----

	struct texture_entry
	{
		IDirect3DBaseTexture9* texture = nullptr;
		texture_desc desc {};
		uint32_t kind = 0;
	};

	struct surface_entry
	{
		IDirect3DSurface9* surface = nullptr;
		uint64_t texture = 0; // owning texture for texture levels
		uint32_t width = 0, height = 0, format = 0, usage = 0, pool = 0;
	};

	struct buffer_entry
	{
		IUnknown* buffer = nullptr;
		bool vertex = true;
		uint32_t size = 0;
	};

	struct replayer
	{
		IDirect3DDevice9* dev = nullptr;
		const capture_file* cap = nullptr; // for run(); a live stream feeds apply() instead

		std::unordered_map<uint64_t, texture_entry> textures;
		std::unordered_map<uint64_t, surface_entry> surfaces;
		std::unordered_map<uint64_t, buffer_entry> buffers;
		std::unordered_map<uint64_t, IDirect3DVertexShader9*> vertex_shaders;
		std::unordered_map<uint64_t, IDirect3DPixelShader9*> pixel_shaders;
		std::unordered_map<uint64_t, IDirect3DVertexDeclaration9*> decls;

		// Resources the frame writes to: their initial contents are re-applied on
		// every pass; everything else keeps what the first pass uploaded. Targets
		// the GPU writes (render targets, StretchRect / ColorFill destinations) are
		// re-applied only with reapply_gpu_targets: an exact repeat of the frame
		// needs them, a live Remix view does not (the frame regenerates them, and
		// each one is a full read-back's worth of upload through the bridge).
		std::unordered_set<uint64_t> written;
		std::unordered_set<uint64_t> gpu_written;
		bool reapply_gpu_targets = true;
		bool first_pass = true;

		// Game-state records (REC_GAME_FRAME, REC_GAME_LIGHTS, REC_TEXTURE_NAME, REC_GAME_FFX),
		// for the host to hand to the proxy. Ignored when unset (the viewer).
		std::function<void(const record&)> on_game_state;

		bool reapply_initial(uint64_t id) const
		{
			return written.contains(id) || (reapply_gpu_targets && gpu_written.contains(id));
		}

		D3DVIEWPORT9 viewport {};
		RECT scissor {};
		uint64_t decl = 0;
		uint32_t fvf = 0;

		const image_buf* reference = nullptr;
		image_buf reference_storage;
		uint32_t errors = 0;
		uint32_t draws = 0;

		// Called after every command that writes pixels (draws, clears, copies),
		// with the record type; draws has already counted a draw. For tools such
		// as the viewer's pixel history.
		std::function<void(uint32_t)> after_command;

		// A live host that has fallen behind still takes every state and data
		// record (later frames are deltas against them) but skips the commands
		// that only produce pixels. While skipping it also keeps device state and
		// buffer writes in a local shadow (deferred) instead of calling the device:
		// through the proxy and the Remix bridge every call costs, and a host that
		// skips must be much faster than the game. The first record applied with
		// skip_commands off flushes the net changes to the device.
		bool skip_commands = false;

		struct deferred_record
		{
			uint32_t type = 0;
			std::vector<uint8_t> payload;
		};
		std::map<uint64_t, deferred_record> deferred; // latest record per piece of state, flushed in key order

		// Shader constants, per register.
		struct const_bank
		{
			uint32_t reg_bytes = 16;
			std::vector<uint8_t> values;
			std::vector<uint8_t> dirty;
			bool any = false;

			void store(uint32_t start, uint32_t count, const uint8_t* src, uint32_t regs)
			{
				if (values.empty())
				{
					values.resize(size_t(regs) * reg_bytes);
					dirty.resize(regs);
				}
				if (start >= regs) return;
				count = std::min(count, regs - start);
				std::memcpy(values.data() + size_t(start) * reg_bytes, src, size_t(count) * reg_bytes);
				std::memset(dirty.data() + start, 1, count);
				any = true;
			}
		};
		const_bank vs_f { 16 }, ps_f { 16 }, vs_i { 16 }, ps_i { 16 }, vs_b { 4 }, ps_b { 4 };

		// Buffer writes: the buffer's bytes and the range written since the flush.
		struct deferred_buffer
		{
			std::vector<uint8_t> bytes;
			uint32_t lo = UINT32_MAX, hi = 0;
		};
		std::unordered_map<uint64_t, deferred_buffer> deferred_buffers;
		bool has_deferred = false;

		// Draw indices (within the frame, as analyze.py numbers them) to leave out,
		// for bisecting which draw produces something in the Remix view.
		std::unordered_set<uint32_t> drop_draws;

		// Pixel shader constant components forced to a value whenever the frame sets
		// them, to replay it with a game parameter changed (e.g. c110.w = 0: no haze).
		struct const_override { uint32_t reg; uint32_t component; float value; };
		std::vector<const_override> ps_const_overrides;

		replayer() = default;
		explicit replayer(const capture_file& c) : cap(&c) {}

		~replayer() { reset(); }

		void release_surface(surface_entry& e)
		{
			if (e.surface) e.surface->Release();
			e.surface = nullptr;
		}

		// An object the game no longer has (live streams).
		void destroy(uint64_t id)
		{
			if (const auto it = textures.find(id); it != textures.end())
			{
				for (auto s = surfaces.begin(); s != surfaces.end();)
				{
					if (s->second.texture == id) { release_surface(s->second); s = surfaces.erase(s); }
					else ++s;
				}
				if (it->second.texture) it->second.texture->Release();
				textures.erase(it);
			}
			if (const auto it = surfaces.find(id); it != surfaces.end())
			{
				release_surface(it->second);
				surfaces.erase(it);
			}
			if (const auto it = buffers.find(id); it != buffers.end())
			{
				if (it->second.buffer) it->second.buffer->Release();
				buffers.erase(it);
			}
			auto drop = [id](auto& map)
			{
				if (const auto it = map.find(id); it != map.end())
				{
					if (it->second) it->second->Release();
					map.erase(it);
				}
			};
			drop(vertex_shaders);
			drop(pixel_shaders);
			drop(decls);
			written.erase(id);
			gpu_written.erase(id);
			deferred_buffers.erase(id);
		}

		void clear_deferred()
		{
			deferred.clear();
			for (auto* b : { &vs_f, &ps_f, &vs_i, &ps_i, &vs_b, &ps_b })
			{
				b->values.clear();
				b->dirty.clear();
				b->any = false;
			}
			deferred_buffers.clear();
			has_deferred = false;
		}

		// Everything released: a new live session starts from nothing.
		void reset()
		{
			if (dev)
			{
				// nothing of ours may stay bound
				for (DWORD i = 0; i < 16; i++) dev->SetStreamSource(i, nullptr, 0, 0);
				dev->SetIndices(nullptr);
				for (DWORD i = 0; i < 20; i++) dev->SetTexture(i < 16 ? i : D3DVERTEXTEXTURESAMPLER0 + (i - 16), nullptr);
				dev->SetVertexShader(nullptr);
				dev->SetPixelShader(nullptr);
				dev->SetVertexDeclaration(nullptr);
				for (DWORD i = 1; i < 4; i++) dev->SetRenderTarget(i, nullptr);
				dev->SetDepthStencilSurface(nullptr);
				if (IDirect3DSurface9* bb = nullptr; SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
				{
					dev->SetRenderTarget(0, bb);
					bb->Release();
				}
			}
			for (auto& [id, e] : surfaces) release_surface(e);
			surfaces.clear();
			for (auto& [id, e] : textures) if (e.texture) e.texture->Release();
			textures.clear();
			for (auto& [id, e] : buffers) if (e.buffer) e.buffer->Release();
			buffers.clear();
			for (auto& [id, s] : vertex_shaders) if (s) s->Release();
			vertex_shaders.clear();
			for (auto& [id, s] : pixel_shaders) if (s) s->Release();
			pixel_shaders.clear();
			for (auto& [id, d] : decls) if (d) d->Release();
			decls.clear();
			written.clear();
			gpu_written.clear();
			clear_deferred();
			first_pass = true;
			decl = 0;
			fvf = 0;
		}

		void error(const char* what, HRESULT hr, uint64_t id = 0)
		{
			if (errors++ < 40)
				std::printf("  %s failed (hr=0x%08lX, id=0x%llX)\n", what, static_cast<unsigned long>(hr), static_cast<unsigned long long>(id));
		}

		IDirect3DBaseTexture9* texture(uint64_t id)
		{
			const auto it = textures.find(id);
			return it == textures.end() ? nullptr : it->second.texture;
		}
		IDirect3DSurface9* surface(uint64_t id)
		{
			const auto it = surfaces.find(id);
			return it == surfaces.end() ? nullptr : it->second.surface;
		}
		template <class T>
		T* find(std::unordered_map<uint64_t, T*>& map, uint64_t id)
		{
			const auto it = map.find(id);
			return it == map.end() ? nullptr : it->second;
		}

		// the resource whose contents a surface write lands in
		uint64_t owner_of_surface(uint64_t id)
		{
			const auto it = surfaces.find(id);
			return (it != surfaces.end() && it->second.texture) ? it->second.texture : id;
		}

		static void copy_rows(uint8_t* dst, UINT dst_pitch, const uint8_t* src, uint32_t rows, uint32_t row_bytes)
		{
			const auto n = std::min<UINT>(row_bytes, dst_pitch);
			for (uint32_t r = 0; r < rows; r++)
				std::memcpy(dst + size_t(r) * dst_pitch, src + size_t(r) * row_bytes, n);
		}

		// DEFAULT-pool targets cannot be locked: stage through system memory.
		void upload_default_surface(IDirect3DSurface9* dst, const texture_data& td, const uint8_t* bits)
		{
			D3DSURFACE_DESC sd;
			dst->GetDesc(&sd);
			IDirect3DSurface9* staging = nullptr;
			HRESULT hr = dev->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &staging, nullptr);
			if (FAILED(hr)) { error("staging surface", hr, td.id); return; }
			D3DLOCKED_RECT lr;
			if (SUCCEEDED(staging->LockRect(&lr, nullptr, 0)))
			{
				if (td.rows > sd.Height)
					error("surface data taller than its surface", E_INVALIDARG, td.id);
				copy_rows(static_cast<uint8_t*>(lr.pBits), static_cast<UINT>(lr.Pitch), bits, std::min<uint32_t>(td.rows, sd.Height), td.row_bytes);
				staging->UnlockRect();
			}
			hr = dev->UpdateSurface(staging, nullptr, dst, nullptr);
			if (FAILED(hr))
			{
				// fall back to a default-pool plain surface and a StretchRect
				IDirect3DSurface9* plain = nullptr;
				if (SUCCEEDED(dev->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_DEFAULT, &plain, nullptr))
					&& SUCCEEDED(dev->UpdateSurface(staging, nullptr, plain, nullptr)))
					hr = dev->StretchRect(plain, nullptr, dst, nullptr, D3DTEXF_NONE);
				if (plain) plain->Release();
				if (FAILED(hr)) error("upload to default surface", hr, td.id);
			}
			staging->Release();
		}

		void upload_texture(const texture_data& td, const uint8_t* bits)
		{
			const auto it = textures.find(td.id);
			if (it == textures.end() || !it->second.texture)
				return;
			auto& e = it->second;
			const bool lockable = (e.desc.pool != D3DPOOL_DEFAULT || (e.desc.usage & D3DUSAGE_DYNAMIC))
				&& !(e.desc.usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL));

			if (e.kind == REC_VOLUME_TEXTURE)
			{
				auto* vol = static_cast<IDirect3DVolumeTexture9*>(e.texture);
				D3DLOCKED_BOX lb;
				if (lockable && SUCCEEDED(vol->LockBox(td.level, &lb, nullptr, 0)))
				{
					for (uint32_t z = 0; z < td.depth; z++)
						copy_rows(static_cast<uint8_t*>(lb.pBits) + size_t(z) * lb.SlicePitch, static_cast<UINT>(lb.RowPitch),
							bits + size_t(z) * td.rows * td.row_bytes, td.rows, td.row_bytes);
					vol->UnlockBox(td.level);
				}
				else error("volume upload", E_FAIL, td.id);
				return;
			}

			if (lockable)
			{
				D3DLOCKED_RECT lr;
				HRESULT hr;
				if (e.kind == REC_TEXTURE)
					hr = static_cast<IDirect3DTexture9*>(e.texture)->LockRect(td.level, &lr, nullptr, 0);
				else
					hr = static_cast<IDirect3DCubeTexture9*>(e.texture)->LockRect(static_cast<D3DCUBEMAP_FACES>(td.face), td.level, &lr, nullptr, 0);
				if (FAILED(hr)) { error("LockRect", hr, td.id); return; }
				copy_rows(static_cast<uint8_t*>(lr.pBits), static_cast<UINT>(lr.Pitch), bits, td.rows, td.row_bytes);
				if (e.kind == REC_TEXTURE)
					static_cast<IDirect3DTexture9*>(e.texture)->UnlockRect(td.level);
				else
					static_cast<IDirect3DCubeTexture9*>(e.texture)->UnlockRect(static_cast<D3DCUBEMAP_FACES>(td.face), td.level);
				return;
			}

			IDirect3DSurface9* s = nullptr;
			if (e.kind == REC_TEXTURE)
				static_cast<IDirect3DTexture9*>(e.texture)->GetSurfaceLevel(td.level, &s);
			else
				static_cast<IDirect3DCubeTexture9*>(e.texture)->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(td.face), td.level, &s);
			if (!s) return;
			upload_default_surface(s, td, bits);
			s->Release();
		}

		void upload_surface(const texture_data& td, const uint8_t* bits)
		{
			const auto it = surfaces.find(td.id);
			if (it == surfaces.end() || !it->second.surface)
				return;
			auto& e = it->second;
			if (e.pool == D3DPOOL_DEFAULT)
			{
				upload_default_surface(e.surface, td, bits);
				return;
			}
			D3DSURFACE_DESC sd;
			e.surface->GetDesc(&sd);
			D3DLOCKED_RECT lr;
			if (SUCCEEDED(e.surface->LockRect(&lr, nullptr, 0)))
			{
				copy_rows(static_cast<uint8_t*>(lr.pBits), static_cast<UINT>(lr.Pitch), bits, std::min<uint32_t>(td.rows, sd.Height), td.row_bytes);
				e.surface->UnlockRect();
			}
		}

		void create_texture(uint32_t kind, const texture_desc& d)
		{
			if (textures.contains(d.id))
				return;
			texture_entry e { nullptr, d, kind };
			const auto fmt = static_cast<D3DFORMAT>(d.format);
			const auto pool = static_cast<D3DPOOL>(d.pool);
			HRESULT hr = E_FAIL;
			if (kind == REC_TEXTURE)
				hr = dev->CreateTexture(d.width, d.height, d.levels, d.usage, fmt, pool, reinterpret_cast<IDirect3DTexture9**>(&e.texture), nullptr);
			else if (kind == REC_CUBE_TEXTURE)
				hr = dev->CreateCubeTexture(d.width, d.levels, d.usage, fmt, pool, reinterpret_cast<IDirect3DCubeTexture9**>(&e.texture), nullptr);
			else
				hr = dev->CreateVolumeTexture(d.width, d.height, d.depth, d.levels, d.usage, fmt, pool, reinterpret_cast<IDirect3DVolumeTexture9**>(&e.texture), nullptr);
			if (FAILED(hr)) { error("create texture", hr, d.id); e.texture = nullptr; }
			textures[d.id] = e;
		}

		void create_surface(const surface_desc& d)
		{
			if (surfaces.contains(d.id))
				return;
			surface_entry e { nullptr, 0, d.width, d.height, d.format, d.usage, d.pool };
			const auto fmt = static_cast<D3DFORMAT>(d.format);
			const auto ms = static_cast<D3DMULTISAMPLE_TYPE>(d.multisample);
			HRESULT hr;
			if (d.usage & D3DUSAGE_RENDERTARGET)
				hr = dev->CreateRenderTarget(d.width, d.height, fmt, ms, d.multisample_quality, FALSE, &e.surface, nullptr);
			else if (d.usage & D3DUSAGE_DEPTHSTENCIL)
				hr = dev->CreateDepthStencilSurface(d.width, d.height, fmt, ms, d.multisample_quality, FALSE, &e.surface, nullptr);
			else
				hr = dev->CreateOffscreenPlainSurface(d.width, d.height, fmt, static_cast<D3DPOOL>(d.pool), &e.surface, nullptr);
			if (FAILED(hr)) { error("create surface", hr, d.id); e.surface = nullptr; }
			surfaces[d.id] = e;
		}

		void create_buffer(bool vertex, const buffer_desc& d)
		{
			if (buffers.contains(d.id))
				return;
			// Managed (or system memory) so every Lock, mid-frame ones included,
			// is ordered against the draws around it without DISCARD games.
			const auto pool = d.pool == D3DPOOL_SYSTEMMEM ? D3DPOOL_SYSTEMMEM : D3DPOOL_MANAGED;
			const DWORD usage = d.usage & ~DWORD(D3DUSAGE_DYNAMIC);
			buffer_entry e { nullptr, vertex, d.size };
			HRESULT hr;
			if (vertex)
				hr = dev->CreateVertexBuffer(d.size, usage, d.format, pool, reinterpret_cast<IDirect3DVertexBuffer9**>(&e.buffer), nullptr);
			else
				hr = dev->CreateIndexBuffer(d.size, usage, static_cast<D3DFORMAT>(d.format), pool, reinterpret_cast<IDirect3DIndexBuffer9**>(&e.buffer), nullptr);
			if (FAILED(hr)) { error("create buffer", hr, d.id); e.buffer = nullptr; }
			buffers[d.id] = e;
		}

		void upload_buffer(const buffer_data& bd, const uint8_t* bytes)
		{
			const auto it = buffers.find(bd.id);
			if (it == buffers.end() || !it->second.buffer || bd.offset + bd.size > it->second.size)
				return;
			void* p = nullptr;
			HRESULT hr;
			if (it->second.vertex)
				hr = static_cast<IDirect3DVertexBuffer9*>(it->second.buffer)->Lock(bd.offset, bd.size, &p, 0);
			else
				hr = static_cast<IDirect3DIndexBuffer9*>(it->second.buffer)->Lock(bd.offset, bd.size, &p, 0);
			if (FAILED(hr) || !p) { error("buffer lock", hr, bd.id); return; }
			std::memcpy(p, bytes, bd.size);
			if (it->second.vertex)
				static_cast<IDirect3DVertexBuffer9*>(it->second.buffer)->Unlock();
			else
				static_cast<IDirect3DIndexBuffer9*>(it->second.buffer)->Unlock();
		}

		static RECT to_win(const rect& r) { return { r.left, r.top, r.right, r.bottom }; }

		// One pass over a capture file. Returns at REC_PRESENT, before presenting.
		void run()
		{
			draws = 0;
			for (const auto& rec : cap->records)
				if (apply(rec))
					break;
			first_pass = false;
		}

		static bool is_pixel_command(uint32_t type)
		{
			switch (type)
			{
			case REC_DRAW: case REC_DRAW_INDEXED: case REC_DRAW_UP: case REC_DRAW_INDEXED_UP:
			case REC_CLEAR: case REC_STRETCH_RECT: case REC_COLOR_FILL:
			case REC_BEGIN_SCENE: case REC_END_SCENE:
				return true;
			default:
				return false;
			}
		}

		// The piece of device state a record sets, as a key ordered by record type
		// (render targets before the viewport they reset), or false if the record
		// is not plain state.
		static bool state_key(const record& rec, uint64_t& key)
		{
			const uint8_t* p = rec.payload;
			uint32_t sub = 0;
			switch (rec.type)
			{
			case REC_RENDER_STATE: case REC_STREAM_FREQ: case REC_LIGHT_ENABLE:
				if (rec.size < sizeof(state_value)) return false;
				sub = read<state_value>(p).state;
				break;
			case REC_SAMPLER_STATE: case REC_TEXTURE_STAGE_STATE:
			{
				if (rec.size < sizeof(stage_state)) return false;
				const auto s = read<stage_state>(p);
				sub = (s.stage << 16) | s.type;
				break;
			}
			case REC_SET_TEXTURE: case REC_RENDER_TARGET:
				if (rec.size < sizeof(bind)) return false;
				sub = read<bind>(p).slot;
				break;
			case REC_STREAM_SOURCE:
				if (rec.size < sizeof(stream_source)) return false;
				sub = read<stream_source>(p).stream;
				break;
			case REC_CLIP_PLANE:
				if (rec.size < sizeof(clip_plane)) return false;
				sub = read<clip_plane>(p).index;
				break;
			case REC_TRANSFORM:
				if (rec.size < sizeof(transform)) return false;
				sub = read<transform>(p).state;
				break;
			case REC_LIGHT:
				if (rec.size < sizeof(light)) return false;
				sub = read<light>(p).index;
				break;
			case REC_SET_VERTEX_SHADER: case REC_SET_PIXEL_SHADER: case REC_SET_VERTEX_DECL: case REC_SET_FVF:
			case REC_INDICES: case REC_DEPTH_STENCIL: case REC_VIEWPORT: case REC_SCISSOR: case REC_MATERIAL:
				break;
			default:
				return false;
			}
			key = (uint64_t(rec.type) << 32) | sub;
			return true;
		}

		// Keeps a record for later instead of calling the device. False when the
		// record must be applied now (resources, contents other than buffers,
		// copies, game state).
		bool defer(const record& rec)
		{
			const uint8_t* p = rec.payload;
			auto constants = [&](const_bank& bank, uint32_t regs)
			{
				const auto r = read<const_range>(p);
				if (rec.size >= sizeof(r) + size_t(r.count) * bank.reg_bytes)
					bank.store(r.start, r.count, p + sizeof(r), regs);
			};
			switch (rec.type)
			{
			case REC_VS_CONST_F: constants(vs_f, 256); break;
			case REC_PS_CONST_F: constants(ps_f, 256); break;
			case REC_VS_CONST_I: constants(vs_i, 16); break;
			case REC_PS_CONST_I: constants(ps_i, 16); break;
			case REC_VS_CONST_B: constants(vs_b, 16); break;
			case REC_PS_CONST_B: constants(ps_b, 16); break;
			case REC_BUFFER_DATA:
			{
				const auto bd = read<buffer_data>(p);
				if (!(bd.flags & DATA_INITIAL))
					written.insert(bd.id);
				const auto it = buffers.find(bd.id);
				if (it == buffers.end() || !it->second.buffer || bd.offset + bd.size > it->second.size || rec.size < sizeof(bd) + bd.size)
					break;
				auto& d = deferred_buffers[bd.id];
				d.bytes.resize(it->second.size);
				std::memcpy(d.bytes.data() + bd.offset, p + sizeof(bd), bd.size);
				d.lo = std::min(d.lo, bd.offset);
				d.hi = std::max(d.hi, bd.offset + bd.size);
				break;
			}
			default:
			{
				uint64_t key;
				if (!state_key(rec, key))
					return false;
				auto& e = deferred[key];
				e.type = rec.type;
				e.payload.assign(p, p + rec.size);
				break;
			}
			}
			has_deferred = true;
			return true;
		}

		void set_ps_const_f(uint32_t start, const float* f, uint32_t count)
		{
			dev->SetPixelShaderConstantF(start, f, count);
			for (const auto& o : ps_const_overrides)
			{
				if (o.reg < start || o.reg >= start + count)
					continue;
				float v[4];
				std::memcpy(v, f + (o.reg - start) * 4, sizeof(v));
				v[o.component] = o.value;
				dev->SetPixelShaderConstantF(o.reg, v, 1);
			}
		}

		// Sends the net state changes and buffer writes of the frames passed over.
		void flush_deferred()
		{
			// buffer writes first: their order against state does not matter
			for (auto& [id, d] : deferred_buffers)
			{
				const auto it = buffers.find(id);
				if (it == buffers.end() || !it->second.buffer || d.lo >= d.hi)
					continue;
				void* dst = nullptr;
				HRESULT hr;
				if (it->second.vertex)
					hr = static_cast<IDirect3DVertexBuffer9*>(it->second.buffer)->Lock(d.lo, d.hi - d.lo, &dst, 0);
				else
					hr = static_cast<IDirect3DIndexBuffer9*>(it->second.buffer)->Lock(d.lo, d.hi - d.lo, &dst, 0);
				if (FAILED(hr) || !dst) { error("buffer lock", hr, id); continue; }
				std::memcpy(dst, d.bytes.data() + d.lo, d.hi - d.lo);
				if (it->second.vertex)
					static_cast<IDirect3DVertexBuffer9*>(it->second.buffer)->Unlock();
				else
					static_cast<IDirect3DIndexBuffer9*>(it->second.buffer)->Unlock();
			}
			deferred_buffers.clear();

			for (const auto& [key, e] : deferred)
				apply_record({ e.type, static_cast<uint32_t>(e.payload.size()), e.payload.data() });
			deferred.clear();

			// constants, in runs of changed registers
			auto runs = [](const_bank& bank, auto&& set)
			{
				if (!bank.any) return;
				const uint32_t regs = static_cast<uint32_t>(bank.dirty.size());
				for (uint32_t i = 0; i < regs;)
				{
					if (!bank.dirty[i]) { i++; continue; }
					const uint32_t start = i;
					while (i < regs && bank.dirty[i]) i++;
					set(start, bank.values.data() + size_t(start) * bank.reg_bytes, i - start);
				}
				std::fill(bank.dirty.begin(), bank.dirty.end(), uint8_t(0));
				bank.any = false;
			};
			runs(vs_f, [&](uint32_t s, const uint8_t* v, uint32_t n) { dev->SetVertexShaderConstantF(s, reinterpret_cast<const float*>(v), n); });
			runs(ps_f, [&](uint32_t s, const uint8_t* v, uint32_t n) { set_ps_const_f(s, reinterpret_cast<const float*>(v), n); });
			runs(vs_i, [&](uint32_t s, const uint8_t* v, uint32_t n) { dev->SetVertexShaderConstantI(s, reinterpret_cast<const int*>(v), n); });
			runs(ps_i, [&](uint32_t s, const uint8_t* v, uint32_t n) { dev->SetPixelShaderConstantI(s, reinterpret_cast<const int*>(v), n); });
			runs(vs_b, [&](uint32_t s, const uint8_t* v, uint32_t n) { dev->SetVertexShaderConstantB(s, reinterpret_cast<const BOOL*>(v), n); });
			runs(ps_b, [&](uint32_t s, const uint8_t* v, uint32_t n) { dev->SetPixelShaderConstantB(s, reinterpret_cast<const BOOL*>(v), n); });
			has_deferred = false;
		}

		// Applies one record. True at REC_PRESENT: the caller presents.
		bool apply(const record& rec)
		{
			if (skip_commands)
			{
				if (is_pixel_command(rec.type) || defer(rec))
					return false;
			}
			else if (has_deferred)
				flush_deferred();
			if (!drop_draws.empty() && rec.type >= REC_DRAW && rec.type <= REC_DRAW_INDEXED_UP && drop_draws.contains(draws))
			{
				draws++;
				return false;
			}
			const bool present = apply_record(rec);
			if (after_command && is_pixel_command(rec.type) && rec.type != REC_BEGIN_SCENE && rec.type != REC_END_SCENE)
				after_command(rec.type);
			return present;
		}

		bool apply_record(const record& rec)
		{
			{
				const uint8_t* p = rec.payload;
				switch (rec.type)
				{
				// ---- resources ----
				case REC_TEXTURE:
				case REC_CUBE_TEXTURE:
				case REC_VOLUME_TEXTURE:
					create_texture(rec.type, read<texture_desc>(p));
					break;
				case REC_SURFACE:
					create_surface(read<surface_desc>(p));
					break;
				case REC_SURFACE_LEVEL:
				{
					const auto sl = read<surface_level>(p);
					if (surfaces.contains(sl.id)) break;
					surface_entry e {};
					e.texture = sl.texture;
					if (const auto it = textures.find(sl.texture); it != textures.end() && it->second.texture)
					{
						if (it->second.kind == REC_TEXTURE)
							static_cast<IDirect3DTexture9*>(it->second.texture)->GetSurfaceLevel(sl.level, &e.surface);
						else if (it->second.kind == REC_CUBE_TEXTURE)
							static_cast<IDirect3DCubeTexture9*>(it->second.texture)->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(sl.face), sl.level, &e.surface);
					}
					surfaces[sl.id] = e;
					break;
				}
				case REC_BACKBUFFER:
				{
					const auto id = read<id_only>(p).id;
					if (surfaces.contains(id)) break;
					surface_entry e {};
					dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &e.surface);
					e.pool = D3DPOOL_DEFAULT;
					e.usage = D3DUSAGE_RENDERTARGET;
					surfaces[id] = e;
					break;
				}
				case REC_VERTEX_BUFFER:
				case REC_INDEX_BUFFER:
					create_buffer(rec.type == REC_VERTEX_BUFFER, read<buffer_desc>(p));
					break;
				case REC_VERTEX_SHADER:
				{
					const auto id = read<id_only>(p).id;
					if (vertex_shaders.contains(id)) break;
					IDirect3DVertexShader9* s = nullptr;
					if (const HRESULT hr = dev->CreateVertexShader(reinterpret_cast<const DWORD*>(p + sizeof(id_only)), &s); FAILED(hr))
						error("CreateVertexShader", hr, id);
					vertex_shaders[id] = s;
					break;
				}
				case REC_PIXEL_SHADER:
				{
					const auto id = read<id_only>(p).id;
					if (pixel_shaders.contains(id)) break;
					IDirect3DPixelShader9* s = nullptr;
					if (const HRESULT hr = dev->CreatePixelShader(reinterpret_cast<const DWORD*>(p + sizeof(id_only)), &s); FAILED(hr))
						error("CreatePixelShader", hr, id);
					pixel_shaders[id] = s;
					break;
				}
				case REC_VERTEX_DECL:
				{
					const auto id = read<id_only>(p).id;
					if (decls.contains(id)) break;
					std::vector<D3DVERTEXELEMENT9> el((rec.size - sizeof(id_only)) / sizeof(D3DVERTEXELEMENT9));
					std::memcpy(el.data(), p + sizeof(id_only), el.size() * sizeof(D3DVERTEXELEMENT9));
					IDirect3DVertexDeclaration9* d = nullptr;
					if (const HRESULT hr = dev->CreateVertexDeclaration(el.data(), &d); FAILED(hr))
						error("CreateVertexDeclaration", hr, id);
					decls[id] = d;
					break;
				}

				// ---- contents ----
				case REC_TEXTURE_DATA:
				case REC_SURFACE_DATA:
				{
					const auto td = read<texture_data>(p);
					if (!(td.flags & DATA_INITIAL))
						written.insert(td.id);
					if (!first_pass && (td.flags & DATA_INITIAL) && !reapply_initial(td.id))
						break;
					if (rec.type == REC_TEXTURE_DATA) upload_texture(td, p + sizeof(td));
					else upload_surface(td, p + sizeof(td));
					break;
				}
				case REC_BUFFER_DATA:
				{
					const auto bd = read<buffer_data>(p);
					if (!(bd.flags & DATA_INITIAL))
						written.insert(bd.id);
					if (!first_pass && (bd.flags & DATA_INITIAL) && !reapply_initial(bd.id))
						break;
					upload_buffer(bd, p + sizeof(bd));
					break;
				}

				// ---- state ----
				case REC_RENDER_STATE:
				{
					const auto s = read<state_value>(p);
					dev->SetRenderState(static_cast<D3DRENDERSTATETYPE>(s.state), s.value);
					break;
				}
				case REC_SAMPLER_STATE:
				{
					const auto s = read<stage_state>(p);
					dev->SetSamplerState(s.stage, static_cast<D3DSAMPLERSTATETYPE>(s.type), s.value);
					break;
				}
				case REC_TEXTURE_STAGE_STATE:
				{
					const auto s = read<stage_state>(p);
					dev->SetTextureStageState(s.stage, static_cast<D3DTEXTURESTAGESTATETYPE>(s.type), s.value);
					break;
				}
				case REC_SET_TEXTURE:
				{
					const auto b = read<bind>(p);
					dev->SetTexture(b.slot, texture(b.id));
					break;
				}
				case REC_SET_VERTEX_SHADER:
					dev->SetVertexShader(find(vertex_shaders, read<id_only>(p).id));
					break;
				case REC_SET_PIXEL_SHADER:
					dev->SetPixelShader(find(pixel_shaders, read<id_only>(p).id));
					break;
				case REC_SET_VERTEX_DECL:
					decl = read<id_only>(p).id;
					if (auto* d = find(decls, decl)) dev->SetVertexDeclaration(d);
					else if (fvf) dev->SetFVF(fvf);
					break;
				case REC_SET_FVF:
					// the declaration the runtime made for an FVF is recorded too, so
					// the FVF only matters when no declaration is bound
					fvf = read<state_value>(p).value;
					if (!find(decls, decl) && fvf) dev->SetFVF(fvf);
					break;
				case REC_VS_CONST_F:
				case REC_PS_CONST_F:
				{
					const auto r = read<const_range>(p);
					const auto* f = reinterpret_cast<const float*>(p + sizeof(r));
					if (rec.type == REC_VS_CONST_F) dev->SetVertexShaderConstantF(r.start, f, r.count);
					else set_ps_const_f(r.start, f, r.count);
					break;
				}
				case REC_VS_CONST_I:
				case REC_PS_CONST_I:
				{
					const auto r = read<const_range>(p);
					const auto* v = reinterpret_cast<const int*>(p + sizeof(r));
					if (rec.type == REC_VS_CONST_I) dev->SetVertexShaderConstantI(r.start, v, r.count);
					else dev->SetPixelShaderConstantI(r.start, v, r.count);
					break;
				}
				case REC_VS_CONST_B:
				case REC_PS_CONST_B:
				{
					const auto r = read<const_range>(p);
					const auto* v = reinterpret_cast<const BOOL*>(p + sizeof(r));
					if (rec.type == REC_VS_CONST_B) dev->SetVertexShaderConstantB(r.start, v, r.count);
					else dev->SetPixelShaderConstantB(r.start, v, r.count);
					break;
				}
				case REC_STREAM_SOURCE:
				{
					const auto s = read<stream_source>(p);
					const auto it = buffers.find(s.id);
					auto* vb = it == buffers.end() ? nullptr : static_cast<IDirect3DVertexBuffer9*>(it->second.buffer);
					dev->SetStreamSource(s.stream, vb, s.offset, s.stride);
					break;
				}
				case REC_STREAM_FREQ:
				{
					const auto s = read<state_value>(p);
					dev->SetStreamSourceFreq(s.state, s.value);
					break;
				}
				case REC_INDICES:
				{
					const auto it = buffers.find(read<id_only>(p).id);
					dev->SetIndices(it == buffers.end() ? nullptr : static_cast<IDirect3DIndexBuffer9*>(it->second.buffer));
					break;
				}
				case REC_RENDER_TARGET:
				{
					const auto b = read<bind>(p);
					auto* s = surface(b.id);
					if (b.slot == 0 && !s) break;
					dev->SetRenderTarget(b.slot, s);
					if (b.id) gpu_written.insert(owner_of_surface(b.id));
					if (b.slot == 0)
					{
						// SetRenderTarget(0) resets these; the capture only records them when they change
						dev->SetViewport(&viewport);
						dev->SetScissorRect(&scissor);
					}
					break;
				}
				case REC_DEPTH_STENCIL:
				{
					const auto id = read<id_only>(p).id;
					dev->SetDepthStencilSurface(surface(id));
					if (id) gpu_written.insert(owner_of_surface(id));
					break;
				}
				case REC_VIEWPORT:
					viewport = read<D3DVIEWPORT9>(p);
					dev->SetViewport(&viewport);
					break;
				case REC_SCISSOR:
					scissor = to_win(read<rect>(p));
					dev->SetScissorRect(&scissor);
					break;
				case REC_CLIP_PLANE:
				{
					const auto c = read<clip_plane>(p);
					dev->SetClipPlane(c.index, c.plane);
					break;
				}
				case REC_TRANSFORM:
				{
					const auto t = read<transform>(p);
					dev->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(t.state), reinterpret_cast<const D3DMATRIX*>(t.m));
					break;
				}
				case REC_MATERIAL:
				{
					const auto m = read<D3DMATERIAL9>(p);
					dev->SetMaterial(&m);
					break;
				}
				case REC_LIGHT:
				{
					const auto l = read<light>(p);
					dev->SetLight(l.index, reinterpret_cast<const D3DLIGHT9*>(l.data));
					break;
				}
				case REC_LIGHT_ENABLE:
				{
					const auto s = read<state_value>(p);
					dev->LightEnable(s.state, s.value);
					break;
				}

				// ---- commands ----
				case REC_DRAW:
				{
					const auto d = read<draw>(p);
					dev->DrawPrimitive(static_cast<D3DPRIMITIVETYPE>(d.primitive), d.start_vertex, d.primitive_count);
					draws++;
					break;
				}
				case REC_DRAW_INDEXED:
				{
					const auto d = read<draw_indexed>(p);
					dev->DrawIndexedPrimitive(static_cast<D3DPRIMITIVETYPE>(d.primitive), d.base_vertex, d.min_index, d.num_vertices, d.start_index, d.primitive_count);
					draws++;
					break;
				}
				case REC_DRAW_UP:
				{
					const auto d = read<draw_up>(p);
					dev->DrawPrimitiveUP(static_cast<D3DPRIMITIVETYPE>(d.primitive), d.primitive_count, p + sizeof(d), d.stride);
					draws++;
					break;
				}
				case REC_DRAW_INDEXED_UP:
				{
					const auto d = read<draw_indexed_up>(p);
					const uint8_t* indices = p + sizeof(d);
					const uint8_t* vertices = indices + d.index_bytes;
					dev->DrawIndexedPrimitiveUP(static_cast<D3DPRIMITIVETYPE>(d.primitive), d.min_index, d.num_vertices, d.primitive_count,
						indices, static_cast<D3DFORMAT>(d.index_format), vertices, d.stride);
					draws++;
					break;
				}
				case REC_CLEAR:
				{
					const auto c = read<clear>(p);
					std::vector<D3DRECT> rects(c.count);
					if (c.count) std::memcpy(rects.data(), p + sizeof(c), c.count * sizeof(D3DRECT));
					dev->Clear(c.count, c.count ? rects.data() : nullptr, c.flags, c.color, c.z, c.stencil);
					break;
				}
				case REC_STRETCH_RECT:
				{
					const auto s = read<stretch_rect>(p);
					// DS1 issues full-screen copies of a render target onto itself. D3D9
					// rejects those (INVALIDCALL) in the game as well, so they are no-ops.
					if (s.src == s.dst)
						break;
					const RECT sr = to_win(s.src_rect), dr = to_win(s.dst_rect);
					if (const HRESULT hr = dev->StretchRect(surface(s.src), s.has_src_rect ? &sr : nullptr, surface(s.dst), s.has_dst_rect ? &dr : nullptr,
						static_cast<D3DTEXTUREFILTERTYPE>(s.filter)); FAILED(hr))
						error("StretchRect", hr, s.dst);
					gpu_written.insert(owner_of_surface(s.dst));
					break;
				}
				case REC_COLOR_FILL:
				{
					const auto f = read<color_fill>(p);
					const RECT r = to_win(f.r);
					dev->ColorFill(surface(f.surface), f.has_rect ? &r : nullptr, f.color);
					gpu_written.insert(owner_of_surface(f.surface));
					break;
				}
				case REC_UPDATE_SURFACE:
				{
					const auto u = read<update_surface>(p);
					const RECT r = to_win(u.r);
					const POINT pt { u.x, u.y };
					if (const HRESULT hr = dev->UpdateSurface(surface(u.src), u.has_rect ? &r : nullptr, surface(u.dst), u.has_point ? &pt : nullptr); FAILED(hr))
						error("UpdateSurface", hr, u.dst);
					written.insert(owner_of_surface(u.dst));
					break;
				}
				case REC_UPDATE_TEXTURE:
				{
					const auto u = read<update_texture>(p);
					if (const HRESULT hr = dev->UpdateTexture(texture(u.src), texture(u.dst)); FAILED(hr))
						error("UpdateTexture", hr, u.dst);
					written.insert(u.dst);
					break;
				}
				case REC_BEGIN_SCENE:
					dev->BeginScene();
					break;
				case REC_END_SCENE:
					dev->EndScene();
					break;
				case REC_PRESENT:
					return true;

				case REC_DESTROY:
					destroy(read<id_only>(p).id);
					break;

				case REC_REFERENCE_IMAGE:
					if (!reference)
					{
						const auto img = read<image>(p);
						reference_storage.width = img.width;
						reference_storage.height = img.height;
						reference_storage.format = img.format;
						reference_storage.row_bytes = img.row_bytes;
						reference_storage.bytes.assign(p + sizeof(img), p + rec.size);
						reference = &reference_storage;
					}
					break;

				case REC_GAME_FRAME:
				case REC_GAME_LIGHTS:
				case REC_TEXTURE_NAME:
				case REC_GAME_FFX:
					if (on_game_state)
						on_game_state(rec);
					break;

				default:
					break;
				}
			}
			return false;
		}

		bool read_backbuffer(image_buf& out)
		{
			IDirect3DSurface9* bb = nullptr;
			if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
				return false;
			D3DSURFACE_DESC sd;
			bb->GetDesc(&sd);
			IDirect3DSurface9* src = bb;
			IDirect3DSurface9* resolved = nullptr;
			if (sd.MultiSampleType != D3DMULTISAMPLE_NONE
				&& SUCCEEDED(dev->CreateRenderTarget(sd.Width, sd.Height, sd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr))
				&& SUCCEEDED(dev->StretchRect(bb, nullptr, resolved, nullptr, D3DTEXF_NONE)))
				src = resolved;
			IDirect3DSurface9* tmp = nullptr;
			bool ok = false;
			if (SUCCEEDED(dev->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &tmp, nullptr))
				&& SUCCEEDED(dev->GetRenderTargetData(src, tmp)))
			{
				D3DLOCKED_RECT lr;
				if (SUCCEEDED(tmp->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
				{
					uint32_t rows, row_bytes;
					if (!slice_layout(sd.Format, sd.Width, sd.Height, rows, row_bytes)) { rows = sd.Height; row_bytes = static_cast<uint32_t>(lr.Pitch); }
					out.width = sd.Width;
					out.height = sd.Height;
					out.format = sd.Format;
					out.row_bytes = row_bytes;
					out.bytes.resize(size_t(rows) * row_bytes);
					for (uint32_t r = 0; r < rows; r++)
						std::memcpy(out.bytes.data() + size_t(r) * row_bytes, static_cast<const uint8_t*>(lr.pBits) + size_t(r) * lr.Pitch, row_bytes);
					tmp->UnlockRect();
					ok = true;
				}
			}
			if (tmp) tmp->Release();
			if (resolved) resolved->Release();
			bb->Release();
			return ok;
		}

		// Shows a CPU image on the back buffer (vanilla / difference views).
		void blit_image(const image_buf& img)
		{
			IDirect3DSurface9* bb = nullptr;
			if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
				return;
			texture_data td {};
			td.rows = img.height;
			td.row_bytes = img.row_bytes;
			upload_default_surface(bb, td, img.bytes.data());
			bb->Release();
		}
	};
}
