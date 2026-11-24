#include "mod/four_players.h"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/log.h"
#include "common/net.h"
#include "mod/binds.h"
#include "mod/costume.h"
#include "mod/lockstep.h"
#include "mod/main_menu.h"
#include "mod/own_camera.h"
#include "mod/patch.h"
#include "mod/revive.h"

namespace four {
namespace {

// --- the game's player table (re/notes.md §19, §20) ----------------------------
// [info] (0x60a438, 0x1D0 bytes, every access in 0x404000..0x405000): +0 the
// number of players; per player 3 ints at +0x28 + p*12; per player 0x44 bytes
// at +0x40 (+0x40 player data, +0x44 turtle, +0x48 position, +0x54 last
// position, +0x60 health ...); per player 0x28 bytes at +0xC8 (+0xC8 character,
// +0xE0/+0xE4, +0xE8, +0xEC takes part); +0x154/+0x158 a flag per player.
// Players 3 and 4 (p = 2, 3) live here instead, in the same layout.
struct Extra {
  uint8_t rec[0x44];   // as at +0x40 + p*0x44
  uint8_t ch[0x28];    // as at +0xC8 + p*0x28
  int32_t setting[3];  // as at +0x28 + p*12
  int32_t flag = 0;    // as at +0x154 / +0x158
};
Extra g_extra[2];
uint8_t** g_info = nullptr;
bool g_on = false;  // three or four
int g_count = 2;    // how many play the story
bool g_installed = false;
int g_keys[2][16];  // [mod] p3_keys / p4_keys
int g_pad_cfg[2] = {-1, -1};  // [mod] p3_pad / p4_pad

bool extra(int p) { return p == 2 || p == 3; }

// Lab ([debug] actions=1): the game's code that reads players 3 and 4's
// records, once per caller and field.
bool g_trace_access = false;
void trace_access(size_t off, int p, void* caller) {
  struct Seen {
    void* caller;
    size_t off;
  };
  static Seen seen[256];
  static int n = 0;
  for (int i = 0; i < n; ++i)
    if (seen[i].caller == caller && seen[i].off == off) return;
  if (n >= 256) return;
  seen[n++] = {caller, off};
  LOGI("trace: record +0x%X of player %d read by %p", (unsigned)off, p + 1, caller);
}

// The camera's "players 1 and 2" while it is aimed at all four (see the camera below).
bool g_box_on = false;
float g_box[2][3];
// The camera's code runs for this player alone (own_camera.cpp: each player's
// own view); -1: for all, as the game does.
int g_solo = -1;
int32_t& rec32(int p, size_t game_off) {
  return *reinterpret_cast<int32_t*>(g_extra[p - 2].rec + (game_off - 0x40));
}
int32_t& ch32(int p, size_t game_off) {
  return *reinterpret_cast<int32_t*>(g_extra[p - 2].ch + (game_off - 0xC8));
}
float* rec_vec(int p, size_t game_off) { return reinterpret_cast<float*>(&rec32(p, game_off)); }

// The accessors, by kind. One instance per hooked function (its original in `orig`).
// ... and enemies' code (0x440000..0x490000) that reads players 1 and 2.
inline void trace_enemy(size_t off, int p, void* caller) {
  uintptr_t c = reinterpret_cast<uintptr_t>(caller);
  if (g_trace_access && c >= 0x440000 && c < 0x490000) trace_access(off, p, caller);
}

template <size_t Off>
struct RecGet {
  static inline int(__cdecl* orig)(int) = nullptr;
  static int __cdecl hook(int p) {
    if (!extra(p)) {
      trace_enemy(Off, p, _ReturnAddress());
      return orig(p);
    }
    if (g_trace_access) trace_access(Off, p, _ReturnAddress());
    return rec32(p, Off);
  }
};
template <size_t Off>
struct RecSet {
  static inline void(__cdecl* orig)(int, int) = nullptr;
  static void __cdecl hook(int p, int v) {
    if (!extra(p)) return orig(p, v);
    rec32(p, Off) = v;
  }
};
template <size_t Off, bool AtLeastZero>
struct RecAdd {
  static inline void(__cdecl* orig)(int, int) = nullptr;
  static void __cdecl hook(int p, int v) {
    if (!extra(p)) return orig(p, v);
    int32_t& f = rec32(p, Off);
    f += v;
    if (AtLeastZero && f < 0) f = 0;
  }
};
template <size_t Off>
struct RecPtr {
  static inline void*(__cdecl* orig)(int) = nullptr;
  static void* __cdecl hook(int p) {
    if constexpr (Off == 0x48)
      if (g_box_on && (p == 0 || p == 1)) return g_box[p];
    if (g_solo >= 0 && p == 0) p = g_solo;  // (the camera's stretch of the stage goes by "player 1": 0x4030ec)
    if (!extra(p)) {
      trace_enemy(Off, p, _ReturnAddress());
      return orig(p);
    }
    if (g_trace_access) trace_access(Off, p, _ReturnAddress());
    return (void*)&rec32(p, Off);
  }
};
// While set: the game hears this character for player 1 (team_progress.cpp
// runs the game's "what does this stage open" once for every turtle of the
// team; the game asks it for player 1 alone).
int g_first_as = 0;
template <size_t Off>
struct ChGet {
  static inline int(__cdecl* orig)(int) = nullptr;
  static int __cdecl hook(int p) {
    if (Off == 0xC8 && p == 0 && g_first_as) return g_first_as;
    return extra(p) ? ch32(p, Off) : orig(p);
  }
};
template <size_t Off>
struct ChSet {
  static inline void(__cdecl* orig)(int, int) = nullptr;
  static void __cdecl hook(int p, int v) {
    if (!extra(p)) return orig(p, v);
    ch32(p, Off) = v;
  }
};

// The ones of a kind of their own.
int(__cdecl* oActive)(int) = nullptr;
int __cdecl hkActive(int p) {
  if (g_box_on && (p == 0 || p == 1)) return 1;  // (the camera counts them first: 0x402f8f)
  if (!extra(p)) {
    trace_enemy(0x1, p, _ReturnAddress());
    return oActive(p);
  }
  if (g_trace_access) trace_access(0x1, p, _ReturnAddress());  // (+1: "in play")
  return rec32(p, 0x40) != 0;
}

void(__cdecl* oInc64)(int) = nullptr;
void __cdecl hkInc64(int p) {
  if (!extra(p)) return oInc64(p);
  ++rec32(p, 0x64);
}

void(__cdecl* oSet54)(int, const float*) = nullptr;
void __cdecl hkSet54(int p, const float* v) {
  if (!extra(p)) return oSet54(p, v);
  std::memcpy(rec_vec(p, 0x54), v, 12);
}

void(__cdecl* oSetPos)(int, const float*) = nullptr;  // 0x404e41: last position, position, "current player"
void __cdecl hkSetPos(int p, const float* v) {
  if (!extra(p)) return oSetPos(p, v);
  std::memcpy(rec_vec(p, 0x54), rec_vec(p, 0x48), 12);
  std::memcpy(rec_vec(p, 0x48), v, 12);
  *reinterpret_cast<int32_t*>(*g_info + 0x12C) = p;
}

void(__cdecl* oSetE0E4)(int, int, int) = nullptr;
void __cdecl hkSetE0E4(int p, int key, int value) {
  // (key: the player's character, value: its costume: the one picked in the
  // character select, costume.cpp)
  if (int worn = costume::worn(key); worn >= 0) {
    if (worn != value) LOGI("costume: player %d's turtle %d wears costume %d (the game had %d)", p + 1, key, worn, value);
    value = worn;
  }
  if (!extra(p)) return oSetE0E4(p, key, value);
  ch32(p, 0xE4) = key;
  ch32(p, 0xE0) = value;
}
int(__cdecl* oGetE0E4)(int, int) = nullptr;
int __cdecl hkGetE0E4(int p, int key) {
  if (!extra(p)) return oGetE0E4(p, key);
  if (int worn = costume::worn(key); worn >= 0 && ch32(p, 0xC8) == key) return worn;
  return ch32(p, 0xE4) == key ? ch32(p, 0xE0) : 0;
}

int(__cdecl* oSetting)(int, int) = nullptr;
int __cdecl hkSetting(int p, int k) {
  if (!extra(p) || k < 0 || k > 2) return oSetting(p, k);
  return g_extra[p - 2].setting[k];
}
void(__cdecl* oSetSetting)(int, int, int) = nullptr;
void __cdecl hkSetSetting(int p, int k, int v) {
  if (!extra(p) || k < 0 || k > 2) return oSetSetting(p, k, v);
  g_extra[p - 2].setting[k] = v;
}

int(__cdecl* oFlag)(int) = nullptr;
int __cdecl hkFlag(int p) { return extra(p) ? g_extra[p - 2].flag : oFlag(p); }

// Loops over the players: with players 3 and 4 in them.
int players() { return *g_info ? *reinterpret_cast<int32_t*>(*g_info) : 0; }
uint8_t* game_rec(int p) { return *g_info + 0x40 + p * 0x44; }
int32_t data_of(int p) { return extra(p) ? rec32(p, 0x40) : *reinterpret_cast<int32_t*>(game_rec(p)); }
const float* pos_of(int p) {
  return extra(p) ? rec_vec(p, 0x48) : reinterpret_cast<const float*>(game_rec(p) + 8);
}
int char_of(int p) {
  return extra(p) ? ch32(p, 0xC8) : *reinterpret_cast<int32_t*>(*g_info + 0xC8 + p * 0x28);
}

// 0x4043ae (players in play) stays the game's: its callers mean "two players"
// by 2 (the camera's zoom 0x403735 takes players 1 and 2 then).

using Length_t = float(__cdecl*)(const float* v);
Length_t g_length = nullptr;  // 0x4b8d10, the game's vector length

// 0x40456e(out_vec, out_dist, point): the nearest player to a point (enemies'
// targets); returns its number. As the game's, over up to four.
// Lab ([debug] actions=1): who asks for the nearest player and gets player 3 or 4.
bool g_trace_nearest = false;
void trace_nearest(void* caller, int p, float d) {
  struct Seen {
    void* caller;
    uint32_t ms;
  };
  static Seen seen[64];
  static int n = 0;
  uint32_t now = GetTickCount();
  for (int i = 0; i < n; ++i)
    if (seen[i].caller == caller) {
      if (now - seen[i].ms < 3000) return;
      seen[i].ms = now;
      LOGI("trace: nearest for %p -> player %d (%.2f)", caller, p + 1, d);
      return;
    }
  if (n < 64) seen[n++] = {caller, now};
  LOGI("trace: nearest for %p -> player %d (%.2f)", caller, p + 1, d);
}

// A turtle lying stunned (revive.cpp) is nobody's target.
int(__cdecl* oNearest)(float*, float*, const float*) = nullptr;
int __cdecl hkNearest(float* out_vec, float* out_dist, const float* point) {
  if (!g_on && !revive::any_down()) return oNearest(out_vec, out_dist, point);
  int best = -1, n = 0;
  float best_d = 0, best_v[3] = {};
  auto consider = [&](int p) {
    if (!data_of(p) || revive::down(p)) return;
    const float* pos = pos_of(p);
    float v[3] = {pos[0] - point[0], pos[1] - point[1], pos[2] - point[2]};
    float d = g_length(v);
    ++n;
    if (best < 0 || d < best_d) best = p, best_d = d, std::memcpy(best_v, v, 12);
  };
  for (int p = 0; p < players() && p < 2; ++p) consider(p);
  if (g_on)
    for (int p = 2; p < 4; ++p) consider(p);
  if (best < 0) return oNearest(out_vec, out_dist, point);
  if (out_vec) std::memcpy(out_vec, best_v, 12);
  if (out_dist) *out_dist = best_d;
  if (g_trace_nearest && (best >= 2 || reinterpret_cast<uintptr_t>(_ReturnAddress()) >= 0x440000))
    trace_nearest(_ReturnAddress(), best, best_d);
  return best;
}

// 0x4186e7(object data): "a player's turtle?": its character (data +0) is
// one of players 1..count's (count = 2, the game's). A hit between a turtle
// and an enemy only counts when this says so (0x418715 in 0x419c22's check):
// players 3 and 4 were neither hurt by enemies nor hurt them (a mouser's bite
// on player 3 was dropped as "one enemy on another").
int(__cdecl* oIsPlayer)(const int32_t*) = nullptr;
int __cdecl hkIsPlayer(const int32_t* data) {
  int r = oIsPlayer(data);
  if (r || !g_on || !data || !g_info || !*g_info) return r;
  for (int p = 2; p < 4; ++p)
    if (char_of(p) && char_of(p) == *data) return 1;
  return 0;
}

// 0x404642(out): the position of the first player who is in play.
void(__cdecl* oFirstPos)(float*) = nullptr;
void __cdecl hkFirstPos(float* out) {
  if (!g_on) return oFirstPos(out);
  for (int p = 0; p < 4; ++p) {
    if (p < 2 && p >= players()) continue;
    if (data_of(p) && char_of(p)) {
      std::memcpy(out, pos_of(p), 12);
      return;
    }
  }
  out[0] = out[1] = out[2] = 0;
}

// A level's start (0x404cad, from the level task's init 0x411db5) clears the
// players' records, but for the score a turtle's end left there for the next
// level (+0x68) and the three ints at +0x28 (the shurikens); the characters
// (chosen before it) stay too. (Until 0.13.8 all of players 3 and 4's record
// went: a real game, "player 3's score is 0 after every level".)
void(__cdecl* oResetGame)() = nullptr;
void __cdecl hkResetGame() {
  oResetGame();
  for (Extra& e : g_extra) {
    const int32_t score = *reinterpret_cast<int32_t*>(e.rec + (0x68 - 0x40));
    std::memset(e.rec, 0, sizeof(e.rec));
    *reinterpret_cast<int32_t*>(e.rec + (0x68 - 0x40)) = score;
    e.flag = 0;
  }
}

// A new run (0x404c8f: the story's start 0x4945ed and 0x494781, the dojo's
// 0x490c09 and 0x491189): the score kept between levels is 0 again and the
// shurikens are {10, 0, 0} (0x404c3f), for players 1 and 2.
void(__cdecl* oNewRun)() = nullptr;
void __cdecl hkNewRun() {
  oNewRun();
  for (Extra& e : g_extra) {
    *reinterpret_cast<int32_t*>(e.rec + (0x68 - 0x40)) = 0;
    e.setting[0] = 10, e.setting[1] = e.setting[2] = 0;
  }
}

// The stages for one turtle (0x40498a: stages 6, 19 and 30, the dojo): the
// level makes player 1 alone there (0x411b7b: 0x4117c9 instead of 0x4115d9).
// In a game together everybody takes part: the level's own choice hears "not
// such a stage" (the other places that ask keep their answer: the dojo's round
// and its panel).
using Solo_t = int(__cdecl*)();
Solo_t g_solo_stage = nullptr;
int __cdecl solo_stage_for_makers() {
  const int solo = g_solo_stage();
  if (solo && lockstep::state() == lockstep::State::Running && lockstep::players() > 1) {
    LOGI("four players: a stage for one turtle: in a game together everybody takes part");
    return 0;
  }
  return solo;
}

// The level's results (the story's flow, 0x494cce): player 1, then player 2
// if it takes part and has a character, get the level's bonus: 0x4949f1
// (player in esi) adds 0x49496c(player) (the time under the level's par, the
// record's +0x80 and +0x64) to the record's +0x6C and to the score kept for
// the next level, +0x68. Players 3 and 4 get theirs the same way, before
// player 1's (the first question there, "does player 1 take part", is asked
// through here).
using TakesPart_t = int(__cdecl*)(int);
TakesPart_t g_results_takes_part = nullptr;
void* g_level_bonus = nullptr;
int __cdecl results_takes_part(int p) {
  if (g_on && g_level_bonus)
    for (int x = 2; x < 4; ++x) {
      if (x >= g_count || !ch32(x, 0xC8) || !ch32(x, 0xEC)) continue;
      const int before = rec32(x, 0x68);
      void* bonus = g_level_bonus;
      __asm {
        push esi
        mov esi, x
        call bonus
        pop esi
      }
      LOGI("four players: player %d's score for the next level %d (%d with the level's bonus)", x + 1, before,
           rec32(x, 0x68));
    }
  return g_results_takes_part(p);
}

void(__cdecl* oBothTakePart)() = nullptr;
void __cdecl hkBothTakePart() {
  oBothTakePart();
  if (g_on) ch32(2, 0xEC) = ch32(3, 0xEC) = 1;
}

// --- the camera --------------------------------------------------------------------
// It aims at the middle of players 1 and 2 (0x402f8f(out, mode, lift): mode 2 =
// their middle at the higher one's height) and pulls back to hold both
// (0x403735(mode)); mode is how many of them are in play. With players 3 or 4
// in play it gets, as "players 1 and 2", the corners of a box around all of
// them: it aims at its middle and pulls back to hold it.
bool make_box() {
  int n = 0;
  float lo[3] = {}, hi[3] = {};
  for (int p = 0; p < 4; ++p) {
    if ((p < 2 && p >= players()) || !data_of(p)) continue;
    const float* v = pos_of(p);
    for (int i = 0; i < 3; ++i) {
      lo[i] = n ? std::min(lo[i], v[i]) : v[i];
      hi[i] = n ? std::max(hi[i], v[i]) : v[i];
    }
    ++n;
  }
  // Only with players 3 or 4 in play (alone too: players 1 and 2 down).
  if (!n || !(data_of(2) || data_of(3))) return false;
  const float a[3] = {lo[0], hi[1], lo[2]}, b[3] = {hi[0], hi[1], hi[2]};
  std::memcpy(g_box[0], a, 12);
  std::memcpy(g_box[1], b, 12);
  return true;
}

void(__cdecl* oCamTarget)(float*, int, float) = nullptr;
// One player alone: both "players" are it.
bool solo_box() {
  if (g_solo < 0 || g_solo > 3 || (g_solo < 2 && g_solo >= players()) || !data_of(g_solo)) return false;
  std::memcpy(g_box[0], pos_of(g_solo), 12);
  std::memcpy(g_box[1], pos_of(g_solo), 12);
  return true;
}

void __cdecl hkCamTarget(float* out, int mode, float lift) {
  if (g_solo >= 0 ? solo_box() : (g_on && make_box())) {
    g_box_on = true;
    oCamTarget(out, 2, lift);
    g_box_on = false;
  } else {
    oCamTarget(out, mode, lift);
  }
  if (g_solo < 0) own_camera::on_aim(out, lift, _ReturnAddress());
}

float(__cdecl* oCamZoom)(int) = nullptr;
float __cdecl hkCamZoom(int mode) {
  // One player alone: the game's zoom for one (how far its turtle is).
  if (g_solo >= 0) return solo_box() ? own_camera::zoom_for(g_box[0]) : oCamZoom(mode);
  if (!g_on || !make_box()) return oCamZoom(mode);
  g_box_on = true;
  float r = oCamZoom(2);
  g_box_on = false;
  return r;
}

// --- pad slots 2 and 3 ---------------------------------------------------------
// The game's two pad slots (0x638a80, 0xAC bytes each): +4 the action table,
// +8 on, +0xC held, +0x10 pressed, +0x14 released, +0x18 repeated, +0x9C the
// sticks (0x80 = centre). A turtle reads the slot of its player number.
struct Slot {
  int32_t on = 0;
  uint32_t held = 0, pressed = 0, released = 0, repeated = 0, sticks = 0x80808080u;
  uint16_t raw = 0, raw_prev = 0;
};
Slot g_slot[2];
uint16_t g_buttons[2];
uint16_t g_key_bits[2];                            // from this keyboard (p3_keys / p4_keys)
uint32_t g_sticks[2] = {0x80808080u, 0x80808080u};  // from their gamepads (0x80 = centre)
uint8_t* g_slots = nullptr;  // the game's slots

bool fake(int s) { return s == 2 || s == 3; }

// A menu led by one player (read_slot0_as): slot 0 is read as that player's
// slot, slot 1 as an empty one.
int g_as = -1;
uint32_t g_as_without = 0, g_as_with = 0;
bool as_nobody(int s) { return g_as >= 0 && s == 1; }
int as_slot(int s) { return g_as >= 0 && s == 0 ? g_as : s; }

int(__cdecl* oSlotOn)(int) = nullptr;
int __cdecl hkSlotOn(int s) { return fake(s) ? g_slot[s - 2].on : oSlotOn(s); }
uint32_t(__cdecl* oSlotHeld)(int) = nullptr;
uint32_t __cdecl hkSlotHeld(int s) {
  if (as_nobody(s)) return 0;
  const int q = as_slot(s);
  const uint32_t v = fake(q) ? g_slot[q - 2].held : oSlotHeld(q);
  return g_as >= 0 && s == 0 ? v & ~g_as_without : v;
}
uint32_t(__cdecl* oSlotPressed)(int) = nullptr;
uint32_t __cdecl hkSlotPressed(int s) {
  if (as_nobody(s)) return 0;
  const int q = as_slot(s);
  const uint32_t v = fake(q) ? g_slot[q - 2].pressed : oSlotPressed(q);
  return g_as >= 0 && s == 0 ? (v & ~g_as_without) | g_as_with : v;
}
uint32_t(__cdecl* oSlotRepeated)(int) = nullptr;
uint32_t __cdecl hkSlotRepeated(int s) {
  if (as_nobody(s)) return 0;
  const int q = as_slot(s);
  const uint32_t v = fake(q) ? g_slot[q - 2].repeated : oSlotRepeated(q);
  return g_as >= 0 && s == 0 ? v & ~g_as_without : v;
}
uint32_t(__cdecl* oSlotSticks)(int) = nullptr;
uint32_t __cdecl hkSlotSticks(int s) {
  if (as_nobody(s)) return 0x80808080u;
  const int q = as_slot(s);
  return fake(q) ? g_slot[q - 2].sticks : oSlotSticks(q);
}
void(__cdecl* oSlotClear)(int) = nullptr;
void __cdecl hkSlotClear(int s) {
  if (!fake(s)) return oSlotClear(s);
  Slot& f = g_slot[s - 2];
  f.held = f.pressed = f.released = f.repeated = 0;
  f.sticks = 0x80808080u;
}
void(__cdecl* oSlotRumbleOff)(int) = nullptr;
void __cdecl hkSlotRumbleOff(int s) {
  if (!fake(s)) oSlotRumbleOff(s);
}
// A turtle hit (0x43385e) or grabbed (0x44f81d) shakes its player's pad:
// 0x4a9af0(slot, power, time) / 0x4a9ab6(slot, power) write slot +0xA0..+0xA8
// and read its device: slots 2 and 3 would be past the game's two.
void(__cdecl* oRumble)(int, int) = nullptr;
void __cdecl hkRumble(int s, int v) {
  if (!fake(s)) oRumble(s, v);
}
void(__cdecl* oRumbleFor)(int, int, float) = nullptr;
void __cdecl hkRumbleFor(int s, int v, float t) {
  if (!fake(s)) oRumbleFor(s, v, t);
}

// 0x4a9520: the game's key bits -> its logical bits (decide 0x10000, cancel
// 0x20000, ...; actions 0x100000 << i from the slot's table).
uint32_t remap(uint32_t b, const uint32_t* table) {
  uint32_t r = b;
  if (b & 0x840) r |= 0x10000;
  if (b & 0x180) r |= 0x20000;
  if (b & 4) r |= 0x40000;
  if (b & 8) r |= 0x80000;
  if (table)
    for (int i = 0; i < 8; ++i)
      if (table[1 + i] & b) r |= 1u << (20 + i);
  return r;
}

void update_slots() {
  const uint32_t* table = g_slots ? *reinterpret_cast<const uint32_t* const*>(g_slots + 4) : nullptr;  // player 1's
  for (int i = 0; i < 2; ++i) {
    Slot& f = g_slot[i];
    f.on = g_on;
    f.raw_prev = f.raw;
    f.raw = g_on ? g_buttons[i] : 0;
    uint16_t pressed = f.raw & ~f.raw_prev, released = f.raw_prev & ~f.raw;
    f.held = remap(f.raw, table);
    f.pressed = remap(pressed, table);
    f.released = remap(released, table);
    f.repeated = f.pressed;
    f.sticks = g_on ? g_sticks[i] : 0x80808080u;
  }
}

// --- gamepads of players 3 and 4 ---------------------------------------------------
// The game polls every gamepad but plays two (players 1 and 2: the game's key
// menu says which). Players 3 and 4 take theirs from the same list: [mod]
// p3_pad / p4_pad, or by themselves the last ones (with four and four pads: the
// third and the fourth; with fewer pads than players they go to players 3 and
// 4 first, who have no keys by default). Their buttons: player 1's gamepad
// buttons in the game's key menu. A gamepad of player 3 or 4 no longer moves
// player 1 or 2 (pad_raw_masks below). Not in a network game: there every PC
// plays one player with its player 1's controls.
int g_pad[2] = {-1, -1};  // this frame's gamepad of players 3 and 4, -1 = none
uint32_t* g_raw = nullptr;     // the game's raw masks {cur, prev}[set 2][player 2] (0x69f5a8)
uint32_t* g_axes = nullptr;    // the game's sticks bytes per player (0x69f588)
void(__cdecl* oRawMasks)() = nullptr;

bool pads_apply() { return g_on && !lockstep::net_active(); }

void choose_pads() {
  int n = binds::pad_count(), want = std::max(0, g_count - 2);  // players 3 (and 4)
  int pad[2] = {-1, -1};
  for (int x = 0; x < want; ++x)
    if (g_pad_cfg[x] > 0) pad[x] = g_pad_cfg[x] - 1 < n ? g_pad_cfg[x] - 1 : -1;
  // The rest by themselves: the last gamepads nobody was given (from the
  // first of the last ones on, then back towards the first).
  int autos = 0;
  for (int x = 0; x < want; ++x) autos += g_pad_cfg[x] < 0;
  int start = std::max(0, n - autos);
  for (int x = 0; x < want; ++x) {
    if (g_pad_cfg[x] >= 0) continue;
    for (int k = 0; k < n && pad[x] < 0; ++k) {
      int i = k < n - start ? start + k : start - 1 - (k - (n - start));
      if (i != pad[0] && i != pad[1]) pad[x] = i;
    }
  }
  for (int x = 0; x < 2; ++x) {
    if (pad[x] != g_pad[x])
      LOGI("four players: player %d %s", x + 3,
           pad[x] < 0 ? "has no gamepad"
                      : ("plays with gamepad " + std::to_string(pad[x] + 1) + " '" + binds::pad_name(pad[x]) + "'").c_str());
    g_pad[x] = pad[x];
  }
}

void read_pads() {
  if (!pads_apply()) {
    g_sticks[0] = g_sticks[1] = 0x80808080u;
    return;
  }
  choose_pads();
  int buttons[tmnp::kNumSlots];
  binds::pad_buttons(0, buttons);
  for (int x = 0; x < 2; ++x) {
    uint16_t bits = 0;
    uint32_t sticks = 0x80808080u;
    if (g_pad[x] >= 0) binds::pad_read(g_pad[x], buttons, bits, sticks);
    g_buttons[x] = g_key_bits[x] | bits;
    g_sticks[x] = sticks;
  }
}

// The game's raw masks of both players (0x4b2839, called once a frame by the
// masks 0x4b2a2f): then player 1 or 2's gamepad part is none while that
// gamepad is player 3 or 4's, and so are its sticks. (Not in the main menu:
// there players 3 and 4 have nothing to do, and their gamepads lead it again.)
void __cdecl hkRawMasks() {
  oRawMasks();
  if (!pads_apply() || main_menu::on_screen()) return;
  for (int p = 0; p < 2; ++p) {
    int mine = binds::pad_of(p);
    if (mine < 0 || (mine != g_pad[0] && mine != g_pad[1])) continue;
    g_raw[4 + p * 2] = 0;  // set 1 (gamepad), this frame
    if (g_axes) g_axes[p] = 0x80808080u;
  }
}

// --- characters and turtles ------------------------------------------------------
using Character_t = int(__cdecl*)(int character);
using Make_t = void*(__cdecl*)(void* scene, int player, int character, const float* pos, const float* dir);
Character_t g_load_character = nullptr, g_free_character = nullptr;
Make_t oMake = nullptr;
int g_loaded[2];  // characters loaded for players 3 and 4
// ... and how many times: the game counts a character's loads (0x427fef: inc
// [0x60eb88], freed at 0), and a scene inside the story loads the players again
// and frees them when it ends (the dojo: 0x491184 and 0x490ebd). Until 0.13.8
// one free here forgot the character: after the dojo "player 3's character is
// not loaded; not made" (a real game: player 3 was gone on the next map).
int g_load_count[2];

constexpr int kTurtles[] = {1, 2, 3, 4};  // the four turtles' character numbers

int g_picks[2];  // players 3 and 4's turtles from the character select (0 = none)
int g_lend = -1;  // player 2's pad slot carries this player's buttons (csel.cpp)
uint32_t g_pad_updates = 0, g_lend_at = 0;

// Players 3 and 4 get the turtles they picked (csel.cpp), else the ones
// nobody took (player 4 only with four).
void assign_characters() {
  int got[4] = {char_of(0), players() > 1 ? char_of(1) : 0, 0, 0};
  auto free_ = [&got](int t) { return t >= 1 && t <= 4 && std::find(got, got + 4, t) == got + 4; };
  for (int p = 2; p < 4; ++p)
    if (p < g_count && free_(g_picks[p - 2])) got[p] = g_picks[p - 2];
  for (int p = 2; p < 4; ++p)
    for (int t : kTurtles)
      if (p < g_count && !got[p] && free_(t)) got[p] = t;
  for (int p = 2; p < 4; ++p) {
    ch32(p, 0xC8) = got[p];
    if (got[p]) ch32(p, 0xEC) = 1;
  }
}

void(__cdecl* oLoadPlayers)() = nullptr;
void __cdecl hkLoadPlayers() {
  oLoadPlayers();
  if (!g_on) return;
  assign_characters();
  for (int p = 2; p < 4; ++p) {
    int& loaded = g_loaded[p - 2];
    int& count = g_load_count[p - 2];
    const int ch = char_of(p);
    if (loaded && loaded != ch) {  // (another turtle than the one loaded: that one goes)
      for (; count > 0; --count) g_free_character(loaded);
      loaded = 0;
    }
    if (!ch) continue;
    g_load_character(ch);
    loaded = ch;
    ++count;
  }
  LOGI("four players: players 3 and 4 are characters %d and %d, loaded %d and %d time(s) (called from %p)", char_of(2),
       char_of(3), g_load_count[0], g_load_count[1], _ReturnAddress());
}

bool g_in_level[2];  // players 3 and 4 were made in this level

void(__cdecl* oFreePlayers)() = nullptr;
void __cdecl hkFreePlayers() {
  oFreePlayers();
  g_in_level[0] = g_in_level[1] = false;
  if (g_loaded[0] || g_loaded[1])
    LOGI("four players: characters %d and %d freed, %d and %d load(s) left (called from %p)", g_loaded[0], g_loaded[1],
         std::max(0, g_load_count[0] - 1), std::max(0, g_load_count[1] - 1), _ReturnAddress());
  for (int i = 0; i < 2; ++i) {
    if (!g_loaded[i]) continue;
    g_free_character(g_loaded[i]);
    if (--g_load_count[i] <= 0) g_load_count[i] = 0, g_loaded[i] = 0;
  }
}

// A turtle lying stunned when its level ended (revive.cpp) has no life: the
// turtle's end keeps it for the next level (0x432159 -> 0x404bba, the record's
// +0x80) and the next turtle starts with it. It starts with a third instead.
// (make_player's own return value is not the turtle: it ends with set_pos's
// eax; the turtle is in the player's record.)
void fix_life(int p) {
  uint8_t* turtle = extra(p) ? reinterpret_cast<uint8_t*>((uintptr_t)rec32(p, 0x44))
                             : *reinterpret_cast<uint8_t**>(game_rec(p) + 4);
  uint8_t* body = turtle ? *reinterpret_cast<uint8_t**>(turtle + 0x60) : nullptr;
  uint8_t* data = body ? *reinterpret_cast<uint8_t**>(body + 0x154) : nullptr;
  if (!data) return;
  int32_t& life = *reinterpret_cast<int32_t*>(data + 0x30);
  const int32_t full = *reinterpret_cast<int32_t*>(data + 0x2C);
  if (life > 0 || full <= 0) return;
  life = std::max(1, (full + 2) / 3);
  LOGI("four players: player %d made with no life, given %d of %d", *reinterpret_cast<int32_t*>(data + 4) + 1, life,
       full);
}

// Players 1 and 2 are made by make_player (0x493d4e) wherever the game does
// it (the level's start, a player's own task, a continue). Players 3 and 4
// come right after the last of them, a step behind players 1 and 2.
struct Made {
  bool seen = false;
  float pos[3], dir[3];
};
Made g_made[2];
bool g_making = false;

void assign_characters();

void make_extras(void* scene) {
  if (!char_of(2) && !char_of(3)) assign_characters();
  const Made& a = g_made[0].seen ? g_made[0] : g_made[1];
  const Made& b = g_made[1].seen ? g_made[1] : a;
  float d[3] = {a.dir[0], 0, a.dir[2]};
  float len = std::sqrt(d[0] * d[0] + d[2] * d[2]);
  if (len > 0.001f) d[0] /= len, d[2] /= len;
  const float back = 1.5f;
  for (int p = 2; p < 4; ++p) {
    int ch = char_of(p);
    if (!ch || rec32(p, 0x44)) continue;  // no turtle for it, or it has one
    if (g_loaded[p - 2] != ch) {
      LOGW("four players: player %d's character %d is not loaded; not made", p + 1, ch);
      continue;
    }
    // Their shurikens (the per-player ints the game keeps between levels): as
    // player 1's when they have none yet.
    int32_t* kept = g_extra[p - 2].setting;
    if (!kept[0] && !kept[1] && !kept[2])
      for (int k = 0; k < 3; ++k) kept[k] = oSetting(0, k);
    const Made& m = p == 2 ? a : b;
    float pos[3] = {m.pos[0] - d[0] * back, m.pos[1], m.pos[2] - d[2] * back};
    g_making = true;
    oMake(scene, p, ch, pos, m.dir);
    g_making = false;
    fix_life(p);
    const bool t = rec32(p, 0x44) != 0;  // its turtle, in its record
    g_in_level[p - 2] = g_in_level[p - 2] || t;
    LOGI("four players: player %d (character %d) %s at (%.2f %.2f %.2f), its score from the level before %d", p + 1, ch,
         t ? "made" : "NOT made", pos[0], pos[1], pos[2], rec32(p, 0x68));
  }
}

void* __cdecl hkMake(void* scene, int player, int ch, const float* pos, const float* dir) {
  void* r = oMake(scene, player, ch, pos, dir);
  if (player >= 0 && player < 4 && (player < 2 || extra(player))) fix_life(player);
  if (g_making || (player != 0 && player != 1)) return r;
  LOGI("four players: the game made player %d (character %d) at (%.2f %.2f %.2f), called from %p", player + 1, ch,
       pos ? pos[0] : 0, pos ? pos[1] : 0, pos ? pos[2] : 0, _ReturnAddress());
  if (pos && dir) {
    g_made[player].seen = true;
    std::memcpy(g_made[player].pos, pos, 12);
    std::memcpy(g_made[player].dir, dir, 12);
  }
  // After the last of players 1 and 2 (player 2, or player 1 alone).
  if (g_on && (player == 1 || char_of(1) == 0)) {
    make_extras(scene);
    g_made[0].seen = g_made[1].seen = false;
  }
  return r;
}

// --- down and back ---------------------------------------------------------------
// The level's round, every frame of play (0x411c48(task), a state of the level
// task): when player 1 or 2 is in play it runs the continue check 0x411332 (ebx
// = task: a player who is down and presses decide comes back for a heart,
// 0x40426d, made at 0x4110ce's place facing 0x411095's way; 2 = player 2 joins
// 0x411bf2, 3 = pause 0x4042a3 + 0x4119db); when both are down it goes to
// "ПРОДОЛЖИТЬ?" (hearts left) or the game is over. With players 3 or 4 still
// in play the round goes on as when somebody is in it, and players 3 and 4
// come back like player 1.
using Int_t = int(__cdecl*)();
using Void_t = void(__cdecl*)();
using Task_t = void(__cdecl*)(void* task);
using Vec_t = void(__cdecl*)(float* out);
Int_t g_flags = nullptr, g_any_flag = nullptr, g_fading = nullptr, g_hearts = nullptr;
void(__cdecl* g_hearts_sub)(int) = nullptr;
void(__cdecl* g_tidy)(void* task, int) = nullptr;
Task_t g_tidy2 = nullptr, g_join = nullptr, g_pause2 = nullptr;
Void_t g_pause1 = nullptr;
Vec_t g_back_pos = nullptr, g_back_dir = nullptr;
uintptr_t g_continue = 0;  // 0x411332, the task in ebx

int continue_check(void* task) {
  uintptr_t fn = g_continue;
  int r;
  __asm {
    push ebx
    mov ebx, task
    call fn
    pop ebx
    mov r, eax
  }
  return r;
}

// Players 3 and 4 who are down come back like player 1 (0x411332's first part).
void continue_extras(void* task) {
  int flags = g_flags();
  if (!(flags & 8) || (flags & 0x200) || g_fading()) return;
  for (int p = 2; p < 4; ++p) {
    int ch = char_of(p);
    if (!g_in_level[p - 2] || data_of(p) || !ch || !(g_slot[p - 2].pressed & 0x10000)) continue;
    if (g_hearts() <= 0) continue;
    g_hearts_sub(1);
    float pos[3] = {}, dir[3] = {};
    g_back_pos(pos);
    g_back_dir(dir);
    g_making = true;
    oMake(task, p, ch, pos, dir);
    const bool t = rec32(p, 0x44) != 0;
    g_making = false;
    LOGI("four players: player %d comes back%s at (%.2f %.2f %.2f), %d hearts left", p + 1, t ? "" : " (NOT made)",
         pos[0], pos[1], pos[2], g_hearts());
  }
}

void(__cdecl* oRound)(void* task) = nullptr;
void __cdecl hkRound(void* task) {
  bool extras_up = g_on && (data_of(2) || data_of(3));
  bool game_up = oActive(0) || oActive(1);
  if (!extras_up || game_up) {
    oRound(task);
  } else {
    // Players 1 and 2 are down, 3 or 4 still play: as the game's round with
    // somebody in it (0x411c48 -> 0x411cca).
    if (!g_any_flag()) g_tidy(task, 0), g_tidy2(task);
    if (g_flags() & 0x40) return;  // over
    int r = continue_check(task);
    if (r == 2) {
      g_join(task);
    } else if (r == 3) {
      g_pause1();
      g_pause2(task);
    }
  }
  if (g_on) continue_extras(task);
}

// --- lab: what a turtle goes through ------------------------------------------------
// [debug] actions=1. Player data: +0x10 action, +0x14 its kind, +0x2C full
// life, +0x30 life. Every change of those; while the life is 0, every dword of
// the data that changes and the body's small numbers; who removes a turtle.
bool g_trace = false;
uint32_t g_down_at = 0;
struct Trace {
  uint8_t* data = nullptr;
  uint8_t* turtle = nullptr;
  uint8_t snap[0x240];
  uint8_t body[0x200];
  int lines = 0;
};
Trace g_trace_of[4];
uint32_t g_frame = 0;

uint8_t* data_ptr(int p) { return reinterpret_cast<uint8_t*>((uintptr_t)data_of(p)); }
uint8_t* turtle_ptr(int p) {
  return extra(p) ? reinterpret_cast<uint8_t*>((uintptr_t)rec32(p, 0x44)) : *reinterpret_cast<uint8_t**>(game_rec(p) + 4);
}

void trace_players() {
  ++g_frame;
  for (int p = 0; p < 4; ++p) {
    if (p < 2 && p >= players()) continue;
    Trace& t = g_trace_of[p];
    uint8_t* d = data_ptr(p);
    uint8_t* tu = turtle_ptr(p);
    if (d != t.data || tu != t.turtle) {
      LOGI("trace %u: player %d data %p turtle %p (was %p %p)", g_frame, p + 1, d, tu, t.data, t.turtle);
      t.data = d;
      t.turtle = tu;
      t.lines = 0;
      if (d) std::memcpy(t.snap, d, sizeof(t.snap));
      uint8_t* b = tu ? *reinterpret_cast<uint8_t**>(tu + 0x60) : nullptr;
      if (b) std::memcpy(t.body, b, sizeof(t.body));
      continue;
    }
    if (!d) continue;
    auto i32 = [&](const uint8_t* base, size_t off) { return *reinterpret_cast<const int32_t*>(base + off); };
    int life = i32(d, 0x30);
    bool key = i32(d, 0x10) != i32(t.snap, 0x10) || i32(d, 0x14) != i32(t.snap, 0x14) ||
               life != i32(t.snap, 0x30) || i32(d, 0x2C) != i32(t.snap, 0x2C);
    if (key)
      LOGI("trace %u: player %d action %d (%d) -> %d (%d), life %d -> %d of %d", g_frame, p + 1, i32(t.snap, 0x10),
           i32(t.snap, 0x14), i32(d, 0x10), i32(d, 0x14), i32(t.snap, 0x30), life, i32(d, 0x2C));
    if (life <= 0 && !g_down_at) g_down_at = net::now_ms() | 1;
    if (life <= 0 && t.lines < 600) {
      char buf[1024];
      int n = 0;
      for (size_t off = 0; off < sizeof(t.snap) && n < 900; off += 4) {
        int32_t a = i32(t.snap, off), b = i32(d, off);
        if (a == b || off == 0x30) continue;
        n += snprintf(buf + n, sizeof(buf) - n, " d%X:%d>%d", (unsigned)off, a, b);
      }
      uint8_t* body = t.turtle ? *reinterpret_cast<uint8_t**>(t.turtle + 0x60) : nullptr;
      if (body)
        for (size_t off = 0; off < sizeof(t.body) && n < 900; off += 4) {
          int32_t a = i32(t.body, off), b = i32(body, off);
          if (a == b || a < -1000 || a > 100000 || b < -1000 || b > 100000) continue;
          n += snprintf(buf + n, sizeof(buf) - n, " b%X:%d>%d", (unsigned)off, a, b);
        }
      if (n) {
        LOGI("trace %u: player %d down:%s", g_frame, p + 1, buf);
        ++t.lines;
      }
      if (body) std::memcpy(t.body, body, sizeof(t.body));
    }
    std::memcpy(t.snap, d, sizeof(t.snap));
  }
}

#pragma optimize("y", off)  // a frame pointer: the game's callers by its chain
void(__cdecl* oRemove)(void*) = nullptr;
void __cdecl hkRemove(void* obj) {
  if (g_trace && g_info && *g_info) {
    for (int p = 0; p < 4; ++p) {
      if ((p < 2 && p >= players()) || obj != turtle_ptr(p)) continue;
      uintptr_t* fp;
      __asm mov fp, ebp
      uintptr_t ret[5] = {};
      for (int i = 0; i < 5 && fp; ++i) {
        ret[i] = fp[1];
        uintptr_t* next = reinterpret_cast<uintptr_t*>(fp[0]);
        if (next <= fp || next > fp + 0x4000) break;
        fp = next;
      }
      uint8_t* d = data_ptr(p);
      LOGI("trace %u: player %d's turtle removed (action %d, life %d), callers %08X %08X %08X %08X %08X", g_frame,
           p + 1, d ? *reinterpret_cast<int32_t*>(d + 0x10) : -1, d ? *reinterpret_cast<int32_t*>(d + 0x30) : -1,
           ret[0], ret[1], ret[2], ret[3], ret[4]);
    }
  }
  oRemove(obj);
}
#pragma optimize("", on)

// --- finding it all --------------------------------------------------------------
struct Hook {
  const char* name;
  const char* pattern;
  void* detour;
  void** original;
};

template <typename T>
void** orig_of(T& fn) {
  return reinterpret_cast<void**>(&fn);
}

uintptr_t call_target(uintptr_t at) {
  if (patch::read<uint8_t>(at) != 0xE8) return 0;
  uintptr_t t = at + 5 + patch::read<int32_t>(at + 1);
  return patch::in_image(t, 1) ? t : 0;
}

}  // namespace

void install(const Config& cfg) {
  g_count = std::clamp(cfg.players, 1, 4);
  g_on = g_count > 2;
  g_trace = cfg.debug_actions;
  g_trace_nearest = cfg.debug_actions;
  g_trace_access = cfg.debug_actions;
  for (int x = 0; x < 2; ++x) {
    for (int i = 0; i < 16; ++i) g_keys[x][i] = i < tmnp::kNumSlots ? cfg.extra_keys[x][i] : -1;
    g_pad_cfg[x] = cfg.extra_pad[x];
  }
  const Hook hooks[] = {
      {"p_set_character", "8B 44 24 04 8B 4C 24 08 8B 15 ?? ?? ?? ?? 8D 44 80 19 89 0C C2 C3",
       (void*)&ChSet<0xC8>::hook, orig_of(ChSet<0xC8>::orig)},
      {"p_character", "8B 44 24 04 8B 0D ?? ?? ?? ?? 8D 44 80 19 8B 04 C1 C3", (void*)&ChGet<0xC8>::hook,
       orig_of(ChGet<0xC8>::orig)},
      {"p_set_68", "8B 44 24 04 8B 4C 24 08 6B C0 44 8B 15 ?? ?? ?? ?? 89 4C 10 68 C3", (void*)&RecSet<0x68>::hook,
       orig_of(RecSet<0x68>::orig)},
      {"p_get_68", "8B 44 24 04 8B 0D ?? ?? ?? ?? 6B C0 44 8B 44 08 68 C3", (void*)&RecGet<0x68>::hook,
       orig_of(RecGet<0x68>::orig)},
      {"p_set_6c", "8B 44 24 04 8B 4C 24 08 6B C0 44 8B 15 ?? ?? ?? ?? 89 4C 10 6C C3", (void*)&RecSet<0x6C>::hook,
       orig_of(RecSet<0x6C>::orig)},
      {"p_get_6c", "8B 44 24 04 8B 0D ?? ?? ?? ?? 6B C0 44 8B 44 08 6C C3", (void*)&RecGet<0x6C>::hook,
       orig_of(RecGet<0x6C>::orig)},
      {"p_set_70", "8B 44 24 04 8B 4C 24 08 6B C0 44 8B 15 ?? ?? ?? ?? 89 4C 10 70 C3", (void*)&RecSet<0x70>::hook,
       orig_of(RecSet<0x70>::orig)},
      {"p_get_70", "8B 44 24 04 8B 0D ?? ?? ?? ?? 6B C0 44 8B 44 08 70 C3", (void*)&RecGet<0x70>::hook,
       orig_of(RecGet<0x70>::orig)},
      {"p_get_74", "8B 44 24 04 8B 0D ?? ?? ?? ?? 6B C0 44 8B 44 08 74 C3", (void*)&RecGet<0x74>::hook,
       orig_of(RecGet<0x74>::orig)},
      {"p_set_74", "8B 44 24 04 8B 4C 24 08 6B C0 44 8B 15 ?? ?? ?? ?? 89 4C 10 74 C3", (void*)&RecSet<0x74>::hook,
       orig_of(RecSet<0x74>::orig)},  // (the best combo, 0x4199b1)
      {"p_add_78", "8B 44 24 04 8B 0D ?? ?? ?? ?? 6B C0 44 8D 44 08 78 8B 4C 24 08 01 08 C3",
       (void*)&RecAdd<0x78, false>::hook, orig_of(RecAdd<0x78, false>::orig)},
      {"p_set_54", "8B 44 24 04 8B 0D ?? ?? ?? ?? 6B C0 44 56 8B 74 24 0C 57 8D 7C 08 54 A5 A5 A5 5F 5E C3",
       (void*)&hkSet54, orig_of(oSet54)},
      {"p_get_e8", "8B 44 24 04 8B 0D ?? ?? ?? ?? 8D 04 80 8B 84 C1 E8 00 00 00 C3", (void*)&ChGet<0xE8>::hook,
       orig_of(ChGet<0xE8>::orig)},
      {"p_set_e8", "8B 44 24 04 8B 4C 24 08 8B 15 ?? ?? ?? ?? 8D 04 80 89 8C C2 E8 00 00 00 C3",
       (void*)&ChSet<0xE8>::hook, orig_of(ChSet<0xE8>::orig)},
      {"p_set_health", "8B 44 24 04 8B 4C 24 08 6B C0 44 8B 15 ?? ?? ?? ?? 89 4C 10 60 C3",
       (void*)&RecSet<0x60>::hook, orig_of(RecSet<0x60>::orig)},
      {"p_add_health", "8B 44 24 04 8B 0D ?? ?? ?? ?? 6B C0 44 8D 44 08 60 8B 4C 24 08 01 08 79 03 83 20 00 C3",
       (void*)&RecAdd<0x60, true>::hook, orig_of(RecAdd<0x60, true>::orig)},
      {"p_health", "8B 44 24 04 8B 0D ?? ?? ?? ?? 6B C0 44 8B 44 08 60 C3", (void*)&RecGet<0x60>::hook,
       orig_of(RecGet<0x60>::orig)},
      {"p_pos_ptr", "8B 44 24 04 8B 0D ?? ?? ?? ?? 6B C0 44 8D 44 08 48 C3", (void*)&RecPtr<0x48>::hook,
       orig_of(RecPtr<0x48>::orig)},
      {"p_p54_ptr", "8B 44 24 04 8B 0D ?? ?? ?? ?? 6B C0 44 8D 44 08 54 C3", (void*)&RecPtr<0x54>::hook,
       orig_of(RecPtr<0x54>::orig)},
      {"p_set_data", "8B 44 24 08 85 C0 75 13 8B 44 24 04 8B 0D ?? ?? ?? ?? 6B C0 44 83 ?? ?? ?? ?? C3",
       (void*)&RecSet<0x40>::hook, orig_of(RecSet<0x40>::orig)},
      {"p_set_actor", "8B 44 24 08 85 C0 75 13 8B 44 24 04 8B 0D ?? ?? ?? ?? 40 6B C0 44 83 24 08 00 C3",
       (void*)&RecSet<0x44>::hook, orig_of(RecSet<0x44>::orig)},
      {"p_active", "8B 44 24 04 8B 15 ?? ?? ?? ?? 6B C0 44 33 C9 39 4C 10 40 0F 95 C1 8B C1 C3", (void*)&hkActive,
       orig_of(oActive)},
      {"p_set_ec", "8B 44 24 04 8B 4C 24 08 8B 15 ?? ?? ?? ?? 8D 04 80 89 8C C2 EC 00 00 00 C3",
       (void*)&ChSet<0xEC>::hook, orig_of(ChSet<0xEC>::orig)},
      {"p_get_ec", "8B 44 24 04 8B 0D ?? ?? ?? ?? 8D 04 80 8B 84 C1 EC 00 00 00 C3", (void*)&ChGet<0xEC>::hook,
       orig_of(ChGet<0xEC>::orig)},
      {"p_flag154", "33 C0 39 44 24 04 75 0C A1 ?? ?? ?? ?? 8B 80 54 01 00 00 C3", (void*)&hkFlag, orig_of(oFlag)},
      {"p_data", "8B 44 24 04 8B 0D ?? ?? ?? ?? 6B C0 44 8B 44 08 40 C3", (void*)&RecGet<0x40>::hook,
       orig_of(RecGet<0x40>::orig)},
      {"p_actor", "8B 44 24 04 8B 0D ?? ?? ?? ?? 40 6B C0 44 8B 04 08 C3", (void*)&RecGet<0x44>::hook,
       orig_of(RecGet<0x44>::orig)},
      {"p_get_64", "8B 44 24 04 8B 0D ?? ?? ?? ?? 6B C0 44 8B 44 08 64 C3", (void*)&RecGet<0x64>::hook,
       orig_of(RecGet<0x64>::orig)},
      {"p_inc_64", "8B 44 24 04 8B 0D ?? ?? ?? ?? 6B C0 44 8D 44 08 64 FF 00 C3", (void*)&hkInc64, orig_of(oInc64)},
      {"p_set_e0e4",
       "8B 44 24 04 8B 0D ?? ?? ?? ?? 8D 04 80 8D 04 C1 8B 4C 24 0C 89 88 E0 00 00 00 8B 4C 24 08 89 88 E4 00 00 00 C3",
       (void*)&hkSetE0E4, orig_of(oSetE0E4)},
      {"p_get_e0e4",
       "8B 44 24 04 8B 0D ?? ?? ?? ?? 8D 04 80 8D 04 C1 8B 88 E4 00 00 00 3B 4C 24 08 75 07 8B 80 E0 00 00 00 C3",
       (void*)&hkGetE0E4, orig_of(oGetE0E4)},
      {"p_add_7c", "8B 44 24 04 8B 0D ?? ?? ?? ?? 6B C0 44 8D 44 08 7C 8B 4C 24 08 01 08 C3",
       (void*)&RecAdd<0x7C, false>::hook, orig_of(RecAdd<0x7C, false>::orig)},
      {"p_get_7c", "8B 44 24 04 8B 0D ?? ?? ?? ?? 6B C0 44 8B 44 08 7C C3", (void*)&RecGet<0x7C>::hook,
       orig_of(RecGet<0x7C>::orig)},
      {"p_set_80", "8B 44 24 04 8B 4C 24 08 6B C0 44 8B 15 ?? ?? ?? ?? 89 8C 10 80 00 00 00 C3",
       (void*)&RecSet<0x80>::hook, orig_of(RecSet<0x80>::orig)},
      {"p_get_80", "8B 44 24 04 8B 0D ?? ?? ?? ?? 6B C0 44 8B 84 08 80 00 00 00 C3", (void*)&RecGet<0x80>::hook,
       orig_of(RecGet<0x80>::orig)},
      {"p_setting", "8B 44 24 04 8B 4C 24 08 8D 0C 41 03 C8 A1 ?? ?? ?? ?? 8B 44 88 28 C3", (void*)&hkSetting,
       orig_of(oSetting)},
      {"p_set_setting", "8B 44 24 04 8B 4C 24 08 8B 15 ?? ?? ?? ?? 8D 0C 41 03 C8 8B 44 24 0C 89 44 8A 28 C3",
       (void*)&hkSetSetting, orig_of(oSetSetting)},
      {"p_set_pos",
       "55 8B EC 83 EC 0C 8B 15 ?? ?? ?? ?? 53 8B 5D 08 8B C3 6B C0 44 56 57 8D 74 10 48 89 75 08 8D 7D F4 A5 A5",
       (void*)&hkSetPos, orig_of(oSetPos)},
      {"p_nearest",
       "55 8B EC 83 EC 28 A1 ?? ?? ?? ?? 53 33 D2 56 33 DB 39 10 57 89 55 FC 7E 4D 8B 7D 10 89 55 F8 8D 75 D8",
       (void*)&hkNearest, orig_of(oNearest)},
      {"p_first_pos",
       "A1 ?? ?? ?? ?? 56 57 8B 38 33 C9 85 FF 7E 1E 8D 90 C8 00 00 00 8D 70 40 83 3E 00 74 05 83 3A 00 75 20",
       (void*)&hkFirstPos, orig_of(oFirstPos)},
      {"p_reset_game",
       "55 8B EC 83 EC 0C A1 ?? ?? ?? ?? D9 EE D9 5D F4 53 56 D9 EE D9 5D F8 57 D9 EE 33 DB 8D B8 44 01 00 00",
       (void*)&hkResetGame, orig_of(oResetGame)},
      {"p_new_run", "A1 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 89 48 24 83 60 68 00 83 A0 AC 00 00 00 00 E9", (void*)&hkNewRun,
       orig_of(oNewRun)},
      {"p_both_take_part", "A1 ?? ?? ?? ?? 33 C9 41 89 88 EC 00 00 00 89 88 14 01 00 00 C3", (void*)&hkBothTakePart,
       orig_of(oBothTakePart)},
      {"slot_rumble", "8B 44 24 08 56 8B 74 24 08 69 F6 AC 00 00 00 81 C6 ?? ?? ?? ?? 89 86 A4 00 00 00 50",
       (void*)&hkRumble, orig_of(oRumble)},
      {"slot_rumble_for", "D9 44 24 0C 56 8B 74 24 08 D8 15 ?? ?? ?? ?? 69 F6 AC 00 00 00 DF E0 81 C6",
       (void*)&hkRumbleFor, orig_of(oRumbleFor)},
      {"p_is_player",
       "56 33 F6 E8 ?? ?? ?? ?? 85 C0 7E 19 56 E8 ?? ?? ?? ?? 59 8B 4C 24 08 39 01 74 0E 46 E8 ?? ?? ?? ?? 3B F0 7C E7 33 C0 5E",
       (void*)&hkIsPlayer, orig_of(oIsPlayer)},
  };
  // Pad slots: the stick accessor is unique, its neighbours are 0x11 bytes
  // apart before it (+8 on, +0xC held, +0x10 pressed, +0x18 repeated).
  constexpr const char* kStickSig = "8B 44 24 04 69 C0 AC 00 00 00 05 ?? ?? ?? ?? 8B 80 9C 00 00 00 C3";
  constexpr const char* kClearSig = "8B 44 24 04 69 C0 AC 00 00 00 05 ?? ?? ?? ?? 33 C9 89 48 0C 89 48 10 89 48 14 89 48 18 E9";
  constexpr const char* kRumbleSig =
      "56 8B 74 24 08 69 F6 AC 00 00 00 81 C6 ?? ?? ?? ?? 8B 06 85 C0 7C 07 50 E8 ?? ?? ?? ?? 59 83 A6 A0 00 00 00 00";
  // Characters and the level's start.
  constexpr const char* kLoadPlayersSig =
      "56 57 33 FF 33 F6 56 E8 ?? ?? ?? ?? 85 C0 59 74 0F 56 E8 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 59 59 47 46 83 FE 02 7C E0 57 E8";
  constexpr const char* kMakeSig =
      "55 8B EC 53 56 57 33 F6 E8 ?? ?? ?? ?? A8 08 74 01 46 8B 45 10 48 8B 7D 14 8B 5D 0C 0F 84 E7 00 00 00";
  constexpr const char* kCamTargetSig =
      "55 8B EC 83 EC 28 56 57 33 FF 33 F6 56 E8 ?? ?? ?? ?? 85 C0 59 74 01 47 46 83 FE 02 7C EE 85 FF 74 09 8B 45 0C 48";
  constexpr const char* kRoundSig =
      "53 E8 ?? ?? ?? ?? 85 C0 8B 5C 24 08 75 11 6A 00 53 E8 ?? ?? ?? ?? 53 E8 ?? ?? ?? ?? 83 C4 0C E8 ?? ?? ?? ?? "
      "A8 40 75 7A 6A 00 E8";
  constexpr const char* kCamZoomSig = "55 8B EC 83 EC 1C 53 56 57 E8 ?? ?? ?? ?? 8B D8 8B 45 08 48 74 78 48 74 0A E8";
  constexpr const char* kLengthSig = "8B 44 24 04 D9 40 04 D9 00 D9 40 08 D9 C1 D8 CA D9 C3 D8 CC DE C1 D9 C1 D8 CA DE C1";

  uintptr_t info_at = patch::find("p_character", hooks[1].pattern);
  uintptr_t stick = patch::find("slot_sticks", kStickSig);
  uintptr_t clear = patch::find("slot_clear", kClearSig);
  uintptr_t rumble = patch::find("slot_rumble_off", kRumbleSig);
  uintptr_t load = patch::find("load_players", kLoadPlayersSig);
  uintptr_t make = patch::find("make_player", kMakeSig);
  uintptr_t length = patch::find("vector_length", kLengthSig);
  uintptr_t cam_target = patch::find("camera_target", kCamTargetSig);
  uintptr_t cam_zoom = patch::find("camera_zoom", kCamZoomSig);
  uintptr_t round = patch::find("level_round", kRoundSig);
  if (!info_at || !stick || !clear || !rumble || !load || !make || !length || !cam_target || !cam_zoom || !round) {
    LOGW("four players: the game is not laid out as expected; two players only");
    return;
  }
  g_info = reinterpret_cast<uint8_t**>(patch::read<uint32_t>(info_at + 6));
  g_slots = reinterpret_cast<uint8_t*>(patch::read<uint32_t>(stick + 11));
  g_length = reinterpret_cast<Length_t>(length);
  // The neighbours of the stick accessor: mov eax, [eax + slots + off].
  struct Near {
    const char* name;
    size_t back, off;
    void* detour;
    void** original;
  };
  const Near near_[] = {{"slot_on", 0x44, 0x08, (void*)&hkSlotOn, orig_of(oSlotOn)},
                        {"slot_held", 0x33, 0x0C, (void*)&hkSlotHeld, orig_of(oSlotHeld)},
                        {"slot_pressed", 0x22, 0x10, (void*)&hkSlotPressed, orig_of(oSlotPressed)},
                        {"slot_repeated", 0x11, 0x18, (void*)&hkSlotRepeated, orig_of(oSlotRepeated)}};
  for (const Near& n : near_) {
    uintptr_t a = stick - n.back;
    if (patch::read<uint32_t>(a) != 0x0424448B || patch::read<uint32_t>(a + 12) != (uint32_t)(uintptr_t)g_slots + n.off) {
      LOGW("four players: %s is not where expected; two players only", n.name);
      return;
    }
  }
  // load_players: its +0x18 call is 0x493c92 (load a character); free_players
  // is its neighbour 0x30 bytes on, calling 0x493cda.
  g_load_character = reinterpret_cast<Character_t>(call_target(load + 0x18));
  uintptr_t free_ = load + 0x30;
  g_free_character = reinterpret_cast<Character_t>(call_target(free_ + 0x15));
  if (!g_load_character || !g_free_character || patch::read<uint32_t>(free_) != 0x56F63356) {
    LOGW("four players: the character loading is not where expected; two players only");
    return;
  }

  // The round (0x411c48) and what it calls: its continue check 0x411332 and
  // the functions that one calls for player 1.
  g_continue = call_target(round + 0x82);
  const uintptr_t cc = g_continue;
  g_any_flag = reinterpret_cast<Int_t>(call_target(round + 0x01));
  g_tidy = reinterpret_cast<void(__cdecl*)(void*, int)>(call_target(round + 0x11));
  g_tidy2 = reinterpret_cast<Task_t>(call_target(round + 0x17));
  g_pause1 = reinterpret_cast<Void_t>(call_target(round + 0x8E));
  g_pause2 = reinterpret_cast<Task_t>(call_target(round + 0x94));
  g_join = reinterpret_cast<Task_t>(call_target(round + 0x9C));
  if (cc) {
    g_flags = reinterpret_cast<Int_t>(call_target(cc + 0x08));
    g_fading = reinterpret_cast<Int_t>(call_target(cc + 0x1E));
    g_hearts = reinterpret_cast<Int_t>(call_target(cc + 0x79));
    g_hearts_sub = reinterpret_cast<void(__cdecl*)(int)>(call_target(cc + 0x8F));
    g_back_dir = reinterpret_cast<Vec_t>(call_target(cc + 0x98));
    g_back_pos = reinterpret_cast<Vec_t>(call_target(cc + 0xA1));
  }
  if (!cc || !g_any_flag || !g_tidy || !g_tidy2 || !g_pause1 || !g_pause2 || !g_join || !g_flags || !g_fading ||
      !g_hearts || !g_hearts_sub || !g_back_dir || !g_back_pos || call_target(cc + 0xBB) != make) {
    LOGW("four players: the level's round is not laid out as expected; two players only");
    return;
  }

  int ok = 0, total = 0;
  for (const Hook& h : hooks) {
    ++total;
    uintptr_t a = patch::find(h.name, h.pattern);
    ok += a && patch::hook(h.name, a, h.detour, h.original);
  }
  for (const Near& n : near_) ++total, ok += patch::hook(n.name, stick - n.back, n.detour, n.original);
  struct Plain {
    const char* name;
    uintptr_t at;
    void* detour;
    void** original;
  };
  const Plain plain[] = {{"slot_sticks", stick, (void*)&hkSlotSticks, orig_of(oSlotSticks)},
                         {"slot_clear", clear, (void*)&hkSlotClear, orig_of(oSlotClear)},
                         {"slot_rumble_off", rumble, (void*)&hkSlotRumbleOff, orig_of(oSlotRumbleOff)},
                         {"load_players", load, (void*)&hkLoadPlayers, orig_of(oLoadPlayers)},
                         {"make_player", make, (void*)&hkMake, orig_of(oMake)},
                         {"camera_target", cam_target, (void*)&hkCamTarget, orig_of(oCamTarget)},
                         {"camera_zoom", cam_zoom, (void*)&hkCamZoom, orig_of(oCamZoom)},
                         {"level_round", round, (void*)&hkRound, orig_of(oRound)}};
  for (const Plain& p : plain) ++total, ok += patch::hook(p.name, p.at, p.detour, p.original);
  // free_players (0x493ec0) loops back to its +3 (jl 0x493ec3): a hook at
  // its start would put its loop into the middle of the jump (the game fell
  // over there whenever the players were freed). Its callers call ours.
  oFreePlayers = reinterpret_cast<void(__cdecl*)()>(free_);
  ++total;
  ok += patch::redirect_calls("free_players", free_, (void*)&hkFreePlayers) > 0;
  // The level's results (0x494cce): players 3 and 4's bonus with players 1 and 2's.
  if (const uintptr_t results = patch::find(
          "level_results", "33 DB 53 E8 ?? ?? ?? ?? 85 C0 59 74 07 33 F6 E8 ?? ?? ?? ?? 33 F6 46 56 E8")) {
    const uintptr_t asked = results + 3 + 5 + patch::read<int32_t>(results + 3 + 1);
    const uintptr_t bonus = results + 15 + 5 + patch::read<int32_t>(results + 15 + 1);
    const int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(&results_takes_part) - (results + 3 + 5));
    if (patch::in_image(asked, 1) && patch::in_image(bonus, 1)) {
      g_results_takes_part = reinterpret_cast<TakesPart_t>(asked);
      g_level_bonus = reinterpret_cast<void*>(bonus);
      const bool in_place = patch::write(results + 3 + 1, &rel, 4);
      LOGI("four players: players 3 and 4 get the level's bonus like players 1 and 2%s", in_place ? "" : ": not in place");
    }
  }
  // The level's choice of whom it makes (0x411b7b): its question "a stage for
  // one turtle?" (0x40498a) is asked through here.
  if (const uintptr_t solo = patch::find("solo_stage_makers",
                                         "E8 ?? ?? ?? ?? 85 C0 74 09 8B C6 E8 ?? ?? ?? ?? EB 07 56 E8 ?? ?? ?? ?? 59 E8")) {
    const uintptr_t asked = solo + 5 + patch::read<int32_t>(solo + 1);
    const int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(&solo_stage_for_makers) - (solo + 5));
    if (patch::in_image(asked, 1)) {
      g_solo_stage = reinterpret_cast<Solo_t>(asked);
      const bool in_place = patch::write(solo + 1, &rel, 4);
      LOGI("four players: in a game together everybody takes part in the stages for one turtle%s",
           in_place ? "" : ": not in place");
    }
  }
  // The raw masks (0x4b2839) and the sticks' bytes (their getter 0x4b271f, next
  // to the hat's 0x4b272b): players 3 and 4's gamepads taken from players 1 and 2.
  // (Its one caller calls ours: its loops stay off its first bytes either way.)
  uintptr_t raw = patch::find("pad_raw_masks",
                              "55 8B EC 83 EC 10 53 56 57 33 DB C7 45 F8 ?? ?? ?? ?? 8B 45 F8 8B D3 C1 E2 03 "
                              "89 45 F4 8D 82 ?? ?? ?? ??");
  uintptr_t axes = patch::find("pad_sticks_bytes", "8B 44 24 04 8B 04 85 ?? ?? ?? ?? C3 8B 04 85 ?? ?? ?? ?? 85 C0 74");
  ++total;
  if (raw && axes && patch::in_image(patch::read<uint32_t>(raw + 0x1F), 0x20) &&
      patch::in_image(patch::read<uint32_t>(axes + 7), 8)) {
    g_raw = reinterpret_cast<uint32_t*>((uintptr_t)patch::read<uint32_t>(raw + 0x1F));
    g_axes = reinterpret_cast<uint32_t*>((uintptr_t)patch::read<uint32_t>(axes + 7));
    oRawMasks = reinterpret_cast<void(__cdecl*)()>(raw);
    ok += patch::redirect_calls("pad_raw_masks", raw, (void*)&hkRawMasks) > 0;
  }
  if (g_trace) {
    uintptr_t remove = patch::find("object_remove", "8B 44 24 04 C7 40 04 02 00 00 00 C3");
    if (remove && patch::hook("object_remove", remove, (void*)&hkRemove, orig_of(oRemove)))
      LOGI("four players (lab): tracing the players' actions");
  }
  if (ok != total) LOGW("four players: %d of %d hooks in place; players 3 and 4 may misbehave", ok, total);
  g_installed = ok == total;
  LOGI("four players: %d play the story (%d hooks)", g_count, ok);
}

