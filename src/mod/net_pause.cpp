#include "mod/net_pause.h"

#include <cstdint>
#include <string>

#include "common/log.h"
#include "mod/four_players.h"
#include "mod/lockstep.h"
#include "mod/native_menu.h"
#include "mod/patch.h"

namespace net_pause {
namespace {

// --- the game (TMNT.EXE bc49a82c..., re/notes.md §34) ------------------------------
// The level's round asks 0x411332 (the task in ebx) every frame of play: with
// player 1 or 2 in play and 0x4b0d74(1, 1, 0) (key 1 = Esc newly down on
// device 0, the keyboard, read raw) it answers 3 = pause: 0x4042a3, 0x4119db
// (the scene stops, the level task's state becomes 0x41181d).
// Here: push edi; push esi; push esi; call 0x4b0d74; ...; push 3.
constexpr const char* kAskSig = "57 56 56 E8 ?? ?? ?? ?? 83 C4 0C 85 C0 74 07 6A 03";
constexpr size_t kAskCall = 3;
// The pause state 0x41181d makes the pause menu (0x41036b(kind): a task, its
// data at [0x60eadc]: +0 kind (0 the story's, 3 rows), +4 state (2 the rows
// are read, 3 a row was taken, 4 "are you sure?", 5 over), +8 the row, +0x10
// the row taken, +0x14 the result) and asks its result every frame
// (0x40fdef): 1 back to the game, 2 leave the level (flag 0x200), 3 quit the
// game (0x4b330d: PostQuitMessage). Here: the call of 0x40fdef and the jumps
// by its value.
constexpr const char* kResultSig =
    "E8 ?? ?? ?? ?? 8B F8 48 74 64 48 74 35 48 74 0C 48 74 5B 48 74 0D 48 74 29 48 75 63 E8";
// The menu's step (0x4100ce): the buttons newly down in pad slots 1 and 0
// together (0x4a9595: four_players.cpp's hook), then by the state; in state 2:
// decide 0x10000 takes the row (result 1 + row; rows 1 and 2 ask "are you
// sure?" first: a menu of the common kind, 0x40f6f1), cancel 0x20000 or start
// 0x800 goes back to the game, up 0x1000 / down 0x4000 move.
constexpr const char* kMenuSig =
    "55 8B EC 83 EC 10 A1 ?? ?? ?? ?? 53 56 33 F6 57 46 56 89 45 FC E8 ?? ?? ?? ?? 6A 00 8B D8 E8 ?? ?? ?? ?? 59 59 0B "
    "D8 A1";
constexpr size_t kMenuHolder = 0x28;  // (A1 <address of the menu's data pointer>)
constexpr size_t kMenuKind = 0x00, kMenuState = 0x04, kMenuRow = 0x08;
constexpr int kStory = 0, kRows = 2, kQuestion = 4;
constexpr int kRowLeave = 1;
constexpr int kBack = 1, kLeave = 2, kQuit = 3;
constexpr uint32_t kDecide = 0x10000, kCancel = 0x20000;

using RawKey_t = int(__cdecl*)(int key, int kind, int device);
using Result_t = int(__cdecl*)();
using Step_t = void(__cdecl*)(void* data);
RawKey_t g_raw_key = nullptr;
Result_t g_result = nullptr;
Step_t oMenuStep = nullptr;
uint8_t** g_menu = nullptr;

int g_owner = -1;      // the player who paused (a game together), -1: no pause of ours
bool g_question = false;
bool g_told = false;   // "only the host" said in this pause
int g_steps = 0;       // the menu's steps in this pause

bool together() { return lockstep::state() == lockstep::State::Running; }
const uint8_t* menu() { return g_menu ? *g_menu : nullptr; }
int32_t i32(const uint8_t* p, size_t at) { return *reinterpret_cast<const int32_t*>(p + at); }

// In place of the round's raw read of Esc.
int __cdecl pause_asked(int key, int kind, int device) {
  if (!together()) return g_raw_key(key, kind, device);
  const int by = lockstep::pause_asked_by();
  if (by < 0) return 0;
  g_owner = by;
  g_question = g_told = false;
  g_steps = 0;
  LOGI("pause: player %d stops the game for everybody (frame %u)", by + 1, lockstep::frame());
  native_menu::notify("ПАУЗА ИГРОКА", std::to_string(by + 1));  // (the value's font has no Russian letters)
  return 1;
}

void __cdecl hkMenuStep(void* data) {
  if (!together() || g_owner < 0) {
    oMenuStep(data);
    return;
  }
  const uint8_t* m = menu();
  uint32_t without = 0, with = 0;
  if (m && i32(m, kMenuKind) == kStory && i32(m, kMenuState) == kRows) {
    // Out of the level goes everybody: the host's to say.
    if (g_owner != 0 && i32(m, kMenuRow) == kRowLeave) {
      without = kDecide;
      if (!g_told && (four::pressed(g_owner) & kDecide)) {
        g_told = true;
        native_menu::notify("В МЕНЮ ВСЕХ УВОДИТ ИГРОК", "1");  // (the menus' letters have no Щ: not "ВЕДУЩИЙ")
      }
    }
    // Anybody's pause key again: back to the game.
    if (g_steps > 20 && lockstep::pause_asked_by() >= 0) with = kCancel;
  }
  ++g_steps;
  four::read_slot0_as(g_owner, without, with);
  oMenuStep(data);
  four::read_slot0_as(-1, 0, 0);
  m = menu();
  g_question = m && i32(m, kMenuState) == kQuestion;
}

// In place of the pause state's read of the menu's result.
int __cdecl pause_result() {
  const int r = g_result();
  if (!r || !together() || g_owner < 0) return r;
  const uint8_t* m = menu();
  int out = r;
  if (m && i32(m, kMenuKind) == kStory && r == kQuit) {
    if (g_owner == lockstep::my_player()) {
      LOGI("pause: this PC's player leaves the game");
    } else {
      out = kBack;  // (that player's game closes; this one goes on)
      native_menu::notify("ИГРУ ПОКИДАЕТ ИГРОК", std::to_string(g_owner + 1));
    }
  }
  LOGI("pause: over at frame %u: %s (player %d's choice)", lockstep::frame(),
       r == kBack ? "back to the game" : r == kLeave ? "everybody leaves the level" : r == kQuit ? "quit" : "?",
       g_owner + 1);
  g_owner = -1;
  g_question = false;
  return out;
}

}  // namespace

void install(const Config&) {
  const uintptr_t ask = patch::find("pause_asked", kAskSig), result = patch::find("pause_result", kResultSig);
  const uintptr_t step = patch::find("pause_menu_step", kMenuSig);
  auto target_of = [](uintptr_t call) { return call + 5 + patch::read<int32_t>(call + 1); };
  if (!ask || !result || !step || !four::installed()) {
    LOGW("pause: the game's pause is not where expected; none in a game together, as before");
    return;
  }
  const uintptr_t raw_key = target_of(ask + kAskCall), menu_result = target_of(result);
  const uintptr_t holder = patch::read<uint32_t>(step + kMenuHolder);
  if (!patch::in_image(raw_key, 1) || !patch::in_image(menu_result, 1) || !patch::in_image(holder, 4)) {
    LOGW("pause: the game's pause is not as expected; none in a game together, as before");
    return;
  }
  g_raw_key = reinterpret_cast<RawKey_t>(raw_key);
  g_result = reinterpret_cast<Result_t>(menu_result);
  g_menu = reinterpret_cast<uint8_t**>(holder);
  if (!patch::hook("pause_menu_step", step, (void*)&hkMenuStep, (void**)&oMenuStep)) return;
  const int32_t rel_ask = (int32_t)(reinterpret_cast<uintptr_t>(&pause_asked) - (ask + kAskCall + 5));
  const int32_t rel_result = (int32_t)(reinterpret_cast<uintptr_t>(&pause_result) - (result + 5));
  const bool ok = patch::write(ask + kAskCall + 1, &rel_ask, 4) && patch::write(result + 1, &rel_result, 4);
  LOGI("pause: in a game together any player's pause key stops every game at once, its menu is that player's%s",
       ok ? "" : ": not in place");
}

int question_of() { return together() && g_question ? g_owner : -1; }
bool up() { return together() && g_owner >= 0; }

}  // namespace net_pause
