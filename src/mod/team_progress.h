// The team's progress: what a stage of the story gives goes to every turtle
// that played it, not to player 1's alone.
//
// The game keeps its progress by character (six records in the save) and
// writes all of it for player 1's character only:
//   - the dojo's prize (a new move after the first dojo, more strength after
//     the second and the third; record flags 2, 4, 8): 0x4109bf / 0x4109e6 /
//     0x410a0d, called by the dojo with the character of player 1;
//   - how far the story has been played (record +0x20): 0x410bae;
//   - "the story is cleared", the costume it opens and everything else a
//     stage opens (0x40f939, asked once after every stage): for player 1.
// With two or more turtles in a stage each of them gets the same. So the
// move a team learns in the dojo is learnt by the whole team, and a team of
// four that beats the last stage has "all four turtles cleared the story",
// which is what the game asks for before its last boss.
//
// The same on every PC of a game together (the records are the host's there).
// re/notes.md §37. Game thread.
#pragma once

#include "common/config.h"

namespace team_progress {

void install(const Config& cfg);

// Lab ([debug] dojo_pass_at=N): the dojo being played is passed now, by the
// game's own routine (0x401198: what its judge does when the dojo's task is
// done), so its prize and what it opens follow as in a dojo really won.
void debug_dojo_pass();

}  // namespace team_progress
