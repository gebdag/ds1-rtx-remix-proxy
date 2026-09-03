#include "std_include.hpp"
#include "frustum_culling.hpp"
#include "camera.hpp"
#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"

namespace comp::game::frustum_culling
{
	struct patch_site
	{
		DWORD rva;
		BYTE original[6];
	};

	// True layouts of the bounds the engine hands each test
	// (analysis/ghidra_camera_culling.md §1). The leaf tests do NOT take min/max
	// AABBs: 0x5C5720 tests a sphere-swept segment (capsule) and 0x5C5870 a
	// sphere-swept parallelogram. Only the coarse region test takes two corners.
	struct swept_segment_bounds // 0x5C5720
	{
		float origin[4]; // world position A
		float extent[4]; // B = origin + extent — a direction, NOT a corner
		float radius;
	};

	struct swept_parallelogram_bounds // 0x5C5870
	{
		float origin[4];
		float dir1[4];
		float dir2[4];
		float radius;
	};

	struct region_bounds // 0x593E40 — genuinely two opposite corners
	{
		float corner_a[4];
		float corner_b[4];
	};

	using coarse_frustum_fn = int(__cdecl*)(void*, const region_bounds*);
	using segment_frustum_fn = int(__cdecl*)(void*, const swept_segment_bounds*);
	using parallelogram_frustum_fn = int(__cdecl*)(void*, const swept_parallelogram_bounds*);

