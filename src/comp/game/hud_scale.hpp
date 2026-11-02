#pragma once

namespace comp::game::hud_scale
{
	// The game's in-game HUD is sized for a television: this scales it, each part towards its own corner. The
	// F4 HUD tab sets the sizes, [Hud] in the ini keeps them.
	//
	// The game's UI is EzMenu dialogs from menu.drb, laid out in a virtual 1280x720, which record a command
	// buffer each frame that the render thread replays (MenuDrawEntity::Draw). So the parts are found by their
	// names in menu.drb where the dialogs record (never by where a draw lands, which cannot tell a pause menu
	// from the HUD), and each is bracketed by two commands of our own in that buffer. While a bracket replays,
	// its draws go through a scaled c0/c1 of the UI vertex shaders. Sprites, gauges and text move as one, which
	// scaling the dialogs' own matrices would not do: text lays out at its font's size whatever the matrix, and
	// their vertices round to the virtual grid. Anything not listed is left as the game draws it: menus,
	// conversations, the aiming reticle. At scale 1 nothing is recorded.

	// Hooks the dialog draws. Not in lookdev mode (the vanilla reference) nor in its host (no game running).
	void install();

	// Around a UI draw on the render thread: applies the scale of the bracket being replayed, if any.
	class draw_scope
	{
	public:
		explicit draw_scope(IDirect3DDevice9* dev);
		~draw_scope();
		draw_scope(const draw_scope&) = delete;
		draw_scope& operator=(const draw_scope&) = delete;

	private:
		IDirect3DDevice9* dev_ = nullptr;
		float constants_[8] = {};
		RECT scissor_ = {};
		bool scissor_changed_ = false;
	};

	// The F4 menu's HUD tab.
	void tab_imgui();
}
