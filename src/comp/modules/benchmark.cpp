#include "std_include.hpp"
#include "benchmark.hpp"

#include "game_lights.hpp"
#include "../game/fps_unlock.hpp"
#include "../game/frustum_culling.hpp"
#include "shared/common/ffp_state.hpp"

namespace comp::benchmark
{
	using shared::common::config;
	using shared::common::ffp_state;
	using shared::common::log;
	using shared::common::LOG_TYPE;

	// Full togglable option state. Each phase applies one of these wholesale, so
	// a phase can never inherit leftovers from the previous phase. Dependencies
	// are expressed by construction: FFP sub-options are only measured in phases
	// that also enable ffp_enabled (plus zwrite_query, which the prepass gate of
	// suppress_aux relies on), standalone options on top of the all-off baseline.
	struct option_state
	{
		bool ffp_enabled = false;
		bool suppress_aux = false;
		bool skip_postfx = false;
		bool tex_filter = false;
		bool expand_skinned_uv = false;
		bool shader_tex_fix = false;
		bool texture_layer_blend = false;
		int dedup_mode = 0;
		bool zwrite_query = false;
		bool skinning = false;
		bool lights = false;
		bool frustum_culling = true; // true = stock engine culling (cheapest)
	};

	struct phase
	{
		std::string name;
		std::string reference; // phase whose avg fps this one is compared against
		option_state state;
	};

	struct phase_result
	{
		std::string name;
		std::string reference;
		std::string options;
		size_t frames = 0;
		double avg_fps = 0.0, median_fps = 0.0, low1_fps = 0.0, min_fps = 0.0, max_fps = 0.0;
		double avg_ms = 0.0;
	};

	namespace
	{
		bool s_running = false;
		std::vector<phase> s_phases;
		std::vector<phase_result> s_results;
		size_t s_phase_idx = 0;
		int s_frame_in_phase = 0;
		int s_settle_frames = 90;
		int s_measure_frames = 240;
		std::vector<double> s_frame_ms;
		LARGE_INTEGER s_qpc_freq = {};
		LARGE_INTEGER s_last_qpc = {};
		bool s_have_last_qpc = false;

		option_state s_snapshot = {};
		bool s_snapshot_auto_enable = false;
		std::string s_last_result_file;

		option_state capture_current()
		{
			auto& cfg = config::get();
			auto& ffp = ffp_state::get();

			option_state s;
			s.ffp_enabled = ffp.is_enabled();
			s.suppress_aux = cfg.ffp.suppress_aux_passes;
			s.skip_postfx = cfg.ffp.skip_postfx;
			s.tex_filter = cfg.ffp.tex_filter;
			s.expand_skinned_uv = cfg.ffp.expand_skinned_uv;
			s.shader_tex_fix = cfg.ffp.shader_tex_fix;
			s.texture_layer_blend = cfg.ffp.texture_layer_blend;
			s.dedup_mode = cfg.ffp.dedup_pass_mode;
			s.zwrite_query = cfg.ffp.zwrite_query_per_draw;
			s.skinning = cfg.skinning.enabled;
			s.lights = game_lights::get() ? game_lights::get()->enabled : false;
			s.frustum_culling = game::frustum_culling::is_available()
				? game::frustum_culling::is_enabled() : true;
			return s;
		}

		void apply_state(const option_state& s)
		{
			auto& cfg = config::get();
			auto& ffp = ffp_state::get();

			ffp.set_enabled(s.ffp_enabled);
			cfg.ffp.suppress_aux_passes = s.suppress_aux;
			cfg.ffp.skip_postfx = s.skip_postfx;
			if (cfg.ffp.tex_filter != s.tex_filter)
			{
				cfg.ffp.tex_filter = s.tex_filter;
				ffp.clear_global_textures(); // same reset the F4 checkbox performs
			}
			cfg.ffp.expand_skinned_uv = s.expand_skinned_uv;
			cfg.ffp.shader_tex_fix = s.shader_tex_fix;
			cfg.ffp.texture_layer_blend = s.texture_layer_blend;
			cfg.ffp.dedup_pass_mode = s.dedup_mode;
			cfg.ffp.zwrite_query_per_draw = s.zwrite_query;
			cfg.skinning.enabled = s.skinning;
			if (auto* gl = game_lights::get())
				gl->set_enabled(s.lights);
			if (game::frustum_culling::is_available())
				game::frustum_culling::set_enabled(s.frustum_culling);
		}

