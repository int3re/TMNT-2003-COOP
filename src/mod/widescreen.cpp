#include "mod/widescreen.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <intrin.h>
#include <cstdio>
#include <cstring>
#include <vector>

#include "common/log.h"
#include "common/net.h"
#include "mod/ambient.h"
#include "mod/four_hud.h"
#include "mod/main_menu.h"
#include "mod/overlay.h"
#include "mod/four_players.h"
#include "mod/nametags.h"
#include "mod/own_camera.h"
#include "mod/patch.h"

namespace widescreen {
namespace {

constexpr float kAspect43 = 4.0f / 3.0f;
Config g_cfg;

// --- 1. Video mode list --------------------------------------------------------
// The list builder (0x4b02a0) keeps modes with w >= 640, h >= 480 and then:
//   if (!g_highreso) { w <= 1024 && h <= 768 }   ; "-highreso" command line
//   w <= 1600 && h <= 1200 && w*3 == h*4         ; 4:3 only
constexpr const char* kModeFilterSig =
    "8B 45 D0 3D 80 02 00 00 0F 8C ?? ?? ?? ?? 8B 4D D4 81 F9 E0 01 00 00 0F 8C ?? ?? ?? ?? "
    "83 3D ?? ?? ?? ?? 00 75 ?? 3D 00 04 00 00 0F 8F ?? ?? ?? ?? 81 F9 00 03 00 00 0F 8F ?? ?? "
    "?? ?? 3D 40 06 00 00 7F ?? 81 F9 B0 04 00 00 7F ?? C1 E1 02 8D 04 40 3B C1 75 ??";

void patch_mode_filter() {
  uintptr_t a = patch::find("video_mode_filter", kModeFilterSig);
  if (!a) return;
  const uint8_t jmp = 0xEB;                // jne -> jmp: always take the -highreso path
  const uint32_t max_dim = 8192;           // was 1600 / 1200
  const uint8_t nops[2] = {0x90, 0x90};    // drop the w*3 == h*4 test
  bool ok = patch::write(a + 36, &jmp, 1) && patch::write(a + 62, &max_dim, 4) &&
            patch::write(a + 70, &max_dim, 4) && patch::write(a + 84, nops, 2);
  LOGI("video mode list: widescreen and modes up to 8192 allowed%s", ok ? "" : " (write FAILED)");
}

// --- 2. Mode selection ------------------------------------------------------------
// select_mode(w, h, depth) (0x4af77c) picks the first listed mode with
// mode.w >= w && mode.h >= h && mode.depth == depth and marks it current.
constexpr const char* kSelectModeSig =
    "8B 0D ?? ?? ?? ?? 69 C9 A8 00 00 00 03 0D ?? ?? ?? ?? 53 8B 5C 24 10 56 8B 71 54";

#pragma pack(push, 1)
struct ModeEntry {  // 0x60 bytes: RwVideoMode + RW mode index + label
  int32_t w, h, depth, flags, refresh, format;
  uint8_t rest[0x48];
};
#pragma pack(pop)
static_assert(sizeof(ModeEntry) == 0x60, "game layout");

using SelectMode_t = int(__cdecl*)(int w, int h, int depth);
SelectMode_t oSelectMode = nullptr;
const int32_t* g_adapter_index = nullptr;
uint8_t* const* g_adapters = nullptr;  // -> array of 0xA8-byte adapter records
int g_want_w = 0, g_want_h = 0, g_pref_hz = 60;

int __cdecl hkSelectMode(int w, int h, int depth) {
  int rw = g_want_w ? g_want_w : w, rh = g_want_h ? g_want_h : h;
  int r = oSelectMode(rw, rh, depth);
  if (r >= 0 && g_adapters && *g_adapters) {
    uint8_t* ad = *g_adapters + *g_adapter_index * 0xA8;
    auto* modes = *reinterpret_cast<ModeEntry**>(ad + 0x50);
    int count = *reinterpret_cast<int32_t*>(ad + 0x54);
    int& cur = *reinterpret_cast<int32_t*>(ad + 0x58);
    if (modes && cur >= 0 && cur < count) {
      // The game takes the first mode at least as big; an exact match wins
      // (1280x720 asked, 1280x800 listed first).
      for (int i = 0; i < count; ++i)
        if (modes[i].w == rw && modes[i].h == rh && modes[i].depth == modes[cur].depth) {
          cur = i;
          break;
        }
      // Same size and depth may be listed once per refresh rate: prefer
      // [video] refresh (60 Hz, what the game was made for), else the highest.
      const ModeEntry& c = modes[cur];
      int best = cur;
      char rates[64] = "";
      for (int i = 0; i < count; ++i) {
        const ModeEntry& m = modes[i];
        if (m.w != c.w || m.h != c.h || m.depth != c.depth) continue;
        char rate[12];
        snprintf(rate, sizeof(rate), rates[0] ? ",%d" : "%d", m.refresh);
        strncat(rates, rate, sizeof(rates) - strlen(rates) - 1);
        const ModeEntry& b = modes[best];
        bool better = (m.refresh == g_pref_hz && b.refresh != g_pref_hz) ||
                      (b.refresh != g_pref_hz && m.refresh > b.refresh);
        if (better) best = i;
      }
      cur = best;
      LOGI("video mode: game asked %dx%dx%d, using %dx%dx%d @%d Hz (listed rates: %s)", w, h,
           depth, modes[cur].w, modes[cur].h, modes[cur].depth, modes[cur].refresh, rates);
    }
  } else if (r < 0) {
    LOGW("video mode %dx%dx%d not available, game falls back to windowed", rw, rh, depth);
  }
  return r;
}

// --- 3. Hor+ camera, the wider view -------------------------------------------------
// RwCameraSetViewWindow (0x4bdad0). The game sets the main camera's window to
// (x, 0.75x); on a wider frame buffer we keep y and widen x. [video] fov then
// widens the main camera's window both ways: what is shown only. The game's
// own reckoning of the view (its x at 0x61cf00: what is off the screen,
// 0x4a2b98) stays as it was, so players with different views stay in step.
constexpr const char* kSetViewWindowSig =
    "8B 44 24 08 56 8B 74 24 08 8B 08 D9 05 ?? ?? ?? ?? 89 4E 68 8B 50 04 D8 76 68 89 56 6C";
constexpr size_t kCameraFrameBuffer = 0x60;  // RwCamera::frameBuffer
constexpr size_t kCameraViewWindow = 0x68;   // RwCamera::viewWindow
constexpr size_t kRasterWidth = 0x0C, kRasterHeight = 0x10;
// The game's view setter 0x4a271c(x): window (x, 0.75x) of the main camera,
// push dword [main camera] at +0x20.
constexpr const char* kViewSetterSig =
    "55 8B EC 51 51 8B 45 08 D9 45 08 D8 0D ?? ?? ?? ?? A3 ?? ?? ?? ?? 89 45 F8 8D 45 F8 50 D9 5D FC FF 35 ?? ?? ?? ?? "
    "E8";
constexpr size_t kViewSetterCamera = 0x22;

struct RwV2d {
  float x, y;
};
using SetViewWindow_t = void*(__cdecl*)(void* camera, const RwV2d* vw);
SetViewWindow_t oSetViewWindow = nullptr;
void* const* g_main_camera = nullptr;
void* g_view_camera = nullptr;  // the main camera as last set, with the window the game asked
RwV2d g_view_asked{}, g_view_set{};

bool raster_size(const uint8_t* raster, int& w, int& h) {
  if (!raster) return false;
  w = *reinterpret_cast<const int32_t*>(raster + kRasterWidth);
  h = *reinterpret_cast<const int32_t*>(raster + kRasterHeight);
  return w > 0 && h > 0;
}

void install_im2d();

// The window a camera gets for the one the game asks: wider on a wide frame
// buffer (Hor+), the main camera's then scaled by [video] fov.
RwV2d shown(void* camera, const RwV2d& asked, bool log) {
  RwV2d v = asked;
  int w, h;
  if (raster_size(*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(camera) + kCameraFrameBuffer), w, h)) {
    float aspect = float(w) / float(h);
    if (g_cfg.widescreen && aspect > kAspect43 + 0.01f && std::fabs(asked.x / asked.y - kAspect43) < 0.01f) {
      v = RwV2d{asked.y * aspect, asked.y};
      static int logged_w = 0, logged_h = 0;
      if (log && (logged_w != w || logged_h != h)) {
        logged_w = w;
        logged_h = h;
        LOGI("Hor+ camera on %dx%d: view window %.3f x %.3f -> %.3f x %.3f", w, h, asked.x, asked.y, v.x, v.y);
      }
    }
  }
  if (g_main_camera && camera == *g_main_camera && g_cfg.fov != 100) {
    float k = g_cfg.fov / 100.0f;
    v.x *= k, v.y *= k;
  }
  return v;
}

void* __cdecl hkSetViewWindow(void* camera, const RwV2d* vw) {
  install_im2d();  // RW engine is up once cameras are configured
  if (!camera || !vw || vw->x <= 0 || vw->y <= 0) return oSetViewWindow(camera, vw);
  if (own_camera::working_out()) return camera;  // (a player's own view: the camera itself stays the game's)
  RwV2d v = shown(camera, *vw, true);
  if (g_main_camera && camera == *g_main_camera) {
    g_view_camera = camera;
    g_view_asked = *vw;
    // In a game together the game reckons with one view on every PC (a 16:9
    // picture of what it asked), whatever this screen shows: the picture
    // gets its real window when it is drawn (own_camera.cpp).
    if (own_camera::same_view_for_all()) {
      v = *vw;
      if (std::fabs(vw->x / vw->y - kAspect43) < 0.01f) v.x = vw->y * (16.0f / 9.0f);
    }
    g_view_set = v;
  }
  return oSetViewWindow(camera, &v);
}

// --- 3b. Text in a box -----------------------------------------------------------
// 0x4a72cb(text, height, box[4], align) writes game text with the RenderWare
// 2D toolkit (Rt2dFontFlow, Im3D through its own camera), not with Im2D: it
// was laid out across the full width while the dialog picture around it was
// pillarboxed. Drawn into a 4:3 viewport it sits where the 4:3 game had it.
constexpr const char* kTextInBoxSig =
    "55 8B EC A1 ?? ?? ?? ?? 56 FF 75 08 FF 90 20 01 00 00 05 80 00 00 00 50 E8";
using TextInBox_t = void(__cdecl*)(const char* text, float height, const float* box, int align);
TextInBox_t oTextInBox = nullptr;
uintptr_t g_text_in_box = 0;  // the game's function (hooked: calls go through hkTextInBox)
// The font it writes with ([0x625970], pushed at +0x4A: FF 35 <address>). The
// game lets it go on its way out and may still draw a picture with text in
// it: the first load's message, when the window is closed in the game's first
// seconds, read through 0 at 0x51255b (lab, 0.13.5: every time).
constexpr size_t kTextFontPush = 0x4A;
const uint32_t* g_text_font = nullptr;
bool g_key_table = false;     // the key table is up: its oversize frame is not a picture

bool screen_size(int& W, int& H);

// Lab ([debug] text_trace=1): each text the game writes, once: its bytes as they are (the Russian
// version's fonts have Russian letters at Latin codes), where it goes and who asked.
void trace_text(const char* text, float height, const float* box, const void* caller) {
  static uint32_t seen[512];
  static int n = 0;
  uint32_t h = 2166136261u;
  for (const char* c = text; *c && c - text < 200; ++c) h = (h ^ (uint8_t)*c) * 16777619u;
  h ^= (uint32_t)(uintptr_t)caller;
  for (int i = 0; i < n; ++i)
    if (seen[i] == h) return;
  if (n >= 512) return;
  seen[n++] = h;
  char out[400];
  size_t at = 0;
  for (const char* c = text; *c && at + 5 < sizeof(out) && c - text < 120; ++c) {
    const uint8_t b = (uint8_t)*c;
    if (b >= 32 && b < 127) out[at++] = (char)b;
    else at += std::snprintf(out + at, sizeof(out) - at, "<%02X>", b);
  }
  out[at] = 0;
  LOGI("text: \"%s\" height %.1f box (%.0f %.0f %.0f %.0f) from %p", out, height, box ? box[0] : 0, box ? box[1] : 0,
       box ? box[2] : 0, box ? box[3] : 0, caller);
}

void __cdecl hkTextInBox(const char* text, float height, const float* box, int align) {
  if (g_text_font && !*g_text_font) return;  // (no font: nothing to write with)
  if (g_cfg.debug_text_trace && text) trace_text(text, height, box, _ReturnAddress());
  int W, H;
  IDirect3DDevice9* dev = overlay::device();
  D3DVIEWPORT9 vp{};
  bool squeeze = (g_cfg.hud_center || g_cfg.fullscreen_pillarbox) && dev && screen_size(W, H) &&
                 W > H * kAspect43 + 1.0f && SUCCEEDED(dev->GetViewport(&vp));
  if (squeeze) {
    DWORD w43 = (DWORD)(vp.Height * kAspect43 + 0.5f);
    squeeze = w43 + 1 < vp.Width;
    if (squeeze) {
      D3DVIEWPORT9 nv = vp;
      nv.X = vp.X + (vp.Width - w43) / 2;
      nv.Width = w43;
      dev->SetViewport(&nv);
    }
  }
  oTextInBox(text, height, box, align);
  if (squeeze) dev->SetViewport(&vp);
}

// --- 4. Im2D (screen-space 2D) --------------------------------------------------
// Most of the HUD is drawn with Im3D relative to the camera and follows the
// Hor+ change by itself. The rest goes through RwIm2D in pixels computed as
// x * width/640: squeeze those into the 4:3 centre. Full-width quads are fades
// (untextured: keep full width) or pictures/movies (textured: pillarbox).
constexpr const char* kIm2DPrimSig = "A1 ?? ?? ?? ?? FF 60 30";  // RwIm2DRenderPrimitive
constexpr size_t kDevRenderStateGet = 0x24, kDevIm2DLine = 0x28, kDevIm2DTri = 0x2C,
                 kDevIm2DPrim = 0x30, kDevIm2DIdx = 0x34;  // RwGlobals::dOpenDevice
constexpr int kRenderStateTextureRaster = 1;

struct Im2DVertex {
  float x, y, z, rhw;
  uint32_t color;
  float u, v;
};
static_assert(sizeof(Im2DVertex) == 28, "RW D3D9 Im2D vertex");

using RSGet_t = int(__cdecl*)(int state, void* value);
using Im2DLine_t = int(__cdecl*)(void* v, int n, int a, int b);
using Im2DTri_t = int(__cdecl*)(void* v, int n, int a, int b, int c);
using Im2DPrim_t = int(__cdecl*)(int type, void* v, int n);
using Im2DIdx_t = int(__cdecl*)(int type, void* v, int n, void* idx, int nidx);

uint8_t* const* g_engine = nullptr;  // &RwEngineInstance
Im2DLine_t oLine = nullptr;
Im2DTri_t oTri = nullptr;
Im2DPrim_t oPrim = nullptr;
Im2DIdx_t oIdx = nullptr;
std::vector<Im2DVertex> g_buf;

bool screen_size(int& w, int& h) {
  uint8_t* engine = g_engine ? *g_engine : nullptr;
  uint8_t* camera = engine ? *reinterpret_cast<uint8_t**>(engine) : nullptr;  // curCamera
  return camera && raster_size(*reinterpret_cast<uint8_t**>(camera + kCameraFrameBuffer), w, h);
}

bool texture_bound() {
  auto get = *reinterpret_cast<RSGet_t*>(*g_engine + kDevRenderStateGet);
  void* raster = nullptr;
  return get && get(kRenderStateTextureRaster, &raster) && raster;
}

void* raster_bound() {
  auto get = *reinterpret_cast<RSGet_t*>(*g_engine + kDevRenderStateGet);
  void* raster = nullptr;
  if (get) get(kRenderStateTextureRaster, &raster);
  return raster;
}

// [debug] log_2d: the 2D primitives of one main menu frame, as the game sends
// them and as they leave the squeeze.
void log_2d(const void* in, const void* out, int n) {
  static int left = 0;
  static uint32_t menu_since = 0, start = net::now_ms();
  static bool done = false;
  if (done || !g_cfg.debug_log_2d) return;
  uint32_t now = net::now_ms();
  if (g_cfg.debug_log_2d_at > 0) {
    if (!left) {
      if (now - start < (uint32_t)g_cfg.debug_log_2d_at) return;
      left = 800;
      LOGI("2D log: the frame at %d ms follows", g_cfg.debug_log_2d_at);
    }
  } else {
    if (!main_menu::on_screen()) {
      menu_since = 0;
      return;
    }
    if (!menu_since) menu_since = now;
    if (!left) {
      if (now - menu_since < 3000) return;
      left = 150;
      LOGI("2D log: main menu frame follows");
    }
  }
  auto box = [n](const void* p, float& x0, float& x1, float& y0, float& y1) {
    auto* v = static_cast<const Im2DVertex*>(p);
    x0 = x1 = v[0].x;
    y0 = y1 = v[0].y;
    for (int i = 1; i < n; ++i) {
      x0 = std::min(x0, v[i].x), x1 = std::max(x1, v[i].x);
      y0 = std::min(y0, v[i].y), y1 = std::max(y1, v[i].y);
    }
  };
  float a0, a1, b0, b1, c0, c1, d0, d1;
  box(in, a0, a1, b0, b1);
  box(out, c0, c1, d0, d1);
  LOGI("2D n=%d tex=%p a=%u x %.1f..%.1f y %.1f..%.1f -> x %.1f..%.1f", n, raster_bound(),
       static_cast<const Im2DVertex*>(in)[0].color >> 24, a0, a1, b0, b1, c0, c1);
  if (--left == 0) done = true;
}

void* adjust_impl(void* verts, int n);

// Bounds of this frame's textured, squeezed 2D, and how much of the screen
// its pieces would cover (reset at Present).
struct Bounds {
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
  float area = 0;
} g_tiles;

IDirect3DTexture9* g_override = nullptr;
int g_edge = 0;          // the next 2D is a corner piece of the HUD: -1 left, 1 right
char g_sprite[33] = "";  // the texture of the game's sprite being drawn ("": 2D of another kind)

// The wide menus. A menu's background is a 4:3 picture across the screen; the
// ones that are a pattern go on to the screen's edges: drawn in the 4:3 middle
// as before, then once more into each side bar, so that nothing is stretched:
// - mirrored at the 4:3 edge (the main menu and its submenus: menu_bg; the
//   options: op_bg; others named ..._bg): the pattern meets itself;
// - its edge column drawn out sideways (the level select: ss_bg, bands and
//   lines that run level, and its title's letters are in the picture: mirrored
//   they would stand at the side backwards).
// Such a frame gets no side bars, and what slides in from beyond the screen
// is not cut at the 4:3 edges. Pictures with drawn figures cut by their edges
// (the character select: csel_bg; the title, the logos, movies) keep 4:3 and
// the bars.
enum class Wide { No, Mirror, Edge };
Wide g_extend = Wide::No;  // this primitive: on to the sides after it
bool g_extended = false;   // a background went to the sides in this frame

Wide wide_background(const char* name) {
  if (!g_cfg.wide_menus || !name || !name[0]) return Wide::No;
  if (!strcmp(name, "csel_bg")) return Wide::No;
  if (!strcmp(name, "ss_bg")) return Wide::Edge;
  size_t len = strlen(name);
  return len > 3 && !strcmp(name + len - 3, "_bg") ? Wide::Mirror : Wide::No;
}

// A picture across the whole width, once a name: what the screens are made of.
void tell_picture(const char* what, float miny, float maxy, int H) {
  static std::string told;
  char name[40];
  if (g_sprite[0])
    snprintf(name, sizeof(name), "%s", g_sprite);
  else
    snprintf(name, sizeof(name), "(no sprite) %p", raster_bound());
  std::string key = std::string("|") + name + "|";
  if (told.size() > 3000 || told.find(key) != std::string::npos) return;
  told += key;
  int rw = 0, rh = 0;
  raster_size(static_cast<uint8_t*>(raster_bound()), rw, rh);
  LOGI("widescreen: picture '%s' across the screen (%dx%d, y %.0f..%.0f of %d): %s", name, rw, rh, miny, maxy, H, what);
}
float g_box_left = 0;    // the last squeezed primitive's 4:3 box (its left edge), for Clip43

// Draws one primitive with tmntnet's texture in place of the game's.
class TexOverride {
 public:
  TexOverride() {
    if (!g_override || !(dev_ = overlay::device())) return;
    dev_->GetTexture(0, &prev_);
    dev_->SetTexture(0, g_override);
  }
  ~TexOverride() {
    if (!dev_) return;
    dev_->SetTexture(0, prev_);
    if (prev_) prev_->Release();
  }

