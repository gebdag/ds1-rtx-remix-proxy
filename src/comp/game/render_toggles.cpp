// DS1 schedule-level render kill switches.
//
// The engine folds the GameRend stage booleans and the draw-group globals
// into its retained draw plan every frame (FUN_00eaf530 / FUN_00eaf490 from
// GameRend::Update), so writing them is exactly what the game's own "GAME
// REND" debug menu does: entities of a killed group are never collected,
// draws of a killed stage are never emitted.
//
// Post-processing lives in the NS_FRPG::ImageFilter chain at GameRend+0x3C
// and is killed with the engine's own mechanism: token 0x15 SetEnable(0)
// (emitter FUN_00467d70: EAX = plan token buffer, ECX = the filter's
// HGDrawPlanEntitySeed, byte payload on the stack, caller cleans up) on the
// filter's retained plan node — how the engine itself toggles MSAA, the
// Z pre-pass and the offscreen-SFX pair per frame.
//
// Emission timing is critical: the token buffer's cursor is only valid while
// the game thread is recording the frame's plan updates. Emitting from the
// proxy's Present hook (render side) crashes — the cursor then points at
// consumed plan memory. So SetEnable tokens are emitted from a MinHook detour
// on the token-0x2C stage-mask emitter (FUN_00467d00), which GameRend::Update
// calls every frame with the live token buffer in EAX: same thread, same
// recording window, same buffer the engine's own emitters use.
//
// All addresses were established by the 2026-07-26/27 Ghidra sessions
// (findings.db topics 'renderpaths'; sources analysis/ghidra_renderpaths.md,
// session-20260727-postfx-gates). ASLR is disabled and ImageBase is
// 0x00400000; VAs are rebased via EXE_BASE anyway so a relocated module
// would still resolve.
#include "std_include.hpp"
#include "render_toggles.hpp"
#include "../modules/crash_context.hpp"
#include "shared/common/config.hpp"
#include "shared/globals.hpp"

namespace comp::game::render_toggles
{
	namespace
	{
		constexpr DWORD IMAGE_BASE = 0x00400000;

		// [0x013783C0] -> HGMan; HGMan+0x6F8 -> NS_FRPG::GameRend
		constexpr DWORD HGMAN_SLOT_VA = 0x013783C0;
		constexpr DWORD GAMEREND_OFFSET = 0x6F8;
		// [0x01378464] -> NS_FRPG::DrawEnvMan
		constexpr DWORD DRAWENVMAN_SLOT_VA = 0x01378464;
		// g_ShadowMapSize — atlas edge length, engine-supported 512..2048
		constexpr DWORD SHADOWMAP_SIZE_VA = 0x012DFF34;
		// GameRend+0x3C -> NS_FRPG::ImageFilter (the post-process chain)
		constexpr DWORD IMAGE_FILTER_OFFSET = 0x3C;
		// Token 0x15 SetEnable(bool) emitter
		constexpr DWORD SET_ENABLE_EMITTER_VA = 0x00467D70;
		// Token 0x2C AddDrawStageMask emitter — hooked to piggyback SetEnable
		// emissions inside the game's own plan-recording window.
		constexpr DWORD STAGE_MASK_EMITTER_VA = 0x00467D00;

		constexpr int LOD_BIAS_STOCK = INT_MIN;

		enum class root : uint8_t
		{
			global,       // absolute VA data write
			gamerend,     // offset on the GameRend object
			drawenvman,   // offset on the DrawEnvMan object
			code,         // absolute VA one-byte code patch (VirtualProtect)
			filter_node,  // addr = ImageFilter offset; SetEnable token per frame
		};

		struct field
		{
			root r;
			DWORD addr;     // VA (global/code) or object offset
			uint8_t size;   // 1, 2 or 4 bytes (2 only for code patches)
			uint32_t kill;  // value written / patched while the toggle is off
			uint32_t stock; // code only: expected stock bytes — patch is skipped on mismatch
		};

		struct entry
		{
			toggle_desc desc;
			const char* ini_key;
			field fields[3];
			int nfields;
			// filter_node only: payload emitted once when the toggle is re-enabled.
			// 1 for everything except DoF, which ships SetEnable(0) at construction
			// and is re-enabled by the engine on demand.
			uint8_t restore_payload = 1;
		};

