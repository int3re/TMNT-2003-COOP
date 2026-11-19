// Picture quality beyond the game's own settings: multisample anti-aliasing
// (added to the game's parameters when the device is made or reset) and
// anisotropic texture filtering (the game's linear minification made
// anisotropic, its nearest mip filter linear). What is shown only: nothing
// the game computes changes.
#pragma once
#include <d3d9.h>

#include "common/config.h"

namespace quality {

void install(const Config& cfg);
// CreateDevice / Reset (overlay.cpp): [video] antialiasing added to the
// game's parameters, the highest level the adapter has at or below it; true =
// pp changed.
bool adjust_present(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, D3DPRESENT_PARAMETERS& pp);
// The device was made or reset with these parameters.
void on_device(IDirect3DDevice9* dev, const D3DPRESENT_PARAMETERS& pp);
// The anti-aliasing samples in use (0 = none).
int msaa();
// Settings (game thread), both at once: anisotropic filtering (0 = the
// game's, 2..16) and anti-aliasing (0 off, 2, 4, 8: the device is reset the
// way the game's own resolution change does it). set_msaa returns the samples
// in use then, or -1 when it could not be done now (it waits for a new start).
void set_anisotropy(int level);
int set_msaa(int samples);
// Lab ([debug] shots "msaa_N:T"): asked from any thread, done by poll() on the
// game thread outside the drawing (the keyboard's poll).
void request_msaa(int samples);
void poll();

}  // namespace quality
