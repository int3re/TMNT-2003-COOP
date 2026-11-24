#include "mod/overlay.h"

#include <windows.h>
#include <d3d9.h>

#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "common/log.h"
#include "common/net.h"
#include "common/screenshot.h"
#include "mod/quality.h"
#include "mod/ambient.h"
#include "mod/audio.h"
#include "mod/autosave.h"
#include "mod/binds.h"
#include "mod/csel.h"
#include "mod/dinput_hook.h"
#include "mod/display.h"
#include "mod/game_options.h"
#include "mod/main_menu.h"
#include "mod/native_menu.h"
#include "mod/four_hud.h"
#include "mod/four_players.h"
#include "mod/nametags.h"
#include "mod/own_camera.h"
#include "mod/revive.h"
#include "mod/lockstep.h"
#include "mod/net_host.h"
#include "mod/net_peer.h"
#include "mod/patch.h"
#include "mod/ui.h"
#include "mod/widescreen.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace overlay {
namespace {

using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND,
                                              const RGNDATA*);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

constexpr int kVtCreateDevice = 16;  // IDirect3D9
constexpr int kVtReset = 16, kVtPresent = 17;  // IDirect3DDevice9
constexpr int kVtDrawPrimitive = 81, kVtDrawIndexedPrimitive = 82, kVtDrawPrimitiveUP = 83, kVtDrawIndexedPrimitiveUP = 84;

Direct3DCreate9_t oDirect3DCreate9 = nullptr;
CreateDevice_t oCreateDevice = nullptr;
Present_t oPresent = nullptr;
Reset_t oReset = nullptr;

IDirect3DDevice9* g_device = nullptr;  // the game's device
HWND g_hwnd = nullptr;
WNDPROC g_game_wndproc = nullptr;
bool g_imgui = false;      // ImGui context + backends initialized
bool g_dx9_ready = false;  // DX9 backend bound to g_device
std::wstring g_shot_dir;
bool g_shot_requested = false;
// Lab ([debug] draw_every=N): the game's draw calls are dropped in all frames
// but one of N, so a run costs the video card next to nothing; with hidden=1
// those frames are not presented either (nobody sees them, and a Present
// waits). The game's drawing code runs as ever: the game played is the same.
bool g_lab_dropped = false;  // this frame's draw calls of the game are dropped
uint32_t g_unblock_at = 0;  // keep blocking game input until this time

// --- input --------------------------------------------------------------------

LRESULT CALLBACK hkWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_DESTROY) lockstep::on_window_gone(), stop_net_host(), stop_net_peer();  // leave the Steam lobby, shut Steam down
  if (g_imgui) {
    // ImGui always sees the mouse (the hint button in the main menu is
    // clickable); keyboard and the rest only matter while our menu is open.
    // In a window the frame is scaled: ImGui works in frame pixels.
    LPARAM ilp = msg == WM_MOUSEMOVE ? display::map_mouse(lp) : lp;
    bool ours = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, ilp) != 0;
    if (ui::wants_input()) {
      if (ours) return TRUE;
      // Keep WM_SYSKEY* (Alt+F4, Alt+Tab) working; swallow keys, chars, mouse.
      if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR ||
          (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST))
        return 0;
    }
  }
  return CallWindowProcW(g_game_wndproc, hwnd, msg, wp, lp);
}

bool key_pressed(int vk) {
  static bool down[256];
  bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
  bool edge = now && !down[vk];
  down[vk] = now;
  return edge && GetForegroundWindow() == g_hwnd;
}

// --- screenshots -------------------------------------------------------------

