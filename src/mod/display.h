// Display modes: exclusive fullscreen (the game's own), borderless (a window
// over the whole monitor) and windowed. The game keeps rendering through its
// fullscreen path at the chosen video mode (widescreen.cpp picks it); for
// borderless/windowed the D3D9 device is created windowed from a copy of the
// game's parameters (RenderWare keeps its own), the window is placed by us
// instead of the game, and Present scales the frame into the window, with
// black bars when the aspect differs. Alt+Enter switches windowed/borderless.
// See re/notes.md "Window, focus".
#pragma once
#include <windows.h>
#include <d3d9.h>

#include "common/config.h"

namespace display {

void install(const Config& cfg);  // InitializeASI: window placement hook, DirectSound

bool windowed_device();  // borderless or windowed: the device is created windowed

// D3D9 CreateDevice/Reset: fills `out` and returns true when the game's
// parameters must be changed (windowed device, vsync).
bool adjust_present(const D3DPRESENT_PARAMETERS& game, D3DPRESENT_PARAMETERS& out);
void on_device(HWND hwnd, UINT backbuffer_w, UINT backbuffer_h);

// Present (game thread): frame limiter and the window rect to draw into.
const RECT* before_present(HWND hwnd, const RECT* game_dst);

// WM_MOUSEMOVE lParam from window pixels to frame pixels (for the overlay).
LPARAM map_mouse(LPARAM lp);
bool backbuffer_size(float& w, float& h);  // only for a windowed device

// The keyboard read failed because the window is not in front and
// [video] background=run: the caller fakes an empty keyboard (P2 still plays).
bool fake_unfocused_keyboard();

HWND window();  // the game's window, once it exists

// Settings window (game thread). Mode: 0 fullscreen, 1 borderless, 2 windowed.
int mode();
bool switch_mode(int m);             // borderless <-> windowed now; false = takes a restart
void set_window_size(int w, int h);  // windowed client size; 0, 0 = from the frame
void set_window_resize(bool on);
void set_background_run(bool on);
void set_fps_limit(int fps);         // 0 = off

}  // namespace display