 private:
  IDirect3DDevice9* dev_ = nullptr;
  IDirect3DBaseTexture9* prev_ = nullptr;
};

// A level is on the screen: its world was drawn in this frame and a player
// has a turtle in it. Then nothing 2D is "a picture of the whole screen": the
// side bars would cover the level and the panels in the corners. (The panels
// alone do not tell: a real game, 0.13.2, a boss -- the bars over the level.)
bool play_screen() {
  if (four_hud::panels_shown()) return true;
  if (!nametags::world_drawn_now()) return false;
  for (int p = 0; p < 4; ++p)
    if (four::position(p)) return true;
  return false;
}

void* adjust(void* verts, int n) {
  void* out = adjust_impl(verts, n);
  if (verts && n > 0) log_2d(verts, out, n);
  return out;
}

void* adjust_impl(void* verts, int n) {
  int W, H;
  if (!verts || n <= 0 || !screen_size(W, H)) return verts;
  float w43 = H * kAspect43;
  if (W <= w43 + 1.0f) return verts;
  auto* v = static_cast<Im2DVertex*>(verts);
  float minx = v[0].x, maxx = v[0].x;
  for (int i = 1; i < n; ++i) {
    minx = v[i].x < minx ? v[i].x : minx;
    maxx = v[i].x > maxx ? v[i].x : maxx;
  }
  bool full_width = minx <= 2.0f && maxx >= W - 2.0f;
  bool textured = texture_bound();
  bool frame = false;  // the key table's frame (0, -14, 512x512): fitted like its words
  if (g_key_table && full_width) {
    float miny = v[0].y, maxy = v[0].y;
    for (int i = 1; i < n; ++i) miny = std::min(miny, v[i].y), maxy = std::max(maxy, v[i].y);
    frame = miny < -1.0f && maxy > H + 1.0f;
    if (frame) full_width = false;
  }
  if (full_width ? !(g_cfg.fullscreen_pillarbox && textured) : !g_cfg.hud_center)
    return verts;
  if (full_width) {
    float y0 = v[0].y, y1 = v[0].y;
    for (int i = 1; i < n; ++i) y0 = std::min(y0, v[i].y), y1 = std::max(y1, v[i].y);
    if (wide_background(g_sprite) == Wide::No) tell_picture("4:3", y0, y1, H);
    // A picture of the whole screen (menus, movies, loading) gets its side bars
    // filled. A band across (a dialogue's box, 1024x256 at the bottom) or
    // anything while the players' panels are up is drawn into 4:3 the same way,
    // but the sides stay the world's: filled, they covered the level and the
    // panels in the corners (a real game, 0.11.10: "black bars" in a dark boss
    // room, the life bars hidden).
    float miny = v[0].y, maxy = v[0].y;
    for (int i = 1; i < n; ++i) miny = std::min(miny, v[i].y), maxy = std::max(maxy, v[i].y);
    bool whole = miny <= 2.0f && maxy >= H - 2.0f;
    const Wide wide = whole && !play_screen() ? wide_background(g_sprite) : Wide::No;
    if (wide != Wide::No) {
      g_extend = wide;  // (the hook draws its sides)
      g_extended = true;
      tell_picture(wide == Wide::Mirror ? "a menu's background: 4:3 and mirrored on to both sides"
                                        : "a menu's background: 4:3 and its edges drawn out to both sides",
                   miny, maxy, H);
    } else if (whole && !play_screen()) {
      if (!g_extended) ambient::note_pillarbox();  // the side bars of this frame get filled
    } else {
      static void* told[16];
      static int n_told = 0;
      void* r = raster_bound();
      if (std::find(told, told + n_told, r) == told + n_told && n_told < 16) {
        told[n_told++] = r;
        int rw = 0, rh = 0;
        raster_size(static_cast<uint8_t*>(r), rw, rh);
        LOGI("widescreen: a full-width picture (raster %p, %dx%d, y %.0f..%.0f of %d%s): 4:3, the sides stay the world's",
             r, rw, rh, miny, maxy, H, whole ? ", over the play screen" : "");
      }
    }
  }
  if (!full_width && textured && !frame) {
    // A full-screen picture made of tiles (the title screen: 4 x 512x512)
    // fills the bars too, once its tiles reach all four corners and cover
    // the screen. The play screen's own pieces can stand in all four corners
    // as well (a boss: the player's panel, the boss's bar and face, the
    // second player's words -- a real game, 0.13.2: the bars over the level
    // and the panel); they cover little of it, and the panels are up.
    float miny = v[0].y, maxy = v[0].y;
    for (int i = 1; i < n; ++i) {
      miny = v[i].y < miny ? v[i].y : miny;
      maxy = v[i].y > maxy ? v[i].y : maxy;
    }
    g_tiles.x0 = std::min(g_tiles.x0, minx), g_tiles.x1 = std::max(g_tiles.x1, maxx);
    g_tiles.y0 = std::min(g_tiles.y0, miny), g_tiles.y1 = std::max(g_tiles.y1, maxy);
    g_tiles.area += std::max(0.0f, std::min(maxx, (float)W) - std::max(minx, 0.0f)) *
                    std::max(0.0f, std::min(maxy, (float)H) - std::max(miny, 0.0f));
    if (g_tiles.x0 <= 2.0f && g_tiles.x1 >= W - 2.0f && g_tiles.y0 <= 2.0f && g_tiles.y1 >= H - 2.0f) {
      const bool covers = g_tiles.area >= 0.85f * (float)W * (float)H;
      const bool play = play_screen();
      if (covers && !play) {
        if (!g_extended) ambient::note_pillarbox();
      } else {
        static bool told = false;
        if (!told && play) {
          told = true;
          LOGI("widescreen: 2D pieces from corner to corner over the play screen (%.0f%% of it): not a picture of the "
               "whole screen, the sides stay the world's",
               g_tiles.area * 100.0f / ((float)W * (float)H));
        }
      }
    }
  }
  float s = w43 / W, o = (W - w43) * 0.5f;
  if (!full_width && g_edge && g_cfg.hud_corners) o = g_edge < 0 ? 0.0f : W - w43;  // at its edge
  g_box_left = o;
  g_buf.assign(v, v + n);
  for (auto& p : g_buf) p.x = p.x * s + o;
  return g_buf.data();
}

// A squeezed primitive is clipped to the 4:3 area, as the original was by the
// screen edge: menus slide text in from beyond it (character select), and
// that must not show up in the side bars.
class Clip43 {
 public:
  Clip43(bool on) {
    int W, H;
    if (!on || !screen_size(W, H)) return;
    dev_ = overlay::device();
    if (!dev_ || FAILED(dev_->GetRenderState(D3DRS_SCISSORTESTENABLE, &old_on_)) ||
        FAILED(dev_->GetScissorRect(&old_rect_))) {
      dev_ = nullptr;
      return;
    }
    LONG w43 = (LONG)(H * kAspect43 + 0.5f), x0 = (LONG)(g_box_left + 0.5f);
    RECT r{x0, 0, x0 + w43, H};
    dev_->SetScissorRect(&r);
    dev_->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
  }
  ~Clip43() {
    if (!dev_) return;
    dev_->SetScissorRect(&old_rect_);
    dev_->SetRenderState(D3DRS_SCISSORTESTENABLE, old_on_);
  }

