#include "std_include.hpp"
#include "atmosphere.hpp"

#include "shared/common/remix_api.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/globals.hpp"
#include "sun_light.hpp"

#include <cmath>

#include <cstring>
#include <format>

namespace comp::game::atmosphere
{
	namespace
	{
		struct haze_values
		{
			float ps[9][4] = {};     // c103..c111
			float exposure[4] = {};  // c135
			float fog_range[4] = {}; // VS c128

			bool operator==(const haze_values& o) const { return std::memcmp(this, &o, sizeof(haze_values)) == 0; }
		};

		struct grade_values
		{
			float scale[4] = {};   // c56
			float rows[3][4] = {}; // c62..c64
			float bright[4] = {};  // c11: x = bright pass threshold near
			float bloom_far[4] = {};     // c71: x threshold far, y bloom rate far, zw bloom depth range
			unsigned int bloom_w = 0, bloom_h = 0; // the bloom buffer ColAdj reads (s1)
			// FRPG_Fil_Dof*'s, still bound at ColAdj: c9 far (begin, end, rate), c10 near,
			// c66/c67 the level blend weights.
			float dof_far[4] = {}, dof_near[4] = {}, dof_scale[4] = {}, dof_bias[4] = {};

			bool operator==(const grade_values& o) const { return std::memcmp(this, &o, sizeof(grade_values)) == 0; }
		};

		// Haze: the frame being drawn, the last complete frame, and what Remix has.
		haze_values g_haze_frame;
		unsigned int g_haze_frame_id = 0;
		unsigned int g_haze_weight = 0;
		haze_values g_haze_ready;
		bool g_have_haze = false;
		haze_values g_haze_sent;
		bool g_haze_sent_once = false;

		// Ambient: the probe-lit draw with the most primitives this frame, and the cube sent.
		struct ambient_source
		{
			IDirect3DBaseTexture9* probe = nullptr;
			float scale[4] = {};      // c86
			float upper[4] = {};      // c98
			float lower[4] = {};      // c99
			float shadow_dir[4] = {}; // c175, the sun's direction of travel
		};
		ambient_source g_ambient_frame;
		unsigned int g_ambient_frame_id = 0;
		unsigned int g_ambient_weight = 0;
		ambient_source g_ambient_ready;
		bool g_have_ambient = false;
		std::array<std::array<float, 3>, 6> g_ambient_sent {};
		bool g_ambient_sent_once = false;

		void close_ambient_frame()
		{
			if (g_ambient_weight)
			{
				g_ambient_ready = g_ambient_frame;
				g_have_ambient = true;
			}
			g_ambient_weight = 0;
		}

		// Grade: one ColAdj draw per frame.
		grade_values g_grade;
		bool g_have_grade = false;
		grade_values g_grade_sent;
		bool g_grade_sent_once = false;

		unsigned int g_updated_frame = ~0u;

		void close_haze_frame()
		{
			if (g_haze_weight)
			{
				g_haze_ready = g_haze_frame;
				g_have_haze = true;
			}
			g_haze_weight = 0;
		}

		bool set(const char* key, const std::string& value)
		{
			auto& api = shared::common::remix_api::get();
			return api.m_bridge.SetConfigVariable &&
				api.m_bridge.SetConfigVariable(key, value.c_str()) == REMIXAPI_ERROR_CODE_SUCCESS;
		}

		std::string vec3(const float* v) { return std::format("{}, {}, {}", v[0], v[1], v[2]); }
		std::string num(float v) { return std::format("{}", v); }

		void report(const char* what, bool ok, const std::string& detail)
		{
			static bool logged_haze = false, logged_grade = false, logged_ambient = false;
			bool& logged = what[0] == 'h' ? logged_haze : what[0] == 'g' ? logged_grade : logged_ambient;
			if (logged && ok)
				return;
			logged = true;
			shared::common::log("Atmosphere", ok ? std::format("game {} sent: {}", what, detail)
				: std::format("Remix refused the {} values (SetConfigVariable)", what));
		}

