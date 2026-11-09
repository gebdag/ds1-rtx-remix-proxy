#pragma once

#include "shared/common/ffp_state.hpp"

namespace comp::game::hit_sparks
{
	// The game's hits this frame, read from its live effects: every new instance of an effect that throws warm streaks is one hit,
	// with the streak count, colour and speed its own FFX file gives (hit_spark_table.inc). ffp_state turns these into Remix particles.
	void collect(std::vector<shared::common::ffp_state::spark_emission>& out);
}