 private:
  IDirect3DDevice9* dev_ = nullptr;
  DWORD old_on_ = FALSE;
  RECT old_rect_{};
};

// A menu's background is drawn into the 4:3 box, then into each side bar:
// mirrored at the box's edge, or its edge column drawn out. The texture must
// not wrap meanwhile: across the seam the picture's other edge would bleed in
// (a thin line down the screen).
class NoWrap {
 public:
  explicit NoWrap(bool on) {
    if (!on || !(dev_ = overlay::device())) return;
    dev_->GetSamplerState(0, D3DSAMP_ADDRESSU, &old_);
    dev_->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  }
  ~NoWrap() {
    if (dev_) dev_->SetSamplerState(0, D3DSAMP_ADDRESSU, old_);
  }

 private:
  IDirect3DDevice9* dev_ = nullptr;
  DWORD old_ = D3DTADDRESS_WRAP;
};

template <typename Draw>
void draw_sides(const void* squeezed, int n, Wide how, Draw draw) {
  int W, H;
  IDirect3DDevice9* dev = overlay::device();
  if (!dev || n <= 0 || !screen_size(W, H)) return;
  const Im2DVertex* in = static_cast<const Im2DVertex*>(squeezed);
  static std::vector<Im2DVertex> side;
  side.assign(in, in + n);
  // The picture's own edges: the sides start exactly there (a shared edge
  // leaves no crack, with anti-aliasing too).
  float minx = in[0].x, maxx = in[0].x, u_min = in[0].u, u_max = in[0].u;
  for (int i = 1; i < n; ++i) {
    if (in[i].x < minx) minx = in[i].x, u_min = in[i].u;
    if (in[i].x > maxx) maxx = in[i].x, u_max = in[i].u;
  }
  const float mid = (minx + maxx) * 0.5f;
  DWORD cull = D3DCULL_NONE;
  dev->GetRenderState(D3DRS_CULLMODE, &cull);
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);  // (mirrored: its triangles turn the other way)
  for (int k = 0; k < 2; ++k) {
    if (k ? maxx >= W - 0.5f : minx <= 0.5f) continue;  // (no bar on that side)
    for (int i = 0; i < n; ++i) {
      if (how == Wide::Mirror) {
        side[i].x = 2.0f * (k ? maxx : minx) - in[i].x;
      } else {  // the bar's own width, all of it the picture's edge column
        const bool outer = k ? in[i].x > mid : in[i].x < mid;
        side[i].x = k ? (outer ? (float)W + 0.5f : maxx) : (outer ? -0.5f : minx);
        // (a little inside: the picture's outermost texels are darker)
        side[i].u = k ? u_max - (u_max - u_min) * 0.004f : u_min + (u_max - u_min) * 0.004f;
      }
    }
    draw(side.data());
  }
  dev->SetRenderState(D3DRS_CULLMODE, cull);
}

