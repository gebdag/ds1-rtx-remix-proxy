#include "std_include.hpp"
#include "estus.hpp"
#include "game_build.hpp"

#include "camera.hpp"
#include "glass.hpp"
#include "host_feed.hpp"
#include "shared/common/ffp_state.hpp"

namespace comp::game::estus
{
	namespace
	{
		bool g_force_empty = false;
		int g_logged_state = -1;

		// GameDataMan -> PlayerGameData -> its EquipGameData, whose goods the HUD reads as well.
		constexpr DWORD kGameDataMan = 0x01378700;
		constexpr DWORD kPlayerGameData = 0x8;
		constexpr DWORD kEquipGameData = 0x1B8;
		constexpr DWORD kInventoryCount = 0x110;
		constexpr DWORD kInventoryFirstGoods = 0x118;
		constexpr DWORD kInventoryLastGoods = 0x13C;
		constexpr DWORD kInventoryArray = 0x124;
		constexpr DWORD kInventoryEntrySize = 0x1C;
		// An entry starts { category, id, quantity }.
		constexpr DWORD kCategoryGoods = 0x40000000;
		// Estus: 201, 203 ... 215 full, one for each level of reinforcement, and the even ids below each empty.
		// An empty flask keeps quantity 1, so its id tells, not its quantity.
		constexpr DWORD kEstusFirst = 200;
		constexpr DWORD kEstusLast = 215;

		// How far from the player's feet a flask counts as the one in the player's hand, in metres. In hand,
		// 0.14-0.26 aside and 1.6 up (frame_20261101_151323, 151350).
		constexpr float kHandAside = 1.5f;
		constexpr float kHandBelow = 0.5f;
		constexpr float kHandAbove = 2.5f;

		// True only when the inventory proves it: an empty flask and no full one. A failed read says not empty.
		bool read_empty()
		{
			const DWORD game_data_man = build::at(kGameDataMan); // 2015-06-15 address -> running build's
			if (!game_data_man)
				return false;
			__try
			{
				const DWORD man = *reinterpret_cast<const DWORD*>(game_data_man);
				if (!man)
					return false;
				const DWORD player = *reinterpret_cast<const DWORD*>(man + kPlayerGameData);
				if (!player)
					return false;
				const DWORD equip = player + kEquipGameData;
				const int count = *reinterpret_cast<const int*>(equip + kInventoryCount);
				const int first = *reinterpret_cast<const int*>(equip + kInventoryFirstGoods);
				const int last = *reinterpret_cast<const int*>(equip + kInventoryLastGoods);
				const DWORD entries = *reinterpret_cast<const DWORD*>(equip + kInventoryArray);
				if (!entries || first < 0 || last >= count || count > 4096)
					return false;

				bool empty = false;
				for (int i = first; i <= last; i++)
				{
					const auto* e = reinterpret_cast<const DWORD*>(entries + static_cast<DWORD>(i) * kInventoryEntrySize);
					if (e[0] != kCategoryGoods || e[1] < kEstusFirst || e[1] > kEstusLast)
						continue;
					if ((e[1] & 1) != 0 && e[2] > 0)
						return false;
					empty = true;
				}
				return empty;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// Only the local player's inventory can be read: anyone else's flask counts as full.
		bool in_players_hand(const float* vs_const)
		{
			float cam_x = 0.0f, cam_up = 0.0f, cam_z = 0.0f;
			float player_x = 0.0f, player_z = 0.0f, player_up = 0.0f;
			if (!vs_const || !shared::common::ffp_state::get().world_offset(cam_x, cam_up, cam_z) ||
				!camera::get_char_pos(player_x, player_z, player_up))
				return false;
			// The flask's bone (VS c8-c10, 3x4 rows) holds its origin relative to the camera. The camera is
			// { x, up, z }, the player { x, z, up }.
			const float aside_x = vs_const[8 * 4 + 3] + cam_x - player_x;
			const float up = vs_const[9 * 4 + 3] + cam_up - player_up;
			const float aside_z = vs_const[10 * 4 + 3] + cam_z - player_z;
			return std::fabs(aside_x) < kHandAside && std::fabs(aside_z) < kHandAside && up > -kHandBelow && up < kHandAbove;
		}
	}

	bool player_flask_empty()
	{
		const bool empty = shared::globals::lookdev_host ? host_feed::frame().estus_empty : read_empty();
		if (static_cast<int>(empty) != g_logged_state)
		{
			g_logged_state = static_cast<int>(empty);
			shared::common::log("Estus", empty ? "the player's flask is empty: the game's green" : "the player's flask has charges: yellow");
		}
		return empty;
	}

	IDirect3DBaseTexture9* flask_texture(IDirect3DDevice9* dev, const float* vs_const)
	{
		if (g_force_empty || (in_players_hand(vs_const) && player_flask_empty()))
			return nullptr;
		return glass::derived_texture(dev, "s00902_full");
	}

	void set_force_empty(bool force)
	{
		g_force_empty = force;
	}
}
