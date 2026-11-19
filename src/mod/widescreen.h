// Widescreen for TMNT (2003): 16:9/16:10 video modes, Hor+ camera, HUD kept
// in the 4:3 centre. All in memory; see re/notes.md "Widescreen".
#pragma once
#include <cstdint>
#include <utility>
#include <vector>

#include "common/config.h"

struct IDirect3DTexture9;

namespace widescreen {

// Must run before the game builds its video mode list (InitializeASI).
void install(const Config& cfg);

// Game thread: the next Im2D primitives are drawn with this texture instead of
// the one the game bound (tmntnet's items drawn as the game's own sprites).
void set_texture_override(IDirect3DTexture9* tex);

// Present: a new frame starts (resets the per-frame picture tracking).
void end_frame();

// Full-screen pictures (menus, movies) are shown 4:3 in the middle.
bool pictures_pillarboxed();
// ... and the menus' own backgrounds go on to the screen's edges, mirrored
// at the 4:3 edges ([video] wide_menus): no side bars in those menus.
bool menus_wide();
// Smaller 2D (the HUD, menu words and cursors) is kept in the 4:3 middle.
bool hud_centered();
// ... and the HUD's corner pieces (players' panels, the radar) at the screen's
// edges ([video] hud_corners).
bool hud_corners();
// The next 2D primitives are such a piece: -1 left, 1 right, 0 none. Game thread.
void set_edge(int side);
// The game's sprite function draws now: its texture's name (null: done). The
// pictures across the screen are told apart by it. Game thread.
void set_sprite(const char* texture_name);
// The game's text-in-box function (0x4a72cb), 0 if not found. Its first bytes
// are this module's hook, so other modules cannot find it by its signature.
uintptr_t text_in_box();
// The game's key table is up (keyconf.cpp): its frame, a quad taller than the
// screen and as wide, is fitted like the menu around it, not like a picture.
void set_key_table(bool up);

// Settings window (game thread): how 2D is fitted, applied from the next frame.
void set_hud_center(bool on);
void set_pillarbox(bool on);
// [video] fov: the main camera's view, % of the game's (100 = the game's).
void set_fov(int percent);
// The view window `camera` is to show when the game asks `asked` (x, y) for
// it: wider on a wide frame buffer, with [video] fov on the main camera. For
// a picture's own window (own_camera.cpp); nothing is set.
bool shown_window(void* camera, const float asked[2], float out[2]);
// The window the game asked last for `camera`, if it is the main camera.
bool asked_window(void* camera, float out[2]);
// The video modes the game lists (width x height, 32-bit), and the one in use.
std::vector<std::pair<int, int>> video_modes();
std::pair<int, int> current_mode();

}  // namespace widescreen
