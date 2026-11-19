// Name tags: in a game together each player's name floats over their turtle
// (the host's and the friend's names as the network page shows them), on the
// level map too, so everybody sees who plays whom. re/notes.md §19.
#pragma once
#include <string>

#include "common/config.h"

struct IDirect3DDevice9;

namespace nametags {

// The world camera drew a picture since the last Present (in play, on the
// level map), and world -> back buffer pixels with it. Present, after
// prepare() (revive.cpp).
bool world_shown();
bool to_screen(const float world[3], float W, float H, float& x, float& y);

void install(const Config& cfg);
void set_enabled(bool on);  // [ui] nametags
// The game's camera begins a picture of the world (own_camera.cpp's hook of
// the camera's begin update; without it this module hooks it itself).
void on_world_picture();
// ... and it did since the last Present (asked while the frame's 2D is drawn).
bool world_drawn_now();

// Player 0..3's name as the tags show it ("" outside a game together, or with
// the tags off). Game thread.
std::string name(int player);

// A name's colour: its turtle's mask (character 1 Raphael red, 2 Michelangelo
// orange, 3 Donatello purple, 4 Leonardo blue; light enough on a dark plate),
// 0xRRGGBB; white without a turtle.
unsigned mask_rgb(int character);

// Present, game thread: works out this frame's tags; true = there are some.
bool prepare(IDirect3DDevice9* dev);
// ... and draws them (inside the overlay's scene).
void draw(IDirect3DDevice9* dev);

}  // namespace nametags
