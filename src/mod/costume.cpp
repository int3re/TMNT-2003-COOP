#include "mod/costume.h"

#include <algorithm>
#include <cstdint>

#include "common/log.h"
#include "mod/four_players.h"
#include "mod/lockstep.h"
#include "mod/patch.h"

namespace costume {
namespace {

// The game's costume code (TMNT.EXE bc49a82c..., re/notes.md §30):
// - 0x404b45(player, character, costume): the story's players' record
//   ([info] + player * 0x28: +0xE0 costume, +0xE4 character), written when the
//   character select ends (0x4953af: costume = 0x404b0e(its "ex" flag));
//   0x404b6a(player, character): that costume if the player has that
//   character, else 0 (asked with the player of a turtle in play: Michelangelo's
//   second costume has a thing of its own, 0x46a57a). Both are hooked by
//   four_players.cpp (players 3 and 4 are not in that record), which asks
//   worn() here;
// - 0x4348e6(character, costume): the TMNT.DAT file of that turtle's pack
//   (tables of {file, 0, 1} words: 0x5e22b8 Raphael, 0x5e2270 Michelangelo,
//   0x5e2294 Donatello, 0x5e224c Leonardo), for every turtle of a stage: the
//   game gives it the costumes of players 1 and 2 only (0x434812);
// - 0x404b0e(ex): the costume the game gives: ex (0 / 1), or on the PC's
//   date ([0x60a440] 31 October, [0x60a43c] 24 / 25 December, set once at the
//   start by 0x404be8) 3 / 5 (hallow) or 2 / 4 (santa).
constexpr const char* kFileSig =
    "55 8B EC 8B 45 08 48 74 3E 48 74 2B 48 74 18 48 74 05 66 33 C0 5D C3 8B 45 0C 8D 04 40 66 8B 04 45";
constexpr const char* kSeasonSig =
    "8B 44 24 04 33 D2 39 15 ?? ?? ?? ?? 74 10 8B C8 2B CA 74 07 49 75 07 6A 05 EB 02 6A 03 58 39 15";

using File_t = int(__cdecl*)(int character, int costume);  // (the file in ax)
using Season_t = int(__cdecl*)(int ex);
File_t oFile = nullptr;
Season_t g_season = nullptr;

int g_worn[5] = {-1, -1, -1, -1, -1};  // by character (1..4): the costume picked, -1 the game's own
bool g_any = false;
int g_debug_default = 0;

bool turtle(int character) { return character >= 1 && character <= 4; }
int picked(int character) { return g_any && turtle(character) ? g_worn[character] : -1; }

int __cdecl hkFile(int character, int costume) {
  int c = picked(character);
  return oFile(character, c >= 0 ? c : costume);
}

}  // namespace

void install(const Config& cfg) {
  g_debug_default = std::clamp(cfg.debug_costume_default, 0, kCount - 1);
  uintptr_t file = patch::find("costume_file", kFileSig), season = patch::find("costume_of_the_day", kSeasonSig);
  if (!file || !four::installed()) {  // (the players' record: four_players.cpp's hooks)
    LOGW("costume: the game's costume code is not where expected; costumes as in the game");
    return;
  }
  if (!patch::hook("costume_file", file, (void*)&hkFile, (void**)&oFile)) {
    LOGW("costume: hook not in place; costumes as in the game");
    oFile = nullptr;
    return;
  }
  g_season = reinterpret_cast<Season_t>(season);
  LOGI("costume: every turtle's six costumes can be picked in the character select");
}

bool ready() { return oFile != nullptr; }

int worn(int character) { return picked(character); }

const char* name(int index) {
  static const char* const kNames[kCount] = {"ОБЫЧНЫЙ", "ОСОБЫЙ", "НОВОГОДНИЙ", "ХЭЛЛОУИН", "ОСОБЫЙ НОВОГОДНИЙ",
                                             "ОСОБЫЙ ХЭЛЛОУИН"};
  return kNames[std::clamp(index, 0, kCount - 1)];
}

int initial() {
  if (g_debug_default) return g_debug_default;
  // In a game together every PC starts on the same one, whatever its date.
  if (lockstep::net_active() || !g_season) return 0;
  return std::clamp(g_season(0), 0, kCount - 1);
}

void set(int character, int index) {
  if (!turtle(character)) return;
  g_worn[character] = std::clamp(index, 0, kCount - 1);
  g_any = true;
}

void clear() {
  if (!g_any) return;
  for (int& w : g_worn) w = -1;
  g_any = false;
}

}  // namespace costume
