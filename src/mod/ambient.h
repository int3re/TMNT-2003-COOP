// Side bars that are not black. When a 4:3 picture (menu background, loading
// screen, movie) is pillarboxed on a wide screen, the bars to its left and
// right get a blurred, darkened copy of the frame scaled to cover the screen,
// the way video players show portrait clips. [video] side_bars=blur|black.
#pragma once
#include "common/config.h"

struct IDirect3DDevice9;

namespace ambient {

void install(const Config& cfg);

// widescreen.cpp: a full-width picture was pillarboxed in this frame.
void note_pillarbox();

// Present, before the overlay draws: fill the bars if this frame needs it.
void before_present(IDirect3DDevice9* dev);

// D3D9 Reset: our render targets live in the default pool.
void release();

// Settings window: blurred (true) or black side bars, from the next frame.
void set_blur(bool on);

}  // namespace ambient
