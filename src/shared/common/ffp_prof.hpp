#pragma once

namespace shared::common
{
	/*
	 * Per-step CPU profiler and kill switches for the FFP conversion pipeline.
	 *
	 * Why this exists. Converting one draw to fixed-function replaces a single
	 * DrawIndexedPrimitive with roughly sixty D3D9 calls plus a full CPU rebuild
	 * of that draw's vertices into a proxy-owned vertex buffer. Over the Remix
	 * bridge the rebuilt buffer is memcpy'd into the IPC channel on every Unlock
	 * and re-uploaded by the runtime (D3D9Rtx::processVertices sees NeedsUpload
	 * and calls FlushBuffer), so a step that looks free on paper can dominate the
	 * frame. Guessing which one is hopeless; this measures it.
	 *
	 * Three kinds of evidence, because no single one is conclusive:
	 *   - self ms/frame  : CPU burned inside the proxy for that step.
	 *   - calls/frame    : how often the step runs (a cheap step run 3000 times
	 *                      is a different problem from an expensive rare one).
	 *   - MB/frame       : bytes handed to the bridge. The bridge is async, so
	 *                      server-side and GPU cost do NOT show up in the ms
	 *                      column — traffic volume is the proxy-side proxy for it.
	 *
	 * The [FFPSteps] toggles then let each step be switched off live so its real
	 * cost can be read off the frame rate rather than inferred from the timer.
	 *
	 * Timing is exclusive (self time): a nested scope's elapsed time is
	 * subtracted from its parent, so the column sums to the total instead of
	 * double-counting engage/expand against their children.
	 */
	namespace ffp_prof
	{
		enum class step : int
		{
			route = 0,      // perf classify + dedup bookkeeping, before routing
			zwrite_query,   // sync_zwrite (D3DRS_ZWRITEENABLE read-back)
			engage,         // engage() itself, minus its children below
			transforms,     //   apply_transforms  (up to 3 SetTransform)
			tex_stages,     //   setup_texture_stages (29 SetTextureStageState)
			albedo,         // setup_albedo_or_fallback / water (8 SetTexture)
			expand,         // expand_rigid_texcoords, minus its children below
			exp_lock,       //   GetDesc + source Lock + destination Lock
			exp_convert,    //   the CPU vertex conversion loop
			exp_unlock,     //   the Unlock pair — where the bridge upload happens
			exp_bind,       //   SetVertexDeclaration + SetStreamSource
			decal_query,    // apply_blended_decal_marker (3 GetRenderState)
			draw_base,      // the base Draw(Indexed)Primitive
			overlay_save,   // save_overlay_render_states (7 GetRenderState)
			overlay_setup,  // setup_overlay_decal_pass (2nd expansion + state)
			overlay_draw,   // the overlay Draw(Indexed)Primitive
			restore_tex,    // restore_textures (8 SetTexture)
			restore_stream, // restore_rigid_source
			disengage,      // disengage()
			ctx_restore,    // drawcall_mod_context::restore_all
			count
		};

		constexpr int STEP_COUNT = static_cast<int>(step::count);

		const char* label(step s);
		const char* describe(step s);
		// True when the step is a child of the preceding parent step (UI indent).
		bool is_child(step s);

		bool profiling();
		void set_profiling(bool e);

		/*
		 * RAII exclusive-time scope. When profiling is off this is a single bool
		 * test and two stores, so the scopes can stay in the hot path unguarded.
		 */
		class scope
		{
		public:
			explicit scope(step s);
			~scope();
			scope(const scope&) = delete;
			scope& operator=(const scope&) = delete;

		private:
			step step_;
			long long start_ = 0;
			long long saved_child_ = 0;
			bool active_ = false;
		};

		// Draw accounting — a converted draw is one that took the full FFP path.
		void count_converted_draw();
		void count_passthrough_draw();

		// Bytes written into a proxy-owned VB and handed to the bridge on Unlock.
		void add_expand_bytes(unsigned int bytes);
		// Bytes a cache hit avoided sending (ExpandReuse).
		void add_saved_bytes(unsigned int bytes);
		// A source range whose fingerprint matched the cached expansion.
		void count_expand_reuse_hit();
		void count_expand_reuse_miss();

		void on_present();
		void reset();

		// --- read-back for the UI (all smoothed per-frame values) ---
		double       self_ms(step s);
		unsigned int calls(step s);
		double       total_ms();
		double       frame_ms();
		unsigned int converted_draws();
		unsigned int passthrough_draws();
		double       expand_mb();
		double       saved_mb();
		unsigned int reuse_hits();
		unsigned int reuse_misses();
	}
}
