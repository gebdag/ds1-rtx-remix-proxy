#include "std_include.hpp"
#include "light_dump.hpp"
#include "map_lights.hpp"
#include "shared/common/ffp_state.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace comp::game::light_dump
{
	namespace
	{
		std::mutex g_lock;
		std::ofstream g_file;
		std::vector<char> g_buffer;
		std::string g_path;
		std::atomic<bool> g_active { false };
		std::atomic<unsigned long long> g_lines { 0 };
		std::chrono::steady_clock::time_point g_start;
		std::chrono::steady_clock::time_point g_stopped;
		UINT g_header_frame = ~0u;

		remixapi_Interface g_original {};
		bool g_hooked = false;

		std::string f3(const remixapi_Float3D& v)
		{
			return std::format("({:.9g}, {:.9g}, {:.9g})", v.x, v.y, v.z);
		}

		std::string shaping(remixapi_Bool has, const remixapi_LightInfoLightShaping& s)
		{
			return std::format("shaping {} dir {} cone {:.9g} softness {:.9g} focus {:.9g}", has ? 1 : 0, f3(s.direction),
				s.coneAngleDegrees, s.coneSoftness, s.focusExponent);
		}

		std::string transform(const remixapi_Transform& t)
		{
			std::string s = "[";
			for (int r = 0; r < 3; r++)
				s += std::format("{}{:.9g} {:.9g} {:.9g} {:.9g}", r ? "; " : "", t.matrix[r][0], t.matrix[r][1], t.matrix[r][2], t.matrix[r][3]);
			return s + "]";
		}

		template <typename T>
		std::string opt(const T* p)
		{
			return p ? std::format("{:.9g}", static_cast<double>(*p)) : std::string("-");
		}

		// The whole info: its own fields, then every structure on its pNext chain.
		std::string describe(const remixapi_LightInfo* info)
		{
			if (!info)
				return "info null";
			std::string s = std::format("sType {} hash {:016x} radiance {}", static_cast<int>(info->sType),
				static_cast<unsigned long long>(info->hash), f3(info->radiance));
			const void* next = info->pNext;
			for (int depth = 0; next && depth < 8; depth++)
			{
				const auto type = *static_cast<const remixapi_StructType*>(next);
				switch (type)
				{
				case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT:
				{
					const auto* e = static_cast<const remixapi_LightInfoSphereEXT*>(next);
					s += std::format(" | SPHERE pos {} radius {:.9g} {} volumetric {:.9g}", f3(e->position), e->radius,
						shaping(e->shaping_hasvalue, e->shaping_value), e->volumetricRadianceScale);
					next = e->pNext;
					break;
				}
				case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT:
				{
					const auto* e = static_cast<const remixapi_LightInfoRectEXT*>(next);
					s += std::format(" | RECT pos {} x {} {:.9g} y {} {:.9g} dir {} {} volumetric {:.9g}", f3(e->position), f3(e->xAxis), e->xSize,
						f3(e->yAxis), e->ySize, f3(e->direction), shaping(e->shaping_hasvalue, e->shaping_value), e->volumetricRadianceScale);
					next = e->pNext;
					break;
				}
				case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT:
				{
					const auto* e = static_cast<const remixapi_LightInfoDiskEXT*>(next);
					s += std::format(" | DISK pos {} x {} {:.9g} y {} {:.9g} dir {} {} volumetric {:.9g}", f3(e->position), f3(e->xAxis), e->xRadius,
						f3(e->yAxis), e->yRadius, f3(e->direction), shaping(e->shaping_hasvalue, e->shaping_value), e->volumetricRadianceScale);
					next = e->pNext;
					break;
				}
				case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT:
				{
					const auto* e = static_cast<const remixapi_LightInfoCylinderEXT*>(next);
					s += std::format(" | CYLINDER pos {} radius {:.9g} axis {} length {:.9g} volumetric {:.9g}", f3(e->position), e->radius,
						f3(e->axis), e->axisLength, e->volumetricRadianceScale);
					next = e->pNext;
					break;
				}
				case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT:
				{
					const auto* e = static_cast<const remixapi_LightInfoDistantEXT*>(next);
					s += std::format(" | DISTANT dir {} angular {:.9g} volumetric {:.9g}", f3(e->direction), e->angularDiameterDegrees,
						e->volumetricRadianceScale);
					next = e->pNext;
					break;
				}
				case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT:
				{
					const auto* e = static_cast<const remixapi_LightInfoDomeEXT*>(next);
					std::string tex;
					for (const wchar_t* c = e->colorTexture; c && *c; ++c)
						tex.push_back(*c < 128 ? static_cast<char>(*c) : '?');
					s += std::format(" | DOME transform {} texture '{}'", transform(e->transform), tex);
					next = e->pNext;
					break;
				}
				case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT:
				{
					const auto* e = static_cast<const remixapi_LightInfoUSDEXT*>(next);
					s += std::format(" | USD type {} transform {} radius {} width {} height {} length {} angle {} colortemp_on {} color {} colortemp {}"
						" exposure {} intensity {} cone {} softness {} focus {} volumetric {}", static_cast<int>(e->lightType), transform(e->transform),
						opt(e->pRadius), opt(e->pWidth), opt(e->pHeight), opt(e->pLength), opt(e->pAngleRadians), opt(e->pEnableColorTemp),
						e->pColor ? f3(*e->pColor) : std::string("-"), opt(e->pColorTemp), opt(e->pExposure), opt(e->pIntensity),
						opt(e->pConeAngleRadians), opt(e->pConeSoftness), opt(e->pFocus), opt(e->pVolumetricRadianceScale));
					next = e->pNext;
					break;
				}
				default:
					s += std::format(" | sType {} (not decoded)", static_cast<int>(type));
					next = nullptr;
					break;
				}
			}
			return s;
		}

		remixapi_ErrorCode REMIXAPI_CALL create_light(const remixapi_LightInfo* info, remixapi_LightHandle* out_handle)
		{
			const auto result = g_original.CreateLight(info, out_handle);
			if (g_active.load(std::memory_order_relaxed))
				write(std::format("API CreateLight result {} handle {:p} {}", static_cast<int>(result),
					out_handle ? static_cast<void*>(*out_handle) : nullptr, describe(info)));
			return result;
		}

		remixapi_ErrorCode REMIXAPI_CALL destroy_light(remixapi_LightHandle handle)
		{
			const auto result = g_original.DestroyLight(handle);
			if (g_active.load(std::memory_order_relaxed))
				write(std::format("API DestroyLight result {} handle {:p}", static_cast<int>(result), static_cast<void*>(handle)));
			return result;
		}

		remixapi_ErrorCode REMIXAPI_CALL draw_light_instance(remixapi_LightHandle handle)
		{
			const auto result = g_original.DrawLightInstance(handle);
			if (g_active.load(std::memory_order_relaxed))
				write(std::format("API DrawLightInstance result {} handle {:p}", static_cast<int>(result), static_cast<void*>(handle)));
			return result;
		}

		remixapi_ErrorCode REMIXAPI_CALL set_config_variable(const char* key, const char* value)
		{
			const auto result = g_original.SetConfigVariable(key, value);
			if (g_active.load(std::memory_order_relaxed))
				write(std::format("API SetConfigVariable result {} {} = {}", static_cast<int>(result), key ? key : "(null)", value ? value : "(null)"));
			return result;
		}
	}

	bool active() { return g_active.load(std::memory_order_relaxed); }

	std::string path()
	{
		std::lock_guard lock(g_lock);
		return g_path;
	}

	unsigned long long line_count() { return g_lines.load(std::memory_order_relaxed); }

	void toggle()
	{
		std::lock_guard lock(g_lock);
		if (g_active)
		{
			g_active = false;
			g_file << std::format("f{} END after {} lines\n", shared::common::ffp_state::get().frame_count(), g_lines.load());
			g_file.close();
			g_stopped = std::chrono::steady_clock::now();
			shared::common::log("LightDump", std::format("stopped, {} lines in {}", g_lines.load(), g_path));
			return;
		}
		const auto dir = std::filesystem::path(shared::globals::root_path) / "rtx_comp" / "light_dumps";
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		SYSTEMTIME t {};
		GetLocalTime(&t);
		g_path = (dir / std::format("lights_{:04}{:02}{:02}_{:02}{:02}{:02}.log", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond)).string();
		g_buffer.resize(16u << 20);
		g_file = std::ofstream();
		g_file.rdbuf()->pubsetbuf(g_buffer.data(), static_cast<std::streamsize>(g_buffer.size()));
		g_file.open(g_path, std::ios::out | std::ios::trunc);
		if (!g_file)
		{
			shared::common::log("LightDump", std::format("could not open {}", g_path), shared::common::LOG_TYPE::LOG_TYPE_ERROR);
			return;
		}
		g_lines = 0;
		g_start = std::chrono::steady_clock::now();
		g_header_frame = ~0u;
		g_file << "# DS1 light dump. Line format: f<frame> t<ms since start> <SECTION> ... (sections: see light_dump.hpp)\n";
		g_active = true;
		shared::common::log("LightDump", std::format("recording to {}", g_path));
	}

	void write(const std::string& text)
	{
		if (!active())
			return;
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_start).count();
		const UINT frame = shared::common::ffp_state::get().frame_count();
		std::lock_guard lock(g_lock);
		if (!g_active)
			return;
		g_file << 'f' << frame << " t" << std::format("{:.3f}", ms) << ' ' << text << '\n';
		g_lines.fetch_add(1, std::memory_order_relaxed);
	}

	void frame_header()
	{
		if (!active())
			return;
		const UINT frame = shared::common::ffp_state::get().frame_count();
		{
			std::lock_guard lock(g_lock);
			if (frame == g_header_frame)
				return;
			g_header_frame = frame;
		}
		float ox = 0.0f, oy = 0.0f, oz = 0.0f;
		const bool offset = shared::common::ffp_state::get().world_offset(ox, oy, oz);
		write(std::format("FRAME world_offset {} ({:.6g}, {:.6g}, {:.6g}) map '{}' id {}", offset ? "valid" : "none", ox, oy, oz,
			map_lights::current_map_name(), map_lights::current_map_id()));
	}

	void hook_api(remixapi_Interface& api)
	{
		if (g_hooked || !api.CreateLight || !api.DestroyLight || !api.DrawLightInstance || !api.SetConfigVariable)
			return;
		g_original = api;
		api.CreateLight = create_light;
		api.DestroyLight = destroy_light;
		api.DrawLightInstance = draw_light_instance;
		api.SetConfigVariable = set_config_variable;
		g_hooked = true;
		shared::common::log("LightDump", "Remix API light calls routed through the F6 light dump");
	}

	void game_draw(const float* ps_const, int light_count, bool skinned, bool lightmapped, int owner, const void* texture, UINT prim_count)
	{
		if (!active() || !ps_const)
			return;
		frame_header();
		std::string s = std::format("DRAW count {} skinned {} lightmapped {} owner {} texture {:p} prims {}", light_count, skinned ? 1 : 0,
			lightmapped ? 1 : 0, owner, texture, prim_count);
		for (int i = 0; i < 4; i++)
		{
			const float* p = &ps_const[(112 + i) * 4];
			const float* c = &ps_const[(116 + i) * 4];
			s += std::format(" | L{} pos ({:.6g}, {:.6g}, {:.6g}, {:.6g}) col ({:.6g}, {:.6g}, {:.6g}, {:.6g})", i, p[0], p[1], p[2], p[3], c[0], c[1], c[2], c[3]);
		}
		write(s);
	}

	void draw_overlay()
	{
		if (!active())
		{
			// The file it went to, for a few seconds after F6 stopped it.
			if (g_stopped.time_since_epoch().count() && std::chrono::steady_clock::now() - g_stopped < std::chrono::seconds(6))
				ImGui::GetForegroundDrawList()->AddText(ImVec2(20.0f, 40.0f), IM_COL32(255, 255, 255, 255),
					std::format("Light dump saved: {} ({} lines)", path(), line_count()).c_str());
			return;
		}
		ImGui::GetForegroundDrawList()->AddText(ImVec2(20.0f, 40.0f), IM_COL32(255, 80, 80, 255),
			std::format("LIGHT DUMP recording ({} lines) - F6 to stop", line_count()).c_str());
	}
}
