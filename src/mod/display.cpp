#include "mod/display.h"

#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/log.h"
#include "mod/lockstep.h"
#include "mod/main_menu.h"
#include "mod/patch.h"

namespace display {
namespace {

enum class Mode { Fullscreen, Borderless, Windowed };
Config g_cfg;
Mode g_mode = Mode::Fullscreen;
std::atomic<HWND> g_hwnd{nullptr};
UINT g_bb_w = 0, g_bb_h = 0;        // the frame the game renders (the video mode)
int g_render_w = 0, g_render_h = 0;  // as passed to the game's window placement
RECT g_dest{};                       // where the frame lands in the client area
SIZE g_windowed_client{};            // last windowed client size, kept across Alt+Enter
bool g_copy = false;                 // swap effect COPY: Present may target a rectangle

const char* mode_name(Mode m) {
  return m == Mode::Borderless ? "borderless" : m == Mode::Windowed ? "windowed" : "fullscreen";
}

// --- the game's window placement ------------------------------------------------
// 0x4afa93 place_window(w, h): windowed ([0x69f530] == 0): style
// WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX, client w x h centred; fullscreen:
// WS_POPUP, HWND_TOPMOST. Called by the video mode setup with the mode size.
constexpr const char* kPlaceSig =
    "55 8B EC 83 EC 18 56 33 F6 39 35 ?? ?? ?? ?? 57 0F 85 ?? ?? ?? ?? 53 FF 75 0C 8D 45 E8 FF 75 08 "
    "56 56 50 FF 15 ?? ?? ?? ?? 68 00 02 00 00 56 68 00 00 CA 00";
constexpr size_t kPlaceHwndPush = 0xC0;  // FF 35 <&g_hwnd> (push [hwnd]) inside the function

LRESULT CALLBACK subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
extern WNDPROC g_game_proc;

using Place_t = void(__cdecl*)(int w, int h);
Place_t oPlace = nullptr;
HWND* g_game_hwnd = nullptr;

MONITORINFO monitor_of(HWND hwnd) {
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
  return mi;
}

DWORD windowed_style() {
  DWORD s = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  if (g_cfg.window_resize) s |= WS_THICKFRAME | WS_MAXIMIZEBOX;
  return s;
}

void apply(HWND hwnd) {
  if (!hwnd || g_mode == Mode::Fullscreen) return;
  DWORD visible = GetWindowLongA(hwnd, GWL_STYLE) & WS_VISIBLE;
  MONITORINFO mi = monitor_of(hwnd);
  if (g_mode == Mode::Borderless) {
    const RECT& m = mi.rcMonitor;
    SetWindowLongA(hwnd, GWL_STYLE, WS_POPUP | visible);
    SetWindowLongA(hwnd, GWL_EXSTYLE, 0);
    SetWindowPos(hwnd, HWND_NOTOPMOST, m.left, m.top, m.right - m.left, m.bottom - m.top,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    LOGI("display: borderless %ldx%ld at %ld,%ld", m.right - m.left, m.bottom - m.top, m.left, m.top);
    return;
  }
  const RECT& work = mi.rcWork;
  int ww = work.right - work.left, wh = work.bottom - work.top;
  int cw = g_windowed_client.cx, ch = g_windowed_client.cy;
  if (!cw || !ch) {
    if (g_cfg.window_w) {
      cw = g_cfg.window_w;
      ch = g_cfg.window_h;
    } else {  // the render size, shrunk to 90% of the work area if needed
      cw = g_render_w > 0 ? g_render_w : 1280;
      ch = g_render_h > 0 ? g_render_h : 720;
      double s = std::min(1.0, std::min(0.9 * ww / cw, 0.9 * wh / ch));
      cw = (int)(cw * s);
      ch = (int)(ch * s);
    }
  }
  DWORD style = windowed_style();
  RECT r{0, 0, cw, ch};
  AdjustWindowRectEx(&r, style, FALSE, 0);
  int w = r.right - r.left, h = r.bottom - r.top;
  int x = work.left + std::max(0, (ww - w) / 2), y = work.top + std::max(0, (wh - h) / 2);
  SetWindowLongA(hwnd, GWL_STYLE, style | visible);
  SetWindowLongA(hwnd, GWL_EXSTYLE, 0);
  SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_FRAMECHANGED);
  LOGI("display: windowed, client %dx%d at %d,%d%s", cw, ch, x, y, g_cfg.window_resize ? ", resizable" : "");
}

void __cdecl hkPlace(int w, int h) {
  LOGD("display: the game places its window for %dx%d", w, h);
  g_render_w = w;
  g_render_h = h;
  HWND hwnd = g_game_hwnd ? *g_game_hwnd : nullptr;
  if (!hwnd) {
    oPlace(w, h);
    return;
  }
  apply(hwnd);
  // Our own window hook, in place before the first frame (the overlay's comes
  // later): Alt+Enter, sizing, cursor and focus handling.
  if (!g_game_proc)
    g_game_proc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&subclass_proc)));
}