void capture(IDirect3DDevice9* dev) {
  std::vector<uint8_t> px;
  unsigned w = 0, h = 0;
  if (!shot::capture(dev, px, w, h)) {
    LOGW("screenshot: could not read the back buffer (32-bit colour only)");
    return;
  }
  CreateDirectoryW(g_shot_dir.c_str(), nullptr);
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t name[64];
  swprintf(name, 64, L"tmnt_%04u%02u%02u_%02u%02u%02u_%03u.png", t.wYear, t.wMonth, t.wDay,
           t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  std::wstring path = g_shot_dir + L"\\" + name;
  // Encoding a 1080p PNG takes a while: off the game thread.
  std::thread([px = std::move(px), w, h, path] {
    if (shot::save_png(path, px, w, h)) {
      LOGI("screenshot saved: %ls", path.c_str());
      native_menu::notify("СКРИНШОТ", "SCREENSHOTS");
    } else {
      LOGW("screenshot: PNG encoding failed for %ls", path.c_str());
    }
  }).detach();
}

// [debug] shots: frame captures a set time after a screen first shows.
Config g_cfg;
void debug_shots(IDirect3DDevice9* dev) {
  static std::vector<bool> done;
  static uint32_t menu_at = 0, csel_at = 0, start_at = 0;
  if (g_cfg.debug_shots.empty()) return;
  uint32_t now = net::now_ms();
  if (!menu_at && main_menu::on_screen()) menu_at = now;
  if (!csel_at && csel::on_screen()) csel_at = now;
  if (!start_at) start_at = now;  // the first frame
  done.resize(g_cfg.debug_shots.size());
  for (size_t i = 0; i < done.size(); ++i) {
    const auto& s = g_cfg.debug_shots[i];
    bool page = s.first.compare(0, 4, "page") == 0;
    bool preset = s.first.compare(0, 7, "preset_") == 0 || s.first.compare(0, 6, "press_") == 0;
    bool cpress = s.first.compare(0, 7, "cpress_") == 0;  // a key press timed from the character select
    bool hurt = s.first.compare(0, 5, "hurt_") == 0 ||   // "hurt_3": player 3's life to 1, "kill_3": its turtle away
                s.first.compare(0, 5, "kill_") == 0;
    bool down = s.first == "down";  // a capture timed from the first life of 0 ([debug] actions=1)
    // "chold_05_2600:N" / "dhold_05_2600:N": key 0x05 held 2600 ms, N ms after the
    // character select / the first life of 0.
    bool chold = s.first.compare(0, 6, "chold_") == 0, dhold = s.first.compare(0, 6, "dhold_") == 0;
    bool dhurt = s.first.compare(0, 6, "dhurt_") == 0 ||  // "dhurt_3:N": hurt_3 timed from the first life of 0
                 s.first.compare(0, 6, "dkill_") == 0;    // ("dkill_3:N": kill_3 the same way)
    down |= dhold || dhurt;
    cpress |= chold;
    bool mouse = s.first.compare(0, 6, "mouse_") == 0;
    bool msaa = s.first.compare(0, 5, "msaa_") == 0;  // "msaa_8:N": anti-aliasing 8x, N ms after the select
    bool autosave = s.first.compare(0, 9, "autosave_") == 0;  // the game's АВТОЗАПИСЬ off/on
    bool savegame = s.first == "savegame" || s.first == "savecheck" || s.first.compare(0, 9, "savemark_") == 0;
    preset |= mouse || autosave || savegame;
    uint32_t at = s.first == "menu" || page || preset ? menu_at
                  : s.first == "csel" || cpress || msaa || (hurt && !dhurt) ? csel_at
                  : s.first == "start"            ? start_at
                  : down                          ? four::debug_down_at()
                                                  : 0;
    if (done[i] || !at) continue;
    if (page && now - at + 1500 >= (uint32_t)s.second)
      ui::open_page(std::atoi(s.first.c_str() + 4));
    if (now - at < (uint32_t)s.second) continue;
    done[i] = true;
    if (mouse) {
      debug_mouse(std::atoi(s.first.c_str() + 6));
      continue;
    }
    if (msaa) {
      quality::request_msaa(std::atoi(s.first.c_str() + 5));
      continue;
    }
    if (hurt || dhurt) {
      int p = std::atoi(s.first.c_str() + (dhurt ? 6 : 5)) - 1;
      if (s.first[0] == 'k' || s.first.compare(0, 6, "dkill_") == 0) four::debug_kill(p);
      else four::debug_set_life(p, 1);
      continue;
    }
    if (autosave) {
      game_options::set_autosave(s.first == "autosave_1");
      continue;
    }
    if (savegame) {
      if (s.first == "savegame")
        autosave::save_now("debug");
      else if (s.first == "savecheck")
        autosave::debug_compare();
      else
        autosave::debug_mark(std::atoi(s.first.c_str() + 9));
      continue;
    }
    if (chold || dhold) {
      char* end = nullptr;
      int dik = (int)std::strtol(s.first.c_str() + 6, &end, 16);
      int ms = end && *end == '_' ? std::atoi(end + 1) : 200;
      debug_hold(dik, ms);
      continue;
    }
    if (s.first.compare(0, 6, "press_") == 0 || cpress) {
      debug_press((int)std::strtol(s.first.c_str() + (cpress ? 7 : 6), nullptr, 16));
      continue;
    }
    if (s.first.compare(0, 7, "preset_") == 0) {
      bool two = s.first == "preset_two";
      LOGI("debug: applying the %s key preset", two ? "two-at-one-keyboard" : "original");
      binds::apply_preset(two ? binds::Preset::TwoOnOneKeyboard : binds::Preset::Original);
      continue;
    }
    if (g_lab_dropped) {  // (draw_every: nothing of the game is in this frame; the next drawn one)
      done[i] = false;
      continue;
    }
    std::vector<uint8_t> px;
    unsigned w = 0, h = 0;
    std::wstring path = g_shot_dir +L"\\debug_" +std::wstring(s.first.begin(), s.first.end()) + L"_" +
                        std::to_wstring(s.second) + L".png";
    CreateDirectoryW(g_shot_dir.c_str(), nullptr);
    if (!shot::capture(dev, px, w, h)) {
      IDirect3DSurface9* bb = nullptr;
      D3DSURFACE_DESC d{};
      HRESULT hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
      if (bb) {
        bb->GetDesc(&d);
        bb->Release();
      }
      LOGW("debug shot: could not read the back buffer (GetBackBuffer 0x%08lX, format %d, %ux%u, ms %d)",
           (unsigned long)hr, (int)d.Format, d.Width, d.Height, (int)d.MultiSampleType);
      continue;
    }
    std::thread([px = std::move(px), w, h, path] {  // WIC/COM off the game thread, like F12
      if (shot::save_png(path, px, w, h))
        LOGI("debug shot saved: %ls", path.c_str());
      else
        LOGW("debug shot: PNG encoding failed for %ls", path.c_str());
    }).detach();
  }
}

// --- ImGui lifetime ------------------------------------------------------------

void init_imgui(IDirect3DDevice9* dev) {
  if (!g_imgui) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini in the game folder
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplWin32_Init(g_hwnd);
    g_game_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hkWndProc)));
    ui::setup_style();
    g_imgui = true;
  }
  ImGui_ImplDX9_Init(dev);
  g_dx9_ready = true;
  LOGI("overlay ready on window %p (F2 network menu, F12 screenshot)", g_hwnd);
}