bool enabled() { return g_on; }
bool installed() { return g_installed; }
int count() { return g_count; }
void set_count(int n) {
  n = std::clamp(n, 1, 4);
  if (n != g_count) LOGI("four players: %d play the story now (was %d)", n, g_count);
  g_count = n;
  g_on = n > 2;
}

int team_size() {
  if (!g_info || !*g_info) return 0;
  int n = std::min(players(), 2);
  if (g_on)
    for (int p = 2; p < 4; ++p) n += char_of(p) != 0;
  return n;
}

void set_picks(int p3, int p4) {
  if (p3 != g_picks[0] || p4 != g_picks[1])
    LOGI("four players: players 3 and 4 picked characters %d and %d", p3, p4);
  g_picks[0] = p3, g_picks[1] = p4;
}

void lend_slot(int player) {
  if (player != g_lend) LOGI("four players: player 2's pad slot %s", player >= 2 ? ("carries player " + std::to_string(player + 1) + "'s buttons").c_str() : "is player 2's again");
  g_lend = player;
  g_lend_at = g_pad_updates;
}

void set_buttons(int player, uint16_t held) {
  if (player == 2 || player == 3) g_buttons[player - 2] = held;
}

// The character select's turn of player 3 or 4: player 2's slot (the game's
// pick and its "are you sure?" read it) gets that player's buttons instead.
void lend() {
  ++g_pad_updates;
  if (g_lend < 2 || g_lend > 3 || g_pad_updates - g_lend_at > 2 || !g_slots) return;
  const Slot& f = g_slot[g_lend - 2];
  uint8_t* s = g_slots + 0xAC;
  *reinterpret_cast<uint32_t*>(s + 0x0C) = f.held;
  *reinterpret_cast<uint32_t*>(s + 0x10) = f.pressed;
  *reinterpret_cast<uint32_t*>(s + 0x14) = f.released;
  *reinterpret_cast<uint32_t*>(s + 0x18) = f.repeated;
}

