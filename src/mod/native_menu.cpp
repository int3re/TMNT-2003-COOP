#include "mod/native_menu.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

#include "common/log.h"
#include "common/net.h"
#include "mod/ambient.h"
#include "mod/game_text.h"
#include "mod/overlay.h"
#include "mod/widescreen.h"

namespace native_menu {
namespace {

// --- the game's look -------------------------------------------------------------
// The game lays its option screens out in a 512x480 space (0x4ab8fc): x
// -256..256, y -240..240 upwards, a sprite's position is its centre, and a
// texel of the option pictures is half a unit. The numbers below are the
// game's: the tables of ПАРАМЕТРЫ (0x5923d0) and ПРОЧИЕ ПАРАМЕТРЫ (0x591a98),
// re/notes.md §16.
constexpr float kTexel = 0.5f;
constexpr float kRowY[] = {110, 75, 41, 7, -28, -62, -95, -129};  // ПАРАМЕТРЫ's rows
constexpr int kMaxRows = 8;
constexpr float kLettersX = -184.5f;   // where a row's letters start
constexpr float kLettersMid = 1.0f;    // their middle line, above the row's y
constexpr float kOurMid = 32.0f;       // our pictures of letters: middle line, texels from the top
constexpr float kOurStart = 4.0f;      // ... and where the letters start
constexpr float kValueX = 126.0f;      // values and their arrows are centred here
constexpr float kValueGap = 6.0f;      // between a value and an arrow
constexpr float kRightEdge = 252.0f;
constexpr float kCursorX = -181.0f, kCursorLift = 2.0f, kCursorSize = 64.0f;  // op_cursor
constexpr float kTitleX = -214.0f, kTitleMid = 173.6f;  // the title's letters
constexpr float kTitleScale = 43.0f / 24.0f;            // as tall as the game's titles (measured on screen)
constexpr float kOkX = -171.0f, kOkSize = 32.0f;          // op_soundset_ok (64 texels)
constexpr float kOkY = -129.0f, kOkLowY = -166.0f;        // (under 8 rows: where ПАРАМЕТРЫ has УЙТИ)
constexpr float kPlainTexel = 0.9f;    // the plain font: its thin 15-texel digits a little over the labels
constexpr float kPlainMid = 16.5f;     // ... their middle line in plain() pictures
// op_other_moji_ya (256x32 texels) is drawn 128x32: arrows at 67..79.5 and
// 171.5..184 around a value at 126. A wider value pushes the halves out; the
// arrows' inner edges are 17.5 and 45.5 units into them.
constexpr float kArrowInnerL = 17.5f, kArrowInnerR = 45.5f, kArrowW = 12.5f, kArrowsRoom = 45.5f;
constexpr DWORD kCyan = 0x00FFFF;      // like ОПЦИИ ПО-УМОЛЧАНИЮ: a note, not a setting
// The volume sliders of ОПЦИИ ЗВУКА (0x5928dc, 0x5928f8): op_soundset_vol's
// third bar with its signs (texel rows 163..233, 256x35 units, centred at x 78)
// and the knob op_soundset_tumami (32x64) at x = level * 53 - 28.
constexpr float kBarX = 78.0f, kBarW = 256.0f, kBarH = 35.0f, kBarV0 = 163.0f / 256.0f, kBarV1 = 233.0f / 256.0f;
constexpr float kBarLift = 3.0f, kKnobLift = 1.0f, kKnobW = 32.0f, kKnobH = 64.0f, kKnobStep = 53.0f,
                kKnobX0 = -28.0f;

// Timing, in the game's frames (60 a second).
constexpr float kAlphaStep = 12.75f;         // sprites fade by this a frame (0x48d4f1): 20 frames
constexpr float kPi = 3.14159265f;
constexpr float kPulseStep = kPi / 30;       // cursors (0x48d70b): (cos + 1) * 128, 60 frames a cycle
constexpr float kCurtainStep = 255.0f / 30;  // screens fade to and from black in half a second

constexpr uint32_t kUp = 0x1000, kRight = 0x2000, kDown = 0x4000, kLeft = 0x8000, kDecide = 0x10000,
                   kCancel = 0x20000;
// The menu object's sounds (0x40f6f1): move, decide, cancel.
constexpr int kSoundMove = 0x263, kSoundDecide = 0x264, kSoundCancel = 0x265;

// tmntnet's own pieces on a page, in the game's look: dark plates with a thin
// line of the background's green round them. The list at the top right (the
// players) and the lines at the bottom (what happens now; what the selected
// row does).
constexpr DWORD kLineRgb = 0x3CF050;
constexpr float kLean = 0.22f;  // bars lean like the game's letters: units right a unit up
constexpr float kPanelX0 = 10, kPanelX1 = 250, kPanelTop = 196, kPanelHead = 27, kCardH = 24.5f, kPanelPitch = 28.5f;
constexpr float kInfoX0 = -202, kInfoX1 = 234, kInfoBottom = -218, kInfoLine = 25;
constexpr DWORD kHintRgb = 0xC8F0FF;

int (*g_play)(int) = nullptr;
void sound(int id) {
  if (g_play) g_play(id);
}

// --- textures ------------------------------------------------------------------

struct Tex {
  IDirect3DTexture9* t = nullptr;
  int w = 0, h = 0, text_w = 0;
};
std::map<std::string, Tex> g_tex;  // "pic:op_bg", "txt:АВТОЗАПИСЬ", "pln:60"
IDirect3DDevice9* g_tex_dev = nullptr;

Tex upload(IDirect3DDevice9* dev, const game_text::Bitmap& b) {
  Tex out;
  IDirect3DTexture9* t = nullptr;
  if (b.w <= 0 || FAILED(dev->CreateTexture(b.w, b.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) return out;
  D3DLOCKED_RECT lr{};
  if (FAILED(t->LockRect(0, &lr, nullptr, 0))) {
    t->Release();
    return out;
  }
  for (int y = 0; y < b.h; ++y)
    memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, &b.bgra[size_t(y) * b.w], b.w * 4);
  t->UnlockRect(0);
  out.t = t, out.w = b.w, out.h = b.h, out.text_w = b.text_w;
  return out;
}

// Small shapes made here (the game has no such pictures), white with soft
// edges, drawn tinted. 48x48: "hex" and "ring" (a hexagon and a ring of one,
// flat top like the background's), "tick", "star", "dot". "beam" (64x16): a
// line of light, bright along its middle, fading to its edges and its ends.
bool inside(const std::vector<std::pair<float, float>>& p, float x, float y) {  // even-odd
  bool in = false;
  for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
    if ((p[i].second > y) != (p[j].second > y) &&
        x < (p[j].first - p[i].first) * (y - p[i].second) / (p[j].second - p[i].second) + p[i].first)
      in = !in;
  return in;
}

// How much of pixel (px, py) of an n x n picture the shape (in 0..1, y down) covers: 4x4 samples.
float coverage(const std::vector<std::pair<float, float>>& p, int px, int py, int n) {
  int hits = 0;
  for (int sy = 0; sy < 4; ++sy)
    for (int sx = 0; sx < 4; ++sx)
      hits += inside(p, (px + (sx + 0.5f) * 0.25f) / n, (py + (sy + 0.5f) * 0.25f) / n) ? 1 : 0;
  return hits / 16.0f;
}

std::vector<std::pair<float, float>> round_shape(int corners, float r_out, float r_in, float turn, float cy = 0.5f) {
  std::vector<std::pair<float, float>> p;
  const bool star = r_in > 0;
  const int n = star ? corners * 2 : corners;
  for (int i = 0; i < n; ++i) {
    float a = turn + 6.2831853f * float(i) / float(n), r = star && (i & 1) ? r_in : r_out;
    p.push_back({0.5f + r * std::cos(a), cy + r * std::sin(a)});
  }
  return p;
}

bool make_shape(const std::string& name, game_text::Bitmap& b) {
  if (name == "beam") {
    b.w = b.text_w = 64, b.h = 16;
    b.bgra.assign(size_t(b.w) * b.h, 0);
    for (int y = 0; y < b.h; ++y)
      for (int x = 0; x < b.w; ++x) {
        float v = 1.0f - std::fabs((y + 0.5f) / b.h * 2.0f - 1.0f), u = (x + 0.5f) / b.w;
        float ends = std::clamp(std::min(u, 1.0f - u) / 0.2f, 0.0f, 1.0f);
        b.bgra[size_t(y) * b.w + x] = (uint32_t(v * v * ends * 255.0f + 0.5f) << 24) | 0xFFFFFF;
      }
    return true;
  }
  const int n = 48;
  std::vector<std::pair<float, float>> shape, hole;
  if (name == "hex" || name == "ring") {
    shape = round_shape(6, 0.47f, 0, 0);
    if (name == "ring") hole = round_shape(6, 0.36f, 0, 0);
  } else if (name == "tick") {
    shape = {{0.10f, 0.52f}, {0.20f, 0.42f}, {0.40f, 0.62f}, {0.789f, 0.202f}, {0.891f, 0.298f}, {0.405f, 0.83f}};
  } else if (name == "star") {
    shape = round_shape(5, 0.48f, 0.21f, -1.5707963f, 0.53f);
  } else if (name == "dot") {
    shape = round_shape(24, 0.42f, 0, 0);
  } else {
    return false;
  }
  b.w = b.h = b.text_w = n;
  b.bgra.assign(size_t(n) * n, 0);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      float a = coverage(shape, x, y, n) - (hole.empty() ? 0.0f : coverage(hole, x, y, n));
      b.bgra[size_t(y) * n + x] = (uint32_t(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f) << 24) | 0xFFFFFF;
    }
  return true;
}

// The plain font's pictures of text that changes (pings) are dropped now and
// then. Only between frames: a page's draw keeps pointers into the map.
void trim_textures() {
  if (g_tex.size() < 500) return;
  for (auto it = g_tex.begin(); it != g_tex.end();) {
    if (it->first.compare(0, 4, "pln:") == 0) {
      if (it->second.t) it->second.t->Release();
      it = g_tex.erase(it);
    } else {
      ++it;
    }
  }
}

const Tex* texture(IDirect3DDevice9* dev, const std::string& key) {
  if (dev != g_tex_dev) {  // a new device: start over
    for (auto& kv : g_tex)
      if (kv.second.t) kv.second.t->Release();
    g_tex.clear();
    g_tex_dev = dev;
  }
  auto it = g_tex.find(key);
  if (it != g_tex.end()) return it->second.t ? &it->second : nullptr;
  game_text::Bitmap b;
  bool ok = key.compare(0, 4, "pic:") == 0   ? game_text::picture(key.c_str() + 4, b)
            : key.compare(0, 4, "pln:") == 0 ? game_text::plain(key.substr(4), b)
            : key.compare(0, 4, "gen:") == 0 ? make_shape(key.substr(4), b)
                                             : game_text::compose(key.substr(4), b);
  Tex t = ok ? upload(dev, b) : Tex{};
  if (!ok) LOGW("native menu: no picture for %s", key.c_str());
  return (g_tex[key] = t).t ? &g_tex[key] : nullptr;
}

// --- drawing ---------------------------------------------------------------------

struct Vtx {
  float x, y, z, rhw;
  DWORD color;
  float u, v;
};
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

struct Screen {
  float left = 0, top = 0, ux = 1, uy = 1;  // where the layout's corner (-256, 240) is; pixels a unit
  float px(float x) const { return left + (x + 256.0f) * ux; }
  float py(float y) const { return top + (240.0f - y) * uy; }
};

// A picture over the layout rectangle x0..x1, y0 (top)..y1, from u0..u1, v0..v1
// of its texture.
void quad(IDirect3DDevice9* dev, const Screen& s, const Tex* t, float x0, float y0, float x1, float y1, int alpha,
          bool additive = false, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1, DWORD rgb = 0xFFFFFF) {
  if (!t || alpha <= 0) return;
  dev->SetTexture(0, t->t);
  dev->SetRenderState(D3DRS_DESTBLEND, additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
  DWORD c = DWORD(std::min(alpha, 255)) << 24 | rgb;
  float X0 = s.px(x0) - 0.5f, X1 = s.px(x1) - 0.5f, Y0 = s.py(y0) - 0.5f, Y1 = s.py(y1) - 0.5f;
  Vtx v[4] = {{X0, Y0, 0, 1, c, u0, v0}, {X1, Y0, 0, 1, c, u1, v0}, {X0, Y1, 0, 1, c, u0, v1}, {X1, Y1, 0, 1, c, u1, v1}};
  dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
}

void solid(IDirect3DDevice9* dev, const Screen& s, float x0, float y0, float x1, float y1, DWORD argb) {
  dev->SetTexture(0, nullptr);
  dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
  dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
  dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  float X0 = s.px(x0), X1 = s.px(x1), Y0 = s.py(y0), Y1 = s.py(y1);
  Vtx v[4] = {{X0, Y0, 0, 1, argb, 0, 0}, {X1, Y0, 0, 1, argb, 0, 0}, {X0, Y1, 0, 1, argb, 0, 0}, {X1, Y1, 0, 1, argb, 0, 0}};
  dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
  dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
}

void begin_2d(IDirect3DDevice9* dev) {
  dev->SetVertexShader(nullptr);
  dev->SetPixelShader(nullptr);
  dev->SetFVF(kFvf);
  dev->SetRenderState(D3DRS_ZENABLE, FALSE);
  dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
  dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
  dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
  dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
  dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
}

bool ascii(const std::string& s) {
  for (unsigned char c : s)
    if (c >= 0x80) return false;
  return true;
}

// A line of text: a picture of the game's letters or of its plain font.
struct Words {
  const Tex* t = nullptr;
  bool plain = false;
  float texel() const { return plain ? kPlainTexel : kTexel; }
  float start() const { return plain ? 2.0f : kOurStart; }  // the letters, in texels
  float end() const { return plain ? t->text_w - 2.0f : (float)t->text_w; }
  float width() const { return t ? (end() - start()) * texel() : 0.0f; }
  float mid() const { return plain ? kPlainMid : kOurMid; }
};

Words words(IDirect3DDevice9* dev, const std::string& text, bool plain) {
  Words w;
  if (text.empty()) return w;
  w.plain = plain || ascii(text);  // (the game's letters have no Latin)
  w.t = texture(dev, (w.plain ? "pln:" : "txt:") + text);
  return w;
}

// Draws words with their letters from x (or centred on x), middle line at y.
void draw_words(IDirect3DDevice9* dev, const Screen& s, const Words& w, float x, float y, int alpha, bool centred,
                float scale = 1, DWORD rgb = 0xFFFFFF) {
  if (!w.t) return;
  float tx = w.texel() * scale;
  float left = centred ? x - (w.start() + w.end()) * 0.5f * tx : x - w.start() * tx;
  float top = y + w.mid() * tx;
  quad(dev, s, w.t, left, top, left + w.t->w * tx, top - w.t->h * tx, alpha, false, 0, 0, 1, 1, rgb);
}

DWORD argb(int a, DWORD rgb) { return DWORD(std::clamp(a, 0, 255)) << 24 | (rgb & 0xFFFFFF); }

// The colour `a` moved towards `b` by t (0..1).
DWORD blend(DWORD a, DWORD b, float t) {
  auto ch = [&](int sh) {
    float x = float((a >> sh) & 255), y = float((b >> sh) & 255);
    return DWORD(x + (y - x) * t + 0.5f) << sh;
  };
  return ch(16) | ch(8) | ch(0);
}

float ease(float t) {  // 0..1, fast at first
  t = std::clamp(t, 0.0f, 1.0f);
  const float u = 1.0f - t;
  return 1.0f - u * u * u;
}

// A shape of up to 8 corners (convex, in order), a colour a corner: fills
// with gradients. Layout units.
struct Pt {
  float x, y;
  DWORD c;
};
void poly(IDirect3DDevice9* dev, const Screen& s, const Pt* p, int n, bool additive = false) {
  if (n < 3 || n > 8) return;
  Vtx v[8];
  for (int i = 0; i < n; ++i) v[i] = Vtx{s.px(p[i].x), s.py(p[i].y), 0, 1, p[i].c, 0, 0};
  dev->SetTexture(0, nullptr);
  dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
  dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
  dev->SetRenderState(D3DRS_DESTBLEND, additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
  dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, n - 2, v, sizeof(Vtx));
  dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
}

// A bar leaning like the game's letters: x0..x1 along its bottom, its top
// `lean` further right; from the colour `left` to `right`.
void bar(IDirect3DDevice9* dev, const Screen& s, float x0, float top, float x1, float bottom, DWORD left, DWORD right,
         float lean, bool additive = false) {
  const Pt p[4] = {{x0, bottom, left}, {x0 + lean, top, left}, {x1 + lean, top, right}, {x1, bottom, right}};
  poly(dev, s, p, 4, additive);
}

// A line of light along y from x0 to x1, h thick (added to the picture).
void beam(IDirect3DDevice9* dev, const Screen& s, float x0, float x1, float y, float h, int a, DWORD rgb) {
  quad(dev, s, texture(dev, "gen:beam"), x0, y + h * 0.5f, x1, y - h * 0.5f, a, true, 0, 0, 1, 1, rgb);
}

// A shape made here ("gen:..."), as wide as high on the screen (the layout's
// units are not square): its centre and half its width.
void icon(IDirect3DDevice9* dev, const Screen& s, const char* key, float x, float y, float half, int a, DWORD rgb) {
  const float hh = half * s.ux / std::max(0.01f, s.uy);
  quad(dev, s, texture(dev, key), x - half, y + hh, x + half, y - hh, a, false, 0, 0, 1, 1, rgb);
}

// tmntnet's plate: a leaning bar, dark with a touch of `rgb` at its left, a
// strip of `rgb` along its left edge, light along its top. x0..x1 at its bottom.
void plate(IDirect3DDevice9* dev, const Screen& s, float x0, float top, float x1, float bottom, int a,
           DWORD rgb = kLineRgb, int dark = 215) {
  if (a <= 0) return;
  const float lean = (top - bottom) * kLean;
  bar(dev, s, x0, top, x1, bottom, argb(a * dark / 255, blend(0x000000, rgb, 0.24f)),
      argb(a * (dark - 60) / 255, 0x020802), lean);
  bar(dev, s, x0, top, x0 + 3.5f, bottom, argb(a, rgb), argb(a, rgb), lean);
  const DWORD lit = argb(a * 210 / 255, blend(rgb, 0xFFFFFF, 0.2f)), dim = argb(a * 30 / 255, rgb);
  const Pt edge[4] = {{x0 + lean, top, lit}, {x1 + lean, top, dim}, {x1 + lean - 0.25f, top - 1.1f, dim},
                      {x0 + lean - 0.25f, top - 1.1f, lit}};
  poly(dev, s, edge, 4);
  beam(dev, s, x0 + lean - 8, x0 + lean + (x1 - x0) * 0.7f, top, 8, a * 150 / 255, rgb);
}

// Where a value goes: at the game's place, or moved and made smaller when it
// is too wide for its arrows or its row.
struct Place {
  float x = kValueX;  // centre
  float half = 0;     // half the width
  float scale = 1;
  bool wide = false;  // the arrows are moved out
};

Place place(float label_end, float width, float value_x) {
  Place p;
  p.x = value_x;
  p.half = width * 0.5f;
  if (p.half + kValueGap <= kArrowsRoom) return p;
  p.wide = true;
  const float side = kValueGap + kArrowW, from = label_end + 12;
  float lo = from + side + p.half, hi = kRightEdge - side - p.half;
  if (lo <= hi) {
    p.x = std::clamp(value_x, lo, hi);
    return p;
  }
  float room = kRightEdge - side - (from + side);
  p.scale = std::max(0.5f, room / width);
  p.half = width * p.scale * 0.5f;
  p.x = from + side + p.half;
  return p;
}

void draw_arrows(IDirect3DDevice9* dev, const Screen& s, const Place& p, float row_y, int alpha) {
  const Tex* t = texture(dev, "pic:op_other_moji_ya");
  if (!t || alpha <= 0) return;
  float top = row_y + 16, bottom = row_y - 16;  // 32 texels at one unit: the game draws it 128x32
  if (!p.wide) {
    quad(dev, s, t, p.x - 64, top, p.x + 64, bottom, alpha, true);
    return;
  }
  float li = p.x - p.half - kValueGap, ri = p.x + p.half + kValueGap;  // the arrows' inner edges
  quad(dev, s, t, li - kArrowInnerL, top, li - kArrowInnerL + 64, bottom, alpha, true, 0, 0, 0.5f, 1);
  quad(dev, s, t, ri - kArrowInnerR, top, ri - kArrowInnerR + 64, bottom, alpha, true, 0.5f, 0, 1, 1);
}

// --- pages -----------------------------------------------------------------------

// A cursor's brightness (0x48d70b / 0x48d77f): (cos(phase) + 1) * 128, the
// phase going round once in 60 frames; switched off, it fades out to dark.
struct Pulse {
  float phase = kPi;  // dark
  bool on = false;
  void step(float frames) {
    if (on) {
      phase += kPulseStep * frames;
      while (phase > kPi) phase -= 2 * kPi;
    } else {
      if (phase < 0) phase = -phase;  // (was brightening: turn round)
      phase = std::min(kPi, phase + kPulseStep * frames);
    }
  }
  int alpha() const { return std::clamp(int((std::cos(phase) + 1) * 128), 0, 255); }
};

struct RowState {
  Pulse arrows;                    // pulse on the selected row, opposite to the cursor
  std::vector<float> value_alpha;  // each choice's picture fades in and out (20 frames)
};

struct Open {
  Page page;
  int cursor = 0;
  Pulse pulse;  // the cursor
  std::vector<RowState> rows;
  float alpha = 0, target = 255;  // the page's words
  float intro = 0;                // 0..1 once it shows: tmntnet's pieces come in (the list from the right)
};

std::vector<Open> g_stack;    // the open pages; the top one has the buttons
std::vector<Open> g_leaving;  // closed ones, fading out
Host g_host = Host::MainMenu;
std::function<void()> g_on_closed;
Config g_cfg;
uint32_t g_last_ms = 0;

// MainMenu host: the black between the menu and the pages.
enum class Stage { Idle, Darken, Show, Open, Hide, Lighten };
Stage g_stage = Stage::Idle;
float g_curtain = 0;

int rows_total(const Page& p) { return (int)std::min<size_t>(p.rows.size(), kMaxRows) + 1; }  // + OK
bool shown(const Page& p, int i) { return i >= (int)p.rows.size() || !p.rows[i].shown || p.rows[i].shown(); }
bool selectable(const Page& p, int i) { return i == rows_total(p) - 1 || (p.rows[i].selectable && shown(p, i)); }
// The place of row i among the rows shown (hidden ones above it leave no gap).
int slot_of(const Page& p, int i) {
  int slot = 0;
  for (int k = 0; k < i && k < (int)p.rows.size(); ++k) slot += shown(p, k) ? 1 : 0;
  return std::min(slot, kMaxRows - 1);
}
float ok_y(const Page& p) { return p.rows.size() >= kMaxRows ? kOkLowY : kOkY; }

// The arrows of the selected row come up from dark (0x48e86f); the others fade.
void select_arrows(std::vector<RowState>& state, int i) {
  for (size_t k = 0; k < state.size(); ++k) {
    RowState& rs = state[k];
    bool on = (int)k == i;
    if (on && !rs.arrows.on) rs.arrows.phase = kPi;
    rs.arrows.on = on;
  }
}

void select(Open& o, int i) {
  o.cursor = i;
  o.pulse.phase = 0;  // 0x48d76f: bright at once
  o.pulse.on = true;
  select_arrows(o.rows, i);
}

void move(Open& o, int dir) {
  int n = rows_total(o.page), i = o.cursor;
  for (int k = 0; k < n; ++k) {
    i = (i + dir + n) % n;
    if (selectable(o.page, i)) break;
  }
  select(o, i);
}

std::vector<RowState> make_states(const std::vector<Row>& rows) {
  std::vector<RowState> out(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& r = rows[i];
    if (r.values.empty() || !r.get || r.text) continue;
    out[i].value_alpha.assign(r.values.size(), 0.0f);
    int v = r.get();
    if (v >= 0 && v < (int)r.values.size()) out[i].value_alpha[v] = 255;
  }
  return out;
}

// Left/right (and decide) on a row: the next choice or the row's action.
bool row_input(Row& r, uint32_t pressed) {
  if ((pressed & (kLeft | kRight)) && r.slider > 0 && r.get && r.set) {
    int v = r.get(), n = std::clamp(v + ((pressed & kRight) ? 1 : -1), 0, r.slider - 1);
    if (n == v) return false;
    r.set(n);
    sound(kSoundMove);
    return true;
  }
  if (pressed & (kLeft | kRight)) {
    if (r.values.empty() || !r.get || !r.set) return false;
    int n = (int)r.values.size(), v = r.get();
    r.set((v + ((pressed & kRight) ? 1 : n - 1)) % n);
    sound(kSoundMove);
    return true;
  }
  if (pressed & kDecide) {
    if (r.action) {
      sound(kSoundDecide);
      r.action();
      return true;
    }
    if (!r.values.empty() && r.get && r.set) {
      r.set((r.get() + 1) % (int)r.values.size());
      sound(kSoundMove);
      return true;
    }
  }
  return false;
}

Open make(Page page, float alpha) {
  Open o;
  o.page = std::move(page);
  o.alpha = alpha;
  o.rows = make_states(o.page.rows);
  select(o, std::clamp(o.page.first, 0, rows_total(o.page) - 1));
  if (!selectable(o.page, o.cursor)) move(o, 1);
  return o;
}

void close_top() {
  if (g_stack.empty()) return;
  if (g_stack.size() > 1) {
    Open& top = g_stack.back();
    if (top.page.on_close) top.page.on_close();
    top.target = 0;
    g_leaving.push_back(std::move(top));
    g_stack.pop_back();
    g_stack.back().target = 255;
    return;
  }
  if (g_host == Host::MainMenu) {
    g_stage = Stage::Hide;  // the black comes over the page, then the page goes
    return;
  }
  Open& top = g_stack.back();
  if (top.page.on_close) top.page.on_close();
  top.target = 0;
  g_leaving.push_back(std::move(top));
  g_stack.clear();
  if (g_on_closed) g_on_closed();
}

bool ready() {
  if (g_stack.empty()) return false;
  if (g_host == Host::MainMenu && g_stage != Stage::Open) return false;
  return g_stack.back().alpha >= 255.0f;
}

void move_toward(float& v, float target, float step, float frames) {
  if (v < target) v = std::min(target, v + step * frames);
  if (v > target) v = std::max(target, v - step * frames);
}

// The arrows' pulse and each choice's fade, for one set of rows.
void step_rows(const std::vector<Row>& rows, std::vector<RowState>& state, float frames) {
  for (size_t i = 0; i < state.size() && i < rows.size(); ++i) {
    RowState& rs = state[i];
    rs.arrows.step(frames);
    if (rs.value_alpha.empty()) continue;
    int v = rows[i].get();
    for (size_t c = 0; c < rs.value_alpha.size(); ++c)
      move_toward(rs.value_alpha[c], (int)c == v ? 255.0f : 0.0f, kAlphaStep, frames);
  }
}

// Frames since the last call, at the game's 60 a second (at most 6).
float frames_since(uint32_t& last_ms) {
  uint32_t now = net::now_ms();
  float frames = last_ms ? std::min(6.0f, (now - last_ms) * 0.06f) : 1.0f;
  last_ms = now;
  return frames;
}

// Time moves on: fades, pulses, the black.
void advance() {
  float frames = frames_since(g_last_ms);
  auto toward = [frames](float& v, float target, float step) { move_toward(v, target, step, frames); };
  auto step_page = [&](Open& o) {
    toward(o.alpha, o.target, kAlphaStep);
    o.pulse.step(frames);
    step_rows(o.page.rows, o.rows, frames);
  };
  for (Open& o : g_stack) step_page(o);
  for (Open& o : g_leaving) step_page(o);
  for (Open& o : g_stack) {
    if (!selectable(o.page, o.cursor)) move(o, -1);  // (the cursor's row was hidden meanwhile)
    if (g_host != Host::MainMenu || g_stage == Stage::Show || g_stage == Stage::Open)
      o.intro = std::min(1.0f, o.intro + frames / 28.0f);
  }
  g_leaving.erase(std::remove_if(g_leaving.begin(), g_leaving.end(), [](const Open& o) { return o.alpha <= 0; }),
                  g_leaving.end());

  switch (g_stage) {
    case Stage::Darken:
      toward(g_curtain, 255, kCurtainStep);
      if (g_curtain >= 255) g_stage = Stage::Show;
      break;
    case Stage::Show:
      toward(g_curtain, 0, kCurtainStep);
      if (g_curtain <= 0) g_stage = Stage::Open;
      break;
    case Stage::Hide:
      toward(g_curtain, 255, kCurtainStep);
      if (g_curtain >= 255) {
        if (!g_stack.empty() && g_stack.front().page.on_close) g_stack.front().page.on_close();
        g_stack.clear();
        g_leaving.clear();
        g_stage = Stage::Lighten;
        if (g_on_closed) g_on_closed();
      }
      break;
    case Stage::Lighten:
      toward(g_curtain, 0, kCurtainStep);
      if (g_curtain <= 0) g_stage = Stage::Idle;
      break;
    default:
      break;
  }
}

// One row at the layout's row_y: its label, its value (the choices crossfaded
// in one place, set by the widest of them) and, on the selected row, the arrows.
void draw_row(IDirect3DDevice9* dev, const Screen& s, const Row& r, const RowState& rs, float row_y, int a,
              float value_x = kValueX, float letters_x = kLettersX, float label_room = 0) {
  float y = row_y + kLettersMid;
  std::string lab = r.label_fn ? r.label_fn() : r.label;
  Words lw = !r.plain_label.empty() ? words(dev, r.plain_label, true) : words(dev, lab, false);
  // (beside a list at the right a long label is made smaller, not drawn under it)
  const float ls = label_room > 0 && lw.width() > label_room ? label_room / lw.width() : 1.0f;
  draw_words(dev, s, lw, letters_x, y, a, false, ls, r.label_rgb ? DWORD(r.label_rgb()) : r.note ? kCyan : 0xFFFFFF);
  float label_end = letters_x + lw.width() * ls;

  if (r.slider > 0 && r.get) {  // like МУЗЫКА / ЗВУК / ГОЛОС
    float by = row_y + kBarLift, ky = row_y + kKnobLift;
    float kx = kKnobX0 + kKnobStep * std::clamp(r.get(), 0, r.slider - 1) * 4.0f / std::max(1, r.slider - 1);
    quad(dev, s, texture(dev, "pic:op_soundset_vol"), kBarX - kBarW * 0.5f, by + kBarH * 0.5f, kBarX + kBarW * 0.5f,
         by - kBarH * 0.5f, a, false, 0, kBarV0, 1, kBarV1);
    quad(dev, s, texture(dev, "pic:op_soundset_tumami"), kx - kKnobW * 0.5f, ky + kKnobH * 0.5f, kx + kKnobW * 0.5f,
         ky - kKnobH * 0.5f, a);
    return;
  }

  bool has_place = false;
  Place pl;
  if (r.text) {  // a live value: codes, names, numbers
    Words vw = words(dev, r.text(), false);
    pl = place(label_end, vw.width(), value_x);
    has_place = true;
    draw_words(dev, s, vw, pl.x, y, a, true, pl.scale, r.text_rgb ? DWORD(r.text_rgb()) : 0xFFFFFF);
  } else if (!rs.value_alpha.empty()) {
    std::vector<Words> vw(r.values.size());
    float widest = 0;
    for (size_t c = 0; c < r.values.size(); ++c) {
      vw[c] = words(dev, r.values[c], false);
      widest = std::max(widest, vw[c].width());
    }
    pl = place(label_end, widest, value_x);
    has_place = true;
    for (size_t c = 0; c < r.values.size(); ++c)
      draw_words(dev, s, vw[c], pl.x, y, int(rs.value_alpha[c] * a / 255), true, pl.scale);
  }
  if (has_place && !r.values.empty()) draw_arrows(dev, s, pl, row_y, rs.arrows.alpha() * a / 255);
}

// The layout on the viewport, fitted like the game's own sprites
// (widescreen.cpp): the full-screen background as [video] fullscreen_images
// says (4:3 or stretched), all smaller pieces (words, turtles, cursors) as
// [video] hud says.
Screen fit(const D3DVIEWPORT9& vp, bool keep43) {
  float W = (float)vp.Width, H = (float)vp.Height, w43 = H * 4.0f / 3.0f;
  bool pillar = keep43 && W > w43 + 1.0f;
  Screen out;
  out.left = vp.X + (pillar ? (W - w43) * 0.5f : 0.0f);
  out.top = (float)vp.Y;
  out.ux = (pillar ? w43 : W) / 512.0f;
  out.uy = H / 480.0f;
  return out;
}

// The list at the top right: its title and count over a line of light, a
// card a player. The cards come in from the right, one after another.
void draw_panel(IDirect3DDevice9* dev, const Screen& s, const Panel& p, int a, float intro) {
  if (a <= 0 || p.lines.empty()) return;
  const float x0 = kPanelX0, x1 = kPanelX1, top = kPanelTop;
  const int ha = int(a * ease(intro * 1.5f));
  const float head = top - 11;
  draw_words(dev, s, words(dev, p.title, false), x0 + 2, head, ha, false, 0.85f);
  Words count = words(dev, p.count, true);
  if (count.t) draw_words(dev, s, count, x1 - 2 - count.width() * 0.85f, head, ha * 215 / 255, false, 0.85f);
  solid(dev, s, x0, top - 22.6f, x1, top - 23.6f, argb(ha * 170 / 255, kLineRgb));
  beam(dev, s, x0 - 10, x1 + 10, top - 23.1f, 9, ha * 190 / 255, kLineRgb);

  const uint32_t ms = net::now_ms() % 1000000;
  const float blink = 0.5f + 0.5f * std::fabs(std::sin(float(ms) * 0.004f));
  const float lean = kCardH * kLean;
  for (size_t i = 0; i < p.lines.size(); ++i) {
    const PanelLine& l = p.lines[i];
    const float e = ease(intro * 1.9f - float(i) * 0.17f);
    if (e <= 0) continue;
    const float dx = (1.0f - e) * 36.0f;
    const float ctop = top - kPanelHead - 3 - float(i) * kPanelPitch, cbot = ctop - kCardH, mid = (ctop + cbot) * 0.5f;
    const float cx0 = x0 + dx, cx1 = x1 - lean + dx;
    const int ca = int(a * e * (l.free ? 0.5f : 1.0f));
    // The card: lit in its colour at the left, a strip of it along the edge, light along the top.
    bar(dev, s, cx0, ctop, cx1, cbot,
        argb(ca * (l.own ? 240 : 215) / 255, blend(0x000000, l.rgb, l.own ? 0.52f : l.free ? 0.10f : 0.34f)),
        argb(ca * 150 / 255, 0x030A03), lean);
    const DWORD strip = argb(l.pulse ? int(ca * blink) : ca, l.rgb);
    bar(dev, s, cx0, ctop, cx0 + 3.5f, cbot, strip, strip, lean);
    const DWORD lit = argb(ca * (l.own ? 235 : 150) / 255, blend(l.rgb, 0xFFFFFF, l.own ? 0.45f : 0.2f));
    const DWORD dim = argb(ca * 30 / 255, l.rgb);
    const Pt edge[4] = {{cx0 + lean, ctop, lit}, {cx1 + lean, ctop, dim}, {cx1 + lean - 0.25f, ctop - 1.1f, dim},
                        {cx0 + lean - 0.25f, ctop - 1.1f, lit}};
    poly(dev, s, edge, 4);
    if (l.own) beam(dev, s, cx0 + lean - 6, cx0 + lean + 120, ctop, 7, ca * 150 / 255, l.rgb);
    // Its number in a hexagon.
    const float bx = cx0 + 20 + lean * 0.5f;
    icon(dev, s, "gen:hex", bx, mid, 8.8f, ca * 235 / 255, 0x061006);
    icon(dev, s, "gen:ring", bx, mid, 8.8f, l.pulse ? int(ca * (0.5f + 0.5f * blink)) : ca, l.rgb);
    draw_words(dev, s, words(dev, l.number, true), bx, mid, ca, true, 0.8f);
    // At the right: the icon, the state before it.
    float right = cx1 + lean * 0.5f - 7;
    if (l.icon == kIconDots) {
      for (int d = 0; d < 3; ++d) {
        const float w = std::sin(float(ms) * 0.0062f - float(d) * 0.95f);
        icon(dev, s, "gen:dot", right - 14 + float(d) * 6.5f, mid, 2.3f, int(ca * (0.3f + 0.7f * std::max(0.0f, w))), l.rgb);
      }
      right -= 21;
    } else if (l.icon != kIconNone) {
      icon(dev, s, l.icon == kIconTick ? "gen:tick" : "gen:star", right - 6, mid, 6.4f, ca, l.rgb);
      right -= 16;
    }
    Words st = words(dev, l.state, false);
    const float ss = 0.62f, sx = right - st.width() * ss;
    draw_words(dev, s, st, sx, mid, ca, false, ss, l.rgb);
    // The name (and the detail after it) in the room left.
    Words nm = words(dev, l.name, true), dt = words(dev, l.detail, true);
    const float nx = bx + 13, room = sx - 8 - nx;
    float ns = 0.9f, ds = 0.62f;
    const float need = nm.width() * ns + (dt.t ? 6 + dt.width() * ds : 0);
    if (need > room && need > 0) {
      const float k = std::max(0.5f, room / need);
      ns *= k, ds *= k;
    }
    draw_words(dev, s, nm, nx, mid, ca, false, ns);
    if (dt.t) draw_words(dev, s, dt, nx + nm.width() * ns + 6, mid - 1, ca * 170 / 255, false, ds);
  }
}

// The lines at the bottom: what happens now (the page's status) and what the
// selected row does (its hint), on a plate in the status's colour. It comes up
// from below.
void draw_info(IDirect3DDevice9* dev, const Screen& s, const Open& o, int a) {
  const Page& p = o.page;
  if (a <= 0) return;
  std::string hint;
  if (o.cursor == rows_total(p) - 1)
    hint = p.ok_hint;
  else if (o.cursor >= 0 && o.cursor < (int)p.rows.size() && p.rows[o.cursor].hint)
    hint = p.rows[o.cursor].hint();
  Status st = p.status ? p.status() : Status{};
  const int lines = (hint.empty() ? 0 : 1) + (st.letters.empty() ? 0 : 1);
  if (!lines) return;
  const float e = ease(o.intro * 1.4f), dy = -(1.0f - e) * 14.0f;
  const int ia = int(a * e);
  const float x0 = kInfoX0, x1 = kInfoX1, bottom = kInfoBottom + dy, top = bottom + lines * kInfoLine + 6;
  plate(dev, s, x0, top, x1, bottom, ia, st.letters.empty() ? kLineRgb : DWORD(st.rgb));
  const float room = x1 - x0 - 30;
  float y = top - 3 - kInfoLine * 0.5f;
  auto left = [&](float at) { return x0 + 13 + (at - bottom) * kLean; };  // (along the leaning edge)
  if (!st.letters.empty()) {
    Words w = words(dev, st.letters, false);
    draw_words(dev, s, w, left(y), y, ia, false, std::min(0.8f, room / std::max(1.0f, w.width())), st.rgb);
    y -= kInfoLine;
  }
  if (!hint.empty()) {
    Words w = words(dev, hint, true);
    draw_words(dev, s, w, left(y), y, ia, false, std::min(0.82f, room / std::max(1.0f, w.width())), kHintRgb);
  }
}

void draw_page(IDirect3DDevice9* dev, const Screen& s, const Open& o) {
  int a = (int)o.alpha;
  if (a <= 0) return;
  const Page& p = o.page;
  Words title = words(dev, p.title, false);
  draw_words(dev, s, title, kTitleX, kTitleMid, a, false, kTitleScale);
  // A line of light under the title.
  const float title_end = kTitleX + title.width() * kTitleScale;
  solid(dev, s, kTitleX - 4, kTitleMid - 21.4f, title_end + 6, kTitleMid - 22.2f, argb(a * 120 / 255, kLineRgb));
  beam(dev, s, kTitleX - 16, title_end + 22, kTitleMid - 21.8f, 8, a * 170 / 255, kLineRgb);
  // Light behind the selected row, with the cursor's pulse.
  if (o.cursor >= 0 && o.cursor < (int)p.rows.size()) {
    const float ry = kRowY[slot_of(p, o.cursor)] + kCursorLift, half = 13.0f, lean = 2 * half * kLean;
    const float x0 = kCursorX - 6, x1 = p.panel ? kPanelX0 - 14 : 70.0f;
    const DWORD lit = argb(a * (26 + o.pulse.alpha() * 34 / 255) / 255, kLineRgb), none = argb(0, kLineRgb);
    const Pt band[4] = {{x0, ry - half, lit}, {x0 + lean, ry + half, lit}, {x1 + lean, ry + half, none}, {x1, ry - half, none}};
    poly(dev, s, band, 4, true);
  }

  size_t n = std::min<size_t>(p.rows.size(), kMaxRows);
  for (size_t i = 0; i < n; ++i)
    if (shown(p, (int)i))
      draw_row(dev, s, p.rows[i], o.rows[i], kRowY[slot_of(p, (int)i)], a, kValueX, kLettersX,
               p.panel ? kPanelX0 - 10 - kLettersX : 0);

  float oy = ok_y(p);
  const float h = kOkSize * 0.5f;
  quad(dev, s, texture(dev, "pic:op_soundset_ok"), kOkX - h, oy + h, kOkX + h, oy - h, a);

  if (p.panel) draw_panel(dev, s, p.panel(), a, o.intro);
  draw_info(dev, s, o, a);

  // The cursor, added like light (0x48d77f).
  float cy = o.cursor == rows_total(p) - 1 ? oy : kRowY[slot_of(p, o.cursor)] + kCursorLift;
  const float c = kCursorSize * 0.5f;
  quad(dev, s, texture(dev, "pic:op_cursor"), kCursorX - c, cy + c, kCursorX + c, cy - c, o.pulse.alpha() * a / 255,
       true);
}

}  // namespace

void install(const Config& cfg) { g_cfg = cfg; }

void set_play_sound(int (*play)(int id)) { g_play = play; }

void set_host(Host host, std::function<void()> on_closed) {
  if (!g_stack.empty()) return;  // (only before the first page)
  g_host = host;
  g_on_closed = std::move(on_closed);
}

void open(Page page) {
  if (g_stack.empty()) {
    g_leaving.clear();
    if (g_host == Host::MainMenu) {
      // Like a new screen: black over the menu, then the page out of black.
      g_stack.push_back(make(std::move(page), 255));
      g_stage = Stage::Darken;
    } else {
      g_stack.push_back(make(std::move(page), 0));
    }
  } else {
    g_stack.back().target = 0;
    g_stack.push_back(make(std::move(page), 0));
  }
  g_last_ms = 0;
  LOGI("native menu: page \"%s\" opened", g_stack.back().page.title.c_str());
}

bool active() {
  if (g_host == Host::MainMenu) return g_stage != Stage::Idle;
  return !g_stack.empty() || !g_leaving.empty();
}

bool covers_screen() {
  return g_host == Host::MainMenu && (g_stage == Stage::Show || g_stage == Stage::Open || g_stage == Stage::Hide);
}

void reset() {
  g_stack.clear();
  g_leaving.clear();
  g_stage = Stage::Idle;
  g_curtain = 0;
  g_on_closed = nullptr;
}

void close_all() {
  if (g_host != Host::MainMenu || g_stack.empty() || g_stage == Stage::Hide || g_stage == Stage::Lighten) return;
  while (g_stack.size() > 1) {  // the pages over the first fade out
    Open& top = g_stack.back();
    if (top.page.on_close) top.page.on_close();
    top.target = 0;
    g_leaving.push_back(std::move(top));
    g_stack.pop_back();
  }
  g_stage = Stage::Hide;  // the black over them, then the menu (advance)
  LOGI("native menu: pages closed for the game together");
}

bool update(uint32_t pressed) {
  if (!active()) return false;
  if (!ready()) return true;  // fading: the buttons wait
  Open& o = g_stack.back();
  Page& p = o.page;
  bool on_ok = o.cursor == rows_total(p) - 1;
  if (pressed & kUp) {
    move(o, -1);
    sound(kSoundMove);
  } else if (pressed & kDown) {
    move(o, 1);
    sound(kSoundMove);
  } else if ((pressed & (kLeft | kRight)) && !on_ok) {
    row_input(p.rows[o.cursor], pressed & (kLeft | kRight));
  } else if (pressed & kDecide) {
    if (on_ok) {
      sound(kSoundDecide);
      close_top();
    } else {
      row_input(p.rows[o.cursor], kDecide);
    }
  } else if (pressed & kCancel) {
    sound(kSoundCancel);
    close_top();
  }
  return true;
}

void back() { close_top(); }

bool replace(Page page) {
  if (g_host != Host::MainMenu || g_stack.empty() || g_stage != Stage::Open) return false;
  for (Open& o : g_stack) {
    if (o.page.on_close) o.page.on_close();
    o.target = 0;
    g_leaving.push_back(std::move(o));
  }
  g_stack.clear();
  g_stack.push_back(make(std::move(page), 0));
  g_last_ms = 0;
  LOGI("native menu: page \"%s\" in the place of the pages open", g_stack.back().page.title.c_str());
  return true;
}

void draw() {
  if (!active()) return;
  trim_textures();
  advance();
  if (!active()) return;
  IDirect3DDevice9* dev = overlay::device();
  if (!dev) return;
  D3DVIEWPORT9 vp{};
  if (FAILED(dev->GetViewport(&vp)) || !vp.Width || !vp.Height) return;
  Screen back = fit(vp, widescreen::pictures_pillarboxed()), s = fit(vp, widescreen::hud_centered());
  const bool wide = widescreen::menus_wide();  // (its background goes on to the screen's edges: no bars)
  if (covers_screen() && widescreen::pictures_pillarboxed() && !wide && vp.Width > vp.Height * 4.0f / 3.0f + 1.0f)
    ambient::note_pillarbox();

  IDirect3DStateBlock9* saved = nullptr;
  if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved))) return;
  begin_2d(dev);

  if (covers_screen()) {
    // The options screen's background: op_bg's 512x480 of its 1024x1024
    // (v to 0.9375), and op_kame at (91, -128), 512x256, cut at the edges.
    quad(dev, back, texture(dev, "pic:op_bg"), -256, 240, 256, -240, 255, false, 0, 0, 1, 0.9375f);
    if (wide) {  // (mirrored at the 4:3 edges, as widescreen.cpp does for the game's menus)
      quad(dev, back, texture(dev, "pic:op_bg"), -768, 240, -256, -240, 255, false, 1, 0, 0, 0.9375f);
      quad(dev, back, texture(dev, "pic:op_bg"), 256, 240, 768, -240, 255, false, 1, 0, 0, 0.9375f);
    }
    quad(dev, s, texture(dev, "pic:op_kame"), -165, 0, 256, -240, 255, false, 0, 0, 421.0f / 512.0f, 0.9375f);
  }
  if (g_host == Host::Options || covers_screen()) {
    for (const Open& o : g_stack) draw_page(dev, s, o);
    for (const Open& o : g_leaving) draw_page(dev, s, o);
  }
  if (g_host == Host::MainMenu && g_curtain > 0) {
    Screen whole = fit(vp, false);  // the black covers the side bars too
    solid(dev, whole, -256, 240, 256, -240, DWORD(std::min(255.0f, g_curtain)) << 24);
  }

  saved->Apply();
  saved->Release();
}

