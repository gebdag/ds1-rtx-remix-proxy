#include "std_include.hpp"
#include "atmosphere.hpp"

#include "shared/common/remix_api.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/ffp_prof.hpp"
#include "shared/common/config.hpp"
#include "shared/globals.hpp"
#include "sun_light.hpp"
#include "../modules/game_lights.hpp"

#include <cmath>

#include <cstring>
#include <format>
#include <vector>

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
			// The same region's set: c135 is global and moves every frame, so it is not part of it.
			bool same_set(const haze_values& o) const
			{
				return std::memcmp(ps, o.ps, sizeof(ps)) == 0 && std::memcmp(fog_range, o.fog_range, sizeof(fog_range)) == 0;
			}
		};

		// A frame's draws can carry several sets of a group's constants: the game applies its
		// per-region DrawParams per draw (Blighttown shows five or more fog and light zones in one
		// view), while Remix gets one set for the whole screen. That set is the one most of the
		// frame's geometry uses, each distinct set weighted by the primitives of every draw using it.
		// Another set takes over only after leading for kHoldFrames frames in a row, or at once when
		// the current one is gone from the frame (a new area), so turning the camera can't flip it.
		// Taking the single largest draw's set instead flipped Blighttown to a small zone's green
		// scattering whenever one of its draws was the largest in view.
		template <class T>
		struct dominant_set
		{
			static constexpr size_t kMaxSets = 16;
			static constexpr int kHoldFrames = 20;
			struct entry { T value; uint64_t weight; };
			std::vector<entry> frame;
			T ready {};
			bool have = false;
			T challenger {};
			int lead = 0;

			void add(const T& value, unsigned int prim_count)
			{
				const uint64_t weight = prim_count ? prim_count : 1;
				for (auto& e : frame)
				{
					if (e.value.same_set(value))
					{
						e.weight += weight;
						e.value = value;
						return;
					}
				}
				if (frame.size() < kMaxSets)
					frame.push_back({ value, weight });
			}

			// Ends the frame; true once a set has been chosen.
			bool close()
			{
				if (frame.empty())
					return have;
				const entry* best = &frame[0];
				const entry* current = nullptr;
				for (const auto& e : frame)
				{
					if (e.weight > best->weight)
						best = &e;
					if (have && e.value.same_set(ready))
						current = &e;
				}
				if (!have || !current)
				{
					ready = best->value;
					have = true;
					lead = 0;
				}
				else if (best == current)
				{
					ready = current->value; // same set, latest values (c135)
					lead = 0;
				}
				else
				{
					lead = lead > 0 && best->value.same_set(challenger) ? lead + 1 : 1;
					challenger = best->value;
					if (lead >= kHoldFrames)
					{
						ready = best->value;
						lead = 0;
					}
					else
						ready = current->value;
				}
				frame.clear();
				return have;
			}
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
		dominant_set<haze_values> g_haze_sets;
		unsigned int g_haze_frame_id = 0;
		haze_values g_haze_ready;
		bool g_have_haze = false;
		haze_values g_haze_sent;
		bool g_haze_sent_once = false;

		// Per-draw haze sets: every distinct set the frame's draws carry holds a slot (1-15, the
		// 4 bits the runtime keeps per surface) until no draw has used it for two frames. A set
		// keeps its slot while it is in use, so a draw's tag always names values Remix already
		// has, except in a set's first frames: its values go out at the next frame_update, once
		// Remix accepted them, and Remix applies options at the end of the frame that receives
		// them, so a slot is tagged only kSlotLatency frames after its values were sent; until
		// then its draws are untagged (the frame's single set).
		struct haze_slot
		{
			haze_values v;
			unsigned int sent_frame = 0; // frame its values went to Remix, 0 = not yet
			unsigned int last_frame = 0;
			bool used = false;
		};
		constexpr int kHazeSlots = 15;
		constexpr unsigned int kSlotLatency = 2;
		constexpr DWORD kHazeMarker = 0xD15DA000;
		haze_slot g_slots[kHazeSlots + 1]; // [0] unused
		bool g_slots_dirty = false;
		DWORD g_tag_set = ~0u; // the stage 7 constant as last set; ~0 = unknown (set it)

		// Self-lit tag: kSelfLitMarker | r << 16 | g << 8 | b, each byte c100 * hemisphere * 255 / 16.
		// Only a colour of at least kSelfLitMin is self-lit: ordinary ambient stays below it (the
		// brightest uniform hemisphere that is not a glow, the Kiln's 2.5 at c100 0.6, gives 1.5).
		constexpr DWORD kSelfLitMarker = 0xD5000000;
		constexpr float kSelfLitMin = 3.0f;
		constexpr float kSelfLitRange = 16.0f;
		DWORD g_self_lit_tag_set = ~0u; // the stage 3 constant as last set; ~0 = unknown (set it)

		// Ambient: the probe-lit set most of the frame uses (dominant_set), and the cube sent.
		struct ambient_source
		{
			IDirect3DBaseTexture9* probe = nullptr;
			float scale[4] = {};      // c86
			float upper[4] = {};      // c98
			float lower[4] = {};      // c99
			float shadow_dir[4] = {}; // c175, the sun's direction of travel

			bool same_set(const ambient_source& o) const { return std::memcmp(this, &o, sizeof(ambient_source)) == 0; }
		};
		dominant_set<ambient_source> g_ambient_sets;
		unsigned int g_ambient_frame_id = 0;
		ambient_source g_ambient_ready;
		bool g_have_ambient = false;
		std::array<std::array<float, 3>, 6> g_ambient_sent {};
		bool g_ambient_sent_once = false;

		void close_ambient_frame()
		{
			if (g_ambient_sets.close())
			{
				g_ambient_ready = g_ambient_sets.ready;
				g_have_ambient = true;
			}
		}

		// Grade: one ColAdj draw per frame.
		grade_values g_grade;
		bool g_have_grade = false;
		grade_values g_grade_sent;
		bool g_grade_sent_once = false;

		// Light shafts: the frame's first LightShaft pass, and what Remix has.
		struct shaft_values
		{
			float direction[3] = {}; // towards the shaft sun (world space, unit)
			float threshold = 1.0f;  // the bright pass threshold for the sky (c71.x)
			float weight = 0.0f;     // ColAdj's c56.y
		};
		shaft_values g_shaft;
		unsigned int g_shaft_frame = 0;        // the last frame with a LightShaft pass, 0 = none yet
		unsigned int g_shaft_seen_frame = ~0u; // the frame whose first pass was read
		shaft_values g_shaft_sent;
		bool g_shaft_active_sent = false;
		// Frames without a LightShaft pass before the shafts are switched off (an area without them,
		// a menu, a load).
		constexpr unsigned int kShaftHoldFrames = 30;

		// T moves a little each frame (c69 is recomputed with the camera): resend only on a change
		// that shows.
		bool shaft_changed(const shaft_values& a, const shaft_values& b)
		{
			for (int i = 0; i < 3; i++)
				if (std::abs(a.direction[i] - b.direction[i]) > 1e-3f)
					return true;
			return a.threshold != b.threshold || a.weight != b.weight;
		}

		unsigned int g_updated_frame = ~0u;

		void close_haze_frame()
		{
			if (g_haze_sets.close())
			{
				g_haze_ready = g_haze_sets.ready;
				g_have_haze = true;
			}
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
			static bool logged_haze = false, logged_grade = false, logged_ambient = false, logged_adaptation = false;
			bool& logged = what[0] == 'h' ? logged_haze : what[0] == 'g' ? logged_grade
				: what[1] == 'd' ? logged_adaptation : logged_ambient;
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

		// The sun's gamma-space ambient and lobe (set_sun_gamma), and what Remix has.
		float g_sun_gamma[2][3] = {};
		bool g_have_sun_gamma = false;
		float g_sun_gamma_sent[2][3] = {};
		bool g_sun_gamma_sent_once = false;

		// c54 as still bound at ColAdj: FRPG_Fil_CalcAdaptedLum's rate, luminance clamp and key.
		// Remix runs the game's meter and adaptation on its own frame with them (rtx.ds1Exposure.adapt*),
		// since under path tracing the game meters a buffer that is not the image shown.
		float g_c54[4] = {};
		float g_c54_sent[4] = {};
		float g_divisor_sent = 0.0f;

		void send_adaptation(const float* c54, float divisor)
		{
			bool ok = true;
			ok &= set("rtx.ds1Exposure.adaptRate", num(c54[0]));
			ok &= set("rtx.ds1Exposure.adaptMinLum", num(c54[1]));
			ok &= set("rtx.ds1Exposure.adaptMaxLum", num(c54[2]));
			ok &= set("rtx.ds1Exposure.adaptKey", num(c54[3]));
			ok &= set("rtx.ds1Exposure.exposureDivisor", num(divisor));
			ok &= set("rtx.ds1Exposure.adaptActive", "True");
			report("adaptation", ok, std::format("rate {}, clamp {}..{}, key {}, c135.y {}", c54[0], c54[1], c54[2], c54[3], divisor));
		}

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
		haze_values v;
		std::memcpy(v.ps, &ps_const[103 * 4], sizeof(v.ps));
		std::memcpy(v.exposure, &ps_const[135 * 4], sizeof(v.exposure));
		std::memcpy(v.fog_range, &vs_const[128 * 4], sizeof(v.fog_range));
		g_haze_sets.add(v, prim_count);
	}

	void tag_haze_set(IDirect3DDevice9* dev, bool has_haze, const float* ps_const, const float* vs_const, unsigned int frame)
	{
		DWORD tag = 0;
		if (has_haze && ps_const && vs_const)
		{
			haze_values v;
			std::memcpy(v.ps, &ps_const[103 * 4], sizeof(v.ps));
			std::memcpy(v.fog_range, &vs_const[128 * 4], sizeof(v.fog_range));
			int slot = 0;
			for (int i = 1; i <= kHazeSlots && !slot; i++)
				if (g_slots[i].used && g_slots[i].v.same_set(v))
					slot = i;
			if (!slot)
			{
				// A free slot, else the one idle longest among those no draw used this frame or the last.
				for (int i = 1; i <= kHazeSlots && !slot; i++)
					if (!g_slots[i].used)
						slot = i;
				if (!slot)
				{
					for (int i = 1; i <= kHazeSlots; i++)
						if (g_slots[i].last_frame + 1 < frame && (!slot || g_slots[i].last_frame < g_slots[slot].last_frame))
							slot = i;
				}
				if (slot)
				{
					g_slots[slot] = { v, 0, frame, true };
					g_slots_dirty = true;
				}
			}
			if (slot)
			{
				g_slots[slot].last_frame = frame;
				if (g_slots[slot].sent_frame && frame - g_slots[slot].sent_frame >= kSlotLatency)
					tag = kHazeMarker | static_cast<DWORD>(slot);
			}
		}
		if (tag != g_tag_set)
		{
			dev->SetTextureStageState(7, D3DTSS_CONSTANT, tag);
			g_tag_set = tag;
		}
	}

	namespace
	{
		// A probe-lit draw's self-lit colour (c100 * its uniform hemisphere ambient, gamma space), when
		// its light set is a glow rather than ambient light (atmosphere.hpp tag_self_lit).
		bool self_lit_colour(const float* ps_const, float value[3])
		{
			const float* probe = &ps_const[86 * 4];
			const float* upper = &ps_const[98 * 4];
			const float* lower = &ps_const[99 * 4];
			const float* diffuse = &ps_const[100 * 4];
			bool uniform_unprobed = true;
			float peak = 0.0f;
			for (int c = 0; c < 3; c++)
			{
				uniform_unprobed &= probe[c] == 0.0f && std::fabs(upper[c] - lower[c]) <= 0.01f * std::max(upper[c], lower[c]);
				value[c] = std::max(diffuse[c] * 0.5f * (upper[c] + lower[c]), 0.0f);
				peak = std::max(peak, value[c]);
			}
			return uniform_unprobed && peak >= kSelfLitMin;
		}
	}

	void tag_self_lit(IDirect3DDevice9* dev, bool ambient, const float* ps_const)
	{
		DWORD tag = 0;
		if (float value[3]; ambient && ps_const && self_lit_colour(ps_const, value))
		{
			tag = kSelfLitMarker;
			for (int c = 0; c < 3; c++)
				tag |= static_cast<DWORD>(std::lround(std::min(value[c] / kSelfLitRange, 1.0f) * 255.0f)) << (16 - 8 * c);
		}
		if (tag != g_self_lit_tag_set)
		{
			dev->SetTextureStageState(3, D3DTSS_CONSTANT, tag);
			g_self_lit_tag_set = tag;
		}
	}

	void observe_ambient(const float* ps_const, IDirect3DBaseTexture9* probe, unsigned int frame, unsigned int prim_count)
	{
		// A self-lit set is a glow, not the area's ambient light (it emits in Remix instead).
		if (float self_lit[3]; !ps_const || !probe || self_lit_colour(ps_const, self_lit))
			return;
		if (frame != g_ambient_frame_id)
		{
			close_ambient_frame();
			g_ambient_frame_id = frame;
		}
		ambient_source a;
		a.probe = probe;
		std::memcpy(a.scale, &ps_const[86 * 4], sizeof(a.scale));
		std::memcpy(a.upper, &ps_const[98 * 4], sizeof(a.upper));
		std::memcpy(a.lower, &ps_const[99 * 4], sizeof(a.lower));
		std::memcpy(a.shadow_dir, &ps_const[175 * 4], sizeof(a.shadow_dir));
		g_ambient_sets.add(a, prim_count);
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
		std::memcpy(g_c54, &ps_const[54 * 4], sizeof(g_c54));
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

	void observe_light_shaft(const float* ps_const, unsigned int frame)
	{
		if (!ps_const || frame == g_shaft_seen_frame)
			return;
		g_shaft_seen_frame = frame;
		const float* rows = shared::common::ffp_state::get().map_viewproj_rows();
		if (!rows)
			return;
		// Rows x, y and w of VP times (T, 0) give c69's x and y as NDC and its w: three equations.
		const float* c69 = &ps_const[69 * 4];
		const double b[3] = { 2.0 * c69[0] - 1.0, 1.0 - 2.0 * c69[1], c69[3] };
		const double m[3][3] = {
			{ rows[0], rows[1], rows[2] },
			{ rows[4], rows[5], rows[6] },
			{ rows[12], rows[13], rows[14] },
		};
		auto det3 = [](const double a[3][3])
		{
			return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
				+ a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
		};
		const double det = det3(m);
		if (std::abs(det) < 1e-9)
			return;
		double t[3];
		for (int k = 0; k < 3; k++)
		{
			double a[3][3];
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					a[i][j] = j == k ? b[i] : m[i][j];
			t[k] = det3(a) / det;
		}
		// T is a unit vector; anything else is not the camera c69 was made with.
		const double len = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
		if (len < 0.9 || len > 1.1)
			return;
		for (int i = 0; i < 3; i++)
			g_shaft.direction[i] = static_cast<float>(t[i] / len);
		g_shaft.threshold = ps_const[71 * 4];
		g_shaft_frame = std::max(frame, 1u);
	}

	// Live options: `rtx.key = value` lines sent to Remix whenever the file changes, so any
	// option can be changed by a script without a restart or a menu. The lookdev host reads
	// rtx_comp\lookdev\remix_live.txt; the game reads rtx_comp\remix_dev.txt, which only
	// exists on a development machine.
	const char* live_options_path()
	{
		return shared::globals::lookdev_host ? "rtx_comp\\lookdev\\remix_live.txt" : "rtx_comp\\remix_dev.txt";
	}

	// Set while the game's rtx_comp\remix_dev.txt exists: frame_update keeps
	// rtx_comp\look_state.txt current with every game value it hands to Remix.
	bool g_dev_file = false;

	// "proxy.FFPSteps.<Key> = value" in the live options file: the [FFPSteps] switches, which the
	// conversion reads every draw, so they take effect at once (A/B without a restart).
	bool set_proxy_option(const std::string& key, const std::string& value)
	{
		auto& steps = shared::common::config::get().ffp_steps;
		const int v = std::atoi(value.c_str());
		const bool b = v != 0 || value == "true" || value == "True";
		if (key == "FFPSteps.Profile")
		{
			steps.profile = b;
			shared::common::ffp_prof::set_profiling(b);
		}
		else if (key == "FFPSteps.TextureStages")   steps.tex_stages = std::clamp(v, 0, 2);
		else if (key == "FFPSteps.Transforms")      steps.transforms = std::clamp(v, 0, 2);
		else if (key == "FFPSteps.RestoreTextures") steps.restore_textures = std::clamp(v, 0, 2);
		else if (key == "FFPSteps.AlbedoNullStages") steps.albedo_null_stages = b;
		else if (key == "FFPSteps.ExpandReuse")     steps.expand_reuse = b;
		else if (key == "FFPSteps.DecalQuery")      steps.decal_query = b;
		else if (key == "FFPSteps.OverlaySaveLazy") steps.overlay_save_lazy = b;
		else if (key == "FFPSteps.SkinUploadUsedBones") steps.skin_upload_used_bones = b;
		else if (key == "FFPSteps.PoolDepthSurfaces") steps.pool_depth_surfaces = b;
		else if (key == "Lights.GameFalloffFX" || key == "Lights.GameFalloffCharacter")
		{
			auto* gl = game_lights::get();
			if (!gl)
				return false;
			gl->set_kind_game_falloff(key.back() == 'X' ? game_lights::light_kind::fx_direct : game_lights::light_kind::lerp_entity, b);
		}
		else return false;
		return true;
	}

	void apply_live_options(unsigned int frame)
	{
		static FILETIME last {};
		if (frame % 6 != 0 || !shared::common::remix_api::is_initialized())
			return;
		WIN32_FILE_ATTRIBUTE_DATA info;
		g_dev_file = GetFileAttributesExA(live_options_path(), GetFileExInfoStandard, &info) != 0;
		if (!g_dev_file || CompareFileTime(&info.ftLastWriteTime, &last) == 0)
			return;
		last = info.ftLastWriteTime;
		FILE* f = nullptr;
		if (fopen_s(&f, live_options_path(), "rb") || !f)
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
			if (key.rfind("proxy.", 0) == 0)
			{
				(set_proxy_option(key.substr(6), value) ? applied : refused)++;
				continue;
			}
			(set(key.c_str(), value) ? applied : refused)++;
		}
		fclose(f);
		shared::common::log("Atmosphere", std::format("{}: {} option(s) applied, {} refused", live_options_path(), applied, refused));
	}

	// rtx_comp\look_state.txt: what the game is handing to Remix right now, rewritten when it
	// changes (checked every 30 frames) while the dev options file exists.
	void write_look_state(unsigned int frame)
	{
		static std::string last;
		if (!g_dev_file || frame % 30 != 0)
			return;
		auto v4 = [](const float* r) { return std::format("{:.6g}, {:.6g}, {:.6g}, {:.6g}", r[0], r[1], r[2], r[3]); };
		std::string s;
		const auto& h = g_haze_ready;
		s += std::format("c135 exposure {}  (x/y = {:.6g})\n", v4(h.exposure), h.exposure[1] != 0.0f ? h.exposure[0] / h.exposure[1] : 0.0f);
		s += std::format("c54 at ColAdj {}\n", v4(g_c54));
		for (int i = 0; i < 9; i++)
			s += std::format("c{} {}\n", 103 + i, v4(h.ps[i]));
		s += std::format("vs c128 {}\n", v4(h.fog_range));
		const auto& g = g_grade;
		s += std::format("c56 {}\nc62 {}\nc63 {}\nc64 {}\n", v4(g.scale), v4(g.rows[0]), v4(g.rows[1]), v4(g.rows[2]));
		s += std::format("c11 {}\nc71 {}\nc9 {}\nc10 {}\nc66 {}\nc67 {}\nbloom {}x{}\n",
			v4(g.bright), v4(g.bloom_far), v4(g.dof_far), v4(g.dof_near), v4(g.dof_scale), v4(g.dof_bias), g.bloom_w, g.bloom_h);
		if (g_have_ambient_gamma)
			for (int f = 0; f < 6; f++)
				s += std::format("ambient face {} (gamma) {:.6g}, {:.6g}, {:.6g}\n", f, g_ambient_gamma[f][0], g_ambient_gamma[f][1], g_ambient_gamma[f][2]);
		if (auto* gl = game_lights::get())
		{
			const auto& sun = sun_light::current(frame, gl->sun_fallback_colour);
			s += std::format("sun vote valid {} dir_from {} colour_from {} dir {:.4g}, {:.4g}, {:.4g} colour {:.4g}, {:.4g}, {:.4g} shadow/map/chr draws {}/{}/{}\n",
				sun.valid, static_cast<int>(sun.dir_from), static_cast<int>(sun.colour_from),
				sun.key.dir[0], sun.key.dir[1], sun.key.dir[2], sun.key.colour[0], sun.key.colour[1], sun.key.colour[2],
				sun.shadow_draws, sun.map_dir3_draws, sun.chr_dir3_draws);
			float rad[3], dir[3];
			if (gl->submitted_sun(rad, dir))
				s += std::format("sun submitted radiance {:.4g}, {:.4g}, {:.4g} dir {:.4g}, {:.4g}, {:.4g} (x{} enabled {})\n",
					rad[0], rad[1], rad[2], dir[0], dir[1], dir[2], gl->sun_intensity_scale, gl->sun_enabled);
			else
				s += std::format("sun submitted none (enabled {})\n", gl->sun_enabled);
			s += std::format("lights live {} harvested {} create_failed {} zero_radiance {}\n",
				gl->active_light_count(), gl->total_light_count(), gl->create_failed_count(), gl->zero_radiance_count());
		}
		if (s == last)
			return;
		last = s;
		FILE* f = nullptr;
		if (fopen_s(&f, shared::globals::lookdev_host ? "rtx_comp\\look_state_host.txt" : "rtx_comp\\look_state.txt", "wb") || !f)
			return;
		fprintf(f, "frame %u\n%s", frame, s.c_str());
		fclose(f);
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
		// The per-draw sets by slot, when a slot was (re)assigned: "slot c103..c111 c128.x c128.y;".
		if (g_slots_dirty)
		{
			std::string s;
			for (int i = 1; i <= kHazeSlots; i++)
			{
				if (!g_slots[i].used)
					continue;
				s += std::to_string(i);
				for (const auto& reg : g_slots[i].v.ps)
					for (const float c : reg)
						s += std::format(" {}", c);
				s += std::format(" {} {};", g_slots[i].v.fog_range[0], g_slots[i].v.fog_range[1]);
			}
			if (set("rtx.ds1Haze.sets", s))
			{
				g_slots_dirty = false;
				for (int i = 1; i <= kHazeSlots; i++)
					if (g_slots[i].used && !g_slots[i].sent_frame)
						g_slots[i].sent_frame = std::max(frame, 1u); // 0 means unsent
			}
		}
		// Re-assert the draw tags once per frame (a device reset clears texture stage states).
		g_tag_set = ~0u;
		g_self_lit_tag_set = ~0u;
		// The adaptation constants and c135.y, when they change (c54.x moves with the frame time).
		if (const float divisor = g_haze_ready.exposure[1]; g_have_grade && g_have_haze && g_c54[3] > 0.0f && divisor > 0.0f &&
			(std::memcmp(g_c54, g_c54_sent, sizeof(g_c54)) != 0 || divisor != g_divisor_sent))
		{
			send_adaptation(g_c54, divisor);
			std::memcpy(g_c54_sent, g_c54, sizeof(g_c54));
			g_divisor_sent = divisor;
		}
		if (g_have_grade && !(g_grade_sent_once && g_grade_sent == g_grade))
		{
			send_grade(g_grade);
			g_grade_sent = g_grade;
			g_grade_sent_once = true;
		}
		// Light shafts, while the game runs its filter.
		if (g_shaft_frame && frame - g_shaft_frame < kShaftHoldFrames)
		{
			g_shaft.weight = g_have_grade ? g_grade.scale[1] : 1.0f;
			if (!g_shaft_active_sent || shaft_changed(g_shaft, g_shaft_sent))
			{
				const bool ok = set("rtx.ds1Look.shaftDirection", vec3(g_shaft.direction))
					& set("rtx.ds1Look.shaftThreshold", num(g_shaft.threshold))
					& set("rtx.ds1Look.shaftWeight", num(g_shaft.weight))
					& set("rtx.ds1Look.shaftActive", "True");
				if (ok)
				{
					if (!g_shaft_active_sent)
						shared::common::log("Atmosphere", std::format("game light shafts sent: towards {}, threshold {}, weight {}",
							vec3(g_shaft.direction), g_shaft.threshold, g_shaft.weight));
					g_shaft_sent = g_shaft;
					g_shaft_active_sent = true;
				}
			}
		}
		else if (g_shaft_active_sent && set("rtx.ds1Look.shaftActive", "False"))
		{
			g_shaft_active_sent = false;
		}
		if (g_have_sun_gamma && !(g_sun_gamma_sent_once && std::memcmp(g_sun_gamma, g_sun_gamma_sent, sizeof(g_sun_gamma)) == 0))
		{
			const bool ok = set("rtx.ds1Ambient.sunBase", vec3(g_sun_gamma[0])) & set("rtx.ds1Ambient.sunLobe", vec3(g_sun_gamma[1]));
			report("ambient", ok, std::format("sun base {}, lobe {}", vec3(g_sun_gamma[0]), vec3(g_sun_gamma[1])));
			if (ok)
			{
				std::memcpy(g_sun_gamma_sent, g_sun_gamma, sizeof(g_sun_gamma));
				g_sun_gamma_sent_once = true;
			}
		}
		write_look_state(frame);
	}

	void set_sun_gamma(const float* base, const float* lobe)
	{
		for (int c = 0; c < 3; c++)
		{
			g_sun_gamma[0][c] = base[c];
			g_sun_gamma[1][c] = lobe[c];
		}
		g_have_sun_gamma = true;
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
