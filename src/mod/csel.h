// Story Mode character select (2 players): makes P2's cursor follow the
// remote P2, or picks a turtle for P2 when nobody is connected.
#pragma once
#include <cstdint>

#include "common/config.h"

namespace csel {

void install(const Config& cfg);
bool on_screen();  // the character select ran during the last few frames

// With three or four in the story, players 3 and 4 pick in player 2's place
// after it: whose turn that is now (2..4), and who took a turtle so far in
// this turn's menu (1..3; 0 = free). Game thread.
int turn();
int taker(int character);
// The player whose tag the select's turtles draw is drawing now (2, 3; 0 =
// none or the game's own "ИГРОК 1"): csel_art.cpp swaps the tag. Game thread.
int tag_player();
// Who picks now (1..4; 0 = nobody: before, after), and a copy of the tag's
// sprite (0x3C bytes: +0 x, +4 y, +0xC width, +0x10 height, +0x14 colour, +0x18
// texture) for drawing a name like one (false until the turtles were drawn).
int picker();
bool tag_model(uint8_t out[0x3C]);
// The turtle the cursor of who picks is on (character 1..4; 0 = the plate).
int pointed();

// Settings: [mod] p2_autopick, p2_autopick_delay_ms, p2_character.
void set_autopick(bool on, int delay_ms, int character);

}  // namespace csel
