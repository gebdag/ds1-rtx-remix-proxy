#include "std_include.hpp"
#include "ffx_lights.hpp"
#include "ffx_instances.hpp"
#include "fx_draw_owner.hpp"
#include "point_lights.hpp"
#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"
#include "camera.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace comp::game::ffx_lights
{
	namespace
	{
		// Slots of these lights (game_lights keys a Remix light by its slot): the engine's slots are
		// below 0x10000, map_lights' stand-ins from 0x10000, effect_lights' from 0x20000.
		constexpr int kFirstSlot = 0x30000;
		// Lights past this distance from the camera (beyond their own reach) are left out, and at most
		// kMaxLights of the nearest are drawn: a map can hold dozens of messages and bloodstains.
		constexpr float kReach = 60.0f;
		constexpr size_t kMaxLights = 64;
		// A map light this close to a persistent effect stands for it (a wall torch's MSB light).
		constexpr float kCoverRadius = 3.0f;
		// An effect that has stopped emitting goes dark for good after this long: its instance can stay behind until the
		// area unloads. Longer than its fade, so a fire whose emitters pause comes back.
		constexpr float kEndedForGood = 1.15f;
		// A fade that does not end at zero goes the rest of the way in this.
		constexpr float kFadeRest = 0.1f;
		constexpr float kFadeOff = 0.01f;
		// Seconds past its visual life after which what keeps a brief effect lit is logged, once per effect.
		constexpr float kLingerLog = 5.0f;
		std::unordered_set<int> g_lingering, g_ended;

		std::unordered_map<int, entry> g_table;
		bool g_loaded = false;

		// What each emitter instance drew: this frame so far, and the last complete frame (swapped in collect).
		struct drawn_sum
		{
			double r = 0.0, g = 0.0, b = 0.0, area = 0.0;
			int sprites = 0;
			float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
			// By each sprite's light (its brightest channel x its area): weight, weighted place and weighted square of the place.
			double w = 0.0, wx[3] = {}, wxx[3] = {};
		};
		std::unordered_map<unsigned, drawn_sum> g_drawn_now, g_drawn_last;
		bool g_drawn_logged = false;
		UINT g_drawn_frame = ~0u;

		// The size rule (ffx_lights.hpp sized_reach). An instance's flame spread, smoothed: particles come and go every frame.
		bool g_size_enabled = true;
		float g_size_reach = 2.68f;   // metres of reach per metre of flame spread: the wall torch's 3.78 / 1.41
		float g_size_min = 0.4f;
		float g_size_max = 12.0f;
		bool g_size_game = true;      // the game's own fire effect lights are extended too
		bool g_size_lift = true;      // fire lights stand at their flame's height (sized_lift)
		float g_flicker = 0.15f;          // share of a lengthened game light's own flicker that is kept (steady_flicker)
		float g_flicker_seconds = 1.0f;   // the time its running mean is taken over
		constexpr int kBonfireLightEffect = 90006;   // "Bonfire light (light-only effect)": at every bonfire
		constexpr float kBonfireLightScatter = 0.6f; // the game's node is 0.30 m from the origin: twice that finds its instance
		bool g_bonfire_centre = true;
		float g_bonfire_height = 0.22f;
		struct flicker_state
		{
			float mean = 0.0f;
			float age = 0.0f;   // seconds the mean has been taken over
			int effect = 0;     // the effect the light belongs to: a slot is reused by other lights
			std::chrono::steady_clock::time_point seen {};
		};
		std::unordered_map<int, flicker_state> g_flicker_state;
		std::unordered_map<int, float> g_flicker_effect_mean; // per effect: the mean its light last settled at
		bool g_size_learn = false;    // the game adds what it measures to the shipped sizes (and writes it down); ini only
		bool g_size_shrink = false;   // a table light may also become shorter than its table reach
		constexpr float kSizeMinLift = 0.5f;
		constexpr int kSizeMinSprites = 4;
		constexpr float kSizeSmoothing = 0.08f; // per frame
		constexpr UINT kSizeForget = 600;
		// An instance's measure counts for its effect once it has been measured in this many frames (its first frames can be a burst).
		constexpr int kSizeSettled = 8;
		constexpr float kSizeMaxLift = 2.5f;
		struct flame_size
		{
			float spread = 0.0f;
			UINT measured = ~0u; // the frame it was last updated in
			UINT seen = 0;
			int frames = 0;      // frames it was measured in
			float lift = 0.0f;   // the flame centre's height over the instance's origin, smoothed
			int ffx_id = 0;      // the effect the measure is of: an address is reused by other effects
			float age = 0.0f;    // the instance's age when last measured: a younger one at the address is a new instance
		};
		std::unordered_map<unsigned, flame_size> g_sizes;
		// The spread known per effect: the largest settled measure, from the shipped table, earlier sessions and this one.
		std::unordered_map<int, float> g_effect_spread;
		std::unordered_map<int, float> g_effect_lift;   // with the spread: the lift of the instance that gave it
		bool g_effect_spread_dirty = false;
		UINT g_effect_spread_saved = 0;
		constexpr UINT kSizeSaveFrames = 3600; // at most this often, and only when something grew

		void load_sizes(const char* file)
		{
			std::ifstream in(shared::globals::root_path + "\\rtx_comp\\" + file);
			std::string line;
			while (std::getline(in, line))
			{
				if (line.empty() || line[0] == '#')
					continue;
				std::istringstream row(line);
				int id = 0;
				float spread = 0.0f, lift = 0.0f;
				if (row >> id >> spread && id > 0 && spread > 0.0f && spread < 100.0f && spread > g_effect_spread[id])
				{
					g_effect_spread[id] = spread;
					g_effect_lift[id] = row >> lift && std::isfinite(lift) ? std::clamp(lift, -10.0f, 10.0f) : 0.0f;
				}
			}
		}

		void save_sizes()
		{
			std::ofstream out(shared::globals::root_path + "\\rtx_comp\\ffx_sizes_learned.tsv");
			if (!out)
				return;
			out << "# FFX id <TAB> flame spread in metres <TAB> the flame centre's height over the effect's origin, metres: the largest settled measure of what the effect's instances drew. Written by the game (ffx_lights).\n";
			std::vector<std::pair<int, float>> rows(g_effect_spread.begin(), g_effect_spread.end());
			std::sort(rows.begin(), rows.end());
			for (const auto& [id, spread] : rows)
				out << id << '\t' << std::format("{:.3f}\t{:.3f}", spread, g_effect_lift[id]) << '\n';
		}
		std::unordered_set<int> g_size_logged;
		std::unordered_map<int, float> g_lifts; // by light slot: how far sized_lift raised it

		// The frame that just ended becomes the one to read. From whatever touches the sums first in a frame: the sprite
		// observer, collect, or a size query; so the sums never span more than one frame, whether or not collect runs.
		void roll_drawn()
		{
			const UINT frame = shared::common::ffp_state::get().frame_count();
			if (frame == g_drawn_frame)
				return;
			g_drawn_frame = frame;
			g_drawn_last.swap(g_drawn_now);
			g_drawn_now.clear();
		}

		bool warm(float r, float g, float b)
		{
			return r > 0.0f && r >= g && g >= b && r >= 1.6f * b;
		}

		// A live instance's slot, kept while the same instance runs the same effect.
		struct tracked
		{
			int ffx_id;
			int slot;
			UINT seen;
		};
		std::unordered_map<unsigned long long, tracked> g_slots; // by instance address, or kSpotKey | a spot's id
		std::vector<point_lights::light> g_out;

		// How bright the effect's sprite is, this far into its life: 0 past the end.
		float fade_at(const entry& e, float seconds)
		{
			if (e.fade_life <= 0.0f)
				return seconds <= 0.0f ? 1.0f : 0.0f;
			if (seconds >= e.fade_life)
				return e.fade.back() * std::max(1.0f - (seconds - e.fade_life) / kFadeRest, 0.0f);
			const float at = std::max(seconds, 0.0f) / e.fade_life * static_cast<float>(e.fade.size() - 1);
			const size_t i = std::min(static_cast<size_t>(at), e.fade.size() - 2);
			return e.fade[i] + (e.fade[i + 1] - e.fade[i]) * (at - static_cast<float>(i));
		}

		// An instance's brightness: 1 while it shows, fading after.
		struct fade
		{
			int ffx_id;
			float level;
			UINT seen;
			float first_seen;   // seconds since g_origin: an instance's own age can stand at 0 (seen with state 0xFFFFFFFE)
			float ended = -1.0f; // when it stopped emitting, on the clock of first_seen
			float own_age = 0.0f; // the instance's age field when last read
		};
		std::unordered_map<unsigned, fade> g_fades;

		// Effects whose light follows the sprites they draw at their spot, as bright as those are opaque: Tomb of the Giants' lantern halos
		// (FFX 540, the Skull Lantern's resonance: EMEVD 11315150 creates and deletes it per lantern). It streams halo particles (s02000,
		// alpha 0.39, 0.67 s) and core particles (s01037, alpha 0.19, 1.33 s); when the game deletes the effect its last particles still fade
		// out. So the light belongs to the lantern's spot, not to the instance: it starts dark, rises and falls with what is drawn there
		// (full at the halo's own alpha, so the core alone gives about half), and outlives the instance until the particles are gone.
		// Their placed stand-ins did the same (map_lights, category lantern, against the highest alpha seen).
		struct follow_rule { int ffx_id; };
		constexpr follow_rule kFollow[] = { { 540 } };
		constexpr float kFollowRadius = 1.5f;
		// Live: proxy.Lights.FollowRiseSeconds / FollowFallSeconds / FollowFullAlpha (set_follow_option).
		float g_follow_rise_seconds = 0.05f;
		float g_follow_fall_seconds = 0.2f;
		float g_follow_full_alpha = 0.39f; // FFX 540's halo records: rgba(0.39, 0.82, 1.00, 0.39); the captures' strongest halo is 99/255
		// An instance this close to a spot is that lantern again (a new instance after the game deleted the last one).
		constexpr float kFollowSameSpot = 0.75f;
		constexpr unsigned long long kSpotKey = 1ull << 32;
		// Only a spot the game would draw its effect at, and shows nothing, has lost it: inside the view less this margin, within this range.
		constexpr float kFollowViewRange = 30.0f;
		constexpr float kFollowViewMargin = 0.1f;
		struct follower
		{
			unsigned id = 0;
			int ffx_id = 0;
			int top_id = 0;
			const entry* e = nullptr;
			float pos[3] = {};
			float alpha_frame = 0.0f; // strongest sprite alpha at the spot since the last collect
			float level = 0.0f;
			UINT instance_seen = 0;   // last frame an instance of the effect stood here
		};
		std::vector<follower> g_followers;
		unsigned g_next_follower = 1;
		std::chrono::steady_clock::time_point g_follow_time;

		const follow_rule* follow_rule_for(int ffx_id)
		{
			for (const auto& r : kFollow)
				if (r.ffx_id == ffx_id)
					return &r;
			return nullptr;
		}

		bool follow_spot_in_view(const float* p)
		{
			float camera[3] = {};
			const float* view_proj = shared::common::ffp_state::get().map_viewproj_rows();
			if (!view_proj || !camera::get_override_pos(camera[0], camera[1], camera[2]))
				return false;
			const float d[3] = { p[0] - camera[0], p[1] - camera[1], p[2] - camera[2] };
			if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > kFollowViewRange * kFollowViewRange)
				return false;
			auto row = [&](int r) { return view_proj[r * 4] * d[0] + view_proj[r * 4 + 1] * d[1] + view_proj[r * 4 + 2] * d[2] + view_proj[r * 4 + 3]; };
			const float w = row(3);
			if (w <= 0.0f)
				return false;
			const float limit = w * (1.0f - kFollowViewMargin);
			return std::abs(row(0)) <= limit && std::abs(row(1)) <= limit;
		}
		const std::chrono::steady_clock::time_point g_origin = std::chrono::steady_clock::now();

		void load()
		{
			g_loaded = true;
			std::ifstream file(shared::globals::root_path + "\\rtx_comp\\ffx_lights.tsv");
			std::string line;
			int refused = 0;
			while (std::getline(file, line))
			{
				if (line.empty() || line[0] == '#')
					continue;
				if (line.back() == '\r')
					line.pop_back();
				std::istringstream in(line);
				int id = 0, planar = 0, persistent = 0, impact = 0;
				std::string fade;
				entry e {};
				std::string confidence;
				in >> id >> e.r >> e.g >> e.b >> e.dwindle_begin >> e.dwindle_end >> planar >> persistent >> confidence >> e.visual_life >> impact >> e.fade_life >> fade;
				if (!in || id <= 0)
				{
					refused++;
					continue;
				}
				in >> std::ws;
				std::getline(in, e.category);
				e.planar = planar != 0;
				e.persistent = persistent != 0;
				e.impact = impact != 0;
				std::istringstream samples(fade);
				float peak = -1.0f;
				size_t peak_at = 0;
				for (size_t i = 0; i < e.fade.size(); i++)
				{
					char comma = 0;
					samples >> e.fade[i];
					samples >> comma;
					if (e.fade[i] > peak)
					{
						peak = e.fade[i];
						peak_at = i;
					}
				}
				e.fade_peak = e.fade_life * static_cast<float>(peak_at) / static_cast<float>(e.fade.size() - 1);
				e.confidence = confidence.empty() ? 'l' : confidence[0];
				g_table[id] = std::move(e);
			}
			shared::common::log("FfxLights", std::format("{} effect lights from rtx_comp\\ffx_lights.tsv, {} rows refused", g_table.size(), refused));
			const auto& cfg = shared::common::config::get();
			g_size_enabled = cfg.get_int("Lights", "EffectSize", 1) != 0;
			g_size_reach = std::clamp(cfg.get_float("Lights", "EffectSizeReach", 2.68f), 0.1f, 20.0f);
			g_size_min = std::clamp(cfg.get_float("Lights", "EffectSizeMin", 0.4f), 0.0f, 20.0f);
			g_size_max = std::clamp(cfg.get_float("Lights", "EffectSizeMax", 12.0f), 0.5f, 100.0f);
			g_size_game = cfg.get_int("Lights", "EffectSizeGame", 1) != 0;
			g_size_lift = cfg.get_int("Lights", "EffectSizeLift", 1) != 0;
			g_size_learn = cfg.get_int("Lights", "EffectSizeLearn", 0) != 0;
			if (const float v = cfg.get_float("Lights", "EffectFlicker", g_flicker); std::isfinite(v))
				g_flicker = std::clamp(v, 0.0f, 1.0f);
			if (const float v = cfg.get_float("Lights", "EffectFlickerSeconds", g_flicker_seconds); std::isfinite(v))
				g_flicker_seconds = std::clamp(v, 0.05f, 10.0f);
			g_size_shrink = cfg.get_int("Lights", "EffectSizeShrink", 0) != 0;
			g_bonfire_centre = cfg.get_int("Lights", "BonfireLightCentre", 1) != 0;
			if (const float v = cfg.get_float("Lights", "BonfireLightHeight", g_bonfire_height); std::isfinite(v))
				g_bonfire_height = std::clamp(v, -0.5f, 1.5f);
			load_sizes("ffx_sizes.tsv");
			// What the game learned itself, only while it is asked to learn (and never in the lookdev host: a capture must look
			// the same whatever was played before).
			if (g_size_learn && !shared::globals::lookdev_host)
				load_sizes("ffx_sizes_learned.tsv");
			shared::common::log("FfxLights", std::format("{} effects with a known flame size", g_effect_spread.size()));
		}

		int free_slot()
		{
			for (int slot = kFirstSlot;; ++slot)
				if (std::none_of(g_slots.begin(), g_slots.end(), [&](const auto& t) { return t.second.slot == slot; }))
					return slot;
		}
	}

	const entry* find(int ffx_id)
	{
		if (!g_loaded)
			load();
		const auto it = g_table.find(ffx_id);
		return it != g_table.end() ? &it->second : nullptr;
	}

	int table_size()
	{
		if (!g_loaded)
			load();
		return static_cast<int>(g_table.size());
	}

	void observe_sprites(const shared::common::ffp_state::sprite_point* points, UINT count)
	{
		// The effect instance whose particle draw issued these sprites (fx_draw_owner): what it draws adds up per frame.
		if (const unsigned owner = fx_draw_owner::current(); owner && points)
		{
			roll_drawn();
			auto& d = g_drawn_now[owner];
			for (UINT v = 0; v < count; ++v)
			{
				const auto& p = points[v];
				if (!p.additive || !std::isfinite(p.area) || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
					continue;
				d.r += static_cast<double>(p.r) * p.area;
				d.g += static_cast<double>(p.g) * p.area;
				d.b += static_cast<double>(p.b) * p.area;
				d.area += p.area;
				d.sprites++;
				const float at[3] = { p.x, p.y, p.z };
				const double weight = static_cast<double>(std::max({ p.r, p.g, p.b })) * p.area;
				d.w += weight;
				for (int k = 0; k < 3; k++)
				{
					d.lo[k] = std::min(d.lo[k], at[k]);
					d.hi[k] = std::max(d.hi[k], at[k]);
					d.wx[k] += weight * at[k];
					d.wxx[k] += weight * at[k] * at[k];
				}
			}
		}
		if (g_followers.empty() || !points)
			return;
		for (UINT v = 0; v < count; ++v)
		{
			const auto& p = points[v];
			for (auto& fl : g_followers)
			{
				const float dx = p.x - fl.pos[0], dy = p.y - fl.pos[1], dz = p.z - fl.pos[2];
				if (dx * dx + dy * dy + dz * dz <= kFollowRadius * kFollowRadius)
					fl.alpha_frame = std::max(fl.alpha_frame, p.alpha);
			}
		}
	}

	drawn_light drawn_by(unsigned instance)
	{
		drawn_sum total;
		for (const auto& [owner, d] : g_drawn_last)
		{
			// The instance itself or one above the emitter.
			unsigned at = owner;
			bool under = false;
			for (int depth = 0; at && depth < 32; ++depth)
			{
				if (at == instance)
				{
					under = true;
					break;
				}
				at = ffx_instances::parent_of(at);
			}
			if (!under || !d.sprites)
				continue;
			total.r += d.r; total.g += d.g; total.b += d.b; total.area += d.area;
			total.sprites += d.sprites;
			total.w += d.w;
			for (int k = 0; k < 3; k++)
			{
				total.lo[k] = std::min(total.lo[k], d.lo[k]);
				total.hi[k] = std::max(total.hi[k], d.hi[k]);
				total.wx[k] += d.wx[k];
				total.wxx[k] += d.wxx[k];
			}
		}
		drawn_light out;
		out.r = static_cast<float>(total.r); out.g = static_cast<float>(total.g); out.b = static_cast<float>(total.b);
		out.area = static_cast<float>(total.area);
		out.sprites = total.sprites;
		if (total.w > 0.0)
		{
			double variance = 0.0;
			for (int k = 0; k < 3; k++)
			{
				const double mean = total.wx[k] / total.w;
				out.centre[k] = static_cast<float>(mean);
				variance += std::max(total.wxx[k] / total.w - mean * mean, 0.0);
			}
			out.spread = 2.0f * static_cast<float>(std::sqrt(variance));
		}
		if (total.sprites)
			out.extent = std::sqrt((total.hi[0] - total.lo[0]) * (total.hi[0] - total.lo[0]) + (total.hi[1] - total.lo[1]) * (total.hi[1] - total.lo[1]) +
				(total.hi[2] - total.lo[2]) * (total.hi[2] - total.lo[2]));
		return out;
	}

	void dump_drawn()
	{
		if (!g_loaded)
			load();
		const auto path = shared::globals::root_path + "\\rtx_comp\\ffx_drawn.tsv";
		std::ofstream out(path);
		if (!out)
			return;
		out << "kind\tffx id\ttop id\tx\tup\tdepth\tage\tdrawn r\tdrawn g\tdrawn b\tarea m2\tsprites\textent m\tspread m\tlift m\tlight r\tlight g\tlight b\treach m\tpersistent\n";
		// Owners that are not live instances (a cluster kind whose first word is something else) are counted, not listed.
		std::unordered_set<unsigned> live;
		for (const auto& inst : ffx_instances::current())
			live.insert(inst.address);
		int stray = 0, stray_sprites = 0;
		for (const auto& [owner, d] : g_drawn_last)
			if (!live.contains(owner))
			{
				stray++;
				stray_sprites += d.sprites;
			}
		int rows = 0;
		for (const auto& inst : ffx_instances::current())
		{
			const auto it = g_table.find(inst.ffx_id);
			const bool top = ffx_instances::parent_of(inst.address) == 0;
			if (!top && it == g_table.end())
				continue;
			const drawn_light d = drawn_by(inst.address);
			if (!d.sprites)
				continue;
			float pos[3] = {};
			ffx_instances::world_position(inst.address, pos);
			out << std::format("{}\t{}\t{}\t{:.2f}\t{:.2f}\t{:.2f}\t{:.2f}\t{:.4f}\t{:.4f}\t{:.4f}\t{:.4f}\t{}\t{:.2f}\t{:.2f}\t{:.2f}",
				it != g_table.end() ? "table" : "top", inst.ffx_id, ffx_instances::top_effect_id(inst.address), pos[0], pos[1], pos[2],
				ffx_instances::age(inst.address), d.r, d.g, d.b, d.area, d.sprites, d.extent, d.spread, d.centre[1] - pos[1]);
			if (it != g_table.end())
				out << std::format("\t{:.3f}\t{:.3f}\t{:.3f}\t{:.2f}\t{}", it->second.r, it->second.g, it->second.b, it->second.dwindle_end, it->second.persistent ? 1 : 0);
			out << "\n";
			rows++;
		}
		// The game's own effect lights (a point-light node in the effect): what the developers gave an effect that draws this much.
		int game_lights = 0;
		for (const auto& l : point_lights::harvest())
		{
			if (!l.ffx_id && !l.ffx_top_id)
				continue;
			out << std::format("game light\t{}\t{}\t{:.2f}\t{:.2f}\t{:.2f}\t\t\t\t\t\t\t\t\t\t{:.3f}\t{:.3f}\t{:.3f}\t{:.2f}\t\n",
				l.ffx_id, l.ffx_top_id, l.x, l.y, l.z, l.r, l.g, l.b, l.dwindle_end);
			game_lights++;
		}
		shared::common::log("FfxLights", std::format("ffx_drawn.tsv: {} effects that drew sprites, {} game effect lights; {} emitters with sprites, {} of them not live instances ({} sprites)",
			rows, game_lights, g_drawn_last.size(), stray, stray_sprites));
	}

	namespace
	{
		// The smoothed spread of what an instance of `effect` (and the effects under it) draws; 0 before it has settled.
		float spread_of(unsigned instance, int effect)
		{
			roll_drawn();
			const UINT frame = shared::common::ffp_state::get().frame_count();
			auto& s = g_sizes[instance];
			// Another effect at the address, or the same one started again: what was measured there is not this instance's.
			const float age = ffx_instances::age(instance);
			if (s.ffx_id != effect || (std::isfinite(age) && age + 0.1f < s.age))
				s = flame_size {};
			s.ffx_id = effect;
			if (std::isfinite(age))
				s.age = age;
			s.seen = frame;
			if (s.measured != frame)
			{
				s.measured = frame;
				const drawn_light d = drawn_by(instance);
				// With nothing drawn this frame (out of view, between bursts) the last size stays.
				if (d.sprites >= kSizeMinSprites && d.spread > 0.0f)
				{
					float origin[3] = {};
					const float lift = ffx_instances::world_position(instance, origin) ? d.centre[1] - origin[1] : 0.0f;
					s.lift = s.spread > 0.0f ? s.lift + (lift - s.lift) * kSizeSmoothing : lift;
					s.spread = s.spread > 0.0f ? s.spread + (d.spread - s.spread) * kSizeSmoothing : d.spread;
					s.frames++;
				}
			}
			return s.frames >= kSizeSettled ? s.spread : 0.0f;
		}
	}

	float sized_reach(unsigned instance, int effect, float r, float g, float b)
	{
		if (!g_size_enabled || !effect || !warm(r, g, b))
			return 0.0f;
		const auto it = g_effect_spread.find(effect);
		float known = it != g_effect_spread.end() ? it->second : 0.0f;
		// With learning on, what this instance draws adds to what is known of its effect: the fullest view of the flame counts.
		if (const float measured = g_size_learn && instance ? spread_of(instance, effect) : 0.0f; measured > known * 1.02f)
		{
			known = measured;
			g_effect_spread[effect] = measured;
			g_effect_lift[effect] = g_sizes[instance].lift;
			g_effect_spread_dirty = true;
		}
		return known > 0.0f ? std::clamp(known * g_size_reach, std::min(g_size_min, g_size_max), g_size_max) : 0.0f;
	}

	float sized_lift(int effect, float r, float g, float b)
	{
		if (!g_size_enabled || !g_size_lift || !effect || !warm(r, g, b))
			return 0.0f;
		// Up only, half a metre or more, and not past a tall fire's middle: a measure taken through a bad frame must not send a light off.
		const auto it = g_effect_lift.find(effect);
		return it != g_effect_lift.end() && it->second >= kSizeMinLift ? std::min(it->second, kSizeMaxLift) : 0.0f;
	}

	void note_lift(int slot, float lift)
	{
		if (lift != 0.0f)
			g_lifts[slot] = lift;
		else if (!g_lifts.empty())
			g_lifts.erase(slot);
	}

	float lift_of(int slot)
	{
		const auto it = g_lifts.find(slot);
		return it != g_lifts.end() ? it->second : 0.0f;
	}

	float sized_reach_at(const point_lights::light& l, float* lift)
	{
		if (lift)
			*lift = 0.0f;
		const int effect = l.ffx_top_id ? l.ffx_top_id : l.ffx_id;
		if (!g_size_enabled || !g_size_game || !effect || !l.has_params || !warm(l.r, l.g, l.b))
			return 0.0f;
		// The top-level instance of the light's effect that stands where the light is.
		unsigned best = 0;
		float best_d2 = 1.5f * 1.5f, best_y = 0.0f;
		for (const auto& inst : ffx_instances::current())
		{
			if (inst.ffx_id != effect || ffx_instances::parent_of(inst.address) != 0)
				continue;
			float pos[3];
			if (!ffx_instances::world_position(inst.address, pos))
				continue;
			const float dx = pos[0] - l.x, dy = pos[1] - l.y, dz = pos[2] - l.z;
			if (const float d2 = dx * dx + dy * dy + dz * dz; d2 <= best_d2)
			{
				best_d2 = d2;
				best = inst.address;
				best_y = pos[1];
			}
		}
		const float reach = sized_reach(best, effect, l.r, l.g, l.b);
		// Up to the flame's centre from where the light node is: the centre stands sized_lift over the instance's origin.
		if (lift && best && reach > 0.0f)
			if (const float centre = sized_lift(effect, l.r, l.g, l.b); centre > 0.0f)
			{
				const float missing = best_y + centre - l.y;
				*lift = missing >= kSizeMinLift ? std::min(missing, kSizeMaxLift) : 0.0f;
			}
		if (reach > l.dwindle_end && g_size_logged.insert(-effect).second)
			shared::common::log("FfxLights", std::format("game light of effect {}: its flame spreads {:.2f} m, reach {:.1f} m (the game's {:.1f} m)",
				effect, g_effect_spread[effect], reach, l.dwindle_end));
		return reach;
	}

	float level_of(unsigned address)
	{
		const auto it = g_fades.find(address);
		return it != g_fades.end() ? it->second.level : -1.0f;
	}

	float effect_flicker()
	{
		if (!g_loaded)
			load();
		return g_flicker;
	}

	bool bonfire_light_centred()
	{
		if (!g_loaded)
			load();
		return g_bonfire_centre;
	}

	float bonfire_light_height()
	{
		if (!g_loaded)
			load();
		return g_bonfire_height;
	}

	bool bonfire_light_place(const point_lights::light& l, float out[3])
	{
		if (!g_loaded)
			load();
		if (!g_bonfire_centre || (l.ffx_top_id ? l.ffx_top_id : l.ffx_id) != kBonfireLightEffect)
			return false;
		// The instance of the light's effect that it scatters round: the nearest top-level one.
		float best_d2 = kBonfireLightScatter * kBonfireLightScatter;
		bool found = false;
		for (const auto& inst : ffx_instances::current())
		{
			if (inst.ffx_id != kBonfireLightEffect || ffx_instances::parent_of(inst.address) != 0)
				continue;
			float pos[3];
			if (!ffx_instances::world_position(inst.address, pos))
				continue;
			const float dx = pos[0] - l.x, dy = pos[1] - l.y, dz = pos[2] - l.z;
			if (const float d2 = dx * dx + dy * dy + dz * dz; d2 <= best_d2)
			{
				best_d2 = d2;
				out[0] = pos[0]; out[1] = pos[1] + g_bonfire_height; out[2] = pos[2];
				found = true;
			}
		}
		return found;
	}

	void steady_flicker(int slot, int effect, float& r, float& g, float& b)
	{
		if (!g_loaded)
			load();
		const float now_level = std::max({ r, g, b });
		if (g_flicker >= 1.0f || !(now_level > 0.0f) || !std::isfinite(now_level))
			return;
		const auto now = std::chrono::steady_clock::now();
		auto& s = g_flicker_state[slot];
		const float dt = std::max(std::chrono::duration<float>(now - s.seen).count(), 0.0f);
		// A light not seen for a second, or of another effect, is another light in the slot or the same one after a cut: its mean
		// starts over, from what this effect's light last settled at when that is known.
		if (!(s.mean > 0.0f) || dt > 1.0f || s.effect != effect)
		{
			const auto known = g_flicker_effect_mean.find(effect);
			s.mean = known != g_flicker_effect_mean.end() ? known->second : now_level;
			s.age = 0.0f;
			s.effect = effect;
		}
		else
		{
			// A plain average while it is younger than the window, the running one after.
			s.age += dt;
			const float share = std::max(1.0f - std::exp(-dt / g_flicker_seconds), s.age > 0.0f ? std::min(dt / s.age, 1.0f) : 1.0f);
			s.mean += (now_level - s.mean) * share;
			if (s.age >= g_flicker_seconds)
				g_flicker_effect_mean[effect] = s.mean;
		}
		s.seen = now;
		// Far under its mean the light is going out: the game's own value, blended in between a fifth and a twentieth of the mean.
		const float ratio = now_level / s.mean;
		const float fading = std::clamp((0.2f - ratio) / 0.15f, 0.0f, 1.0f);
		const float kept = s.mean * std::pow(ratio, g_flicker + (1.0f - g_flicker) * fading);
		const float scale = kept / now_level;
		r *= scale; g *= scale; b *= scale;
		if (g_flicker_state.size() > 256)
			std::erase_if(g_flicker_state, [&](const auto& t) { return std::chrono::duration<float>(now - t.second.seen).count() > 5.0f; });
	}

	bool size_rule_enabled()
	{
		// The ini is read with the table: before that the switches are not the user's yet, and a later load would undo a change.
		if (!g_loaded)
			load();
		return g_size_enabled;
	}

	bool size_lift_enabled()
	{
		if (!g_loaded)
			load();
		return g_size_lift;
	}

	bool set_follow_option(const std::string& key, float value)
	{
		if (!g_loaded)
			load();
		if (key == "Lights.EffectSize") { g_size_enabled = value != 0.0f; g_size_logged.clear(); }
		else if (key == "Lights.EffectSizeReach") { g_size_reach = std::clamp(value, 0.1f, 20.0f); g_size_logged.clear(); }
		else if (key == "Lights.EffectSizeMin") g_size_min = std::clamp(value, 0.0f, 20.0f);
		else if (key == "Lights.EffectSizeMax") g_size_max = std::clamp(value, 0.5f, 100.0f);
		else if (key == "Lights.EffectSizeGame") { g_size_game = value != 0.0f; g_size_logged.clear(); }
		else if (key == "Lights.EffectSizeLift") g_size_lift = value != 0.0f;
		else if (key == "Lights.EffectSizeShrink") { g_size_shrink = value != 0.0f; g_size_logged.clear(); }
		else if (key == "Lights.EffectFlicker") { if (std::isfinite(value)) g_flicker = std::clamp(value, 0.0f, 1.0f); }
		else if (key == "Lights.EffectFlickerSeconds") { if (std::isfinite(value)) g_flicker_seconds = std::clamp(value, 0.05f, 10.0f); }
		else if (key == "Lights.BonfireLightCentre") g_bonfire_centre = value != 0.0f;
		else if (key == "Lights.BonfireLightHeight") { if (std::isfinite(value)) g_bonfire_height = std::clamp(value, -0.5f, 1.5f); }
		else if (key == "Lights.FollowRiseSeconds") g_follow_rise_seconds = std::max(value, 0.001f);
		else if (key == "Lights.FollowFallSeconds") g_follow_fall_seconds = std::max(value, 0.001f);
		else if (key == "Lights.FollowFullAlpha") g_follow_full_alpha = std::max(value, 0.001f);
		else return false;
		return true;
	}

	const std::vector<point_lights::light>& collect(const std::vector<const point_lights::light*>& covers, float colour_scale)
	{
		g_out.clear();
		if (!g_loaded)
			load();
		float cam[3] = {};
		if (g_table.empty() || !shared::common::ffp_state::get().world_offset(cam[0], cam[1], cam[2]))
			return g_out;

		const UINT frame = shared::common::ffp_state::get().frame_count();
		roll_drawn();
		if (!g_drawn_logged && !g_drawn_last.empty())
		{
			g_drawn_logged = true;
			int sprites = 0;
			for (const auto& [owner, d] : g_drawn_last)
				sprites += d.sprites;
			shared::common::log("FfxLights", std::format("sprites carry their effect: {} emitters drew {} additive sprites in a frame", g_drawn_last.size(), sprites));
		}
		const auto time_now = std::chrono::steady_clock::now();
		const float now = std::chrono::duration<float>(time_now - g_origin).count();
		const float follow_dt = std::clamp(std::chrono::duration<float>(time_now - g_follow_time).count(), 0.0f, 0.5f);
		g_follow_time = time_now;
		const float follow_rise = 1.0f - std::exp(-follow_dt / g_follow_rise_seconds);
		const float follow_fall = 1.0f - std::exp(-follow_dt / g_follow_fall_seconds);
		// inst is null for a spot that follows its effect's sprites (its key is kSpotKey | its id).
		struct candidate { const ffx_instances::instance* inst; const entry* e; float pos[3]; float distance; float level; unsigned long long key; int ffx_id; int top_id; };
		std::vector<candidate> candidates;
		for (const auto& inst : ffx_instances::current())
		{
			const auto it = g_table.find(inst.ffx_id);
			if (it == g_table.end())
				continue;
			candidate c { &inst, &it->second };
			if (!ffx_instances::world_position(inst.address, c.pos))
				continue;
			c.key = inst.address;
			c.ffx_id = inst.ffx_id;
			// One that follows its effect's sprites: its light is the spot's, below.
			if (follow_rule_for(inst.ffx_id))
			{
				auto spot = std::find_if(g_followers.begin(), g_followers.end(), [&](const follower& fl)
					{
						const float dx = fl.pos[0] - c.pos[0], dy = fl.pos[1] - c.pos[1], dz = fl.pos[2] - c.pos[2];
						return fl.ffx_id == inst.ffx_id && dx * dx + dy * dy + dz * dz <= kFollowSameSpot * kFollowSameSpot;
					});
				if (spot == g_followers.end())
				{
					g_followers.push_back({ g_next_follower++, inst.ffx_id });
					spot = std::prev(g_followers.end());
				}
				std::memcpy(spot->pos, c.pos, sizeof(spot->pos));
				spot->e = c.e;
				spot->top_id = ffx_instances::top_effect_id(inst.address);
				spot->instance_seen = frame;
				continue;
			}
			// In the lookdev host the instance is a capture's, frozen: its level is the one the game had for it at that moment
			// (full when the capture has none), not one that runs on with the host's clock.
			if (const auto* snap = ffx_instances::captured(inst.address))
			{
				c.level = snap->level >= 0.0f ? snap->level : 1.0f;
				if (c.level < kFadeOff)
					continue;
				if (c.e->persistent && std::any_of(covers.begin(), covers.end(), [&](const point_lights::light* l)
					{
						const float dx = l->x - c.pos[0], dy = l->y - c.pos[1], dz = l->z - c.pos[2];
						return dx * dx + dy * dy + dz * dz <= kCoverRadius * kCoverRadius;
					}))
					continue;
				const float dx = c.pos[0] - cam[0], dy = c.pos[1] - cam[1], dz = c.pos[2] - cam[2];
				c.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
				if (c.distance - c.e->dwindle_end <= kReach)
					candidates.push_back(c);
				continue;
			}
			// How bright it is now: it ends with what it shows, not with the instance.
			auto [f, fresh_fade] = g_fades.try_emplace(inst.address, fade { inst.ffx_id, 1.0f, frame, now });
			if (fresh_fade || f->second.ffx_id != inst.ffx_id)
				f->second = { inst.ffx_id, 1.0f, frame, now };
			f->second.seen = frame;
			// The light follows what the game's particles do, from the effect's own fade: an impact over its whole life, others once they stop emitting,
			// when the last particles out go through theirs.
			const float own_age = ffx_instances::age(inst.address);
			if (std::isfinite(own_age))
			{
				// A new instance at a freed one's address, running the same effect: its age starts again.
				if (own_age + 0.1f < f->second.own_age)
					f->second = { inst.ffx_id, 1.0f, frame, now };
				f->second.own_age = own_age;
			}
			const float age = std::isfinite(own_age) ? std::max(own_age, now - f->second.first_seen) : now - f->second.first_seen;
			if (c.e->impact)
				f->second.level = fade_at(*c.e, age);
			else if (age >= c.e->visual_life)
			{
				// "Persistent" is inferred for many effects (a character's event effects), which then play once and
				// stay behind like the brief ones. One with no children at all draws by itself: it stays lit.
				int working = 0;
				// A brief effect's light also ends when only 2118 remains (Hellkite's fire). A persistent one, a torch's, only by 2101.
				const auto children = ffx_instances::child_state(inst.address, &working, !c.e->persistent);
				const bool ended_for_good = f->second.ended >= 0.0f && now - f->second.ended > kEndedForGood;
				const bool lit = c.e->persistent ? children != ffx_instances::children::leftover
					: !ended_for_good && children == ffx_instances::children::running;
				const bool just_ended = !lit && f->second.ended < 0.0f;
				if (lit)
					f->second.ended = -1.0f;
				else if (just_ended)
					f->second.ended = now;
				// The sprite's own fade from its brightest on, sped up where it runs past kEndedForGood: some last 13 s, and that is a light left hanging.
				const float tail = c.e->fade_life - c.e->fade_peak;
				f->second.level = lit ? 1.0f : fade_at(*c.e, c.e->fade_peak + (now - f->second.ended) * std::max(tail / kEndedForGood, 1.0f));
				if (lit && !c.e->persistent && age > c.e->visual_life + kLingerLog && g_lingering.insert(inst.ffx_id).second)
					shared::common::log("FfxLights", std::format("effect {} (under {}) lit {:.0f} s past its visual life {:.2f} s, kept on by child {}",
						inst.ffx_id, ffx_instances::top_effect_id(inst.address), age - c.e->visual_life, c.e->visual_life, working));
				if (just_ended && c.e->persistent && g_ended.insert(inst.ffx_id).second)
					shared::common::log("FfxLights", std::format("persistent effect {} (under {}) ends at age {:.1f} s: only leftovers remain",
						inst.ffx_id, ffx_instances::top_effect_id(inst.address), age));
			}
			else
			{
				// A fresh instance's age can read as junk for a frame: that frame's ending does not stay.
				f->second.level = 1.0f;
				f->second.ended = -1.0f;
			}
			c.level = f->second.level;
			if (c.level < kFadeOff)
				continue;
			if (c.e->persistent && std::any_of(covers.begin(), covers.end(), [&](const point_lights::light* l)
				{
					const float dx = l->x - c.pos[0], dy = l->y - c.pos[1], dz = l->z - c.pos[2];
					return dx * dx + dy * dy + dz * dz <= kCoverRadius * kCoverRadius;
				}))
				continue;
			const float dx = c.pos[0] - cam[0], dy = c.pos[1] - cam[1], dz = c.pos[2] - cam[2];
			c.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (c.distance - c.e->dwindle_end <= kReach)
				candidates.push_back(c);
		}
		std::erase_if(g_fades, [&](const auto& t) { return t.second.seen != frame; });

		// The spots that follow their effect's sprites: as bright as what is drawn there is opaque, against the halo's full alpha. With the
		// effect still placed there, a spot in view showing nothing has lost it and one out of view keeps its level; with the effect gone,
		// its last particles fade and so does the light.
		for (auto& fl : g_followers)
		{
			const float alpha = fl.alpha_frame;
			fl.alpha_frame = 0.0f;
			const bool placed = fl.instance_seen == frame;
			if (alpha > 0.0f || !placed || follow_spot_in_view(fl.pos))
			{
				const float target = std::min(1.0f, alpha / g_follow_full_alpha);
				fl.level += (target - fl.level) * (target > fl.level ? follow_rise : follow_fall);
			}
			if (fl.level < kFadeOff || !fl.e)
				continue;
			candidate c { nullptr, fl.e, { fl.pos[0], fl.pos[1], fl.pos[2] } };
			c.level = fl.level;
			c.key = kSpotKey | fl.id;
			c.ffx_id = fl.ffx_id;
			c.top_id = fl.top_id;
			const float dx = c.pos[0] - cam[0], dy = c.pos[1] - cam[1], dz = c.pos[2] - cam[2];
			c.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (c.distance - c.e->dwindle_end <= kReach)
				candidates.push_back(c);
		}
		std::erase_if(g_followers, [&](const follower& fl) { return fl.instance_seen != frame && fl.level < kFadeOff; });
		if (candidates.size() > kMaxLights)
		{
			std::nth_element(candidates.begin(), candidates.begin() + kMaxLights, candidates.end(),
				[](const candidate& a, const candidate& b) { return a.distance < b.distance; });
			candidates.resize(kMaxLights);
		}

		for (const auto& c : candidates)
		{
			auto [it, fresh] = g_slots.try_emplace(c.key, tracked { c.ffx_id, 0, frame });
			if (fresh || it->second.ffx_id != c.ffx_id)
				it->second = { c.ffx_id, free_slot(), frame };
			it->second.seen = frame;

			point_lights::light l {};
			l.slot = it->second.slot;
			l.x = c.pos[0]; l.y = c.pos[1]; l.z = c.pos[2];
			l.r = c.e->r * c.level * colour_scale; l.g = c.e->g * c.level * colour_scale; l.b = c.e->b * c.level * colour_scale;
			l.dwindle_begin = c.e->dwindle_begin;
			l.dwindle_end = c.e->dwindle_end;
			float lifted = 0.0f;
			// A fire's reach follows the flame its own effect draws (sized_reach). Not a spot that follows sprites (no instance).
			if (const float flame = c.inst ? sized_reach(c.inst->address, c.ffx_id, c.e->r, c.e->g, c.e->b) : 0.0f; flame > 0.0f)
			{
				// Only longer than the table's, unless [Lights] EffectSizeShrink.
				const float sized = g_size_shrink ? flame : std::max(flame, c.e->dwindle_end);
				if (g_size_logged.insert(c.ffx_id).second)
					shared::common::log("FfxLights", std::format("effect {}: its flame spreads {:.2f} m, reach {:.1f} m (the table's {:.1f} m)",
						c.ffx_id, g_effect_spread[c.ffx_id], sized, c.e->dwindle_end));
				l.dwindle_end = sized;
				l.dwindle_begin = std::min(l.dwindle_begin, sized);
				// ... and it stands at its flame's height, not inside what the flame burns in (sized_lift). A steady flame only: a
				// brief effect moves as it plays, and its centre is no place for a light (the bonfire's sit effect 90012, a strong
				// light, lifted half a metre onto the character's back and burned it red, frame_20261111_143022).
				if (c.e->persistent)
					lifted = sized_lift(c.ffx_id, c.e->r, c.e->g, c.e->b);
				l.y += lifted;
			}
			note_lift(l.slot, lifted);
			l.has_params = true;
			l.source = point_lights::param_source::ffx_table;
			l.ffx_id = c.ffx_id;
			l.ffx_top_id = c.inst ? ffx_instances::top_effect_id(c.inst->address) : c.top_id;
			g_out.push_back(l);
		}
		std::erase_if(g_slots, [&](const auto& t) { return t.second.seen != frame; });
		std::erase_if(g_sizes, [&](const auto& t) { return frame - t.second.seen > kSizeForget; });
		// What the game learned goes to disk now and then (never from the lookdev host, whose frames are a capture's).
		if (g_size_learn && g_effect_spread_dirty && !shared::globals::lookdev_host && frame - g_effect_spread_saved >= kSizeSaveFrames)
		{
			g_effect_spread_saved = frame;
			g_effect_spread_dirty = false;
			save_sizes();
		}
		// The lookdev host: what came of the capture's instances, when it changes (another capture, or a setting).
		if (static size_t logged = ~size_t(0); shared::globals::lookdev_host && g_out.size() != logged)
		{
			logged = g_out.size();
			std::string ids;
			for (size_t i = 0; i < g_out.size() && i < 12; ++i)
				ids += std::format("{}{} ({:.2f}, {:.2f}, {:.2f}) to {:.1f} m", i ? "; " : "", g_out[i].ffx_id, g_out[i].r, g_out[i].g, g_out[i].b, g_out[i].dwindle_end);
			shared::common::log("FfxLights", std::format("{} effect lights from the capture's instances{}{}", g_out.size(), g_out.empty() ? "" : ": ", ids));
		}
		return g_out;
	}
}
