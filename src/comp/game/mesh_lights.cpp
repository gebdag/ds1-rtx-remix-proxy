#include "std_include.hpp"
#include "mesh_lights.hpp"
#include "map_lights.hpp"
#include "texture_names.hpp"

#include "shared/common/ffp_state.hpp"

#include <format>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace comp::game::mesh_lights
{
	namespace
	{
		using shared::common::ffp_state;

		// One lamp's parts lie within this of each other (a cap and a base a metre apart); lamps stand further apart.
		constexpr float kPartAcross = 0.8f;
		constexpr float kPartUp = 2.0f;
		constexpr DWORD kCheckEveryMs = 1000;

		struct rule
		{
			std::string texture;
			float begin = 0.0f, end = 1.0f;
			float colour[3] = {};
		};
		struct lamp
		{
			float at[3];
			int rule;
		};

		std::vector<rule> g_rules;
		std::unordered_map<std::string, int> g_rule_of_name;
		std::unordered_map<IDirect3DBaseTexture9*, int> g_rule_of_texture;
		std::unordered_set<uint64_t> g_read_draws;
		std::vector<lamp> g_lamps;
		std::vector<point_lights::light> g_out;
		std::string g_map;
		std::string g_path;
		FILETIME g_file_time {};
		bool g_file_seen = false;
		DWORD g_last_check = 0;

		void forget_lamps()
		{
			g_read_draws.clear();
			g_lamps.clear();
			g_out.clear();
		}

		void parse_file()
		{
			g_rules.clear();
			g_rule_of_name.clear();
			g_rule_of_texture.clear();
			forget_lamps();
			std::ifstream in(g_path);
			std::string line;
			while (std::getline(in, line))
			{
				if (line.empty() || line[0] == '#' || line[0] == ';')
					continue;
				std::istringstream s(line);
				rule r;
				float pct = 100.0f, c[3];
				if (!(s >> r.texture >> r.begin >> r.end >> c[0] >> c[1] >> c[2] >> pct))
				{
					if (line.find_first_not_of(" \t\r") != std::string::npos)
						shared::common::log("MeshLights", std::format("line ignored: {}", line), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
					continue;
				}
				for (int i = 0; i < 3; i++)
					r.colour[i] = c[i] / 255.0f * pct / 100.0f;
				g_rule_of_name[r.texture] = static_cast<int>(g_rules.size());
				g_rules.push_back(r);
			}
			shared::common::log("MeshLights", std::format("{}: {} texture(s)", g_path, g_rules.size()), shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}

		void update_state()
		{
			const auto map = map_lights::current_map_name();
			if (map != g_map)
			{
				g_map = map;
				forget_lamps();
			}
			const DWORD now = GetTickCount();
			if (g_file_seen && now - g_last_check < kCheckEveryMs)
				return;
			g_last_check = now;
			if (g_path.empty())
				g_path = shared::globals::root_path + "\\rtx_comp\\lights\\mesh_lights.txt";
			WIN32_FILE_ATTRIBUTE_DATA info;
			if (!GetFileAttributesExA(g_path.c_str(), GetFileExInfoStandard, &info))
			{
				g_file_seen = true;
				return;
			}
			if (g_file_seen && CompareFileTime(&info.ftLastWriteTime, &g_file_time) == 0)
				return;
			g_file_seen = true;
			g_file_time = info.ftLastWriteTime;
			parse_file();
		}

		void add_lamp(int rule_index, const float* at)
		{
			for (const auto& l : g_lamps)
			{
				const float dx = l.at[0] - at[0], dz = l.at[2] - at[2];
				if (dx * dx + dz * dz < kPartAcross * kPartAcross && std::abs(l.at[1] - at[1]) < kPartUp)
					return;
			}
			lamp l;
			l.rule = rule_index;
			std::memcpy(l.at, at, sizeof(l.at));
			g_lamps.push_back(l);

			const auto& r = g_rules[rule_index];
			point_lights::light p {};
			p.slot = kFirstSlot + static_cast<int>(g_out.size());
			p.x = at[0];
			p.y = at[1];
			p.z = at[2];
			p.r = r.colour[0];
			p.g = r.colour[1];
			p.b = r.colour[2];
			p.dwindle_begin = r.begin;
			p.dwindle_end = r.end;
			p.has_params = true;
			p.source = point_lights::param_source::mesh;
			g_out.push_back(p);
		}

		// The draw's vertices in the world, each piece one lamp.
		void read_draw(IDirect3DDevice9* dev, int rule_index, D3DPRIMITIVETYPE prim, INT base_vertex, UINT start_index, UINT prim_count)
		{
			auto& ffp = ffp_state::get();
			IDirect3DVertexBuffer9* vb = ffp.stream_vb(0);
			const UINT stride = ffp.stream_stride(0), vb_offset = ffp.stream_offset(0);
			IDirect3DIndexBuffer9* ib = ffp.cur_ib();
			const float* vs = ffp.vs_const_data();
			float ox, oy, oz;
			if (!vb || !ib || !stride || !vs || !ffp.world_offset(ox, oy, oz))
				return;
			if (prim != D3DPT_TRIANGLELIST && prim != D3DPT_TRIANGLESTRIP)
				return;

			IDirect3DVertexDeclaration9* decl = nullptr;
			if (FAILED(dev->GetVertexDeclaration(&decl)) || !decl)
				return;
			D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH];
			UINT count = 0;
			const HRESULT got = decl->GetDeclaration(elements, &count);
			decl->Release();
			if (FAILED(got))
				return;
			int pos_off = -1, bone_off = -1;
			for (UINT i = 0; i < count && elements[i].Stream != 0xFF; i++)
			{
				if (elements[i].Stream != 0 || elements[i].UsageIndex != 0)
					continue;
				if (elements[i].Usage == D3DDECLUSAGE_POSITION && elements[i].Type == D3DDECLTYPE_FLOAT3)
					pos_off = elements[i].Offset;
				else if (elements[i].Usage == D3DDECLUSAGE_BLENDINDICES)
					bone_off = elements[i].Offset;
			}
			if (pos_off < 0)
				return;

			D3DINDEXBUFFER_DESC idesc {};
			D3DVERTEXBUFFER_DESC vdesc {};
			if (FAILED(ib->GetDesc(&idesc)) || FAILED(vb->GetDesc(&vdesc)))
				return;
			// Write-only video memory reads back undefined data; the map's buffers are managed.
			if ((idesc.Pool == D3DPOOL_DEFAULT && (idesc.Usage & D3DUSAGE_WRITEONLY)) ||
				(vdesc.Pool == D3DPOOL_DEFAULT && (vdesc.Usage & D3DUSAGE_WRITEONLY)))
				return;
			const UINT isz = idesc.Format == D3DFMT_INDEX32 ? 4 : 2;
			const UINT indices = prim == D3DPT_TRIANGLESTRIP ? prim_count + 2 : prim_count * 3;
			if (static_cast<uint64_t>(start_index + indices) * isz > idesc.Size)
				return;
			void* idata = nullptr;
			if (FAILED(ib->Lock(start_index * isz, indices * isz, &idata, D3DLOCK_READONLY)) || !idata)
				return;
			void* vdata = nullptr;
			if (FAILED(vb->Lock(0, 0, &vdata, D3DLOCK_READONLY)) || !vdata)
			{
				ib->Unlock();
				return;
			}
			std::unordered_set<int64_t> used;
			used.reserve(indices);
			for (UINT i = 0; i < indices; i++)
				used.insert(static_cast<int64_t>(isz == 4 ? static_cast<const uint32_t*>(idata)[i] : static_cast<const uint16_t*>(idata)[i]) + base_vertex);
			std::vector<std::array<float, 3>> points;
			points.reserve(used.size());
			const auto* bytes = static_cast<const unsigned char*>(vdata);
			for (const int64_t v : used)
			{
				const int64_t at = static_cast<int64_t>(vb_offset) + v * stride;
				if (v < 0 || at + static_cast<int64_t>(stride) > static_cast<int64_t>(vdesc.Size))
					continue;
				float p[3];
				std::memcpy(p, bytes + at + pos_off, sizeof(p));
				// The rigid bone's three rows (VS c8 on), relative to the camera.
				const int bone = bone_off >= 0 ? bytes[at + bone_off] : 0;
				const float* m = vs + (8 + 3 * bone) * 4;
				points.push_back({
					m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3] + ox,
					m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7] + oy,
					m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11] + oz });
			}
			vb->Unlock();
			ib->Unlock();

			std::vector<char> taken(points.size(), 0);
			for (size_t i = 0; i < points.size(); i++)
			{
				if (taken[i])
					continue;
				float lo[3] = { points[i][0], points[i][1], points[i][2] };
				float hi[3] = { lo[0], lo[1], lo[2] };
				for (size_t j = i; j < points.size(); j++)
				{
					if (taken[j])
						continue;
					const float dx = points[j][0] - points[i][0], dz = points[j][2] - points[i][2];
					if (dx * dx + dz * dz >= kPartAcross * kPartAcross || std::abs(points[j][1] - points[i][1]) >= kPartUp)
						continue;
					taken[j] = 1;
					for (int c = 0; c < 3; c++)
					{
						lo[c] = std::min(lo[c], points[j][c]);
						hi[c] = std::max(hi[c], points[j][c]);
					}
				}
				const float middle[3] = { (lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f };
				add_lamp(rule_index, middle);
			}
		}
	}

	void observe_draw(IDirect3DDevice9* dev, IDirect3DBaseTexture9* texture, D3DPRIMITIVETYPE prim, INT base_vertex,
		UINT start_index, UINT prim_count)
	{
		update_state();
		if (g_rules.empty() || !texture)
			return;
		int rule_index = -1;
		if (const auto it = g_rule_of_texture.find(texture); it != g_rule_of_texture.end())
			rule_index = it->second;
		else
		{
			const auto name = texture_names::name_of(texture);
			// No name yet: asked again next time. Named: remembered, a rule or none.
			if (name.empty())
				return;
			const auto found = g_rule_of_name.find(name);
			rule_index = found != g_rule_of_name.end() ? found->second : -1;
			g_rule_of_texture[texture] = rule_index;
		}
		if (rule_index < 0)
			return;

		auto& ffp = ffp_state::get();
		uint64_t key = reinterpret_cast<uintptr_t>(ffp.stream_vb(0));
		auto mix = [&key](uint64_t v) { key = (key ^ v) * 0x100000001B3ull; };
		mix(reinterpret_cast<uintptr_t>(ffp.cur_ib()));
		mix(static_cast<uint32_t>(base_vertex));
		mix(start_index);
		mix(prim_count);
		if (!g_read_draws.insert(key).second)
			return;
		const size_t before = g_lamps.size();
		read_draw(dev, rule_index, prim, base_vertex, start_index, prim_count);
		if (g_lamps.size() != before)
			shared::common::log("MeshLights", std::format("{}: {} lamp(s) more, {} on this map", g_rules[rule_index].texture, g_lamps.size() - before, g_lamps.size()), shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
	}

	bool is_lamp(IDirect3DBaseTexture9* texture)
	{
		if (g_rules.empty() || !texture)
			return false;
		const auto it = g_rule_of_texture.find(texture);
		return it != g_rule_of_texture.end() && it->second >= 0;
	}

	const std::vector<point_lights::light>& get()
	{
		update_state();
		return g_out;
	}

	std::string texture_of(int slot)
	{
		const size_t i = static_cast<size_t>(slot - kFirstSlot);
		return i < g_lamps.size() ? g_rules[g_lamps[i].rule].texture : std::string{};
	}

	const std::string& path()
	{
		return g_path;
	}
}
