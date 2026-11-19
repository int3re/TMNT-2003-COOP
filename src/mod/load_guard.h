// The main menu with its textures. The menu scene makes its texture dictionary
// from menu_pc.txd as the game's loader read it, sets it current and creates
// the menu, whose sprites take their textures from the current dictionary.
// Once (a lab run with two copies under load, the intro skipped by presses)
// menu_bg was not in it and the menu's first sprite crashed the game (a null
// texture, 0x4ad064). ensure() looks first; a missing texture has the
// dictionary made again from TMNT.DAT and set current. Game thread.
#pragma once
#include "common/config.h"

namespace load_guard {

// [debug] break_dict=1: the first check finds no current dictionary (as the
// lab's crash had it), so the guard's path runs.
void install(const Config& cfg);
// Before a scene's sprites are made: `texture` must be in the current texture
// dictionary, else `file` of TMNT.DAT (a .txd) becomes it.
void ensure(const char* texture, const char* file);

}  // namespace load_guard