// --- tmntnet's rows inside the game's own pages ------------------------------------

namespace {
struct Embedded {
  std::vector<Row> rows;
  std::vector<float> ys;
  float value_x = kValueX, letters_x = kLettersX;
  std::vector<RowState> state;
  uint32_t last_ms = 0;
};
std::map<int, Embedded> g_embedded;
}  // namespace

void embed(int page, std::vector<Row> rows, std::vector<float> ys, float value_x, float letters_x) {
  Embedded& e = g_embedded[page];
  e.rows = std::move(rows);
  e.ys = std::move(ys);
  e.value_x = value_x;
  e.letters_x = letters_x;
  e.state = make_states(e.rows);
  e.last_ms = 0;
}

void embed_select(int page, int row) {
  auto it = g_embedded.find(page);
  if (it != g_embedded.end()) select_arrows(it->second.state, row);
}

bool embed_input(int page, int row, uint32_t pressed) {
  auto it = g_embedded.find(page);
  if (it == g_embedded.end() || row < 0 || row >= (int)it->second.rows.size()) return false;
  return row_input(it->second.rows[row], pressed);
}

void embed_draw(int page, int alpha) {
  auto it = g_embedded.find(page);
  if (it == g_embedded.end()) return;
  Embedded& e = it->second;
  step_rows(e.rows, e.state, frames_since(e.last_ms));
  if (alpha <= 0) return;
  IDirect3DDevice9* dev = overlay::device();
  D3DVIEWPORT9 vp{};
  if (!dev || FAILED(dev->GetViewport(&vp)) || !vp.Width || !vp.Height) return;
  Screen s = fit(vp, widescreen::hud_centered());
  IDirect3DStateBlock9* saved = nullptr;
  if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved))) return;
  begin_2d(dev);
  for (size_t i = 0; i < e.rows.size() && i < e.ys.size(); ++i)
    draw_row(dev, s, e.rows[i], e.state[i], e.ys[i], std::min(alpha, 255), e.value_x, e.letters_x);
  saved->Apply();
  saved->Release();
}