// --- the client size RenderWare sees -------------------------------------------
// RW's D3D9 driver compares the window's client size with the back buffer on
// every camera update and rebuilds the buffers to the window's size when they
// differ: a 1920x1080 frame in a 1664x936 window then no longer fits, the
// engine fails to start and the game quits (tmntnet 0.6, windowed test). So
// tmnt.exe's own GetClientRect import reports the frame size for the game
// window; D3D9, ImGui and Windows still see the real window.
BOOL WINAPI hkGetClientRect(HWND hwnd, LPRECT r) {
  BOOL ok = GetClientRect(hwnd, r);
  HWND game = g_game_hwnd ? *g_game_hwnd : nullptr;
  // Also when minimized: the game sizes things from this and is not ready for
  // 0x0 (it crashed at 0x51255b); RenderWare checks IsIconic on its own.
  if (ok && r && hwnd && hwnd == game) {
    UINT w = g_bb_w ? g_bb_w : (UINT)g_render_w, h = g_bb_h ? g_bb_h : (UINT)g_render_h;
    if (w && h) *r = RECT{0, 0, (LONG)w, (LONG)h};
  }
  return ok;
}

// RenderWare's camera begin (0x4c4530) and clear (0x4c582b) ask IsIconic and
// do nothing for a minimized window: the game's drawing then stops halfway,
// and drawing plays the game (a shadow's splash throws dice). In a game
// together one PC's window minimized (Win+D, the taskbar) parted the games
// (lab, 0.13.6: focus2, the first minimize; back1 and keep1 before it). In a
// session the game is told its window is not minimized: it draws on, into a
// window nobody sees.
BOOL WINAPI hkIsIconic(HWND hwnd) {
  HWND game = g_game_hwnd ? *g_game_hwnd : nullptr;
  if (hwnd && hwnd == game && lockstep::net_active()) return FALSE;
  return IsIconic(hwnd);
}

// Points tmnt.exe's import of user32!name at `hook`.
bool patch_import(const char* name, void* hook) {
  auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return false;
  for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
    if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "user32.dll") != 0) continue;
    auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk
                                                                                      : imp->FirstThunk));
    auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      auto by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (strcmp(reinterpret_cast<const char*>(by_name->Name), name) != 0) continue;
      uintptr_t target = reinterpret_cast<uintptr_t>(hook);
      return patch::write(reinterpret_cast<uintptr_t>(&slots->u1.Function), &target, sizeof(target));
    }
  }
  return false;
}

// --- helpers ------------------------------------------------------------------------

void keep_aspect(WPARAM edge, RECT* r) {
  if (!g_bb_w || !g_bb_h) return;
  RECT frame{0, 0, 0, 0};
  AdjustWindowRectEx(&frame, windowed_style(), FALSE, 0);
  int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
  int cw = (r->right - r->left) - fw, ch = (r->bottom - r->top) - fh;
  double aspect = double(g_bb_w) / g_bb_h;
  bool by_width = edge == WMSZ_LEFT || edge == WMSZ_RIGHT || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT ||
                  edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT;
  if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) by_width = false;
  if (by_width)
    ch = (int)(cw / aspect + 0.5);
  else
    cw = (int)(ch * aspect + 0.5);
  if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
    r->top = r->bottom - (ch + fh);
  else
    r->bottom = r->top + ch + fh;
  if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
    r->left = r->right - (cw + fw);
  else
    r->right = r->left + cw + fw;
}

