#include "mod/dinput_hook.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "common/log.h"
#include "common/net.h"
#include "common/protocol.h"
#include "mod/display.h"
#include "mod/four_players.h"
#include "mod/audio.h"
#include "mod/binds.h"
#include "mod/csel.h"
#include "mod/lockstep.h"
#include "mod/main_menu.h"
#include "mod/p2_binds.h"
#include "mod/overlay.h"
#include "mod/patch.h"
#include "mod/quality.h"
#include "mod/shared_state.h"

namespace {

using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using GetDeviceState_t = HRESULT(__stdcall*)(IDirectInputDevice8A*, DWORD, LPVOID);

constexpr int kVtGetDeviceState = 9;
constexpr DWORD kKeyboardStateSize = 256;  // c_dfDIKeyboard
constexpr uint32_t kStatsEveryMs = 10000;

GetDeviceState_t oGetDeviceState = nullptr;
int g_autostart = 0;      // [debug] autostart
bool g_intro_skip = false;  // [audio] intro=skip

struct Stats {
  uint32_t frames = 0, active = 0, age_sum = 0, age_max = 0, since_ms = 0;
  bool was_connected = false;
} g_stats;

// "input age" = how long the newest P2 input waited for a game frame; the
// network part of the latency is about rtt/2.
void log_stats_if_due(bool connected, uint32_t age_ms, uint32_t now) {
  if (connected != g_stats.was_connected) {
    g_stats = Stats{};
    g_stats.was_connected = connected;
    g_stats.since_ms = now;
    LOGI("P2 injection %s", connected ? "active" : "idle (P2 released)");
  }
  if (!connected) return;
  ++g_stats.frames;
  g_stats.age_sum += age_ms;
  if (age_ms > g_stats.age_max) g_stats.age_max = age_ms;
  if (now - g_stats.since_ms >= kStatsEveryMs) {
    LOGI("P2 latency: rtt=%u ms, input age at injection avg=%u max=%u ms; %u frames in %u ms (%u with buttons held)",
         g_p2.rtt_ms.load(), g_stats.age_sum / g_stats.frames, g_stats.age_max,
         g_stats.frames, now - g_stats.since_ms, g_stats.active);
    g_stats.frames = g_stats.active = g_stats.age_sum = g_stats.age_max = 0;
    g_stats.since_ms = now;
  }
}

// Game thread. Never blocks: reads the network thread's atomics only.
void inject_p2_keyboard(BYTE* keys) {
  g_frame.fetch_add(1, std::memory_order_relaxed);
  bool connected = g_p2.connected.load(std::memory_order_acquire);
  uint32_t buttons = g_p2.buttons.load(std::memory_order_acquire) |
                     g_p2.latch.exchange(0, std::memory_order_acq_rel);
  uint32_t now = net::now_ms();
  uint32_t age = connected ? now - g_p2.last_input_ms.load() : 0;
  log_stats_if_due(connected, age, now);
  if (!connected || !buttons) return;

  ++g_stats.active;
  int codes[tmnp::kNumSlots];
  p2binds::current(codes);
  for (int i = 0; i < tmnp::kNumSlots; ++i) {
    if ((buttons & (1u << i)) && codes[i] > 0 && codes[i] < 256) keys[codes[i]] = 0x80;
  }
}

// The mouse buttons held now, while the game's window is in front and the
// pointer is over it. After the window comes to the front, buttons count only
// once all are released (the click that brought it there is not an attack).
std::atomic<int> g_debug_mouse{-1};  // [debug] mouse_N: button N "down" for 200 ms
std::atomic<uint32_t> g_debug_mouse_until{0};

void mouse_down(bool down[binds::kMouseButtons]) {
  static bool armed = false;
  HWND hwnd = display::window();
  POINT pt{};
  RECT rc{};
  bool front = hwnd && GetForegroundWindow() == hwnd && GetCursorPos(&pt) && ScreenToClient(hwnd, &pt) &&
               GetClientRect(hwnd, &rc) && PtInRect(&rc, pt);
  static const int kVk[binds::kMouseButtons] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
  bool any = false;
  for (int i = 0; i < binds::kMouseButtons; ++i) any |= down[i] = (GetAsyncKeyState(kVk[i]) & 0x8000) != 0;
  if (int b = debug_mouse_button(); b >= 0) {
    for (int i = 0; i < binds::kMouseButtons; ++i) down[i] = i == b;  // lab: as if pressed over the window in front
    front = armed = true;
  }
  bool counts = front && armed;
  armed = front && (armed || !any);
  if (!counts)
    for (int i = 0; i < binds::kMouseButtons; ++i) down[i] = false;
}

// Mouse buttons set in the game's key table are keys of their own
// (binds::kMouseKey + button): the game binds, swaps, saves and reads them
// like keys, and the netplay lock below treats them like P2's other keys.
void press_mouse_keys(BYTE* keys, const bool down[binds::kMouseButtons]) {
  for (int i = 0; i < binds::kMouseButtons; ++i)
    if (down[i]) keys[binds::kMouseKey + i] = 0x80;
}

// [mouse] in tmntnet.ini: a button added to a player's key for a slot ("F
// +ЛКМ" in the key table), unless that button is a key of its own there.
std::atomic<bool> g_mouse_on{true};
std::atomic<bool> g_mouse_paused{false};
std::atomic<int> g_mouse_player[5] = {1, 1, 1, 1, 1};
std::atomic<int> g_mouse_slot[5] = {-1, -1, -1, -1, -1};

void press_mouse_buttons(BYTE* keys, const bool down[binds::kMouseButtons], bool remote_p2) {
  if (!g_mouse_on.load() || g_mouse_paused.load()) return;
  for (int i = 0; i < binds::kMouseButtons; ++i) {
    int player = g_mouse_player[i].load(), slot = g_mouse_slot[i].load();
    if (!down[i] || slot < 0 || (player == 2 && remote_p2)) continue;  // (P2 is the remote player's)
    if (binds::uses_key(binds::kMouseKey + i)) continue;               // set in the key table: that wins
    int code = binds::key_for(player - 1, slot);
    if (code > 0) keys[code] = 0x80;
  }
}

// [debug] press_XX: a key held down for a moment, as if from the keyboard.
std::atomic<int> g_debug_key{0};
std::atomic<uint32_t> g_debug_key_until{0};

// Key capture for the controls page: the first key that goes down.
std::atomic<bool> g_capturing{false};
std::atomic<int> g_captured{-1};
bool g_p2_lock = true;  // [mod] p2_lock

void capture_key(const BYTE* keys) {
  static BYTE prev[kKeyboardStateSize];
  if (g_capturing.load()) {
    for (int c = 1; c < (int)kKeyboardStateSize; ++c)
      if ((keys[c] & 0x80) && !(prev[c] & 0x80)) {
        g_captured = c;
        g_capturing = false;
        break;
      }
  }
  std::memcpy(prev, keys, kKeyboardStateSize);
}

// Taps P1 Start (Enter) and decide (F) by turns: logos, the intro movie and
// the title screen all give way to one of them ([audio] intro=skip).
void press_through(BYTE* keys) {
  uint32_t phase = net::now_ms() / 150 % 8;  // 150 ms down, 450 ms up
  if (phase == 0) keys[0x1C] = 0x80;
  if (phase == 4) keys[0x21] = 0x80;
}

// [debug] autostart (lab runs): presses through to the main menu, so tests
// reach it without anyone at the keys; autostart=2 then picks the first main
// menu item (STORY) and decides until the character select shows.
void autostart(BYTE* keys) {
  static int stage = 0;  // 0 = to the main menu, 1 = wait in it, 2 = to the character select, 3 = done
  static uint32_t since = 0;
  uint32_t now = net::now_ms();
  if (stage == 0 && main_menu::on_screen()) {
    LOGI("debug autostart: main menu reached");
    stage = g_autostart >= 2 ? 1 : 3;
    since = now;
  }
  if (stage == 1 && now - since > 4000) stage = 2;
  if (stage == 2 && csel::on_screen()) {
    LOGI("debug autostart: character select reached");
    stage = 3;
  }
  if (stage == 0) press_through(keys);
  if (stage == 2 && now / 150 % 8 == 4) keys[0x21] = 0x80;  // P1 decide only
}

// While tmntnet's own menu is open the game sees "nothing pressed" on every
// device (keyboard, mouse, joysticks).
void blank_state(DWORD cb, void* data) {
  std::memset(data, 0, cb);
  if (cb == sizeof(DIJOYSTATE) || cb == sizeof(DIJOYSTATE2)) {
    auto* js = static_cast<DIJOYSTATE*>(data);
    for (auto& pov : js->rgdwPOV) pov = 0xFFFFFFFF;  // centred hat; axes 0 = centre (+-128 range)
  }
}

HRESULT __stdcall hkGetDeviceState(IDirectInputDevice8A* self, DWORD cb, LPVOID data) {
  HRESULT hr = oGetDeviceState(self, cb, data);
  if (g_autostart && cb == kKeyboardStateSize) {  // [debug] lab runs: a heartbeat of the game loop
    static uint32_t polls = 0, since = 0;
    ++polls;
    uint32_t now = net::now_ms();
    if (!since) since = now;
    if (now - since >= 5000) {
      LOGI("debug: %u keyboard polls in the last %u ms (hr 0x%08lX)", polls, now - since, (unsigned long)hr);
      polls = 0, since = now;
    }
  }
  // [video] background=run: the keyboard (foreground cooperative level) cannot
  // be read while another window is in front. Report an empty keyboard so P2
  // keeps playing; P1 is not at the keys anyway.
  if ((hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) && data && cb == kKeyboardStateSize &&
      display::fake_unfocused_keyboard()) {
    std::memset(data, 0, cb);
    hr = DI_OK;
  }
  if (SUCCEEDED(hr) && data) {
    // [debug] press_XX: as if the key were down on the real keyboard.
    if (int k = g_debug_key.load(); cb == kKeyboardStateSize && k > 0 && k < 256 &&
                                    (int32_t)(g_debug_key_until.load() - net::now_ms()) > 0)
      static_cast<BYTE*>(data)[k] = 0x80;
    if (cb == kKeyboardStateSize) capture_key(static_cast<const BYTE*>(data));
    if (cb == kKeyboardStateSize) quality::poll();  // ([debug] shots msaa_N)
    bool remote_p2 = g_p2_lock && g_p2.connected.load(std::memory_order_acquire);
    if (overlay::blocks_game_input()) {
      blank_state(cb, data);
      if (cb == kKeyboardStateSize) g_frame.fetch_add(1, std::memory_order_relaxed);
    } else if (remote_p2 && cb != kKeyboardStateSize && cb != 16 && binds::is_p2_gamepad(self)) {
      blank_state(cb, data);  // netplay: P2's gamepad at the host does nothing
    } else if (cb == kKeyboardStateSize) {
      // Only the keyboard uses a 256-byte state (mouse 16, joystick 80 in this game).
      BYTE* keys = static_cast<BYTE*>(data);
      bool mouse[binds::kMouseButtons];
      mouse_down(mouse);
      press_mouse_keys(keys, mouse);
      if (remote_p2) {  // netplay: the host's own P2 keys do nothing (mouse ones too)
        static std::vector<int> only;
        binds::p2_only_keys(only);
        for (int c : only) keys[c] = 0;
      }
      press_mouse_buttons(keys, mouse, remote_p2);
      inject_p2_keyboard(keys);
      four::on_keyboard(keys);
      if (g_autostart)
        autostart(keys);
      else if (g_intro_skip && audio::in_intro())
        press_through(keys);
      lockstep::on_keyboard(keys);
    }
  }
  return hr;
}

}  // namespace

