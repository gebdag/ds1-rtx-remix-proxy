#pragma once

namespace comp::hitch_watch
{
	// What the game was doing while a frame was late.
	//
	// The [Hitch] line (d3d9ex.cpp, Present) says how long a frame took and how much of it was Remix's Present; it cannot
	// say where the rest went. A watchdog thread looks at the time since the last Present; once that passes [Diagnostics]
	// HitchStackMs (50; 0 = off)it takes the call stack of the thread that presents (the game's render thread) and of the
	// thread that owns the game window, every 50 ms until the frame arrives, then logs them, equal stacks counted together:
	//
	//   [HitchStack] 412 ms without a Present, 7 + 7 samples
	//       render 6x: ntdll.dll+0x... < d3d9_remix.dll!... < DARKSOULS.exe:0x00F1A2B3 < ...
	//
	// DARKSOULS.exe addresses are absolute (the exe has no ASLR: a Ghidra address as it stands). The game's code keeps no
	// frame pointers, so a stack is read by scanning the thread's stack for return addresses: every value that points into a
	// module right after a call instruction. A frame's stale locals can add a wrong entry; the order is innermost first.
	// While a thread is suspended nothing is called that could want a lock it holds.
	//
	// Only with dev mode or [Troubleshooting] Verbose (config dev_logs): a player's game starts no watchdog.
	// draws: the draw calls of the frame being presented. A late frame after one with fewer than 50 (the game's start, the
	// title, a loading screen) is not sampled: those are the game's own waits.
	void on_present(unsigned draws);
}