void limit_fps() {
  static LARGE_INTEGER freq{}, next{};
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  LONGLONG period = freq.QuadPart / g_cfg.fps_limit;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (!next.QuadPart || now.QuadPart - next.QuadPart > period * 4) next = now;  // start / long stall
  for (;;) {
    QueryPerformanceCounter(&now);
    LONGLONG left = next.QuadPart - now.QuadPart;
    if (left <= 0) break;
    if (left * 1000 / freq.QuadPart >= 2)
      Sleep(1);
    else
      YieldProcessor();
  }
  next.QuadPart += period;
}

}  // namespace

void install(const Config& cfg) {
  g_cfg = cfg;
  g_mode = cfg.display == "borderless" ? Mode::Borderless : cfg.display == "windowed" ? Mode::Windowed
                                                                                       : Mode::Fullscreen;
  if (cfg.fps_limit) timeBeginPeriod(1);
  LOGI("display: %s%s%s%s", mode_name(g_mode), cfg.vsync < 0 ? "" : cfg.vsync ? ", vsync on" : ", vsync off",
       cfg.fps_limit ? (", fps limit " + std::to_string(cfg.fps_limit)).c_str() : "",
       g_mode != Mode::Fullscreen && cfg.background_run ? ", keeps running in the background" : "");
  if (g_mode == Mode::Fullscreen) {
    if (cfg.background_run) LOGW("display: background=run needs display=borderless or windowed; ignored");
    return;
  }
  uintptr_t a = patch::find("place_window", kPlaceSig);
  if (a && patch::read<uint8_t>(a + kPlaceHwndPush) == 0xFF && patch::read<uint8_t>(a + kPlaceHwndPush + 1) == 0x35) {
    g_game_hwnd = reinterpret_cast<HWND*>(patch::read<uint32_t>(a + kPlaceHwndPush + 2));
    if (!patch::in_image((uintptr_t)g_game_hwnd, 4) ||
        !patch::hook("place_window", a, (void*)&hkPlace, (void**)&oPlace)) {
      g_game_hwnd = nullptr;
    }
  }
  if (!g_game_hwnd) {
    LOGW("display: the game's window code was not recognised; staying in fullscreen");
    g_mode = Mode::Fullscreen;
    return;
  }
  if (!patch_import("GetClientRect", (void*)&hkGetClientRect)) {
    LOGW("display: cannot redirect the game's GetClientRect; staying in fullscreen");
    g_mode = Mode::Fullscreen;
    return;
  }
  if (!patch_import("IsIconic", (void*)&hkIsIconic))
    LOGW("display: cannot redirect the game's IsIconic; a minimized window parts a game together");
}

bool windowed_device() { return g_mode != Mode::Fullscreen; }

bool adjust_present(const D3DPRESENT_PARAMETERS& game, D3DPRESENT_PARAMETERS& out) {
  if (g_mode == Mode::Fullscreen && g_cfg.vsync < 0) return false;
  out = game;
  if (g_mode != Mode::Fullscreen) {
    out.Windowed = TRUE;
    out.FullScreen_RefreshRateInHz = 0;
    // Present with a destination rectangle (scaling, black bars) needs the
    // COPY swap effect, which allows one back buffer and no multisampling.
    g_copy = game.MultiSampleType == D3DMULTISAMPLE_NONE;
    if (g_copy) {
      out.SwapEffect = D3DSWAPEFFECT_COPY;
      out.BackBufferCount = 1;
    }
  }
  if (g_cfg.vsync >= 0)
    out.PresentationInterval = g_cfg.vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
  return true;
}

void on_device(HWND hwnd, UINT w, UINT h) {
  g_hwnd = hwnd;
  g_bb_w = w;
  g_bb_h = h;
  if (g_mode == Mode::Fullscreen) return;
  RECT c{};
  GetClientRect(hwnd, &c);
  LOGI("display: frame %ux%u shown in a %ldx%ld %s window", w, h, c.right, c.bottom, mode_name(g_mode));
}

