#include "std_include.hpp"
#include "frustum_culling.hpp"
#include "game_build.hpp"
#include "camera.hpp"
#include "sun_light.hpp"
#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"

#include <atomic>

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
	// sphere-swept parallelogram. Only the region test takes two corners.
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

	// 0x5AEF50 moves the frustum into the box's local space (transposed transform)
	// and runs the region test there against the box [-half_extent, +half_extent].
	struct oriented_box_bounds
	{
		float half_extent[4];
		float transform[16]; // local -> world, row-vector convention: rows 0-2 axes, row 3 translation
	};

	using region_frustum_fn = int(__cdecl*)(void*, const region_bounds*);
	using segment_frustum_fn = int(__cdecl*)(void*, const swept_segment_bounds*);
	using parallelogram_frustum_fn = int(__cdecl*)(void*, const swept_parallelogram_bounds*);
	using oriented_box_frustum_fn = int(__cdecl*)(void*, const oriented_box_bounds*);

	// World-geometry frustum tests reached from the HGFrustum virtual wrappers
	// (vtable 0x11E0324 slots +0x18..+0x24). Return 0 = outside, 1 = straddling,
	// 2 = fully inside. The proxy detours them to preserve stock behavior, fully
	// disable culling, relax culling within a radius of the tested frustum's own
	// apex, and/or keep large light blockers the engine would cull.
	static constexpr patch_site PATCH_SITES[] = {
		{ 0x00193E40, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 } }, // region AABB
		{ 0x001C5720, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 } }, // swept segment
		{ 0x001C5870, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 } }, // swept parallelogram
		{ 0x001AEF50, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 } }, // oriented box
	};

	static bool available_ = false;
	static bool enabled_ = true;
	static bool hooks_installed_ = false;
	static float radius_ = 0.0f;
	static bool keep_light_blockers_ = false;
	static light_blocker_settings blocker_settings_;

	static region_frustum_fn region_frustum_original_ = nullptr;
	static segment_frustum_fn segment_frustum_original_ = nullptr;
	static parallelogram_frustum_fn parallelogram_frustum_original_ = nullptr;
	static oriented_box_frustum_fn oriented_box_frustum_original_ = nullptr;

	// The oriented-box test calls the region test with a frustum it built on its own
	// stack in box-local space. That inner call is part of the outer test, not a
	// separate world-space query: it must reach the engine untouched and must not be
	// solved for an apex (the pointer is reused with different planes every call).
	static thread_local bool in_oriented_box_test_ = false;

	// The RVAs are the 2015-06-15 build's (game_build.hpp); 0 when not available on this build.
	static DWORD patch_address(const patch_site& site)
	{
		return build::at(0x00400000 + site.rva);
	}

	static bool bytes_match(DWORD address, const BYTE* expected, size_t size)
	{
		if (!address)
			return false;
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

	static bool fully_disabled()
	{
		return !enabled_ && radius_ <= 0.0f;
	}

	// --- Per-frame light blocker counters ----------------------------------------

	static std::atomic<UINT> blocker_rejected_ = 0;
	static std::atomic<UINT> blocker_kept_near_ = 0;
	static std::atomic<UINT> blocker_kept_sun_ = 0;
	static std::atomic<UINT> blocker_kept_regions_ = 0;
	static std::atomic<UINT> blocker_unreadable_boxes_ = 0;
	static light_blocker_stats blocker_last_frame_;

	static void publish_blocker_stats()
	{
		blocker_last_frame_ = {
			blocker_rejected_.exchange(0, std::memory_order_relaxed),
			blocker_kept_near_.exchange(0, std::memory_order_relaxed),
			blocker_kept_sun_.exchange(0, std::memory_order_relaxed),
			blocker_kept_regions_.exchange(0, std::memory_order_relaxed),
			blocker_unreadable_boxes_.exchange(0, std::memory_order_relaxed),
		};
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
			publish_blocker_stats();
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

	// --- Tested shapes, reduced to what the relaxation policies need -------------

	enum class shape_kind { region, segment, parallelogram, oriented_box };

	struct shape_summary
	{
		shape_kind kind;
		float center[3];
		float bounding_radius;    // sphere around the whole swept shape
		float core_distance_sq;   // apex to the unswept core shape
		float sweep_radius;       // added to the core on every side
	};

	static float length3(const float x, const float y, const float z)
	{
		return std::sqrt(x * x + y * y + z * z);
	}

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

	static shape_summary summarize(const region_bounds& b, const apex_sample& apex)
	{
		const float* a = b.corner_a;
		const float* c = b.corner_b;
		return {
			shape_kind::region,
			{ (a[0] + c[0]) * 0.5f, (a[1] + c[1]) * 0.5f, (a[2] + c[2]) * 0.5f },
			0.5f * length3(c[0] - a[0], c[1] - a[1], c[2] - a[2]),
			distance_sq_point_to_aabb(apex.x, apex.y, apex.z, a, c),
			0.0f,
		};
	}

	static shape_summary summarize(const swept_segment_bounds& b, const apex_sample& apex)
	{
		const float* o = b.origin;
		const float* e = b.extent;
		const float sweep = std::max(0.0f, b.radius);
		return {
			shape_kind::segment,
			{ o[0] + e[0] * 0.5f, o[1] + e[1] * 0.5f, o[2] + e[2] * 0.5f },
			0.5f * length3(e[0], e[1], e[2]) + sweep,
			distance_sq_point_to_segment(apex.x, apex.y, apex.z, o[0], o[1], o[2], e[0], e[1], e[2]),
			sweep,
		};
	}

	static shape_summary summarize(const swept_parallelogram_bounds& b, const apex_sample& apex)
	{
		const float* o = b.origin;
		const float* d1 = b.dir1;
		const float* d2 = b.dir2;
		const float sweep = std::max(0.0f, b.radius);
		const float diagonal = std::max(
			length3(d1[0] + d2[0], d1[1] + d2[1], d1[2] + d2[2]),
			length3(d1[0] - d2[0], d1[1] - d2[1], d1[2] - d2[2]));
		return {
			shape_kind::parallelogram,
			{ o[0] + (d1[0] + d2[0]) * 0.5f, o[1] + (d1[1] + d2[1]) * 0.5f, o[2] + (d1[2] + d2[2]) * 0.5f },
			0.5f * diagonal + sweep,
			distance_sq_point_to_parallelogram(apex.x, apex.y, apex.z, o, d1, d2),
			sweep,
		};
	}

	// False when the transform is not an affine row-vector matrix, i.e. when this
	// layout reading does not hold for the box at hand.
	static bool summarize(const oriented_box_bounds& b, const apex_sample& apex, shape_summary& out)
	{
		const float* m = b.transform;
		const bool affine_row_vector =
			std::fabs(m[3]) < 1.0e-3f && std::fabs(m[7]) < 1.0e-3f && std::fabs(m[11]) < 1.0e-3f &&
			std::fabs(m[15] - 1.0f) < 1.0e-3f;
		if (!affine_row_vector)
		{
			return false;
		}

		const float* axes[3] = { &m[0], &m[4], &m[8] };
		const float* center = &m[12];
		const float r[3] = { apex.x - center[0], apex.y - center[1], apex.z - center[2] };

		float closest[3] = { center[0], center[1], center[2] };
		float half_diagonal_sq = 0.0f;
		for (int i = 0; i < 3; ++i)
		{
			const float* axis = axes[i];
			const float axis_sq = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
			const float half = std::fabs(b.half_extent[i]);
			half_diagonal_sq += axis_sq * half * half;
			if (axis_sq <= 1.0e-8f)
			{
				continue;
			}
			const float local = std::clamp((r[0] * axis[0] + r[1] * axis[1] + r[2] * axis[2]) / axis_sq, -half, half);
			for (int k = 0; k < 3; ++k)
			{
				closest[k] += axis[k] * local;
			}
		}

		const float dx = apex.x - closest[0];
		const float dy = apex.y - closest[1];
		const float dz = apex.z - closest[2];
		out = {
			shape_kind::oriented_box,
			{ center[0], center[1], center[2] },
			std::sqrt(half_diagonal_sq),
			dx * dx + dy * dy + dz * dz,
			0.0f,
		};
		return true;
	}

	// --- Relaxation policies ------------------------------------------------------
	//
	// Culling-off radius: with engine culling disabled and a radius set, any bounds
	// within the radius of the tested frustum's own apex is forced fully visible — no
	// visibility history required, so geometry that was never on screen (e.g. behind
	// the player on approach) comes back too. The region test is relaxed the same way,
	// because a failed region test prunes the whole subtree before the leaf tests ever
	// see its contents.
	//
	// Light blockers: a path tracer needs off-screen geometry that stands between the
	// visible scene and the sky or sun, but only the large pieces matter — a wall or a
	// roof, not a crate. A rejected shape of at least min_size is kept when it lies
	// within near_range of the camera (roofs, walls behind the player) or when its
	// shadow — the shape swept along the sun's direction by sun_reach — falls into the
	// view frustum. Only perspective frustums qualify, so the orthographic shadow
	// cascades keep stock culling. Kept shapes report "straddling" rather than "fully
	// inside" so a kept region still has its contents tested one by one.

	static bool within_culling_off_radius(const shape_summary& s)
	{
		const float allowed = radius_ + s.sweep_radius;
		return s.core_distance_sq <= allowed * allowed;
	}

	static bool shadow_reaches_view(void* frustum, const shape_summary& s)
	{
		float sun_dir[3];
		if (blocker_settings_.sun_reach <= 0.0f || !sun_light::key_direction(sun_dir))
		{
			return false;
		}

		const float reach = blocker_settings_.sun_reach;
		const swept_segment_bounds shadow = {
			{ s.center[0], s.center[1], s.center[2], 1.0f },
			{ sun_dir[0] * reach, sun_dir[1] * reach, sun_dir[2] * reach, 0.0f },
			s.bounding_radius,
		};
		return segment_frustum_original_(frustum, &shadow) != 0;
	}

	// Returns the visibility to report for a light blocker, or 0 to keep it culled.
	static int keep_as_light_blocker(void* frustum, const shape_summary& s)
	{
		blocker_rejected_.fetch_add(1, std::memory_order_relaxed);
		if (2.0f * s.bounding_radius < blocker_settings_.min_size)
		{
			return 0;
		}

		const float surface_distance = std::max(0.0f, std::sqrt(s.core_distance_sq) - s.sweep_radius);
		const bool close_to_camera = surface_distance <= blocker_settings_.near_range;
		if (!close_to_camera && !shadow_reaches_view(frustum, s))
		{
			return 0;
		}

		(close_to_camera ? blocker_kept_near_ : blocker_kept_sun_).fetch_add(1, std::memory_order_relaxed);
		if (s.kind == shape_kind::region)
		{
			blocker_kept_regions_.fetch_add(1, std::memory_order_relaxed);
		}
		return 1;
	}

	// Decides the visibility of a shape the engine rejected (result 0).
	static int relax_rejected(void* frustum, const shape_summary& s)
	{
		if (!enabled_ && radius_ > 0.0f && within_culling_off_radius(s))
		{
			return 2;
		}
		if (keep_light_blockers_)
		{
			return keep_as_light_blocker(frustum, s);
		}
		return 0;
	}

	static bool any_relaxation()
	{
		return !enabled_ || keep_light_blockers_;
	}

	// --- Hooks ------------------------------------------------------------------

	static int __cdecl region_frustum_hk(void* frustum, const region_bounds* region)
	{
		if (in_oriented_box_test_)
		{
			return region_frustum_original_(frustum, region);
		}

		const apex_sample* apex = apex_for_frustum(frustum);
		if (fully_disabled())
		{
			return 2;
		}

		const int result = region_frustum_original_(frustum, region);
		if (result != 0 || !region || !apex || !any_relaxation())
		{
			return result;
		}
		return relax_rejected(frustum, summarize(*region, *apex));
	}

	static int __cdecl segment_frustum_hk(void* frustum, const swept_segment_bounds* bounds)
	{
		const apex_sample* apex = apex_for_frustum(frustum);
		if (fully_disabled())
		{
			return 2;
		}

		const int result = segment_frustum_original_(frustum, bounds);
		if (result != 0 || !bounds || !apex || !any_relaxation())
		{
			return result;
		}
		return relax_rejected(frustum, summarize(*bounds, *apex));
	}

	static int __cdecl parallelogram_frustum_hk(void* frustum, const swept_parallelogram_bounds* bounds)
	{
		const apex_sample* apex = apex_for_frustum(frustum);
		if (fully_disabled())
		{
			return 2;
		}

		const int result = parallelogram_frustum_original_(frustum, bounds);
		if (result != 0 || !bounds || !apex || !any_relaxation())
		{
			return result;
		}
		return relax_rejected(frustum, summarize(*bounds, *apex));
	}

	static int __cdecl oriented_box_frustum_hk(void* frustum, const oriented_box_bounds* box)
	{
		const apex_sample* apex = apex_for_frustum(frustum);
		if (fully_disabled())
		{
			return 2;
		}

		in_oriented_box_test_ = true;
		const int result = oriented_box_frustum_original_(frustum, box);
		in_oriented_box_test_ = false;
		if (result != 0 || !box || !apex || !any_relaxation())
		{
			return result;
		}

		shape_summary summary;
		if (!summarize(*box, *apex, summary))
		{
			blocker_unreadable_boxes_.fetch_add(1, std::memory_order_relaxed);
			return result;
		}
		return relax_rejected(frustum, summary);
	}

	static void log_state()
	{
		std::string message = enabled_
			? "Engine frustum culling enabled: stock game behavior."
			: radius_ > 0.0f
				? std::format("Engine frustum culling modified: world bounds within {:.1f} units of the camera stay visible.", radius_)
				: "Engine frustum culling disabled globally.";
		if (keep_light_blockers_ && !fully_disabled())
		{
			message += std::format(" Keeping light blockers >= {:.1f} units (near {:.1f}, sun reach {:.1f}).",
				blocker_settings_.min_size, blocker_settings_.near_range, blocker_settings_.sun_reach);
		}

		shared::common::log(
			"FrustumCulling",
			message,
			shared::common::LOG_TYPE::LOG_TYPE_DEFAULT,
			true);
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
				reinterpret_cast<void*>(&region_frustum_hk),
				reinterpret_cast<void**>(&region_frustum_original_)) &&
			shared::utils::hook::detour(
				patch_address(PATCH_SITES[1]),
				reinterpret_cast<void*>(&segment_frustum_hk),
				reinterpret_cast<void**>(&segment_frustum_original_)) &&
			shared::utils::hook::detour(
				patch_address(PATCH_SITES[2]),
				reinterpret_cast<void*>(&parallelogram_frustum_hk),
				reinterpret_cast<void**>(&parallelogram_frustum_original_)) &&
			shared::utils::hook::detour(
				patch_address(PATCH_SITES[3]),
				reinterpret_cast<void*>(&oriented_box_frustum_hk),
				reinterpret_cast<void**>(&oriented_box_frustum_original_));

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

	static light_blocker_settings sanitized(const light_blocker_settings& settings)
	{
		return {
			std::max(0.0f, settings.min_size),
			std::max(0.0f, settings.near_range),
			std::max(0.0f, settings.sun_reach),
		};
	}

	void init()
	{
		const auto& culling = shared::common::config::get().culling;
		available_ = true;
		enabled_ = culling.enable_frustum_culling;
		radius_ = std::max(0.0f, culling.frustum_relaxation_radius);
		keep_light_blockers_ = culling.keep_light_blockers;
		blocker_settings_ = sanitized({
			culling.light_blocker_min_size,
			culling.light_blocker_near_range,
			culling.light_blocker_sun_reach,
		});
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
		enabled_ = enabled;
		shared::common::config::get().culling.enable_frustum_culling = enabled;
		if (available_)
		{
			log_state();
		}
	}

	void set_radius(float radius)
	{
		radius_ = std::max(0.0f, radius);
		shared::common::config::get().culling.frustum_relaxation_radius = radius_;
		if (available_ && !enabled_)
		{
			log_state();
		}
	}

	bool keeps_light_blockers()
	{
		return keep_light_blockers_;
	}

	void set_keep_light_blockers(bool keep)
	{
		keep_light_blockers_ = keep;
		shared::common::config::get().culling.keep_light_blockers = keep;
		if (available_)
		{
			log_state();
		}
	}

	light_blocker_settings light_blockers()
	{
		return blocker_settings_;
	}

	void reread_light_blockers()
	{
		const auto& culling = shared::common::config::get().culling;
		enabled_ = culling.enable_frustum_culling;
		radius_ = std::max(0.0f, culling.frustum_relaxation_radius);
		keep_light_blockers_ = culling.keep_light_blockers;
		blocker_settings_ = sanitized({
			culling.light_blocker_min_size,
			culling.light_blocker_near_range,
			culling.light_blocker_sun_reach,
		});
	}

	void set_light_blockers(const light_blocker_settings& settings)
	{
		blocker_settings_ = sanitized(settings);
		auto& culling = shared::common::config::get().culling;
		culling.light_blocker_min_size = blocker_settings_.min_size;
		culling.light_blocker_near_range = blocker_settings_.near_range;
		culling.light_blocker_sun_reach = blocker_settings_.sun_reach;
	}

	light_blocker_stats last_frame_light_blockers()
	{
		return blocker_last_frame_;
	}
}
