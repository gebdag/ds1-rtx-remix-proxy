#pragma once

// Lookdev frame capture (.ds1cap): one presented frame, recorded exactly as the
// real d3d9 device received it, for the lookdev viewer to replay.
//
// Shared by the 32-bit proxy (writer) and the 64-bit viewer (reader), so every
// payload is packed, fixed-size and pointer-free. Object ids are the COM
// pointer values in the game process, widened to 64 bits; 0 means null.
//
// The stream is a sequence of { record_header, payload } in submission order.
// A resource's description record and its initial contents (flag INITIAL) are
// written right before the first record that references it; in-frame writes
// (Lock/Unlock, UpdateSurface sources) follow as non-initial data records.
// State records carry the device's effective state at each draw, read back
// from the device and written only when it changed, so state blocks and the
// proxy's own calls are covered without tracking them.

#include <cstdint>

namespace lookdev
{
	constexpr uint32_t CAPTURE_MAGIC = 0x43315344; // "DS1C"
	constexpr uint32_t CAPTURE_VERSION = 1;

#pragma pack(push, 1)

	struct file_header
	{
		uint32_t magic;
		uint32_t version;
		uint32_t header_size;
		uint32_t adapter;
		uint32_t device_type;
		uint32_t behavior_flags;
		// D3DPRESENT_PARAMETERS without the HWND
		uint32_t bb_width;
		uint32_t bb_height;
		uint32_t bb_format;
		uint32_t bb_count;
		uint32_t multisample;
		uint32_t multisample_quality;
		uint32_t swap_effect;
		uint32_t windowed;
		uint32_t auto_depth_stencil;
		uint32_t auto_depth_stencil_format;
		uint32_t present_flags;
		uint32_t refresh_rate;
		uint32_t presentation_interval;
		// constant register counts read back per draw
		uint32_t vs_const_f;
		uint32_t ps_const_f;
		uint64_t frame_index;
		char game_ini[64]; // INI the game ran with, for reference
	};

	struct record_header
	{
		uint32_t type;
		uint32_t size; // payload bytes following the header
	};

	enum record_type : uint32_t
	{
		// resource descriptions
		REC_TEXTURE = 1,         // texture_desc
		REC_CUBE_TEXTURE,        // texture_desc (height = edge, depth = 1)
		REC_VOLUME_TEXTURE,      // texture_desc
		REC_SURFACE,             // surface_desc: standalone RT / depth-stencil / offscreen plain
		REC_SURFACE_LEVEL,       // surface_level: a mip/face of a texture
		REC_BACKBUFFER,          // id_only: the implicit swap chain's back buffer 0
		REC_VERTEX_BUFFER,       // buffer_desc
		REC_INDEX_BUFFER,        // buffer_desc
		REC_VERTEX_SHADER,       // id_only + bytecode
		REC_PIXEL_SHADER,        // id_only + bytecode
		REC_VERTEX_DECL,         // id_only + D3DVERTEXELEMENT9[] including D3DDECL_END
		REC_DESTROY,             // id_only: the object is gone (live streams; its address was reused)

		// resource contents
		REC_TEXTURE_DATA = 32,   // texture_data + depth * rows * row_bytes
		REC_SURFACE_DATA,        // texture_data (face/level 0) + rows * row_bytes
		REC_BUFFER_DATA,         // buffer_data + bytes

		// device state
		REC_RENDER_STATE = 64,   // state_value { state, value }
		REC_SAMPLER_STATE,       // stage_state { sampler, type, value }
		REC_TEXTURE_STAGE_STATE, // stage_state { stage, type, value }
		REC_SET_TEXTURE,         // bind { stage, id }
		REC_SET_VERTEX_SHADER,   // id_only
		REC_SET_PIXEL_SHADER,    // id_only
		REC_SET_VERTEX_DECL,     // id_only
		REC_SET_FVF,             // state_value { 0, fvf }
		REC_VS_CONST_F,          // const_range + float[4 * count]
		REC_PS_CONST_F,          // const_range + float[4 * count]
		REC_VS_CONST_I,          // const_range + int[4 * count]
		REC_PS_CONST_I,          // const_range + int[4 * count]
		REC_VS_CONST_B,          // const_range + BOOL[count]
		REC_PS_CONST_B,          // const_range + BOOL[count]
		REC_STREAM_SOURCE,       // stream_source
		REC_STREAM_FREQ,         // state_value { stream, divider }
		REC_INDICES,             // id_only
		REC_RENDER_TARGET,       // bind { index, surface id }
		REC_DEPTH_STENCIL,       // id_only (surface)
		REC_VIEWPORT,            // D3DVIEWPORT9
		REC_SCISSOR,             // rect
		REC_CLIP_PLANE,          // clip_plane
		REC_TRANSFORM,           // transform
		REC_MATERIAL,            // D3DMATERIAL9
		REC_LIGHT,               // light
		REC_LIGHT_ENABLE,        // state_value { index, enable }

