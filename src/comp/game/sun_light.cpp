#include "std_include.hpp"
#include "sun_light.hpp"

#include "shared/common/ffp_state.hpp"

#include <cmath>

namespace comp::game::sun_light
{
	namespace
	{
		using shared::common::ffp_state;

		// A frame without a direction vote for this long turns the sun off. The
		// grace covers frames where only sky, effects or the menu are drawn.
		constexpr unsigned int kGraceFrames = 30;
		constexpr int kMaxCandidates = 8;

		// One tally per distinct constant value. regs holds the registers the
		// value came from, as bound on the first draw that voted for it.
		struct candidate
		{
			int key[6] = {};
			unsigned int weight = 0;
			float regs[8 * 4] = {};
		};

		struct tally
		{
			candidate c[kMaxCandidates];
			int count = 0;
			unsigned int draws = 0;

			void clear() { count = 0; draws = 0; }

			// key_regs: the float4s that make a value distinct; stored: the
			// registers copied into the candidate (n float4s from store_from).
			void vote(const float* ps, std::initializer_list<int> key_regs,
				int store_from, int store_n, unsigned int weight)
			{
				++draws;
				int key[6] = {};
				int k = 0;
				for (int reg : key_regs)
					for (int i = 0; i < 3 && k < 6; ++i)
						key[k++] = static_cast<int>(std::lround(ps[reg * 4 + i] * 1000.0f));

				for (int i = 0; i < count; ++i)
				{
					if (std::memcmp(c[i].key, key, sizeof(key)) == 0)
					{
						c[i].weight += weight;
						return;
					}
				}
				if (count >= kMaxCandidates)
					return;

				auto& n = c[count++];
				std::memcpy(n.key, key, sizeof(key));
				n.weight = weight;
				std::memcpy(n.regs, &ps[store_from * 4], store_n * 4 * sizeof(float));
			}

			const candidate* best() const
			{
				const candidate* b = nullptr;
				for (int i = 0; i < count; ++i)
					if (!b || c[i].weight > b->weight)
						b = &c[i];
				return b;
			}
		};

		tally g_shadow;   // c175
		tally g_map_dir3; // c92..c99 from non-skinned HemDir3 draws
		tally g_chr_dir3; // c92..c99 from skinned HemDir3 draws
		unsigned int g_accum_frame = 0;
		bool g_accum_open = false;

		state g_state;
		unsigned int g_last_seen_frame = 0;
		bool g_have_colour = false;
		float g_last_colour[3] = {};
		const float* g_ps_const = nullptr;

		bool normalise(float* out, const float* in)
		{
			const float len = std::sqrt(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]);
			if (len < 1e-6f)
				return false;
			for (int i = 0; i < 3; ++i)
				out[i] = in[i] / len;
			return true;
		}

		void load_light(light& out, const float* dir, const float* colour)
		{
			normalise(out.dir, dir);
			for (int i = 0; i < 3; ++i)
				out.colour[i] = std::max(0.0f, colour[i]);
		}

		float g_probe_sun[3] = {};
		bool g_have_probe_sun = false;

		void close_frame()
		{
			if (!g_accum_open)
				return;
			g_accum_open = false;

			g_state.shadow_draws = g_shadow.draws;
			g_state.map_dir3_draws = g_map_dir3.draws;
			g_state.chr_dir3_draws = g_chr_dir3.draws;
			g_state.shadow_candidates = g_shadow.count;
			g_state.dir3_candidates = g_map_dir3.count + g_chr_dir3.count;

			const candidate* shadow = g_shadow.best();
			const candidate* map = g_map_dir3.best();
			const candidate* chr = g_chr_dir3.best();
			const candidate* dir3 = map ? map : chr; // regs[0] = c92

			// Direction: the shadow light first, it is where the game's sun
			// shadows fall.
			bool have_dir = false;
			if (shadow && normalise(g_state.key.dir, shadow->regs))
			{
				g_state.dir_from = dir_source::shadow_c175;
				have_dir = true;
			}
			else if (dir3 && normalise(g_state.key.dir, &dir3->regs[0 * 4]))
			{
				g_state.dir_from = map ? dir_source::map_c92 : dir_source::chr_c92;
				have_dir = true;
			}

			if (dir3)
			{
				const float* r = dir3->regs;
				for (int i = 0; i < 3; ++i)
					g_last_colour[i] = !map && g_have_probe_sun ? g_probe_sun[i] : std::max(0.0f, r[3 * 4 + i]);
				g_have_colour = true;
				g_state.colour_from = map ? colour_source::map_c95 : g_have_probe_sun ? colour_source::probe : colour_source::chr_c95;

				load_light(g_state.fill[0], &r[1 * 4], &r[4 * 4]);
				load_light(g_state.fill[1], &r[2 * 4], &r[5 * 4]);
				for (int i = 0; i < 3; ++i)
				{
					g_state.hemi_up[i] = r[6 * 4 + i];
					g_state.hemi_down[i] = r[7 * 4 + i];
				}
			}
			else if (g_have_probe_sun)
			{
				for (int i = 0; i < 3; ++i)
					g_last_colour[i] = g_probe_sun[i];
				g_have_colour = true;
				g_state.colour_from = colour_source::probe;
			}
			else if (g_have_colour)
			{
				g_state.colour_from = colour_source::last_seen;
			}

			if (have_dir)
			{
				g_state.valid = true;
				g_last_seen_frame = g_accum_frame;
			}

			g_shadow.clear();
			g_map_dir3.clear();
			g_chr_dir3.clear();
		}
	}

	void observe(const float* ps_const, unsigned int frame, unsigned int prim_count,
		uint32_t flags, bool skinned)
	{
		if (!ps_const)
			return;
		g_ps_const = ps_const;

		if (g_accum_open && frame != g_accum_frame)
			close_frame();
		if (!g_accum_open)
		{
			g_accum_open = true;
			g_accum_frame = frame;
		}

		const unsigned int weight = std::max(1u, prim_count);
		auto is_zero = [&](int reg)
		{
			const float* v = &ps_const[reg * 4];
			return v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f;
		};

		// A zero direction is an unset register, not a light.
		if ((flags & ffp_state::PS_LIGHT_SHADOW) && !is_zero(175))
			g_shadow.vote(ps_const, { 175 }, 175, 1, weight);

		if ((flags & ffp_state::PS_LIGHT_DIR3) && !is_zero(92))
			(skinned ? g_chr_dir3 : g_map_dir3).vote(ps_const, { 92, 95 }, 92, 8, weight);
	}

	const state& current(unsigned int frame, const float* fallback_colour)
	{
		if (g_accum_open && frame != g_accum_frame)
			close_frame();

		for (int i = 0; i < 3; ++i)
			g_state.key.colour[i] = g_have_colour ? g_last_colour[i] : std::max(0.0f, fallback_colour[i]);
		if (!g_have_colour)
			g_state.colour_from = colour_source::fallback;

		g_state.frames_since_seen = frame - g_last_seen_frame;
		if (g_state.valid && g_state.frames_since_seen > kGraceFrames)
			g_state.valid = false;

		return g_state;
	}

	void set_probe_sun(const float* colour)
	{
		for (int i = 0; i < 3; ++i)
			g_probe_sun[i] = std::max(0.0f, colour[i]);
		g_have_probe_sun = true;
	}

	bool key_direction(float* out_dir)
	{
		if (!g_state.valid)
			return false;
		for (int i = 0; i < 3; ++i)
			out_dir[i] = g_state.key.dir[i];
		return true;
	}

	const float* ps_constants()
	{
		return g_ps_const;
	}
}