// (With a background on to the sides nothing is cut at the 4:3 edges: the
// menus' pieces slide in from beyond the screen over it.)
int __cdecl hkLine(void* v, int n, int a, int b) {
  void* o = adjust(v, n);
  Clip43 clip(o != v && !g_extended);
  TexOverride tex;
  return oLine(o, n, a, b);
}
int __cdecl hkTri(void* v, int n, int a, int b, int c) {
  void* o = adjust(v, n);
  Clip43 clip(o != v && !g_extended);
  TexOverride tex;
  return oTri(o, n, a, b, c);
}
int __cdecl hkPrim(int t, void* v, int n) {
  g_extend = Wide::No;
  void* o = adjust(v, n);
  const Wide sides = o != v ? g_extend : Wide::No;
  g_extend = Wide::No;
  Clip43 clip(o != v && !g_extended);
  TexOverride tex;
  NoWrap no_wrap(sides != Wide::No);
  int r = oPrim(t, o, n);
  if (sides != Wide::No) draw_sides(o, n, sides, [&](void* s) { oPrim(t, s, n); });
  return r;
}
int __cdecl hkIdx(int t, void* v, int n, void* i, int ni) {
  g_extend = Wide::No;
  void* o = adjust(v, n);
  const Wide sides = o != v ? g_extend : Wide::No;
  g_extend = Wide::No;
  Clip43 clip(o != v && !g_extended);
  TexOverride tex;
  NoWrap no_wrap(sides != Wide::No);
  int r = oIdx(t, o, n, i, ni);
  if (sides != Wide::No) draw_sides(o, n, sides, [&](void* s) { oIdx(t, s, n, i, ni); });
  return r;
}