void after_pad_update() {
  read_pads();
  update_slots();
  lend();
  if (!g_info || !*g_info) return;
  if (g_trace) trace_players();
  revive::on_frame();
}

uint32_t pressed(int p) {
  if (p == 2 || p == 3) return g_slot[p - 2].pressed;
  if ((p == 0 || p == 1) && g_slots) return *reinterpret_cast<const uint32_t*>(g_slots + p * 0xAC + 0x10);
  return 0;
}

void read_slot0_as(int player, uint32_t without, uint32_t with) {
  g_as = player >= 0 && player <= 3 ? player : -1;
  g_as_without = g_as >= 0 ? without : 0;
  g_as_with = g_as >= 0 ? with : 0;
}

uint32_t held(int p) {
  if (extra(p)) return g_slot[p - 2].held;
  if ((p == 0 || p == 1) && g_slots) return *reinterpret_cast<const uint32_t*>(g_slots + p * 0xAC + 0x0C);
  return 0;
}

void mask(int p, uint16_t raw) {
  if (!raw || p < 0 || p > 3) return;
  // The bits the remap makes of them too (the slot's table; players 3 and 4 use player 1's).
  const uint8_t* slot = g_slots ? g_slots + (extra(p) ? 0 : p) * 0xAC : nullptr;
  const uint32_t* table = slot ? *reinterpret_cast<const uint32_t* const*>(slot + 4) : nullptr;
  uint32_t bits = remap(raw, table);
  if (extra(p)) {
    Slot& f = g_slot[p - 2];
    f.held &= ~bits, f.pressed &= ~bits, f.released &= ~bits, f.repeated &= ~bits;
    return;
  }
  if (!g_slots) return;
  for (size_t off : {0x0C, 0x10, 0x14, 0x18}) *reinterpret_cast<uint32_t*>(g_slots + p * 0xAC + off) &= ~bits;
}

