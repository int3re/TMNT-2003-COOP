// The story's levels as the game's story flow starts and ends them (0x494503:
// state 9 starts the level's scene, 0x49493a; state 10 waits for its end,
// 0x4ab428). For now a lab instrument: [debug] level_dump=1 writes the game's
// static memory and its two long-lived blocks (the info, the config) to
// tmntnet-dump-<n>-<start|end>.bin at both, to learn what a level leaves
// behind (re/notes.md §35: a PC coming into a running game could then pass
// the levels already played).
#pragma once
#include "common/config.h"

namespace level_marks {

void install(const Config& cfg);

}  // namespace level_marks
