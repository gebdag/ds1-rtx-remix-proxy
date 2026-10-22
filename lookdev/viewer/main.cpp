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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../src/shared/lookdev/capture_format.hpp"

#pragma comment(lib, "d3d9.lib")

using namespace lookdev;

namespace
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

	bool is_bgra8(uint32_t format) { return format == D3DFMT_A8R8G8B8 || format == D3DFMT_X8R8G8B8; }

	bool write_bmp(const std::filesystem::path& path, const image_buf& img)
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
	compare_result compare(const image_buf& a, const image_buf& b)
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
		const capture_file& cap;

		std::unordered_map<uint64_t, texture_entry> textures;
		std::unordered_map<uint64_t, surface_entry> surfaces;
		std::unordered_map<uint64_t, buffer_entry> buffers;
		std::unordered_map<uint64_t, IDirect3DVertexShader9*> vertex_shaders;
		std::unordered_map<uint64_t, IDirect3DPixelShader9*> pixel_shaders;
		std::unordered_map<uint64_t, IDirect3DVertexDeclaration9*> decls;

		// Resources the frame writes to: their initial contents are re-applied on
		// every pass; everything else keeps what the first pass uploaded.
		std::unordered_set<uint64_t> written;
		bool first_pass = true;

		D3DVIEWPORT9 viewport {};
		RECT scissor {};
		uint64_t decl = 0;
		uint32_t fvf = 0;

		const image_buf* reference = nullptr;
		image_buf reference_storage;
		uint32_t errors = 0;
		uint32_t draws = 0;

		explicit replayer(const capture_file& c) : cap(c) {}

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
				copy_rows(static_cast<uint8_t*>(lr.pBits), static_cast<UINT>(lr.Pitch), bits, td.rows, td.row_bytes);
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
			D3DLOCKED_RECT lr;
			if (SUCCEEDED(e.surface->LockRect(&lr, nullptr, 0)))
			{
				copy_rows(static_cast<uint8_t*>(lr.pBits), static_cast<UINT>(lr.Pitch), bits, td.rows, td.row_bytes);
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

		// One pass over the frame. Returns at REC_PRESENT, before presenting.
		void run()
		{
			draws = 0;
			for (const auto& rec : cap.records)
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
					if (!first_pass && (td.flags & DATA_INITIAL) && !written.contains(td.id))
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
					if (!first_pass && (bd.flags & DATA_INITIAL) && !written.contains(bd.id))
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
					else dev->SetPixelShaderConstantF(r.start, f, r.count);
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
					if (b.id) written.insert(owner_of_surface(b.id));
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
					if (id) written.insert(owner_of_surface(id));
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
					written.insert(owner_of_surface(s.dst));
					break;
				}
				case REC_COLOR_FILL:
				{
					const auto f = read<color_fill>(p);
					const RECT r = to_win(f.r);
					dev->ColorFill(surface(f.surface), f.has_rect ? &r : nullptr, f.color);
					written.insert(owner_of_surface(f.surface));
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
					first_pass = false;
					return;

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

				default:
					break;
				}
			}
			first_pass = false;
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