template <typename F>
void swap_ptr(uint8_t* engine, size_t off, F& original, F detour) {
  F& slot = *reinterpret_cast<F*>(engine + off);
  if (slot && slot != detour) {
    original = slot;
    slot = detour;
  }
}

void install_im2d() {
  uint8_t* engine = g_engine ? *g_engine : nullptr;
  if (!engine || *reinterpret_cast<Im2DPrim_t*>(engine + kDevIm2DPrim) == &hkPrim) return;
  swap_ptr<Im2DLine_t>(engine, kDevIm2DLine, oLine, &hkLine);
  swap_ptr<Im2DTri_t>(engine, kDevIm2DTri, oTri, &hkTri);
  swap_ptr<Im2DPrim_t>(engine, kDevIm2DPrim, oPrim, &hkPrim);
  swap_ptr<Im2DIdx_t>(engine, kDevIm2DIdx, oIdx, &hkIdx);
  LOGI("Im2D hooked (hud=%s, full-screen pictures=%s)", g_cfg.hud_center ? "center" : "stretch",
       g_cfg.fullscreen_pillarbox ? "pillarbox" : "stretch");
}

}  // namespace

void install_hooks() {
  if (uintptr_t a = patch::find("view_setter", kViewSetterSig)) {
    g_main_camera = reinterpret_cast<void* const*>(patch::read<uint32_t>(a + kViewSetterCamera));
    if (!patch::in_image((uintptr_t)g_main_camera, 4)) g_main_camera = nullptr;
  }
  if (!g_main_camera) LOGW("the game's main camera was not found: the view stays as the game has it");
  else if (g_cfg.fov != 100) LOGI("view: %d%% of the game's", g_cfg.fov);
  if (uintptr_t a = patch::find("RwIm2DRenderPrimitive", kIm2DPrimSig)) {
    g_engine = reinterpret_cast<uint8_t* const*>(patch::read<uint32_t>(a + 1));
    if (!patch::in_image((uintptr_t)g_engine, 4)) g_engine = nullptr;
  }
  if (uintptr_t a = patch::find("RwCameraSetViewWindow", kSetViewWindowSig))
    patch::hook("RwCameraSetViewWindow", a, reinterpret_cast<void*>(&hkSetViewWindow),
                reinterpret_cast<void**>(&oSetViewWindow));
  if (uintptr_t a = patch::find("text_in_box", kTextInBoxSig)) {
    g_text_in_box = a;
    if (patch::read<uint8_t>(a + kTextFontPush) == 0xFF && patch::read<uint8_t>(a + kTextFontPush + 1) == 0x35) {
      const uintptr_t font = patch::read<uint32_t>(a + kTextFontPush + 2);
      if (patch::in_image(font, 4)) g_text_font = reinterpret_cast<const uint32_t*>(font);
    }
    patch::hook("text_in_box", a, reinterpret_cast<void*>(&hkTextInBox), reinterpret_cast<void**>(&oTextInBox));
  }
}