Picture letters_picture(const std::string& text) {
  Picture out;
  IDirect3DDevice9* dev = overlay::device();
  if (!dev || !game_text::ready()) return out;
  if (const Tex* t = texture(dev, "txt:" + text)) out.texture = t->t, out.w = t->w, out.h = t->h, out.text_w = t->text_w;
  return out;
}

// --- notices ---------------------------------------------------------------------

namespace {
struct Notice {
  std::string letters, value;
  uint32_t until;
};
std::mutex g_notice_lock;
std::vector<Notice> g_notices;
std::string g_badge;
std::string g_status, g_status_value;
Prompt g_prompt;
uint32_t g_prompt_ms = 0;  // when it was set last (0: never)
bool prompt_live(uint32_t now) { return g_prompt_ms && now - g_prompt_ms < 150; }
}  // namespace

void show_prompt(const Prompt& p) {
  std::lock_guard<std::mutex> lock(g_notice_lock);
  g_prompt = p;
  g_prompt_ms = net::now_ms() ? net::now_ms() : 1;
}

void notify(const std::string& letters, const std::string& value) {
  std::lock_guard<std::mutex> lock(g_notice_lock);
  g_notices.push_back(Notice{letters, latin(value), net::now_ms() + 3500});
  if (g_notices.size() > 3) g_notices.erase(g_notices.begin());
}

