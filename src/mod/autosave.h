// Saving the story progress after every cleared stage. The game itself saves
// only at chapter ends and after challenges, and its question there ("ВЫ
// ХОТИТЕ СОХРАНИТЬСЯ?") starts on "НЕТ". With [mod] autosave=1 tmntnet writes
// savegame.dat with the game's own serializer right after each cleared stage
// (no screen, no question) and keeps the game's own АВТОЗАПИСЬ on, so its
// chapter saves go without the question too. See re/notes.md §14.
#pragma once
#include "common/config.h"

namespace autosave {

void install(const Config& cfg);  // InitializeASI

bool enabled();
void set_enabled(bool on);  // settings window: also the game's АВТОЗАПИСЬ

// Game thread, a moment after the main menu shows (the game has loaded its
// save by then): applies [mod] autosave to the game's АВТОЗАПИСЬ once, then
// follows that switch if it is changed in the game's own options.
void poll();

// Writes savegame.dat now. Game thread, outside a stage.
bool save_now(const char* why);

// [debug] lab check: does the progress in memory match savegame.dat?
void debug_compare();
void debug_mark(int value);

}  // namespace autosave
