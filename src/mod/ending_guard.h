// The story's ending taken away before its own end. The ending is a scene of
// the game's flow that starts tasks of its own (the cutscenes, the stage
// behind them, the staff roll with a task for every line on the screen, the
// last picture) and ends them from its own states only; the scene's "destroy"
// leaves them. The window closed during the ending ends the scene from
// outside: its tasks went on without it, the staff roll's lines drew with the
// game's font after the game had let it go (C0000005 at 0x513E98), and what
// they held was still there when RenderWare closed (C0000005 at 0x4C0F08).
// Here the scene's "destroy" ends what the scene still has, the way its own
// states do, and a cutscene's actors with their cutscene (left to themselves
// they end a frame later and ask for the scene's fade task: 0x4ACF7F).
// re/notes.md §38.
#pragma once
#include "common/config.h"

namespace ending_guard {

// [debug] ending_guard=0: as before (lab: the fault).
void install(const Config& cfg);

}  // namespace ending_guard
