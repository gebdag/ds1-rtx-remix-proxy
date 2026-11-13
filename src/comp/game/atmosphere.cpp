#include "std_include.hpp"
#include "atmosphere.hpp"
#include "custom_lights.hpp"
#include "eye_sprites.hpp"

#include "estus.hpp"
#include "ffx_lights.hpp"
#include "shared/common/remix_api.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/ffp_prof.hpp"
#include "shared/common/config.hpp"
#include "shared/globals.hpp"
#include "sun_light.hpp"
#include "map_lights.hpp"
#include "texture_names.hpp"
#include "mesh_tables.hpp"
#include "cheats.hpp"
#include "cutscenes.hpp"
#include "host_feed.hpp"
#include "../modules/game_lights.hpp"
#include "../modules/skinning.hpp"

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

		// Per-draw haze sets: every distinct set the frame's draws carry holds a slot (1-14 of the
		// 4 bits the runtime keeps per surface; 15 is kUnhazedSlot) until no draw has used it for two frames. A set
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
		constexpr int kHazeSlots = 14;
		constexpr DWORD kUnhazedSlot = 15;
		constexpr unsigned int kSlotLatency = 2;
		constexpr DWORD kHazeMarker = 0xD15DA000;
		haze_slot g_slots[kHazeSlots + 1]; // [0] unused
		// The last tag_self_lit put a light shaft's own haze into its colour (tag_unhazed then keeps the look's haze off it).
		bool g_shaft_own_haze = false;
		bool g_slots_dirty = false;
		DWORD g_tag_set = ~0u; // the stage 7 constant as last set; ~0 = unknown (set it)

		// Self-lit tag: kSelfLitMarker | r << 16 | g << 8 | b, each byte c100 * hemisphere * 255 / 16.
		// Only a colour of at least kSelfLitMin is self-lit: ordinary ambient stays below it (the
		// brightest uniform hemisphere that is not a glow, the Kiln's 2.5 at c100 0.6, gives 1.5).
		constexpr DWORD kSelfLitMarker = 0xD5000000;
		constexpr float kSelfLitMin = 3.0f;
		constexpr float kSelfLitRange = 16.0f;
		// The light of a self-lit draw facing straight down, when it differs: stage 2 (no lava draw fades by its view), read with stage 3's.
		constexpr DWORD kSelfLitBelowMarker = 0xD9000000;
		DWORD g_self_lit_below_tag = 0;
		constexpr float kBrightCharacterMin = 1.2f;
		int g_seath_glow = 2;            // proxy.Character.SeathGlow, live: 0 off, 1 additive, 2 cutout
		int g_white_passage = 1;         // [FFP] WhitePassage, live proxy.FFP.WhitePassage (tag_white_passage)
		// [FFP] WhitePassageLight, live: the light the box throws from inside, against the game's c100. 0.06: the stairs beside the
		// player read 75 of 255 (game 72) and the player 18 (game 19) in frame_20261111_112454; without it 72 and 11, the player a
		// silhouette with a hard shadow. At 0.1 they read 81 and 26, at 0.25 124 and 73.
		float g_white_passage_light = 0.06f;
		bool g_white_passage_loaded = false;
		// proxy.Character.GhostSelfLit, live, off: New Londo's ghosts take their look from their replacement material in the mod
		// (z_pbr ghosts.usda: Remix's own additive blend with emission from the albedo), not from a proxy tag.
		bool g_ghost_self_lit = false;
		// proxy.Character.SeathAlphaRef, live. 64 cut most of the wing away (its alpha is low; the game's c100 3 makes even that solid light);
		// 8 against the game: frame error 22.4 / 29.3 on frame_20261109_224107 / frame_20261110_001243 (36.6 / 38.4 as sent), 24 gave 23.6 / 29.8.
		DWORD g_seath_alpha_ref = 8;
		DWORD g_self_lit_tag_set = ~0u; // the stage 3 constant as last set; ~0 = unknown (set it)

		// View fade tag (tag_view_fade): 0xD6000000 | c176.x * 255 << 8 | c176.y * 16.
		constexpr DWORD kViewFadeMarker = 0xD6000000;
		DWORD g_view_fade_tag_set = ~0u; // the stage 2 constant as last set; ~0 = unknown (set it)

		// Ambient: the probe-lit set most of the frame uses (dominant_set), and the cube sent.
		struct ambient_source
		{
			// Which probe, for comparing sets only. The game frees it with its map while this set is still held, so it is never called: its
			// faces are read while it is bound to a draw.
			IDirect3DBaseTexture9* probe = nullptr;
			std::array<std::array<float, 3>, 6> faces {};
			unsigned int has_faces = 0;
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
		float g_menu_diffuse = 1.0f; // c100: the shaders' scale on the diffuse (x1 to the light in a menu's view)
		bool g_menu_grade_sent = false;
		float g_menu_ambient_scale = 0.7f; // proxy.MenuScene.AmbientScale, gamma space: the key light takes the rest

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

		// Light shafts: the frame's LightShaft passes, and what Remix has.
		struct shaft_values
		{
			float direction[3] = {}; // towards the shaft sun (world space, unit)
			float threshold = 1.0f;  // the shaft bright pass threshold (c71.x)
			float weight = 0.0f;     // ColAdj's c56.y
			// Each pass's c70: reach in texels, weight, decay per tap. The game's: (100, 0.04, 0.434), (10, 1, 0.92).
			float pass[2][3] = { { 100.0f, 0.04f, 0.434f }, { 10.0f, 1.0f, 0.92f } };
		};
		shaft_values g_shaft;
		unsigned int g_shaft_frame = 0;        // the last frame with a LightShaft pass, 0 = none yet
		unsigned int g_shaft_seen_frame = ~0u; // the frame whose passes are being read
		int g_shaft_pass = 0;                  // passes seen in that frame
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
			for (int p = 0; p < 2; p++)
				for (int i = 0; i < 3; i++)
					if (a.pass[p][i] != b.pass[p][i])
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
			if (!s.has_faces)
				return false;
			const auto& faces = s.faces;
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

		bool g_sky_in_shot_sent = false;

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
			bool ok = true;
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

	void tag_unhazed(IDirect3DDevice9* dev)
	{
		const DWORD tag = kHazeMarker | kUnhazedSlot;
		if (dev && g_shaft_own_haze && tag != g_tag_set)
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

	namespace
	{
		// A lava draw's light, gamma space (see tag_self_lit): c100 * (c86 * probe + lerp(c99, c98, N.y * 0.5 + 0.5)), per pixel in
		// the game. Both ends go to the runtime, which takes the pixel's own normal: lava facing up gets c98 and lava overhead
		// c99, (2.43, 4, 4) at c100 2.75 in Lost Izalith, which clips its ceiling veins to yellow. No single value suits both.
		bool lava_colour(const float* ps_const, IDirect3DBaseTexture9* probe, float value[3], float below[3])
		{
			std::array<std::array<float, 3>, 6> faces {};
			const bool probed = probe && shared::common::ffp_state::get().cube_face_averages(probe, faces);
			const float* scale = &ps_const[86 * 4];
			const float* upper = &ps_const[98 * 4];
			const float* lower = &ps_const[99 * 4];
			const float* diffuse = &ps_const[100 * 4];
			for (int c = 0; c < 3; c++)
			{
				value[c] = std::max(diffuse[c] * ((probed ? faces[2][c] : 0.0f) * scale[c] + upper[c]), 0.0f);
				below[c] = std::max(diffuse[c] * ((probed ? faces[3][c] : 0.0f) * scale[c] + lower[c]), 0.0f);
			}
			// Gwynevere's curtains: the game's value is only how they look. As an emitter it floods a room the game keeps dark. [Lights] CurtainGlow, gamma.
			auto& ffp = shared::common::ffp_state::get();
			if (std::string_view(texture_names::name_of(ffp.cur_texture(static_cast<UINT>(shared::common::config::get().ffp.albedo_stage)))).rfind("m15_queen_curtain_", 0) == 0)
			{
				static const float glow = shared::common::config::get().get_float("Lights", "CurtainGlow", 0.15f);
				for (int c = 0; c < 3; c++)
				{
					value[c] *= glow;
					below[c] *= glow;
				}
			}
			return std::max({ value[0], value[1], value[2], below[0], below[1], below[2] }) > 0.0f;
		}
	}

	namespace
	{
		// An additive (SrcAlpha + One) lit draw: the game adds its texture times its region's light
		// (New Londo's ghosts: c100 0.3 * hemisphere (0.16..2.0)), about 0.3 in gamma. Remix gives an
		// additive surface a fixed emission (rtx.emissiveBlendOverrideEmissiveIntensity 2), so the
		// ghosts blazed and gave themselves away from afar. Tagged with that light, averaged over
		// the directions (c100 * (c86 * the probe's mean face + the hemisphere's mean)), the runtime
		// emits texture * light instead.
		bool additive_lit_colour(IDirect3DDevice9* dev, const float* ps_const, IDirect3DBaseTexture9* probe, float value[3], bool bright_character)
		{
			DWORD blend = FALSE, dst = 0;
			dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
			dev->GetRenderState(D3DRS_DESTBLEND, &dst);
			if (!bright_character && (!blend || dst != D3DBLEND_ONE))
				return false;
			std::array<std::array<float, 3>, 6> faces {};
			const bool probed = probe && shared::common::ffp_state::get().cube_face_averages(probe, faces);
			const float* scale = &ps_const[86 * 4];
			const float* upper = &ps_const[98 * 4];
			const float* lower = &ps_const[99 * 4];
			const float* diffuse = &ps_const[100 * 4];
			for (int c = 0; c < 3; c++)
			{
				float probe_mean = 0.0f;
				if (probed)
				{
					for (const auto& f : faces)
						probe_mean += f[c];
					probe_mean /= 6.0f;
				}
				value[c] = std::max(diffuse[c] * (probe_mean * scale[c] + 0.5f * (upper[c] + lower[c])), 0.0f);
			}
			return true;
		}
	}

	namespace
	{
		// FRPG_Phn_*_HemDir3*'s light on a surface tilted 45 degrees from up, one value for its waves: three directional
		// lights (c92-c94, colours c95-c97), a hemisphere (lerp(c99, c98, N.y / 2 + 1 / 2)). Its point lights are small.
		float lit_effect_light(const float* ps_const, int channel)
		{
			constexpr float kUp = 0.7071f;
			float light = 0.0f;
			for (int i = 0; i < 3; i++)
				light += std::max(-ps_const[(92 + i) * 4 + 1] * kUp, 0.0f) * ps_const[(95 + i) * 4 + channel];
			const float t = kUp * 0.5f + 0.5f;
			return light + ps_const[99 * 4 + channel] + (ps_const[98 * 4 + channel] - ps_const[99 * 4 + channel]) * t;
		}
	}

	void tag_white_passage(IDirect3DDevice9* dev, const float* ps_const)
	{
		if (!g_white_passage_loaded)
		{
			g_white_passage_loaded = true;
			const auto& cfg = shared::common::config::get();
			g_white_passage = std::clamp(cfg.get_int("FFP", "WhitePassage", g_white_passage), 0, 3);
			g_white_passage_light = std::clamp(cfg.get_float("FFP", "WhitePassageLight", g_white_passage_light), 0.0f, 2.0f);
		}
		if (!dev || !ps_const || !g_white_passage)
			return;
		// The far view: where the game's own tint (c105) would darken it and still leave it white, c100 times the tint at 1 or
		// more. From inside the passage the tint is 10.
		const float tint = std::max({ ps_const[105 * 4], ps_const[105 * 4 + 1], ps_const[105 * 4 + 2] });
		const float level = std::max({ ps_const[100 * 4], ps_const[100 * 4 + 1], ps_const[100 * 4 + 2] });
		const bool far_view = tint < 1.0f && level * tint >= 1.0f;
		if (g_white_passage != 1 && !far_view)
			return;
		if (g_white_passage == 1 || g_white_passage == 3)
		{
			// Its own light, c100 times its texture, as a blended unlit layer's: the white box is where the walkway's light comes from.
			// From inside, the look's haze makes the box white already (tint 10) and all the tag adds is the light it throws on the
			// stairs and the player: that part is scaled by WhitePassageLight. In the far view it needs its full c100 to stay white.
			DWORD tag = kSelfLitMarker;
			for (int c = 0; c < 3; c++)
				tag |= static_cast<DWORD>(std::clamp(std::lround(std::max(ps_const[100 * 4 + c] * (far_view ? 1.0f : g_white_passage_light), 0.0f) / kSelfLitRange * 255.0f), 1L, 255L)) << (16 - 8 * c);
			if (tag != g_self_lit_tag_set)
			{
				dev->SetTextureStageState(3, D3DTSS_CONSTANT, tag);
				g_self_lit_tag_set = tag;
			}
		}
		else if (const DWORD tag = kHazeMarker | kUnhazedSlot; tag != g_tag_set)
		{
			// Left alone by the look's haze.
			dev->SetTextureStageState(7, D3DTSS_CONSTANT, tag);
			g_tag_set = tag;
		}
	}

	void tag_veil(IDirect3DDevice9* dev, const float* ps_const, const float* vs_const, IDirect3DBaseTexture9* probe, float distance)
	{
		if (!ps_const)
			return;
		std::array<std::array<float, 3>, 6> faces {};
		const bool probed = probe && shared::common::ffp_state::get().cube_face_averages(probe, faces);
		DWORD tag = kSelfLitMarker;
		for (int c = 0; c < 3; c++)
		{
			float mean = 0.0f;
			for (const auto& f : faces)
				mean += f[c] / 6.0f;
			const float ambient = (probed ? ps_const[86 * 4 + c] * mean : 0.0f) + 0.5f * (ps_const[98 * 4 + c] + ps_const[99 * 4 + c]);
			// Its own haze at its distance, as the light shaft's (tag_self_lit): what the fog leaves, then the extinction times
			// c105 for c110.w of it. Without a distance, up close: no fog and no extinction.
			float fog = 0.0f, ext = 1.0f;
			if (vs_const && distance > 0.0f)
			{
				fog = std::clamp(std::clamp((distance - vs_const[128 * 4]) * vs_const[128 * 4 + 1], 0.0f, 1.0f) * ps_const[103 * 4 + 3], 0.0f, 1.0f);
				ext = std::exp2(-ps_const[104 * 4 + c] * distance * ps_const[111 * 4 + 3] * 2.08136892f);
			}
			const float share = std::clamp(ps_const[110 * 4 + 3], 0.0f, 1.0f);
			float hazed = (1.0f - fog) * (1.0f - share + share * ext * ps_const[105 * 4 + c]);
			if (!std::isfinite(hazed))
				hazed = 1.0f;
			const float value = ps_const[100 * 4 + c] * ambient * hazed;
			// At least one step, so the tag is there whatever the colour.
			tag |= static_cast<DWORD>(std::clamp(std::lround(std::max(value, 0.0f) / kSelfLitRange * 255.0f), 1L, 255L)) << (16 - 8 * c);
		}
		if (dev && tag != g_self_lit_tag_set)
		{
			dev->SetTextureStageState(3, D3DTSS_CONSTANT, tag);
			g_self_lit_tag_set = tag;
		}
	}

	int seath_glow_mode()
	{
		return g_seath_glow;
	}

	DWORD seath_alpha_ref()
	{
		return g_seath_alpha_ref;
	}

	void tag_self_lit(IDirect3DDevice9* dev, bool ambient, const float* ps_const, IDirect3DBaseTexture9* probe, bool lava, bool unlit, bool lit_effect,
		bool light_shaft, const float* shaft_gain, const float* vs_const, float shaft_distance, bool glow_mesh)
	{
		DWORD tag = 0;
		g_self_lit_below_tag = 0;
		g_shaft_own_haze = false;
		// Only blended layers (Bed of Chaos's glyph shells, the large bonfire's flame meshes). An opaque unlit mesh
		// (m18_ao, c100 7) is an ordinary surface Remix lights, and the tag would add its c100 glow on top of that
		// light (the Kiln's white passage gets it in its far view all the same: tag_white_passage). The tag makes the resolve sum a layer in gamma as the game's target does: the large bonfire's five
		// flame layers, each (1, 0.39, 0.2) * texture, clip red and climb through orange to yellow there, while a
		// linear sum stays red (frame_20261101_135237).
		DWORD blended = FALSE, dst_blend = 0, src_blend = 0;
		if ((unlit || lit_effect || light_shaft) && dev)
		{
			dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blended);
			dev->GetRenderState(D3DRS_DESTBLEND, &dst_blend);
			dev->GetRenderState(D3DRS_SRCBLEND, &src_blend);
		}
		// The Skull Lantern's eyes and mouth: their own 348-triangle piece, unbumped, drawn at c100 2 from the cream patches of its texture. The game's only "glow" is
		// that doubling under its lights, so in the dark it shows nothing. A little light of its own at rest, more held up: the game's head light (c116) is 2.4 or 4
		// then and 1.26 otherwise. [Lights] LanternIdle, LanternHeld, gamma.
		if (ambient && ps_const && glow_mesh)
		{
			static const float idle = shared::common::config::get().get_float("Lights", "LanternIdle", 0.6f);
			static const float held = shared::common::config::get().get_float("Lights", "LanternHeld", 1.6f);
			// Only the Tomb's head light is 1.26 unless the lantern is up: elsewhere the game's own colour says nothing about it.
			const bool raised = map_lights::current_map_name() == "m13_01_00_00" && std::max({ ps_const[116 * 4], ps_const[116 * 4 + 1], ps_const[116 * 4 + 2] }) >= 1.8f;
			const float value = raised ? held : idle;
			static constexpr float kWarm[3] = { 1.0f, 0.85f, 0.6f };
			tag = kSelfLitMarker;
			for (int c = 0; c < 3; c++)
				tag |= static_cast<DWORD>(std::clamp(std::lround(value * kWarm[c] / kSelfLitRange * 255.0f), 1L, 255L)) << (16 - 8 * c);
		}
		// The shaft's light is the HemEnv shader's ambient (c86, c98, c99, probe): only a lit one, blended as the unlit layer
		// marker requires (ffp_state::apply_unlit_layer_marker), which the renderer then sets.
		else if (light_shaft && ambient && !unlit && blended && src_blend == D3DBLEND_SRCALPHA && dst_blend == D3DBLEND_INVSRCALPHA && ps_const)
		{
			// A light shaft: the game lights it as a map surface, texture * c100 * (c86 * probe(N) + lerp(c99, c98, N.y / 2 + 1 / 2)).
			// It hangs in the air and is seen from every side: the probe's mean over its faces and the hemisphere's middle.
			std::array<std::array<float, 3>, 6> faces {};
			const bool probed = probe && shared::common::ffp_state::get().cube_face_averages(probe, faces);
			tag = kSelfLitMarker;
			for (int c = 0; c < 3; c++)
			{
				float mean = 0.0f;
				for (const auto& f : faces)
					mean += f[c] / 6.0f;
				// Its own haze, which the game's shader puts on it with its own set (c103-c111, VS c128) at its own depth, and
				// the look's haze leaves alone (tag_unhazed): the gain at its centre's distance. Without it Bed of Chaos's shaft
				// came out 1.27 / 1.42 / 1.55 times the game's (frame_20261106_153141).
				float gain = 1.0f;
				if (vs_const && shaft_distance > 0.0f)
				{
					const float fog = std::clamp(std::clamp((shaft_distance - vs_const[128 * 4]) * vs_const[128 * 4 + 1], 0.0f, 1.0f) *
						ps_const[103 * 4 + 3], 0.0f, 1.0f);
					const float ext = std::exp2(-ps_const[104 * 4 + c] * shaft_distance * ps_const[111 * 4 + 3] * 2.08136892f);
					const float blend = ps_const[110 * 4 + 3];
					gain = (1.0f - fog) * (1.0f + (ext * ps_const[105 * 4 + c] - 1.0f) * blend);
					if (!std::isfinite(gain))
						gain = 1.0f;
					g_shaft_own_haze = true;
				}
				const float value = gain * (shaft_gain ? shaft_gain[c] : 1.0f) * ps_const[100 * 4 + c] * ((probed ? mean : 0.0f) * ps_const[86 * 4 + c] +
					0.5f * (ps_const[98 * 4 + c] + ps_const[99 * 4 + c]));
				tag |= static_cast<DWORD>(std::clamp(std::lround(std::max(value, 0.0f) / kSelfLitRange * 255.0f), 1L, 255L)) << (16 - 8 * c);
			}
		}
		else if ((unlit || lit_effect) && blended && ps_const)
		{
			// The game's light for it is c100 alone, or c100 * the lit effect mesh's light. At least 1 per channel, as for
			// additive draws below.
			const float* diffuse = &ps_const[100 * 4];
			tag = kSelfLitMarker;
			for (int c = 0; c < 3; c++)
				tag |= static_cast<DWORD>(std::clamp(std::lround(std::max(diffuse[c] * (lit_effect ? lit_effect_light(ps_const, c) : 1.0f), 0.0f) / kSelfLitRange * 255.0f), 1L, 255L)) << (16 - 8 * c);
		}
		else if (float value[3], below[3]; ambient && ps_const && (lava ? lava_colour(ps_const, probe, value, below) : self_lit_colour(ps_const, value)))
		{
			tag = kSelfLitMarker;
			for (int c = 0; c < 3; c++)
				tag |= static_cast<DWORD>(std::lround(std::min(value[c] / kSelfLitRange, 1.0f) * 255.0f)) << (16 - 8 * c);
			if (lava)
			{
				g_self_lit_below_tag = kSelfLitBelowMarker;
				for (int c = 0; c < 3; c++)
					g_self_lit_below_tag |= static_cast<DWORD>(std::lround(std::min(below[c] / kSelfLitRange, 1.0f) * 255.0f)) << (16 - 8 * c);
			}
		}
		// Or a character the game lights past its texture (c100 above 1: Seath 1.5-3, Bed of Chaos's body 1.7;
		// every other character is 1 or below, frames across 158 captures): the game's ambient on it, as its
		// own light. Remix's albedo stops at 1, and such a boss went dim in its cave.
		else if (const bool bright_character = shared::common::ffp_state::get().cur_decl_is_skinned() && ps_const &&
				std::max({ ps_const[100 * 4], ps_const[100 * 4 + 1], ps_const[100 * 4 + 2] }) >= kBrightCharacterMin;
			ambient && ps_const && (bright_character || g_ghost_self_lit) && additive_lit_colour(dev, ps_const, probe, value, bright_character))
		{
			// At least 1 per channel: the runtime tells a tagged additive surface by a non-zero value.
			tag = kSelfLitMarker;
			for (int c = 0; c < 3; c++)
				tag |= static_cast<DWORD>(std::clamp(std::lround(value[c] / kSelfLitRange * 255.0f), 1L, 255L)) << (16 - 8 * c);
		}
		if (tag != g_self_lit_tag_set)
		{
			dev->SetTextureStageState(3, D3DTSS_CONSTANT, tag);
			g_self_lit_tag_set = tag;
		}
	}

	void tag_view_fade(IDirect3DDevice9* dev, bool fade, const float* ps_const)
	{
		DWORD tag = 0;
		if (fade && ps_const)
		{
			const float* c176 = &ps_const[176 * 4];
			const long offset = std::clamp(std::lround(c176[0] * 255.0f), 0L, 255L);
			const long scale = std::clamp(std::lround(c176[1] * 16.0f), 0L, 255L);
			tag = kViewFadeMarker | static_cast<DWORD>(offset) << 8 | static_cast<DWORD>(scale);
		}
		// tag_self_lit, just before this for every draw, sets it or clears it.
		if (g_self_lit_below_tag)
			tag = g_self_lit_below_tag;
		if (tag != g_view_fade_tag_set)
		{
			dev->SetTextureStageState(2, D3DTSS_CONSTANT, tag);
			g_view_fade_tag_set = tag;
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
		a.has_faces = shared::common::ffp_state::get().cube_face_averages(probe, a.faces) ? 1 : 0;
		std::memcpy(a.scale, &ps_const[86 * 4], sizeof(a.scale));
		std::memcpy(a.upper, &ps_const[98 * 4], sizeof(a.upper));
		std::memcpy(a.lower, &ps_const[99 * 4], sizeof(a.lower));
		std::memcpy(a.shadow_dir, &ps_const[175 * 4], sizeof(a.shadow_dir));
		g_menu_diffuse = ps_const[100 * 4];
		// An area with several light sets of similar size (Ash Lake: the Hydra's end of the lake is far brighter) would swing the whole sky with
		// the view. Named in an area override, one set is the area's and the others are not counted.
		if (const auto* gl = game_lights::get(); gl && gl->ambient_prefer_scale > 0.0f && std::abs(a.scale[0] - gl->ambient_prefer_scale) > 0.01f)
			return;
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
		if (!ps_const)
			return;
		if (frame != g_shaft_seen_frame)
		{
			g_shaft_seen_frame = frame;
			g_shaft_pass = 0;
		}
		const int pass = g_shaft_pass++;
		if (pass < 2)
			for (int i = 0; i < 3; i++)
				g_shaft.pass[pass][i] = ps_const[70 * 4 + i];
		if (pass != 0)
			return;
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
		else if (key == "FFPSteps.ImguiSkipEmpty") steps.imgui_skip_empty = b;
// Light diagnostics: Lights.Sun, Lights.Kind<n> (game_lights::light_kind), Lights.EmitterRadius, Lights.Dump
		// (appends the drawn light list to rtx_comp\light_rows.txt).
		else if (key == "Estus.ForceEmpty") estus::set_force_empty(b);
		else if (key == "Sfx.OwnWhite") shared::common::config::get().sfx.own_white = b;
		else if (key == "Sfx.OwnFog") shared::common::config::get().sfx.own_fog = b;
		else if (key == "Sfx.SitSmoke") shared::common::config::get().sfx.sit_smoke = b;
		else if (key == "Character.GhostSelfLit") g_ghost_self_lit = b;
		else if (key == "FFP.WhitePassage") { g_white_passage_loaded = true; g_white_passage = std::clamp(std::atoi(value.c_str()), 0, 3); }
		else if (key == "FFP.WhitePassageLight") { g_white_passage_loaded = true; g_white_passage_light = std::clamp(static_cast<float>(std::atof(value.c_str())), 0.0f, 2.0f); }
		else if (key == "Character.SeathGlow") g_seath_glow = std::clamp(std::atoi(value.c_str()), 0, 2);
		else if (key == "Character.SeathAlphaRef") g_seath_alpha_ref = static_cast<DWORD>(std::clamp(std::atoi(value.c_str()), 0, 255));
		else if (ffx_lights::set_follow_option(key, static_cast<float>(std::atof(value.c_str())))) {}
		// HDR10 output: the final pass copy and the runtime's rtx.hdr.enable follow at once; the scene target's format
		// is set when the game creates it (a restart).
		else if (key == "HDR.Enabled") shared::common::config::get().hdr.enabled = b;
		else if (key == "MenuScene.PathTrace") shared::common::config::get().ffp.menu_scene_path_trace = b;
		else if (key == "Meshes.GroundCards") shared::common::config::get().meshes.ground_cards = b;
		else if (key == "Meshes.CutMeshes") shared::common::config::get().meshes.cut_meshes = std::clamp(v, 0, 3);
		else if (key == "Cheats.TeleportSpot") { static std::string last; if (value != last) { last = value; cheats::teleport_to_spot(value); } }
		else if (key == "Cheats.TeleportXYZ")
		{
			static std::string last;
			float x = 0.0f, y = 0.0f, z = 0.0f;
			if (value != last && sscanf_s(value.c_str(), "%f , %f , %f", &x, &y, &z) == 3)
			{
				last = value;
				cheats::teleport_to_xyz(x, y, z);
			}
		}
		else if (key == "Meshes.CutoutLists")shared::common::config::get().meshes.cutout_lists = b;
		else if (key == "Meshes.CutMeshFilter") shared::common::config::get().meshes.cut_mesh_filter = value == "-" ? std::string() : value;
		else if (key == "Meshes.CutoutCensus") { static std::string last; if (value != last) { last = value; mesh_tables::request_cutout_census(); } }
		else if (key == "Lights.EffectDrawDump") { static std::string last; if (value != last) { last = value; ffx_lights::dump_drawn(); } }
		else if (key == "Fur.ShellGeometry") comp::skinning::s_shell_fur = b;
		else if (key == "Meshes.Veils") shared::common::config::get().meshes.veils = value != "0";
		else if (key == "Meshes.VeilTuck") shared::common::config::get().meshes.veil_tuck = std::clamp(static_cast<float>(std::atof(value.c_str())), 0.0f, 1.0f);
		else if (key == "Meshes.SkirtBacking") shared::common::config::get().meshes.skirt_backing = value != "0";
		else if (key == "Meshes.GapSkirtAlphaRef") shared::common::config::get().meshes.gap_skirt_alpha_ref = std::clamp(std::atoi(value.c_str()), 0, 255);
		else if (key == "Meshes.GroundCardHeightKept") shared::common::config::get().meshes.ground_card_height_kept = std::clamp(static_cast<float>(std::atof(value.c_str())), 0.0f, 1.0f);
		else if (key == "Meshes.GroundCardClearance") shared::common::config::get().meshes.ground_card_clearance = std::clamp(static_cast<float>(std::atof(value.c_str())), 0.0f, 0.05f);
		else if (key == "MenuScene.AmbientScale") g_menu_ambient_scale = static_cast<float>(std::atof(value.c_str()));
		else if (key == "MenuScene.RowDistance") shared::common::ffp_state::get().set_menu_row_distance(static_cast<float>(std::atof(value.c_str())));
		else if (key == "MenuScene.Key")
		{
			if (auto* gl = game_lights::get())
				sscanf_s(value.c_str(), "%f,%f,%f,%f,%f,%f,%f", &gl->menu_key[0], &gl->menu_key[1], &gl->menu_key[2],
					&gl->menu_key[3], &gl->menu_key[4], &gl->menu_key[5], &gl->menu_key[6]);
		}
		else if (key == "Lights.Sun") { if (auto* gl = game_lights::get()) gl->sun_enabled = b; }
		else if (key == "Eyes.Gain") eye_sprites::set_gain(static_cast<float>(std::atof(value.c_str())));
		else if (key == "Lights.PlayerIntensity") { if (auto* gl = game_lights::get()) gl->set_player_intensity(static_cast<float>(std::atof(value.c_str()))); }
		else if (key == "Lights.PlayerGammaAdd") { if (auto* gl = game_lights::get()) gl->set_player_gamma_add(b); }
		else if (key.rfind("Lights.Kind", 0) == 0) { if (auto* gl = game_lights::get()) gl->set_kind_enabled(std::atoi(key.c_str() + 11), b); }
		else if (key == "Lights.EmitterRadius") { if (auto* gl = game_lights::get()) gl->set_emitter_radius(static_cast<float>(std::atof(value.c_str()))); }
		else if (key == "Lights.Dump") { if (auto* gl = game_lights::get()) gl->dump_rows(); }
		else if (key == "Lights.FlameLift") map_lights::set_flame_lift(b);
		// Reads the current map's custom lights file again (rtx_comp\lights\<map>.custom.txt), for placing lights from outside the F4 menu.
		// Only on a new value: the live file is replayed whole on every write, and a reload throws unsaved menu edits away.
		else if (key == "Lights.ReloadCustom") { static std::string last; if (value != last) { last = value; game::custom_lights::reload(); } }
		else if (key == "Lights.SelectSun") { if (auto* gl = game_lights::get()) gl->select_sun(); }
		// Menu.Tab=<name>: opens the F4 menu on that tab (lookdev screenshots of the UI); "Lights+" also
		// opens every section of the Lights tab. Menu.Tab=0 closes the menu.
		else if (key == "Menu.Tab")
		{
			shared::globals::imgui_menu_open = value != "0";
			shared::globals::imgui_open_light_sections = !value.empty() && value.back() == '+';
			shared::globals::imgui_select_tab = shared::globals::imgui_open_light_sections ? value.substr(0, value.size() - 1) : value;
		}
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
		// Dev mode only (the lookdev host forces it on): a player's game never looks for the file, and so never writes
		// look_state.txt either.
		static const bool dev = shared::common::config::get().dev.enabled;
		if (!dev || frame % 6 != 0 || !shared::common::remix_api::is_initialized())
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

	bool sky_in_shot_scene()
	{
		// The Undead Asylum intro (remo scene 180101). In its shots of the knight looking down the shaft into the cell the game's
		// ambient on bounce rays burns him out against the sky behind him (frame_20261108_123227, frame_20261109_080703,
		// frame_20261109_080657); the rays take the sky texture instead, chosen over dimming the ambient, which left him very dark.
		// Held for the whole scene, not per shot: a per-shot switch can only follow a cut a frame late, which flickers, and the
		// scene's other shots don't suffer from the sky. Play and every other scene keep the ambient.
		constexpr int kSkyInShotScenes[] = { 180101 };
		if (shared::globals::lookdev_host)
			return host_feed::frame().sky_in_shot_scene;
		return std::ranges::any_of(kSkyInShotScenes, [](int id) { return cutscenes::is_playing(id); });
	}

	void frame_update(unsigned int frame)
	{
		if (frame == g_updated_frame)
			return;
		g_updated_frame = frame;
		apply_live_options(frame);
		// HDR10 output ([HDR] Enabled): the runtime's half of the toggle follows the proxy's, sent once and on change.
		if (frame % 30 == 0 && shared::common::remix_api::is_initialized())
		{
			static int sent = -1;
			const int want = shared::common::config::get().hdr.enabled ? 1 : 0;
			if (want != sent && set("rtx.hdr.enable", want ? "True" : "False"))
				sent = want;
		}
		if (frame != g_haze_frame_id)
			close_haze_frame();
		if (frame != g_ambient_frame_id)
			close_ambient_frame();
		if (!shared::common::remix_api::is_initialized())
			return;
		// Sent once when the scene starts and once when it ends, not at its cuts.
		if (const bool sky = sky_in_shot_scene(); sky != g_sky_in_shot_sent && set("rtx.ds1Ambient.skyInShot", sky ? "True" : "False"))
		{
			g_sky_in_shot_sent = sky;
			shared::common::log("Atmosphere", sky ? "Intro scene: bounce rays take the sky (skyInShot on)" : "skyInShot off");
		}
		if (probe_split split; g_have_ambient && split_probe(g_ambient_ready, split))
		{
			sun_light::set_probe_sun(split.sun);
			std::array<std::array<float, 3>, 6> cube;
			ambient_cube(g_ambient_ready, split, cube);
			// [Ambient] ProbeFaces: the probe's faces as the game's shaders read them, each side its own (cube * c86 + the
			// hemisphere), with nothing split off for a sun. The Abyss: its character probe is brightest from -X and above and
			// half that from +X, and the fit to an up-down gradient gave every side their mean (frame_20261112_020437). The runtime
			// takes them as sky radiance, so after its cosine weighting the contrast between sides is about half the probe's.
			// The sun split above still runs: harmless where the map's sun is none (m16), counted twice where it is not.
			if (const auto* gl = game_lights::get(); gl && gl->ambient_probe_faces && g_ambient_ready.has_faces)
				for (int f = 0; f < 6; f++)
					for (int c = 0; c < 3; c++)
					{
						const auto& s = g_ambient_ready;
						const float hemi = f == 2 ? s.upper[c] : f == 3 ? s.lower[c] : 0.5f * (s.upper[c] + s.lower[c]);
						cube[f][c] = s.scale[c] * s.faces[f][c] + hemi;
					}
			if (const auto* gl = game_lights::get(); gl && gl->ambient_lock_set)
				for (auto& face : cube)
					for (int c = 0; c < 3; c++)
						face[c] = gl->ambient_lock[c];
			// A menu's view has no sun to split off (its shaders read no c175): the game's light is the probe's own faces.
			if (shared::common::ffp_state::get().menu_is_path_traced() && g_ambient_ready.has_faces)
				for (int f = 0; f < 6; f++)
					for (int c = 0; c < 3; c++)
					{
						const auto& s = g_ambient_ready;
						const float hemi = f == 2 ? s.upper[c] : f == 3 ? s.lower[c] : 0.5f * (s.upper[c] + s.lower[c]);
						cube[f][c] = g_menu_ambient_scale * g_menu_diffuse * (s.scale[c] * s.faces[f][c] + hemi);
					}
			g_ambient_gamma = cube;
			g_have_ambient_gamma = true;
			for (auto& face : cube)
				for (float& v : face)
					v = std::pow(std::max(v, 0.0f), 2.2f);
			// The game's ambient is per region and jumps at a border, where its baked light hides that. Remix's sky must not: it eases over about
			// a second, sent a few times a second while it moves. Menus keep theirs exact.
			static std::array<std::array<float, 3>, 6> shown = {};
			static auto last_time = std::chrono::steady_clock::now();
			static auto last_send = last_time;
			const auto now = std::chrono::steady_clock::now();
			const float dt = std::clamp(std::chrono::duration<float>(now - last_time).count(), 0.0f, 0.5f);
			last_time = now;
			// Nor does a new map's sky after a load (the map id is 0 through one): the first moments, since its first frames can still hold the old map's values.
			static auto last_load = last_time;
			if (map_lights::current_map_id() == 0)
				last_load = now;
			const bool just_loaded = std::chrono::duration<float>(now - last_load).count() < 2.0f;
			if (!g_ambient_sent_once || shared::common::ffp_state::get().menu_is_path_traced() || just_loaded || shared::globals::lookdev_host)
				shown = cube;
			else
				for (int f = 0; f < 6; f++)
					for (int c = 0; c < 3; c++)
					{
						shown[f][c] += (cube[f][c] - shown[f][c]) * (1.0f - std::exp(-dt / 1.0f));
						// Arrived: the sun's strength reads this, so it must stop changing too.
						if (std::abs(cube[f][c] - shown[f][c]) <= 0.005f * std::max(cube[f][c], 1e-3f))
							shown[f][c] = cube[f][c];
					}
			float moved = 0.0f;
			if (g_ambient_sent_once)
				for (int f = 0; f < 6; f++)
					for (int c = 0; c < 3; c++)
						moved = std::max(moved, std::abs(shown[f][c] - g_ambient_sent[f][c]) / std::max(g_ambient_sent[f][c], 1e-3f));
			// The sun's strength is measured against this sky (ambient_toward): it must not step while the sky eases.
			for (int f = 0; f < 6; f++)
				for (int c = 0; c < 3; c++)
					g_ambient_gamma[f][c] = std::pow(std::max(shown[f][c], 0.0f), 1.0f / 2.2f);
			if (!g_ambient_sent_once || (just_loaded && moved > 0.0f) || (moved > 0.01f && std::chrono::duration<float>(now - last_send).count() > 0.12f))
			{
				send_ambient(shown);
				g_ambient_sent = shown;
				g_ambient_sent_once = true;
				last_send = now;
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
		g_view_fade_tag_set = ~0u;
		// The adaptation constants and c135.y, when they change (c54.x moves with the frame time).
		if (const float divisor = g_haze_ready.exposure[1]; g_have_grade && g_have_haze && g_c54[3] > 0.0f && divisor > 0.0f &&
			(std::memcmp(g_c54, g_c54_sent, sizeof(g_c54)) != 0 || divisor != g_divisor_sent))
		{
			send_adaptation(g_c54, divisor);
			std::memcpy(g_c54_sent, g_c54, sizeof(g_c54));
			g_divisor_sent = divisor;
		}
		// A menu's view is what its shaders output, c135 included: none of the map's post chain runs after it.
		if (const bool menu = shared::common::ffp_state::get().menu_is_path_traced(); menu != g_menu_grade_sent)
		{
			g_menu_grade_sent = menu;
			if (menu)
			{
				grade_values neutral;
				neutral.scale[2] = 1.0f;
				neutral.rows[0][0] = neutral.rows[1][1] = neutral.rows[2][2] = 1.0f;
				neutral.bright[0] = neutral.bloom_far[0] = neutral.bloom_far[3] = 1.0f;
				neutral.dof_far[0] = 10.0f;
				neutral.dof_far[1] = 100.0f;
				neutral.dof_near[1] = 1.0f;
				const float dof_scale[4] = { -6.0f, -3.0f, -2.0f, 0.0f }, dof_bias[4] = { 1.0f, 1.5f, 2.0f, 1.0f };
				std::memcpy(neutral.dof_scale, dof_scale, sizeof(dof_scale));
				std::memcpy(neutral.dof_bias, dof_bias, sizeof(dof_bias));
				send_grade(neutral);
				set("rtx.ds1Exposure.adaptActive", "False");
			}
			else
			{
				g_grade_sent_once = false;
				std::memset(g_c54_sent, 0, sizeof(g_c54_sent));
			}
		}
		if (!g_menu_grade_sent && g_have_grade && !(g_grade_sent_once && g_grade_sent == g_grade))
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
					& set("rtx.ds1Look.shaftPass1", vec3(g_shaft.pass[0]))
					& set("rtx.ds1Look.shaftPass2", vec3(g_shaft.pass[1]))
					& set("rtx.ds1Look.shaftActive", "True");
				if (ok)
				{
					if (!g_shaft_active_sent)
						shared::common::log("Atmosphere", std::format("game light shafts sent: towards {}, threshold {}, weight {}, passes {} / {}",
							vec3(g_shaft.direction), g_shaft.threshold, g_shaft.weight, vec3(g_shaft.pass[0]), vec3(g_shaft.pass[1])));
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

	bool probe_lobe(IDirect3DBaseTexture9* probe, const float* scale, const float* direction, float* up, float* down, float* lobe)
	{
		ambient_source s;
		s.probe = probe;
		s.has_faces = shared::common::ffp_state::get().cube_face_averages(probe, s.faces) ? 1 : 0;
		for (int c = 0; c < 3; c++)
		{
			s.scale[c] = scale[c];
			s.shadow_dir[c] = direction[c];
		}
		probe_split split;
		if (!split_probe(s, split))
			return false;
		for (int c = 0; c < 3; c++)
		{
			up[c] = split.up[c];
			down[c] = split.down[c];
			lobe[c] = split.sun[c];
		}
		return true;
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

	bool ambient_is_cold()
	{
		return g_have_ambient && g_ambient_ready.scale[2] > 1.3f * g_ambient_ready.scale[0];
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