		// Order must match the toggle enum.
		const entry k_entries[static_cast<int>(toggle::count)] =
		{
			{ { "Map (static world)",            "GROUP Map" },            "Map",            {{ root::global, 0x012DEFEB, 1, 0 }}, 1 },
			{ { "Objects (dynamic/physics)",     "GROUP Obj" },            "Objects",        {{ root::global, 0x012DF241, 1, 0 }}, 1 },
			{ { "Characters",                    "GROUP Chr" },            "Characters",     {{ root::global, 0x012DF242, 1, 0 }}, 1 },
			{ { "Particle effects",              "GROUP Sfx" },            "Sfx",            {{ root::global, 0x012DF243, 1, 0 }}, 1 },
			{ { "Cutscene models",               "GROUP Remo" },           "Cutscenes",      {{ root::global, 0x012DF3D1, 1, 0 }}, 1 },
			{ { "Light shafts",                  "GROUP LightShaft" },     "LightShafts",    {{ root::global, 0x012DF3D3, 1, 0 }}, 1 },

			{ { "Z prepass",                     "IsEnableZPrePass" },               "ZPrePass",            {{ root::gamerend, 0x23C, 1, 0 }}, 1 },
			{ { "Depth+normal buffer",           "DepthDraw_DepthNormal" },          "DepthNormalStage",    {{ root::gamerend, 0x23E, 1, 0 }}, 1 },
			{ { "Depth tex-edge",                "DepthDraw_DepthTexEdge" },         "DepthTexEdgeStage",   {{ root::gamerend, 0x23F, 1, 0 }}, 1 },
			{ { "Main opaque geometry",          "NormalDraw_Normal" },              "NormalStage",         {{ root::gamerend, 0x240, 1, 0 }}, 1 },
			{ { "Bounding-region draws",         "NormalDraw_BoundingRegion" },      "BoundingRegionStage", {{ root::gamerend, 0x241, 1, 0 }}, 1 },
			{ { "Texture-edge outlines",         "NormalDraw_TexEdge" },             "TexEdgeStage",        {{ root::gamerend, 0x242, 1, 0 }}, 1 },
			{ { "Water mask (offscreen)",        "NormalDraw_WaterMask" },           "WaterMaskStage",      {{ root::gamerend, 0x243, 1, 0 }}, 1 },
			{ { "Water surfaces",                "NormalDraw_Water" },               "WaterStage",          {{ root::gamerend, 0x244, 1, 0 }}, 1 },
			{ { "Translucent geometry",          "NormalDraw_Trans" },               "TransStage",          {{ root::gamerend, 0x245, 1, 0 }}, 1 },
			{ { "Shadow-caster geometry",        "ShadowDepth_ShadowDepthNormal" },  "ShadowDepthStage",    {{ root::gamerend, 0x246, 1, 0 }}, 1 },
			{ { "Shadow-caster tex-edge",        "ShadowDepth_ShadowDepthTexEdge" }, "ShadowTexEdgeStage",  {{ root::gamerend, 0x247, 1, 0 }}, 1 },

			{ { "Shadow cascade scheduling",     "DrawEnvMan.shadow_cascade_count" }, "ShadowCascades",     {{ root::drawenvman, 0x30, 4, 0 }}, 1 },
			// The reflect sub-plan is retained (built once in the GameRend ctor) and has
			// no enable node; the only per-frame variable is the stage mask FUN_00eaf530
			// folds onto the reflect-exclusive node [GameRend+0xCC]. Patching JE(74 05)
			// to JMP(EB 13) at 0xEAF61D skips both bit-sets, so the node gets mask 0 and
			// zero geometry is submitted with the mirrored ReflectCamera. The RT, its
			// grey clear, the camera and the clip plane all stay intact — water materials
			// that sample the map read uniform grey, not stale garbage.
			// (GameRend+0x2E5..+0x2E7 are debug map overlays defaulting to 0 — writing
			// them was a no-op; see analysis/ghidra_water_reflection_gate.md.)
			{ { "Water reflection capture",      "reflect stage mask @0xEAF61D" }, "WaterReflection",
				{{ root::code, 0x00EAF61D, 2, 0x13EB, 0x0574 }}, 1 },
			{ { "Offscreen SFX (half-res aux)",  "OffSfx SwitchMode = force off" },  "OffscreenSfx",        {{ root::gamerend, 0x21E, 1, 1 }}, 1 },
			// The MSAA switch byte is +0x22D per the 2026-07-27 trace of FUN_00eb0230;
			// the 2026-07-26 debug-menu table reads it as +0x231. Unresolved, so this
			// ships stock — it writes nothing until someone confirms the offset live.
			{ { "MSAA resolve/downsample",       "MSAA SwitchMode = force off" },    "MSAA",                {{ root::gamerend, 0x22D, 1, 1 }}, 1 },
			{ { "Env-map cube capture",          "JE gate 0xF8F053" },               "EnvMapCapture",       {{ root::code, 0x00F8F053, 1, 0xEB, 0x74 }}, 1 },

			{ { "Depth of field",                "ImageProcessDof" },        "DoF",             {{ root::filter_node, 0x18, 0, 0 }}, 1, 0 },
			{ { "Bloom",                         "ImageProcessBloom" },      "Bloom",           {{ root::filter_node, 0x24, 0, 0 }}, 1, 1 },
			{ { "Tone mapping",                  "ImageProcessToneMap" },    "ToneMap",         {{ root::filter_node, 0x20, 0, 0 }}, 1, 1 },
			{ { "Light shaft filter",            "ImageProcessLightShaft" }, "LightShaftFilter",{{ root::filter_node, 0x48, 0, 0 }}, 1, 1 },
			{ { "Camera blur",                   "ImageProcessCameraBlur" }, "CameraBlur",      {{ root::filter_node, 0x30, 0, 0 }}, 1, 1 },
			{ { "Post-FX downsample",            "ImageProcessDownSample" }, "PostFxDownsample",{{ root::filter_node, 0x1C, 0, 0 }}, 1, 1 },
			{ { "Post-FX combine",               "ImageProcessCombine" },    "Combine",         {{ root::filter_node, 0x28, 0, 0 }}, 1, 1 },
		};