const RECT* before_present(HWND hwnd, const RECT* game_dst) {
  if (g_cfg.fps_limit) limit_fps();
  if (g_mode == Mode::Fullscreen || game_dst || !g_copy || !hwnd || !g_bb_w || !g_bb_h) return game_dst;
  RECT c{};
  if (!GetClientRect(hwnd, &c) || c.right <= 0 || c.bottom <= 0) return game_dst;
  // Fit the frame into the client area, keeping its aspect.
  // Always an explicit rectangle: D3D9 itself must not ask for the client size.
  double s = std::min(double(c.right) / g_bb_w, double(c.bottom) / g_bb_h);
  int w = (int)(g_bb_w * s + 0.5), h = (int)(g_bb_h * s + 0.5);
  if (std::abs(w - c.right) <= 1 && std::abs(h - c.bottom) <= 1) {
    g_dest = c;
    return &g_dest;  // fills the window
  }
  g_dest = RECT{(c.right - w) / 2, (c.bottom - h) / 2, 0, 0};
  g_dest.right = g_dest.left + w;
  g_dest.bottom = g_dest.top + h;
  // D3D only draws the frame: paint the bars black.
  if (HDC dc = GetDC(hwnd)) {
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT bars[4] = {{0, 0, c.right, g_dest.top},
                    {0, g_dest.bottom, c.right, c.bottom},
                    {0, g_dest.top, g_dest.left, g_dest.bottom},
                    {g_dest.right, g_dest.top, c.right, g_dest.bottom}};
    for (const RECT& b : bars)
      if (b.right > b.left && b.bottom > b.top) FillRect(dc, &b, black);
    ReleaseDC(hwnd, dc);
  }
  return &g_dest;
}

namespace {

// The game goes on by itself (its frames and pictures), whatever its window
// is told: with [video] background=run, while it is still loading (before the
// main menu first shows), and in a game together (the other PCs wait for this
// one's every frame).
bool goes_on_by_itself() {
  static bool menu_seen = false;
  menu_seen = menu_seen || main_menu::on_screen();
  return g_cfg.background_run || !menu_seen || lockstep::net_active();
}

bool filter(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
  // WM_PAINT: the game answers with a whole picture drawn outside its frame
  // (0x4afcf6: BeginPaint, 0x4ac4b0, EndPaint). A picture plays the game here
  // (a shadow on water throws dice for a splash, 0x418a98): one PC's window
  // repainted (uncovered, restored, the screen's mode changed) and the games
  // of a session were no longer the same (lab, 0.13.5: paint1, keep1). While
  // the game loads, that picture crashed (0x51255b: no font yet). The game
  // draws its next frame anyway: the window is only told it is painted.
  if (msg == WM_PAINT && goes_on_by_itself()) {
    ValidateRect(hwnd, nullptr);
    *result = 0;
    return true;
  }
  if (g_mode == Mode::Fullscreen) return false;
  switch (msg) {
    case WM_SYSKEYDOWN:  // Alt+Enter: windowed <-> borderless (the game ignores it)
      if (wp == VK_RETURN && (lp & (1 << 29)) && !(lp & (1 << 30))) {
        if (g_mode == Mode::Windowed) {
          RECT c{};
          GetClientRect(hwnd, &c);
          if (!IsZoomed(hwnd)) g_windowed_client = SIZE{c.right, c.bottom};
          if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        }
        g_mode = g_mode == Mode::Windowed ? Mode::Borderless : Mode::Windowed;
        LOGI("display: Alt+Enter -> %s", mode_name(g_mode));
        apply(hwnd);
        *result = 0;
        return true;
      }
      return false;
    case WM_SYSCHAR:  // no "ding" for Alt+Enter
      if (wp == VK_RETURN) {
        *result = 0;
        return true;
      }
      return false;
    case WM_SETCURSOR:  // the game hides the cursor everywhere, title bar included
      if (LOWORD(lp) != HTCLIENT) {
        *result = DefWindowProcA(hwnd, msg, wp, lp);
        return true;
      }
      return false;
    case WM_SIZING:
      if (g_mode == Mode::Windowed && g_cfg.window_resize) {
        keep_aspect(wp, reinterpret_cast<RECT*>(lp));
        *result = TRUE;
        return true;
      }
      return false;
    case WM_ACTIVATE: {  // the game pauses (waits for messages) while inactive
      // Before the main menu first shows, the game is still loading: pausing
      // it then crashed in a window (0x51255b after minimize + restore).
      // In a game together the other PC waits for this one's every frame.
      bool keep_running = goes_on_by_itself();
      LOGD("display: window %s%s", LOWORD(wp) == WA_INACTIVE ? "inactive" : "active",
           keep_running && LOWORD(wp) == WA_INACTIVE ? ", the game keeps running" : "");
      if (keep_running && LOWORD(wp) == WA_INACTIVE) {
        *result = DefWindowProcA(hwnd, msg, wp, lp);
        return true;
      }
      return false;
    }
    default:
      return false;
  }
}

WNDPROC g_game_proc = nullptr;

LRESULT CALLBACK subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  LRESULT r = 0;
  if (filter(hwnd, msg, wp, lp, &r)) return r;
  return CallWindowProcA(g_game_proc, hwnd, msg, wp, lp);
}

}  // namespace