		void send_haze(const haze_values& v)
		{
			const auto* c = v.ps; // c[0] = c103 ... c[8] = c111
			bool ok = true;
			ok &= set("rtx.ds1Haze.fogColor", vec3(c[0]));
			ok &= set("rtx.ds1Haze.fogStrength", num(c[0][3]));
			ok &= set("rtx.ds1Haze.fogStart", num(v.fog_range[0]));
			ok &= set("rtx.ds1Haze.fogInvRange", num(v.fog_range[1]));
			ok &= set("rtx.ds1Haze.extinction", vec3(c[1]));
			ok &= set("rtx.ds1Haze.surfaceTint", vec3(c[2]));
			ok &= set("rtx.ds1Haze.inscatterScale", num(c[2][3]));
			ok &= set("rtx.ds1Haze.inverseExtinction", vec3(c[3]));
			ok &= set("rtx.ds1Haze.phase", vec3(c[4]));
			ok &= set("rtx.ds1Haze.rayleigh", vec3(c[5]));
			ok &= set("rtx.ds1Haze.mie", vec3(c[6]));
			ok &= set("rtx.ds1Haze.sunColor", vec3(c[7]));
			ok &= set("rtx.ds1Haze.scatterBlend", num(c[7][3]));
			ok &= set("rtx.ds1Haze.sunDirection", vec3(c[8]));
			ok &= set("rtx.ds1Haze.extinctionScale", num(c[8][3]));
			ok &= set("rtx.ds1Haze.active", "True");
			ok &= set("rtx.ds1Exposure.gameExposure", num(v.exposure[1] != 0.0f ? v.exposure[0] / v.exposure[1] : 1.0f));
			ok &= set("rtx.ds1Exposure.gameActive", "True");
			report("haze", ok, std::format("fog {} x{}, extinction {}, blend {}", vec3(c[0]), c[0][3], vec3(c[1]), c[7][3]));
		}

		// The probe's light (faces * c86) as a sky gradient lerp(down, up, n.y * 0.5 + 0.5)
		// plus a sun lobe sun * max(0, dot(n, -L)) along the shadow light, least squares
		// per channel over the six faces. The game's probe holds both; a path tracer wants
		// the sun as a light (sharp shadows) and the rest as the sky.
		struct probe_split
		{
			float up[3] = {}, down[3] = {}, sun[3] = {};
		};