void set_badge(const std::string& plain) {
  std::lock_guard<std::mutex> lock(g_notice_lock);
  g_badge = plain;
}

void set_status(const std::string& letters, const std::string& value) {
  std::lock_guard<std::mutex> lock(g_notice_lock);
  if (letters != g_status && !letters.empty()) LOGI("status line: %s %s", letters.c_str(), value.c_str());
  g_status = letters;
  g_status_value = letters.empty() ? std::string() : latin(value);
}

bool has_notices() {
  std::lock_guard<std::mutex> lock(g_notice_lock);
  return !g_notices.empty() || !g_badge.empty() || !g_status.empty() || prompt_live(net::now_ms());
}

void draw_notices(IDirect3DDevice9* dev) {
  std::vector<Notice> live;
  std::string badge, status, status_value;
  Prompt prompt;
  bool prompt_on = false;
  {
    std::lock_guard<std::mutex> lock(g_notice_lock);
    uint32_t now = net::now_ms();
    g_notices.erase(std::remove_if(g_notices.begin(), g_notices.end(),
                                   [now](const Notice& n) { return (int32_t)(n.until - now) <= 0; }),
                    g_notices.end());
    live = g_notices;
    badge = g_badge;
    status = g_status;
    status_value = g_status_value;
    prompt_on = prompt_live(now);
    if (prompt_on) prompt = g_prompt;
  }
  if (live.empty() && badge.empty() && status.empty() && !prompt_on) return;
  IDirect3DSurface9* bb = nullptr;
  D3DSURFACE_DESC d{};
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
  bb->GetDesc(&d);
  bb->Release();
  // In the 4:3 middle of the screen, in the layout's units.
  Screen s;
  float W = (float)d.Width, H = (float)d.Height, w43 = std::min(W, H * 4.0f / 3.0f);
  s.left = (W - w43) * 0.5f, s.ux = w43 / 512.0f, s.uy = H / 480.0f;

  IDirect3DStateBlock9* saved = nullptr;
  if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved))) return;
  D3DVIEWPORT9 vp{0, 0, d.Width, d.Height, 0.0f, 1.0f};
  dev->SetViewport(&vp);
  begin_2d(dev);

  uint32_t now = net::now_ms();
  float y = 210;  // the first line's middle
  auto line = [&](const std::string& letters, const std::string& value, int a) {
    Words t = words(dev, letters, false), v = words(dev, value, true);
    float total = t.width() + (v.t ? 10 + v.width() : 0), x = -total * 0.5f;
    plate(dev, s, x - 17, y + 14, x + total + 10, y - 14, a, kLineRgb, 200);
    draw_words(dev, s, t, x, y, a, false);
    if (v.t) draw_words(dev, s, v, x + t.width() + 10, y, a, false);
    y -= 34;
  };
  const bool alone = prompt_on && prompt.alone;
  if (!alone && !status.empty()) line(status, status_value, 255);
  for (const Notice& n : live) {
    if (alone) break;
    uint32_t left = n.until - now;
    line(n.letters, n.value, left < 400 ? int(255 * left / 400) : 255);
  }
  if (prompt_on) {
    // A plate over the select's middle panel (under it: the picker's own
    // label; every turtle stays in view): the title and the count, the value
    // between two arrows that pulse (in two lines when long), the hints.
    const float x0 = -88, x1 = 76, top = 68, bottom = -58;
    bar(dev, s, x0, top, x1, bottom, argb(255, 0x05060A), argb(255, 0x05060A), (top - bottom) * kLean);
    plate(dev, s, x0, top, x1, bottom, 255, prompt.rgb, 235);
    auto at = [&](float py) { return (py - bottom) * kLean; };  // (the leaning edges at that height)
    auto middle = [&](float py) { return (x0 + x1) * 0.5f + at(py); };
    std::vector<std::string> hints;  // ('|' between the lines)
    for (size_t from = 0; from <= prompt.hint.size();) {
      size_t bar_at = prompt.hint.find('|', from);
      if (bar_at == std::string::npos) bar_at = prompt.hint.size();
      if (bar_at > from) hints.push_back(prompt.hint.substr(from, bar_at - from));
      from = bar_at + 1;
    }
    const float ty = top - 14, pitch = 11.5f, hints_top = bottom + 1 + pitch * (float)hints.size();
    draw_words(dev, s, words(dev, prompt.title, false), x0 + 12 + at(ty), ty, 255, false, 0.72f);
    Words count = words(dev, prompt.count, true);
    if (count.t) draw_words(dev, s, count, x1 - 9 + at(ty) - count.width() * 0.8f, ty, 215, false, 0.8f);
    const float rule = top - 27;
    solid(dev, s, x0 + 8 + at(rule), rule + 0.4f, x1 - 6 + at(rule), rule - 0.4f, argb(120, prompt.rgb));
    const float vy = (rule + hints_top) * 0.5f, room = x1 - x0 - 52;
    Words v = words(dev, prompt.value, true);
    const size_t space = prompt.value.find(' ');
    float half = 0;
    if (space != std::string::npos && v.width() > room) {
      Words a = words(dev, prompt.value.substr(0, space), true), b = words(dev, prompt.value.substr(space + 1), true);
      const float wide = std::max(1.0f, std::max(a.width(), b.width())), vs = std::min(1.15f, room / wide);
      draw_words(dev, s, a, middle(vy + 11), vy + 11, 255, true, vs, prompt.rgb);
      draw_words(dev, s, b, middle(vy - 11), vy - 11, 255, true, vs, prompt.rgb);
      half = wide * vs * 0.5f;
    } else {
      const float vs = std::min(1.5f, room / std::max(1.0f, v.width()));
      draw_words(dev, s, v, middle(vy), vy, 255, true, vs, prompt.rgb);
      half = v.width() * vs * 0.5f;
    }
    const int pa = 120 + int(135 * std::fabs(std::sin(float(now % 100000) * 0.005f)));
    const float cx = middle(vy), ax = half + 11, aw = 7, ah = 8;
    const Pt left_arrow[3] = {{cx - ax - aw, vy, argb(pa, 0xFFFFFF)}, {cx - ax, vy + ah, argb(pa, 0xFFFFFF)}, {cx - ax, vy - ah, argb(pa, 0xFFFFFF)}};
    const Pt right_arrow[3] = {{cx + ax + aw, vy, argb(pa, 0xFFFFFF)}, {cx + ax, vy - ah, argb(pa, 0xFFFFFF)}, {cx + ax, vy + ah, argb(pa, 0xFFFFFF)}};
    if (prompt.arrows) {
      poly(dev, s, left_arrow, 3);
      poly(dev, s, right_arrow, 3);
    }
    for (size_t k = 0; k < hints.size(); ++k) {
      const float hy = hints_top - pitch * (float(k) + 0.5f);
      Words hint = words(dev, hints[k], true);
      if (!hint.t) continue;
      const float hs = std::min(0.62f, (x1 - x0 - 24) / std::max(1.0f, hint.width()));
      draw_words(dev, s, hint, middle(hy), hy, 235, true, hs, kHintRgb);
    }
  }
  if (!badge.empty()) {
    Words b = words(dev, badge, true);
    const float scale = 0.7f;
    if (b.t) {
      float w = b.width() * scale, x = kRightEdge - 4 - w, mid = 228;
      solid(dev, s, x - 5, mid + 8, x + w + 5, mid - 8, 0x70000000);
      draw_words(dev, s, b, x, mid, 230, false, scale);
    }
  }
  saved->Apply();
  saved->Release();
}

