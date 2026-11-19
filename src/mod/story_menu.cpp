#include "mod/story_menu.h"

#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>

#include "common/log.h"
#include "mod/four_players.h"
#include "mod/game_text.h"
#include "mod/lockstep.h"
#include "mod/main_menu.h"
#include "mod/overlay.h"
#include "mod/patch.h"

namespace story_menu {
namespace {

// The submenu of СЮЖЕТ and ПОЕДИНОК (task 0x591684: size 0x88, init 0x48d1ad,
// term 0x48d0e8, update 0x48d2e8). Task: +0 cursor (where its sprite is),
// +0x14 menu object (+0xC cursor), +0x54 mode (0 story, 1 VS), +0x58 the items'
// values (story: 0 one player, 1 two, 2 Splinter, 3 Casey), +0x68 count, +0x7C
// item sprites (0x60 each, the quad at +0x24). The init makes the menu object
// 0x40f867(3, count, 0, 7) and the sprites 0x48d3f7(table, count, 0.0) from
// the story's table (0x5e5d18: {x, y, w, h, layer, texture}, rows 39 apart, the
// names written by the init); a decide posts {mode, value} to the main menu.
constexpr const char* kInitSig = "55 8B EC 53 56 8B 75 08 57 68 88 00 00 00 33 FF 57 56 E8";
constexpr size_t kMakeMenuCall = 0xDE, kSetCursorCall = 0xE9, kMakeSpritesCall = 0x124, kTableImm = 0x107;
constexpr const char* kTermSig = "56 8B 74 24 08 FF 76 68 FF 76 7C E8 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? FF 76 14 E8";
constexpr size_t kFreeSpritesCall = 0x0B, kFreeMenuCall = 0x19;
constexpr const char* kUpdateSig =
    "56 8B 74 24 08 83 7E 6C 00 0F 85 ?? ?? ?? ?? 83 7E 08 00 0F 84 ?? ?? ?? ?? 83 7E 74 00 0F 84";
// The update takes input only while +0x6C is 0, +8 (shown) and +0x74 (faded
// in) are set and its parent (+0x78) is not busy (0x48e09c, called at +0x26).
// The task lives with the main menu: it is updated while it is not open too.
constexpr size_t kTaskShown = 0x08, kTaskClosing = 0x6C, kTaskFadedIn = 0x74, kTaskParent = 0x78, kParentBusyCall = 0x26;
// Its cursor sprite goes to a table of places (0x48d055 -> 0x48d77f(task,
// 0x591620): x -170, rows 126, 87, 47, 7); five menus share that code, ours is
// told by the table's data.
constexpr const char* kCursorSig = "FF 74 24 04 E8 ?? ?? ?? ?? C7 04 24 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 59 59 C3";
constexpr size_t kCursorImm = 12;
constexpr float kGameCursor[8] = {-170, 126, -170, 87, -170, 47, -170, 7};

constexpr size_t kTaskCursor = 0x00, kTaskMenu = 0x14, kTaskMode = 0x54, kTaskValues = 0x58, kTaskCount = 0x68,
                 kTaskSprites = 0x7C;
constexpr size_t kMenuCursor = 0x0C, kSpriteSize = 0x60, kSpriteQuad = 0x24;
constexpr size_t kSlotSize = 0xAC, kSlotPressed = 0x10, kSlotRepeated = 0x18;
constexpr uint32_t kPadDecide = 0x10000, kPadUp = 0x1000, kPadDown = 0x4000;
constexpr int kRow = 39;
constexpr int kOurs = 2, kNew = 2;  // our items: rows 2 and 3, right under 2 ИГРОКА
constexpr int kMaxGame = 4;
// The labels are of the story menu's own lettering (game_text::Style::Story),
// at its size; else of the option labels', whose letters are smaller: 19 px of
// 480 for the menu's against 10 at the main menu's scale (21 / 24).
constexpr float kOptionLetters = 21.0f / 24.0f * 1.9f;
float g_letter_scale = 1.0f;
// Room on the left of our pictures for a slanted Т / Ч (game_text::compose);
// the sprite moves left by what is more than its usual 4 texels.
constexpr int kMargin = 16;
const char* const kLabels[kNew] = {"ТРИ ИГРОКА", "ЧЕТЫРЕ ИГРОКА"};

#pragma pack(push, 1)
struct SpriteEntry {
  int32_t x, y, w, h, layer;
  const char* texture;
};
#pragma pack(pop)
static_assert(sizeof(SpriteEntry) == 0x18, "game layout");

SpriteEntry g_table[kMaxGame + kNew];
float g_cursor[(kMaxGame + kNew) * 2];

using Init_t = int(__cdecl*)(uint8_t* task, void* params);
using Task_t = void(__cdecl*)(uint8_t* task);
using MakeMenu_t = uint8_t*(__cdecl*)(int type, int count, void* values, int flags);
using SetCursor_t = void(__cdecl*)(uint8_t* menu, int cursor);
using MakeSprites_t = uint8_t*(__cdecl*)(const SpriteEntry* table, int count, float alpha);
using FreeSprites_t = void(__cdecl*)(uint8_t* sprites, int count);
using FreeMenu_t = void(__cdecl*)(uint8_t* menu);
using Busy_t = int(__cdecl*)(uint8_t* parent);
Busy_t g_parent_busy = nullptr;
Init_t oInit = nullptr;
Task_t oUpdate = nullptr, oTerm = nullptr;
MakeMenu_t g_make_menu = nullptr;
SetCursor_t g_set_cursor = nullptr;
MakeSprites_t g_make_sprites = nullptr;
FreeSprites_t g_free_sprites = nullptr;
FreeMenu_t g_free_menu = nullptr;
const SpriteEntry* g_game_table = nullptr;

uint8_t* g_task = nullptr;  // the live story submenu with our items (game thread)

std::atomic<int> g_pictures{0};  // the labels: 0 being made, 1 ready, -1 not possible
game_text::Bitmap g_bitmaps[kNew];
IDirect3DTexture9* g_textures[kNew] = {};
IDirect3DDevice9* g_textures_dev = nullptr;

int32_t& i32(uint8_t* p, size_t off) { return *reinterpret_cast<int32_t*>(p + off); }
uint8_t*& ptr(uint8_t* p, size_t off) { return *reinterpret_cast<uint8_t**>(p + off); }

uintptr_t call_target(uintptr_t at) {
  if (patch::read<uint8_t>(at) != 0xE8) return 0;
  uintptr_t t = at + 5 + patch::read<int32_t>(at + 1);
  return patch::in_image(t, 1) ? t : 0;
}

IDirect3DTexture9* texture(int k) {
  IDirect3DDevice9* dev = overlay::device();
  if (!dev) return nullptr;
  if (g_textures_dev != dev) {  // (a new device)
    for (auto& t : g_textures)
      if (t) t->Release(), t = nullptr;
    g_textures_dev = dev;
  }
  if (g_textures[k]) return g_textures[k];
  const game_text::Bitmap& b = g_bitmaps[k];
  IDirect3DTexture9* t = nullptr;
  if (FAILED(dev->CreateTexture(b.w, b.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) return nullptr;
  D3DLOCKED_RECT lr{};
  if (FAILED(t->LockRect(0, &lr, nullptr, 0))) {
    t->Release();
    return nullptr;
  }
  for (int y = 0; y < b.h; ++y)
    std::memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, &b.bgra[size_t(y) * b.w], size_t(b.w) * 4);
  t->UnlockRect(0);
  g_textures[k] = t;
  return t;
}

// After the game's init: the story's menu again with our two items, under
// 2 ИГРОКА (the game's other items one row lower each).
int __cdecl hkInit(uint8_t* task, void* params) {
  int r = oInit(task, params);
  if (g_pictures.load() != 1 || i32(task, kTaskMode) != 0) return r;
  const int n = i32(task, kTaskCount);
  uint8_t* menu = ptr(task, kTaskMenu);
  uint8_t* sprites = ptr(task, kTaskSprites);
  if (n < 2 || n > kMaxGame || !menu || !sprites) return r;
  const int total = n + kNew;
  const SpriteEntry& two = g_game_table[1];  // 2 ИГРОКА
  for (int i = 0, g = 0; i < total; ++i) {
    SpriteEntry e;
    if (i >= kOurs && i < kOurs + kNew) {
      // Any texture of the menu will do for the sprite (a missing one crashes
      // the game): ours is put in when it is drawn. Left-aligned with the others.
      const game_text::Bitmap& b = g_bitmaps[i - kOurs];
      e = two;
      e.w = (int)std::lround(b.w * 0.5f * g_letter_scale);
      e.h = (int)std::lround(two.h * g_letter_scale);
      e.x = two.x - two.w / 2 + e.w / 2 - (int)std::lround((kMargin - 4) * 0.5f * g_letter_scale);
    } else {
      e = g_game_table[g++];
    }
    e.y = g_game_table[0].y - kRow * i;
    g_table[i] = e;
  }
  int cursor = i32(task, kTaskCursor);
  if (cursor >= kOurs) cursor += kNew;
  if (cursor < 0 || cursor >= total) cursor = 0;
  g_free_sprites(sprites, n);
  g_free_menu(menu);
  menu = g_make_menu(3, total, nullptr, 7);
  if (menu) g_set_cursor(menu, cursor);
  ptr(task, kTaskMenu) = menu;
  i32(task, kTaskCursor) = cursor;
  ptr(task, kTaskSprites) = g_make_sprites(g_table, total, 0.0f);
  i32(task, kTaskCount) = total;
  if (n == 2)  // (the values of rows 2 and 3, should a decide get past hkUpdate: 2 ИГРОКА)
    for (int i = kOurs; i < kOurs + kNew; ++i) i32(task, kTaskValues + i * 4) = 1;
  g_task = task;
  LOGI("story menu: %d items, ТРИ ИГРОКА and ЧЕТЫРЕ ИГРОКА under 2 ИГРОКА", total);
  return r;
}

// A decide: how many play, and the game's own item for it (ours start the
// story as 2 ИГРОКА; the game reads the value of the item under its cursor).
void update(uint8_t* task, uint8_t* menu);

void __cdecl hkUpdate(uint8_t* task) {
  uint8_t* menu = task == g_task ? ptr(task, kTaskMenu) : nullptr;
  if (!menu) return oUpdate(task);
  // In a game together the story is for as many as play it: the cursor stands
  // on that row and does not move (0.13.0: a decide on 1 ИГРОК began the story
  // for the host alone, the friend had no turn in the character select).
  const int together = lockstep::players();
  uint8_t* slots = main_menu::pad_slots();
  if (together < 2 || together > kOurs + kNew || !slots) return update(task, menu);
  if (i32(menu, kMenuCursor) != together - 1) {
    g_set_cursor(menu, together - 1);
    i32(task, kTaskCursor) = together - 1;
  }
  int32_t& pressed = i32(slots, kSlotPressed);
  int32_t& repeated = i32(slots, kSlotRepeated);
  const int32_t kept_pressed = pressed, kept_repeated = repeated;
  pressed &= ~(int32_t)(kPadUp | kPadDown);
  repeated &= ~(int32_t)(kPadUp | kPadDown);
  update(task, menu);
  pressed = kept_pressed, repeated = kept_repeated;
}

void update(uint8_t* task, uint8_t* menu) {
  const int cur = i32(menu, kMenuCursor);
  uint8_t* slots = main_menu::pad_slots();
  bool decide = false;
  // (in a game together only player 1 drives this menu: host_menus.cpp)
  int readers = lockstep::state() == lockstep::State::Running ? 1 : 2;
  for (int s = 0; slots && s < readers; ++s) decide |= (i32(slots, s * kSlotSize + kSlotPressed) & kPadDecide) != 0;
  bool open = !i32(task, kTaskClosing) && i32(task, kTaskShown) && i32(task, kTaskFadedIn) &&
              !g_parent_busy(ptr(task, kTaskParent));
  if (!decide || !open || cur < 0) return oUpdate(task);
  const int game = cur < kOurs ? cur : cur < kOurs + kNew ? 1 : cur - kNew;
  four::set_count(cur < kOurs + kNew ? cur + 1 : 1);  // (Splinter and Casey: one player)
  if (game == cur) return oUpdate(task);
  i32(menu, kMenuCursor) = game;
  oUpdate(task);
  if (g_task == task && ptr(task, kTaskMenu) == menu) i32(menu, kMenuCursor) = cur;
}

void __cdecl hkTerm(uint8_t* task) {
  if (task == g_task) g_task = nullptr;
  oTerm(task);
}

}  // namespace

uint8_t* menu_object() { return g_task ? ptr(g_task, kTaskMenu) : nullptr; }

void stand_by() {
  if (g_task) i32(g_task, kTaskShown) = 0;  // (as after its own choice: 0x48d392)
}

void install(const Config&) {
  uintptr_t init = patch::find("story_menu_init", kInitSig);
  uintptr_t term = patch::find("story_menu_term", kTermSig);
  uintptr_t update = patch::find("story_menu_update", kUpdateSig);
  uintptr_t site = 0;
  for (uintptr_t a : patch::find_all(kCursorSig)) {
    uintptr_t t = patch::read<uint32_t>(a + kCursorImm);
    if (patch::in_image(t, sizeof(kGameCursor)) && std::memcmp(reinterpret_cast<const void*>(t), kGameCursor, sizeof(kGameCursor)) == 0)
      site = a;
  }
  if (!init || !term || !update || !site) {
    LOGW("story menu: not laid out as expected; how many play is set by [mod] players");
    return;
  }
  g_make_menu = reinterpret_cast<MakeMenu_t>(call_target(init + kMakeMenuCall));
  g_set_cursor = reinterpret_cast<SetCursor_t>(call_target(init + kSetCursorCall));
  g_make_sprites = reinterpret_cast<MakeSprites_t>(call_target(init + kMakeSpritesCall));
  g_free_sprites = reinterpret_cast<FreeSprites_t>(call_target(term + kFreeSpritesCall));
  g_free_menu = reinterpret_cast<FreeMenu_t>(call_target(term + kFreeMenuCall));
  g_parent_busy = reinterpret_cast<Busy_t>(call_target(update + kParentBusyCall));
  uintptr_t table = patch::read<uint8_t>(init + kTableImm - 3) == 0xC7 ? patch::read<uint32_t>(init + kTableImm) : 0;
  if (!g_make_menu || !g_set_cursor || !g_make_sprites || !g_free_sprites || !g_free_menu || !g_parent_busy ||
      !patch::in_image(table, sizeof(SpriteEntry) * kMaxGame)) {
    LOGW("story menu: its parts are not where expected; how many play is set by [mod] players");
    return;
  }
  g_game_table = reinterpret_cast<const SpriteEntry*>(table);
  // Its cursor: the game's four places and two more rows.
  std::memcpy(g_cursor, kGameCursor, sizeof(kGameCursor));
  for (int i = kMaxGame; i < kMaxGame + kNew; ++i) {
    g_cursor[i * 2] = kGameCursor[0];
    g_cursor[i * 2 + 1] = kGameCursor[7] - kRow * (i - 3);
  }
  uint32_t cursor_addr = (uint32_t)reinterpret_cast<uintptr_t>(g_cursor);
  if (!patch::write(site + kCursorImm, &cursor_addr, 4) ||
      !patch::hook("story_menu_term", term, (void*)&hkTerm, (void**)&oTerm) ||
      !patch::hook("story_menu_update", update, (void*)&hkUpdate, (void**)&oUpdate) ||
      !patch::hook("story_menu_init", init, (void*)&hkInit, (void**)&oInit)) {
    LOGW("story menu: hooks not in place");
    return;
  }
  // The labels: the game's letters, read from TMNT.DAT off the game thread.
  std::thread([] {
    bool ok = game_text::load(), own = ok;
    for (int k = 0; own && k < kNew; ++k) own = game_text::compose(kLabels[k], g_bitmaps[k], kMargin, game_text::Style::Story);
    for (int k = 0; ok && !own && k < kNew; ++k) ok = game_text::compose(kLabels[k], g_bitmaps[k], kMargin);
    g_letter_scale = own ? 1.0f : kOptionLetters;
    g_pictures = ok ? 1 : -1;
    if (ok)
      LOGI("story menu: \"%s\" and \"%s\" made of the game's %s letters", kLabels[0], kLabels[1],
           own ? "story menu" : "option page");
    else
      LOGW("story menu: the game's letters are not available; the menu stays the game's");
  }).detach();
}

IDirect3DTexture9* texture_for(const void* quad) {
  if (!g_task || !quad || g_pictures.load() != 1) return nullptr;
  uint8_t* sprites = ptr(g_task, kTaskSprites);
  for (int k = 0; sprites && k < kNew; ++k)
    if (quad == sprites + (kOurs + k) * kSpriteSize + kSpriteQuad) return texture(k);
  return nullptr;
}

}  // namespace story_menu