		struct toggle_state
		{
			bool render = true;
			bool overridden = false;       // kill currently in effect
			bool logged = false;           // first-kill console line emitted
			bool pending_restore = false;  // filter_node: one-shot re-enable queued
			uint32_t original[3] = {};     // pre-kill values (data/code writes)
			const void* capture_root[3] = {}; // object identity at capture time
		};

		toggle_state s_states[static_cast<int>(toggle::count)];
		bool s_config_seeded = false;
		bool s_available = false;
		int s_desired_shadow_map_size = 0;   // 0 = stock
		int s_desired_lod_bias = LOD_BIAS_STOCK;
		uint32_t s_original_shadow_map_size = 0;

		bool plausible(const void* p)
		{
			const auto v = reinterpret_cast<uintptr_t>(p);
			return v >= 0x10000 && v < 0x7FFF0000;
		}

		uint8_t* rebase(DWORD va)
		{
			return reinterpret_cast<uint8_t*>(EXE_BASE + (va - IMAGE_BASE));
		}

		uint8_t* resolve_gamerend()
		{
			if (shared::globals::lookdev_host) return nullptr;
			auto* hgman = *reinterpret_cast<uint8_t**>(rebase(HGMAN_SLOT_VA));
			if (!plausible(hgman)) return nullptr;
			auto* gamerend = *reinterpret_cast<uint8_t**>(hgman + GAMEREND_OFFSET);
			return plausible(gamerend) ? gamerend : nullptr;
		}

		uint8_t* resolve_drawenvman()
		{
			if (shared::globals::lookdev_host) return nullptr;
			auto* dem = *reinterpret_cast<uint8_t**>(rebase(DRAWENVMAN_SLOT_VA));
			return plausible(dem) ? dem : nullptr;
		}