void install(const Config& cfg) {
  g_cfg = cfg;
  if (!cfg.widescreen) {
    // 4:3 as in the original: nothing to fit, only tmntnet's own sprites.
    g_cfg.hud_center = g_cfg.fullscreen_pillarbox = false;
    install_hooks();
    LOGI("widescreen disabled ([video] widescreen=0)");
    return;
  }
  patch_mode_filter();

  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);
  g_pref_hz = cfg.refresh_hz ? cfg.refresh_hz : (int)dm.dmDisplayFrequency;
  if (cfg.res_mode == 0) {
    g_want_w = dm.dmPelsWidth;
    g_want_h = dm.dmPelsHeight;
  } else if (cfg.res_mode == 2) {
    g_want_w = cfg.res_w;
    g_want_h = cfg.res_h;
  }
  if (cfg.display == "windowed" && cfg.window_w && cfg.res_mode == 0) {
    // A window of a set size: render at that size (the nearest listed mode at
    // or above it), not at the desktop's, so pixels map 1:1.
    g_want_w = cfg.window_w;
    g_want_h = cfg.window_h;
  }
  if (uintptr_t a = patch::find("select_video_mode", kSelectModeSig)) {
    g_adapter_index = reinterpret_cast<const int32_t*>(patch::read<uint32_t>(a + 2));
    g_adapters = reinterpret_cast<uint8_t* const*>(patch::read<uint32_t>(a + 14));
    if (patch::in_image((uintptr_t)g_adapter_index, 4) && patch::in_image((uintptr_t)g_adapters, 4) &&
        patch::hook("select_video_mode", a, reinterpret_cast<void*>(&hkSelectMode),
                    reinterpret_cast<void**>(&oSelectMode))) {
      if (g_want_w)
        LOGI("resolution: %dx%d (%s)", g_want_w, g_want_h,
             cfg.res_mode == 2 ? "tmntnet.ini" : cfg.display == "windowed" && cfg.window_w ? "window_size" : "desktop");
      else
        LOGI("resolution: game setting (Options > Display)");
    }
  }

  install_hooks();
}