		bool split_probe(const ambient_source& s, probe_split& out)
		{
			std::array<std::array<float, 3>, 6> faces;
			if (!shared::common::ffp_state::get().cube_face_averages(s.probe, faces))
				return false;
			static const float normals[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
			const float* l = s.shadow_dir;
			const float len = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
			double basis[6][3];
			for (int f = 0; f < 6; f++)
			{
				const double t = normals[f][1] * 0.5 + 0.5;
				const double lambert = len > 0.0f
					? std::max(0.0, -double(normals[f][0] * l[0] + normals[f][1] * l[1] + normals[f][2] * l[2]) / len) : 0.0;
				basis[f][0] = t;
				basis[f][1] = 1.0 - t;
				basis[f][2] = lambert;
			}
			// Normal equations A^T A x = A^T b, solved by Cramer's rule.
			double ata[3][3] = {};
			for (int f = 0; f < 6; f++)
				for (int i = 0; i < 3; i++)
					for (int j = 0; j < 3; j++)
						ata[i][j] += basis[f][i] * basis[f][j];
			auto det3 = [](const double m[3][3])
			{
				return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
					+ m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
			};
			const double det = det3(ata);
			for (int c = 0; c < 3; c++)
			{
				double atb[3] = {};
				for (int f = 0; f < 6; f++)
					for (int i = 0; i < 3; i++)
						atb[i] += basis[f][i] * faces[f][c] * s.scale[c];
				double x[3] = {};
				if (std::abs(det) > 1e-9)
				{
					for (int k = 0; k < 3; k++)
					{
						double m[3][3];
						for (int i = 0; i < 3; i++)
							for (int j = 0; j < 3; j++)
								m[i][j] = j == k ? atb[i] : ata[i][j];
						x[k] = det3(m) / det;
					}
				}
				else
				{
					// No sun direction: the sky alone, from the up and down faces.
					x[0] = faces[2][c] * s.scale[c];
					x[1] = faces[3][c] * s.scale[c];
				}
				out.up[c] = static_cast<float>(std::max(0.0, x[0]));
				out.down[c] = static_cast<float>(std::max(0.0, x[1]));
				out.sun[c] = static_cast<float>(std::max(0.0, x[2]));
			}
			return true;
		}

		// The sky bounce rays see: the probe's sky gradient plus the hemisphere ambient
		// (c98 straight up, c99 straight down, their mean sideways).
		void ambient_cube(const ambient_source& s, const probe_split& split, std::array<std::array<float, 3>, 6>& cube)
		{
			for (int f = 0; f < 6; f++)
				for (int c = 0; c < 3; c++)
				{
					const float sky = f == 2 ? split.up[c] : f == 3 ? split.down[c] : 0.5f * (split.up[c] + split.down[c]);
					const float hemi = f == 2 ? s.upper[c] : f == 3 ? s.lower[c] : 0.5f * (s.upper[c] + s.lower[c]);
					cube[f][c] = sky + hemi;
				}
		}

		// The last ambient cube, gamma space, for ambient_toward.
		std::array<std::array<float, 3>, 6> g_ambient_gamma = {};
		bool g_have_ambient_gamma = false;

		void send_ambient(const std::array<std::array<float, 3>, 6>& cube)
		{
			static const char* keys[6] = { "rtx.ds1Ambient.posX", "rtx.ds1Ambient.negX", "rtx.ds1Ambient.posY",
				"rtx.ds1Ambient.negY", "rtx.ds1Ambient.posZ", "rtx.ds1Ambient.negZ" };
			bool ok = true;
			for (int f = 0; f < 6; f++)
				ok &= set(keys[f], vec3(cube[f].data()));
			ok &= set("rtx.ds1Ambient.active", "True");
			report("ambient", ok, std::format("up {}, down {}", vec3(cube[2].data()), vec3(cube[3].data())));
		}

		void send_grade(const grade_values& v)
		{
			const float offset[3] = { v.rows[0][3], v.rows[1][3], v.rows[2][3] };
			bool ok = true;
			ok &= set("rtx.ds1grading.gameScale", num(v.scale[2]));
			ok &= set("rtx.ds1grading.gameRowR", vec3(v.rows[0]));
			ok &= set("rtx.ds1grading.gameRowG", vec3(v.rows[1]));
			ok &= set("rtx.ds1grading.gameRowB", vec3(v.rows[2]));
			ok &= set("rtx.ds1grading.gameOffset", vec3(offset));
			ok &= set("rtx.ds1grading.gameActive", "True");
			// The game's own image pipeline (rtx.ds1Look): ColAdj, bloom and bright pass values.
			auto vec4 = [](const float* r) { return std::format("{}, {}, {}, {}", r[0], r[1], r[2], r[3]); };
			ok &= set("rtx.ds1Look.sceneScale", num(v.scale[2]));
			ok &= set("rtx.ds1Look.bloomRateNear", num(v.scale[0]));
			ok &= set("rtx.ds1Look.bloomRateFar", num(v.bloom_far[1]));
			ok &= set("rtx.ds1Look.thresholdNear", num(v.bright[0]));
			ok &= set("rtx.ds1Look.thresholdFar", num(v.bloom_far[0]));
			ok &= set("rtx.ds1Look.depthBegin", num(v.bloom_far[2]));
			ok &= set("rtx.ds1Look.depthEnd", num(v.bloom_far[3]));
			ok &= set("rtx.ds1Look.gradeRowR", vec4(v.rows[0]));
			ok &= set("rtx.ds1Look.gradeRowG", vec4(v.rows[1]));
			ok &= set("rtx.ds1Look.gradeRowB", vec4(v.rows[2]));
			if (v.bloom_w && v.bloom_h)
			{
				// The bright pass runs at twice the final bloom buffer's size.
				ok &= set("rtx.ds1Look.bloomWidth", std::to_string(2 * v.bloom_w));
				ok &= set("rtx.ds1Look.bloomHeight", std::to_string(2 * v.bloom_h));
			}
			ok &= set("rtx.ds1Look.dofFarBegin", num(v.dof_far[0]));
			ok &= set("rtx.ds1Look.dofFarEnd", num(v.dof_far[1]));
			ok &= set("rtx.ds1Look.dofFarStrength", num(v.dof_far[2]));
			ok &= set("rtx.ds1Look.dofNearBegin", num(v.dof_near[0]));
			ok &= set("rtx.ds1Look.dofNearEnd", num(v.dof_near[1]));
			ok &= set("rtx.ds1Look.dofNearStrength", num(v.dof_near[2]));
			ok &= set("rtx.ds1Look.dofBlendScale", vec4(v.dof_scale));
			ok &= set("rtx.ds1Look.dofBlendBias", vec4(v.dof_bias));
			ok &= set("rtx.ds1Look.gameActive", "True");
			report("grade", ok, std::format("scale {}, diagonal {}, {}, {}", v.scale[2], v.rows[0][0], v.rows[1][1], v.rows[2][2]));
		}
	}