void render(IDirect3DDevice9* dev) {
  if (dev != g_device) return;
  HRESULT coop = dev->TestCooperativeLevel();
  static HRESULT last_coop = D3D_OK;
  if (coop != last_coop) {  // (nothing of tmntnet is drawn while the device is lost)
    LOGI("D3D9 device %s (0x%08lX)", coop == D3D_OK ? "back" : "lost", (unsigned long)coop);
    last_coop = coop;
  }
  if (coop != D3D_OK) return;
  if (!g_dx9_ready) init_imgui(dev);

  if (key_pressed(VK_F12)) g_shot_requested = true;
  // F2: the ImGui window, for debugging, or the way in when the game's menus
  // could not be patched (an unknown exe).
  if (key_pressed(VK_F2) && (g_cfg.debug_window || !main_menu::native())) ui::toggle();

  ImGui_ImplDX9_NewFrame();
  ImGui_ImplWin32_NewFrame();
  float bw, bh;
  if (display::backbuffer_size(bw, bh)) ImGui::GetIO().DisplaySize = ImVec2(bw, bh);  // not the window
  ImGui::NewFrame();
  ImGui::GetIO().MouseDrawCursor = ui::wants_input();  // the game hides the OS cursor
  static bool menu_seen = false;  // the game has loaded its save by then
  menu_seen = menu_seen || main_menu::on_screen();
  if (menu_seen) autosave::poll();
  ui::frame();
  ImGui::EndFrame();
  ImGui::Render();
  ImDrawData* dd = ImGui::GetDrawData();
  bool notes = native_menu::has_notices();
  own_camera::frame_done();
  bool tags = nametags::prepare(dev);
  bool stars = revive::prepare(dev);  // (after the tags: the camera's picture)
  bool panels = four_hud::prepare(dev);
  if (((dd && dd->TotalVtxCount > 0) || notes || tags || stars || panels) && SUCCEEDED(dev->BeginScene())) {
    if (panels) four_hud::draw(dev);
    if (stars) revive::draw(dev);
    if (tags) nametags::draw(dev);
    if (notes) native_menu::draw_notices(dev);
    if (dd && dd->TotalVtxCount > 0) ImGui_ImplDX9_RenderDrawData(dd);
    dev->EndScene();
  }
  debug_shots(dev);
  if (g_shot_requested) {
    g_shot_requested = false;
    capture(dev);
  }
}

