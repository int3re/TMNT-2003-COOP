#include "mod/map_turtles.h"

#include <windows.h>
#include <intrin.h>

#include <cstring>

#include "common/log.h"
#include "common/net.h"
#include "mod/four_players.h"
#include "mod/patch.h"

namespace map_turtles {
namespace {

// The player records ([info], re/notes.md §19): the character of player p at
// +0xC8 + p*0x28 (0 nobody, 1..4 the turtles, 0x33 and 0x39 two more).
// 0x404740(p) reads it; +6 = &info.
constexpr const char* kCharacterSig = "8B 44 24 04 8B 0D ?? ?? ?? ?? 8D 44 80 19 8B 04 C1 C3";
constexpr size_t kInfoPtr = 6, kCharacterAt = 0xC8, kCharacterStep = 0x28;

// A map scene's set-up, 0x4a05f1(scene): the level select (task 0x4a2119) and
// the jump into a level (task 0x4a0371). It loads the common pieces and then
// the models of player 1's character, 0x493c92(character) (the call at +0x30).
constexpr const char* kLoadSig =
    "6A 00 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C8 08 50 E8 ?? ?? ?? ?? FF 74 24 0C E8 ?? ?? ?? ?? "
    "E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 00 E8";
constexpr size_t kLoadCall = 0x30;
// Its end, 0x4a062a(scene): 0x493cda(character) frees them (the call at +0xF).
constexpr const char* kFreeSig =
    "6A 00 E8 ?? ?? ?? ?? 6A 00 E8 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? FF 74 24 10 E8";
constexpr size_t kFreeCall = 0x0F;

// 0x4a07b0(scene, start): player 1's turtle. It calls its character's maker,
// maker(scene, player 0, start, turn), keeps the turtle in a global (mov at
// +0xBB), gives it the map's pad (mov [input + 0x30] at +0xDA) and the map's
// brain (push at +0xD4, set by 0x4a4a4b). Every frame the brain puts the
// turtle at the x and z the map wants (globals, movs at +0x13 and +0x39); its
// height is the jump's.
constexpr const char* kTurtleSig = "55 8B EC 83 EC 0C 56 D9 EE 8B 75 0C D9 5D F8 8B 06 D9 EE A3";
constexpr size_t kTargetX = 0x13, kTargetZ = 0x39, kTurtleStore = 0xBB, kBrainPush = 0xD4, kPadStore = 0xDA;
struct Maker {
  int character;
  size_t call;  // the maker's call inside 0x4a07b0
};
constexpr Maker kMakers[] = {{1, 0xB3}, {2, 0xA3}, {3, 0x93}, {4, 0x83}, {0x33, 0x73}, {0x39, 0x63}};

// A turtle (an actor): +0x60 its body; body +0x7C position (x, y up, z),
// +0x12C brain, +0x154 player data; player data +0x224 input, input +0x30 pad.
constexpr size_t kBody = 0x60, kPos = 0x7C, kBrain = 0x12C, kPlayerData = 0x154, kInput = 0x224, kPad = 0x30;

using Scene_t = void(__cdecl*)(void* scene);
using Turtle_t = void(__cdecl*)(void* scene, const float* start);
using Maker_t = uint8_t*(__cdecl*)(void* scene, int player, const float* start, const float* turn);
using Brain_t = int(__cdecl*)(uint8_t* turtle, void* ctx);
using Character_t = int(__cdecl*)(int character);

Scene_t oLoad = nullptr, oFree = nullptr;
Turtle_t oTurtle = nullptr;
Character_t g_load_character = nullptr, g_free_character = nullptr;
uint8_t** g_info = nullptr;
uint8_t** g_turtle = nullptr;  // player 1's map turtle
float* g_target_x = nullptr;
float* g_target_z = nullptr;
Brain_t g_brain = nullptr;  // the map's brain (0x4a06be)
uintptr_t g_pad = 0;        // the map's pad (0x4a09ac)
Maker_t g_makers[6] = {};

constexpr int kPlayers = 4;
float g_offset[2] = {-1.15f, 0.0f};  // player 2's turtle from player 1's (x, z): on its right
// Four players: a row on the card, player 1 on the left (the map's camera
// looks along +z: world -x is to the right on the screen).
constexpr float kRowStep = 0.85f;

// While a map scene runs: the character of each player's turtle on it (0:
// none; player 1's is the game's), the models this scene loaded for it (0:
// none or player 1's), and where it stands from the map's target (x, z).
int g_ch[kPlayers] = {};
int g_loaded[kPlayers] = {};
float g_place[kPlayers][2] = {};

struct Seen {
  float pos[3] = {};
  uint32_t at_ms = 0;
};
Seen g_seen[kPlayers];

uint8_t* body(uint8_t* turtle) { return *reinterpret_cast<uint8_t**>(turtle + kBody); }

int character(int player) {
  uint8_t* info = g_info ? *g_info : nullptr;
  return info ? *reinterpret_cast<int*>(info + kCharacterAt + player * kCharacterStep) : 0;
}

Maker_t maker(int ch) {
  for (size_t i = 0; i < sizeof(kMakers) / sizeof(kMakers[0]); ++i)
    if (kMakers[i].character == ch) return g_makers[i];
  return nullptr;
}

void seen(int player, uint8_t* turtle) {
  std::memcpy(g_seen[player].pos, body(turtle) + kPos, sizeof(g_seen[player].pos));
  g_seen[player].at_ms = net::now_ms() | 1;
}

// Each turtle goes where the map wants player 1's, at its place (the brain
// only reads the target: 0x4a067c and 0x4a07b0 set it).
int run_brain(int player, uint8_t* turtle, void* ctx) {
  float x = *g_target_x, z = *g_target_z;
  *g_target_x = x + g_place[player][0];
  *g_target_z = z + g_place[player][1];
  int r = g_brain(turtle, ctx);
  *g_target_x = x;
  *g_target_z = z;
  seen(player, turtle);
  return r;
}
template <int P>
int __cdecl brain(uint8_t* turtle, void* ctx) {
  return run_brain(P, turtle, ctx);
}
const Brain_t kBrains[kPlayers] = {&brain<0>, &brain<1>, &brain<2>, &brain<3>};

// Who stands where: two players as before (player 2 at [debug] map_offset from
// player 1); with four, a row centred on the card.
void place_turtles() {
  for (auto& p : g_place) p[0] = p[1] = 0;
  if (!four::enabled()) {
    g_place[1][0] = g_offset[0];
    g_place[1][1] = g_offset[1];
    return;
  }
  int n = 0;
  for (int p = 0; p < kPlayers; ++p) n += p == 0 || g_ch[p] != 0;
  for (int p = 0, i = 0; p < kPlayers; ++p) {
    if (p != 0 && !g_ch[p]) continue;
    g_place[p][0] = ((n - 1) * 0.5f - i) * kRowStep;
    ++i;
  }
}

void __cdecl hkLoad(void* scene) {
  oLoad(scene);
  for (int p = 0; p < kPlayers; ++p) g_ch[p] = g_loaded[p] = 0;
  int n = 2;
  if (four::enabled()) {
    four::choose_characters();  // (before the first level they have none yet)
    n = kPlayers;
  }
  int c1 = character(0);
  for (int p = 1; p < n; ++p) {
    int c = p < 2 ? character(p) : four::character(p);
    if (c <= 0 || !maker(c)) continue;
    if (c != c1) {
      g_load_character(c);  // (counted: each load has its free)
      g_loaded[p] = c;
    }
    g_ch[p] = c;
    LOGI("level map: player %d (character %d) gets a turtle on the map too", p + 1, c);
  }
  place_turtles();
}

void __cdecl hkFree(void* scene) {
  for (int p = 0; p < kPlayers; ++p) {
    if (g_loaded[p]) g_free_character(g_loaded[p]);
    g_ch[p] = g_loaded[p] = 0;
  }
  oFree(scene);
}

void __cdecl hkTurtle(void* scene, const float* start) {
  oTurtle(scene, start);
  uint8_t* p1 = *g_turtle;
  if (!p1) return;
  uint8_t* b1 = body(p1);
  if (*reinterpret_cast<Brain_t*>(b1 + kBrain) == g_brain) {
    *reinterpret_cast<void**>(b1 + kBrain) = (void*)kBrains[0];
    float* pos = reinterpret_cast<float*>(b1 + kPos);
    pos[0] += g_place[0][0];
    pos[2] += g_place[0][1];
  }
  const float turn[3] = {-180.0f, 0.0f, 0.0f};  // as player 1's: facing the camera
  for (int p = 1; p < kPlayers; ++p) {
    if (!g_ch[p]) continue;
    uint8_t* t = maker(g_ch[p])(scene, p, start, turn);
    if (!t) {
      LOGW("level map: player %d's turtle was not made", p + 1);
      continue;
    }
    uint8_t* b = body(t);
    uint8_t* data = *reinterpret_cast<uint8_t**>(b + kPlayerData);
    uint8_t* input = data ? *reinterpret_cast<uint8_t**>(data + kInput) : nullptr;
    if (input) *reinterpret_cast<uintptr_t*>(input + kPad) = g_pad;  // the same pad: it jumps with player 1's
    *reinterpret_cast<void**>(b + kBrain) = (void*)kBrains[p];
    float* pos = reinterpret_cast<float*>(b + kPos);
    pos[0] += g_place[p][0];
    pos[2] += g_place[p][1];
    LOGI("level map: player %d's turtle made at (%.2f %.2f %.2f), player 1's start (%.2f %.2f %.2f)", p + 1, pos[0],
         pos[1], pos[2], start[0], start[1], start[2]);
  }
}

uintptr_t call_target(uintptr_t at) {
  if (patch::read<uint8_t>(at) != 0xE8) return 0;
  uintptr_t t = at + 5 + patch::read<int32_t>(at + 1);
  return patch::in_image(t, 1) ? t : 0;
}

bool find_all(uintptr_t load, uintptr_t free_, uintptr_t turtle) {
  uintptr_t ch = patch::find("player_character", kCharacterSig);
  if (!ch || !load || !free_ || !turtle) return false;
  g_info = reinterpret_cast<uint8_t**>(patch::read<uint32_t>(ch + kInfoPtr));
  g_load_character = reinterpret_cast<Character_t>(call_target(load + kLoadCall));
  g_free_character = reinterpret_cast<Character_t>(call_target(free_ + kFreeCall));
  // Inside 0x4a07b0: the stores and pushes it is known by.
  auto u8 = [&](size_t off) { return patch::read<uint8_t>(turtle + off); };
  auto imm = [&](size_t off) { return patch::read<uint32_t>(turtle + off); };
  bool ok = u8(kTargetX) == 0xA3 && u8(kTargetZ) == 0xA3 && u8(kTurtleStore) == 0xA3 && u8(kBrainPush) == 0x68 &&
            u8(kPadStore) == 0xC7 && u8(kPadStore + 1) == 0x41 && u8(kPadStore + 2) == 0x30;
  for (size_t i = 0; ok && i < sizeof(kMakers) / sizeof(kMakers[0]); ++i) {
    size_t at = kMakers[i].call;
    ok = u8(at - 3) == 0xFF && u8(at - 2) == 0x75 && u8(at - 1) == 0x08;  // push [ebp+8] (the scene)
    g_makers[i] = reinterpret_cast<Maker_t>(call_target(turtle + at));
    ok = ok && g_makers[i];
  }
  if (!ok) {
    LOGW("level map: 0x4a07b0 is not laid out as expected; no second turtle");
    return false;
  }
  g_target_x = reinterpret_cast<float*>(imm(kTargetX + 1));
  g_target_z = reinterpret_cast<float*>(imm(kTargetZ + 1));
  g_turtle = reinterpret_cast<uint8_t**>(imm(kTurtleStore + 1));
  g_brain = reinterpret_cast<Brain_t>(imm(kBrainPush + 1));
  g_pad = imm(kPadStore + 3);
  return g_load_character && g_free_character && patch::in_image((uintptr_t)g_info, 4) &&
         patch::in_image((uintptr_t)g_target_x, 4) && patch::in_image((uintptr_t)g_target_z, 4) &&
         patch::in_image((uintptr_t)g_turtle, 4) && patch::in_image((uintptr_t)g_brain, 1) &&
         patch::in_image(g_pad, 1);
}

// [debug] stage=N (lab: the levels after the first without playing up to them):
// the story's stage is set by 0x4047d7(stage) ([info] +0x13C); the level map
// sets the chapter of its point (0x4a1f8a: push [point * 4 + 0x59a9c8]: 1, 6, 7,
// 13, 19, 20, 25, 30, 31; the stages between follow one another). That one call
// gets N instead. Every PC of a session has the same N, so they stay in step.
constexpr const char* kSetStageSig = "8B 44 24 04 8B 0D ?? ?? ?? ?? 89 81 3C 01 00 00 C3";
constexpr const char* kMapStageSig = "FF 34 85 ?? ?? ?? ?? E8 ?? ?? ?? ?? 59 59 8B 0D ?? ?? ?? ?? C7 41 30 04 00 00 00";
constexpr size_t kMapStageReturn = 12;
using SetStage_t = void(__cdecl*)(int stage);
SetStage_t oSetStage = nullptr;
uintptr_t g_map_stage_return = 0;
int g_debug_stage = 0;

void __cdecl hkSetStage(int stage) {
  if (g_debug_stage > 0 && reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_map_stage_return) {
    LOGI("level map: [debug] stage %d instead of %d", g_debug_stage, stage);
    stage = g_debug_stage;
  }
  oSetStage(stage);
}

}  // namespace

void install(const Config& cfg) {
  g_offset[0] = cfg.debug_map_offset[0];
  g_offset[1] = cfg.debug_map_offset[1];
  if (cfg.debug_stage > 0) {
    uintptr_t set = patch::find("set_story_stage", kSetStageSig), map = patch::find("map_stage_choice", kMapStageSig);
    if (set && map) {
      g_debug_stage = cfg.debug_stage;
      g_map_stage_return = map + kMapStageReturn;
      patch::hook("set_story_stage", set, (void*)&hkSetStage, (void**)&oSetStage);
    }
  }
  uintptr_t load = patch::find("map_load", kLoadSig), free_ = patch::find("map_free", kFreeSig),
            turtle = patch::find("map_turtle", kTurtleSig);
  if (!find_all(load, free_, turtle)) return;
  // Freeing first: models loaded for players 2..4 are always freed again.
  bool ok = patch::hook("map_free", free_, (void*)&hkFree, (void**)&oFree) &&
            patch::hook("map_turtle", turtle, (void*)&hkTurtle, (void**)&oTurtle) &&
            patch::hook("map_load", load, (void*)&hkLoad, (void**)&oLoad);
  if (ok)
    LOGI("level map: the other players' turtles ready (player 2 %.2f, %.2f from player 1's; four in a row %.2f apart)",
         g_offset[0], g_offset[1], kRowStep);
}

bool position(int player, float out[3]) {
  if (player < 0 || player >= kPlayers) return false;
  const Seen& s = g_seen[player];
  if (!s.at_ms || net::now_ms() - s.at_ms > 100) return false;
  std::memcpy(out, s.pos, sizeof(s.pos));
  return true;
}

}  // namespace map_turtles
