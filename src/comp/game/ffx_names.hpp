#pragma once

namespace comp::game::ffx_names
{
	// Readable names of the game's FFX effects, from rtx_comp\ffx_names.tsv (one "id<TAB>name" per line,
	// generated from the effect catalogue: Vibe-Reverse-Engineering\patches\DarkSouls\analysis\
	// ffx_catalogue.csv). Loaded on first use.
	//
	// The name of an effect, or nullptr when the table has none.
	const char* name(int ffx_id);
}