		// SetEnable owner tag of an ImageFilter node: the u32 stored at
		// filterobj+4, passed by VALUE — verified in the engine's own caller
		// FUN_00d86070 (`mov ecx,[edi+0x48]; mov ecx,[ecx+4]; call 0x467D70`).
		// It is an opaque plan-node tag, not necessarily a pointer, so it is
		// only null-checked (exactly what FUN_00eb0130 does with GameRend+0x78).
		uint8_t* resolve_filter_seed(uint8_t* gamerend, DWORD filter_offset)
		{
			auto* image_filter = *reinterpret_cast<uint8_t**>(gamerend + IMAGE_FILTER_OFFSET);
			if (!plausible(image_filter)) return nullptr;
			auto* filter = *reinterpret_cast<uint8_t**>(image_filter + filter_offset);
			if (!plausible(filter)) return nullptr;
			return *reinterpret_cast<uint8_t**>(filter + 4);
		}

		// FUN_00467d70: EAX = token buffer, ECX = owner seed, byte payload on
		// the stack, plain ret (caller cleanup). Emits token 0x15 SetEnable.
		// Only callable from inside the plan-recording window (the stage-mask
		// emitter detour below) — the buffer cursor is invalid at other times.
		void emit_set_enable(uint8_t* token_buf, uint8_t* owner_seed, uint8_t payload)
		{
			const DWORD fn = reinterpret_cast<DWORD>(rebase(SET_ENABLE_EMITTER_VA));
			const DWORD payload32 = payload;
			__asm
			{
				push eax
				push ecx
				push edx
				mov eax, token_buf
				mov ecx, owner_seed
				push payload32
				call fn
				add esp, 4
				pop edx
				pop ecx
				pop eax
			}
		}

		// ---- stage-mask emitter detour: the safe SetEnable emission window ----

		void* s_stage_mask_trampoline = nullptr;
		uint8_t* s_live_token_buf = nullptr;   // valid only inside the detour
		volatile LONG s_emitted_this_frame = 0; // reset in on_present
		bool s_hook_installed = false;
		bool s_hook_failed = false;

		// Runs on the game thread, inside the recording window, once per frame
		// (first stage-mask emission of the frame). Emits SetEnable(0) for every
		// killed filter node and one-shot restore payloads for re-enabled ones.
		void __stdcall emit_pending_filter_tokens()
		{
			if (s_emitted_this_frame)
				return;

			auto* gamerend = resolve_gamerend();
			if (!gamerend || !s_live_token_buf)
				return; // keep the gate open — retry on the next emission this frame

			if (InterlockedExchange(&s_emitted_this_frame, 1) != 0)
				return;

			for (int i = 0; i < static_cast<int>(toggle::count); i++)
			{
				const auto& e = k_entries[i];
				if (e.fields[0].r != root::filter_node)
					continue;

				auto& st = s_states[i];
				const bool want_kill = !st.render;
				if (!want_kill && !st.pending_restore)
					continue;

				auto* seed = resolve_filter_seed(gamerend, e.fields[0].addr);
				if (!seed)
					continue;

				if (want_kill)
				{
					if (!st.overridden)
						crash_context::note("postfx kill: %s", e.desc.label);
					// Re-emitted every frame: the engine may re-enable the node
					// itself (DoF is driven on demand, ToneMap-driven enables).
					emit_set_enable(s_live_token_buf, seed, 0);
					st.overridden = true;
				}
				else
				{
					crash_context::note("postfx restore: %s", e.desc.label);
					emit_set_enable(s_live_token_buf, seed, e.restore_payload);
					st.pending_restore = false;
					st.overridden = false;
				}
			}
		}

		// Detour on FUN_00467d00 (token 0x2C AddDrawStageMask). Same custom
		// convention as the SetEnable emitter: EAX = token buffer, ECX = owner,
		// one dword stack arg, caller cleanup. Captures the live buffer, runs
		// our emission, then tail-jumps to the original via the trampoline so
		// the stack arg and return path are untouched.
		__declspec(naked) void stage_mask_emitter_detour()
		{
			__asm
			{
				mov s_live_token_buf, eax
				pushad
				call emit_pending_filter_tokens
				popad
				mov dword ptr [s_live_token_buf], 0
				jmp s_stage_mask_trampoline
			}
		}