void set_texture_override(IDirect3DTexture9* tex) { g_override = tex; }

void end_frame() {
  g_tiles = Bounds{};
  g_extended = false;
}

bool pictures_pillarboxed() { return g_cfg.widescreen && g_cfg.fullscreen_pillarbox; }
bool menus_wide() { return pictures_pillarboxed() && g_cfg.wide_menus; }
bool hud_centered() { return g_cfg.widescreen && g_cfg.hud_center; }
bool hud_corners() { return hud_centered() && g_cfg.hud_corners; }
void set_edge(int side) { g_edge = side; }

void set_sprite(const char* texture_name) {
  if (!texture_name) {
    g_sprite[0] = 0;
    return;
  }
  strncpy_s(g_sprite, texture_name, 32);
}

uintptr_t text_in_box() { return g_text_in_box; }

void set_key_table(bool up) { g_key_table = up; }

void set_hud_center(bool on) { g_cfg.hud_center = on; }

bool shown_window(void* camera, const float asked[2], float out[2]) {
  if (!camera || !(asked[0] > 0) || !(asked[1] > 0)) return false;
  RwV2d v = shown(camera, RwV2d{asked[0], asked[1]}, false);
  out[0] = v.x, out[1] = v.y;
  return true;
}

bool asked_window(void* camera, float out[2]) {
  if (!camera || camera != g_view_camera || !(g_view_asked.x > 0) || !(g_view_asked.y > 0)) return false;
  out[0] = g_view_asked.x, out[1] = g_view_asked.y;
  return true;
}

