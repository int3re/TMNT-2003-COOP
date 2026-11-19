// The level map (ВЫБОР УРОВНЕЙ, and the jump into a level / between levels):
// the game puts player 1's turtle on it only. With two players player 2's
// turtle stands beside it and jumps along, so the map shows who is playing;
// with four (players=4) the four turtles stand in a row on the card.
// re/notes.md §19, §20. Both PCs of a game together do the same (no setting).
#pragma once
#include "common/config.h"

namespace map_turtles {

void install(const Config& cfg);

// Where player p's turtle on the map is (world, its feet), when it moved
// during the last few frames; false = no map turtle of that player. Game thread.
bool position(int player, float out[3]);

}  // namespace map_turtles
