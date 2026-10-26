#pragma once

namespace comp::perf
{
	// Game-logic draw categories, classified per draw from the proxy's view:
	// bound render target (pass), vertex declaration (skinned), pixel shader
	// (water), and tracked render state (Z-write, alpha blend).
	enum class draw_cat : int
	{
		shadow_atlas = 0, // draws into the square shadow atlas RT
		reflection,       // 256x180 water reflection RT
		aux_halfres,      // 512x360 auxiliary half-res RT
		offscreen_other,  // any other offscreen RT (env cube, water mask, post chain)
		water,            // water-surface pixel shader draws
		skinned,          // skinned vertex declaration (characters, cloth, flags)
		prepass,          // main RT with Z-write ON (depth-building passes)
		translucent,      // main RT alpha-blended draws
		world_rigid,      // main RT rigid world geometry (Map + Obj groups)
		hud_geo,          // main RT draws without NORMAL (menu/frontend geometry)
		hud_quads,        // DrawIndexedPrimitiveUP menu quads (12-byte vertices)
		other,            // everything else
		count
	};

	const char* cat_label(draw_cat c);

	// Classify the pending DrawPrimitive/DrawIndexedPrimitive from current
	// ffp/rt state and count it. Returns true when the draw must be dropped
	// because its category filter is off.
	bool classify_and_filter();

	// HUD/menu quad path (DrawIndexedPrimitiveUP, 12-byte vertices, INDEX16,
	// TRIANGLEFAN 4v/2p or TRIANGLESTRIP 10v/8p — the watertight P0 predicate
	// from the 2026-07-26 HUD analysis). Counts the draw.
	struct hud_quad_result
	{
		bool is_hud = false; // matched the EzMenu draw signature
		bool drop = false;   // HUD rendering is toggled off
	};
	hud_quad_result filter_hud_quad(D3DPRIMITIVETYPE type, UINT num_verts, UINT prim_count,
		D3DFORMAT index_format, UINT stride);

	bool render_enabled(draw_cat c);
	void set_render(draw_cat c, bool render);

	UINT count_last(draw_cat c); // draws in this category last frame
	UINT total_last();           // all game draws last frame

	void on_present();           // roll over per-frame counters, advance probes
	void on_reset();             // drop the draw inspector's texture refs (they can block a device Reset)
	void tab_imgui();            // the F4 "Perf" tab

	// --- Draw-call inspector (Dev tab) ---
	//
	// Steps through the frame's draws one by one to identify useless ones.
	// The index counts every game draw in issue order at the device-wrapper
	// level (DIP, DP, DIPUP, DPUP), before any proxy routing or filtering.
	struct inspector_result
	{
		bool selected = false; // this draw is the inspected one
		bool drop = false;     // solo mode: kill this (non-selected) draw
		bool mark = false;     // selected + mark mode: swap in the placeholder texture
	};

	// Called from the device wrapper for every game draw, before routing.
	// Counts the draw, snapshots full pipeline state when it is the selected
	// one, and decides mark/solo handling.
	inspector_result inspector_on_draw(const char* call_type, D3DPRIMITIVETYPE prim_type,
		UINT prim_count, UINT num_verts, INT base_vtx, UINT start_idx, UINT stride);

	// Magenta/green checkerboard bound to all stages of a marked draw.
	IDirect3DBaseTexture9* inspector_placeholder_texture();

	void inspector_imgui();      // Dev tab UI (selector + info + log button)
}