uint32_t debug_down_at() { return g_down_at; }

void on_keyboard(const uint8_t* keys) {
  if (!g_on || lockstep::net_active()) return;
  for (int x = 0; x < 2; ++x) {
    uint16_t held = 0;
    for (int i = 0; i < 16; ++i)
      if (g_keys[x][i] > 0 && (keys[g_keys[x][i]] & 0x80)) held |= uint16_t(1u << i);
    g_key_bits[x] = held;
  }
}

void* turtle(int player) {
  if (!g_info || !*g_info || player < 0 || player > 3) return nullptr;
  if (extra(player)) return reinterpret_cast<void*>((uintptr_t)rec32(player, 0x44));
  return *reinterpret_cast<void**>(game_rec(player) + 4);
}

bool down(int player) {
  return g_on && extra(player) && g_info && *g_info && g_in_level[player - 2] && !data_of(player) && char_of(player);
}

int credits() { return g_hearts ? g_hearts() : 0; }

void take_credits() {
  int n = credits();
  if (n > 0 && g_hearts_sub) g_hearts_sub(n);
}

void debug_kill(int player) {
  // 0x4a91a1(object): "remove it" ([+4] = 2); its end (0x432159 for a turtle in
  // play) keeps the score and clears the player's record, as after a death.
  // (hooked with [debug] actions=1: then its trampoline, the pattern is gone)
  static uintptr_t remove =
      oRemove ? reinterpret_cast<uintptr_t>(oRemove) : patch::find("object_remove", "8B 44 24 04 C7 40 04 02 00 00 00 C3");
  void* t = turtle(player);
  if (!t || !remove) return;
  reinterpret_cast<void(__cdecl*)(void*)>(remove)(t);
  LOGI("four players (lab): player %d's turtle taken away", player + 1);
}

