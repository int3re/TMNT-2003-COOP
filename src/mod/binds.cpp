#include "mod/binds.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "common/log.h"
#include "mod/patch.h"

namespace binds {
namespace {

// Live table int32 code[set 2][player 2][slot 16]: set 0 = keyboard (DIK
// codes), set 1 = gamepad (button numbers). Getter (set, player).
constexpr const char* kTableSig = "8B 44 24 08 8B 4C 24 04 8D 04 88 C1 E0 06 05 ?? ?? ?? ?? C3";
constexpr size_t kTableImm = 15;
// rebuild(set): points every bind at its device's state byte again.
constexpr const char* kRebuildSig =
    "55 8B EC 83 EC 0C 83 65 FC 00 53 56 8B 75 08 C1 E6 08 57 C7 45 F8 ?? ?? ?? ?? 81 C6 ?? ?? ?? ?? FF 75 FC";
// save(): serialises the live settings into savesetting.dat.
constexpr const char* kSaveSig = "55 8B EC 81 EC A4 03 00 00 53 56 57 C7 85 5C FC FF FF 02 00 00 00 33 DB";
// Getter + setter of the gamepad each player uses: int index[2].
constexpr const char* kPadIndexSig = "8B 44 24 04 8B 04 85 ?? ?? ?? ?? C3 8B 44 24 08 8B 4C 24 04 89 04 8D";
constexpr size_t kPadIndexImm = 7;
// Gamepad poll: joystick count, device list head (keyboard, mouse, gamepads...).
constexpr const char* kDevicesSig =
    "83 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 57 8B 3D ?? ?? ?? ?? 6A 02 58 48 8B 7F 1C 75 FA";
constexpr size_t kDevCountImm = 2, kDevHeadImm = 16;
constexpr size_t kNodeName = 0x04, kNodeDevice = 0x08, kNodeSize = 0x0C, kNodeState = 0x10, kNodeNext = 0x1C;

int32_t* g_table = nullptr;
int32_t* g_pad_index = nullptr;
uint8_t** g_dev_head = nullptr;
int32_t* g_dev_count = nullptr;
using Rebuild_t = void(__cdecl*)(int set);
using Save_t = void(__cdecl*)();
Rebuild_t g_rebuild = nullptr;
Save_t g_save = nullptr;

int32_t* codes(int set, int player) { return g_table + (player + set * 4) * tmnp::kNumSlots; }

uint8_t* first_gamepad() {
  uint8_t* n = g_dev_head ? *g_dev_head : nullptr;
  for (int i = 0; i < 2 && n; ++i) n = *reinterpret_cast<uint8_t**>(n + kNodeNext);  // keyboard, mouse
  return n;
}

uint8_t* gamepad(int index) {
  int count = g_dev_count ? *g_dev_count : 0;
  if (index < 0 || index >= count) return nullptr;
  uint8_t* n = first_gamepad();
  for (int i = 0; i < index && n; ++i) n = *reinterpret_cast<uint8_t**>(n + kNodeNext);
  return n;
}

// The game's byte of an axis (0x4b2660): clamped to +-127, 0x80 = centre.
uint8_t axis_byte(LONG v) { return (uint8_t)(std::clamp<LONG>(v, -127, 127) + 0x80); }

// Slot order: dash, juggle, special, shuriken change, shuriken, jump, attack,
// strong, pause, -, -, start, up, right, down, left.
constexpr int kOriginal[2][tmnp::kNumSlots] = {
    {0x12, 0x13, 0x1D, 0x2A, 0x1F, 0x39, 0x21, 0x20, 0x01, -1, -1, 0x1C, 0xC8, 0xCD, 0xD0, 0xCB},
    {0x4E, 0x4D, 0x49, 0x48, 0x47, 0x52, 0x4B, 0x4C, 0x01, -1, -1, 0x9C, 0xC7, 0xD1, 0xCF, 0xD3}};
// Two at one keyboard: P1 on the left (WASD to move, J K L / U I O H, Space),
// P2 on the right (arrows to move, the number pad for everything else).
constexpr int kTwoOnOne[2][tmnp::kNumSlots] = {
    {0x16, 0x17, 0x18, 0x23, 0x26, 0x39, 0x24, 0x25, 0x01, -1, -1, 0x1C, 0x11, 0x20, 0x1F, 0x1E},
    {0x4F, 0x50, 0x48, 0x51, 0x4D, 0x52, 0x4B, 0x4C, 0x01, -1, -1, 0x9C, 0xC8, 0xCD, 0xD0, 0xCB}};

}  // namespace

void init(const Config&) {
  if (uintptr_t a = patch::find("binds_table_getter", kTableSig)) {
    uintptr_t t = patch::read<uint32_t>(a + kTableImm);
    if (patch::in_image(t, 0x100)) g_table = reinterpret_cast<int32_t*>(t);
  }
  if (uintptr_t a = patch::find("binds_rebuild", kRebuildSig)) g_rebuild = reinterpret_cast<Rebuild_t>(a);
  if (uintptr_t a = patch::find("settings_save", kSaveSig)) g_save = reinterpret_cast<Save_t>(a);
  if (uintptr_t a = patch::find("pad_index_getter", kPadIndexSig)) {
    uintptr_t t = patch::read<uint32_t>(a + kPadIndexImm);
    if (patch::in_image(t, 8)) g_pad_index = reinterpret_cast<int32_t*>(t);
  }
  if (uintptr_t a = patch::find("gamepad_poll", kDevicesSig)) {
    uintptr_t c = patch::read<uint32_t>(a + kDevCountImm), h = patch::read<uint32_t>(a + kDevHeadImm);
    if (patch::in_image(c, 4) && patch::in_image(h, 4)) {
      g_dev_count = reinterpret_cast<int32_t*>(c);
      g_dev_head = reinterpret_cast<uint8_t**>(h);
    }
  }
  LOGI("controls: %s", ready() ? "the game's binding table is editable" : "read-only or not found");
}

bool ready() { return g_table && g_rebuild && g_save; }

bool snapshot(Snapshot& s) {
  if (!g_table) return false;
  for (int p = 0; p < 2; ++p)
    for (int i = 0; i < tmnp::kNumSlots; ++i) {
      s.key[p][i] = codes(0, p)[i];
      s.pad[p][i] = codes(1, p)[i];
    }
  if (g_pad_index) {
    s.pad_index[0] = g_pad_index[0];
    s.pad_index[1] = g_pad_index[1];
  }
  s.pads.clear();
  int count = g_dev_count ? *g_dev_count : 0;
  uint8_t* n = first_gamepad();
  for (int i = 0; i < count && n; ++i, n = *reinterpret_cast<uint8_t**>(n + kNodeNext)) {
    const char* name = *reinterpret_cast<const char**>(n + kNodeName);
    s.pads.push_back(name ? name : "?");
  }
  return true;
}

bool set_keys(int player, const int in[tmnp::kNumSlots]) {
  if (!ready() || player < 0 || player > 1) return false;
  int32_t* c = codes(0, player);
  for (int i = 0; i < tmnp::kNumSlots; ++i)
    if (i != 9 && i != 10) c[i] = in[i];  // 9 and 10 are unused by the game
  g_rebuild(0);
  g_save();
  LOGI("controls: player %d keyboard keys changed and saved to savesetting.dat", player + 1);
  return true;
}

bool apply_preset(Preset p) {
  const auto& t = p == Preset::Original ? kOriginal : kTwoOnOne;
  bool ok = set_keys(0, t[0]) && set_keys(1, t[1]);
  LOGI("controls: preset %s", p == Preset::Original ? "original" : "two players at one keyboard");
  return ok;
}

void save_settings() {
  if (g_save) g_save();
}

int key_for(int player, int slot) {
  if (!g_table || player < 0 || player > 1 || slot < 0 || slot >= tmnp::kNumSlots) return -1;
  int c = codes(0, player)[slot];
  return c > 0 && c < 256 ? c : -1;
}

bool uses_key(int code) {
  if (!g_table || code <= 0) return false;
  for (int pl = 0; pl < 2; ++pl)
    for (int i = 0; i < tmnp::kNumSlots; ++i)
      if (codes(0, pl)[i] == code) return true;
  return false;
}

bool matches(Preset p) {
  if (!g_table) return false;
  const auto& t = p == Preset::Original ? kOriginal : kTwoOnOne;
  for (int pl = 0; pl < 2; ++pl)
    for (int i = 0; i < tmnp::kNumSlots; ++i)
      if (i != 9 && i != 10 && codes(0, pl)[i] != t[pl][i]) return false;
  return true;
}

void p2_only_keys(std::vector<int>& out) {
  out.clear();
  if (!g_table) return;
  const int32_t *p1 = codes(0, 0), *p2 = codes(0, 1);
  for (int i = 0; i < tmnp::kNumSlots; ++i) {
    int c = p2[i];
    if (c > 0 && c < 256 && std::find(p1, p1 + tmnp::kNumSlots, c) == p1 + tmnp::kNumSlots) out.push_back(c);
  }
}

bool is_p2_gamepad(const void* device) {
  if (!g_pad_index || !g_dev_count || !device) return false;
  int want = g_pad_index[1], count = *g_dev_count;
  uint8_t* n = first_gamepad();
  for (int i = 0; i < count && n; ++i, n = *reinterpret_cast<uint8_t**>(n + kNodeNext))
    if (i == want) return *reinterpret_cast<void**>(n + kNodeDevice) == device;
  return false;
}

int pad_count() { return g_dev_count ? std::max(0, (int)*g_dev_count) : 0; }

int pad_of(int player) { return g_pad_index && (player == 0 || player == 1) ? g_pad_index[player] : -1; }

std::string pad_name(int index) {
  uint8_t* n = gamepad(index);
  const char* name = n ? *reinterpret_cast<const char**>(n + kNodeName) : nullptr;
  return name ? name : "";
}

bool pad_read(int index, const int buttons[tmnp::kNumSlots], uint16_t& bits, uint32_t& sticks) {
  bits = 0;
  sticks = 0x80808080u;
  uint8_t* n = gamepad(index);
  if (!n || !*reinterpret_cast<void**>(n + kNodeDevice)) return false;
  const auto* js = *reinterpret_cast<const DIJOYSTATE* const*>(n + kNodeState);
  if (!js || *reinterpret_cast<const int32_t*>(n + kNodeSize) < (int32_t)sizeof(DIJOYSTATE)) return false;
  for (int i = 0; i < tmnp::kNumSlots; ++i) {
    int b = buttons[i];
    if (b >= 0 && b < 32 && (js->rgbButtons[b] & 0x80)) bits |= uint16_t(1u << i);
  }
  // The stick: a direction past 0x19 / 0xE5 of its byte (0x4b2839).
  uint8_t x = axis_byte(js->lX), y = axis_byte(js->lY);
  if (x < 0x19) bits |= 0x8000;
  else if (x > 0xE5) bits |= 0x2000;
  if (y < 0x19) bits |= 0x1000;
  else if (y > 0xE5) bits |= 0x4000;
  // The hat (0x4b272b): its eight exact directions, centre = -1.
  switch (js->rgdwPOV[0]) {
    case 0: bits |= 0x1000; break;
    case 4500: bits |= 0x3000; break;
    case 9000: bits |= 0x2000; break;
    case 13500: bits |= 0x6000; break;
    case 18000: bits |= 0x4000; break;
    case 22500: bits |= 0xC000; break;
    case 27000: bits |= 0x8000; break;
    case 31500: bits |= 0x9000; break;
    default: break;
  }
  // The pad slot's sticks (0x4b2660): axes 2, 3 (Rz, slider), X, Y.
  sticks = (uint32_t)axis_byte(js->lRz) | (uint32_t)axis_byte(js->rglSlider[0]) << 8 |
           (uint32_t)x << 16 | (uint32_t)y << 24;
  return true;
}

void pad_buttons(int player, int out[tmnp::kNumSlots]) {
  for (int i = 0; i < tmnp::kNumSlots; ++i) out[i] = g_table && (player == 0 || player == 1) ? codes(1, player)[i] : -1;
}

std::string key_name(int dik) {
  static const struct {
    int code;
    const char* name;
  } kNames[] = {
      {0x01, "Esc"},        {0x0C, "-"},           {0x0D, "="},          {0x0E, "Backspace"},
      {0x0F, "Tab"},        {0x1A, "["},           {0x1B, "]"},          {0x1C, "Enter"},
      {0x1D, "Левый Ctrl"}, {0x27, ";"},           {0x28, "'"},          {0x29, "`"},
      {0x2A, "Левый Shift"}, {0x2B, "\\"},         {0x33, ","},          {0x34, "."},
      {0x35, "/"},          {0x36, "Правый Shift"}, {0x37, "Num *"},     {0x38, "Левый Alt"},
      {0x39, "Пробел"},     {0x3A, "Caps Lock"},   {0x45, "Num Lock"},   {0x46, "Scroll Lock"},
      {0x47, "Num 7"},      {0x48, "Num 8"},       {0x49, "Num 9"},      {0x4A, "Num -"},
      {0x4B, "Num 4"},      {0x4C, "Num 5"},       {0x4D, "Num 6"},      {0x4E, "Num +"},
      {0x4F, "Num 1"},      {0x50, "Num 2"},       {0x51, "Num 3"},      {0x52, "Num 0"},
      {0x53, "Num ."},      {0x57, "F11"},         {0x58, "F12"},        {0x9C, "Num Enter"},
      {0x9D, "Правый Ctrl"}, {0xB5, "Num /"},      {0xB8, "Правый Alt"}, {0xC7, "Home"},
      {0xC8, "↑"},          {0xC9, "Page Up"},     {0xCB, "←"},          {0xCD, "→"},
      {0xCF, "End"},        {0xD0, "↓"},           {0xD1, "Page Down"},  {0xD2, "Insert"},
      {0xD3, "Delete"},     {kMouseKey, "ЛКМ"},    {kMouseKey + 1, "ПКМ"}, {kMouseKey + 2, "СКМ"},
      {kMouseKey + 3, "Мышь 4"}, {kMouseKey + 4, "Мышь 5"}};
  for (const auto& k : kNames)
    if (k.code == dik) return k.name;
  static const char kRows[] = "QWERTYUIOP\0\0\0\0ASDFGHJKL";  // 0x10..0x26
  if (dik >= 0x02 && dik <= 0x0B) return std::string(1, "1234567890"[dik - 0x02]);
  if (dik >= 0x10 && dik <= 0x26 && kRows[dik - 0x10]) return std::string(1, kRows[dik - 0x10]);
  if (dik >= 0x2C && dik <= 0x32) return std::string(1, "ZXCVBNM"[dik - 0x2C]);
  if (dik >= 0x3B && dik <= 0x44) return "F" + std::to_string(dik - 0x3B + 1);
  if (dik <= 0) return "—";
  char buf[16];
  snprintf(buf, sizeof(buf), "код 0x%02X", dik);
  return buf;
}

}  // namespace binds