		void install_stage_mask_hook()
		{
			if (s_hook_installed || s_hook_failed)
				return;

			auto* target = rebase(STAGE_MASK_EMITTER_VA);
			if (MH_CreateHook(target, &stage_mask_emitter_detour, &s_stage_mask_trampoline) == MH_OK &&
				MH_EnableHook(target) == MH_OK)
			{
				s_hook_installed = true;
				shared::common::log("Perf", "post-FX SetEnable hook installed (stage-mask emitter 0x467D00)");
			}
			else
			{
				s_hook_failed = true;
				shared::common::log("Perf", "post-FX SetEnable hook FAILED — filter kill switches inactive",
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			}
		}

		// Code bytes are written in one store so the game thread can never
		// execute a torn patch.
		void patch_code(uint8_t* addr, uint32_t value, uint8_t size)
		{
			DWORD old_protect;
			VirtualProtect(addr, size, PAGE_EXECUTE_READWRITE, &old_protect);
			if (size == 2)
				*reinterpret_cast<uint16_t*>(addr) = static_cast<uint16_t>(value);
			else
				*addr = static_cast<uint8_t>(value);
			VirtualProtect(addr, size, old_protect, &old_protect);
		}

		uint8_t* field_address(const field& f, uint8_t* root_ptr)
		{
			return (f.r == root::global || f.r == root::code) ? rebase(f.addr) : root_ptr + f.addr;
		}

		uint32_t read_field(const field& f, uint8_t* addr)
		{
			if (f.size == 1)
				return *addr;
			if (f.size == 2)
				return *reinterpret_cast<uint16_t*>(addr);
			return *reinterpret_cast<uint32_t*>(addr);
		}

		void write_field(const field& f, uint8_t* addr, uint32_t value)
		{
			if (f.r == root::code)
				patch_code(addr, value, f.size);
			else if (f.size == 1)
				*addr = static_cast<uint8_t>(value);
			else
				*reinterpret_cast<uint32_t*>(addr) = value;
		}

		void log_once(toggle_state& st, const entry& e, bool killed)
		{
			if (killed && !st.logged)
			{
				st.logged = true;
				shared::common::log("Perf", std::format("kill: {} [{}]", e.desc.label, e.desc.game_name));
			}
			else if (!killed)
			{
				st.logged = false;
			}
		}

		// State transitions only — the actual token emission happens inside the
		// stage-mask emitter detour, on the game thread, in the recording window.
		void assert_filter_node(int i)
		{
			const auto& e = k_entries[i];
			auto& st = s_states[i];

			if (!st.render)
			{
				st.pending_restore = false;
				log_once(st, e, true);
			}
			else
			{
				if (st.overridden)
					st.pending_restore = true;
				log_once(st, e, false);
			}
		}

		void assert_toggle(int i, uint8_t* gamerend, uint8_t* drawenvman)
		{
			const auto& e = k_entries[i];
			auto& st = s_states[i];

			if (e.fields[0].r == root::filter_node)
			{
				assert_filter_node(i);
				return;
			}

			if (!st.render)
			{
				for (int f = 0; f < e.nfields; f++)
				{
					const auto& fld = e.fields[f];
					uint8_t* root_ptr = fld.r == root::gamerend ? gamerend
						: fld.r == root::drawenvman ? drawenvman : nullptr;
					if ((fld.r == root::gamerend || fld.r == root::drawenvman) && !root_ptr)
						continue;

					uint8_t* addr = field_address(fld, root_ptr);

					// A code site carrying neither the stock nor the kill bytes is
					// not the instruction this patch was built for — leave it alone.
					if (fld.r == root::code)
					{
						const uint32_t cur = read_field(fld, addr);
						if (cur != fld.stock && cur != fld.kill)
							continue;
					}

					// (Re)capture the pre-kill value when first overriding or when
					// the owning object was recreated (fresh ctor defaults).
					if (!st.overridden || st.capture_root[f] != root_ptr)
					{
						const uint32_t cur = read_field(fld, addr);
						if (cur != fld.kill)
							st.original[f] = cur;
						else if (!st.overridden)
							st.original[f] = fld.r == root::code ? fld.stock : (fld.kill ? 0 : 1);
						st.capture_root[f] = root_ptr;
					}

					if (read_field(fld, addr) != fld.kill)
						write_field(fld, addr, fld.kill);
				}
				st.overridden = true;
				log_once(st, e, true);
			}
			else if (st.overridden)
			{
				// Restore only when the owning object is the one we captured from;
				// a recreated object already carries fresh ctor defaults.
				for (int f = 0; f < e.nfields; f++)
				{
					const auto& fld = e.fields[f];
					uint8_t* root_ptr = fld.r == root::gamerend ? gamerend
						: fld.r == root::drawenvman ? drawenvman : nullptr;
					if ((fld.r == root::gamerend || fld.r == root::drawenvman) &&
						(!root_ptr || root_ptr != st.capture_root[f]))
						continue;
					write_field(fld, field_address(fld, root_ptr), st.original[f]);
				}
				st.overridden = false;
				log_once(st, e, false);
			}
		}
	}

