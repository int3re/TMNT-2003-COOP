#include "mod/option_pages.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "common/log.h"
#include "mod/main_menu.h"
#include "mod/mod_pages.h"
#include "mod/native_menu.h"
#include "mod/patch.h"

namespace option_pages {
namespace {

// --- the game's option pages (TMNT.EXE bc49a82c..., re/notes.md §16) -------------
// A page's data starts with its cursor object: +8 active, +0x14 the menu
// object (+0 result: 2 moved, 3 decided, 4 cancelled; +8 count; +0xC cursor;
// +0x10 items of 0x18: state, value, left, right, up, down). The page reads
// left/right from the pad slots itself, and only for its own rows; for a
// cursor beyond them it only draws the cursor at positions[cursor]. Its hub
// (ПАРАМЕТРЫ) has +0x10 busy and +0x78 the "sure" dialog of ПО УМОЛЧАНИЮ.
constexpr size_t kActive = 0x08, kMenu = 0x14, kTaskData = 0x34;
constexpr size_t kMenuResult = 0x00, kMenuCount = 0x08, kMenuCursor = 0x0C, kMenuItems = 0x10, kItemSize = 0x18,
                 kItemUp = 0x10, kItemDown = 0x14;
constexpr size_t kHubBusy = 0x10, kHubDialog = 0x78;
constexpr size_t kSpriteSize = 0x60, kSpriteAlphaTarget = 0x04;  // the pages' sprites (0x48d3f7)
constexpr size_t kSlotHeld = 0x0C, kSlotPressed = 0x10, kSlotSize = 0xAC;
constexpr int kMoved = 2, kDecided = 3, kCancelled = 4;
constexpr uint32_t kLeft = 0x8000, kRight = 0x2000;
constexpr float kCursorX = -181.0f;

#pragma pack(push, 1)
struct SpriteEntry {  // the game's layout table entry
  int32_t x, y, w, h, layer;
  const char* texture;
};
#pragma pack(pop)
static_assert(sizeof(SpriteEntry) == 0x18, "game layout");

struct Pos {
  float x, y;
};

using Init_t = int(__cdecl*)(uint8_t* data, void* hub);
using Task_t = int(__cdecl*)(uint8_t* data);
using Answer_t = int(__cdecl*)(void* dialog);
Answer_t g_answer = nullptr;  // a "sure" dialog's answer: 1 yes, 2 no, 0 not yet (0x4127db)

// One of the game's pages with tmntnet's rows in it.
struct Page {
  const char* name;
  int id;               // native_menu::embed's
  size_t state, hub, labels;  // data: state (0 = normal), the hub's task, the labels' sprites
  int native;           // the game's menu items
  std::vector<int> order;     // menu items top to bottom (ours are native..)
  std::vector<native_menu::Row> rows;
  std::vector<float> ys;      // our rows' y
  float value_x = 126.0f;     // the page's column of values
  float letters_x = -184.5f;  // where its rows' letters start
  bool revert = true;         // cancel puts our values back (pages with OK)
  void (*after)(struct Page&, uint8_t* data) = nullptr;  // more after its update
  std::vector<int> kept;      // the values when the page opened (cancel puts them back)
  float dim = 255.0f;         // our rows' share of the page's alpha
  uint32_t last_ms = 0;
  int last_state = 0, list_index = -1;  // (ПАРАМЕТРЫ ГРАФИКИ: its list of resolutions)
  bool was_active = false;
  bool wait_release = false;  // left/right taken: the next only after they are let go
  bool ready = false;
  Init_t oInit = nullptr;
  Task_t oUpdate = nullptr, oDraw = nullptr;
};

int32_t& i32(uint8_t* p, size_t off) { return *reinterpret_cast<int32_t*>(p + off); }
float& f32(uint8_t* p, size_t off) { return *reinterpret_cast<float*>(p + off); }
uint8_t* ptr(uint8_t* p, size_t off) { return *reinterpret_cast<uint8_t**>(p + off); }

int value_of(const native_menu::Row& r) { return r.raw ? r.raw() : r.get ? r.get() : 0; }

void relink(uint8_t* menu, const std::vector<int>& order) {
  uint8_t* items = ptr(menu, kMenuItems);
  size_t n = order.size();
  for (size_t k = 0; k < n; ++k) {
    uint8_t* it = items + order[k] * kItemSize;
    i32(it, kItemUp) = order[(k + n - 1) % n];
    i32(it, kItemDown) = order[(k + 1) % n];
  }
}

// Both pad slots' bits (pressed this frame, or held).
uint32_t pad_bits(size_t field) {
  uint8_t* slots = main_menu::pad_slots();
  if (!slots) return 0;
  return *reinterpret_cast<uint32_t*>(slots + field) | *reinterpret_cast<uint32_t*>(slots + kSlotSize + field);
}

int page_init(Page& p, uint8_t* d, void* hub) {
  int r = p.oInit(d, hub);
  uint8_t* menu = ptr(d, kMenu);
  p.ready = menu && i32(menu, kMenuCount) == p.native + (int)p.rows.size();
  if (p.ready) {
    relink(menu, p.order);
    native_menu::embed(p.id, p.rows, p.ys, p.value_x, p.letters_x);
  } else {
    LOGW("option pages: %s has %d items, not %d: no rows of tmntnet there", p.name, menu ? i32(menu, kMenuCount) : -1,
         p.native + (int)p.rows.size());
  }
  return r;
}

// Around the page's update: left/right on our rows (the game's code ignores
// them), cancel puts our values back, ПО УМОЛЧАНИЮ resets them too.
int page_update(Page& p, uint8_t* d) {
  if (!p.ready) return p.oUpdate(d);
  uint8_t* hub_task = ptr(d, p.hub);
  uint8_t* hub = hub_task ? ptr(hub_task, kTaskData) : nullptr;
  bool active = i32(d, kActive) != 0;
  int state = p.state ? i32(d, p.state) : 0;  // (0: a page without a dialog)
  bool ran = state == 0 && active && hub && i32(hub, kHubBusy) == 0;  // the page takes buttons this frame
  int answer = state == 1 && hub && ptr(hub, kHubDialog) ? g_answer(ptr(hub, kHubDialog)) : 0;
  if (active && !p.was_active) {
    p.kept.clear();
    for (const native_menu::Row& r : p.rows) p.kept.push_back(value_of(r));
  }
  p.was_active = active;

  int result = p.oUpdate(d);

  uint8_t* menu = ptr(d, kMenu);
  int res = i32(menu, kMenuResult), ours = i32(menu, kMenuCursor) - p.native;
  // One step a press, like the game's own rows (no repeat). A layout change
  // also moves which player a held arrow belongs to, so a held key would be
  // "pressed" again: wait until left/right are let go.
  if (p.wait_release && !(pad_bits(kSlotHeld) & (kLeft | kRight))) p.wait_release = false;
  if (ran && ours >= 0 && ours < (int)p.rows.size()) {
    if (res == kDecided) {
      i32(menu, kMenuResult) = 0;  // (like the game's value rows: decide does nothing)
    } else if (res != kMoved && res != kCancelled && !p.wait_release) {
      uint32_t pressed = pad_bits(kSlotPressed);
      if (pressed & (kLeft | kRight)) {
        native_menu::embed_input(p.id, ours, (pressed & kLeft) ? kLeft : kRight);
        p.wait_release = true;
      }
    }
  }
  if (ran && res == kCancelled && p.revert && p.kept.size() == p.rows.size())
    for (size_t i = 0; i < p.rows.size(); ++i)
      if (p.rows[i].set && value_of(p.rows[i]) != p.kept[i]) p.rows[i].set(p.kept[i]);
  if (answer == 1)  // ПО УМОЛЧАНИЮ: yes
    for (native_menu::Row& r : p.rows)
      if (r.set && r.default_value != native_menu::kNoDefault && value_of(r) != r.default_value) r.set(r.default_value);
  native_menu::embed_select(p.id, active && ours >= 0 ? ours : -1);
  if (p.after) p.after(p, d);
  return result;
}

// After the page's draw: our rows, with the page's fade (its first label's alpha).
int page_draw(Page& p, uint8_t* d) {
  int r = p.oDraw(d);
  if (!p.ready) return r;
  uint8_t* labels = ptr(d, p.labels);
  float a = labels ? *reinterpret_cast<float*>(labels) : 0.0f;
  native_menu::embed_draw(p.id, (int)(std::clamp(a, 0.0f, 255.0f) * p.dim / 255.0f));
  return r;
}

// ПАРАМЕТРЫ ГРАФИКИ: the resolution's name stands in its row, in the column
// of values, and the game's frame with the list of resolutions shows only
// while it is open (decide on РАЗРЕШЕНИЕ); then ТЕНИ and our rows give way
// to it. The name is drawn by the game (0x4a7296, its call redirected here).
constexpr size_t kDispModes = 0x150, kDispMode = 0x154, kDispModeCount = 0x158, kDispState = 0x15C,
                 kDispLabels = 0x168, kDispValues = 0x16C;
constexpr size_t kModeSize = 0x60;  // the adapter's modes: +0 width, +4 height
constexpr int kDispShadows = 1, kDispOk = 2, kDispFrame = 4;  // in its labels: title, ТЕНИ, OK, РАЗРЕШЕНИЕ, frame
constexpr float kResSize = 19.0f, kResX = 126.0f, kResY = 103.0f;
using FontDraw_t = void(__cdecl*)(const char* text, float size, float x, float y);
FontDraw_t g_font_draw = nullptr;
bool g_disp_list = false;

void __cdecl hkResolutionName(const char* text, float size, float x, float y) {
  if (g_disp_list || size <= 0) {  // in the frame, as the game has it
    g_font_draw(text, size, x, y);
    return;
  }
  float w = -2.0f * x * kResSize / size;  // (the game passes x = -width / 2)
  g_font_draw(text, kResSize, kResX - w * 0.5f, kResY);
}

void disp_after(Page& p, uint8_t* d) {
  bool active = i32(d, kActive) != 0, list = i32(d, kDispState) == 1;
  g_disp_list = list;
  if (uint8_t* labels = ptr(d, kDispLabels)) {
    f32(labels + kDispFrame * kSpriteSize, kSpriteAlphaTarget) = active && list ? 255.0f : 0.0f;
    f32(labels + kDispShadows * kSpriteSize, kSpriteAlphaTarget) = active && !list ? 255.0f : 0.0f;
  }
  uint32_t now = (uint32_t)GetTickCount();
  float frames = p.last_ms ? std::min(6.0f, (now - p.last_ms) * 0.06f) : 1.0f;
  p.last_ms = now;
  float target = list ? 0.0f : 255.0f, step = 12.75f * frames;
  p.dim = p.dim < target ? std::min(target, p.dim + step) : std::max(target, p.dim - step);
  // ТЕНИ's value: the game brings the current one up every frame, so under the
  // frame it is held down with our rows.
  if (uint8_t* values = ptr(d, kDispValues))
    for (int i = 0; i < 2; ++i) {
      float& a = f32(values + i * kSpriteSize, 0);
      a = std::min(a, p.dim);
    }
  // A resolution taken from the list (decide, not cancel) is kept for the next
  // start too ([video] resolution).
  int state = i32(d, kDispState), mode = i32(d, kDispMode);
  if (p.last_state == 0 && state == 1) p.list_index = mode;
  if (p.last_state == 1 && state == 0 && mode != p.list_index && mode >= 0 && mode < i32(d, kDispModeCount))
    if (uint8_t* modes = ptr(d, kDispModes))
      mod_pages::resolution_chosen(i32(modes + mode * kModeSize, 0), i32(modes + mode * kModeSize, 4));
  p.last_state = state;
}

Page g_other, g_sound, g_disp, g_ctrl;
int __cdecl hkOtherInit(uint8_t* d, void* hub) { return page_init(g_other, d, hub); }
int __cdecl hkOtherUpdate(uint8_t* d) { return page_update(g_other, d); }
int __cdecl hkOtherDraw(uint8_t* d) { return page_draw(g_other, d); }
int __cdecl hkSoundInit(uint8_t* d, void* hub) { return page_init(g_sound, d, hub); }
int __cdecl hkSoundUpdate(uint8_t* d) { return page_update(g_sound, d); }
int __cdecl hkSoundDraw(uint8_t* d) { return page_draw(g_sound, d); }
int __cdecl hkDispInit(uint8_t* d, void* hub) { return page_init(g_disp, d, hub); }
int __cdecl hkDispUpdate(uint8_t* d) { return page_update(g_disp, d); }
int __cdecl hkDispDraw(uint8_t* d) { return page_draw(g_disp, d); }
int __cdecl hkCtrlInit(uint8_t* d, void* hub) { return page_init(g_ctrl, d, hub); }
int __cdecl hkCtrlUpdate(uint8_t* d) { return page_update(g_ctrl, d); }
int __cdecl hkCtrlDraw(uint8_t* d) { return page_draw(g_ctrl, d); }

// The tables the pages are patched to use (they live as long as the game).
SpriteEntry g_other_labels[5], g_sound_labels[6], g_disp_labels[5], g_disp_values[2];
Pos g_other_pos[9], g_sound_pos[8], g_disp_pos[11], g_disp_shadow_arrows, g_ctrl_pos[4];

struct Site {
  const char* name;
  const char* sig;
  int offset;
};

uintptr_t site(const Site& s) {
  uintptr_t at = patch::find(s.name, s.sig);
  return at ? at + s.offset : 0;
}

// The page's patches: its menu count, its cursor positions, its labels (one
// of them moved down). All or nothing.
bool patch_page(const char* name, const Site& count, uint8_t native, uint8_t total, const Site& positions,
                Pos* pos, size_t npos, const Site& labels, SpriteEntry* table, size_t nlabels, int moved,
                int moved_y) {
  uintptr_t c = site(count), p = site(positions), l = site(labels);
  if (!c || !p || !l || patch::read<uint8_t>(c) != native) return false;
  uintptr_t old_labels = patch::read<uint32_t>(l);
  if (!patch::in_image(old_labels, sizeof(SpriteEntry) * nlabels)) return false;
  std::memcpy(table, reinterpret_cast<void*>(old_labels), sizeof(SpriteEntry) * nlabels);
  table[moved].y = moved_y;
  uint32_t pos_addr = (uint32_t)reinterpret_cast<uintptr_t>(pos), labels_addr = (uint32_t)reinterpret_cast<uintptr_t>(table);
  bool ok = patch::write(c, &total, 1) && patch::write(p, &pos_addr, 4) && patch::write(l, &labels_addr, 4);
  if (!ok) LOGE("option pages: patching %s failed half way", name);
  (void)npos;
  return ok;
}

}  // namespace

void install(const Config& cfg) {
  (void)cfg;
  if (!main_menu::pad_slots()) {
    LOGW("option pages: the pad slots are not known, no rows of tmntnet in the game's options");
    return;
  }
  uintptr_t answer = patch::find("dialog_answer", "8B 44 24 04 8B 40 34 8B 40 08 C3 68 55 01 00 00");
  if (!answer) return;
  g_answer = reinterpret_cast<Answer_t>(answer);

  // ПРОЧИЕ ПАРАМЕТРЫ: УРОВЕНЬ СЛОЖНОСТИ, АВТОЗАПИСЬ, СИЛА УДАРА, then ours
  // (items 5..8 at 7, -28, -62, -96), ПО УМОЛЧАНИЮ moved from 6 to -130, OK
  // from -129 to -164.
  g_other = Page{"ПРОЧИЕ ПАРАМЕТРЫ", 1, 0x150, 0x164, 0x168, 5, {0, 1, 2, 5, 6, 7, 8, 3, 4}, mod_pages::other_rows(),
                 {7, -28, -62, -96}};
  {
    const Pos pos[9] = {{kCursorX, 112}, {kCursorX, 76}, {kCursorX, 42}, {kCursorX, -128}, {kCursorX, -164},
                        {kCursorX, 9},   {kCursorX, -26}, {kCursorX, -60}, {kCursorX, -94}};
    static_assert(sizeof(pos) == sizeof(g_other_pos), "one place a menu item");
    std::memcpy(g_other_pos, pos, sizeof(pos));
    uintptr_t init = patch::find("other_init", "56 8B 74 24 08 57 68 7C 01 00 00 33 FF");
    uintptr_t update = patch::find("other_update", "83 EC 70 53 55 56 8B B4 24 80 00 00 00");
    uintptr_t draw = patch::find("other_draw", "56 8B 74 24 08 6A 01 FF B6 78 01 00 00");
    if (init && update && draw &&
        patch_page("ПРОЧИЕ ПАРАМЕТРЫ", {"other_menu_count", "68 07 60 00 00 57 6A 05", 7}, 5, 9,
                   {"other_positions", "C7 04 24 10 1B 59 00 56", 3}, g_other_pos, 9,
                   {"other_labels", "83 C4 20 D9 1C 24 6A 05 68 ?? ?? ?? ??", 9}, g_other_labels, 5, 3, -130)) {
      g_other_labels[4].y = -164;  // (OK, under ПО УМОЛЧАНИЮ)
      patch::hook("other_init", init, (void*)&hkOtherInit, (void**)&g_other.oInit);
      patch::hook("other_update", update, (void*)&hkOtherUpdate, (void**)&g_other.oUpdate);
      patch::hook("other_draw", draw, (void*)&hkOtherDraw, (void**)&g_other.oDraw);
      LOGI("option pages: ПРОЧИЕ ПАРАМЕТРЫ with %zu rows of tmntnet", g_other.rows.size());
    } else {
      LOGW("option pages: ПРОЧИЕ ПАРАМЕТРЫ not recognised");
    }
  }

  // ПАРАМЕТРЫ ЗВУКА: МУЗЫКА, ЗВУК, ГОЛОС, our РОЛИКИ slider (item 6 at 5),
  // РЕЖИМ АУДИО, our ЗАСТАВКА (item 7 at -62), ПО УМОЛЧАНИЮ moved from -60 to
  // -95, OK -130.
  g_sound = Page{"ПАРАМЕТРЫ ЗВУКА", 2, 0xA8, 0xCC, 0xD0, 6, {0, 1, 2, 6, 3, 7, 4, 5}, mod_pages::sound_rows(), {5, -62},
                 19.0f};  // (its values stand at 19, like СТЕРЕО)
  {
    const Pos pos[8] = {{kCursorX, 112}, {kCursorX, 76}, {kCursorX, 42}, {kCursorX, -26}, {kCursorX, -93},
                        {kCursorX, -129}, {kCursorX, 7}, {kCursorX, -60}};
    std::memcpy(g_sound_pos, pos, sizeof(pos));
    uintptr_t init = patch::find("sound_init", "56 8B 74 24 08 57 68 E4 00 00 00 33 FF");
    uintptr_t update = patch::find("sound_update", "53 8B 5C 24 08 55 56 8D AB BC 00 00 00");
    uintptr_t draw = patch::find("sound_draw", "56 8B 74 24 08 6A 01 FF B6 E0 00 00 00");
    if (init && update && draw &&
        patch_page("ПАРАМЕТРЫ ЗВУКА", {"sound_menu_count", "68 07 60 00 00 57 6A 06", 7}, 6, 8,
                   {"sound_positions", "8B F0 C7 04 24 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 68 ?? ?? ?? ??", 5}, g_sound_pos,
                   8, {"sound_labels", "83 C4 24 D9 1C 24 6A 06 68 ?? ?? ?? ??", 9}, g_sound_labels, 6, 4, -95)) {
      patch::hook("sound_init", init, (void*)&hkSoundInit, (void**)&g_sound.oInit);
      patch::hook("sound_update", update, (void*)&hkSoundUpdate, (void**)&g_sound.oUpdate);
      patch::hook("sound_draw", draw, (void*)&hkSoundDraw, (void**)&g_sound.oDraw);
      LOGI("option pages: ПАРАМЕТРЫ ЗВУКА with %zu rows of tmntnet", g_sound.rows.size());
    } else {
      LOGW("option pages: ПАРАМЕТРЫ ЗВУКА not recognised");
    }
  }

  // ПАРАМЕТРЫ ГРАФИКИ: РАЗРЕШЕНИЕ (its name in the row), ТЕНИ moved up from
  // -59 to 80 with its value and arrows in the column at 126, then ours (items
  // 3..10, 30 apart from 50 down to -160), OK moved down from -129 to -188.
  g_disp = Page{"ПАРАМЕТРЫ ГРАФИКИ", 3, kDispState, 0x164, kDispLabels, 3, {0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 2},
                mod_pages::graphics_rows(), {50, 20, -10, -40, -70, -100, -130, -160}, 126.0f, -184.5f, true,
                &disp_after};
  {
    const Pos pos[11] = {{kCursorX, 112}, {kCursorX, 82},  {kCursorX, -188}, {kCursorX, 52},
                         {kCursorX, 22},  {kCursorX, -8},  {kCursorX, -38},  {kCursorX, -68},
                         {kCursorX, -98}, {kCursorX, -128}, {kCursorX, -158}};
    static_assert(sizeof(pos) == sizeof(g_disp_pos), "one place a menu item");
    std::memcpy(g_disp_pos, pos, sizeof(pos));
    g_disp_shadow_arrows = {126, 80};
    uintptr_t init = patch::find("disp_init", "56 8B 74 24 08 57 68 70 01 00 00 33 FF");
    uintptr_t update = patch::find("disp_update", "53 55 56 8B 74 24 10 8B 86 5C 01 00 00");
    uintptr_t draw = patch::find("disp_draw", "55 8B EC 51 56 8B 75 08 57 8D BE 68 01 00 00");
    uintptr_t values = site({"disp_shadow_values", "6A 02 68 ?? ?? ?? ?? 89 86 68 01 00 00", 3});
    uintptr_t arrows = site({"disp_shadow_arrows", "E8 ?? ?? ?? ?? 68 C4 18 59 00", 6});
    uintptr_t name_call = site({"disp_name_call", "D9 1C 24 50 E8 ?? ?? ?? ?? 83 C4 10 5F", 4});
    uintptr_t old_values = values ? patch::read<uint32_t>(values) : 0;
    bool ok = init && update && draw && arrows && name_call && patch::read<uint8_t>(name_call) == 0xE8 &&
              patch::in_image(old_values, sizeof(SpriteEntry) * 2);
    if (ok) {
      g_font_draw = reinterpret_cast<FontDraw_t>(name_call + 5 + patch::read<int32_t>(name_call + 1));
      std::memcpy(g_disp_values, reinterpret_cast<void*>(old_values), sizeof(g_disp_values));
      for (SpriteEntry& e : g_disp_values) e.x = 126, e.y = 80;
      ok = patch_page("ПАРАМЕТРЫ ГРАФИКИ", {"disp_menu_count", "6A 03 6A 03 89 86 58 01 00 00", 1}, 3, 11,
                      {"disp_positions", "C7 04 24 9C 18 59 00 56", 3}, g_disp_pos, 11,
                      {"disp_labels", "6A 05 68 ?? ?? ?? ?? 89 46 14", 3}, g_disp_labels, 5, kDispShadows, 80);
      if (ok) g_disp_labels[kDispOk].y = -188;
    }
    if (ok) {
      uint32_t values_addr = (uint32_t)reinterpret_cast<uintptr_t>(g_disp_values);
      uint32_t arrows_addr = (uint32_t)reinterpret_cast<uintptr_t>(&g_disp_shadow_arrows);
      int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(&hkResolutionName) - (name_call + 5));
      ok = patch::write(values, &values_addr, 4) && patch::write(arrows, &arrows_addr, 4) &&
           patch::write(name_call + 1, &rel, 4);
    }
    if (ok) {
      patch::hook("disp_init", init, (void*)&hkDispInit, (void**)&g_disp.oInit);
      patch::hook("disp_update", update, (void*)&hkDispUpdate, (void**)&g_disp.oUpdate);
      patch::hook("disp_draw", draw, (void*)&hkDispDraw, (void**)&g_disp.oDraw);
      LOGI("option pages: ПАРАМЕТРЫ ГРАФИКИ with %zu rows of tmntnet", g_disp.rows.size());
    } else {
      LOGW("option pages: ПАРАМЕТРЫ ГРАФИКИ not recognised");
    }
  }

