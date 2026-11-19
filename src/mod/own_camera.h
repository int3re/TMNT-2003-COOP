// Every PC its own camera. The game has one camera for all players: it aims
// at their middle and opens its view to hold them. In a game together every
// PC runs the same game, so that camera has to stay the same on all of them:
// the turtles move by its direction, and what is "on the screen" decides what
// the game draws and does (a character's draw function throws dice for its
// splashes, enemies know whether they are seen).
//
// So the game keeps its camera, and two things are added:
// - every frame the game's own follow camera is run once more for every
//   player alone (its aim, its zoom, the stage's camera path: what the game
//   shows a single player), on every PC the same; the game's camera is then
//   put back as it was;
// - the picture alone is drawn with the view of this PC's player, put in
//   place right before the world's picture and taken away right after, byte
//   for byte. The wide screen (Hor+) and [video] fov are applied to the
//   picture the same way, so they no longer reach the game;
// - in a game together "on the screen" means: seen by the game's camera or by
//   any player's own view.
// re/notes.md §31.
#pragma once
#include <cstdint>

#include "common/config.h"

namespace own_camera {

void install(const Config& cfg);
void set_enabled(bool on);  // [video] own_camera: this PC draws around its own turtle
bool enabled();

// The camera's begin and end update are hooked here: the picture gets its
// window and camera from this module (widescreen.cpp then leaves the main
// camera's window to it; nametags.cpp is told of the world's pictures).
bool swaps();
// In a game together the game's own reckoning uses one view on every PC,
// whatever each screen shows (widescreen.cpp: the main camera's window).
bool same_view_for_all();

// The game's follow camera asked where to aim (four_players.cpp's hook of
// 0x402f8f; lab trace). Game thread.
void on_aim(const float* shared, float lift, const void* caller);
// A player's own view is being worked out: the game's camera code runs for it
// alone and must set nothing of the real camera (widescreen.cpp).
bool working_out();
// The game's zoom for one player at `pos` (0x403735, one player in play).
float zoom_for(const float* pos);

// Present: the next picture is a new frame.
void frame_done();

// The last world picture was drawn with this module's camera: its frame's
// matrix in the world (16 floats: right, up, at, pos) and 1 / view window
// (x, y), for what is drawn over the picture later (name tags, the bars of a
// fallen turtle). Null: drawn as the game's camera stands.
const float* picture_matrix();
const float* picture_recip_window();

}  // namespace own_camera