		std::string describe(const option_state& s)
		{
			std::string out;
			auto add = [&out](const char* name) {
				if (!out.empty()) out += ", ";
				out += name;
			};
			if (s.ffp_enabled) add("FFP");
			if (s.suppress_aux) add("SuppressAuxPasses");
			if (s.skip_postfx) add("SkipPostFX");
			if (s.tex_filter) add("TexFilter");
			if (s.expand_skinned_uv) add("ExpandSkinnedUV");
			if (s.shader_tex_fix) add("ShaderTexFix");
			if (s.texture_layer_blend) add("TextureLayerBlend");
			if (s.dedup_mode != 0) add(std::format("DedupPassMode={}", s.dedup_mode).c_str());
			if (s.zwrite_query) add("ZWriteQueryPerDraw");
			if (s.skinning) add("Skinning");
			if (s.lights) add("GameLights");
			if (!s.frustum_culling) add("FrustumCullingOff");
			if (out.empty()) out = "(everything off)";
			return out;
		}

		void build_phase_plan()
		{
			s_phases.clear();

			const option_state base = {}; // everything off, stock culling
			auto add = [](const char* name, const char* ref, const option_state& st) {
				s_phases.push_back({ name, ref, st });
			};

			add("all_off", "", base);

			// Standalone options — measured on top of the all-off baseline.
			{ auto p = base; p.zwrite_query = true;   add("zwrite_query", "all_off", p); }
			{ auto p = base; p.shader_tex_fix = true; add("shader_tex_fix", "all_off", p); }
			{ auto p = base; p.lights = true;         add("lights", "all_off", p); }
			if (game::frustum_culling::is_available())
			{
				auto p = base; p.frustum_culling = false;
				add("culling_off", "all_off", p);
			}

			// FFP and its sub-options — each sub-option on top of the bare FFP
			// phase so its delta vs ffp_base isolates that option's cost.
			// ffp_base keeps the z-write query on: SuppressAuxPasses' prepass
			// detection depends on it, so all FFP phases share that baseline cost.
			option_state ffp_base = base;
			ffp_base.ffp_enabled = true;
			ffp_base.zwrite_query = true;
			add("ffp_base", "all_off", ffp_base);

			{ auto p = ffp_base; p.suppress_aux = true;        add("ffp_suppress_aux", "ffp_base", p); }
			{ auto p = ffp_base; p.skip_postfx = true;         add("ffp_skip_postfx", "ffp_base", p); }
			{ auto p = ffp_base; p.tex_filter = true;          add("ffp_tex_filter", "ffp_base", p); }
			{ auto p = ffp_base; p.dedup_mode = 2;             add("ffp_dedup_keep_last", "ffp_base", p); }
			{ auto p = ffp_base; p.expand_skinned_uv = true;   add("ffp_expand_skinned_uv", "ffp_base", p); }
			{ auto p = ffp_base; p.texture_layer_blend = true; add("ffp_texture_layer_blend", "ffp_base", p); }
			{ auto p = ffp_base; p.skinning = true;            add("ffp_skinning", "ffp_base", p); }

			// Where the user actually is, measured uncapped like everything else.
			add("user_config", "all_off", s_snapshot);
		}

		double percentile_low_fps(std::vector<double>& sorted_ms, double fraction)
		{
			// Mean of the worst <fraction> frame times → "1% low" style fps.
			const size_t n = sorted_ms.size();
			size_t count = static_cast<size_t>(static_cast<double>(n) * fraction);
			if (count == 0) count = 1;
			double sum = 0.0;
			for (size_t i = n - count; i < n; i++)
				sum += sorted_ms[i];
			const double avg = sum / static_cast<double>(count);
			return avg > 0.0 ? 1000.0 / avg : 0.0;
		}

