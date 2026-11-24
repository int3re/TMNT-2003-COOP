// Four players in the story: the four turtles at once. The game keeps its
// players in a record table and two pad slots made for two (re/notes.md §20).
// tmntnet keeps players 3 and 4 in tables of its own and answers the game's
// accessors for them, so the game's own code runs them like players 1 and 2:
// they get the two turtles nobody took, appear with the others when a level
// starts, enemies go for them too. Game thread.
#pragma once
#include <cstdint>

#include "common/config.h"

namespace four {

void install(const Config& cfg);

bool enabled();    // three or four players in the story
bool installed();  // its hooks are in place (the game is laid out as known)
// How many play the story: 1..4 ([mod] players, the story's menu: story_menu.cpp).
int count();
void set_count(int players);
// Turtles of this level's team (players 1..4 with a character).
int team_size();

// This frame's buttons of player 3 or 4 (player = 2 or 3) as the game's 16
// key-table bits: bit i = slot i of the key table, 0x1000 up, 0x2000 right,
// 0x4000 down, 0x8000 left. Taken when the game reads its pads.
void set_buttons(int player, uint16_t held);

// The keyboard as the game reads it: players 3 and 4 at this keyboard
// ([mod] p3_keys / p4_keys). Game thread.
void on_keyboard(const uint8_t* keys);

// Right after the game updated its two pad slots (0x4a984d, hooked by
// main_menu.cpp): players 3 and 4's slots from their buttons. Game thread.
void after_pad_update();

// Player p's buttons this frame as its pad slot has them (key-table bits,
// and the game's own: decide 0x10000 ...), and taking some of them away (key
// table bits) before the turtles read them. Game thread.
uint32_t held(int player);
void mask(int player, uint16_t raw);
// The buttons newly down this frame in player p's pad slot (0..3).
uint32_t pressed(int player);
// While set (player 0..3; -1 ends it): what the game reads of pad slot 0 is
// this player's buttons, less `without`, with `with` among the newly pressed
// ones; slot 1 gives nothing. For a menu led by one player (net_pause.cpp).
void read_slot0_as(int player, uint32_t without, uint32_t with);

// Player p (0..3): its turtle (actor) or null, its player data (the
// turtle's: +0x2C full life, +0x30 life, +0x5C score, +0x84 shuriken kind,
// +0x88/+0x90/+0x8C shurikens of kind 0/1/2) or null, its character (0 = none).
void* turtle(int player);
void* player_data(int player);
int character(int player);
// Player p (0..3) plays the story now and has a character (players 3 and 4:
// only when that many play).
bool plays(int player);
// While `character` is not 0 the game's "player 1's character" (0x404740(0))
// is answered with it: the game's code that looks at player 1 alone runs once
// more for another turtle of the team (team_progress.cpp). Game thread.
void answer_first_as(int character);
// Player p's place in the level as the game keeps it for its camera and the
// enemies (3 floats; the feet), null while it has no turtle.
const float* position(int player);
// The game's camera code now sees this player alone (its aim, its zoom, its
// stretch of the stage's camera path); -1: all players again. For
// own_camera.cpp, around its runs of the follow camera.
void camera_solo(int player);

// Players 3 and 4 get their characters now (their picks in the character
// select, else the turtles players 1 and 2 did not take), as a level's load
// does: the level map shows them before the level. Game thread.
void choose_characters();

// The character select (csel.cpp): players 3 and 4's turtles (character
// numbers, 0 = none: a free one), and the player whose buttons player 2's pad
// slot carries this frame (2 or 3; -1 = player 2's own), so the game's own
// pick for player 2 serves players 3 and 4 in their turn. Game thread.
void set_picks(int p3, int p4);
void lend_slot(int player);

// Player 3 or 4 was in this level and is down now (its turtle is gone): it
// comes back with its decide while there are hearts (credits()).
bool down(int player);
int credits();
void take_credits();  // none left (the team's rules: revive.cpp)

// Lab ([debug] shots "hurt_3:N"): player p's life set to `life`;
// "kill_3:N": its turtle taken away as after a death (no animation).
void debug_set_life(int player, int life);
void debug_kill(int player);
// Lab ([debug] stage_over_at=N): the stage's script says "done" (the game's
// flags, [info +0x14] bit 0x800: the mode's rules make "stage clear" 0x1000 of
// it, the level's task ends the level and the story goes on to the next one).
void debug_stage_over();
// Lab ([debug] actions=1): when a player's life first came to 0 (net::now_ms;
// 0 = not yet), for shots timed from it ("down:N").
uint32_t debug_down_at();

}  // namespace four