bool install_dinput_hooks(const Config& cfg) {
  g_autostart = cfg.debug_autostart;
  g_intro_skip = cfg.intro_skip;
  g_p2_lock = cfg.p2_lock;
  set_mouse_binds(cfg.mouse_on, cfg.mouse_player, cfg.mouse_slot);
  // Load the real DLL by full path: a dinput8.dll shim may sit next to tmnt.exe.
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  std::wstring path = std::wstring(sys) + L"\\dinput8.dll";
  HMODULE di = LoadLibraryW(path.c_str());
  if (!di) {
    LOGE("cannot load %ls (error %lu)", path.c_str(), GetLastError());
    return false;
  }
  auto create = reinterpret_cast<DirectInput8Create_t>(GetProcAddress(di, "DirectInput8Create"));
  if (!create) {
    LOGE("DirectInput8Create not exported by system dinput8.dll");
    return false;
  }

  // The vtable belongs to dinput8's device class and is shared by every
  // IDirectInputDevice8A, including the ones the game already created.
  IDirectInput8A* dinput = nullptr;
  HRESULT hr = create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A,
                      reinterpret_cast<void**>(&dinput), nullptr);
  if (FAILED(hr) || !dinput) {
    LOGE("DirectInput8Create failed: 0x%08lX", hr);
    return false;
  }
  IDirectInputDevice8A* kb = nullptr;
  hr = dinput->CreateDevice(GUID_SysKeyboard, &kb, nullptr);
  if (FAILED(hr) || !kb) {
    LOGE("CreateDevice(GUID_SysKeyboard) failed: 0x%08lX", hr);
    dinput->Release();
    return false;
  }

  // Hook the implementation itself (not the vtable slot): it is shared by every
  // device the game creates, and also catches calls from wrapper DLLs.
  void** vt = *reinterpret_cast<void***>(kb);
  void* target = vt[kVtGetDeviceState];
  kb->Release();
  dinput->Release();
  // dinput8.dll stays loaded (we never FreeLibrary) so the target stays valid.

  LOGD("dinput8 device vtable %p, GetDeviceState %p", vt, target);
  return patch::hook("IDirectInputDevice8A::GetDeviceState", reinterpret_cast<uintptr_t>(target),
                     reinterpret_cast<void*>(&hkGetDeviceState),
                     reinterpret_cast<void**>(&oGetDeviceState));
}

