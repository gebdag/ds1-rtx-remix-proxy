#include "std_include.hpp"
#include "thin_walls.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace comp::game::thin_walls
{
	namespace
	{
		// Facing faces at most this far apart (metres) make a wall; closer ones are coplanar layers (decal overlays).
		constexpr float kMaxGap = 0.5f;
		constexpr float kMinGap = 0.01f;
		// Fronts anti-parallel within this cosine, and a face's vertex normals along its plane within it.
		constexpr float kFacing = 0.95f;
		// A mesh is drawn double-sided when facing pairs cover this share of its area and at least this many m^2.
		constexpr float kMinShare = 0.1f;
		constexpr float kMinPairedArea = 1.0f;
		constexpr float kMinTriangleArea = 1e-4f;
		constexpr UINT kMaxTriangles = 65536;
		constexpr int kMeasuresPerFrame = 2;
		constexpr unsigned int kPruneFrames = 36000;

		struct layout
		{
			bool valid = false;
			int pos_off = -1;     // FLOAT3
			int nrm_off = -1;
			BYTE nrm_type = 0;    // UBYTE4N or FLOAT3
		};

		layout layout_of(IDirect3DVertexDeclaration9* decl, UINT stride)
		{
			layout l;
			D3DVERTEXELEMENT9 els[MAXD3DDECLLENGTH + 1] = {};
			UINT n = 0;
			if (!decl || FAILED(decl->GetDeclaration(nullptr, &n)) || n > MAXD3DDECLLENGTH + 1 || FAILED(decl->GetDeclaration(els, &n)))
				return l;
			for (UINT i = 0; i < n && els[i].Stream != 0xFF; i++)
			{
				const auto& e = els[i];
				if (e.Stream != 0 || e.UsageIndex != 0)
					continue;
				if (e.Usage == D3DDECLUSAGE_POSITION && e.Type == D3DDECLTYPE_FLOAT3)
					l.pos_off = e.Offset;
				else if (e.Usage == D3DDECLUSAGE_NORMAL && (e.Type == D3DDECLTYPE_UBYTE4N || e.Type == D3DDECLTYPE_FLOAT3))
				{
					l.nrm_off = e.Offset;
					l.nrm_type = e.Type;
				}
			}
			const int nrm_bytes = l.nrm_type == D3DDECLTYPE_FLOAT3 ? 12 : 4;
			l.valid = l.pos_off >= 0 && l.nrm_off >= 0 &&
				l.pos_off + 12 <= static_cast<int>(stride) && l.nrm_off + nrm_bytes <= static_cast<int>(stride);
			return l;
		}

		struct mesh_key
		{
			uintptr_t vb, ib, decl;
			UINT vb_offset, stride;
			INT base_vertex;
			UINT start_index, prim_count;
			DWORD prim;

			bool operator==(const mesh_key&) const = default;
		};

		struct mesh_key_hash
		{
			size_t operator()(const mesh_key& k) const
			{
				size_t h = 1469598103u;
				auto mix = [&](size_t v) { h ^= v + 0x9e3779b9 + (h << 6) + (h >> 2); };
				mix(k.vb); mix(k.ib); mix(k.decl); mix(k.vb_offset); mix(k.stride);
				mix(static_cast<size_t>(k.base_vertex)); mix(k.start_index); mix(k.prim_count); mix(k.prim);
				return h;
			}
		};

		struct entry
		{
			bool measured = false;
			bool inside_out = false;
			unsigned int last_frame = 0;
		};

		// Draws come from the render thread; index buffers can be created on the game's loading thread.
		std::mutex g_mutex;
		std::unordered_map<mesh_key, entry, mesh_key_hash> g_meshes;
		unsigned int g_budget_frame = ~0u;
		int g_measures_left = 0;
		unsigned int g_last_prune = 0;

		struct face
		{
			float centre[3];
			float front[3];   // unit, the side its vertex normals point to
			float lo[3], hi[3];
			float area;
		};

		float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

		// Bucket of faces with a similar front (components rounded to quarters) and plane offset (kMaxGap bins).
		int64_t bucket_key(int qx, int qy, int qz, int64_t plane_bin)
		{
			return ((plane_bin * 16 + (qx + 4)) * 16 + (qy + 4)) * 16 + (qz + 4);
		}

		int quantise(float v) { return std::clamp(static_cast<int>(std::lround(v * 4.0f)), -4, 4); }

		bool read_faces(const mesh_key& k, const layout& l, IDirect3DVertexBuffer9* vb, IDirect3DIndexBuffer9* ib,
			std::vector<face>& faces, float& total_area)
		{
			D3DINDEXBUFFER_DESC idesc {};
			D3DVERTEXBUFFER_DESC vdesc {};
			if (FAILED(ib->GetDesc(&idesc)) || FAILED(vb->GetDesc(&vdesc)))
				return false;
			// Write-only default pool buffers cannot be read back.
			if ((idesc.Pool == D3DPOOL_DEFAULT && (idesc.Usage & D3DUSAGE_WRITEONLY)) ||
				(vdesc.Pool == D3DPOOL_DEFAULT && (vdesc.Usage & D3DUSAGE_WRITEONLY)))
				return false;
			const UINT isz = idesc.Format == D3DFMT_INDEX32 ? 4 : 2;
			const bool strip = k.prim == D3DPT_TRIANGLESTRIP;
			const UINT count = strip ? k.prim_count + 2 : k.prim_count * 3;
			if (static_cast<uint64_t>(k.start_index + count) * isz > idesc.Size)
				return false;

			void* idata = nullptr;
			if (FAILED(ib->Lock(k.start_index * isz, count * isz, &idata, D3DLOCK_READONLY)) || !idata)
				return false;
			void* vdata = nullptr;
			if (FAILED(vb->Lock(0, 0, &vdata, D3DLOCK_READONLY)) || !vdata)
			{
				ib->Unlock();
				return false;
			}

			auto index = [&](UINT i) -> int64_t
			{
				const int64_t raw = isz == 4 ? static_cast<const uint32_t*>(idata)[i] : static_cast<const uint16_t*>(idata)[i];
				return raw + k.base_vertex;
			};
			const auto* vbytes = static_cast<const unsigned char*>(vdata);
			auto vertex = [&](int64_t v) -> const unsigned char*
			{
				const int64_t at = static_cast<int64_t>(k.vb_offset) + v * k.stride;
				if (v < 0 || at + static_cast<int64_t>(k.stride) > static_cast<int64_t>(vdesc.Size))
					return nullptr;
				return vbytes + at;
			};
			auto normal = [&](const unsigned char* v, float* n)
			{
				if (l.nrm_type == D3DDECLTYPE_FLOAT3)
					std::memcpy(n, v + l.nrm_off, 12);
				else
					for (int c = 0; c < 3; c++)
						n[c] = v[l.nrm_off + c] / 127.5f - 1.0f;
			};

			faces.reserve(k.prim_count);
			for (UINT t = 0; t < k.prim_count; t++)
			{
				const UINT first = strip ? t : t * 3;
				const int64_t idx[3] = { index(first), index(first + 1), index(first + 2) };
				if (idx[0] == idx[1] || idx[1] == idx[2] || idx[0] == idx[2])
					continue;
				const unsigned char* v[3];
				bool ok = true;
				for (int c = 0; c < 3 && ok; c++)
					ok = (v[c] = vertex(idx[c])) != nullptr;
				if (!ok)
					continue;

				float p[3][3], nsum[3] = {};
				for (int c = 0; c < 3; c++)
				{
					std::memcpy(p[c], v[c] + l.pos_off, sizeof(p[c]));
					float n[3];
					normal(v[c], n);
					nsum[0] += n[0]; nsum[1] += n[1]; nsum[2] += n[2];
				}
				const float a[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
				const float b[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
				float g[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
				const float glen = std::sqrt(dot3(g, g));
				const float nlen = std::sqrt(dot3(nsum, nsum));
				const float area = 0.5f * glen;
				if (!(area > kMinTriangleArea) || !(nlen > 1e-3f))
					continue;
				total_area += area;
				for (float& c : g) c /= glen;
				const float along = dot3(g, nsum) / nlen;
				// Only flat faces whose vertex normals follow the plane say which side is their front.
				if (std::fabs(along) < kFacing)
					continue;

				face f {};
				for (int c = 0; c < 3; c++)
				{
					f.centre[c] = (p[0][c] + p[1][c] + p[2][c]) / 3.0f;
					f.front[c] = along > 0.0f ? g[c] : -g[c];
					f.lo[c] = std::min({ p[0][c], p[1][c], p[2][c] });
					f.hi[c] = std::max({ p[0][c], p[1][c], p[2][c] });
				}
				f.area = area;
				faces.push_back(f);
			}
			vb->Unlock();
			ib->Unlock();
			return true;
		}

		// Area of the faces that have a face in front of them, within kMaxGap, overlapping them, whose front points back.
		float facing_pair_area(const std::vector<face>& faces)
		{
			std::unordered_map<int64_t, std::vector<uint32_t>> buckets;
			buckets.reserve(faces.size());
			for (uint32_t i = 0; i < faces.size(); i++)
			{
				const face& f = faces[i];
				const auto bin = static_cast<int64_t>(std::floor(dot3(f.front, f.centre) / kMaxGap));
				buckets[bucket_key(quantise(f.front[0]), quantise(f.front[1]), quantise(f.front[2]), bin)].push_back(i);
			}

			float paired = 0.0f;
			for (const face& a : faces)
			{
				const float plane = dot3(a.front, a.centre);
				// A face B in front of A, its front opposite: dot(B.front, B.centre) = -(plane + gap).
				const auto bin_lo = static_cast<int64_t>(std::floor((-plane - kMaxGap) / kMaxGap));
				const auto bin_hi = static_cast<int64_t>(std::floor((-plane - kMinGap) / kMaxGap));
				const int qx = quantise(-a.front[0]), qy = quantise(-a.front[1]), qz = quantise(-a.front[2]);
				bool found = false;
				for (int64_t bin = bin_lo; bin <= bin_hi && !found; bin++)
					for (int dx = -1; dx <= 1 && !found; dx++)
						for (int dy = -1; dy <= 1 && !found; dy++)
							for (int dz = -1; dz <= 1 && !found; dz++)
							{
								const auto it = buckets.find(bucket_key(std::clamp(qx + dx, -4, 4), std::clamp(qy + dy, -4, 4),
									std::clamp(qz + dz, -4, 4), bin));
								if (it == buckets.end())
									continue;
								for (const uint32_t j : it->second)
								{
									const face& b = faces[j];
									if (dot3(a.front, b.front) > -kFacing)
										continue;
									const float d[3] = { b.centre[0] - a.centre[0], b.centre[1] - a.centre[1], b.centre[2] - a.centre[2] };
									const float gap = dot3(a.front, d);
									if (gap < kMinGap || gap > kMaxGap)
										continue;
									bool overlap = true;
									for (int c = 0; c < 3 && overlap; c++)
										overlap = a.lo[c] <= b.hi[c] + kMaxGap && b.lo[c] <= a.hi[c] + kMaxGap;
									if (overlap)
									{
										found = true;
										break;
									}
								}
							}
				if (found)
					paired += a.area;
			}
			return paired;
		}

		bool measure(const mesh_key& k, const layout& l, IDirect3DVertexBuffer9* vb, IDirect3DIndexBuffer9* ib)
		{
			std::vector<face> faces;
			float total_area = 0.0f;
			if (!read_faces(k, l, vb, ib, faces, total_area) || faces.empty())
				return false;
			const float paired = facing_pair_area(faces);
			const bool inside_out = paired >= kMinPairedArea && paired >= kMinShare * total_area;
			if (inside_out)
				shared::common::log("ThinWalls", std::format("double-sided: {} triangles, {:.1f} of {:.1f} m^2 in facing pairs",
					k.prim_count, paired, total_area));
			return inside_out;
		}
	}

	bool is_inside_out(IDirect3DVertexBuffer9* vb, UINT vb_offset, UINT stride, IDirect3DVertexDeclaration9* decl,
		IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE prim, INT base_vertex, UINT start_index, UINT prim_count,
		unsigned int frame)
	{
		if (!vb || !ib || !decl || !stride || !prim_count || prim_count > kMaxTriangles ||
			(prim != D3DPT_TRIANGLELIST && prim != D3DPT_TRIANGLESTRIP))
			return false;

		std::lock_guard lock(g_mutex);
		if (frame != g_budget_frame)
		{
			g_budget_frame = frame;
			g_measures_left = kMeasuresPerFrame;
		}

		const mesh_key k { reinterpret_cast<uintptr_t>(vb), reinterpret_cast<uintptr_t>(ib), reinterpret_cast<uintptr_t>(decl),
			vb_offset, stride, base_vertex, start_index, prim_count, static_cast<DWORD>(prim) };
		auto& e = g_meshes[k];
		e.last_frame = frame;
		if (!e.measured && g_measures_left > 0)
		{
			g_measures_left--;
			const layout l = layout_of(decl, stride);
			e.inside_out = l.valid && measure(k, l, vb, ib);
			e.measured = true;
		}

		if (frame - g_last_prune > kPruneFrames)
		{
			g_last_prune = frame;
			std::erase_if(g_meshes, [&](const auto& kv) { return frame - kv.second.last_frame > kPruneFrames; });
		}
		return e.inside_out;
	}

	void on_index_buffer_created(IDirect3DIndexBuffer9* ib)
	{
		const auto key = reinterpret_cast<uintptr_t>(ib);
		std::lock_guard lock(g_mutex);
		std::erase_if(g_meshes, [&](const auto& kv) { return kv.first.ib == key; });
	}
}