	void observe_haze(const float* ps_const, const float* vs_const, unsigned int frame, unsigned int prim_count)
	{
		if (!ps_const || !vs_const)
			return;
		if (frame != g_haze_frame_id)
		{
			close_haze_frame();
			g_haze_frame_id = frame;
		}
		if (prim_count < g_haze_weight)
			return;
		g_haze_weight = prim_count ? prim_count : 1;
		std::memcpy(g_haze_frame.ps, &ps_const[103 * 4], sizeof(g_haze_frame.ps));
		std::memcpy(g_haze_frame.exposure, &ps_const[135 * 4], sizeof(g_haze_frame.exposure));
		std::memcpy(g_haze_frame.fog_range, &vs_const[128 * 4], sizeof(g_haze_frame.fog_range));
	}

	void observe_ambient(const float* ps_const, IDirect3DBaseTexture9* probe, unsigned int frame, unsigned int prim_count)
	{
		if (!ps_const || !probe)
			return;
		if (frame != g_ambient_frame_id)
		{
			close_ambient_frame();
			g_ambient_frame_id = frame;
		}
		if (prim_count < g_ambient_weight)
			return;
		g_ambient_weight = prim_count ? prim_count : 1;
		g_ambient_frame.probe = probe;
		std::memcpy(g_ambient_frame.scale, &ps_const[86 * 4], sizeof(g_ambient_frame.scale));
		std::memcpy(g_ambient_frame.upper, &ps_const[98 * 4], sizeof(g_ambient_frame.upper));
		std::memcpy(g_ambient_frame.lower, &ps_const[99 * 4], sizeof(g_ambient_frame.lower));
		std::memcpy(g_ambient_frame.shadow_dir, &ps_const[175 * 4], sizeof(g_ambient_frame.shadow_dir));
	}

	void observe_grade(const float* ps_const, IDirect3DDevice9* dev)
	{
		if (!ps_const)
			return;
		std::memcpy(g_grade.scale, &ps_const[56 * 4], sizeof(g_grade.scale));
		std::memcpy(g_grade.rows, &ps_const[62 * 4], sizeof(g_grade.rows));
		// c11 is the BrightPassFilter's, still bound from that draw.
		std::memcpy(g_grade.bright, &ps_const[11 * 4], sizeof(g_grade.bright));
		std::memcpy(g_grade.bloom_far, &ps_const[71 * 4], sizeof(g_grade.bloom_far));
		std::memcpy(g_grade.dof_far, &ps_const[9 * 4], sizeof(g_grade.dof_far));
		std::memcpy(g_grade.dof_near, &ps_const[10 * 4], sizeof(g_grade.dof_near));
		std::memcpy(g_grade.dof_scale, &ps_const[66 * 4], sizeof(g_grade.dof_scale));
		std::memcpy(g_grade.dof_bias, &ps_const[67 * 4], sizeof(g_grade.dof_bias));
		IDirect3DBaseTexture9* bloom = nullptr;
		if (dev && SUCCEEDED(dev->GetTexture(1, &bloom)) && bloom)
		{
			if (bloom->GetType() == D3DRTYPE_TEXTURE)
			{
				D3DSURFACE_DESC desc {};
				if (SUCCEEDED(static_cast<IDirect3DTexture9*>(bloom)->GetLevelDesc(0, &desc)))
				{
					g_grade.bloom_w = desc.Width;
					g_grade.bloom_h = desc.Height;
				}
			}
			bloom->Release();
		}
		g_have_grade = true;
	}

