#include "mod/csel.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "common/log.h"
#include "common/net.h"
#include "mod/costume.h"
#include "mod/main_menu.h"
#include "mod/native_menu.h"
#include "mod/csel_art.h"
#include "mod/four_players.h"
#include "mod/lockstep.h"
#include "mod/nametags.h"
#include "mod/patch.h"
#include "mod/shared_state.h"

namespace csel {
namespace {

// Character select update (0x495881), a task callback: void(CharSelect*).
constexpr const char* kUpdateSig =
    "55 8B EC 51 53 56 8B 75 08 8B 46 04 57 6A 02 5B 33 FF 3B C3 74 ?? 83 F8 03 74 ?? 83 F8 05 "
    "74 ?? 83 F8 06";
// assign_pad_slot(slot, player) (0x4a971e); imm32 at +13 = pad slot array.
constexpr const char* kAssignSig =
    "56 8B 74 24 08 69 F6 AC 00 00 00 81 C6 ?? ?? ?? ?? 83 3E 00 7C 05 E8";

// CharSelect: +0 two_players, +4 state, +8 step within the state, +0x10
// menu[2], +0x18 choice[2], +0x34/+0x38 the "are you sure?" dialogs of P1/P2.
// States: 2 P1 picks, 3 P1 confirm, 4 prepare P2 (1P: -> 7), 5 P2 picks,
// 6 P2 confirm, 7.. on to the level (0x4957ed: step 0 opens the dialog, step 1
// reads its answer every frame: 1 yes, 2 no -> back to 5).
constexpr int kStateP2Picks = 5, kStateP2Confirm = 6;
constexpr size_t kStep = 0x08, kP2Dialog = 0x38;
// The yes/no dialog is a task of its own (0x41273d) that reads pad slot 1 by
// itself, outside this update: its data (task+0x34) has +4 state (1 showing
// up, 2 waiting for an answer, 4 closing) and +8 answer (0 none yet). The mod
// answers "yes" there, a moment after the question shows.
constexpr size_t kTaskData = 0x34, kDialogState = 0x04, kDialogAnswer = 0x08;
constexpr int kAnswerYes = 1;
constexpr uint32_t kConfirmDelayMs = 600;
// Menu: +0 event (1 invalid, 2 moved, 3 decided, 4 cancel; reset every frame),
// +4 flags (bit0 reads pad slot 0, bit1 slot 1), +8 count, +0xC cursor,
// +0x10 items[count] of 0x18 bytes: +0 state (2 selectable, 1 taken), +4 value.
struct Menu {
  int32_t event, flags, count, cursor;
  uint8_t* items;
};
// Pad slot (0xAC bytes): +0 physical player (-1 none), +8 enabled,
// +0xC held, +0x10 pressed (logical bits), +0x14 released.
constexpr size_t kSlotSize = 0xAC, kSlotPressed = 0x10;
constexpr uint32_t kPadDecide = 0x10000;

using Update_t = void(__cdecl*)(uint8_t* cs);
using Assign_t = void(__cdecl*)(int slot, int player);
Update_t oUpdate = nullptr;
Assign_t g_assign = nullptr;
uint8_t* g_slots = nullptr;
Config g_cfg;

struct Watch {
  uint8_t* cs = nullptr;
  int state = -1;
  uint32_t since_ms = 0;
  bool p2_moved = false;   // somebody moved P2's cursor: a local P2 is playing
  bool picked = false;     // the mod picked for P2 in this round
  bool confirmed = false;  // and answered P2's "are you sure?"
} g_w;

int32_t& i32(uint8_t* base, size_t off) { return *reinterpret_cast<int32_t*>(base + off); }
Menu* menu_of(uint8_t* cs, int p) { return *reinterpret_cast<Menu**>(cs + 0x10 + 4 * p); }
int slot_player(int slot) { return i32(g_slots, slot * kSlotSize); }

void log_state(uint8_t* cs) {
  char menus[160] = "";
  for (int p = 0; p < 2; ++p) {
    Menu* m = menu_of(cs, p);
    char part[80];
    if (m) {
      char items[40] = "";
      for (int i = 0; i < m->count && i < 8; ++i) {
        char it[16];
        snprintf(it, sizeof(it), "%s%d:%d", i ? "," : "", i32(m->items, i * 0x18 + 4),
                 i32(m->items, i * 0x18));
        strncat(items, it, sizeof(items) - strlen(items) - 1);
      }
      snprintf(part, sizeof(part), " menu%d{flags=0x%X cur=%d items(value:state)=%s}", p + 1,
               m->flags, m->cursor, items);
    } else {
      snprintf(part, sizeof(part), " menu%d{none}", p + 1);
    }
    strncat(menus, part, sizeof(menus) - strlen(menus) - 1);
  }
  LOGI("char select: state=%d players=%d choice=%d/%d slots=%d/%d p2net=%d%s", i32(cs, 4),
       i32(cs, 0) ? 2 : 1, i32(cs, 0x18), i32(cs, 0x1C), slot_player(0), slot_player(1),
       g_p2.connected.load() ? 1 : 0, menus);
}

// Chooses P2's item: the configured value if free, else a random free one
// ([mod] p2_character=first: the next free one).
int choose_item(const Menu* m) {
  auto selectable = [m](int i) { return i32(m->items, i * 0x18) == 2; };
  if (g_cfg.p2_character >= 0)
    for (int i = 0; i < m->count; ++i)
      if (selectable(i) && i32(m->items, i * 0x18 + 4) == g_cfg.p2_character) return i;
  std::vector<int> free;
  for (int k = 0; k < m->count; ++k) {
    int i = (m->cursor + k) % m->count;
    if (selectable(i)) free.push_back(i);
  }
  if (free.empty()) return -1;
  if (g_cfg.p2_character == -2) return free.front();
  static std::mt19937 rng{std::random_device{}()};
  return free[std::uniform_int_distribution<size_t>(0, free.size() - 1)(rng)];
}

// A one-frame "decide" on pad slot 1 for this update (an unassigned slot is
// never refreshed by the game, so the old value is put back right after).
struct Decide {
  bool on = false;
  int32_t saved = 0;
  void press() {
    saved = i32(g_slots, kSlotSize + kSlotPressed);
    i32(g_slots, kSlotSize + kSlotPressed) = saved | (int32_t)kPadDecide;
    on = true;
  }
  void release() {
    if (on) i32(g_slots, kSlotSize + kSlotPressed) = saved;
  }
};

std::atomic<uint32_t> g_seen_ms{0};

// --- players 3 and 4 pick too (four_players.cpp) ----------------------------------
// The game picks for two: player 1 (states 2, 3), then player 2 (4 prepares
// player 2's menu with player 1's turtle taken, 5 picks, 6 asks "are you
// sure?"), then 7 on. With three or four, players 3 and 4 have player 2's turn
// after it, in order: when the "yes" of a turn would go on to 7, it goes to 4
// again and the turtles picked so far are taken in the menu; player 2's pad
// slot carries the buttons of the player whose turn it is
// (four::lend_slot). The picks are kept here; player 2's goes back to its place
// before 7 (7 shows and commits players 1 and 2), players 3 and 4's go to
// four::set_picks. A cancel in player 3 or 4's pick goes back one turn.
// Everything here follows the game's state and the players' buttons only: the
// same on every PC of a game together.
constexpr int kStatePrepareP2 = 4, kStateOn = 7;
constexpr size_t kChoice2 = 0x1C, kCostume2 = 0x24, kTimer = 0x0C;
uint8_t* g_turn_cs = nullptr;
int g_turn = 2;      // whose turn the game's player 2 states are (2..4)
int g_pick[4] = {};  // the turtles picked, by player (player 1 is the game's)
int g_costume[4] = {};
bool g_all_picked = false;  // three or four: every player's turtle is in g_pick

void again(uint8_t* cs, int state) {
  i32(cs, 4) = state;
  i32(cs, kStep) = 0;
  *reinterpret_cast<float*>(cs + kTimer) = 0.0f;
}

void take_item(Menu* m, int value) {
  for (int i = 0; m && value && i < m->count; ++i)
    if (i32(m->items, i * 0x18 + 4) == value) i32(m->items, i * 0x18) = 1;
}

// After the game's update: a turn's end, a cancel, a new menu.
void follow_turns(uint8_t* cs, int before) {
  int after = i32(cs, 4), n = four::count();
  bool more = i32(cs, 0) != 0 && n >= 3;
  if (more && before == kStateP2Confirm && after == kStateOn) {
    g_pick[g_turn - 1] = i32(cs, kChoice2);
    g_costume[g_turn - 1] = i32(cs, kCostume2);
    if (g_turn < n) {
      ++g_turn;
      again(cs, kStatePrepareP2);
      LOGI("char select: player %d picks now (taken: %d %d %d)", g_turn, i32(cs, 0x18), g_pick[1], g_pick[2]);
    } else {
      i32(cs, kChoice2) = g_pick[1];  // the game shows and commits players 1 and 2
      i32(cs, kCostume2) = g_costume[1];
      four::set_picks(g_pick[2], n >= 4 ? g_pick[3] : 0);
      LOGI("char select: all %d picked (%d %d %d %d)", n, i32(cs, 0x18), g_pick[1], g_pick[2], g_pick[3]);
      g_all_picked = true;
      g_turn = 2;
      csel_art::restore();
    }
  } else if (more && g_turn >= 3 && before == kStateP2Picks && after == 1) {
    --g_turn;  // the cancel of player 3 or 4: the one before picks again
    g_pick[g_turn - 1] = 0;
    if (g_turn == 2) csel_art::restore();
    again(cs, kStatePrepareP2);
    LOGI("char select: back to player %d's pick", g_turn);
  } else if (more && g_turn >= 3 && before == kStatePrepareP2 && after == kStateP2Picks) {
    Menu* m = menu_of(cs, 1);  // made as player 2's: player 1's turtle taken
    for (int p = 1; p < g_turn - 1; ++p) take_item(m, g_pick[p]);
  }
  int now = i32(cs, 4);
  four::lend_slot(more && g_turn >= 3 && now >= kStatePrepareP2 && now <= kStateP2Confirm ? g_turn - 1 : -1);
}

// --- the costume (costume.cpp) ------------------------------------------------------
// A step more in a player's pick: its decide on a turtle opens the costume
// choice (the turtle stays pointed at): left / right go through the six
// costumes, a second decide goes on to the game's "are you sure?" with that
// turtle, cancel goes back to the turtles. While it is open the pick's menu
// hears nothing (its pad slot's buttons are taken for the game's update).
// Only the players' buttons decide it: the same on every PC of a game together.
constexpr uint32_t kPadRight = 0x2000, kPadLeft = 0x8000, kPadCancel = 0x20000;
constexpr size_t kSlotRepeated = 0x18;
constexpr int kStateP1Picks = 2;
constexpr int kSoundMove = 0x263, kSoundDecide = 0x264, kSoundCancel = 0x265;
int g_choice[4] = {};       // each player's costume
bool g_step = false;        // the player whose turn it is chooses its costume now

struct Taken {  // a pad slot's buttons as they were before the costume step
  int slot = -1;
  int32_t pressed = 0, repeated = 0;
  void give_back() const {
    if (slot < 0) return;
    i32(g_slots, slot * kSlotSize + kSlotPressed) = pressed;
    i32(g_slots, slot * kSlotSize + kSlotRepeated) = repeated;
  }
};

Taken costume_step(uint8_t* cs, int state) {
  Taken t;
  int slot = state == kStateP1Picks ? 0 : state == kStateP2Picks && i32(cs, 0) ? 1 : -1;
  Menu* m = slot >= 0 && costume::ready() && g_cfg.debug_costume_step ? menu_of(cs, slot) : nullptr;
  if (!m || m->cursor < 0 || m->cursor >= m->count) {
    g_step = false;
    return t;
  }
  int player = slot == 0 ? 0 : g_turn - 1;
  int value = i32(m->items, m->cursor * 0x18 + 4);
  bool turtle = i32(m->items, m->cursor * 0x18) == 2 && value >= 1 && value <= 4;
  if (!turtle) {
    g_step = false;
    return t;
  }
  int32_t& pressed = i32(g_slots, slot * kSlotSize + kSlotPressed);
  int32_t& repeated = i32(g_slots, slot * kSlotSize + kSlotRepeated);
  t.slot = slot, t.pressed = pressed, t.repeated = repeated;
  if (!g_step) {
    if (pressed & (int32_t)kPadDecide) {  // the turtle: now its costume
      g_step = true;
      pressed &= ~(int32_t)kPadDecide;
      main_menu::play_sound(kSoundDecide);
      LOGI("char select: player %d chooses the costume of turtle %d", player + 1, value);
    }
    return t;
  }
  int& c = g_choice[player];
  if (pressed & (int32_t)kPadLeft) {
    c = (c + costume::kCount - 1) % costume::kCount;
    main_menu::play_sound(kSoundMove);
  } else if (pressed & (int32_t)kPadRight) {
    c = (c + 1) % costume::kCount;
    main_menu::play_sound(kSoundMove);
  }
  const bool go = (pressed & (int32_t)kPadDecide) != 0;
  if (go) {
    g_step = false;  // (the decide goes on to the game: "are you sure?")
    LOGI("char select: player %d's turtle %d wears costume %d", player + 1, value, c);
  } else if (pressed & (int32_t)kPadCancel) {
    g_step = false;  // back to the turtles
    main_menu::play_sound(kSoundCancel);
  }
  pressed = go ? (int32_t)kPadDecide : 0;
  repeated = 0;
  if (g_step) {
    native_menu::Prompt p;
    p.title = "КОСТЮМ";
    p.count = std::to_string(c + 1) + " / " + std::to_string(costume::kCount);
    p.value = costume::name(c);
    p.hint = "ВЛЕВО / ВПРАВО - ДРУГОЙ|ВЫБРАТЬ - ДАЛЬШЕ|ОТМЕНА - НАЗАД";
    p.rgb = nametags::mask_rgb(value);
    native_menu::show_prompt(p);
  }
  return t;
}

// After the game's update: who wears what so far (the game stores and loads
// the costumes by it, costume.cpp).
void publish_costumes(uint8_t* cs) {
  costume::clear();
  int state = i32(cs, 4);
  if (state < 3) return;
  costume::set(i32(cs, 0x18), g_choice[0]);  // player 1 has decided
  if (!i32(cs, 0)) return;
  const int n = four::count();
  if (g_all_picked) {  // (three or four: the game's place holds player 2's again)
    for (int p = 1; p < n && p < 4; ++p) costume::set(g_pick[p], g_choice[p]);
    return;
  }
  for (int p = 1; p < g_turn - 1; ++p) costume::set(g_pick[p], g_choice[p]);  // the turns before this one
  if (state >= kStateP2Confirm) costume::set(i32(cs, kChoice2), g_choice[g_turn - 1]);
}

int g_tag = 0;  // whose tag is being drawn

// --- the select's turtles with several taken ---------------------------------------
// The select's turtles are a task (made by 0x496aab; its draw 0x496a87 calls
// 0x4967e3 with ebx = its data): two backgrounds by whose turn it is (+8), then
// the four turtles (sprites at +0x1C + n * 0x3C), the one taken (+4) dark
// (0x40) with the tag "ИГРОК 1" (sprite +0x3DC) on it; then, while it is shown
// (+0x10), the pointed one's name bubble (0x496913, esi = data) and more
// (0x4969fd, edi = data). In player 3 or 4's turn every turtle taken so far is
// dark with the tag of who took it: its draw is ours then, the same steps.
struct Spot {
  int value, sprite;
  float x, y;
};
constexpr Spot kSpots[4] = {{3, 0xE, 157, -20}, {4, 0xF, -181, -53}, {1, 0xC, 7, 119}, {2, 0xD, 24, -154}};
constexpr size_t kTurtleSprites = 0x1C, kTurtleSprite = 0x3C, kTag = 0x3DC;
using Sprite_t = void(__cdecl*)(uint8_t* sprite);
using Colour_t = void(__cdecl*)(uint8_t* sprite, int r, int g, int b, int a);  // (0x4aba36: bytes +0x14..+0x17)
using Place_t = void(__cdecl*)(uint8_t* sprite, float x, float y);
Sprite_t g_draw = nullptr;
Colour_t g_colour = nullptr;
Place_t g_place = nullptr;
Sprite_t g_turtles_draw = nullptr;  // the game's (0x496a87)
uintptr_t g_bubble = 0, g_more = 0;

void call_esi(uintptr_t fn, uint8_t* d) {
  __asm {
    mov esi, d
    call fn
  }
}
void call_edi(uintptr_t fn, uint8_t* d) {
  __asm {
    mov edi, d
    call fn
  }
}

constexpr size_t kSprite = 0x3C;
uint8_t g_tag_model[kSprite];
bool g_have_model = false;
int g_pointed = 0;  // the turtles' pointed one (their data +0)

bool names_shown() {
  for (int p = 0; p < 4; ++p)
    if (!nametags::name(p).empty()) return true;
  return false;
}

// Under a taken turtle's tag: the name of who took it, in its mask's colour (a
// copy of the tag's sprite a tag lower, its picture the name: csel_art.cpp).
void name_under(const uint8_t* tag, int player, int character) {
  std::string n = nametags::name(player - 1);
  if (n.empty()) return;
  uint8_t copy[kSprite];
  std::memcpy(copy, tag, kSprite);
  float h = *reinterpret_cast<float*>(copy + 0x10);
  *reinterpret_cast<float*>(copy + 4) -= h * 0.85f;
  copy[0x14] = copy[0x15] = copy[0x16] = 0xFF;
  csel_art::name_next(n, character);
  g_draw(copy);
  csel_art::name_done();
}

void __cdecl hkTurtlesDraw(uint8_t* d) {
  std::memcpy(g_tag_model, d + kTag, kSprite);
  g_have_model = true;
  int shown = i32(d, 0);
  g_pointed = shown >= 1 && shown <= 4 ? shown : 0;
  if (turn() < 3 && !names_shown()) return g_turtles_draw(d);
  bool first = i32(d, 8) == 0;
  g_draw(d + (first ? 0x1C : 0x58));
  g_draw(d + (first ? 0x94 : 0xD0));
  for (const Spot& s : kSpots) {
    uint8_t* sp = d + kTurtleSprites + s.sprite * kTurtleSprite;
    int who = taker(s.value);
    if (!who) {
      g_colour(sp, 0xFF, 0xFF, 0xFF, 0xFF);
      g_draw(sp);
      continue;
    }
    g_colour(sp, 0x40, 0x40, 0x40, 0xFF);
    g_draw(sp);
    g_place(d + kTag, s.x, s.y);
    g_tag = who;
    g_draw(d + kTag);
    g_tag = 0;
    name_under(d + kTag, who, s.value);
  }
  if (i32(d, 0x10)) {
    call_esi(g_bubble, d);
    call_edi(g_more, d);
  }
}

uintptr_t call_at(uintptr_t at) {
  if (!at || patch::read<uint8_t>(at) != 0xE8) return 0;
  uintptr_t t = at + 5 + patch::read<int32_t>(at + 1);
  return patch::in_image(t, 1) ? t : 0;
}

void install_turtles() {
  uintptr_t make = patch::find("csel_turtles_make",
                               "55 8B EC 83 EC 20 6A 02 58 33 C9 51 89 45 E0 89 45 E8 51 8D 45 E0 50 89 4D E4 C7 45 EC 18 04 "
                               "00 00 C7 45 F0 ?? ?? ?? ?? C7 45 F4 ?? ?? ?? ?? C7 45 F8 ?? ?? ?? ?? C7 45 FC ?? ?? ?? ??");
  uintptr_t cb = patch::find("csel_turtles_draw",
                             "53 56 8B 74 24 0C 8B DE E8 ?? ?? ?? ?? 83 7E 10 00 74 0E 57 E8 ?? ?? ?? ?? 8B FE E8 ?? ?? ?? ?? "
                             "5F 5E 5B C3");
  uintptr_t body = patch::find("csel_turtles_body",
                               "55 8B EC 83 EC 48 83 7B 08 00 56 57 6A 04 5E C7 45 B8 03 00 00 00 C7 45 BC 0E 00 00 00");
  if (!make || !cb || !body || patch::read<uint32_t>(make + 0x39) != (uint32_t)cb || call_at(cb + 0x08) != body) {
    LOGW("char select: the turtles' draw is not where expected; players 3 and 4 see one taken turtle");
    return;
  }
  g_draw = reinterpret_cast<Sprite_t>(call_at(body + 0x81));
  g_colour = reinterpret_cast<Colour_t>(call_at(body + 0xCD));
  g_place = reinterpret_cast<Place_t>(call_at(body + 0xF0));
  g_bubble = call_at(cb + 0x14);
  g_more = call_at(cb + 0x1B);
  if (!g_draw || !g_colour || !g_place || !g_bubble || !g_more) {
    LOGW("char select: the turtles' draw steps are not where expected; players 3 and 4 see one taken turtle");
    return;
  }
  g_turtles_draw = reinterpret_cast<Sprite_t>(cb);
  uint32_t ours = (uint32_t)reinterpret_cast<uintptr_t>(&hkTurtlesDraw);
  if (patch::write(make + 0x39, &ours, 4)) LOGI("char select: the turtles' draw shows every taken turtle");
}

void __cdecl hkUpdate(uint8_t* cs) {
  uint32_t now = net::now_ms();
  g_seen_ms = now;
  int state = i32(cs, 4);
  bool two = i32(cs, 0) != 0;
  const bool new_select = cs != g_turn_cs || state == 0;  // (by the game's state: the same on every PC)
  if (new_select || state <= 1) {  // a new select, or player 1 picks (again)
    if (g_turn != 2) csel_art::restore();
    g_turn_cs = cs;
    g_turn = 2;
    std::memset(g_pick, 0, sizeof(g_pick));
    std::memset(g_costume, 0, sizeof(g_costume));
    four::set_picks(0, 0);
    g_all_picked = false;
    g_step = false;
    if (new_select) {  // (a cancel back to player 1 keeps the costumes chosen)
      for (int& c : g_choice) c = costume::initial();
    }
  }
  if (cs != g_w.cs || state != g_w.state) {
    g_w.cs = cs;
    g_w.state = state;
    g_w.since_ms = now;
    if (state == kStateP2Picks) g_w.p2_moved = g_w.picked = false;
    if (state == kStateP2Confirm) g_w.confirmed = false;
    log_state(cs);
  }

  // (before the help below: its decide for an absent player 2 is not the costume step's)
  const Taken taken = costume_step(cs, state);
  Decide decide;
  bool net = g_p2.connected.load(std::memory_order_acquire);
  // A game together (the game on every PC): P2 is a real player, and the
  // help below goes by this PC's clock, so it would part the two games.
  bool together = net || lockstep::net_active();
  if (two && state == kStateP2Confirm && g_turn == 2 && g_w.picked && !g_w.confirmed && !together && i32(cs, kStep) == 1 &&
      now - g_w.since_ms >= kConfirmDelayMs) {
    uint8_t* dialog = *reinterpret_cast<uint8_t**>(cs + kP2Dialog);
    uint8_t* d = dialog ? *reinterpret_cast<uint8_t**>(dialog + kTaskData) : nullptr;
    int ds = d ? i32(d, kDialogState) : 0;
    if (d && (ds == 1 || ds == 2) && i32(d, kDialogAnswer) == 0) {
      i32(d, kDialogAnswer) = kAnswerYes;
      g_w.confirmed = true;
      LOGI("char select: answered \"are you sure?\" with yes for P2");
    }
  }
  if (two && state == kStateP2Picks) {
    Menu* m = menu_of(cs, 1);
    // Remote P2 present: P2's cursor reads pad slot 1, which must belong to
    // physical player 1 (the one the mod injects as).
    if (net && g_assign && slot_player(1) != 1) {
      LOGI("char select: pad slot 1 had player %d, giving it to P2", slot_player(1));
      g_assign(1, 1);
    }
    if (!together && g_cfg.p2_autopick && g_turn == 2 && m && !g_w.p2_moved && !g_w.picked &&
        now - g_w.since_ms >= (uint32_t)g_cfg.p2_autopick_delay_ms) {
      int item = choose_item(m);
      if (item >= 0) {
        m->cursor = item;
        decide.press();
        g_w.picked = true;
        LOGI("char select: no remote P2, picked item %d (value %d) for P2", item,
             i32(m->items, item * 0x18 + 4));
      }
    }
  }

  oUpdate(cs);
  decide.release();
  taken.give_back();
  follow_turns(cs, state);
  publish_costumes(cs);

  if (two && state == kStateP2Picks) {
    Menu* m = menu_of(cs, 1);
    if (m && m->event == 2 && !g_w.p2_moved) {
      g_w.p2_moved = true;
      LOGI("char select: P2 cursor moved by a player, no auto-pick");
    }
  }
}

}  // namespace

void install(const Config& cfg) {
  g_cfg = cfg;
  uintptr_t assign = patch::find("assign_pad_slot", kAssignSig);
  if (assign) {
    g_slots = reinterpret_cast<uint8_t*>(patch::read<uint32_t>(assign + 13));
    if (patch::in_image((uintptr_t)g_slots, 2 * kSlotSize))
      g_assign = reinterpret_cast<Assign_t>(assign);
    else
      g_slots = nullptr;
  }
  uintptr_t upd = patch::find("char_select_update", kUpdateSig);
  if (!upd || !g_slots) {
    LOGW("char select support disabled (game code not recognised)");
    return;
  }
  patch::hook("char_select_update", upd, reinterpret_cast<void*>(&hkUpdate),
              reinterpret_cast<void**>(&oUpdate));
  install_turtles();
  csel_art::install();
  LOGI("char select: P2 auto-pick %s (delay %d ms, character %s)",
       cfg.p2_autopick ? "on when no remote P2" : "off", cfg.p2_autopick_delay_ms,
       cfg.p2_character >= 0 ? std::to_string(cfg.p2_character).c_str()
       : cfg.p2_character == -2 ? "first free" : "random free");
}

bool on_screen() { return net::now_ms() - g_seen_ms.load() < 300; }

int turn() { return on_screen() ? g_turn : 2; }

int tag_player() { return g_tag; }

int picker() {
  if (!on_screen() || !g_turn_cs) return 0;
  int state = i32(g_turn_cs, 4);
  if (state == 2 || state == 3) return 1;
  if (state >= kStatePrepareP2 && state <= kStateP2Confirm && i32(g_turn_cs, 0)) return g_turn;
  return 0;
}

int pointed() { return on_screen() ? g_pointed : 0; }

bool tag_model(uint8_t out[0x3C]) {
  if (!g_have_model || !on_screen()) return false;
  std::memcpy(out, g_tag_model, kSprite);
  return true;
}

int taker(int value) {
  if (!g_turn_cs || !value) return 0;
  int state = i32(g_turn_cs, 4);  // (player 1 picks: nothing taken yet)
  if (state < kStatePrepareP2 || state > kStateP2Confirm || !i32(g_turn_cs, 0)) return 0;
  if (i32(g_turn_cs, 0x18) == value) return 1;
  for (int p = 1; p < g_turn - 1; ++p)
    if (g_pick[p] == value) return p + 1;
  return 0;
}

void set_autopick(bool on, int delay_ms, int character) {
  g_cfg.p2_autopick = on;
  g_cfg.p2_autopick_delay_ms = delay_ms;
  g_cfg.p2_character = character;
}

}  // namespace csel
