// Down, not out (the story with two to four players): a turtle whose life
// runs out lies stunned where it fell, stars over it, while another turtle is
// still up; enemies leave it alone. A turtle next to it holding [mod]
// revive_button (shuriken change) for two seconds raises it with a third of
// its life. The level is lost only when they are all down: the hearts are not
// used then (nor shown), the game ends as with none left. re/notes.md §21.
// Both PCs of a game together take the host's setting. Game thread.
#pragma once
#include <cstdint>

#include "common/config.h"

struct IDirect3DDevice9;

namespace revive {

void install(const Config& cfg);

// [mod] revive and its button (a slot of the key table), or the host's in a
// game together.
bool enabled();
int button();
void set_session(bool on, int button);
void clear_session();

// Revive on and two or more turtles in this level: the team's rules.
bool team();

// Every frame of the game, right after its pads are read (four_players.cpp).
void on_frame();

// Player p (0..3) lies stunned now.
bool down(int player);
bool any_down();

// Present: stars going round over the stunned, the bar of one being raised.
bool prepare(IDirect3DDevice9* dev);
void draw(IDirect3DDevice9* dev);

}  // namespace revive