void draw_tags(IDirect3DDevice9* dev, const std::vector<Tag>& tags) {
  if (tags.empty()) return;
  IDirect3DSurface9* bb = nullptr;
  D3DSURFACE_DESC d{};
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
  bb->GetDesc(&d);
  bb->Release();
  IDirect3DStateBlock9* saved = nullptr;
  if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved))) return;
  D3DVIEWPORT9 vp{0, 0, d.Width, d.Height, 0.0f, 1.0f};
  dev->SetViewport(&vp);
  begin_2d(dev);
  // Units are pixels: layout x = pixel x, layout y = -pixel y.
  Screen s;
  s.left = -256.0f, s.top = -240.0f, s.ux = 1.0f, s.uy = 1.0f;
  struct Plate {
    Words w;
    float scale, pad, left, right, top, bottom;
  };
  std::vector<Plate> plates;
  for (const Tag& t : tags) {
    Words w = words(dev, t.text, true);
    if (!w.t || t.alpha <= 0) {
      plates.push_back(Plate{});
      continue;
    }
    const float h = t.size, pad = std::max(2.0f, h * 0.3f);
    const float scale = h / (23.0f * w.texel());  // the plain font's letters are 23 texels high
    const float half = w.width() * scale * 0.5f;
    Plate p{w, scale, pad, t.x - half - pad, t.x + half + pad, t.y - h - 2 * pad, t.y};
    // Turtles side by side: a plate that would cover an earlier one goes on top of it.
    for (const Plate& q : plates)
      if (q.w.t && p.left < q.right && q.left < p.right && p.top < q.bottom && q.top < p.bottom) {
        float up = p.bottom - (q.top - 2.0f);
        p.top -= up, p.bottom -= up;
      }
    plates.push_back(p);
  }
  for (size_t i = 0; i < tags.size(); ++i) {
    const Plate& p = plates[i];
    if (!p.w.t) continue;
    const Tag& t = tags[i];
    solid(dev, s, p.left, -p.top, p.right, -p.bottom, DWORD(t.alpha * 150 / 255) << 24);
    draw_words(dev, s, p.w, t.x, -(p.top + p.pad + t.size * 0.5f), t.alpha, true, p.scale, t.rgb);
  }
  saved->Apply();
  saved->Release();
}