		void finish_phase()
		{
			const auto& ph = s_phases[s_phase_idx];

			phase_result r;
			r.name = ph.name;
			r.reference = ph.reference;
			r.options = describe(ph.state);
			r.frames = s_frame_ms.size();

			if (!s_frame_ms.empty())
			{
				std::vector<double> sorted = s_frame_ms;
				std::sort(sorted.begin(), sorted.end());

				double total = 0.0;
				for (double ms : sorted) total += ms;
				r.avg_ms = total / static_cast<double>(sorted.size());
				r.avg_fps = r.avg_ms > 0.0 ? 1000.0 / r.avg_ms : 0.0;
				const double med = sorted[sorted.size() / 2];
				r.median_fps = med > 0.0 ? 1000.0 / med : 0.0;
				r.low1_fps = percentile_low_fps(sorted, 0.01);
				r.min_fps = sorted.back() > 0.0 ? 1000.0 / sorted.back() : 0.0;
				r.max_fps = sorted.front() > 0.0 ? 1000.0 / sorted.front() : 0.0;
			}

			s_results.push_back(r);
			log("Benchmark", std::format("phase {}/{} '{}' done: {:.1f} fps avg ({:.2f} ms)",
				s_phase_idx + 1, s_phases.size(), r.name, r.avg_fps, r.avg_ms));
		}

		double reference_avg_fps(const std::string& ref)
		{
			for (const auto& r : s_results)
			{
				if (r.name == ref)
					return r.avg_fps;
			}
			return 0.0;
		}

