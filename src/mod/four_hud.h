// The play screen's panels of players 3 and 4 (four players): the game draws
// two (its panel blocks are made for two), tmntnet draws these two more out of
// the same pictures (gameinfopc.txd) and layout, smaller, under players 1 and
// 2's: portrait, health, shuriken, score. re/notes.md §20. Game thread.
#pragma once
#include "common/config.h"

struct IDirect3DDevice9;

namespace four_hud {

void install(const Config& cfg);
// A sprite of the play screen is being drawn (0x4abab2, hooked by
// main_menu.cpp); false = not to be drawn (the hearts under the team's rules,
// revive.cpp). Game thread.
bool on_sprite(const void* sprite);
// The players' panels are on the screen (player 1's portrait drawn within the
// last half second of frames): a level is being played.
bool panels_shown();
// [video] hud_size: the panels at the top (players 1 and 2's, the hearts, and
// players 3 and 4's), % of the game's size, toward their corners of the screen.
// [video] hud_corners: on a wide screen with the HUD in the 4:3 middle, the
// players' panels and the radar go to the screen's edges.
void set_size(int percent);
// [debug] hud_trace: a sprite of the play screen drawn from this call site
// (logged once per site, with its picture, place and the sprites' space).
void trace(const void* sprite, const void* site, const char* how);
// While alive, a sprite of the play screen drawn from that call site (the
// draw's return address) is placed as above: a panel's sprite at its size
// (place and size scaled around its corner, put back after) and at its edge,
// the radar's at the right edge. Game thread.
class Scaled {
 public:
  Scaled(void* sprite, const void* site);
  ~Scaled();
  Scaled(const Scaled&) = delete;
  Scaled& operator=(const Scaled&) = delete;

 private:
  float* s_ = nullptr;
  float saved_[4] = {};
  int edge_ = 0;
};
// Present: works out this frame's pieces; true = there are some.
bool prepare(IDirect3DDevice9* dev);
// ... and draws them (inside the overlay's scene).
void draw(IDirect3DDevice9* dev);

}  // namespace four_hud
