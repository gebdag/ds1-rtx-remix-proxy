#include "std_include.hpp"
#include "d3dx_texture_watch.hpp"
#include "texture_names.hpp"
#include "../modules/crash_context.hpp"

namespace comp::game::d3dx_texture_watch
{
	namespace
	{
		using create_texture_t = HRESULT(WINAPI*)(LPDIRECT3DDEVICE9, LPCVOID, UINT, UINT, UINT, UINT,
			DWORD, D3DFORMAT, D3DPOOL, DWORD, DWORD, D3DCOLOR, D3DXIMAGE_INFO*, PALETTEENTRY*,
			LPDIRECT3DTEXTURE9*);
		using create_cube_texture_t = HRESULT(WINAPI*)(LPDIRECT3DDEVICE9, LPCVOID, UINT, UINT, UINT,
			DWORD, D3DFORMAT, D3DPOOL, DWORD, DWORD, D3DCOLOR, D3DXIMAGE_INFO*, PALETTEENTRY*,
			LPDIRECT3DCUBETEXTURE9*);

		create_texture_t o_create_texture = nullptr;
		create_cube_texture_t o_create_cube_texture = nullptr;

		// The game passes D3DFMT_UNKNOWN for most TPF entries and lets D3DX take
		// the format from the DDS header, so the requested format argument says
		// nothing about what was actually asked for. Read the header instead.
		std::string dds_format_tag(LPCVOID src, UINT size)
		{
			constexpr UINT kFourCcOffset = 0x54;  // DDS_HEADER.ddspf.dwFourCC
			if (!src || size < kFourCcOffset + 4)
				return "?";

			const auto* bytes = static_cast<const uint8_t*>(src);
			if (memcmp(bytes, "DDS ", 4) != 0)
				return "notdds";

			DWORD fourcc;
			memcpy(&fourcc, bytes + kFourCcOffset, sizeof(fourcc));
			if (!fourcc)
				return "rgb";  // uncompressed, described by the bit masks instead

			const char tag[5] = {
				static_cast<char>(fourcc), static_cast<char>(fourcc >> 8),
				static_cast<char>(fourcc >> 16), static_cast<char>(fourcc >> 24), '\0'
			};
			return tag;
		}

		// One line per distinct DDS format the game actually asks for — enough to
		// tell whether the formats the Remix runtime rejects (AL16, R16) reach
		// D3DX at all, without logging every texture load.
		void note_first_use(const std::string& tag)
		{
			static std::mutex mutex;
			static std::set<std::string> seen;
			std::lock_guard lock(mutex);
			if (seen.insert(tag).second)
				shared::common::log("D3DXWatch", std::format("DDS format in use: {}", tag));
		}

		HRESULT WINAPI create_texture(LPDIRECT3DDEVICE9 device, LPCVOID src, UINT src_size,
			UINT width, UINT height, UINT mip_levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
			DWORD filter, DWORD mip_filter, D3DCOLOR color_key, D3DXIMAGE_INFO* src_info,
			PALETTEENTRY* palette, LPDIRECT3DTEXTURE9* out)
		{
			const auto dds = dds_format_tag(src, src_size);
			note_first_use(dds);

			const crash_context::stopwatch sw;
			const auto hr = o_create_texture(device, src, src_size, width, height, mip_levels, usage,
				format, pool, filter, mip_filter, color_key, src_info, palette, out);
			if (FAILED(hr))
				crash_context::note("D3DXTex2D %uB dds=%s t=%u hr=%08X", src_size, dds.c_str(), sw.ms(), hr);
			else if (out && *out)
				texture_names::on_d3dx_created(*out, src, src_size);
			return hr;
		}

		HRESULT WINAPI create_cube_texture(LPDIRECT3DDEVICE9 device, LPCVOID src, UINT src_size,
			UINT size, UINT mip_levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, DWORD filter,
			DWORD mip_filter, D3DCOLOR color_key, D3DXIMAGE_INFO* src_info, PALETTEENTRY* palette,
			LPDIRECT3DCUBETEXTURE9* out)
		{
			const auto dds = dds_format_tag(src, src_size);
			note_first_use(dds);

			const crash_context::stopwatch sw;
			const auto hr = o_create_cube_texture(device, src, src_size, size, mip_levels, usage,
				format, pool, filter, mip_filter, color_key, src_info, palette, out);
			if (FAILED(hr))
				crash_context::note("D3DXTexCube %uB dds=%s t=%u hr=%08X", src_size, dds.c_str(), sw.ms(), hr);
			return hr;
		}

		template <typename fn_t>
		void hook_export(HMODULE module, const char* name, void* detour, fn_t& original)
		{
			auto* target = reinterpret_cast<void*>(GetProcAddress(module, name));
			if (!target || MH_CreateHook(target, detour, reinterpret_cast<void**>(&original)) != MH_OK)
				shared::common::log("D3DXWatch", std::format("failed to hook {}", name),
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}
	}

	void install()
	{
		// Hooking the exports covers every call site without touching the IAT.
		const auto module = GetModuleHandleA("d3dx9_43.dll");
		if (!module)
		{
			shared::common::log("D3DXWatch", "d3dx9_43.dll not loaded — texture-create failures will go unrecorded",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}

		hook_export(module, "D3DXCreateTextureFromFileInMemoryEx", &create_texture, o_create_texture);
		hook_export(module, "D3DXCreateCubeTextureFromFileInMemoryEx", &create_cube_texture, o_create_cube_texture);
	}
}
