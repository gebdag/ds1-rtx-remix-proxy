# Moving-frame tests

A capture replays one frozen frame, so it cannot show what changes between frames. These play a capture into a live host through the
game's stream, with no game running, and change it every frame the way the game does.

`warpsim.py <seconds per capture> --days N --match <name>`
- `--move M --spawn R`: 48-byte sprite batches as the game's are: vertices in fixed slots, each quad moving and fading from alpha 0 and
  back, and the index list rewritten every frame with the living quads sorted by depth.
- `--churn N`: bone palettes and sprite vertices change, so geometry hashes as new each frame.

`warpwatch.py <tag> <seconds per capture> [warpsim args]`: starts a live host and the simulator; output next to the script in `warp_<tag>`.

`flick.py <tag> label="settings" ...` (host and simulator running): applies each set of live options and measures frame-to-frame
flicker in the host window. A flip is a pixel going one way and straight back; a moving sprite changes pixels but does not flip them.

`focus_test.py minimise <seconds> <count>`: minimises and restores the host window.

Two findings came from these and from nothing else: a minimised host dying on restore (`warpwatch.py t 400 --match <capture> --churn 200`,
then `focus_test.py minimise 45 1`), and sprites flickering from opacity micromaps (`--move 0.08 --spawn 0.15`, flips 1.58 against 0.18).
Any change to effect handling needs a check here, see analysis/effects_ground_truth.md invariant I15.