	// World-geometry frustum tests reached from HGFrustum virtual wrappers.
	// Return 0 = outside, 1 = straddling, 2 = fully inside. The proxy detours them
	// to preserve stock behavior, fully disable culling, or relax culling within a
	// radius of the tested frustum's own apex (the camera position).
	static constexpr patch_site PATCH_SITES[] = {
		{ 0x00193E40, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 } },
		{ 0x001C5720, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 } },
		{ 0x001C5870, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 } },
	};

	static bool available_ = false;
	static bool enabled_ = true;
	static bool hooks_installed_ = false;
	static float radius_ = 0.0f;

	static coarse_frustum_fn coarse_frustum_original_ = nullptr;
	static segment_frustum_fn segment_frustum_original_ = nullptr;
	static parallelogram_frustum_fn parallelogram_frustum_original_ = nullptr;

	static DWORD patch_address(const patch_site& site)
	{
		return EXE_BASE + site.rva;
	}

	static bool bytes_match(DWORD address, const BYTE* expected, size_t size)
	{
		return memcmp(reinterpret_cast<const void*>(address), expected, size) == 0;
	}

	static bool site_matches(const patch_site& site)
	{
		return bytes_match(patch_address(site), site.original, sizeof(site.original));
	}

	static UINT current_frame()
	{
		return shared::common::ffp_state::get().frame_count();
	}

	// --- Camera apex recovery -------------------------------------------------
	//
	// A frustum is 6 world-space planes (n.xyz, d) starting at the pointer the engine
	// hands the test. All four side planes meet at the camera, so intersecting three of
	// them yields the camera's absolute world position — in the same space as the tested
	// bounds by construction. Sampling is keyed on the frustum pointer so the solve runs
	// once per distinct frustum per frame, not per test.

	static constexpr int MAX_APEX_SAMPLES = MAX_FRAME_APEXES;
	static apex_sample apex_samples_[MAX_APEX_SAMPLES];
	static int apex_sample_count_ = 0;
	static UINT apex_frame_ = 0;

	static bool solve_plane_intersection(const float* planes, float& x, float& y, float& z)
	{
		// Planes 0-2 (left, right, bottom) all contain the apex.
		const float m[3][3] = {
			{ planes[0], planes[1], planes[2] },
			{ planes[4], planes[5], planes[6] },
			{ planes[8], planes[9], planes[10] },
		};
		const float rhs[3] = { -planes[3], -planes[7], -planes[11] };

		const float cofactor00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
		const float cofactor01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
		const float cofactor02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
		const float det = m[0][0] * cofactor00 + m[0][1] * cofactor01 + m[0][2] * cofactor02;
		if (std::fabs(det) < 1.0e-6f)
		{
			return false; // parallel side planes — an orthographic frustum has no apex
		}

		const float inv_det = 1.0f / det;
		x = inv_det * (rhs[0] * cofactor00 + rhs[1] * (m[0][2] * m[2][1] - m[0][1] * m[2][2]) +
			rhs[2] * (m[0][1] * m[1][2] - m[0][2] * m[1][1]));
		y = inv_det * (rhs[0] * cofactor01 + rhs[1] * (m[0][0] * m[2][2] - m[0][2] * m[2][0]) +
			rhs[2] * (m[0][2] * m[1][0] - m[0][0] * m[1][2]));
		z = inv_det * (rhs[0] * cofactor02 + rhs[1] * (m[0][1] * m[2][0] - m[0][0] * m[2][1]) +
			rhs[2] * (m[0][0] * m[1][1] - m[0][1] * m[1][0]));
		return true;
	}

	// Returns the apex for this frustum, solving and caching it on first sight each
	// frame. Null when the frustum has no apex (orthographic) or the table is full.
	static const apex_sample* apex_for_frustum(const void* frustum)
	{
		if (!frustum)
		{
			return nullptr;
		}

		const UINT frame = current_frame();
		if (frame != apex_frame_)
		{
			apex_frame_ = frame;
			apex_sample_count_ = 0;
		}

		for (int i = 0; i < apex_sample_count_; ++i)
		{
			if (apex_samples_[i].frustum == frustum)
			{
				++apex_samples_[i].tests;
				return &apex_samples_[i];
			}
		}

		if (apex_sample_count_ >= MAX_APEX_SAMPLES)
		{
			return nullptr;
		}

		float x = 0.0f, y = 0.0f, z = 0.0f;
		if (!solve_plane_intersection(reinterpret_cast<const float*>(frustum), x, y, z))
		{
			return nullptr;
		}

		apex_samples_[apex_sample_count_] = { frustum, x, y, z, 1 };
		return &apex_samples_[apex_sample_count_++];
	}

	int frame_apexes(apex_sample* out, int max_out)
	{
		const int count = std::min(max_out, apex_sample_count_);
		for (int i = 0; i < count; ++i)
		{
			out[i] = apex_samples_[i];
		}
		return count;
	}

	// --- Distance from the relaxation origin to the tested shape ---------------

	static float distance_sq_point_to_segment(
		const float px, const float py, const float pz,
		const float ax, const float ay, const float az,
		const float ex, const float ey, const float ez)
	{
		const float len_sq = ex * ex + ey * ey + ez * ez;
		float t = 0.0f;
		if (len_sq > 1.0e-8f)
		{
			t = ((px - ax) * ex + (py - ay) * ey + (pz - az) * ez) / len_sq;
			t = std::clamp(t, 0.0f, 1.0f);
		}
		const float dx = px - (ax + ex * t);
		const float dy = py - (ay + ey * t);
		const float dz = pz - (az + ez * t);
		return dx * dx + dy * dy + dz * dz;
	}

	// Closest point via clamped projection on each edge axis — exact for rectangles,
	// a tight approximation for sheared parallelograms.
	static float distance_sq_point_to_parallelogram(
		const float px, const float py, const float pz,
		const float* origin, const float* dir1, const float* dir2)
	{
		const float rx = px - origin[0];
		const float ry = py - origin[1];
		const float rz = pz - origin[2];

		const float d1_sq = dir1[0] * dir1[0] + dir1[1] * dir1[1] + dir1[2] * dir1[2];
		const float d2_sq = dir2[0] * dir2[0] + dir2[1] * dir2[1] + dir2[2] * dir2[2];

		float s = 0.0f;
		if (d1_sq > 1.0e-8f)
		{
			s = std::clamp((rx * dir1[0] + ry * dir1[1] + rz * dir1[2]) / d1_sq, 0.0f, 1.0f);
		}
		float t = 0.0f;
		if (d2_sq > 1.0e-8f)
		{
			t = std::clamp((rx * dir2[0] + ry * dir2[1] + rz * dir2[2]) / d2_sq, 0.0f, 1.0f);
		}

		const float dx = rx - dir1[0] * s - dir2[0] * t;
		const float dy = ry - dir1[1] * s - dir2[1] * t;
		const float dz = rz - dir1[2] * s - dir2[2] * t;
		return dx * dx + dy * dy + dz * dz;
	}

	static float distance_sq_point_to_aabb(
		const float px, const float py, const float pz,
		const float* corner_a, const float* corner_b)
	{
		const float low_x = std::min(corner_a[0], corner_b[0]);
		const float high_x = std::max(corner_a[0], corner_b[0]);
		const float low_y = std::min(corner_a[1], corner_b[1]);
		const float high_y = std::max(corner_a[1], corner_b[1]);
		const float low_z = std::min(corner_a[2], corner_b[2]);
		const float high_z = std::max(corner_a[2], corner_b[2]);

		const float dx = px < low_x ? (low_x - px) : (px > high_x ? px - high_x : 0.0f);
		const float dy = py < low_y ? (low_y - py) : (py > high_y ? py - high_y : 0.0f);
		const float dz = pz < low_z ? (low_z - pz) : (pz > high_z ? pz - high_z : 0.0f);
		return dx * dx + dy * dy + dz * dz;
	}

	static bool within_radius(const apex_sample* apex, const float distance_sq, const float shape_radius)
	{
		if (!apex)
		{
			return false;
		}
		const float allowed = radius_ + std::max(0.0f, shape_radius);
		return distance_sq <= allowed * allowed;
	}

	// --- Hooks ------------------------------------------------------------------
	//
	// Relaxation policy: with culling disabled and a radius set, any bounds whose
	// swept shape lies within the radius of the tested frustum's own apex is forced
	// fully visible — no visibility history required, so geometry that was never
	// on screen (e.g. behind the player on approach) comes back too. The coarse
	// region test is relaxed the same way, because a failed region test prunes the
	// whole subtree before the leaf tests ever see its contents.

	static int __cdecl coarse_frustum_hk(void* frustum, const region_bounds* region)
	{
		const apex_sample* apex = apex_for_frustum(frustum);

		if (enabled_)
		{
			return coarse_frustum_original_(frustum, region);
		}

		if (radius_ <= 0.0f)
		{
			return 2;
		}

		const int original_result = coarse_frustum_original_(frustum, region);
		if (original_result != 0 || !region)
		{
			return original_result;
		}

		const float distance_sq = apex
			? distance_sq_point_to_aabb(apex->x, apex->y, apex->z, region->corner_a, region->corner_b)
			: 0.0f;
		return within_radius(apex, distance_sq, 0.0f) ? 2 : original_result;
	}

	static int __cdecl segment_frustum_hk(void* frustum, const swept_segment_bounds* bounds)
	{
		const apex_sample* apex = apex_for_frustum(frustum);

		if (enabled_)
		{
			return segment_frustum_original_(frustum, bounds);
		}

		if (radius_ <= 0.0f)
		{
			return 2;
		}

		const int original_result = segment_frustum_original_(frustum, bounds);
		if (original_result != 0 || !bounds)
		{
			return original_result;
		}

		const float distance_sq = apex
			? distance_sq_point_to_segment(apex->x, apex->y, apex->z,
				bounds->origin[0], bounds->origin[1], bounds->origin[2],
				bounds->extent[0], bounds->extent[1], bounds->extent[2])
			: 0.0f;
		return within_radius(apex, distance_sq, bounds->radius) ? 2 : original_result;
	}

	static int __cdecl parallelogram_frustum_hk(void* frustum, const swept_parallelogram_bounds* bounds)
	{
		const apex_sample* apex = apex_for_frustum(frustum);

		if (enabled_)
		{
			return parallelogram_frustum_original_(frustum, bounds);
		}

		if (radius_ <= 0.0f)
		{
			return 2;
		}

		const int original_result = parallelogram_frustum_original_(frustum, bounds);
		if (original_result != 0 || !bounds)
		{
			return original_result;
		}

		const float distance_sq = apex
			? distance_sq_point_to_parallelogram(apex->x, apex->y, apex->z,
				bounds->origin, bounds->dir1, bounds->dir2)
			: 0.0f;
		return within_radius(apex, distance_sq, bounds->radius) ? 2 : original_result;
	}

	static void log_state()
	{
		const auto message = enabled_
			? "Engine frustum culling enabled: stock game behavior."
			: radius_ > 0.0f
				? std::format("Engine frustum culling modified: world bounds within {:.1f} units of the camera stay visible.", radius_)
				: "Engine frustum culling disabled globally.";

		shared::common::log(
			"FrustumCulling",
			message,
			enabled_ ? shared::common::LOG_TYPE::LOG_TYPE_GREEN : shared::common::LOG_TYPE::LOG_TYPE_WARN,
			true);
	}

	static bool set_patch_state(bool enabled)
	{
		enabled_ = enabled;
		shared::common::config::get().culling.enable_frustum_culling = enabled;
		if (available_)
		{
			log_state();
		}
		return available_;
	}

	static bool set_patch_radius(float radius)
	{
		radius_ = std::max(0.0f, radius);
		shared::common::config::get().culling.frustum_relaxation_radius = radius_;
		if (available_ && !enabled_)
		{
			log_state();
		}
		return available_;
	}

	void install_hooks()
	{
		if (!available_ || hooks_installed_)
		{
			return;
		}

		const bool hooks_ok =
			shared::utils::hook::detour(
				patch_address(PATCH_SITES[0]),
				reinterpret_cast<void*>(&coarse_frustum_hk),
				reinterpret_cast<void**>(&coarse_frustum_original_)) &&
			shared::utils::hook::detour(
				patch_address(PATCH_SITES[1]),
				reinterpret_cast<void*>(&segment_frustum_hk),
				reinterpret_cast<void**>(&segment_frustum_original_)) &&
			shared::utils::hook::detour(
				patch_address(PATCH_SITES[2]),
				reinterpret_cast<void*>(&parallelogram_frustum_hk),
				reinterpret_cast<void**>(&parallelogram_frustum_original_));

		if (!hooks_ok)
		{
			available_ = false;
			shared::common::log(
				"FrustumCulling",
				"Failed to install frustum culling hooks; patch unavailable.",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR,
				true);
			return;
		}

		hooks_installed_ = true;
		log_state();
	}

	void init()
	{
		available_ = true;
		enabled_ = shared::common::config::get().culling.enable_frustum_culling;
		radius_ = std::max(0.0f, shared::common::config::get().culling.frustum_relaxation_radius);
		for (const auto& site : PATCH_SITES)
		{
			if (!site_matches(site))
			{
				available_ = false;
				shared::common::log(
					"FrustumCulling",
					std::format("Unexpected bytes at 0x{:08X}; frustum culling patch unavailable.", patch_address(site)),
					shared::common::LOG_TYPE::LOG_TYPE_ERROR,
					true);
				break;
			}
		}
	}

	bool is_available()
	{
		return available_;
	}

	bool is_enabled()
	{
		return enabled_;
	}

	float radius()
	{
		return radius_;
	}

	void set_enabled(bool enabled)
	{
		set_patch_state(enabled);
	}

	void set_radius(float radius)
	{
		set_patch_radius(radius);
	}
}