void draw_hud(IDirect3DDevice9* dev, const std::vector<HudSprite>& sprites) {
  if (sprites.empty()) return;
  IDirect3DSurface9* bb = nullptr;
  D3DSURFACE_DESC d{};
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
  bb->GetDesc(&d);
  bb->Release();
  IDirect3DStateBlock9* saved = nullptr;
  if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved))) return;
  D3DVIEWPORT9 vp{0, 0, d.Width, d.Height, 0.0f, 1.0f};
  dev->SetViewport(&vp);
  begin_2d(dev);
  const Screen mid = fit(vp, widescreen::hud_centered());
  Screen left = mid, right = mid;  // the corner pieces ([video] hud_corners)
  if (widescreen::hud_corners()) {
    float W = (float)vp.Width, w43 = vp.Height * 4.0f / 3.0f;
    if (W > w43 + 1.0f) left.left = (float)vp.X, right.left = vp.X + (W - w43);
  }
  for (const HudSprite& h : sprites) {
    const Screen& s = h.edge < 0 ? left : h.edge > 0 ? right : mid;
    // (the layout's y is up, the play screen's down)
    quad(dev, s, texture(dev, std::string("pic:") + h.picture), h.x - h.w * 0.5f, -(h.y - h.h * 0.5f),
         h.x + h.w * 0.5f, -(h.y + h.h * 0.5f), h.alpha, false, h.u0, h.v0, h.u1, h.v1, h.rgb);
  }
  saved->Apply();
  saved->Release();
}

