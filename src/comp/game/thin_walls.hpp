#pragma once

namespace comp::game::thin_walls
{
	// Some DS1 map meshes build a wall from two single-sided faces a few centimetres apart whose fronts
	// face each other (Blighttown's cave walls, m14_b_wall_01: 0.27 m apart, each face's front towards
	// the other). The game culls whichever face is seen from behind, and its point lights cast no
	// shadows, so the wall looks right from both sides. Ray traced, the face seen from behind still
	// shadows the one in front of it from every light, and both sides of the wall go black.
	//
	// Such a mesh is drawn double-sided: from either side the near face is then the one hit (from
	// behind, its normal turned towards the viewer), and nothing lies between it and the lights on that
	// side, while the far face still shadows the lights on the other side.

	// True when the indexed draw's mesh has such facing pairs over a sizeable share of its area. The
	// first call for a mesh queues its measurement (a few meshes per frame read their buffers); until
	// then, and for meshes it cannot read, it returns false. Positions are FLOAT3 object space, fronts
	// come from the vertex normals (UBYTE4N or FLOAT3).
	bool is_inside_out(IDirect3DVertexBuffer9* vb, UINT vb_offset, UINT stride, IDirect3DVertexDeclaration9* decl,
		IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE prim, INT base_vertex, UINT start_index, UINT prim_count,
		unsigned int frame);

	// A new index buffer can reuse a freed one's address: forget its meshes.
	void on_index_buffer_created(IDirect3DIndexBuffer9* ib);
}