// --- D3D9 hooks ---------------------------------------------------------------

// --- catching up with a game: no picture of it ------------------------------------------
// A PC coming into a running game plays its frames as fast as they go. The
// game's drawing still runs (it plays the game: re/notes.md §31), but nothing
// of it reaches the device, no sound is heard, and the screen shows a plate
// with how far it is, eight times a second.
bool g_covered = false;      // the game's draw calls are dropped
bool g_own_drawing = false;  // ... but not tmntnet's own
bool drop_draw(IDirect3DDevice9* dev) { return (g_covered || g_lab_dropped) && !g_own_drawing && dev == g_device; }

using DrawPrimitive_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitive_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUP_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUP_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT,
                                                             const void*, D3DFORMAT, const void*, UINT);
DrawPrimitive_t oDrawPrimitive = nullptr;
DrawIndexedPrimitive_t oDrawIndexedPrimitive = nullptr;
DrawPrimitiveUP_t oDrawPrimitiveUP = nullptr;
DrawIndexedPrimitiveUP_t oDrawIndexedPrimitiveUP = nullptr;

HRESULT STDMETHODCALLTYPE hkDrawPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT start, UINT count) {
  return drop_draw(dev) ? D3D_OK : oDrawPrimitive(dev, type, start, count);
}
HRESULT STDMETHODCALLTYPE hkDrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT base, UINT min_index,
                                                 UINT vertices, UINT start, UINT count) {
  return drop_draw(dev) ? D3D_OK : oDrawIndexedPrimitive(dev, type, base, min_index, vertices, start, count);
}
HRESULT STDMETHODCALLTYPE hkDrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT count, const void* data,
                                            UINT stride) {
  return drop_draw(dev) ? D3D_OK : oDrawPrimitiveUP(dev, type, count, data, stride);
}
HRESULT STDMETHODCALLTYPE hkDrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT min_index,
                                                   UINT vertices, UINT count, const void* indices, D3DFORMAT format,
                                                   const void* data, UINT stride) {
  return drop_draw(dev) ? D3D_OK
                        : oDrawIndexedPrimitiveUP(dev, type, min_index, vertices, count, indices, format, data, stride);
}

// The frame's end while catching up: true = this frame is not shown.
bool cover_frame(IDirect3DDevice9* dev) {
  const bool coming = lockstep::catching_up();  // (to its end: "fast" comes and goes near it)
  if (coming != g_covered) {
    g_covered = coming;
    audio::set_silent(coming);
  }
  if (!coming) return false;
  // Shown eight times a second: a Present waits for the screen's composer, and
  // at every tenth frame half of the catching up was that wait (lab profile:
  // 48 % in NtWaitForSingleObject under Present).
  static DWORD shown_at = 0;
  const DWORD now = GetTickCount();
  if (lockstep::fast() && now - shown_at < 125) return true;
  shown_at = now;
  // The plate, over black: the game's letters, the percent, a line of plain words.
  if (FAILED(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0)))
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
  native_menu::Prompt p;
  p.title = "ПОДКЛЮЧЕНИЕ К ИГРЕ";
  p.value = std::to_string(std::max(0, lockstep::catchup_percent())) + " %";
  p.hint = "ДОГОНЯЕМ ОСТАЛЬНЫХ ИГРОКОВ";
  p.rgb = 0x80FF80;
  p.arrows = false;
  p.alone = true;
  native_menu::show_prompt(p);
  return false;
}

