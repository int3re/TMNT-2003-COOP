#include "mod/keyconf.h"

#include <windows.h>

#include <cstring>
#include <string>

#include "common/log.h"
#include "mod/binds.h"
#include "mod/dinput_hook.h"
#include "mod/patch.h"
#include "mod/widescreen.h"

namespace keyconf {
namespace {

// --- the key-config scene (TMNT.EXE bc49a82c..., re/notes.md §17) ---------------
// Task data ([0x69f584], valid from its init to its term): +0 mode (0 the
// keyboard table, 1 the gamepad one), +0x14 cursor (0..11 player 1's rows,
// 12..23 player 2's, 24 Сбросить, 25 Базовые Установки, 26 OK), +0x20 the
// "sure" dialog, +0x24 the working copies of the binds (0x40 a player: 16
// slots of DIK codes). Aux ([0x69f580]): +0 1 moving / 2 in a cell / 4 the OK
// dialog / 5 committed; +4 (in a cell) 1 waiting for a key, 3 the dialog of
// Сбросить / Базовые Установки.
constexpr size_t kMode = 0x00, kCursor = 0x14, kDialog = 0x20, kWork = 0x24;
constexpr size_t kAuxMain = 0x00, kAuxCell = 0x04;
constexpr int kInCell = 2, kOkDialog = 4, kCellDialog = 3;
constexpr int kCursorReset = 24, kCursorDefaults = 25;
// The name boxes: x per player column, 76x12 at the row's y (512x480, y up).
constexpr float kCellX[2] = {175.0f, 387.0f}, kTextH = 12.0f, kSuffixGap = 2.0f, kSuffixW = 40.0f;

constexpr const char* kUpdateSig =
    "A1 ?? ?? ?? ?? 8B 48 04 83 F9 07 57 8B 3D ?? ?? ?? ?? 0F 87 ?? ?? ?? ?? 56 33 F6 46 FF 24 8D";  // +1 task, +14 aux
constexpr const char* kDrawNamesSig =
    "55 8B EC 83 EC 30 8B 0D ?? ?? ?? ?? A1 ?? ?? ?? ?? 53 33 DB 39 19 89 45 FC 56 C7 45 D8 00 80 8C 43 C7 45 DC 00 "
    "80 9B 43 C7 45 E0 00 00 98 42 C7 45 E4";
constexpr const char* kInitSig =
    "55 8B EC 83 EC 24 56 57 33 F6 56 E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 6A 2C FF 90 34 01 00 00 6A 2C 56 50 A3";
constexpr const char* kTermSig =
    "A1 ?? ?? ?? ?? 83 38 00 75 08 6A 00 E8 ?? ?? ?? ?? 59 FF 35 ?? ?? ?? ?? A1 ?? ?? ?? ?? FF 90 38 01 00 00";
constexpr const char* kTextWidthSig = "D9 44 24 08 51 D9 1C 24 FF 74 24 08 FF 35 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 0C C3";
constexpr const char* kAnswerSig = "8B 44 24 04 8B 40 34 8B 40 08 C3 68 55 01 00 00";
constexpr const char* kRowsSig = "6A 0C 5E 99 F7 FE 51 FF 34 D5 ?? ?? ?? ?? 53 E8";       // +10 rows
constexpr const char* kNamesSig = "FF 34 8D ?? ?? ?? ?? EB 05 68 ?? ?? ?? ?? 50 E8";        // +3 names, +10 "none"
// A waiting cell takes the first key with a new press whose byte here is set.
constexpr const char* kAllowedSig = "38 99 ?? ?? ?? ?? 74 2F A1";                           // +2 allowed[256]

struct TableRow {  // 0x5bdbc8: the 12 rows of a column
  int32_t slot;
  float y;
};

using Init_t = int(__cdecl*)(uint8_t* data, void* params);
using Task_t = int(__cdecl*)(uint8_t* data);
using DrawNames_t = void(__cdecl*)();
using TextInBox_t = void(__cdecl*)(const char* text, float height, const float* box, int align);
using TextWidth_t = float(__cdecl*)(const char* text, float height);
using Answer_t = int(__cdecl*)(void* dialog);

Init_t oInit = nullptr;
Task_t oTerm = nullptr, oUpdate = nullptr;
DrawNames_t oDrawNames = nullptr;
TextInBox_t g_text_in_box = nullptr;
TextWidth_t g_text_width = nullptr;
Answer_t g_answer = nullptr;
uint8_t** g_task = nullptr;  // &[0x69f584]
uint8_t** g_aux = nullptr;   // &[0x69f580]
const TableRow* g_rows = nullptr;
const char* const* g_names = nullptr;
const char* g_none = "none";

constexpr int kButtons = binds::kMouseButtons;
constexpr const char* kKeys[kButtons] = {"left", "right", "middle", "x1", "x2"};
// In the table's font capitals are Cyrillic (L = Л, K = К, M = М, P = П, R = С):
// the buttons as keys are "ЛКМ", "ПКМ", "СКМ", "М4", "М5"; added to a key
// ([mouse] in tmntnet.ini) they follow its name as "+ЛКМ".
constexpr const char* kNames[kButtons] = {"LKM", "PKM", "RKM", "M4", "M5"};
constexpr const char* kSuffix[kButtons] = {"+LKM", "+PKM", "+RKM", "+M4", "+M5"};

// [mouse] additions: whose (0 = player 1) and which slot, -1 = none.
struct Bind {
  int player = 0, slot = -1;
};
Bind g_live[kButtons];  // as in use and in tmntnet.ini
Bind g_work[kButtons];  // while the table is up, committed by its OK
bool g_on = true;       // [mouse] player != 0
std::wstring g_ini;
bool g_alive = false;   // between the scene's init and term

int32_t& i32(uint8_t* p, size_t off) { return *reinterpret_cast<int32_t*>(p + off); }
void* ptr(uint8_t* p, size_t off) { return *reinterpret_cast<void**>(p + off); }

int row_of(int slot) {
  for (int r = 0; r < 12; ++r)
    if (g_rows[r].slot == slot) return r;
  return -1;
}

int32_t* work(uint8_t* t) { return reinterpret_cast<int32_t*>(t + kWork); }  // [player * 16 + slot]

bool in_work(uint8_t* t, int code) {
  for (int i = 0; i < 32; ++i)
    if (work(t)[i] == code) return true;
  return false;
}

const char* button_name(int code) {
  static const char* kRu[kButtons] = {"ЛКМ", "ПКМ", "СКМ", "М4", "М5"};
  return code >= binds::kMouseKey && code < binds::kMouseKey + kButtons ? kRu[code - binds::kMouseKey] : "?";
}

void commit() {
  if (!g_on) return;  // ([mouse] player=0: no additions, shown or kept)
  int players[kButtons], slots[kButtons];
  for (int i = 0; i < kButtons; ++i) {
    if (g_work[i].slot != g_live[i].slot || (g_work[i].slot >= 0 && g_work[i].player != g_live[i].player))
      ini_set(g_ini, "mouse", kKeys[i], mouse_button_value(g_work[i].player + 1, g_work[i].slot));
    g_live[i] = g_work[i];
    players[i] = g_live[i].player + 1, slots[i] = g_live[i].slot;
  }
  set_mouse_binds(g_on, players, slots);
}

int __cdecl hkInit(uint8_t* data, void* params) {
  int r = oInit(data, params);
  g_alive = true;
  for (int i = 0; i < kButtons; ++i) g_work[i] = g_on ? g_live[i] : Bind{};
  pause_mouse(true);  // no "+" presses here (a click would be taken as F); mouse keys work
  widescreen::set_key_table(true);
  return r;
}

int __cdecl hkTerm(uint8_t* data) {
  g_alive = false;  // (without OK the working copy is dropped)
  pause_mouse(false);
  widescreen::set_key_table(false);
  return oTerm(data);
}

int __cdecl hkUpdate(uint8_t* data) {
  uint8_t* t = *g_task;
  uint8_t* aux = *g_aux;
  if (!g_alive || !t || !aux) return oUpdate(data);
  // The dialogs' answers are read before the game closes them.
  int main = i32(aux, kAuxMain), cell = i32(aux, kAuxCell), cursor = i32(t, kCursor);
  bool cell_dialog = main == kInCell && cell == kCellDialog, ok_dialog = main == kOkDialog;
  int answer = (cell_dialog || ok_dialog) && ptr(t, kDialog) ? g_answer(ptr(t, kDialog)) : 0;
  int32_t was[32];
  std::memcpy(was, work(t), sizeof(was));
  int r = oUpdate(data);
  if (i32(t, kMode) != 0) return r;  // the gamepad table
  for (int i = 0; i < 32; ++i)
    if (work(t)[i] != was[i] && work(t)[i] >= binds::kMouseKey && work(t)[i] < binds::kMouseKey + kButtons)
      LOGI("key table: player %d slot %d = %s", i / 16 + 1, i % 16, button_name(work(t)[i]));
  if (answer == 1 && cell_dialog && cursor == kCursorReset)
    for (int i = 0; i < kButtons; ++i) g_work[i] = g_on ? g_live[i] : Bind{};
  if (answer == 1 && cell_dialog && cursor == kCursorDefaults)
    for (Bind& m : g_work) m = Bind{};  // the game's keys, no mouse
  // A button set as a key of its own loses its "+" (also after OK).
  for (int b = 0; b < kButtons; ++b)
    if (g_work[b].slot >= 0 && in_work(t, binds::kMouseKey + b)) g_work[b].slot = -1;
  if (answer == 1 && ok_dialog) {
    commit();
    std::string keys;
    for (int i = 0; i < 32; ++i)
      if (work(t)[i] >= binds::kMouseKey && work(t)[i] < binds::kMouseKey + kButtons)
        keys += (keys.empty() ? "" : ", ") + std::string("P") + std::to_string(i / 16 + 1) + " slot " +
                std::to_string(i % 16) + " " + button_name(work(t)[i]);
    LOGI("key table: OK, mouse keys: %s; [mouse] left %s, right %s", keys.empty() ? "none" : keys.c_str(),
         mouse_button_value(g_live[0].player + 1, g_live[0].slot).c_str(),
         mouse_button_value(g_live[1].player + 1, g_live[1].slot).c_str());
  }
  return r;
}

// After the game's names: "+ЛКМ" right after the name in a cell with a mouse
// button added ([mouse]), same font, colour and fade.
void __cdecl hkDrawNames() {
  oDrawNames();
  uint8_t* t = g_alive ? *g_task : nullptr;
  if (!t || i32(t, kMode) != 0) return;
  for (int b = 0; b < kButtons; ++b) {
    const Bind& m = g_work[b];
    int row = m.slot >= 0 ? row_of(m.slot) : -1;
    if (row < 0 || m.player < 0 || m.player > 1) continue;
    int code = i32(t, kWork + m.player * 0x40 + m.slot * 4);
    const char* name = code > 0 && code < 256 && g_names[code] ? g_names[code] : g_none;
    float box[4] = {kCellX[m.player] + g_text_width(name, kTextH) + kSuffixGap, g_rows[row].y, kSuffixW, kTextH};
    g_text_in_box(kSuffix[b], kTextH, box, 0);
  }
}

// The Russian font draws capitals in Cyrillic, and five names are capitals.
void fix_names(uintptr_t names) {
  static const struct {
    int code;
    const char *was, *now;
  } kFix[] = {{0x13, "R", "r"}, {0x1E, "A", "a"}, {0x22, "G", "g"}, {0x30, "B", "b"}, {0xD0, "DOWN", "down"}};
  int fixed = 0;
  for (const auto& f : kFix) {
    uintptr_t at = names + f.code * 4;
    uintptr_t s = patch::read<uint32_t>(at);
    if (!patch::in_image(s, 8) || std::strcmp(reinterpret_cast<const char*>(s), f.was) != 0) continue;
    uint32_t now = (uint32_t)reinterpret_cast<uintptr_t>(f.now);
    fixed += patch::write(at, &now, 4) ? 1 : 0;
  }
  LOGI("key table: %d key names written in Latin (DOWN was ГОЦН)", fixed);
}

// The mouse buttons become keys of the table: their codes (binds::kMouseKey,
// which no keyboard has) allowed and named. Only codes the game leaves
// unnamed (code 0's "NULL") and not allowed are taken.
bool mouse_keys(uintptr_t allowed, uintptr_t names) {
  if (!patch::in_image(allowed, 256)) return false;
  uint32_t unnamed = patch::read<uint32_t>(names);
  for (int b = 0; b < kButtons; ++b) {
    int code = binds::kMouseKey + b;
    if (patch::read<uint8_t>(allowed + code) != 0 || patch::read<uint32_t>(names + code * 4) != unnamed) return false;
  }
  for (int b = 0; b < kButtons; ++b) {
    int code = binds::kMouseKey + b;
    uint8_t yes = 1;
    uint32_t name = (uint32_t)reinterpret_cast<uintptr_t>(kNames[b]);
    if (!patch::write(allowed + code, &yes, 1) || !patch::write(names + code * 4, &name, 4)) return false;
  }
  return true;
}

}  // namespace

void install(const Config& cfg) {
  g_ini = module_dir(nullptr) + L"tmntnet.ini";
  g_on = cfg.mouse_on;
  for (int i = 0; i < kButtons; ++i) g_live[i] = Bind{cfg.mouse_player[i] - 1, cfg.mouse_slot[i]};

  uintptr_t update = patch::find("keyconf_update", kUpdateSig);
  uintptr_t draw_names = patch::find("keyconf_draw_names", kDrawNamesSig);
  uintptr_t init = patch::find("keyconf_init", kInitSig);
  uintptr_t term = patch::find("keyconf_term", kTermSig);
  uintptr_t text_in_box = widescreen::text_in_box();  // (widescreen::install found it first)
  uintptr_t text_width = patch::find("text_width", kTextWidthSig);
  uintptr_t answer = patch::find("dialog_answer", kAnswerSig);
  uintptr_t rows = patch::find("keyconf_rows", kRowsSig);
  uintptr_t names = patch::find("keyconf_names", kNamesSig);
  uintptr_t allowed = patch::find("keyconf_allowed", kAllowedSig);
  if (!(update && draw_names && init && term && text_in_box && text_width && answer && rows && names && allowed)) {
    LOGW("key table not recognised: mouse buttons are set in tmntnet.ini only");
    return;
  }
  g_task = reinterpret_cast<uint8_t**>(patch::read<uint32_t>(update + 1));
  g_aux = reinterpret_cast<uint8_t**>(patch::read<uint32_t>(update + 14));
  g_rows = reinterpret_cast<const TableRow*>(patch::read<uint32_t>(rows + 10));
  uintptr_t name_table = patch::read<uint32_t>(names + 3);
  g_names = reinterpret_cast<const char* const*>(name_table);
  g_none = reinterpret_cast<const char*>(patch::read<uint32_t>(names + 10));
  if (!patch::in_image(reinterpret_cast<uintptr_t>(g_task), 4) || !patch::in_image(reinterpret_cast<uintptr_t>(g_aux), 4) ||
      !patch::in_image(reinterpret_cast<uintptr_t>(g_rows), sizeof(TableRow) * 12) ||
      !patch::in_image(name_table, 256 * 4) || g_rows[5].slot != 6 || g_rows[6].slot != 7) {
    LOGW("key table: unexpected data, mouse buttons are set in tmntnet.ini only");
    return;
  }
  g_text_in_box = reinterpret_cast<TextInBox_t>(text_in_box);  // (through widescreen's 4:3 hook)
  g_text_width = reinterpret_cast<TextWidth_t>(text_width);
  g_answer = reinterpret_cast<Answer_t>(answer);
  fix_names(name_table);
  bool keys = mouse_keys(patch::read<uint32_t>(allowed + 2), name_table);
  patch::hook("keyconf_init", init, (void*)&hkInit, (void**)&oInit);
  patch::hook("keyconf_term", term, (void*)&hkTerm, (void**)&oTerm);
  patch::hook("keyconf_update", update, (void*)&hkUpdate, (void**)&oUpdate);
  patch::hook("keyconf_draw_names", draw_names, (void*)&hkDrawNames, (void**)&oDrawNames);
  if (keys)
    LOGI("key table: mouse buttons are keys there (ЛКМ, ПКМ, СКМ, М4, М5)");
  else
    LOGW("key table: codes 0x%02X.. are taken, mouse buttons only through [mouse] in tmntnet.ini", binds::kMouseKey);
}

}  // namespace keyconf
