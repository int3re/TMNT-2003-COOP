#include "mod/csel_art.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "common/log.h"
#include "mod/csel.h"
#include "mod/game_text.h"
#include "mod/nametags.h"
#include "mod/overlay.h"
#include "mod/widescreen.h"

namespace csel_art {
namespace {

// --- digits drawn like the game's ----------------------------------------------
// A digit's strokes in a box of 1 x 1 (x right, y down, 1 = cap height), as
// the game's neon and tag lettering has them: one even stroke with round ends.
struct Pt {
  float x, y;
};
using Line = std::vector<Pt>;

Line arc(float cx, float cy, float rx, float ry, float a0, float a1, int n = 24) {
  Line out;
  for (int i = 0; i <= n; ++i) {
    float t = (a0 + (a1 - a0) * i / n) * 3.14159265f / 180.0f;
    out.push_back({cx + rx * std::cos(t), cy + ry * std::sin(t)});
  }
  return out;
}

std::vector<Line> digit(char c) {
  switch (c) {
    case '2': {
      Line l = arc(0.46f, 0.27f, 0.36f, 0.25f, -170, 25);
      l.push_back({0.04f, 0.97f});
      l.push_back({0.86f, 0.97f});
      return {l};
    }
    case '3':
      return {arc(0.42f, 0.26f, 0.34f, 0.22f, -160, 95), arc(0.44f, 0.71f, 0.38f, 0.26f, -95, 160)};
    case '4':
      return {{{0.66f, 0.03f}, {0.02f, 0.68f}, {0.90f, 0.68f}}, {{0.66f, 0.03f}, {0.66f, 0.97f}}};
    default:
      return {};
  }
}

float seg_dist(float px, float py, Pt a, Pt b) {
  float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
  float t = l2 > 0 ? std::clamp(((px - a.x) * dx + (py - a.y) * dy) / l2, 0.0f, 1.0f) : 0.0f;
  return std::hypot(px - (a.x + t * dx), py - (a.y + t * dy));
}

struct Rgb {
  float r, g, b;
};

// Puts colour c with coverage k over pixel p (straight alpha, 0xAARRGGBB).
void over(uint32_t& p, Rgb c, float k) {
  if (k <= 0) return;
  float a = (p >> 24) / 255.0f, na = k + a * (1 - k);
  auto ch = [&](int shift, float v) {
    float old = (float)((p >> shift) & 255);
    return (uint32_t)std::lround(std::clamp((v * k + old * a * (1 - k)) / na, 0.0f, 255.0f)) << shift;
  };
  p = (uint32_t)std::lround(na * 255) << 24 | ch(16, c.r) | ch(8, c.g) | ch(0, c.b);
}

// The digit with its box's left edge at x0 on the baseline, its top at `top`;
// it leans right by `slant` (x per y). A glow of `glow_a` fades out by `sigma`.
void draw_digit(game_text::Bitmap& b, char c, float x0, float top, float height, float width, float slant,
                float stroke, Rgb core, const Rgb* glow, float glow_a, float sigma) {
  std::vector<Line> lines = digit(c);
  std::vector<std::pair<Pt, Pt>> segs;
  float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
  for (const Line& l : lines)
    for (size_t i = 0; i + 1 < l.size(); ++i) {
      Pt a{x0 + l[i].x * width, top + l[i].y * height}, z{x0 + l[i + 1].x * width, top + l[i + 1].y * height};
      segs.push_back({a, z});
      for (Pt q : {a, z}) minx = std::min(minx, q.x), maxx = std::max(maxx, q.x), miny = std::min(miny, q.y), maxy = std::max(maxy, q.y);
    }
  float base = top + height;
  int margin = (int)(stroke + 4 * sigma + 4);
  int y0 = std::max(0, (int)miny - margin), y1 = std::min(b.h, (int)maxy + margin);
  int xa = std::max(0, (int)(minx - slant * height) - margin), xb = std::min(b.w, (int)(maxx + slant * height) + margin);
  for (int y = y0; y < y1; ++y)
    for (int x = xa; x < xb; ++x) {
      float ux = x - slant * (base - y), d = 1e9f;
      for (const auto& s : segs) d = std::min(d, seg_dist(ux, (float)y, s.first, s.second));
      uint32_t& p = b.bgra[size_t(y) * b.w + x];
      if (glow) {
        float e = std::max(0.0f, d - stroke / 2);
        over(p, *glow, glow_a / 255.0f * std::exp(-e * e / (2 * sigma * sigma)));
      }
      over(p, core, std::clamp(stroke / 2 + 0.5f - d, 0.0f, 1.0f));
    }
}

// --- the pictures ----------------------------------------------------------------
// Measured in the game's: csel_st_p2 (1024 x 256) has its "2" in x 682..745,
// y 93..169, a 12-texel stroke leaning 0.139, a (254, 0, 239) glow; csel_selct
// (256 x 64) its "1" in x 185..196 (from 182 at the top, leaning 0.27), y 23..41,
// a 3.7-texel black stroke on the white of the tag.
constexpr int kLabelW = 1024, kLabelH = 256, kTagW = 256, kTagH = 64;

void label(const game_text::Bitmap& two, char c, Rgb glow, game_text::Bitmap& out) {
  out = two;
  for (uint32_t& p : out.bgra) {  // the glow had no green: green is how white a texel is
    float t = ((p >> 8) & 255) / 255.0f;
    auto ch = [t](float v) { return (uint32_t)std::lround(v + (255 - v) * t); };
    p = (p & 0xFF000000u) | ch(glow.r) << 16 | ch(glow.g) << 8 | ch(glow.b);
  }
  for (int y = 0; y < out.h; ++y)  // away with the 2 and its glow
    for (int x = 664; x < 800 && x < out.w; ++x) out.bgra[size_t(y) * out.w + x] &= 0x00FFFFFFu;
  draw_digit(out, c, 676, 93, 76, 62, 0.139f, 12.5f, {255, 255, 255}, &glow, 70, 6.0f);
}

// csel_pl2 (256 x 256): "ИГРОК 2" over "ПОМОЩЬ", its 2 in x 182..202, y 50..73,
// a 4.6-texel white stroke leaning 0.24 with a faint white glow.
constexpr int kPlate = 256;

void plate(const game_text::Bitmap& two, char c, game_text::Bitmap& out) {
  out = two;
  for (int y = 40; y < 82; ++y)
    for (int x = 176; x < 214; ++x) out.bgra[size_t(y) * out.w + x] &= 0x00FFFFFFu;
  Rgb white{255, 255, 255};
  draw_digit(out, c, 182, 50, 23.5f, 18, 0.24f, 4.6f, white, &white, 25, 1.6f);
}

void tag(const game_text::Bitmap& one, char c, game_text::Bitmap& out) {
  out = one;
  for (int y = 19; y < 46 && y < out.h; ++y)  // away with the 1: the tag's white again
    for (int x = (int)(182 - 0.27f * (y - 23)); x < 207 && x < out.w; ++x) {
      uint32_t& p = out.bgra[size_t(y) * out.w + x];
      if ((p >> 24) > 200) p = 0xFFFFFFFFu;
    }
  draw_digit(out, c, 181.5f, 23, 18.5f, 12.5f, 0.27f, 3.7f, {0, 0, 0}, nullptr, 0, 1);
}

// --- the label's own texture ------------------------------------------------------
// RwRaster (0x34 bytes in RW 3.x) is followed by the D3D9 driver's part, whose
// first field is the IDirect3DTexture9: found by its vtable (every texture of
// the device has the same), not by an offset.
bool readable(const void* p) {
  MEMORY_BASIC_INFORMATION m{};
  return p && VirtualQuery(p, &m, sizeof(m)) && m.State == MEM_COMMIT &&
         !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (m.Protect & 0xEE);
}

void* texture_vtable() {
  static void* vt = nullptr;
  if (vt) return vt;
  IDirect3DDevice9* dev = overlay::device();
  IDirect3DTexture9* t = nullptr;
  if (dev && SUCCEEDED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) {
    vt = *reinterpret_cast<void**>(t);
    t->Release();
  }
  return vt;
}

IDirect3DTexture9* d3d_texture_of(const uint8_t* raster) {
  void* vt = texture_vtable();
  if (!vt || !readable(raster)) return nullptr;
  for (size_t off = 0x20; off <= 0x80; off += 4) {
    uintptr_t p = *reinterpret_cast<const uintptr_t*>(raster + off);
    if (p < 0x10000 || (p & 3) || !readable(reinterpret_cast<void*>(p))) continue;
    if (*reinterpret_cast<void* const*>(p) == vt) return reinterpret_cast<IDirect3DTexture9*>(p);
  }
  return nullptr;
}

// The banner and the plate: whose they show, the game's pixels while ours are in.
struct Swap {
  const char* name;
  int w, h, first;  // size; the bitmap of player 3 (then 4)
  IDirect3DTexture9* tex = nullptr;
  std::vector<uint32_t> orig;
  int shown = 2;
};
Swap g_swaps[2] = {{"csel_st_p2", kLabelW, kLabelH, 0}, {"csel_pl2", kPlate, kPlate, 4}};

bool write_pixels(IDirect3DTexture9* t, int w, int h, const uint32_t* src, std::vector<uint32_t>* keep) {
  D3DSURFACE_DESC d{};
  if (FAILED(t->GetLevelDesc(0, &d)) || (d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_X8R8G8B8) ||
      (int)d.Width != w || (int)d.Height != h)
    return false;
  D3DLOCKED_RECT lr{};
  if (FAILED(t->LockRect(0, &lr, nullptr, 0))) return false;
  for (int y = 0; y < h; ++y) {
    uint8_t* row = static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch;
    if (keep) std::memcpy(&(*keep)[size_t(y) * w], row, size_t(w) * 4);
    std::memcpy(row, src + size_t(y) * w, size_t(w) * 4);
  }
  t->UnlockRect(0);
  return true;
}

void show(Swap& s, const uint8_t* rw_texture, int turn);

// ИГРОК 3, ИГРОК 4 (banners), ИГРОК 2, ИГРОК 3 (tags), ИГРОК 3, ИГРОК 4 (plates)
game_text::Bitmap g_bitmaps[6];
IDirect3DTexture9* g_textures[6] = {};
IDirect3DDevice9* g_dev = nullptr;
std::atomic<int> g_ready{0};  // 0 making, 1 made, -1 not possible

IDirect3DTexture9* texture(int k) {
  IDirect3DDevice9* dev = overlay::device();
  if (!dev || g_ready.load() != 1) return nullptr;
  if (g_dev != dev) {
    for (auto& t : g_textures)
      if (t) t->Release(), t = nullptr;
    g_dev = dev;
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

// --- the players' names -----------------------------------------------------------
constexpr int kNameW = 256, kNameH = 64;  // as the tag's picture: drawn in its box
struct NameTex {
  std::string text;
  int character;
  IDirect3DTexture9* tex;
};
std::vector<NameTex> g_names;
IDirect3DDevice9* g_names_dev = nullptr;
IDirect3DTexture9* g_name_now = nullptr;  // the next sprite's picture instead of its own

bool name_bitmap(const std::string& utf8, int character, game_text::Bitmap& out) {
  unsigned rgb = nametags::mask_rgb(character);
  const Rgb tint{(float)(rgb >> 16 & 255), (float)(rgb >> 8 & 255), (float)(rgb & 255)};
  game_text::Bitmap g;
  if (!game_text::plain(utf8, g) || g.text_w <= 0 || g.h <= 0 || g.w <= 0) return false;
  float scale = std::min(1.8f, (kNameW - 24.0f) / g.text_w);
  float x0 = (kNameW - g.text_w * scale) / 2, y0 = (kNameH - g.h * scale) / 2;
  auto alpha = [&g](float u, float v) {  // the letters' coverage at (u, v) of their picture
    int iu = (int)std::floor(u), iv = (int)std::floor(v);
    float fu = u - iu, fv = v - iv, sum = 0;
    for (int j = 0; j < 2; ++j)
      for (int i = 0; i < 2; ++i) {
        int x = iu + i, y = iv + j;
        if (x < 0 || y < 0 || x >= g.w || y >= g.h) continue;
        float a = (g.bgra[size_t(y) * g.w + x] >> 24) / 255.0f;
        sum += a * (i ? fu : 1 - fu) * (j ? fv : 1 - fv);
      }
    return sum;
  };
  out.w = kNameW, out.h = kNameH, out.text_w = kNameW;
  out.bgra.assign(size_t(kNameW) * kNameH, 0);
  Rgb black{0, 0, 0};
  for (int y = 0; y < kNameH; ++y)
    for (int x = 0; x < kNameW; ++x) {
      float u = (x + 0.5f - x0) / scale - 0.5f, v = (y + 0.5f - y0) / scale - 0.5f;
      float rim = 0;
      for (int dy = -3; dy <= 3; ++dy)
        for (int dx = -3; dx <= 3; ++dx)
          if (dx * dx + dy * dy <= 10) rim = std::max(rim, alpha(u + dx / scale, v + dy / scale));
      uint32_t& p = out.bgra[size_t(y) * kNameW + x];
      over(p, black, std::min(1.0f, rim * 1.2f) * 0.85f);
      over(p, tint, alpha(u, v));
    }
  return true;
}

IDirect3DTexture9* name_texture(const std::string& utf8, int character) {
  IDirect3DDevice9* dev = overlay::device();
  if (!dev || utf8.empty()) return nullptr;
  if (g_names_dev != dev) {
    for (NameTex& n : g_names)
      if (n.tex) n.tex->Release();
    g_names.clear();
    g_names_dev = dev;
  }
  for (const NameTex& n : g_names)
    if (n.character == character && n.text == utf8) return n.tex;
  game_text::Bitmap b;
  IDirect3DTexture9* t = nullptr;
  if (name_bitmap(utf8, character, b) &&
      SUCCEEDED(dev->CreateTexture(b.w, b.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) {
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(t->LockRect(0, &lr, nullptr, 0))) {
      for (int y = 0; y < b.h; ++y)
        std::memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, &b.bgra[size_t(y) * b.w], size_t(b.w) * 4);
      t->UnlockRect(0);
    } else {
      t->Release();
      t = nullptr;
    }
  }
  if (g_names.size() > 32) {  // (names come and go: keep a few)
    for (NameTex& n : g_names)
      if (n.tex) n.tex->Release();
    g_names.clear();
  }
  g_names.push_back(NameTex{utf8, character, t});
  return t;
}

}  // namespace

void install() {
  std::thread([] {
    std::vector<game_text::Bitmap> got;
    bool ok = game_text::pictures_in("chrsel_sl_pc.txd", {"csel_st_p2", "csel_selct", "csel_pl2"}, got) &&
              got.size() == 3 && got[0].w == kLabelW && got[0].h == kLabelH && got[1].w == kTagW && got[1].h == kTagH &&
              got[2].w == kPlate && got[2].h == kPlate;
    if (ok) {
      label(got[0], '3', {0, 170, 255}, g_bitmaps[0]);
      label(got[0], '4', {255, 140, 0}, g_bitmaps[1]);
      tag(got[1], '2', g_bitmaps[2]);
      tag(got[1], '3', g_bitmaps[3]);
      plate(got[2], '3', g_bitmaps[4]);
      plate(got[2], '4', g_bitmaps[5]);
    }
    g_ready = ok ? 1 : -1;
    if (ok)
      LOGI("char select: \"ИГРОК 3/4\" (banner, plate) and the tags \"ИГРОК 2/3\" made of the game's pictures");
    else
      LOGW("char select: the game's pictures for players 3 and 4 are not available; theirs show as player 2's");
  }).detach();
}

namespace {
void show(Swap& s, const uint8_t* rw_texture, int turn) {
  if (turn == s.shown) return;
  IDirect3DTexture9* t = d3d_texture_of(*reinterpret_cast<const uint8_t* const*>(rw_texture));
  if (!t) return;
  if (t != s.tex) {  // (a texture of its own: the old one is gone)
    s.tex = t;
    s.orig.clear();
    s.shown = 2;
    if (turn == 2) return;
  }
  bool ok;
  if (turn >= 3) {
    bool first = s.orig.empty();
    if (first) s.orig.resize(size_t(s.w) * s.h);
    ok = write_pixels(t, s.w, s.h, g_bitmaps[s.first + turn - 3].bgra.data(), first ? &s.orig : nullptr);
    if (!ok && first) s.orig.clear();
  } else {
    ok = !s.orig.empty() && write_pixels(t, s.w, s.h, s.orig.data(), nullptr);
    if (ok) s.orig.clear();
  }
  if (ok) s.shown = turn;
  if (ok) LOGI("char select: %s shows player %d", s.name, turn);
}
}  // namespace

void restore() {
  for (Swap& s : g_swaps)
    if (s.tex && !s.orig.empty() && write_pixels(s.tex, s.w, s.h, s.orig.data(), nullptr)) {
      s.orig.clear();
      s.shown = 2;
      LOGI("char select: %s is the game's again", s.name);
    }
}

void name_next(const std::string& utf8, int character) { g_name_now = name_texture(utf8, character); }

void name_done() { g_name_now = nullptr; }

void after_sprite(uint8_t* sprite, void(__cdecl* draw)(uint8_t*)) {
  if (g_name_now || !sprite || !draw) return;
  const uint8_t* tex = *reinterpret_cast<const uint8_t* const*>(sprite + 0x18);
  if (!tex) return;
  const char* name = reinterpret_cast<const char*>(tex + 0x10);
  if (std::strncmp(name, "csel_pl1", 32) != 0 && std::strncmp(name, "csel_pl2", 32) != 0) return;
  int p = csel::picker();
  std::string n = p >= 1 ? nametags::name(p - 1) : std::string();
  uint8_t m[0x3C];
  if (n.empty() || !csel::tag_model(m)) return;
  IDirect3DTexture9* t = name_texture(n, csel::pointed());
  if (!t) return;
  // Under the plate's word (ОСНОВНОЕ / ПОМОЩЬ), in a tag's box.
  float plate_h = *reinterpret_cast<const float*>(sprite + 0x10);
  *reinterpret_cast<float*>(m + 0) = *reinterpret_cast<const float*>(sprite + 0);
  *reinterpret_cast<float*>(m + 4) = *reinterpret_cast<const float*>(sprite + 4) - plate_h * 0.30f;
  m[0x14] = m[0x15] = m[0x16] = 0xFF;
  m[0x17] = sprite[0x17];  // (the plate's fade)
  widescreen::set_texture_override(t);
  draw(m);
  widescreen::set_texture_override(nullptr);
}

IDirect3DTexture9* instead_of(const uint8_t* rw_texture) {
  if (g_name_now) return g_name_now;
  if (!rw_texture || g_ready.load() != 1) return nullptr;
  const char* name = reinterpret_cast<const char*>(rw_texture + 0x10);
  for (Swap& s : g_swaps)
    if (std::strncmp(name, s.name, 32) == 0) {
      show(s, rw_texture, csel::turn());
      return nullptr;
    }
  if (std::strncmp(name, "csel_selct", 32) == 0) {
    int p = csel::tag_player();
    return p >= 2 && p <= 3 ? texture(2 + p - 2) : nullptr;
  }
  return nullptr;
}

}  // namespace csel_art
