#include <windows.h>
#include <xinput.h>

#include <cctype>
#include <cstring>
#include <string>

#include "common/config.h"

namespace {

std::string upper(const std::string& s) {
  std::string u = s;
  for (auto& ch : u) ch = (char)std::toupper((unsigned char)ch);
  return u;
}

struct Named {
  const char* name;
  int value;
};

constexpr Named kVk[] = {
    {"SPACE", VK_SPACE},       {"ENTER", VK_RETURN},     {"RETURN", VK_RETURN},
    {"BACKSPACE", VK_BACK},    {"TAB", VK_TAB},          {"ESC", VK_ESCAPE},
    {"ESCAPE", VK_ESCAPE},     {"SHIFT", VK_SHIFT},      {"LSHIFT", VK_LSHIFT},
    {"RSHIFT", VK_RSHIFT},     {"CTRL", VK_CONTROL},     {"LCTRL", VK_LCONTROL},
    {"RCTRL", VK_RCONTROL},    {"ALT", VK_MENU},         {"LALT", VK_LMENU},
    {"RALT", VK_RMENU},        {"UP", VK_UP},            {"DOWN", VK_DOWN},
    {"LEFT", VK_LEFT},         {"RIGHT", VK_RIGHT},      {"HOME", VK_HOME},
    {"END", VK_END},           {"PGUP", VK_PRIOR},       {"PGDN", VK_NEXT},
    {"INSERT", VK_INSERT},     {"DELETE", VK_DELETE},    {"NUMPAD0", VK_NUMPAD0},
    {"NUMPAD1", VK_NUMPAD1},   {"NUMPAD2", VK_NUMPAD2},  {"NUMPAD3", VK_NUMPAD3},
    {"NUMPAD4", VK_NUMPAD4},   {"NUMPAD5", VK_NUMPAD5},  {"NUMPAD6", VK_NUMPAD6},
    {"NUMPAD7", VK_NUMPAD7},   {"NUMPAD8", VK_NUMPAD8},  {"NUMPAD9", VK_NUMPAD9},
    {"ADD", VK_ADD},           {"SUBTRACT", VK_SUBTRACT}, {"MULTIPLY", VK_MULTIPLY},
    {"DIVIDE", VK_DIVIDE},     {"DECIMAL", VK_DECIMAL},  {"COMMA", VK_OEM_COMMA},
    {"PERIOD", VK_OEM_PERIOD}, {"SEMICOLON", VK_OEM_1},  {"SLASH", VK_OEM_2},
    {"MINUS", VK_OEM_MINUS},   {"PLUS", VK_OEM_PLUS},    {"LBRACKET", VK_OEM_4},
    {"RBRACKET", VK_OEM_6},    {"QUOTE", VK_OEM_7},      {"BACKSLASH", VK_OEM_5},
};

constexpr Named kPad[] = {
    {"A", XINPUT_GAMEPAD_A},
    {"B", XINPUT_GAMEPAD_B},
    {"X", XINPUT_GAMEPAD_X},
    {"Y", XINPUT_GAMEPAD_Y},
    {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER},
    {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {"LT", (int)kPadLT},
    {"RT", (int)kPadRT},
    {"BACK", XINPUT_GAMEPAD_BACK},
    {"START", XINPUT_GAMEPAD_START},
    {"LS", XINPUT_GAMEPAD_LEFT_THUMB},
    {"RS", XINPUT_GAMEPAD_RIGHT_THUMB},
    {"DPAD_UP", XINPUT_GAMEPAD_DPAD_UP},
    {"DPAD_DOWN", XINPUT_GAMEPAD_DPAD_DOWN},
    {"DPAD_LEFT", XINPUT_GAMEPAD_DPAD_LEFT},
    {"DPAD_RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
};

}  // namespace

int parse_vk(const std::string& name) {
  std::string u = upper(name);
  if (u.empty()) return -1;
  if (u.size() == 1 && (std::isalnum((unsigned char)u[0]))) return u[0];  // 'A'..'Z', '0'..'9'
  if (u[0] == 'F' && u.size() <= 3) {
    int n = std::atoi(u.c_str() + 1);
    if (n >= 1 && n <= 24) return VK_F1 + n - 1;
  }
  for (const auto& e : kVk)
    if (u == e.name) return e.value;
  int v = parse_int(name, -1);
  return (v > 0 && v < 256) ? v : -1;
}

uint32_t parse_pad_button(const std::string& name) {
  std::string u = upper(name);
  for (const auto& e : kPad)
    if (u == e.name) return (uint32_t)e.value;
  return 0;
}
