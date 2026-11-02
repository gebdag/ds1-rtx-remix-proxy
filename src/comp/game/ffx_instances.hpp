#pragma once

namespace comp::game::ffx_instances
{
	// The game's live FFX effect instances, read from its effect tree (findings.db rows 256-263): every
	// group of both FFX::FXManagers, each instance with its children. One walk per frame at most; the
	// list is valid until the next frame (instances are freed after a frame on the managers' dead lists).
	struct instance
	{
		unsigned address;  // the FFX instance block
		unsigned user;     // its FrpgFx user object (instance + 0xB0), which holds the current draw host
		int ffx_id;        // the effect it runs (0 when its root is not a loaded FXR1 effect)
	};

	const std::vector<instance>& current();

	// The FFX id of the outermost effect above an instance (itself at the top level).
	int top_effect_id(unsigned address);

	// An instance's world position this frame, engine raw (x, up, depth) order: the translation of the
	// world matrix the FX update composes for it every frame (children against their parent's, so it is
	// absolute at any depth) and hands to its draw host, which places a point light at exactly this
	// point (findings.db rows 279-285). False when the instance is no longer valid.
	bool world_position(unsigned address, float* out);

	// How long an instance has run, in seconds (scaled by its group's time scale, as its draw host sees it).
	float age(unsigned address);

	// Whether an instance still has child effects at work. Its emitters are its child instances, removed as
	// they finish, so an instance can outlive what it shows by minutes.
	bool still_running(unsigned address);

	// How many instances the last walk visited.
	int last_walk_count();
}