LPARAM map_mouse(LPARAM lp) {
  if (g_mode == Mode::Fullscreen || !g_bb_w) return lp;
  int dw = g_dest.right - g_dest.left, dh = g_dest.bottom - g_dest.top;
  if (dw <= 0 || dh <= 0) return lp;
  int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
  x = (int)((x - g_dest.left) * (double)g_bb_w / dw);
  y = (int)((y - g_dest.top) * (double)g_bb_h / dh);
  return MAKELPARAM((WORD)(short)x, (WORD)(short)y);
}

bool backbuffer_size(float& w, float& h) {
  if (g_mode == Mode::Fullscreen || !g_bb_w) return false;
  w = (float)g_bb_w;
  h = (float)g_bb_h;
  return true;
}

bool fake_unfocused_keyboard() {
  HWND hwnd = g_hwnd;
  return g_mode != Mode::Fullscreen && (g_cfg.background_run || lockstep::net_active()) && hwnd &&
         GetForegroundWindow() != hwnd;
}

int mode() { return g_mode == Mode::Borderless ? 1 : g_mode == Mode::Windowed ? 2 : 0; }

bool switch_mode(int m) {
  Mode want = m == 1 ? Mode::Borderless : m == 2 ? Mode::Windowed : Mode::Fullscreen;
  if (want == g_mode) return true;
  if (g_mode == Mode::Fullscreen || want == Mode::Fullscreen) return false;  // the device must change
  HWND hwnd = g_hwnd;
  if (g_mode == Mode::Windowed && hwnd) {
    RECT c{};
    GetClientRect(hwnd, &c);
    if (!IsZoomed(hwnd) && c.right > 0) g_windowed_client = SIZE{c.right, c.bottom};
    if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
  }
  g_mode = want;
  apply(hwnd);
  return true;
}

void set_window_size(int w, int h) {
  g_cfg.window_w = w;
  g_cfg.window_h = h;
  g_windowed_client = SIZE{w, h};
  if (g_mode == Mode::Windowed) apply(g_hwnd);
}

void set_window_resize(bool on) {
  g_cfg.window_resize = on;
  if (g_mode != Mode::Windowed) return;
  HWND hwnd = g_hwnd;
  RECT c{};
  if (hwnd && GetClientRect(hwnd, &c) && c.right > 0) g_windowed_client = SIZE{c.right, c.bottom};
  apply(hwnd);
}

void set_background_run(bool on) { g_cfg.background_run = on; }

void set_fps_limit(int fps) {
  if (fps && !g_cfg.fps_limit) timeBeginPeriod(1);
  g_cfg.fps_limit = fps;
}

HWND window() { return g_hwnd; }

}  // namespace display
