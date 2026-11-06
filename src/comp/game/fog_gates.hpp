#pragma once

namespace comp::game::fog_gates
{
	// Fog gates as a volume in the runtime (rtx.ds1FogGate.*, post_fx_dark_souls_fog_gate.h) instead of the game's sprite ribbons. A prototype.
	//
	// The game's own draws say where a gate is, how large and how much of it there is, so nothing about the 107 placements is stored. Its opaque quad (s00917) is the door; a gate
	// without one gets a slab fitted to its ribbons. The sum of the ribbons' vertex alpha, against the most this gate has shown, is how much fog there is. An area gate's effect fading
	// takes the volume with it. A boss gate holds. A gate the game removes (the boss is dead) dissolves. While the volume stands the ribbons are not drawn, and the quad is drawn with a
	// marker the camera's rays pass (Surface::isDs1CameraPasses): it still keeps light out of the room behind the door.

	// From the sprite path, for each batch: ffp_state::sprite_batch_fn.
	DWORD observe_batch(IDirect3DBaseTexture9* texture, const void* vertices, UINT stride, UINT position_offset, UINT colour_offset, UINT count);

	// Once a frame: strengths, and what the runtime needs to hear.
	void frame_update();
}
