#pragma once
#include "common/config.h"

// Hooks IDirectInputDevice8A::GetDeviceState (vtable[9] of the system
// dinput8.dll device class) with MinHook so P2's network buttons are OR'd
// into the game's keyboard state.
bool install_dinput_hooks(const Config& cfg);

// Controls page: catch the next key pressed on the real keyboard (DIK code),
// even while tmntnet's window keeps it from the game. Game thread.
void begin_key_capture();
int take_captured_key();  // -1 = nothing yet
void cancel_key_capture();

// Settings window: [mod] p2_lock from now on.
void set_p2_lock(bool on);

// [mouse] from now on: buttons added to a player's key for a slot (player
// 0..2, slots of left, right, middle, side 1, side 2). Buttons set in the
// game's key table are keys of their own (binds::kMouseKey) and always work.
void set_mouse_binds(bool on, const int players[5], const int slots[5]);
// The game's key table is up (keyconf.cpp): the [mouse] additions do nothing
// there (a click would press F); buttons as keys of their own still work.
void pause_mouse(bool paused);

// [debug] shots=press_1F:6000 (lab): hold one key (DIK code) for 200 ms;
// mouse_0:6000: the left mouse button (0..4) as if pressed over the game.
void debug_press(int dik);
void debug_hold(int dik, int ms);  // the same, held for `ms`
void debug_mouse(int button);
int debug_mouse_button();  // the button of a lab mouse_N step now, -1 none
