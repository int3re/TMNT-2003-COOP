#include "mod/team_progress.h"

#include <cstdint>
#include <cstdio>
#include <string>

#include "common/log.h"
#include "mod/four_players.h"
#include "mod/lockstep.h"
#include "mod/patch.h"

namespace team_progress {
namespace {

// TMNT.EXE bc49a82c...; each pattern has one match in .text (re/signatures.md).
// The character's record in the save: +4 flags (1 the story cleared, 2 / 4 / 8
// the three dojos' prizes, 0x10 its second costume), +0x20 the furthest stage.
constexpr const char* kPrizeSig[3] = {
    "55 8B EC 83 EC 30 8D 45 D0 50 FF 75 08 E8 ?? ?? ?? ?? 83 4D D4 02 8D 45 D0",   // 0x4109bf
    "55 8B EC 83 EC 30 8D 45 D0 50 FF 75 08 E8 ?? ?? ?? ?? 83 4D D4 04 8D 45 D0",   // 0x4109e6
    "55 8B EC 83 EC 30 8D 45 D0 50 FF 75 08 E8 ?? ?? ?? ?? 83 4D D4 08 8D 45 D0"};  // 0x410a0d
constexpr const char* kPrizeName[3] = {"dojo_prize_1", "dojo_prize_2", "dojo_prize_3"};
constexpr const char* kFurthestSig = "55 8B EC 83 EC 30 8D 45 D0 50 FF 75 08 E8 ?? ?? ?? ?? 8B 45 0C 39 45 F0";  // 0x410bae
constexpr const char* kOpensSig = "53 56 57 33 F6 33 FF E8 ?? ?? ?? ?? 33 DB 43 83 F8 18";                       // 0x40f939
constexpr const char* kSoloSig = "A1 ?? ?? ?? ?? 8B 80 3C 01 00 00 83 F8 06 74 0D 83 F8 13";                     // 0x40498a
constexpr const char* kConfigSig = "A1 ?? ?? ?? ?? 8B 80 54 11 00 00 C3";  // the progress ([0x60eae4]): the turtles' records at +0, 0x30 each

using Prize_t = void(__cdecl*)(int character);
using Furthest_t = void(__cdecl*)(int character, uint32_t stage);
using Opens_t = int(__cdecl*)();
using Solo_t = int(__cdecl*)();

Prize_t oPrize[3] = {nullptr, nullptr, nullptr};
Furthest_t oFurthest = nullptr;
Opens_t oOpens = nullptr;
Solo_t g_solo = nullptr;
uint8_t** g_config = nullptr;
bool g_on = false;  // the hooks are in place

// The turtles of the team in the stage just played, but player 1's (the
// game's own code is for it). The dojo is for player 1 alone outside a game
// together (four_players.cpp: solo_stage_for_makers), so it has no team there.
int others(int out[3]) {
  if (!g_on || !four::installed()) return 0;
  if (g_solo && g_solo() && !(lockstep::state() == lockstep::State::Running && lockstep::players() > 1)) return 0;
  const int first = four::character(0);
  int n = 0;
  for (int p = 1; p < 4; ++p) {
    const int c = four::character(p);
    if (four::plays(p) && c != first) out[n++] = c;
  }
  return n;
}

const char* list(const int* team, int n) {
  static char text[48];
  int at = 0;
  text[0] = 0;
  for (int i = 0; i < n; ++i) at += std::snprintf(text + at, sizeof(text) - at, i ? ", %d" : "%d", team[i]);
  return text;
}

// The four turtles' records for the log: flags and furthest stage.
const char* records() {
  static char text[96];
  text[0] = 0;
  if (!g_config || !*g_config) return text;
  int at = 0;
  for (int c = 1; c <= 4; ++c) {
    const uint8_t* rec = *g_config + (c - 1) * 0x30;
    at += std::snprintf(text + at, sizeof(text) - at, "%s%d: flags 0x%X, stage %d", c > 1 ? "; " : "",
                        c, *reinterpret_cast<const uint32_t*>(rec + 4), *reinterpret_cast<const int32_t*>(rec + 0x20));
  }
  return text;
}

// The dojo's prize: the dojo calls it with the character it took from player
// 1 at its start, when the dojo is passed (0x491201).
void prize(int which, int character) {
  oPrize[which](character);
  if (character != four::character(0)) return;
  int team[3];
  const int n = others(team);
  for (int i = 0; i < n; ++i) oPrize[which](team[i]);
  if (n)
    LOGI("team progress: dojo %d's prize goes to the team's other turtles too (characters %s); the records: %s", which + 1,
         list(team, n), records());
}
void __cdecl hkPrize1(int character) { prize(0, character); }
void __cdecl hkPrize2(int character) { prize(1, character); }
void __cdecl hkPrize3(int character) { prize(2, character); }

// "This character has played the story up to this stage" (the story's results
// 0x494aaf and the dojo's end 0x491395, both with player 1's character).
void __cdecl hkFurthest(int character, uint32_t stage) {
  oFurthest(character, stage);
  if (character != four::character(0)) return;
  int team[3];
  const int n = others(team);
  for (int i = 0; i < n; ++i) oFurthest(team[i], stage);
  if (n) LOGI("team progress: stage %u is played by the team (characters %d, %s); the records: %s", stage, character, list(team, n), records());
}

// What the stage just played opens (the story's results 0x494d14, the dojo's
// 0x490d4b and 0x4912bf): by the stage and player 1's character; the flags
// it returns are the notices shown next (0x49d107: one per bit). Asked again
// as each other turtle of the team; the notices add up.
int __cdecl hkOpens() {
  int flags = oOpens();
  int team[3];
  const int n = others(team);
  if (!n) return flags;
  const std::string before = records();
  int more = 0;
  for (int i = 0; i < n; ++i) {
    four::answer_first_as(team[i]);
    more |= oOpens();
    four::answer_first_as(0);
  }
  if ((more & ~flags) || before != records())
    LOGI("team progress: the stage opens more for the team's other turtles (characters %s): notices 0x%X, with them 0x%X; "
         "the records: %s",
         list(team, n), flags, flags | more, records());
  return flags | more;
}

}  // namespace

void install(const Config& cfg) {
  if (!cfg.debug_team_progress) {
    LOGI("team progress (lab): off; a stage's prizes go to player 1's turtle alone, as in the game");
    return;
  }
  uintptr_t prize[3], furthest = patch::find("furthest_stage", kFurthestSig), opens = patch::find("stage_opens", kOpensSig);
  bool found = furthest && opens;
  for (int i = 0; i < 3; ++i) found = (prize[i] = patch::find(kPrizeName[i], kPrizeSig[i])) != 0 && found;
  if (const uintptr_t solo = patch::find("solo_stage", kSoloSig)) g_solo = reinterpret_cast<Solo_t>(solo);
  if (const uintptr_t config = patch::find("config_ptr", kConfigSig)) {
    const uintptr_t holder = patch::read<uint32_t>(config + 1);
    if (patch::in_image(holder, 4)) g_config = reinterpret_cast<uint8_t**>(holder);
  }
  if (!found || !g_solo) {
    LOGW("team progress: game code not recognised; a stage's prizes go to player 1's turtle alone, as in the game");
    return;
  }
  void* const detour[3] = {(void*)&hkPrize1, (void*)&hkPrize2, (void*)&hkPrize3};
  int ok = 0;
  for (int i = 0; i < 3; ++i) ok += patch::hook(kPrizeName[i], prize[i], detour[i], (void**)&oPrize[i]);
  ok += patch::hook("furthest_stage", furthest, (void*)&hkFurthest, (void**)&oFurthest);
  ok += patch::hook("stage_opens", opens, (void*)&hkOpens, (void**)&oOpens);
  g_on = ok == 5;
  if (!g_on) LOGW("team progress: %d of 5 hooks in place", ok);
  LOGI("team progress: the dojo's prizes, the story's progress and what a stage opens go to every turtle that played it");
}

void debug_dojo_pass() {
  const uintptr_t pass = patch::find("dojo_passed", "33 C0 40 50 A3 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 00 FF 35 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 0C E8");
  if (!pass || !g_solo || !g_solo()) {
    LOGW("team progress (lab): not in a dojo, or its code is not recognised; nothing done");
    return;
  }
  LOGI("team progress (lab): the dojo is passed now (the game's own routine); the records: %s", records());
  reinterpret_cast<void(__cdecl*)()>(pass)();
}

}  // namespace team_progress