HRESULT STDMETHODCALLTYPE hkPresent(IDirect3DDevice9* dev, const RECT* src, const RECT* dst,
                                    HWND wnd, const RGNDATA* dirty) {
  if (dev == g_device && cover_frame(dev)) {  // coming into a game: every tenth frame shown, no wait for the screen
    widescreen::end_frame();
    return D3D_OK;
  }
  g_own_drawing = true;
  if (dev == g_device && !g_covered) ambient::before_present(dev);  // side bars, under the overlay
  widescreen::end_frame();
  if (g_cfg.debug_shots.size()) {  // [debug] lab runs: a heartbeat of the frames
    static uint32_t frames = 0, since = 0;
    ++frames;
    uint32_t now = net::now_ms();
    if (!since) since = now;
    if (now - since >= 5000) {
      LOGI("debug: %u frames presented in the last %u ms", frames, now - since);
      frames = 0, since = now;
    }
  }
  render(dev);
  g_own_drawing = false;
  if (dev == g_device) dst = display::before_present(wnd ? wnd : g_hwnd, dst);
  if (dev == g_device && g_cfg.debug_draw_every > 1) {
    static uint32_t frame = 0;
    const bool dropped = g_lab_dropped;
    g_lab_dropped = ++frame % (uint32_t)g_cfg.debug_draw_every != 0;  // (the next frame's)
    if (dropped && g_cfg.debug_hidden) return D3D_OK;
  }
  HRESULT hr = oPresent(dev, src, dst, wnd, dirty);
  static unsigned failures = 0;
  if (FAILED(hr) && hr != D3DERR_DEVICELOST && (++failures == 1 || failures % 600 == 0))
    LOGW("D3D9 Present failed: 0x%08lX (%u times)", (unsigned long)hr, failures);
  return hr;
}

// The game's parameters with tmntnet's: anti-aliasing (quality.cpp), then the
// window (display.cpp). Points at the game's own when nothing changes.
D3DPRESENT_PARAMETERS* ours_for(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, D3DPRESENT_PARAMETERS* pp, bool aa,
                                D3DPRESENT_PARAMETERS& with_aa, D3DPRESENT_PARAMETERS& out) {
  if (!pp) return pp;
  with_aa = *pp;
  bool ms = aa && quality::adjust_present(d3d, adapter, type, with_aa);
  const D3DPRESENT_PARAMETERS& game = ms ? with_aa : *pp;
  if (display::adjust_present(game, out)) return &out;
  return ms ? &with_aa : pp;
}

HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
  bool ours = dev == g_device && g_dx9_ready;
  if (ours) ImGui_ImplDX9_InvalidateDeviceObjects();
  if (dev == g_device) ambient::release();
  D3DPRESENT_PARAMETERS with_aa, mine;
  D3DPRESENT_PARAMETERS* use = pp;
  IDirect3D9* d3d = nullptr;
  D3DDEVICE_CREATION_PARAMETERS cp{};
  bool known = dev == g_device && SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d && SUCCEEDED(dev->GetCreationParameters(&cp));
  if (dev == g_device) use = ours_for(d3d, cp.AdapterOrdinal, cp.DeviceType, pp, known, with_aa, mine);
  HRESULT hr = oReset(dev, use);
  if (FAILED(hr) && use && pp && use->MultiSampleType != pp->MultiSampleType) {
    LOGW("D3D9 Reset with anti-aliasing refused (0x%08lX), again without", (unsigned long)hr);
    use = ours_for(d3d, cp.AdapterOrdinal, cp.DeviceType, pp, false, with_aa, mine);
    hr = oReset(dev, use);
  }
  if (d3d) d3d->Release();
  if (use && dev == g_device)
    LOGI("D3D9 Reset %ux%u %s: 0x%08lX", use->BackBufferWidth, use->BackBufferHeight,
         use->Windowed ? "windowed" : "fullscreen", (unsigned long)hr);
  if (SUCCEEDED(hr) && use && dev == g_device) {
    display::on_device(g_hwnd, use->BackBufferWidth, use->BackBufferHeight);
    quality::on_device(dev, *use);
  }
  if (ours && SUCCEEDED(hr)) ImGui_ImplDX9_CreateDeviceObjects();
  return hr;
}