	// Lookdev host only: rtx_comp\lookdev\remix_live.txt holds `rtx.key = value` lines that are
	// sent to Remix whenever the file changes, so any option can be changed on the running
	// host by a script without a restart or a menu.
	void apply_live_options(unsigned int frame)
	{
		static FILETIME last {};
		if (!shared::globals::lookdev_host || frame % 6 != 0 || !shared::common::remix_api::is_initialized())
			return;
		WIN32_FILE_ATTRIBUTE_DATA info;
		if (!GetFileAttributesExA("rtx_comp\\lookdev\\remix_live.txt", GetFileExInfoStandard, &info)
			|| CompareFileTime(&info.ftLastWriteTime, &last) == 0)
			return;
		last = info.ftLastWriteTime;
		FILE* f = nullptr;
		if (fopen_s(&f, "rtx_comp\\lookdev\\remix_live.txt", "rb") || !f)
			return;
		char line[4096];
		int applied = 0, refused = 0;
		while (fgets(line, sizeof(line), f))
		{
			std::string s(line);
			const auto eq = s.find('=');
			if (s.empty() || s[0] == '#' || eq == std::string::npos)
				continue;
			auto trim = [](const std::string& t)
			{
				const auto a = t.find_first_not_of(" \t\r\n"), b = t.find_last_not_of(" \t\r\n");
				return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
			};
			const auto key = trim(s.substr(0, eq)), value = trim(s.substr(eq + 1));
			(set(key.c_str(), value) ? applied : refused)++;
		}
		fclose(f);
		shared::common::log("Atmosphere", std::format("remix_live.txt: {} option(s) applied, {} refused", applied, refused));
	}

	void frame_update(unsigned int frame)
	{
		if (frame == g_updated_frame)
			return;
		g_updated_frame = frame;
		apply_live_options(frame);
		if (frame != g_haze_frame_id)
			close_haze_frame();
		if (frame != g_ambient_frame_id)
			close_ambient_frame();
		if (!shared::common::remix_api::is_initialized())
			return;
		if (probe_split split; g_have_ambient && split_probe(g_ambient_ready, split))
		{
			sun_light::set_probe_sun(split.sun);
			std::array<std::array<float, 3>, 6> cube;
			ambient_cube(g_ambient_ready, split, cube);
			g_ambient_gamma = cube;
			g_have_ambient_gamma = true;
			for (auto& face : cube)
				for (float& v : face)
					v = std::pow(std::max(v, 0.0f), 2.2f);
			if (!(g_ambient_sent_once && cube == g_ambient_sent))
			{
				send_ambient(cube);
				g_ambient_sent = cube;
				g_ambient_sent_once = true;
			}
		}
		if (g_have_haze && !(g_haze_sent_once && g_haze_sent == g_haze_ready))
		{
			send_haze(g_haze_ready);
			g_haze_sent = g_haze_ready;
			g_haze_sent_once = true;
		}
		if (g_have_grade && !(g_grade_sent_once && g_grade_sent == g_grade))
		{
			send_grade(g_grade);
			g_grade_sent = g_grade;
			g_grade_sent_once = true;
		}
	}

	bool ambient_toward(const float* normal, float* out_gamma)
	{
		const float len = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
		if (!g_have_ambient_gamma || len <= 0.0f)
			return false;
		for (int c = 0; c < 3; c++)
			out_gamma[c] = 0.0f;
		// The cube's weighting: each axis face by the squared normal component.
		for (int axis = 0; axis < 3; axis++)
		{
			const float d = normal[axis] / len;
			const auto& face = g_ambient_gamma[2 * axis + (d >= 0.0f ? 0 : 1)];
			for (int c = 0; c < 3; c++)
				out_gamma[c] += d * d * face[c];
		}
		return true;
	}
}
