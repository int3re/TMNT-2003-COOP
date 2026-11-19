// In-game overlay: Dear ImGui drawn on the game's Direct3D 9 device right
// before Present, plus F12 screenshots. The UI itself lives in ui.cpp.
#pragma once
#include "common/config.h"

struct IDirect3DDevice9;

namespace overlay {

// Hooks Direct3DCreate9 -> CreateDevice -> Present/Reset. Call from
// InitializeASI, before the game creates its device.
void install(const Config& cfg);

// True while tmntnet's own menu has focus (and briefly after it closes, until
// the keys used to close it are released): the game must not see input then.
bool blocks_game_input();

// The game's Direct3D 9 device (nullptr before it exists). Game thread only.
IDirect3DDevice9* device();

}  // namespace overlay