		// commands
		REC_DRAW = 96,           // draw
		REC_DRAW_INDEXED,        // draw_indexed
		REC_DRAW_UP,             // draw_up + vertex bytes
		REC_DRAW_INDEXED_UP,     // draw_indexed_up + index bytes + vertex bytes
		REC_CLEAR,               // clear + D3DRECT[count]
		REC_STRETCH_RECT,        // stretch_rect
		REC_COLOR_FILL,          // color_fill
		REC_UPDATE_SURFACE,      // update_surface
		REC_UPDATE_TEXTURE,      // update_texture
		REC_BEGIN_SCENE,         // empty
		REC_END_SCENE,           // empty
		REC_PRESENT,             // empty: end of the frame

		// extras
		REC_REFERENCE_IMAGE = 128, // image: the back buffer as the game presented it
		REC_NOTE,                  // UTF-8 text

		// game state for the lookdev host (comp::game::host_feed), written at the
		// frame's first BeginScene, where the proxy updates its lights in game
		REC_GAME_FRAME = 130,      // game_frame
		REC_GAME_LIGHTS,           // game_lights + count * element_size (proxy's point_lights::light)
		REC_TEXTURE_NAME,          // id_only + name (no terminator): the TPF name of a texture

		REC_STREAM_BEGIN = 140,    // file_header: first record of a live stream session
	};

	// texture_data / buffer_data flags
	constexpr uint32_t DATA_INITIAL = 1; // contents at first use in the frame (re-applied on every replay pass)

	struct id_only { uint64_t id; };

	struct texture_desc
	{
		uint64_t id;
		uint32_t width, height, depth;
		uint32_t levels;
		uint32_t usage;
		uint32_t format;
		uint32_t pool;
	};

	struct surface_desc
	{
		uint64_t id;
		uint32_t width, height;
		uint32_t format;
		uint32_t usage;
		uint32_t pool;
		uint32_t multisample;
		uint32_t multisample_quality;
	};

	struct surface_level
	{
		uint64_t id;
		uint64_t texture;
		uint32_t face;
		uint32_t level;
	};

	struct buffer_desc
	{
		uint64_t id;
		uint32_t size;
		uint32_t usage;
		uint32_t format; // FVF for vertex buffers, D3DFORMAT for index buffers
		uint32_t pool;
	};

	struct texture_data
	{
		uint64_t id;
		uint32_t flags;
		uint32_t face;
		uint32_t level;
		uint32_t rows;      // rows per slice (block rows for compressed formats)
		uint32_t row_bytes; // packed: no pitch padding in the file
		uint32_t depth;     // slices (1 unless volume)
	};

	struct buffer_data
	{
		uint64_t id;
		uint32_t flags;
		uint32_t offset;
		uint32_t size;
	};

	struct state_value { uint32_t state, value; };
	struct stage_state { uint32_t stage, type, value; };
	struct bind { uint32_t slot; uint64_t id; };
	struct const_range { uint32_t start, count; };

	struct stream_source
	{
		uint32_t stream;
		uint64_t id;
		uint32_t offset;
		uint32_t stride;
	};

	struct rect { int32_t left, top, right, bottom; };

	struct clip_plane { uint32_t index; float plane[4]; };
	struct transform { uint32_t state; float m[16]; };
	struct light { uint32_t index; uint8_t data[104]; }; // D3DLIGHT9

	struct draw { uint32_t primitive, start_vertex, primitive_count; };

	struct draw_indexed
	{
		uint32_t primitive;
		int32_t base_vertex;
		uint32_t min_index, num_vertices, start_index, primitive_count;
	};

	struct draw_up
	{
		uint32_t primitive, primitive_count, stride;
		uint32_t vertex_bytes;
	};

	struct draw_indexed_up
	{
		uint32_t primitive, min_index, num_vertices, primitive_count;
		uint32_t index_format, stride;
		uint32_t index_bytes, vertex_bytes;
	};

	struct clear
	{
		uint32_t count, flags, color;
		float z;
		uint32_t stencil;
	};

	struct stretch_rect
	{
		uint64_t src, dst;
		uint32_t has_src_rect, has_dst_rect;
		rect src_rect, dst_rect;
		uint32_t filter;
	};

	struct color_fill
	{
		uint64_t surface;
		uint32_t has_rect;
		rect r;
		uint32_t color;
	};

	struct update_surface
	{
		uint64_t src, dst;
		uint32_t has_rect;
		rect r;
		uint32_t has_point;
		int32_t x, y;
	};

	struct update_texture { uint64_t src, dst; };

	struct image
	{
		uint32_t width, height, format;
		uint32_t row_bytes;
	};

	constexpr uint32_t GAME_FRAME_CAMERA = 1;
	constexpr uint32_t GAME_FRAME_PLAYER = 2;
	constexpr uint32_t GAME_FRAME_HEADING = 4;

	struct game_frame
	{
		uint32_t flags;      // GAME_FRAME_*: which values are valid
		float camera[3];     // camera override position, engine raw {X, Z, Y}
		float player[3];     // player position, raw
		float heading;       // player heading, radians
		uint32_t map_id;
	};