  // ПАРАМЕТРЫ УПРАВЛЕНИЯ (the small page before the key table): КЛАВИАТУРА,
  // ГЕЙМПАД, our РАСКЛАДКА (item 3 at -8, indented like them: cursor at -156),
  // УЙТИ. Decide on item 3 does nothing in the game's code. No OK there, so a
  // layout taken stays.
  g_ctrl = Page{"ПАРАМЕТРЫ УПРАВЛЕНИЯ", 4, 0, 0x54, 0x64, 3, {0, 1, 3, 2}, mod_pages::controls_rows(), {-8},
                126.0f, -159.5f, false};
  {
    const Pos pos[4] = {{-156, 59}, {-156, 26}, {-156, -42}, {-156, -6}};
    std::memcpy(g_ctrl_pos, pos, sizeof(pos));
    uintptr_t init = patch::find("controls_init", "56 8B 74 24 08 6A 68 6A 00 56 E8");
    uintptr_t update = patch::find("controls_update", "56 8B 74 24 08 57 33 FF 39 7E 08 0F 84 31 01 00 00 FF 76 54 E8");
    uintptr_t draw = patch::find("controls_draw", "8B 44 24 04 6A 03 FF 70 64 E8 ?? ?? ?? ?? 59 59 C3 55 8B EC 83 EC 10");
    uintptr_t count = site({"controls_menu_count", "6A 00 F7 D1 23 C8 6A 03 6A 03 89 46 60", 7});
    uintptr_t positions = site({"controls_positions", "C7 04 24 10 1A 59 00 50 E8", 3});
    const uint8_t four = 4;
    uint32_t pos_addr = (uint32_t)reinterpret_cast<uintptr_t>(g_ctrl_pos);
    if (init && update && draw && count && positions && patch::read<uint8_t>(count) == 3 &&
        patch::write(count, &four, 1) && patch::write(positions, &pos_addr, 4)) {
      patch::hook("controls_init", init, (void*)&hkCtrlInit, (void**)&g_ctrl.oInit);
      patch::hook("controls_update", update, (void*)&hkCtrlUpdate, (void**)&g_ctrl.oUpdate);
      patch::hook("controls_draw", draw, (void*)&hkCtrlDraw, (void**)&g_ctrl.oDraw);
      LOGI("option pages: ПАРАМЕТРЫ УПРАВЛЕНИЯ with %zu rows of tmntnet", g_ctrl.rows.size());
    } else {
      LOGW("option pages: ПАРАМЕТРЫ УПРАВЛЕНИЯ not recognised");
    }
  }
}

}  // namespace option_pages
