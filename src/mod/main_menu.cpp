#include "mod/main_menu.h"

#include <windows.h>
#include <intrin.h>
#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>

#include "common/log.h"
#include "common/net.h"
#include "mod/costume.h"
#include "mod/csel.h"
#include "mod/csel_art.h"
#include "mod/four_hud.h"
#include "mod/load_guard.h"
#include "mod/lockstep.h"
#include "mod/four_players.h"
#include "mod/game_text.h"
#include "mod/native_menu.h"
#include "mod/net_pages.h"
#include "mod/overlay.h"
#include "mod/patch.h"
#include "mod/story_menu.h"
#include "mod/ui.h"
#include "mod/widescreen.h"

namespace main_menu {
namespace {

// --- game layout (TMNT.EXE bc49a82c..., see re/notes.md section 9) -----------
// Main menu task data: +0 cursor index (for the cursor sprite), +8 ready,
// +0x10 busy, +0x14 menu object, +0xA4 item sprites (0x60 each), +0xB0 count.
// Item sprite: +0 alpha (0..255 float), +4 alpha target, +0x1C/+0x20 x/y now.
// Menu object: +0xC cursor, +0x10 items (0x18 each: state, value, l, r, up, down).
// Pad slots: 2 x 0xAC, +0x10 pressed (0x10000 = decide).
constexpr size_t kTaskCursor = 0x00, kTaskReady = 0x08, kTaskBusy = 0x10, kTaskMenu = 0x14,
                 kTaskSprites = 0xA4, kTaskCount = 0xB0;
constexpr size_t kSpriteSize = 0x60, kSpriteAlpha = 0x00, kSpriteAlphaTarget = 0x04,
                 kSpriteX = 0x1C, kSpriteY = 0x20;
constexpr size_t kMenuFlags = 0x04, kMenuCursor = 0x0C, kMenuItems = 0x10, kItemSize = 0x18;
constexpr size_t kItemState = 0x00, kItemValue = 0x04, kItemUp = 0x10, kItemDown = 0x14;
constexpr size_t kSlotSize = 0xAC, kSlotPressed = 0x10;
constexpr uint32_t kPadDecide = 0x10000, kPadCancel = 0x20000, kPadDirections = 0xF000;
constexpr int kSoundDecide = 0x264, kSoundInvalid = 0x266;

constexpr int kOldCount = 8, kNewItem = 8, kStory = 0, kVs = 1, kChallenge = 2, kSave = 5, kLoad = 6, kQuit = 7;
// The new item is the second row, right under STORY (network co-op is a way
// to play the story): VS..LOAD move one row down (rows are 39 units apart,
// the background's hex column continues there), QUIT stays. Same column as
// the others (left edge -206): sprite centre (-142, 143), VS's old place.
constexpr int kRow = 39;
constexpr int kItemX = -142, kItemY = 143, kItemW = 128, kItemH = 32, kItemLayer = 3;
constexpr float kCursorX = -200.0f, kCursorY = 145.0f;  // like the other rows: y + 2
constexpr int kItemLeft = kItemX - kItemW / 2;            // -206, the column's left edge

// The item's picture is put together from the game's own letters (game_text)
// and the game draws it as its sprite: 0x4abab2(quad) binds the sprite's
// texture and draws it with Im2D, and tmntnet swaps its texture in for that
// one draw. So position, fades and draw order are the game's, in every mode.
// Its letters are the main menu's own (game_text::Style::MainMenu, at their
// size); for a label they lack, the option labels' (cap height 24 of 64 against
// the menu's 21: the quad scaled by 21/24).
constexpr const char* kDrawSpriteSig = "55 8D 6C 24 8C 81 EC 88 00 00 00 8B 45 7C 83 78 38 00 0F 84";
// The menu's draw (0x48ccdf): background, then the items. tmntnet's pages
// (native_menu) are drawn right after, inside the same frame.
constexpr const char* kMenuDrawSig = "56 8B 74 24 08 8D 46 68 50 E8 ?? ?? ?? ?? FF B6 B0 00 00 00 FF B6 A4 00 00 00 E8";
constexpr size_t kSpriteQuad = 0x24;
constexpr float kOptionLetters = 21.0f / 24.0f;
float g_letter_scale = 1.0f;  // the picture's letters to the menu's (1: they are the menu's own)

// Signatures.
constexpr const char* kInitSig =
    "55 8B EC 53 56 57 8B 7D 08 68 B4 00 00 00 33 DB 53 57 E8";
constexpr const char* kUpdateSig =
    "55 8B EC 51 51 56 57 8D 45 FC 50 8D 45 F8 50 E8 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 8B 7D 08 EB";
constexpr const char* kCountSig = "83 67 10 00 6A 08 89 07 58 6A 07";  // push 8 (items)
constexpr const char* kSpriteTableSig =
    "FF B7 B0 00 00 00 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 89 87 A4 00 00 00";
constexpr const char* kCursorTableSig =  // shared by every menu: pick ours by its data
    "FF 74 24 04 E8 ?? ?? ?? ?? C7 04 24 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 59 59 C3";
constexpr const char* kSoundSig =
    "55 8B EC 83 EC 0C 57 E8 ?? ?? ?? ?? 33 FF 39 78 08 75 07 33 C0 E9";
constexpr const char* kPadAssignSig =  // only for the pad slot array address
    "56 8B 74 24 08 69 F6 AC 00 00 00 81 C6 ?? ?? ?? ?? 83 3E 00 7C 05 E8";

#pragma pack(push, 1)
struct SpriteEntry {  // the game's layout table entry
  int32_t x, y, w, h, layer;
  const char* texture;
};
#pragma pack(pop)
static_assert(sizeof(SpriteEntry) == 0x18, "game layout");

SpriteEntry g_sprites[kOldCount + 1];
// ПОЕДИНОК and РЕЖИМ СОСТЯЗАНИЯ are pictures with bigger letters than the
// other items' (28 and 29 texels high against 21 in the Russian version) at the
// same 32 units: made smaller by that much, left edge kept. Measured on the
// pictures (game_text::white_rows) off the game thread, applied at the menu's
// init; 1 = as the game has it.
std::atomic<float> g_item_scale[kOldCount] = {1, 1, 1, 1, 1, 1, 1, 1};
SpriteEntry g_game_sizes[kOldCount];  // the table as the game made it (sizes and places)

void fit_item(int i) {
  const SpriteEntry& g = g_game_sizes[i];
  SpriteEntry& e = g_sprites[i];
  float k = g_item_scale[i].load();
  int left = g.x - g.w / 2;
  e.w = (int)std::lround(g.w * k);
  e.h = (int)std::lround(g.h * k);
  e.x = left + e.w / 2;
}
float g_cursor[(kOldCount + 1) * 2];

using Init_t = int(__cdecl*)(uint8_t* task, void* params);
using Update_t = void(__cdecl*)(uint8_t* task);
using Sound_t = int(__cdecl*)(int id);
Init_t oInit = nullptr;
Update_t oUpdate = nullptr;
Sound_t g_sound = nullptr;
uint8_t* g_slots = nullptr;
bool g_native = false;

std::atomic<uint32_t> g_seen_ms{0};
uint32_t g_updates = 0;  // the menu task's updates so far (game thread)
uint8_t* g_task_now = nullptr;  // the menu task seen last (game thread)
uint8_t* g_task = nullptr;  // the live main menu (game thread only)

using DrawSprite_t = void(__cdecl*)(uint8_t* quad);
DrawSprite_t oDrawSprite = nullptr;
using MenuDraw_t = void(__cdecl*)(uint8_t* task);
MenuDraw_t oMenuDraw = nullptr;

// The pad slots' per-frame update (0x4a984d, called by the game's input
// pass). While a page of ours is open its buttons are taken right here, so no
// menu of the game sees them (menus read the slots outside their task update).
constexpr const char* kPadUpdateSig = "55 8B EC 83 EC 0C 53 56 57 E8 ?? ?? ?? ?? 83 65 F4 00 BE ?? ?? ?? ?? 8D 7E F4";
constexpr size_t kSlotHeld = 0x0C;
using PadUpdate_t = void(__cdecl*)();
PadUpdate_t oPadUpdate = nullptr;
uint32_t g_page_pressed = 0;  // taken from the slots for the page (game thread)
int32_t g_menu_flags = 0;     // the menu object's input flags while a page is open
uint8_t* g_slots_early = nullptr;

void __cdecl hkPadUpdate() {
  oPadUpdate();
  four::after_pad_update();  // players 3 and 4
  if (!g_slots_early || !native_menu::active()) return;
  for (int s = 0; s < 2; ++s) {
    uint8_t* slot = g_slots_early + s * 0xAC;
    g_page_pressed |= *reinterpret_cast<uint32_t*>(slot + 0x10);
    *reinterpret_cast<uint32_t*>(slot + 0x10) = 0;  // pressed
    *reinterpret_cast<uint32_t*>(slot + kSlotHeld) = 0;
  }
}
bool g_items_hidden = false;  // a page of ours is over the menu

void __cdecl hkMenuDraw(uint8_t* task) {
  oMenuDraw(task);
  native_menu::draw();
}

std::atomic<int> g_picture{0};  // 0 being made, 1 ready, -1 not possible (label drawn by ImGui)
game_text::Bitmap g_bitmap;
IDirect3DTexture9* g_texture = nullptr;
IDirect3DDevice9* g_texture_dev = nullptr;
uint8_t* g_item_quad = nullptr;  // our sprite's quad in the live menu
bool g_native_label = false;     // this menu draws the picture (not the ImGui label)

// ЗАПИСЬ is drawn in the game in another design (rounder, wider letters):
// its picture is swapped for the word in the other items' lettering.
std::atomic<bool> g_save_ready{false};
game_text::Bitmap g_save_bitmap;
IDirect3DTexture9* g_save_texture = nullptr;
IDirect3DDevice9* g_save_dev = nullptr;

// A picture's texture on the game's device (made again for a new device).
IDirect3DTexture9* texture_of(const game_text::Bitmap& b, IDirect3DTexture9*& kept, IDirect3DDevice9*& owner) {
  IDirect3DDevice9* dev = overlay::device();
  if (!dev) return nullptr;
  if (kept && owner == dev) return kept;
  if (kept) kept->Release(), kept = nullptr;  // (a new device)
  IDirect3DTexture9* t = nullptr;
  if (FAILED(dev->CreateTexture(b.w, b.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) return nullptr;
  D3DLOCKED_RECT lr{};
  if (FAILED(t->LockRect(0, &lr, nullptr, 0))) {
    t->Release();
    return nullptr;
  }
  for (int y = 0; y < b.h; ++y)
    memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, &b.bgra[size_t(y) * b.w], size_t(b.w) * 4);
  t->UnlockRect(0);
  kept = t;
  owner = dev;
  return t;
}

IDirect3DTexture9* picture_texture() { return texture_of(g_bitmap, g_texture, g_texture_dev); }

// RwTexture: +0 raster, +0x10 name[32].
bool texture_named(const uint8_t* quad, const char* name) {
  const char* tex = *reinterpret_cast<const char* const*>(quad + 0x18);
  return tex && strncmp(tex + 0x10, name, 32) == 0;
}

bool g_trace_textures = false;  // [debug] actions=1: the character select's textures, once each

void trace_texture(const char* tex) {
  static std::string seen;
  std::string name(tex, strnlen(tex, 32));
  if (seen.find("|" + name + "|") != std::string::npos || seen.size() > 4000) return;
  seen += "|" + name + "|";
  LOGI("trace: the character select draws texture '%s'", name.c_str());
}

void draw_sprite(uint8_t* quad);

void __cdecl hkDrawSprite(uint8_t* quad) {
  four_hud::trace(quad, _ReturnAddress(), "sprite");  // ([debug] hud_trace)
  four_hud::Scaled size(quad, _ReturnAddress());  // [video] hud_size, hud_corners
  const char* tex = quad ? *reinterpret_cast<const char* const*>(quad + 0x18) : nullptr;
  widescreen::set_sprite(tex ? tex + 0x10 : nullptr);  // (which picture it is: the wide menus)
  draw_sprite(quad);
  widescreen::set_sprite(nullptr);
  csel_art::after_sprite(quad, oDrawSprite);  // (the character select: who picks, under the centre plate)
}

void draw_sprite(uint8_t* quad) {
  // (players 3 and 4's panels go with player 1's; the team has no hearts)
  if (!four_hud::on_sprite(quad)) return;
  if (quad && native_menu::covers_screen() && texture_named(quad, "menu_cursor")) return;  // a page is over the menu
  if (const uint8_t* tex = quad ? *reinterpret_cast<const uint8_t* const*>(quad + 0x18) : nullptr) {
    if (g_trace_textures && csel::on_screen()) trace_texture(reinterpret_cast<const char*>(tex + 0x10));
    if (IDirect3DTexture9* t = csel_art::instead_of(tex)) {  // ИГРОК 3 / 4 in the character select
      widescreen::set_texture_override(t);
      oDrawSprite(quad);
      widescreen::set_texture_override(nullptr);
      return;
    }
  }
  if (quad && g_save_ready.load() && texture_named(quad, "menu_moj_save")) {  // ЗАПИСЬ
    if (IDirect3DTexture9* t = texture_of(g_save_bitmap, g_save_texture, g_save_dev)) {
      widescreen::set_texture_override(t);
      oDrawSprite(quad);
      widescreen::set_texture_override(nullptr);
      return;
    }
  }
  if (IDirect3DTexture9* t = story_menu::texture_for(quad)) {  // ТРИ / ЧЕТЫРЕ ИГРОКА
    widescreen::set_texture_override(t);
    oDrawSprite(quad);
    widescreen::set_texture_override(nullptr);
    return;
  }
  if (quad && quad == g_item_quad && g_native_label && net::now_ms() - g_seen_ms.load() < 250) {
    if (IDirect3DTexture9* t = picture_texture()) {
      widescreen::set_texture_override(t);
      oDrawSprite(quad);
      widescreen::set_texture_override(nullptr);
      return;
    }
  }
  oDrawSprite(quad);
}
Label g_label;              // where our item is now, as of g_label_ms
uint32_t g_label_ms = 0;

int32_t& i32(uint8_t* p, size_t off) { return *reinterpret_cast<int32_t*>(p + off); }
float& f32(uint8_t* p, size_t off) { return *reinterpret_cast<float*>(p + off); }

uint8_t* menu_of(uint8_t* task) { return *reinterpret_cast<uint8_t**>(task + kTaskMenu); }
uint8_t* item_of(uint8_t* menu, int i) {
  return *reinterpret_cast<uint8_t**>(menu + kMenuItems) + i * kItemSize;
}

int __cdecl hkInit(uint8_t* task, void* params) {
  if (g_native) {
    fit_item(kVs), fit_item(kChallenge);
    if (g_save_ready.load()) {  // ЗАПИСЬ: as wide as its new picture, left edge kept
      const SpriteEntry& g = g_game_sizes[kSave];
      SpriteEntry& e = g_sprites[kSave];
      e.w = g_save_bitmap.w / 2;
      e.x = g.x - g.w / 2 + e.w / 2;
    }
  }
  g_native_label = g_native && g_picture.load() == 1 && oDrawSprite;
  if (g_native_label) {
    int w = (int)std::lround(g_bitmap.w * 0.5f * g_letter_scale), h = (int)std::lround(kItemH * g_letter_scale);
    g_sprites[kNewItem].w = w;
    g_sprites[kNewItem].h = h;
    g_sprites[kNewItem].x = kItemLeft + w / 2;
  }
  load_guard::ensure("menu_bg", "menu_pc.txd");  // (its textures, before its sprites are made)
  int r = oInit(task, params);
  uint8_t* menu = menu_of(task);
  if (g_native && menu && i32(menu, 0x08) == kOldCount + 1) {
    // The game built a 9-item wrap-around list (... LOAD, QUIT, NET -> STORY).
    // Put the new item between STORY and VS, where it is drawn.
    uint8_t* net = item_of(menu, kNewItem);
    i32(net, kItemState) = 2;
    i32(net, kItemValue) = kNewItem;
    i32(item_of(menu, kStory), kItemDown) = kNewItem;
    i32(net, kItemUp) = kStory;
    i32(net, kItemDown) = kVs;
    i32(item_of(menu, kVs), kItemUp) = kNewItem;
    i32(item_of(menu, kLoad), kItemDown) = kQuit;
    i32(item_of(menu, kQuit), kItemUp) = kLoad;
    i32(item_of(menu, kQuit), kItemDown) = kStory;
    i32(item_of(menu, kStory), kItemUp) = kQuit;
    LOGI("main menu: network item in place (9 items, second row)");
  }
  g_task = task;
  return r;
}

// While a page is open the menu's items fade out (the game moves each item's
// alpha towards its target), and fade back in when it closes.
void set_items_hidden(uint8_t* task, bool hidden) {
  if (hidden == g_items_hidden) return;
  g_items_hidden = hidden;
  int n = i32(task, kTaskCount);
  uint8_t* sprites = *reinterpret_cast<uint8_t**>(task + kTaskSprites);
  for (int i = 0; sprites && i < n && i <= kOldCount; ++i)
    f32(sprites + i * kSpriteSize, kSpriteAlphaTarget) = hidden ? 0.0f : 255.0f;
}

// НАЧАТЬ ИГРУ (lockstep's story command: the same frame on every PC): the
// story for as many as play, the way the game itself takes when 2 ИГРОКА is
// decided in РЕЖИМ СЮЖЕТА. That submenu (0x48d36f) posts message 8 with
// {its mode: 0 story, the row's value: 0 one player, 1 two} to this menu,
// which passes it to its own parent, the menu screen (0x48cf03:
// 0x4abf14(parent [+0x58], 8, data)); the screen keeps the two numbers
// (0x48c8a1), goes dark and starts the character select (0x48c924: the
// players' records cleared, 0x404399(1 or 2), scene 4). Here the same
// message goes straight to the screen, and this menu takes no more buttons
// (+8 = 0, as after a choice of its own: 0x48cfc5). Three and four play by
// the way of two (four_players.cpp).
constexpr const char* kPostSig = "8D 8F A8 00 00 00 51 89 01 6A 08 FF 77 58 E8 ?? ?? ?? ?? 83 C4 0C 89 77 08";
constexpr size_t kPostCall = 14, kTaskParent = 0x58;
constexpr int kMsgChoice = 8;
using Post_t = void(__cdecl*)(void* task, int message, void* data);
Post_t g_post = nullptr;
bool g_story_wanted = false;
uint32_t g_story_since = 0;  // (the session's frame)

void start_story(uint8_t* task) {
  static int32_t choice[2];  // (read by the screen in its own update)
  const int n = std::clamp(lockstep::players(), 1, 4);
  choice[0] = kStory;
  choice[1] = n >= 2 ? 1 : 0;
  four::set_count(n);
  if (native_menu::active()) native_menu::close_all();
  g_post(*reinterpret_cast<void**>(task + kTaskParent), kMsgChoice, choice);
  i32(task, kTaskReady) = 0;
  story_menu::stand_by();
  LOGI("main menu: НАЧАТЬ ИГРУ: the story for %d, the choice given to the menu screen (frame %u)", n,
       lockstep::frame());
}

void __cdecl hkUpdate(uint8_t* task) {
  uint32_t now = net::now_ms();
  if (now - g_seen_ms.load() > 1000) LOGI("main menu on screen");
  g_seen_ms = now;
  ++g_updates;
  g_task_now = task;
  costume::clear();  // (a story's costumes end here: the game decides by itself again)

  // The host's НАЧАТЬ ИГРУ: as soon as this menu takes buttons (the game's
  // own state, the same on every PC).
  if (lockstep::take_story_command()) g_story_wanted = true, g_story_since = lockstep::frame();
  if (g_story_wanted) {
    if (lockstep::state() != lockstep::State::Running || lockstep::frame() - g_story_since > 900) {
      g_story_wanted = false;
    } else if (g_post && i32(task, kTaskReady) && !i32(task, kTaskBusy)) {
      g_story_wanted = false;
      start_story(task);
    }
  }

  // In a friend's game its page (the players, Я ГОТОВ) is the screen while
  // the game together waits: it comes up when the session is set (in the
  // place of the pages open), goes when the host starts (its game at the main
  // menu, this player ГОТОВ) so that this game is at its main menu too, and
  // comes back if the host went back to its own page instead.
  static bool lobby_wanted = false;
  static uint32_t gone_for_start = 0;  // when it went for the host's start (0: it did not)
  if (lockstep::take_close_pages()) lobby_wanted = true;
  const lockstep::Waiting waits = lockstep::waiting();
  const bool guest_waits = waits.pending && lockstep::is_guest();
  const bool host_starts = guest_waits && waits.there == lockstep::Where::Ready && lockstep::player_ready();
  if (!guest_waits) lobby_wanted = false, gone_for_start = 0;
  if (host_starts) {
    lobby_wanted = false;
    if (native_menu::active()) {
      native_menu::close_all();
      if (!gone_for_start) gone_for_start = now ? now : 1;
    }
  } else if (gone_for_start && now - gone_for_start > 2500) {
    gone_for_start = 0;
    lobby_wanted = true;
  }
  if (lobby_wanted) {
    if (native_menu::active()) {
      if (net_pages::show_guest()) lobby_wanted = false;
    } else if (i32(task, kTaskReady) && !i32(task, kTaskBusy)) {
      native_menu::set_host(native_menu::Host::MainMenu);
      if (net_pages::show_guest()) lobby_wanted = false;
    }
  }

  // A page of ours is open: its buttons are not the menu's.
  bool page = native_menu::active();
  if (g_native) set_items_hidden(task, native_menu::covers_screen());
  if (page && g_slots) {
    uint32_t pressed = g_page_pressed;
    g_page_pressed = 0;
    native_menu::update(pressed);
    // The menu object reads more than the pad slots (its flags 7): with no
    // flags it hears nothing until the page closes.
    uint8_t* m = menu_of(task);
    if (m && i32(m, kMenuFlags)) g_menu_flags = i32(m, kMenuFlags), i32(m, kMenuFlags) = 0;
    oUpdate(task);
    g_task = task;
    return;
  }
  g_page_pressed = 0;
  if (g_menu_flags) {
    if (uint8_t* m = menu_of(task)) i32(m, kMenuFlags) = g_menu_flags;
    g_menu_flags = 0;
  }

  // "Decide" on our item: open the network page instead of letting the
  // game post item 8 to its parent screen (which only knows 0..7).
  bool consumed = false;
  int32_t saved[2] = {0, 0};
  uint8_t* menu = g_native ? menu_of(task) : nullptr;
  if (menu && i32(task, kTaskReady) && !i32(task, kTaskBusy) && !ui::wants_input() &&
      i32(menu, kMenuCursor) == kNewItem && g_slots) {
    bool decide = false;
    for (int s = 0; s < 2; ++s) decide |= (i32(g_slots, s * kSlotSize + kSlotPressed) & kPadDecide) != 0;
    if (decide) {
      for (int s = 0; s < 2; ++s) {
        saved[s] = i32(g_slots, s * kSlotSize + kSlotPressed);
        i32(g_slots, s * kSlotSize + kSlotPressed) = saved[s] & ~(int32_t)kPadDecide;
      }
      consumed = true;
    }
  }

  // A game together is set and has not begun: the menu waits for the other
  // games (its network item still opens), so nobody goes off alone: the
  // cursor moves, decide and cancel do nothing (the game's "invalid" sound).
  int32_t kept[2] = {0, 0};
  bool hold = menu && !consumed && g_slots && lockstep::state() == lockstep::State::Pending;
  bool refused = false;
  if (hold) {
    for (int s = 0; s < 2; ++s) {
      int32_t& pressed = i32(g_slots, s * kSlotSize + kSlotPressed);
      kept[s] = pressed;
      refused |= (pressed & (int32_t)(kPadDecide | kPadCancel)) != 0;
      pressed &= (int32_t)kPadDirections;
    }
  }

  oUpdate(task);

  if (hold) {
    for (int s = 0; s < 2; ++s) i32(g_slots, s * kSlotSize + kSlotPressed) = kept[s];
    if (refused && g_sound) g_sound(kSoundInvalid);
  }
  if (consumed) {
    for (int s = 0; s < 2; ++s) i32(g_slots, s * kSlotSize + kSlotPressed) = saved[s];
    if (g_sound) g_sound(kSoundDecide);
    native_menu::set_host(native_menu::Host::MainMenu);
    net_pages::open_network((saved[0] & (int32_t)kPadDecide) ? 0 : 1);  // (whose decide: player 1's first)
  }
  // Our sprite borrows another item's texture: with the picture of game
  // letters it is swapped at draw time; otherwise the sprite stays invisible
  // and ui.cpp draws the label.
  if (g_native && i32(task, kTaskCount) == kOldCount + 1) {
    uint8_t* sprites = *reinterpret_cast<uint8_t**>(task + kTaskSprites);
    uint8_t* sp = sprites + kNewItem * kSpriteSize;
    g_item_quad = sp + kSpriteQuad;
    if (!g_native_label) {
      f32(sp, kSpriteAlpha) = 0.0f;
      f32(sp, kSpriteAlphaTarget) = 0.0f;
    }
    Label l;
    l.x = f32(sp, kSpriteX) - kItemW * 0.5f;
    l.y = f32(sp, kSpriteY);
    // Follow the game's fades: QUIT's alpha is what our item would have had.
    float a = f32(sprites + kQuit * kSpriteSize, kSpriteAlpha) / 255.0f;  // (QUIT is not moved)
    l.alpha = a < 0 ? 0 : (a > 1 ? 1 : a);
    l.selected = i32(task, kTaskCursor) == kNewItem;
    l.visible = l.alpha > 0.01f;
    g_label = l;
    g_label_ms = net::now_ms();
  }
  g_task = task;
}

}  // namespace

void install(const Config& cfg) {
  g_trace_textures = cfg.debug_actions;
  uintptr_t init = patch::find("main_menu_init", kInitSig);
  uintptr_t update = patch::find("main_menu_update", kUpdateSig);
  if (!update) {
    LOGW("main menu not recognised: the network window is only reachable with F2");
    return;
  }

  // Everything the native item needs, or nothing: fall back to a button.
  uintptr_t count = patch::find("main_menu_item_count", kCountSig);
  uintptr_t table = patch::find("main_menu_sprite_table", kSpriteTableSig);
  uintptr_t sound = patch::find("play_sound", kSoundSig);
  uintptr_t assign = patch::find("assign_pad_slot", kPadAssignSig);
  uintptr_t cursor = 0;
  for (uintptr_t a : patch::find_all(kCursorTableSig)) {
    uintptr_t t = patch::read<uint32_t>(a + 12);
    if (patch::in_image(t, sizeof(g_cursor)) && patch::read<float>(t) == -200.0f &&
        patch::read<float>(t + 4) == 184.0f && patch::read<float>(t + 56) == -124.0f)
      cursor = a;
  }
  uintptr_t old_table = table ? patch::read<uint32_t>(table + 7) : 0;
  bool ok = init && count && table && cursor && assign &&
            patch::in_image(old_table, sizeof(SpriteEntry) * kOldCount);
  if (ok) {
    std::memcpy(g_sprites, reinterpret_cast<void*>(old_table), sizeof(SpriteEntry) * kOldCount);
    std::memcpy(g_game_sizes, g_sprites, sizeof(g_game_sizes));
    // Any existing texture will do (a missing one crashes the game); the
    // sprite is kept invisible and the label is drawn on top.
    g_sprites[kNewItem] = {kItemX, kItemY, kItemW, kItemH, kItemLayer, g_sprites[3].texture};
    std::memcpy(g_cursor, reinterpret_cast<void*>(patch::read<uint32_t>(cursor + 12)),
                sizeof(float) * 2 * kOldCount);
    for (int i = kVs; i <= kLoad; ++i) {  // make room under STORY
      g_sprites[i].y -= kRow;
      g_cursor[i * 2 + 1] -= (float)kRow;
    }
    g_cursor[kNewItem * 2] = kCursorX;
    g_cursor[kNewItem * 2 + 1] = kCursorY;
    g_slots = reinterpret_cast<uint8_t*>(patch::read<uint32_t>(assign + 13));

    const uint8_t nine = kOldCount + 1;
    uint32_t sprites_addr = (uint32_t)reinterpret_cast<uintptr_t>(g_sprites);
    uint32_t cursor_addr = (uint32_t)reinterpret_cast<uintptr_t>(g_cursor);
    ok = patch::write(count + 5, &nine, 1) && patch::write(table + 7, &sprites_addr, 4) &&
         patch::write(cursor + 12, &cursor_addr, 4);
    g_native = ok;
    if (sound) g_sound = reinterpret_cast<Sound_t>(sound);
  }
  if (uintptr_t post = patch::find("menu_choice_post", kPostSig)) {
    uintptr_t target = post + kPostCall + 5 + patch::read<int32_t>(post + kPostCall + 1);
    if (patch::read<uint8_t>(post + kPostCall) == 0xE8 && patch::in_image(target, 1))
      g_post = reinterpret_cast<Post_t>(target);
  }
  if (!g_post) LOGW("main menu: the menu's word to its screen is not where expected; НАЧАТЬ ИГРУ brings the games together only");
  if (init) patch::hook("main_menu_init", init, (void*)&hkInit, (void**)&oInit);
  patch::hook("main_menu_update", update, (void*)&hkUpdate, (void**)&oUpdate);
  if (g_native) {
    if (uintptr_t draw = patch::find("draw_sprite", kDrawSpriteSig))
      patch::hook("draw_sprite", draw, (void*)&hkDrawSprite, (void**)&oDrawSprite);
    if (uintptr_t draw = patch::find("main_menu_draw", kMenuDrawSig))
      patch::hook("main_menu_draw", draw, (void*)&hkMenuDraw, (void**)&oMenuDraw);
    if (uintptr_t pad = patch::find("pad_slots_update", kPadUpdateSig)) {
      g_slots_early = g_slots;
      patch::hook("pad_slots_update", pad, (void*)&hkPadUpdate, (void**)&oPadUpdate);
    }
    native_menu::set_play_sound(g_sound);
    // The picture: the game's letters, read from TMNT.DAT off the game thread.
    std::string text = cfg.menu_label;
    std::thread([text] {
      bool ok = game_text::load(), own = ok && game_text::compose(text, g_bitmap, 4, game_text::Style::MainMenu);
      ok = own || (ok && game_text::compose(text, g_bitmap));
      g_letter_scale = own ? 1.0f : kOptionLetters;
      g_save_ready = ok && game_text::compose("ЗАПИСЬ", g_save_bitmap, 4, game_text::Style::MainMenu);
      if (g_save_ready.load()) LOGI("main menu: ЗАПИСЬ in the other items' letters (%dx%d)", g_save_bitmap.w, g_save_bitmap.h);
      // The items' letters as high as the story's.
      int story = game_text::white_rows("menu_moj_story");
      for (int i : {kVs, kChallenge}) {
        int rows = game_text::white_rows(i == kVs ? "menu_moj_vs" : "menu_moj_challenge");
        if (story > 0 && rows > story + 1) {
          g_item_scale[i] = float(story) / float(rows);
          LOGI("main menu: item %d drawn %.2f the size (letters %d rows high, the story's %d)", i,
               g_item_scale[i].load(), rows, story);
        }
      }
      g_picture = ok ? 1 : -1;
      if (ok)
        LOGI("main menu: item label \"%s\" made of the game's %s letters (%dx%d)", text.c_str(),
             own ? "main menu" : "option page", g_bitmap.w, g_bitmap.h);
      else
        LOGW("main menu: the game's letters are not available for \"%s\", label drawn by tmntnet", text.c_str());
    }).detach();
  }
  if (g_native)
    LOGI("main menu: native NETWORK item enabled");
  else
    LOGW("main menu: native item not possible on this exe, showing a button instead");
}

bool on_screen() { return oUpdate && net::now_ms() - g_seen_ms.load() < 300; }

uint32_t updates() { return g_updates; }

bool steady() {
  uint8_t* t = on_screen() ? g_task_now : nullptr;
  return t && i32(t, kTaskReady) && !i32(t, kTaskBusy) && menu_of(t) && !native_menu::active();
}

bool reset_cursor() {
  if (!steady()) return false;
  i32(g_task_now, kTaskCursor) = 0;
  i32(menu_of(g_task_now), kMenuCursor) = 0;
  return true;
}

uint8_t* pad_slots() { return g_slots; }

void play_sound(int id) {
  if (g_sound) g_sound(id);
}

uint8_t* menu_object() { return g_task_now && on_screen() ? menu_of(g_task_now) : nullptr; }

bool starts_story() { return g_post != nullptr; }

uint32_t take_page_buttons() {
  uint32_t b = g_page_pressed;
  g_page_pressed = 0;
  return b;
}

bool g_fake = false;
Label g_fake_label;
bool native() { return g_native || g_fake; }
void debug_fake(const Label& l) {
  g_fake = true;
  g_fake_label = l;
}

Label label() {
  if (g_fake) return g_fake_label;
  if (g_native_label) return Label{};  // the game draws the item itself
  // Taken in the menu's own update. The menu sometimes stalls for a moment
  // (loading) while frames still go out: keep the last label through that.
  if (net::now_ms() - g_label_ms > 500) return Label{};
  return g_label;
}

}  // namespace main_menu
