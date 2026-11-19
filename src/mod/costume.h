// The turtles' costumes. The game has six a turtle (TMNT.DAT files 27..50,
// <turtle>_normal / _ex / _santa / _hallow / _ex_santa / _ex_hallow.pac) and
// picks one by itself: the second one ("ex") only for a turtle whose save
// record has it unlocked, and only while a secret button is held at the
// decide of the character select; the festive ones only on the PC's date
// (31 October; 24 and 25 December). Here every player picks any of the six in
// the character select (csel.cpp's costume step), and what the game then
// stores and loads follows that choice. re/notes.md §30.
#pragma once
#include "common/config.h"

namespace costume {

constexpr int kCount = 6;  // 0 normal, 1 ex, 2 santa, 3 hallow, 4 ex + santa, 5 ex + hallow (the game's numbers)

void install(const Config& cfg);
bool ready();  // the game's costume code was found: the choice works

// A costume's name for the character select (Russian capitals).
const char* name(int index);
// The costume a select starts on: the first, or off line the one the game
// itself would give today (a festive one on its dates).
int initial();

// The story select's result: the turtle `character` (1 Raphael, 2
// Michelangelo, 3 Donatello, 4 Leonardo) wears costume `index`. Kept until
// clear(). Game thread.
void set(int character, int index);
void clear();  // the game decides by itself again (the main menu, a new select)
// The costume picked for that turtle, -1: none (the game's own). For the
// players' record (four_players.cpp).
int worn(int character);

}  // namespace costume