void draw_px(IDirect3DDevice9* dev, const std::vector<HudSprite>& sprites) {
  if (sprites.empty()) return;
  IDirect3DSurface9* bb = nullptr;
  D3DSURFACE_DESC d{};
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
  bb->GetDesc(&d);
  bb->Release();
  IDirect3DStateBlock9* saved = nullptr;
  if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved))) return;
  D3DVIEWPORT9 vp{0, 0, d.Width, d.Height, 0.0f, 1.0f};
  dev->SetViewport(&vp);
  begin_2d(dev);
  Screen s;  // layout units = pixels: px(x) = x, py(-y) = y
  s.left = -256.0f;
  s.top = -240.0f;
  for (const HudSprite& h : sprites) {
    float x0 = h.x - h.w * 0.5f, x1 = h.x + h.w * 0.5f, y0 = h.y - h.h * 0.5f, y1 = h.y + h.h * 0.5f;
    if (h.picture)
      quad(dev, s, texture(dev, std::string("pic:") + h.picture), x0, -y0, x1, -y1, h.alpha, false, h.u0, h.v0, h.u1,
           h.v1, h.rgb);
    else
      solid(dev, s, x0, -y0, x1, -y1, DWORD(std::clamp(h.alpha, 0, 255)) << 24 | (h.rgb & 0xFFFFFF));
  }
  saved->Apply();
  saved->Release();
}

