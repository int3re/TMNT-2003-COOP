// The game's controls, readable and editable from tmntnet's window: keyboard
// keys of both players (the game's own binding table, saved by the game into
// savesetting.dat, so its own key menu keeps working), gamepad buttons and
// which gamepad each player uses, plus "P2 only from the network" for netplay.
// Game thread only. See re/notes.md "Players and binds".
#pragma once
#include <string>
#include <vector>

#include "common/config.h"
#include "common/protocol.h"

namespace binds {

void init(const Config& cfg);  // InitializeASI (signatures only)
bool ready();

struct Snapshot {
  int key[2][tmnp::kNumSlots];  // DIK code per player and slot, <= 0 = none
  int pad[2][tmnp::kNumSlots];  // gamepad button per player and slot, < 0 = none
  int pad_index[2] = {0, 1};    // which gamepad each player uses (0 = the first)
  std::vector<std::string> pads;  // connected gamepads, in the game's order
};
bool snapshot(Snapshot& out);

// Writes one player's keyboard keys into the game and saves its settings.
bool set_keys(int player, const int codes[tmnp::kNumSlots]);

enum class Preset { Original, TwoOnOneKeyboard };
bool apply_preset(Preset p);
bool matches(Preset p);  // both players' keys are exactly that preset

// Netplay lock ([mod] p2_lock): while a remote P2 is connected, the host's own
// P2 keys and P2 gamepad do nothing. Codes only P2 uses (shared keys like Esc
// stay), and whether a DirectInput device is the gamepad P2 plays with.
void p2_only_keys(std::vector<int>& out);
bool is_p2_gamepad(const void* device);

// Writes the game's settings (savesetting.dat) the way the game does.
void save_settings();

// The keyboard key (DIK code) a player's slot is bound to, -1 = none.
int key_for(int player, int slot);

// Mouse buttons as keys of the game's key table (keyconf.cpp): codes no
// keyboard has, pressed by the keyboard hook while the button is down
// (dinput_hook.cpp). kMouseKey + 0 left, 1 right, 2 middle, 3 side 1, 4 side 2.
constexpr int kMouseKey = 0xF0;
constexpr int kMouseButtons = 5;
// A code is bound to some keyboard slot of either player.
bool uses_key(int code);

// Readable key name for a DirectInput code ("Num 4", "Пробел", "Стрелка вверх").
std::string key_name(int dik);

// Gamepads as the game reads them: its device list holds every gamepad
// attached when it started, and it polls them all every frame before it makes
// the players' masks. How many, which one player 1 / 2 plays with (the
// game's key menu), one's name, and one's state this frame made the game's
// way (0x4b2839, 0x4b2660): key-table bits by a slot -> button table (the
// buttons, the stick past about 80%, the hat's eighths) and the sticks as the
// pad slots keep them (4 bytes, 0x80 = centre). Game thread.
int pad_count();
int pad_of(int player);  // 0 / 1; may be past pad_count()
std::string pad_name(int index);
bool pad_read(int index, const int buttons[tmnp::kNumSlots], uint16_t& bits, uint32_t& sticks);
// Player 0 / 1's gamepad button per slot in the game's key menu, < 0 = none.
void pad_buttons(int player, int out[tmnp::kNumSlots]);

}  // namespace binds
