#include "mod/host_menus.h"

#include <cstdint>

#include "common/log.h"
#include "mod/lockstep.h"
#include "mod/four_players.h"
#include "mod/main_menu.h"
#include "mod/net_pause.h"
#include "mod/patch.h"
#include "mod/story_menu.h"

namespace host_menus {
namespace {

// A menu object's update (0x40f6f1(menu)): +4 its flags: bit 0 reads player
// 1's pad slot, bit 1 player 2's, bit 2 repeats held directions. The main menu
// and the story submenu read both (7); the character select has one menu a
// player (0x2001 / 0x2002). In a game together the main menu and the story
// submenu read only player 1's (the host's) slot.
constexpr const char* kMenuUpdateSig =
    "53 56 8B 74 24 0C 57 33 FF 33 DB F6 46 04 01 74 12 53 E8 ?? ?? ?? ?? 53 8B F8 E8";
constexpr size_t kMenuFlags = 0x04;
using MenuUpdate_t = int(__cdecl*)(uint8_t* menu);
MenuUpdate_t oMenuUpdate = nullptr;

bool together() { return lockstep::state() == lockstep::State::Running; }

int __cdecl hkMenuUpdate(uint8_t* menu) {
  // The pause's "are you sure?" (a level's only menu of this kind): answered
  // by the player who leads the pause.
  if (const int who = net_pause::question_of(); who >= 0 && menu) {
    four::read_slot0_as(who, 0, 0);
    const int r = oMenuUpdate(menu);
    four::read_slot0_as(-1, 0, 0);
    return r;
  }
  if (!together() || !menu || (menu != main_menu::menu_object() && menu != story_menu::menu_object()))
    return oMenuUpdate(menu);
  int32_t& flags = *reinterpret_cast<int32_t*>(menu + kMenuFlags);
  int32_t kept = flags;
  flags &= ~2;
  int r = oMenuUpdate(menu);
  flags = kept;
  return r;
}

// The level map reads player 2's slot, then player 1's, and takes both
// (0x4a1988, 0x4a1bd0: held; 0x4a1db8: pressed and held). Those reads of
// player 2's slot give nothing in a game together.
struct MapRead {
  const char* sig;
  size_t at;  // the call (E8) reading player 2's slot
};
const MapRead kMapReads[] = {
    {"83 C7 6C E8 ?? ?? ?? ?? 6A 00 8B E8 E8 ?? ?? ?? ?? 0B E8 8B 06 83 F8 0A", 3},
    {"83 C7 78 E8 ?? ?? ?? ?? 6A 00 8B E8 E8 ?? ?? ?? ?? 59 59 0B E8 8B 06", 3},
    {"6A 01 83 C3 5C E8 ?? ?? ?? ?? 6A 00 8B F0 E8 ?? ?? ?? ?? 6A 01 0B F0 E8 ?? ?? ?? ?? 6A 00 89 45 FC E8", 5},
    {"6A 01 83 C3 5C E8 ?? ?? ?? ?? 6A 00 8B F0 E8 ?? ?? ?? ?? 6A 01 0B F0 E8 ?? ?? ?? ?? 6A 00 89 45 FC E8", 0x17},
};
using Slot_t = uint32_t(__cdecl*)(int slot);
Slot_t g_read[4] = {};

template <int I>
uint32_t __cdecl map_read(int slot) {
  return slot == 1 && together() ? 0 : g_read[I](slot);
}
void* const kMapDetours[4] = {(void*)&map_read<0>, (void*)&map_read<1>, (void*)&map_read<2>, (void*)&map_read<3>};

}  // namespace

void install(const Config&) {
  if (uintptr_t a = patch::find("menu_update", kMenuUpdateSig))
    patch::hook("menu_update", a, (void*)&hkMenuUpdate, (void**)&oMenuUpdate);
  int maps = 0;
  for (int i = 0; i < 4; ++i) {
    uintptr_t a = patch::find("level_map_player2_read", kMapReads[i].sig);
    if (!a) continue;
    uintptr_t call = a + kMapReads[i].at;
    if (patch::read<uint8_t>(call) != 0xE8) continue;
    uintptr_t target = call + 5 + patch::read<int32_t>(call + 1);
    if (!patch::in_image(target, 1)) continue;
    g_read[i] = reinterpret_cast<Slot_t>(target);
    int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(kMapDetours[i]) - (call + 5));
    maps += patch::write(call + 1, &rel, 4) ? 1 : 0;
  }
  LOGI("host menus: in a game together the main menu, the story menu%s follow player 1 only",
       maps == 4 ? " and the level map" : " (the level map: not found)");
}

}  // namespace host_menus
