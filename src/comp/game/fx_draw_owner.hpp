#pragma once

// Which effect instance is drawing its particles right now.
//
// The game draws every particle cluster of an effect through one of nine host draw functions (the FrpgFx draw entity hosts:
// billboards, multi-texture billboards, point sprites, tracers and the rest; vtables at 0x11E7310..0x11E73B0). Each is
// `void __thiscall Draw(host, context, cluster, arg)`: it fills the vertices of the cluster's live particles and hands them to the
// device (DrawPrimitiveUP / DrawIndexedPrimitiveUP through 0x431B20 / 0x431250) before it returns, on the thread that called it.
// The cluster's first word is the FFX instance that owns it (the billboard draw at 0x551BB0 reads [[cluster] + 0xA8], the
// instance's group). So a wrapper round those nine says, for every sprite batch the proxy sees, which effect instance it
// belongs to: the link between an effect's light and the sprites it draws, with no guessing by place or texture.
//
// In the lookdev host there is no game: the capture carries the owner with each sprite draw (REC_GAME_FX_OWNER) and
// set_host_owner puts it here.
namespace comp::game::fx_draw_owner
{
	// Game: wraps the nine draw functions (before MH_EnableHook(MH_ALL_HOOKS)). Nothing in the lookdev host, or when the
	// functions are not where they should be (another executable).
	void install();

	// The effect instance whose particles are being drawn on this thread, 0 outside a particle draw. Not validated: check it
	// against ffx_instances::current() before use (a cluster kind whose first word is something else gives a stray value).
	unsigned current();

	// Host: the owner of the sprite draws that follow, from the capture.
	void set_host_owner(unsigned owner);

	// How many particle draws were wrapped since the start (for the log).
	unsigned long long draws_seen();
}