	const toggle_desc& describe(toggle t)
	{
		return k_entries[static_cast<int>(t)].desc;
	}

	bool available() { return s_available; }

	bool get_render(toggle t)
	{
		return s_states[static_cast<int>(t)].render;
	}

	void set_render(toggle t, bool render)
	{
		s_states[static_cast<int>(t)].render = render;
	}

	int shadow_map_size()
	{
		if (shared::globals::lookdev_host) return 0; // callers fall back to the stock 2048
		return static_cast<int>(*reinterpret_cast<uint32_t*>(rebase(SHADOWMAP_SIZE_VA)));
	}

	void set_shadow_map_size(int size)
	{
		s_desired_shadow_map_size = size;
	}

	int shadow_cascade_count()
	{
		auto* dem = resolve_drawenvman();
		return dem ? *reinterpret_cast<int*>(dem + 0x30) : 0;
	}

	int lod_bias()
	{
		auto* gr = resolve_gamerend();
		return gr ? *reinterpret_cast<int*>(gr + 0x290) : 0;
	}

	void set_lod_bias(int bias)
	{
		s_desired_lod_bias = bias;
	}

	void init_from_config()
	{
		if (s_config_seeded)
			return;
		s_config_seeded = true;

		auto& cfg = shared::common::config::get();
		for (int i = 0; i < static_cast<int>(toggle::count); i++)
			s_states[i].render = cfg.get_bool("Perf", k_entries[i].ini_key, true);

		// 0 = stock size (2048). The atlas is created from this value, so a
		// changed size takes effect when the game (re)creates it — area load.
		s_desired_shadow_map_size = cfg.get_int("Perf", "ShadowMapSize", 0);
	}

	void on_present()
	{
		// The toggles patch and write the game's scheduler: nothing to act on in the host.
		if (shared::globals::lookdev_host)
			return;

		init_from_config();
		install_stage_mask_hook();

		// Reopen the once-per-frame emission gate for the detour.
		InterlockedExchange(&s_emitted_this_frame, 0);

		auto* gamerend = resolve_gamerend();
		auto* drawenvman = resolve_drawenvman();
		s_available = gamerend != nullptr && drawenvman != nullptr;

		for (int i = 0; i < static_cast<int>(toggle::count); i++)
			assert_toggle(i, gamerend, drawenvman);

		if (s_desired_shadow_map_size >= 512 && s_desired_shadow_map_size <= 2048)
		{
			auto* addr = reinterpret_cast<uint32_t*>(rebase(SHADOWMAP_SIZE_VA));
			if (!s_original_shadow_map_size)
				s_original_shadow_map_size = *addr;
			if (*addr != static_cast<uint32_t>(s_desired_shadow_map_size))
				*addr = static_cast<uint32_t>(s_desired_shadow_map_size);
		}
		else if (s_original_shadow_map_size)
		{
			*reinterpret_cast<uint32_t*>(rebase(SHADOWMAP_SIZE_VA)) = s_original_shadow_map_size;
			s_original_shadow_map_size = 0;
		}

		if (gamerend && s_desired_lod_bias != LOD_BIAS_STOCK)
			*reinterpret_cast<int*>(gamerend + 0x290) = s_desired_lod_bias;
	}
}
