#include "std_include.hpp"
#include "point_lights.hpp"
#include "fx_owner.hpp"
#include "host_feed.hpp"
#include "light_dump.hpp"

#include <algorithm>
#include <cmath>

namespace comp::game::point_lights
{
	// NS_FRPG::KPointLightMan singleton (VA 0x0137D938). Slots start at +0x20 and are
	// 0x30 bytes each, capacity 512 (KPointLightMan_CreatePointLight, VA 0x00D0D8A0).
	//
	// KPointLightMan_InitSlot (VA 0x00FC7CE0) fills a slot with:
	//   +0x10  float4 world position
	//   +0x20  LIGHT_BANK row, already resolved to an absolute pointer
	//   +0x24  DLRF entity handle — non-zero while the slot is live
	//
	// +0x24 is a HANDLE, not a PointLightEntity*: InitSlot stores the return of the
	// seed creator at 0x00F91F60 (-> 0x00BAF530) there, and the engine's own consumer
	// FUN_00f6d000 resolves such references as (handle & 0xFFFFFF) indexed into a
	// registry before it may call vtable slot 12. Dereferencing it directly yields
	// garbage, so light parameters come from the resolved LIGHT_BANK row instead.
	static constexpr DWORD POINT_LIGHT_MAN_RVA = 0x00F7D938;
	static constexpr DWORD SLOT_ARRAY_OFFSET = 0x20;
	static constexpr DWORD SLOT_STRIDE = 0x30;
	static constexpr int SLOT_CAPACITY = 512;
	static constexpr DWORD SLOT_POSITION = 0x10;
	static constexpr DWORD SLOT_PARAM_ROW = 0x20;
	static constexpr DWORD SLOT_ENTITY_HANDLE = 0x24;

	// Handle -> entity, the way the renderer itself does it in the point-light constant
	// funnel (VA 0x00F6D000):
	//
	//   and  eax, 0xffffff        ; index = handle & 0x00FFFFFF (handles have bit 31 set)
	//   mov  edx, [edi + 4]       ; edi = draw context
	//   mov  eax, [edx + 0x18]    ; HGEntityManager
	//   mov  edx, [eax + 4]       ; entity pointer table
	//   mov  ecx, [edx + ecx*4]   ; the entity
	//   call [[ecx] + 0x30]       ; GetLightParams(out 0x20 bytes)
	//
	// The draw context is not reachable from a global, but the same manager is: both slot
	// initialisers hand [DAT_013783C0 + 0x0C] to the seed creator that mints the handle
	// (VA 0x00FC7C00 direct lights, 0x00FC7CE0 bank lights, creator 0x00F91E10 / 0x00F91F60),
	// and that creator walks ctx -> [ctx+4] -> [*] -> +0x18 to reach the very same
	// HGEntityManager. So the manager can be found without hooking anything.
	static constexpr DWORD HG_GLOBAL_RVA = 0x00F783C0;   // VA 0x013783C0
	static constexpr DWORD HG_ENTITY_CTX = 0x0C;
	static constexpr DWORD CTX_HOLDER = 0x04;
	static constexpr DWORD HOLDER_MANAGER = 0x18;
	static constexpr DWORD MANAGER_TABLE = 0x04;
	static constexpr DWORD HANDLE_INDEX_MASK = 0x00FFFFFF;
	static constexpr DWORD VTABLE_GET_LIGHT_PARAMS = 0x30;   // slot 12

	// RTTI-confirmed vftables of the three concrete PointLightEntity subclasses. A
	// resolved pointer is only trusted when it carries one of them, which makes a stale
	// or recycled handle harmless instead of a wild call.
	static constexpr DWORD VTABLE_BANK_RVA = 0x00DAB110;       // NS_FRPG::BankPointLightEntity
	static constexpr DWORD VTABLE_DIRECT_RVA = 0x00DAB24C;     // NS_FRPG::DirectPointLightEntity
	static constexpr DWORD VTABLE_LERP_BANK_RVA = 0x00DAB394;  // NS_FRPG::LerpBankPointLightEntity

	// What GetLightParams writes: two float4 shader registers, position + colour.
	struct resolved_params
	{
		float x, y, z;
		float inv_dwindle_range;   // 1 / (dwindleEnd - dwindleBegin), clamped to 1e5f
		float r, g, b;
		float dwindle_end;
	};

	using get_light_params_fn = void(__thiscall*)(void* self, resolved_params* out);

