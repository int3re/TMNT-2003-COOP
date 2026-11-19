// The pause in a game together. The game asks for its pause straight from the
// keyboard (Esc, raw: 0x4b0d74(1, 1, 0) in the level's round), and the raw
// buffers are blanked in a session (lockstep.cpp), so there was none. Now:
// - the pause key newly down in ANY player's buttons of a frame (they come
//   with the frame on every PC) answers that question on every PC at once:
//   the game's own pause and its own menu, in the same frame everywhere;
// - the menu and its "are you sure?" are led by the player who paused (the
//   game reads pad slots 0 and 1 there: slot 0 then gives that player's
//   buttons, slot 1 nothing); the pause key of anybody again goes back to the
//   game;
// - "leave the level" takes everybody out of it: only when the host leads;
// - "quit the game" closes the game of the player who chose it; the others'
//   games go back to the level (its turtle stands, as after any leaving).
// re/notes.md §34.
#pragma once
#include "common/config.h"

namespace net_pause {

void install(const Config& cfg);

// The pause's question is up in a game together: the player who answers it
// (0-based), else -1. For host_menus.cpp's hook of the menus' update.
int question_of();
// The pause's menu is up in a game together (the name tags go meanwhile).
bool up();

}  // namespace net_pause
