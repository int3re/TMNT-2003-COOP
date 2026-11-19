// The game's own options that tmntnet's window shows as well. "АВТОЗАПИСЬ"
// (ПАРАМЕТРЫ -> ПРОЧИЕ ПАРАМЕТРЫ) is bit 0 of the options' +0x6C, "СИЛА УДАРА"
// bit 1 (TMNT.EXE bc49a82c..., re/notes.md §13). Game thread only.
#pragma once
#include <cstdint>

namespace game_options {

void init();  // InitializeASI (signatures only)
bool ready();

bool autosave();
// Like the game's own options screen, and saved to savesetting.dat at once.
void set_autosave(bool on);

// The options that change how the game plays (+0x58..+0x6F: difficulty,
// flags...), 24 bytes, for a lockstep session: the host's go to the other
// PC, which applies them in memory only (nothing saved).
constexpr int kPlayBytes = 24;
bool get_play(uint8_t out[kPlayBytes]);
bool set_play(const uint8_t in[kPlayBytes]);

}  // namespace game_options