void debug_stage_over() {
  if (!g_info || !*g_info) return;
  *reinterpret_cast<uint32_t*>(*g_info + 0x14) |= 0x800;
  LOGI("four players (lab): the stage is over (flags %08X)", *reinterpret_cast<uint32_t*>(*g_info + 0x14));
}

void debug_set_life(int player, int life) {
  uint8_t* data = static_cast<uint8_t*>(player_data(player));
  if (!data) return;
  *reinterpret_cast<int32_t*>(data + 0x30) = life;
  LOGI("four players (lab): player %d's life set to %d", player + 1, life);
}

void* player_data(int player) {
  if (!g_info || !*g_info || player < 0 || player > 3) return nullptr;
  if (extra(player)) return reinterpret_cast<void*>((uintptr_t)rec32(player, 0x40));
  return *reinterpret_cast<void**>(game_rec(player));
}

int character(int player) {
  if (!g_info || !*g_info || player < 0 || player > 3) return 0;
  return char_of(player);
}

bool plays(int player) {
  if (!g_info || !*g_info || player < 0 || player > 3) return false;
  if (player == 1 ? players() < 2 : player > 1 && !(g_on && player < g_count)) return false;
  return char_of(player) != 0;
}

void answer_first_as(int character) { g_first_as = character; }

void camera_solo(int player) { g_solo = player; }

const float* position(int player) {
  if (!g_info || !*g_info || player < 0 || player > 3) return nullptr;
  if ((player < 2 && player >= players()) || !data_of(player)) return nullptr;
  return pos_of(player);
}

void choose_characters() {
  if (g_on && g_info && *g_info) assign_characters();
}

}  // namespace four