void draw_stars(IDirect3DDevice9* dev, const std::vector<Star>& stars) {
  if (stars.empty()) return;
  IDirect3DSurface9* bb = nullptr;
  D3DSURFACE_DESC d{};
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
  bb->GetDesc(&d);
  bb->Release();
  IDirect3DStateBlock9* saved = nullptr;
  if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved))) return;
  D3DVIEWPORT9 vp{0, 0, d.Width, d.Height, 0.0f, 1.0f};
  dev->SetViewport(&vp);
  begin_2d(dev);
  dev->SetTexture(0, nullptr);
  dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
  dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
  dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  auto star = [&](const Star& s, float r, DWORD argb) {
    Vtx v[12];
    v[0] = {s.x, s.y, 0, 1, argb, 0, 0};
    for (int i = 0; i <= 10; ++i) {
      float a = s.turn + i * 3.14159265f / 5.0f - 3.14159265f / 2.0f;
      float rr = (i % 2) ? r * 0.45f : r;
      v[i + 1] = {s.x + std::cos(a) * rr, s.y + std::sin(a) * rr, 0, 1, argb, 0, 0};
    }
    dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 10, v, sizeof(Vtx));
  };
  for (const Star& s : stars) {
    int a = std::clamp(s.alpha, 0, 255);
    star(s, s.r + std::max(1.5f, s.r * 0.22f), DWORD(a * 3 / 4) << 24);  // the dark outline
    star(s, s.r, DWORD(a) << 24 | (s.rgb & 0xFFFFFF));
  }
  saved->Apply();
  saved->Release();
}

std::string latin(const std::string& utf8) {
  static const char* kRu[] = {"A", "B", "V", "G", "D", "E", "ZH", "Z", "I", "Y", "K", "L", "M", "N", "O", "P",
                              "R", "S", "T", "U", "F", "KH", "TS", "CH", "SH", "SCH", "", "Y", "", "E", "YU", "YA"};
  std::string out;
  for (size_t i = 0; i < utf8.size();) {
    unsigned char c = (unsigned char)utf8[i];
    if (c < 0x80) {
      out += (char)c;
      ++i;
      continue;
    }
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    uint32_t cp = c & (0x3F >> n);
    for (int k = 1; k <= n && i + k < utf8.size(); ++k) cp = (cp << 6) | ((unsigned char)utf8[i + k] & 63);
    i += n + 1;
    if (cp >= 0x410 && cp <= 0x42F) out += kRu[cp - 0x410];
    else if (cp >= 0x430 && cp <= 0x44F) out += kRu[cp - 0x430];
    else if (cp == 0x401 || cp == 0x451) out += "E";
    else out += '?';
  }
  if (out.size() > 28) out.resize(28);
  return out;
}

}  // namespace native_menu