void begin_key_capture() {
  g_captured = -1;
  g_capturing = true;
}

int take_captured_key() {
  if (g_capturing.load()) return -1;
  return g_captured.exchange(-1);
}

void cancel_key_capture() {
  g_capturing = false;
  g_captured = -1;
}

void set_p2_lock(bool on) { g_p2_lock = on; }

void debug_press(int dik) {
  g_debug_key = dik;
  g_debug_key_until = net::now_ms() + 100;  // (shorter than the menus' key repeat)
  LOGI("debug: key 0x%02X held for 100 ms", dik);
}

void debug_hold(int dik, int ms) {
  g_debug_key = dik;
  g_debug_key_until = net::now_ms() + (uint32_t)std::max(ms, 1);
  LOGI("debug: key 0x%02X held for %d ms", dik, ms);
}

void pause_mouse(bool paused) { g_mouse_paused = paused; }

void set_mouse_binds(bool on, const int players[5], const int slots[5]) {
  g_mouse_on = on;
  for (int i = 0; i < 5; ++i) g_mouse_player[i] = players[i] == 2 ? 2 : 1, g_mouse_slot[i] = slots[i];
}

int debug_mouse_button() {
  int b = g_debug_mouse.load();
  return b >= 0 && b < 5 && (int32_t)(g_debug_mouse_until.load() - net::now_ms()) > 0 ? b : -1;
}

void debug_mouse(int button) {
  g_debug_mouse = button;
  g_debug_mouse_until = net::now_ms() + 200;
  LOGI("debug: mouse button %d held for 200 ms", button);
}