	// LIGHT_BANK row layout, from the evaluator at VA 0x00FC83D0. Colour components are
	// 0-255 shorts scaled by a percentage, which is why a raw read looks washed out.
	static constexpr DWORD ROW_DWINDLE_BEGIN = 0x00;
	static constexpr DWORD ROW_DWINDLE_END = 0x04;
	static constexpr DWORD ROW_COLOR_R = 0x08;
	static constexpr DWORD ROW_COLOR_G = 0x0A;
	static constexpr DWORD ROW_COLOR_B = 0x0C;
	static constexpr DWORD ROW_INTENSITY_PCT = 0x0E;

	static std::vector<light> s_lights;
	static int s_slots_live = 0;
	static int s_slots_with_params = 0;
	static int s_from_bank = 0;
	static int s_from_entity = 0;
	static int s_unresolved = 0;
	static bool s_resolve_entities = true;

	static DWORD safe_read_ptr(DWORD address)
	{
		if (!address) return 0;
		__try {
			return *reinterpret_cast<DWORD*>(address);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
	}

	static bool read_position(DWORD slot, light& out)
	{
		__try {
			const auto* position = reinterpret_cast<const float*>(slot + SLOT_POSITION);
			out.x = position[0];
			out.y = position[1];
			out.z = position[2];
			return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	static bool read_params(DWORD row, light& out)
	{
		__try {
			const auto* bytes = reinterpret_cast<const unsigned char*>(row);
			const float begin = *reinterpret_cast<const float*>(bytes + ROW_DWINDLE_BEGIN);
			const float end = *reinterpret_cast<const float*>(bytes + ROW_DWINDLE_END);
			const short red = *reinterpret_cast<const short*>(bytes + ROW_COLOR_R);
			const short green = *reinterpret_cast<const short*>(bytes + ROW_COLOR_G);
			const short blue = *reinterpret_cast<const short*>(bytes + ROW_COLOR_B);
			const short intensity_pct = *reinterpret_cast<const short*>(bytes + ROW_INTENSITY_PCT);

			if (!std::isfinite(begin) || !std::isfinite(end) || end <= 0.0f)
			{
				return false;
			}

			const float intensity = static_cast<float>(intensity_pct) * 0.01f;
			out.r = (static_cast<float>(red) / 255.0f) * intensity;
			out.g = (static_cast<float>(green) / 255.0f) * intensity;
			out.b = (static_cast<float>(blue) / 255.0f) * intensity;
			out.dwindle_begin = begin;
			out.dwindle_end = end;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// The entity pointer table, or 0 before the world (and its HG system) exists.
	// Candidate walks from the HG global to the entity pointer table.
	//
	// The two call sites disagree on depth. The renderer's resolver (0x00F6D000) reaches
	// the manager from its per-draw context in two dereferences —
	//   mov edx,[edi+4] / mov eax,[edx+0x18] / mov edx,[eax+4]
	// — while the seed creator (0x00F91F60), reached from [DAT_013783C0+0x0C], takes three:
	//   mov eax,[esi+4] / mov ecx,[eax] / mov eax,[ecx+0x18]
	// Whether those two contexts are the same object one level apart is not decidable from
	// the disassembly, and the slot initialisers use both [hg+0x08] and [hg+0x0C]. So the
	// walk is picked by validation rather than assumption: the right table is the one whose
	// entries, for the handles the light manager is actually holding, are objects carrying
	// one of the three RTTI-confirmed PointLightEntity vftables. Nothing else in the process
	// would satisfy that for several independent handles at once.
	struct table_walk
	{
		const char* name;
		DWORD ctx_offset;   // field of the HG global holding the context
		int depth;          // dereferences between the context and the manager
	};

	static constexpr table_walk WALKS[] = {
		{ "hg+0x0C d3", 0x0C, 3 },
		{ "hg+0x0C d2", 0x0C, 2 },
		{ "hg+0x0C d1", 0x0C, 1 },
		{ "hg+0x08 d3", 0x08, 3 },
		{ "hg+0x08 d2", 0x08, 2 },
		{ "hg+0x08 d1", 0x08, 1 },
	};
	static constexpr int WALK_COUNT = static_cast<int>(sizeof(WALKS) / sizeof(WALKS[0]));

	static int s_walk_scores[WALK_COUNT] = {};
	static int s_walk_chosen = -1;

	static DWORD walk_to_table(const table_walk& walk)
	{
		const DWORD hg = safe_read_ptr(EXE_BASE + HG_GLOBAL_RVA);
		if (!hg) return 0;
		const DWORD ctx = safe_read_ptr(hg + walk.ctx_offset);
		if (!ctx) return 0;

		DWORD manager = 0;
		switch (walk.depth)
		{
		case 3: manager = safe_read_ptr(safe_read_ptr(safe_read_ptr(ctx + CTX_HOLDER)) + HOLDER_MANAGER); break;
		case 2: manager = safe_read_ptr(safe_read_ptr(ctx + CTX_HOLDER) + HOLDER_MANAGER); break;
		default: manager = safe_read_ptr(ctx + HOLDER_MANAGER); break;
		}
		if (!manager) return 0;
		return safe_read_ptr(manager + MANAGER_TABLE);
	}

	// Light vtables are DARKSOULS.exe addresses, and the exe has no ASLR: it always loads at its
	// image base. EXE_BASE is the running module, which in the lookdev host is the host exe (relocated),
	// so comparing a captured vtable against it classified every light from a capture as unresolved.
	constexpr DWORD kGameImageBase = 0x00400000;

	static bool is_light_vtable(DWORD vtable)
	{
		return vtable == kGameImageBase + VTABLE_BANK_RVA
			|| vtable == kGameImageBase + VTABLE_DIRECT_RVA
			|| vtable == kGameImageBase + VTABLE_LERP_BANK_RVA;
	}

	const char* entity_class_name(unsigned vtable)
	{
		if (vtable == kGameImageBase + VTABLE_BANK_RVA) return "Bank";
		if (vtable == kGameImageBase + VTABLE_DIRECT_RVA) return "Direct";
		if (vtable == kGameImageBase + VTABLE_LERP_BANK_RVA) return "LerpBank";
		return "?";
	}

	// handle -> entity, rejecting anything that is not one of the three light classes.
	static DWORD resolve_entity(DWORD table, DWORD handle, DWORD& out_vtable)
	{
		out_vtable = 0;
		if (!table || !handle) return 0;

		// Live handles have bit 31 set; the low 24 bits index the table. A value without
		// the top bit is not a handle the renderer would follow either.
		if (!(handle & 0x80000000)) return 0;

		const DWORD entity = safe_read_ptr(table + (handle & HANDLE_INDEX_MASK) * 4);
		if (entity < 0x10000) return 0;

		const DWORD vtable = safe_read_ptr(entity);
		if (!is_light_vtable(vtable)) return 0;

		out_vtable = vtable;
		return entity;
	}

	// How many of these handles land on a real light entity through this table.
	static int score_table(DWORD table, const std::vector<DWORD>& handles)
	{
		if (!table) return 0;
		int hits = 0;
		for (const DWORD handle : handles)
		{
			DWORD vtable = 0;
			if (resolve_entity(table, handle, vtable)) ++hits;
		}
		return hits;
	}

	// Score every candidate walk against the handles this frame is actually holding and
	// keep the best. Re-scored each harvest: the manager is rebuilt on world load, and a
	// walk that stops validating must not keep being trusted.
	static DWORD choose_table(const std::vector<DWORD>& handles)
	{
		s_walk_chosen = -1;
		if (handles.empty()) return 0;

		DWORD best_table = 0;
		int best_score = 0;
		for (int i = 0; i < WALK_COUNT; ++i)
		{
			const DWORD table = walk_to_table(WALKS[i]);
			s_walk_scores[i] = score_table(table, handles);
			if (s_walk_scores[i] > best_score)
			{
				best_score = s_walk_scores[i];
				best_table = table;
				s_walk_chosen = i;
			}
		}
		return best_table;
	}

	// Ask the entity for its own resolved record. This is the accessor the renderer
	// calls, so bank rows, colour-power scaling and lerp state are already applied —
	// and unlike the slot's row pointer it also answers for Direct (FX/SFX) lights,
	// which never carry a row at all.
	static bool read_entity_params(DWORD entity, DWORD vtable, light& out)
	{
		resolved_params params = {};
		__try {
			const auto* vt = reinterpret_cast<DWORD*>(vtable);
			const auto fn = reinterpret_cast<get_light_params_fn>(vt[VTABLE_GET_LIGHT_PARAMS / 4]);
			fn(reinterpret_cast<void*>(entity), &params);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}

		if (!std::isfinite(params.dwindle_end) || params.dwindle_end <= 0.0f) return false;
		if (!std::isfinite(params.inv_dwindle_range) || params.inv_dwindle_range <= 0.0f) return false;
		if (!std::isfinite(params.r) || !std::isfinite(params.g) || !std::isfinite(params.b)) return false;

		// GetLightParams hands back 1/(end - begin); invert it to recover the inner radius.
		const float range = 1.0f / params.inv_dwindle_range;
		out.dwindle_end = params.dwindle_end;
		out.dwindle_begin = std::max(0.0f, params.dwindle_end - range);
		out.r = params.r;
		out.g = params.g;
		out.b = params.b;
		return true;
	}

	bool is_available()
	{
		if (shared::globals::lookdev_host)
			return !host_feed::lights().empty();
		return safe_read_ptr(EXE_BASE + POINT_LIGHT_MAN_RVA) != 0;
	}

	void set_resolve_entities(bool on) { s_resolve_entities = on; }
	bool resolve_entities() { return s_resolve_entities; }

	int slots_live() { return s_slots_live; }
	int slots_with_params() { return s_slots_with_params; }
	int slots_from_bank() { return s_from_bank; }
	int slots_from_entity() { return s_from_entity; }
	int slots_unresolved() { return s_unresolved; }

	std::string chain_diagnostics()
	{
		std::string text;
		const DWORD hg = safe_read_ptr(EXE_BASE + HG_GLOBAL_RVA);
		text += std::format("HG global (0x{:08X}) -> 0x{:08X}\n", EXE_BASE + HG_GLOBAL_RVA, hg);

		for (int i = 0; i < WALK_COUNT; ++i)
		{
			text += std::format("  {:<12} table 0x{:08X}  validated {} handle(s){}\n",
				WALKS[i].name, walk_to_table(WALKS[i]), s_walk_scores[i],
				i == s_walk_chosen ? "   <- chosen" : "");
		}

		if (s_walk_chosen < 0)
		{
			if (s_slots_live == 0)
				text += "  the light manager holds no slots at all here\n";
			else if (s_unresolved == 0)
				text += "  nothing to validate: every live slot already had a bank row\n";
			else
				text += "  NO walk validated: every candidate table failed the vftable check\n";
		}
		return text;
	}

	static const std::vector<light>& harvest_impl()
	{
		if (shared::globals::lookdev_host)
			return host_feed::lights();   // the game's harvest of this frame, from the capture

		s_lights.clear();
		s_slots_live = 0;
		s_slots_with_params = 0;
		s_from_bank = 0;
		s_from_entity = 0;
		s_unresolved = 0;

		const DWORD manager = safe_read_ptr(EXE_BASE + POINT_LIGHT_MAN_RVA);
		if (!manager)
		{
			return s_lights;
		}

		// Pass 1: every live slot, with whatever the slot itself can answer.
		std::vector<DWORD> rowless_handles;
		for (int i = 0; i < SLOT_CAPACITY; ++i)
		{
			const DWORD slot = manager + SLOT_ARRAY_OFFSET + i * SLOT_STRIDE;
			const DWORD handle = safe_read_ptr(slot + SLOT_ENTITY_HANDLE);
			if (!handle)
			{
				continue;
			}

			++s_slots_live;

			light out = {};
			out.slot = i;
			out.entity_handle = handle;
			out.source = param_source::none;

			// The slot position is written synchronously by whoever owns the light
			// (the FX host, ChrPointLightSlot, the map light manager), whereas the
			// entity's own position arrives through a queued render command and can
			// lag a frame — so the slot always wins for position.
			if (!read_position(slot, out))
			{
				continue;
			}

			const DWORD row = safe_read_ptr(slot + SLOT_PARAM_ROW);
			if (row && read_params(row, out))
			{
				out.param_row = row;
				out.source = param_source::bank_row;
				out.has_params = true;
				++s_from_bank;
				++s_slots_with_params;
			}
			else
			{
				// Direct lights — the class the SFX system uses — set slot+0x20 to zero
				// (VA 0x00FC7C00) and keep their colour and falloff on the entity, so
				// they are only reachable through the handle.
				rowless_handles.push_back(handle);
			}

			s_lights.push_back(out);
		}

		// Pass 2: the walk to the entity table is chosen by validating it against the
		// handles collected above, so it needs them first.
		if (!s_resolve_entities || rowless_handles.empty())
		{
			s_unresolved = static_cast<int>(rowless_handles.size());
			return s_lights;
		}

		const DWORD table = choose_table(rowless_handles);
		for (auto& out : s_lights)
		{
			if (out.source != param_source::none)
			{
				continue;
			}

			DWORD vtable = 0;
			const DWORD entity = resolve_entity(table, out.entity_handle, vtable);
			if (entity && read_entity_params(entity, vtable, out))
			{
				out.entity = entity;
				out.entity_vtable = vtable;
				out.source = param_source::entity;
				out.has_params = true;
				++s_from_entity;
				++s_slots_with_params;
			}
			else
			{
				++s_unresolved;
			}
		}

		fx_owner::resolve(s_lights);
		return s_lights;
	}

	// Raw row fields, straight off the param row, so a dark light can be told apart
	// from a missing one: the evaluator at 0x00FC83D0 turns these into the colour.
	static bool read_raw_row(DWORD row, float& begin, float& end, short rgb[3], short& intensity)
	{
		__try {
			const auto* bytes = reinterpret_cast<const unsigned char*>(row);
			begin = *reinterpret_cast<const float*>(bytes + ROW_DWINDLE_BEGIN);
			end = *reinterpret_cast<const float*>(bytes + ROW_DWINDLE_END);
			rgb[0] = *reinterpret_cast<const short*>(bytes + ROW_COLOR_R);
			rgb[1] = *reinterpret_cast<const short*>(bytes + ROW_COLOR_G);
			rgb[2] = *reinterpret_cast<const short*>(bytes + ROW_COLOR_B);
			intensity = *reinterpret_cast<const short*>(bytes + ROW_INTENSITY_PCT);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// SEH needs functions without C++ objects to unwind: the dump's two reads.
	static bool copy_bytes(DWORD address, void* out, size_t size)
	{
		__try { memcpy(out, reinterpret_cast<const void*>(address), size); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	static bool call_light_params(DWORD entity, DWORD vtable, resolved_params& p)
	{
		__try {
			const auto* vt = reinterpret_cast<DWORD*>(vtable);
			reinterpret_cast<get_light_params_fn>(vt[VTABLE_GET_LIGHT_PARAMS / 4])(reinterpret_cast<void*>(entity), &p);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// F6 light dump (light_dump.hpp): the manager's slots as they are in memory, every one with any non-zero byte (harvest
	// skips slots without a handle or with a non-finite position), with the bank row's raw fields and the entity's own
	// GetLightParams answer; then what harvest returned.
	static void dump_slots_and_harvest()
	{
		if (shared::globals::lookdev_host)
		{
			light_dump::write(std::format("SLOT lookdev host: harvest is the capture's ({} lights)", s_lights.size()));
		}
		else
		{
			const DWORD manager = safe_read_ptr(EXE_BASE + POINT_LIGHT_MAN_RVA);
			const DWORD table = s_walk_chosen >= 0 ? walk_to_table(WALKS[s_walk_chosen]) : 0;
			light_dump::write(std::format("SLOT manager 0x{:08X} entity_table 0x{:08X} walk {} live {} bank {} entity {} unresolved {} resolve_entities {}",
				manager, table, s_walk_chosen >= 0 ? WALKS[s_walk_chosen].name : "-", s_slots_live, s_from_bank, s_from_entity, s_unresolved,
				s_resolve_entities ? 1 : 0));
			for (int i = 0; manager && i < SLOT_CAPACITY; ++i)
			{
				DWORD raw[SLOT_STRIDE / 4] = {};
				const bool readable = copy_bytes(manager + SLOT_ARRAY_OFFSET + i * SLOT_STRIDE, raw, sizeof(raw));
				bool any = !readable;
				for (const DWORD d : raw)
					any = any || d != 0;
				if (!any)
					continue;
				std::string s = std::format("SLOT {} {}raw", i, readable ? "" : "UNREADABLE ");
				for (const DWORD d : raw)
					s += std::format(" {:08X}", d);
				float pos[4];
				memcpy(pos, &raw[SLOT_POSITION / 4], sizeof(pos));
				s += std::format(" | pos ({:.6g}, {:.6g}, {:.6g}, {:.6g}) row 0x{:08X} handle 0x{:08X}", pos[0], pos[1], pos[2], pos[3],
					raw[SLOT_PARAM_ROW / 4], raw[SLOT_ENTITY_HANDLE / 4]);
				if (const DWORD row = raw[SLOT_PARAM_ROW / 4])
				{
					float begin = 0.0f, end = 0.0f;
					short rgb[3] = {}, intensity = 0;
					DWORD row_raw[8] = {};
					bool row_ok = read_raw_row(row, begin, end, rgb, intensity);
					row_ok = copy_bytes(row, row_raw, sizeof(row_raw)) && row_ok;
					s += row_ok ? std::format(" | row dwindle {:.6g}-{:.6g} rgb {} {} {} intensity {}% raw", begin, end, rgb[0], rgb[1], rgb[2], intensity)
						: std::string(" | row UNREADABLE");
					for (const DWORD d : row_raw)
						s += std::format(" {:08X}", d);
				}
				DWORD vtable = 0;
				if (const DWORD entity = resolve_entity(table, raw[SLOT_ENTITY_HANDLE / 4], vtable))
				{
					resolved_params p = {};
					const bool ok = call_light_params(entity, vtable, p);
					s += std::format(" | entity 0x{:08X} {} ", entity, entity_class_name(vtable));
					s += ok ? std::format("params pos ({:.6g}, {:.6g}, {:.6g}) inv_range {:.6g} rgb ({:.6g}, {:.6g}, {:.6g}) dwindle_end {:.6g}",
						p.x, p.y, p.z, p.inv_dwindle_range, p.r, p.g, p.b, p.dwindle_end) : std::string("params FAULT");
				}
				else
					s += " | entity -";
				light_dump::write(s);
			}
		}
		for (const auto& l : s_lights)
			light_dump::write(std::format("HARVEST slot {} pos ({:.6g}, {:.6g}, {:.6g}) rgb ({:.6g}, {:.6g}, {:.6g}) dwindle {:.6g}-{:.6g} has_params {} source {}"
				" handle 0x{:08X} entity 0x{:08X} class {} row 0x{:08X} ffx {} top {}",
				l.slot, l.x, l.y, l.z, l.r, l.g, l.b, l.dwindle_begin, l.dwindle_end, l.has_params ? 1 : 0, static_cast<int>(l.source),
				l.entity_handle, l.entity, entity_class_name(l.entity_vtable), l.param_row, l.ffx_id, l.ffx_top_id));
	}

	const std::vector<light>& harvest()
	{
		const auto& lights = harvest_impl();
		if (light_dump::active())
		{
			light_dump::frame_header();
			dump_slots_and_harvest();
		}
		return lights;
	}

	std::string dump_to_log()
	{
		const std::string path = shared::globals::root_path + "\\rtx_comp\\lights_dump.txt";
		FILE* f = fopen(path.c_str(), "a");
		if (!f)
		{
			return {};
		}

		const auto& lights = harvest();
		fprintf(f, "\n=== KPointLightMan harvest: %d live slots — %d from a bank row, "
			"%d from the entity, %d unresolved (entity resolution %s) ===\n",
			s_slots_live, s_from_bank, s_from_entity, s_unresolved,
			s_resolve_entities ? "on" : "off");
		fputs(chain_diagnostics().c_str(), f);
		fprintf(f, "slot  handle      entity      class     src   pos(x,z,y raw)                  "
			"rowRGB/intensity   colour(r,g,b)        dwindle        result\n");

		for (const auto& l : lights)
		{
			char raw[64] = "-";
			if (l.param_row)
			{
				float begin = 0.0f, end = 0.0f;
				short rgb[3] = {}, intensity = 0;
				if (read_raw_row(l.param_row, begin, end, rgb, intensity))
				{
					snprintf(raw, sizeof(raw), "%d,%d,%d @%d%%", rgb[0], rgb[1], rgb[2], intensity);
				}
			}

			const char* src = l.source == param_source::bank_row ? "row"
				: l.source == param_source::entity ? "ent" : "-";

			const char* result = !l.has_params
				? "UNRESOLVED - no row and the handle did not resolve"
				: (l.r <= 0.0f && l.g <= 0.0f && l.b <= 0.0f)
					? "resolved but BLACK (zero colour/intensity)"
					: "ok";

			fprintf(f, "%4d  0x%08X  0x%08X  %-8s  %-4s  %9.2f %9.2f %9.2f  %-18s "
				"%5.2f,%5.2f,%5.2f  %6.1f-%6.1f  %s\n",
				l.slot, l.entity_handle, l.entity, entity_class_name(l.entity_vtable), src,
				l.x, l.y, l.z, raw, l.r, l.g, l.b,
				l.dwindle_begin, l.dwindle_end, result);
		}

		fclose(f);
		return path;
	}
}