void set_fov(int percent) {
  g_cfg.fov = percent;
  // The main camera now, if it still has the window set last (a new camera, or
  // one the game has set since, gets it from the game).
  auto* c = static_cast<uint8_t*>(g_view_camera);
  if (own_camera::swaps()) return;  // (every picture takes [video] fov as it is then)
  if (!c || !g_main_camera || *g_main_camera != g_view_camera || !oSetViewWindow) return;
  const auto* now = reinterpret_cast<const RwV2d*>(c + kCameraViewWindow);
  if (now->x == g_view_set.x && now->y == g_view_set.y) hkSetViewWindow(c, &g_view_asked);
}
void set_pillarbox(bool on) { g_cfg.fullscreen_pillarbox = on; }

std::vector<std::pair<int, int>> video_modes() {
  std::vector<std::pair<int, int>> out;
  if (!g_adapters || !*g_adapters || !g_adapter_index) return out;
  uint8_t* ad = *g_adapters + *g_adapter_index * 0xA8;
  auto* modes = *reinterpret_cast<ModeEntry**>(ad + 0x50);
  int count = *reinterpret_cast<int32_t*>(ad + 0x54);
  for (int i = 0; modes && i < count; ++i) {
    std::pair<int, int> m{modes[i].w, modes[i].h};
    if (modes[i].depth == 32 && std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::pair<int, int> current_mode() {
  if (!g_adapters || !*g_adapters || !g_adapter_index) return {0, 0};
  uint8_t* ad = *g_adapters + *g_adapter_index * 0xA8;
  auto* modes = *reinterpret_cast<ModeEntry**>(ad + 0x50);
  int count = *reinterpret_cast<int32_t*>(ad + 0x54), cur = *reinterpret_cast<int32_t*>(ad + 0x58);
  if (!modes || cur < 0 || cur >= count) return {0, 0};
  return {modes[cur].w, modes[cur].h};
}

}  // namespace widescreen