		void write_results()
		{
			const std::string dir = shared::globals::root_path + "\\rtx_comp";
			std::filesystem::create_directories(dir);

			SYSTEMTIME st = {};
			GetLocalTime(&st);
			const std::string file = std::format("{}\\benchmark_{:04}{:02}{:02}_{:02}{:02}{:02}.txt",
				dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

			std::ofstream out(file, std::ios::out | std::ios::trunc);
			if (!out.is_open())
			{
				log("Benchmark", "failed to open results file", LOG_TYPE::LOG_TYPE_ERROR, true);
				return;
			}

			out << std::format("DS1 RTX proxy option-cost benchmark — {:04}-{:02}-{:02} {:02}:{:02}:{:02}\n",
				st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
			out << std::format("settle={} frames, measure={} frames per phase; FPS-unlock cap bypassed during the run\n",
				s_settle_frames, s_measure_frames);
			out << "Numbers compare proxy CPU/bridge cost. Make sure Remix vsync is off (rtx.conf) and\n";
			out << "stand still in a representative scene; GPU-bound scenes flatten the deltas.\n";
			out << "delta = avg fps vs the reference phase (ffp sub-options reference ffp_base).\n\n";

			out << std::format("{:<26} {:>8} {:>8} {:>8} {:>8} {:>8} {:>9} {:<12} options\n",
				"phase", "avg", "median", "1% low", "min", "max", "avg ms", "delta(ref)");
			out << std::string(140, '-') << "\n";

			for (const auto& r : s_results)
			{
				std::string delta = "-";
				if (!r.reference.empty())
				{
					const double ref_fps = reference_avg_fps(r.reference);
					if (ref_fps > 0.0)
						delta = std::format("{:+.1f}", r.avg_fps - ref_fps);
				}
				out << std::format("{:<26} {:>8.1f} {:>8.1f} {:>8.1f} {:>8.1f} {:>8.1f} {:>9.2f} {:<12} {}\n",
					r.name, r.avg_fps, r.median_fps, r.low1_fps, r.min_fps, r.max_fps, r.avg_ms,
					delta + (r.reference.empty() ? "" : " vs " + r.reference), r.options);
			}

			out.close();
			s_last_result_file = file;
			log("Benchmark", std::format("results written: {}", file), LOG_TYPE::LOG_TYPE_GREEN, true);
		}

		void restore_and_stop()
		{
			apply_state(s_snapshot);
			config::get().ffp.auto_enable = s_snapshot_auto_enable;
			game::fps_unlock::set_cap_bypass(false);
			s_running = false;
			s_frame_ms.clear();
			s_have_last_qpc = false;
		}
	}

	bool is_running() { return s_running; }

	void start()
	{
		if (s_running) return;

		auto& cfg = config::get();
		s_settle_frames = std::max(10, cfg.benchmark.settle_frames);
		s_measure_frames = std::max(30, cfg.benchmark.measure_frames);

		s_snapshot = capture_current();
		// Auto-enable would fight the per-phase FFP state (it re-enables FFP once
		// enough eligible draws are seen) — park it for the duration of the run.
		s_snapshot_auto_enable = cfg.ffp.auto_enable;
		cfg.ffp.auto_enable = false;

		build_phase_plan();
		s_results.clear();
		s_phase_idx = 0;
		s_frame_in_phase = 0;
		s_frame_ms.clear();
		s_frame_ms.reserve(static_cast<size_t>(s_measure_frames));
		s_have_last_qpc = false;
		QueryPerformanceFrequency(&s_qpc_freq);

		game::fps_unlock::set_cap_bypass(true);
		shared::globals::imgui_menu_open = false; // overlay UI would skew every phase

		s_running = true;
		log("Benchmark", std::format("started: {} phases, settle {} + measure {} frames each",
			s_phases.size(), s_settle_frames, s_measure_frames), LOG_TYPE::LOG_TYPE_GREEN, true);
	}

	void cancel()
	{
		if (!s_running) return;
		restore_and_stop();
		log("Benchmark", "cancelled — settings restored", LOG_TYPE::LOG_TYPE_WARN, true);
	}

	void on_present()
	{
		if (!s_running) return;

		if (s_frame_in_phase == 0)
		{
			apply_state(s_phases[s_phase_idx].state);
			s_frame_ms.clear();
			s_have_last_qpc = false;
			log("Benchmark", std::format("phase {}/{}: {} [{}]",
				s_phase_idx + 1, s_phases.size(), s_phases[s_phase_idx].name,
				describe(s_phases[s_phase_idx].state)));
		}

		s_frame_in_phase++;

		if (s_frame_in_phase > s_settle_frames)
		{
			LARGE_INTEGER now = {};
			QueryPerformanceCounter(&now);
			if (s_have_last_qpc)
			{
				const double ms = static_cast<double>(now.QuadPart - s_last_qpc.QuadPart)
					* 1000.0 / static_cast<double>(s_qpc_freq.QuadPart);
				s_frame_ms.push_back(ms);
			}
			s_last_qpc = now;
			s_have_last_qpc = true;
		}

		if (s_frame_in_phase >= s_settle_frames + s_measure_frames)
		{
			finish_phase();
			s_phase_idx++;
			s_frame_in_phase = 0;

			if (s_phase_idx >= s_phases.size())
			{
				write_results();
				restore_and_stop();
				log("Benchmark", "complete — settings restored", LOG_TYPE::LOG_TYPE_GREEN, true);
			}
		}
	}

	void tab_imgui()
	{
		auto& cfg = config::get();

		ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Option-Cost Benchmark");
		ImGui::Separator();
		ImGui::TextWrapped(
			"Toggles every option off, measures a baseline, then measures each option "
			"individually (FFP sub-options on top of a bare-FFP phase), finishing with "
			"your current configuration. Settings are restored afterwards and results "
			"go to rtx_comp\\benchmark_<timestamp>.txt.");
		ImGui::TextWrapped(
			"Stand still in a representative in-game scene, disable Remix vsync in "
			"rtx.conf, and don't touch settings during the run.");
		ImGui::Spacing();

		ImGui::BeginDisabled(s_running);
		ImGui::SliderInt("Settle Frames", &cfg.benchmark.settle_frames, 10, 600);
		ImGui::SliderInt("Measure Frames", &cfg.benchmark.measure_frames, 30, 2000);
		ImGui::EndDisabled();

		const int per_phase = cfg.benchmark.settle_frames + cfg.benchmark.measure_frames;
		ImGui::TextDisabled("~%d frames per phase, 15 phases (14 without culling patch)", per_phase);
		ImGui::Spacing();

		if (!s_running)
		{
			if (ImGui::Button("Start Benchmark (closes this menu)", ImVec2(280, 0)))
				start();
		}
		else
		{
			if (ImGui::Button("Cancel", ImVec2(280, 0)))
				cancel();
			ImGui::SameLine();
			ImGui::Text("phase %zu/%zu: %s", s_phase_idx + 1, s_phases.size(),
				s_phase_idx < s_phases.size() ? s_phases[s_phase_idx].name.c_str() : "");
			ImGui::ProgressBar(static_cast<float>(s_frame_in_phase) / static_cast<float>(per_phase));
		}

		if (!s_last_result_file.empty())
		{
			ImGui::Spacing();
			ImGui::TextWrapped("Last results: %s", s_last_result_file.c_str());
		}
	}
}