	struct game_lights
	{
		uint32_t count;
		uint32_t element_size;
	};

	// ---- live stream ----
	//
	// The host creates a named file mapping: stream_header followed by a byte
	// ring that carries the same record stream as a capture file, frame after
	// frame. The game (lookdev_capture) polls for it and writes; the host reads.
	// Positions are running byte totals; ring offset = position % ring_size.
	//
	// Sessions: the host writes a new consumer_epoch whenever it attaches. The
	// game then starts a new session at its next Present: it forgets everything
	// it sent, sets session_start (where REC_STREAM_BEGIN goes) and bumps
	// session. A host that sees a new session skips to session_start. A record
	// may be larger than the ring; the host consumes it in pieces.

	constexpr uint32_t STREAM_MAGIC = 0x4D525453; // "STRM"
	constexpr uint32_t STREAM_VERSION = 1;
	constexpr const wchar_t* STREAM_NAME = L"Local\\ds1_lookdev_stream";
	constexpr uint32_t STREAM_RING_SIZE = 128u << 20;

	struct stream_header
	{
		uint32_t magic;
		uint32_t version;
		uint32_t header_size;
		uint32_t ring_size;
		volatile int64_t write_pos;        // game: bytes published
		volatile int64_t read_pos;         // host: bytes consumed
		volatile int64_t session_start;    // game: position of the session's REC_STREAM_BEGIN
		volatile uint32_t session;         // game: bumped per session
		volatile uint32_t consumer_epoch;  // host: new value on every attach
		volatile uint32_t consumer_pid;
		volatile uint32_t producer_pid;
		volatile uint32_t consumer_heartbeat; // host: GetTickCount, while it is alive
		volatile uint32_t frames_written;  // game: REC_PRESENT records published this session
		volatile uint32_t frames_consumed; // host: REC_PRESENT records processed this session
		uint32_t reserved[15];
	};

#pragma pack(pop)

	static_assert(sizeof(stream_header) == 128);
	static_assert(sizeof(file_header) == 156);
	static_assert(sizeof(texture_desc) == 36);
	static_assert(sizeof(bind) == 12);
	static_assert(sizeof(light) == 108);

	// ---- format layout ----

	inline constexpr uint32_t make_fourcc(char a, char b, char c, char d)
	{
		return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
	}

	// Bytes per 4x4 block for block-compressed formats, 0 otherwise.
	inline uint32_t block_bytes(uint32_t format)
	{
		switch (format)
		{
		case make_fourcc('D', 'X', 'T', '1'):
		case make_fourcc('A', 'T', 'I', '1'):
		case make_fourcc('B', 'C', '4', 'U'):
			return 8;
		case make_fourcc('D', 'X', 'T', '2'):
		case make_fourcc('D', 'X', 'T', '3'):
		case make_fourcc('D', 'X', 'T', '4'):
		case make_fourcc('D', 'X', 'T', '5'):
		case make_fourcc('A', 'T', 'I', '2'):
		case make_fourcc('B', 'C', '5', 'U'):
			return 16;
		default:
			return 0;
		}
	}

	// Bits per pixel for uncompressed formats, 0 if unknown.
	inline uint32_t bits_per_pixel(uint32_t format)
	{
		switch (format)
		{
		case 20: // R8G8B8
			return 24;
		case 21: case 22: // A8R8G8B8, X8R8G8B8
		case 31: case 32: case 33: case 35: // A2B10G10R10, A8B8G8R8, X8B8G8R8, A2R10G10B10
		case 34: // G16R16
		case 62: case 63: case 64: case 67: // X8L8V8U8, Q8W8V8U8, V16U16, A2W10V10U10
		case 112: case 114: // G16R16F, R32F
			return 32;
		case 23: case 24: case 25: case 26: case 29: case 30: // R5G6B5, X1R5G5B5, A1R5G5B5, A4R4G4B4, A8R3G3B2, X4R4G4B4
		case 40: case 51: // A8P8, A8L8
		case 60: case 61: case 117: // V8U8, L6V5U5, CxV8U8
		case 81: case 111: // L16, R16F
		case 80: // D16
			return 16;
		case 27: case 28: case 41: case 50: case 52: // R3G3B2, A8, P8, L8, A4L4
			return 8;
		case 36: case 110: case 113: case 115: // A16B16G16R16, Q16W16V16U16, A16B16G16R16F, G32R32F
			return 64;
		case 116: // A32B32G32R32F
			return 128;
		default:
			return 0;
		}
	}

	// Rows and packed row size of one slice. Returns false for formats this
	// table does not know; the caller then falls back to the locked pitch.
	inline bool slice_layout(uint32_t format, uint32_t width, uint32_t height, uint32_t& rows, uint32_t& row_bytes)
	{
		if (const auto bb = block_bytes(format))
		{
			rows = (height + 3) / 4;
			row_bytes = ((width + 3) / 4) * bb;
			return true;
		}
		if (const auto bpp = bits_per_pixel(format))
		{
			rows = height;
			row_bytes = (width * bpp + 7) / 8;
			return true;
		}
		return false;
	}
}