HRESULT STDMETHODCALLTYPE hkCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus,
                                         DWORD flags, D3DPRESENT_PARAMETERS* pp,
                                         IDirect3DDevice9** out) {
  // Borderless / windowed: the device is created windowed from a copy, so
  // RenderWare keeps its own (fullscreen) parameters and frame size.
  D3DPRESENT_PARAMETERS with_aa, mine;
  D3DPRESENT_PARAMETERS* use = ours_for(d3d, adapter, type, pp, true, with_aa, mine);
  HRESULT hr = oCreateDevice(d3d, adapter, type, focus, flags, use, out);
  if (FAILED(hr) && use && pp && use->MultiSampleType != pp->MultiSampleType) {
    LOGW("D3D9: device with anti-aliasing refused (0x%08lX), making it without", (unsigned long)hr);
    use = ours_for(d3d, adapter, type, pp, false, with_aa, mine);
    hr = oCreateDevice(d3d, adapter, type, focus, flags, use, out);
  }
  if (FAILED(hr) && use != pp) {
    LOGW("D3D9: windowed device refused (0x%08lX), creating the game's own", (unsigned long)hr);
    use = pp;
    hr = oCreateDevice(d3d, adapter, type, focus, flags, pp, out);
  }
  if (FAILED(hr) || !out || !*out) return hr;
  if (g_dx9_ready) {  // the game re-created its device (mode change)
    ImGui_ImplDX9_Shutdown();
    g_dx9_ready = false;
  }
  g_device = *out;
  if (!g_hwnd) g_hwnd = (pp && pp->hDeviceWindow) ? pp->hDeviceWindow : focus;
  if (!oPresent) {
    void** vt = *reinterpret_cast<void***>(*out);
    patch::hook("IDirect3DDevice9::Present", (uintptr_t)vt[kVtPresent], (void*)&hkPresent,
                (void**)&oPresent);
    patch::hook("IDirect3DDevice9::Reset", (uintptr_t)vt[kVtReset], (void*)&hkReset,
                (void**)&oReset);
    patch::hook("IDirect3DDevice9::DrawPrimitive", (uintptr_t)vt[kVtDrawPrimitive], (void*)&hkDrawPrimitive,
                (void**)&oDrawPrimitive);
    patch::hook("IDirect3DDevice9::DrawIndexedPrimitive", (uintptr_t)vt[kVtDrawIndexedPrimitive],
                (void*)&hkDrawIndexedPrimitive, (void**)&oDrawIndexedPrimitive);
    patch::hook("IDirect3DDevice9::DrawPrimitiveUP", (uintptr_t)vt[kVtDrawPrimitiveUP], (void*)&hkDrawPrimitiveUP,
                (void**)&oDrawPrimitiveUP);
    patch::hook("IDirect3DDevice9::DrawIndexedPrimitiveUP", (uintptr_t)vt[kVtDrawIndexedPrimitiveUP],
                (void*)&hkDrawIndexedPrimitiveUP, (void**)&oDrawIndexedPrimitiveUP);
  }
  LOGI("D3D9 device %ux%u %s, present interval 0x%lX", use ? use->BackBufferWidth : 0,
       use ? use->BackBufferHeight : 0, use && use->Windowed ? "windowed" : "fullscreen",
       use ? (unsigned long)use->PresentationInterval : 0ul);
  if (use) {
    display::on_device(g_hwnd, use->BackBufferWidth, use->BackBufferHeight);
    quality::on_device(*out, *use);
  }
  return hr;
}

IDirect3D9* WINAPI hkDirect3DCreate9(UINT sdk) {
  IDirect3D9* d3d = oDirect3DCreate9(sdk);
  if (d3d && !oCreateDevice) {
    void** vt = *reinterpret_cast<void***>(d3d);
    patch::hook("IDirect3D9::CreateDevice", (uintptr_t)vt[kVtCreateDevice], (void*)&hkCreateDevice,
                (void**)&oCreateDevice);
  }
  return d3d;
}

}  // namespace

void install(const Config& cfg) {
  g_cfg = cfg;
  g_shot_dir = module_dir(nullptr) + L"screenshots";
  HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");  // statically imported by tmnt.exe
  if (!d3d9) d3d9 = LoadLibraryW(L"d3d9.dll");
  auto create = d3d9 ? (uintptr_t)GetProcAddress(d3d9, "Direct3DCreate9") : 0;
  if (!create) {
    LOGW("overlay: Direct3DCreate9 not found, in-game menu disabled");
    return;
  }
  patch::hook("Direct3DCreate9", create, (void*)&hkDirect3DCreate9, (void**)&oDirect3DCreate9);
}

IDirect3DDevice9* device() { return g_device; }

bool blocks_game_input() {
  uint32_t now = net::now_ms();
  if (ui::wants_input()) {
    g_unblock_at = now + 300;  // swallow the key that closes the menu, too
    return true;
  }
  return (int32_t)(g_unblock_at - now) > 0;
}

}  // namespace overlay
