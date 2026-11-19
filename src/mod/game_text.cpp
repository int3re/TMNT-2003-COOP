#include "mod/game_text.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include "common/config.h"
#include "common/log.h"

namespace game_text {
namespace {

// Labels with known text, all in the option pages' size (cap height 24 of
// 64): texture name -> its words. Cyan ones are recoloured to white.
struct Source {
  const char* texture;
  const char* text;
  bool cyan;
  float scale = 1.0f;  // its letters made this much smaller first
};
const Source kSources[] = {
    {"op_other_moji_autosave", "АВТОЗАПИСЬ", false},
    {"op_soundset_moji_stereo", "СТЕРЕО", false},
    {"op_selcon_1", "ИГРОК 1", false},
    {"op_selcon_2", "ИГРОК 2", false},
    {"disset_moji_resolution", "РАЗРЕШЕНИЕ", false},
    {"op_other_moji_effect", "СИЛА УДАРА", false},
    {"op_soundset_moji_bgm", "МУЗЫКА", false},
    {"op_cont_set_pad", "ГЕЙМПАД", false},
    {"op_cont_set_key", "КЛАВИАТУРА", false},
    {"disset_moji_toon", "ТЕНИ", false},
    {"op_moji_contset", "ПАРАМЕТРЫ УПРАВЛЕНИЯ", false},
    {"op_moji_selcon", "ВЫБОР УПРАВЛЕНИЯ", false},
    {"op_moji_otherset", "ДРУГИЕ ПАРАМЕТРЫ", false},
    {"op_moji_dispset", "ПАРАМЕТРЫ ГРАФИКИ", false},
    {"op_moji_soundset", "ОПЦИИ ЗВУКА", false},
    {"op_moji_pass", "ПАРОЛЬ", false},
    {"op_moji_exit", "УЙТИ", false},
    {"op_soundset_moji_audio", "РЕЖИМ АУДИО", false},
    {"op_soundset_moji_mono", "МОНО", false},
    {"op_soundset_moji_se", "ЗВУК", false},
    {"op_other_moji_normal", "НОРМА", false},
    {"op_other_moji_easy", "ЛЕГКО", false},
    {"disset_moji_antialiasing", "СГЛАЖИВАНИЕ", false},
    {"op_other_off", "ВЫК", false},
    {"op_other_on", "ВКЛ", false},
    {"op_soundset_moji_default", "ПО УМОЛЧАНИЮ", true},
    {"op_moji_defaut", "ОПЦИИ ПО-УМОЛЧАНИЮ", true},
};
// The main menu's items (menu_pc.txd), cap height 21; РЕЖИМ СОСТЯЗАНИЯ is
// drawn bigger (29): made smaller to 21 for its Я.
const Source kMenuSources[] = {
    {"menu_moj_story", "РЕЖИМ СЮЖЕТА", false},
    {"menu_moj_data", "БАЗА ДАННЫХ", false},
    {"menu_moj_load", "ЗАГРУЗКА", false},
    {"menu_moj_option", "ПАРАМЕТРЫ", false},
    {"menu_moj_quit", "ВЫХОД", false},
    {"menu_moj_challenge", "РЕЖИМ СОСТЯЗАНИЯ", false, 21.0f / 29.0f},
};
// The story submenu's items (menu_pc.txd), cap height 39.
const Source kStorySources[] = {
    {"menu_moj_story_2p", "2 ИГРОКА", false},
    {"menu_moj_story_1p", "1 ИГРОК", false},
    {"menu_moj_story_splinter", "СПЛИНТЕР", false},
    {"menu_moj_story_casey", "КЕЙСИ", false},
};
constexpr int kCanvasH = 64;

struct Image {
  int w = 0, h = 0;
  std::vector<uint32_t> px;  // 0xAARRGGBB
  uint32_t at(int x, int y) const { return px[size_t(y) * w + x]; }
};

// A label's pixels by letter (see own_pixels): which letter each belongs to
// (-1 none) and its colour for that letter.
struct Owners {
  std::vector<int16_t> letter;
  std::vector<uint32_t> px;
};

struct Glyph {
  const Image* img = nullptr;
  float shear = 0;    // italic slant of the source picture (x per row up)
  int base = 0;       // the source's bottom row (shear origin)
  int x0 = 0, x1 = 0;  // band to copy, in unslanted columns
  int body0 = 0, body1 = 0;  // the white letter itself
  int baseline = 0;    // bottom of the source's white letters
  const Owners* owners = nullptr;  // its pixels in the label (null: the band)
  int letter = 0;                  // its number there
};

// One lettering: its letters, and its spacing as measured in its labels.
struct Font {
  std::map<uint32_t, Glyph> glyphs;  // Unicode code point -> glyph
  std::vector<int> gaps, spaces;
  int gap = 3, space = 12;
  int baseline;  // where the letters stand in the 64-texel picture
};

std::mutex g_lock;
std::atomic<bool> g_ready{false};
std::vector<Image> g_images;  // stable: reserved before filling
std::vector<Owners> g_owners;  // one a label, stable the same way
// Style::Options, MainMenu, Story (whose labels have spaces after digits only:
// its word space is set, 0.4 of its cap height).
Font g_fonts[3] = {{{}, {}, {}, 3, 12, 43}, {{}, {}, {}, 1, 8, 40}, {{}, {}, {}, 2, 16, 47}};

bool white(uint32_t c) {
  uint32_t a = c >> 24, r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
  return a > 180 && r > 170 && g > 170 && b > 170;
}

std::vector<uint32_t> codepoints(const std::string& s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    uint32_t cp = c;
    int n = 0;
    if (c >= 0xF0) cp = c & 7, n = 3;
    else if (c >= 0xE0) cp = c & 15, n = 2;
    else if (c >= 0xC0) cp = c & 31, n = 1;
    for (int k = 1; k <= n && i + k < s.size(); ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 63);
    i += n + 1;
    if (cp == 0x401) cp = 0x415;  // Ё -> Е, as the game writes it
    out.push_back(cp);
  }
  return out;
}

// --- TMNT.DAT (CRI AFS) and platform-independent texture dictionaries ----------

bool read_entry(HANDLE f, const char* name, std::vector<uint8_t>& out) {
  DWORD n = 0;
  uint8_t head[8];
  SetFilePointer(f, 0, nullptr, FILE_BEGIN);
  if (!ReadFile(f, head, 8, &n, nullptr) || n != 8 || memcmp(head, "AFS\0", 4) != 0) return false;
  uint32_t count;
  memcpy(&count, head + 4, 4);
  if (count == 0 || count > 100000) return false;
  std::vector<uint32_t> toc(size_t(count) * 2 + 2);
  if (!ReadFile(f, toc.data(), DWORD(toc.size() * 4), &n, nullptr) || n != toc.size() * 4) return false;
  uint32_t dir = toc[count * 2], dir_size = toc[count * 2 + 1];
  if (!dir || dir_size < count * 0x30) return false;
  std::vector<char> names(dir_size);
  SetFilePointer(f, (LONG)dir, nullptr, FILE_BEGIN);
  if (!ReadFile(f, names.data(), dir_size, &n, nullptr) || n != dir_size) return false;
  for (uint32_t i = 0; i < count; ++i) {
    if (_strnicmp(&names[size_t(i) * 0x30], name, 32) != 0) continue;
    uint32_t off = toc[i * 2], size = toc[i * 2 + 1];
    out.resize(size);
    SetFilePointer(f, (LONG)off, nullptr, FILE_BEGIN);
    return ReadFile(f, out.data(), size, &n, nullptr) && n == size;
  }
  return false;
}

// Decodes the textures named in `want` (8-, 4- or 32-bit images).
void decode_txd(const std::vector<uint8_t>& d, const std::map<std::string, size_t>& want,
                std::vector<Image>& images, std::vector<int>& index) {
  auto u16 = [&](size_t o) { return o + 2 <= d.size() ? uint32_t(d[o] | d[o + 1] << 8) : 0u; };
  auto u32 = [&](size_t o) {
    uint32_t v = 0;
    if (o + 4 <= d.size()) memcpy(&v, &d[o], 4);
    return v;
  };
  if (u32(0) != 0x23) return;
  uint32_t count = u16(12);
  size_t o = 16;
  for (uint32_t k = 0; k < count && o + 72 <= d.size(); ++k) {
    char name[33] = {};
    memcpy(name, &d[o], 32);
    uint32_t levels = u32(o + 64);
    o += 72;
    for (uint32_t lv = 0; lv < levels && o + 40 <= d.size(); ++lv) {
      uint32_t chunk = u32(o + 4), ss = u32(o + 16);
      int w = (int)u32(o + 24), h = (int)u32(o + 28), depth = (int)u32(o + 32), stride = (int)u32(o + 36);
      size_t p = o + 24 + ss;
      auto it = want.find(name);
      if (lv == 0 && it != want.end() && w > 0 && h > 0 && w <= 2048 && h <= 2048 &&
          p + size_t(h) * stride <= d.size()) {
        Image img;
        img.w = w, img.h = h;
        img.px.resize(size_t(w) * h);
        const uint8_t* pal = depth <= 8 ? &d[p + size_t(h) * stride] : nullptr;
        bool ok = !pal || p + size_t(h) * stride + (size_t(4) << depth) <= d.size();
        for (int y = 0; ok && y < h; ++y)
          for (int x = 0; x < w; ++x) {
            const uint8_t* row = &d[p + size_t(y) * stride];
            const uint8_t* c;
            if (depth == 32) c = row + x * 4;
            else if (depth == 8) c = pal + row[x] * 4;
            else if (depth == 4) c = pal + ((x & 1) ? (row[x / 2] & 15) : (row[x / 2] >> 4)) * 4;
            else { ok = false; break; }
            img.px[size_t(y) * w + x] = uint32_t(c[3]) << 24 | uint32_t(c[0]) << 16 | uint32_t(c[1]) << 8 | c[2];
          }
        if (ok) {
          index[it->second] = (int)images.size();
          images.push_back(std::move(img));
        }
      }
      o += 12 + chunk;
    }
  }
}

// --- cutting the letters ---------------------------------------------------------

struct Run {
  int a, b;  // unslanted columns [a, b) of one white letter
};

int bottom_row(const Image& img) {
  for (int y = img.h - 1; y >= 0; --y)
    for (int x = 0; x < img.w; ++x)
      if (img.at(x, y) >> 24) return y;
  return img.h - 1;
}

int column(const Image& img, float s, int base, int xp) {
  int n = 0;
  for (int y = 0; y < img.h; ++y) {
    int x = (int)std::lround(xp + s * (base - y));
    if (x >= 0 && x < img.w && white(img.at(x, y))) ++n;
  }
  return n;
}

std::vector<Run> runs(const Image& img, float s, int base) {
  std::vector<Run> out;
  int start = -1;
  for (int xp = -img.h; xp < img.w + img.h; ++xp) {
    int n = column(img, s, base, xp);
    if (n && start < 0) start = xp;
    if (!n && start >= 0) {
      if (xp - start >= 2) out.push_back({start, xp});
      start = -1;
    }
  }
  return out;
}

// Which letter of a label each pixel belongs to. The letters are white with a
// drop shadow 3..4 pixels down and to the right and no outline, set 2 apart:
// a band between two white letters also takes the neighbours' shadows and
// edges, black slivers beside a letter once it stands among other letters.
// White: the letter of its column. Dark: the letter casting it (white up and
// left of it within the shadow's reach), else none. A light edge: the nearest
// white within 2; on that letter's left or top (not in its own shadow) it was
// an edge against a neighbour's shadow, now white fading out.
constexpr int kShadowX = 6, kShadowY = 7, kDark = 110;

int run_of(const std::vector<Run>& r, float xp) {
  int best = 0;
  float bd = 1e9f;
  for (int i = 0; i < (int)r.size(); ++i) {
    if (xp >= r[i].a - 0.5f && xp < r[i].b + 0.5f) return i;
    float d = std::min(std::fabs(xp - r[i].a), std::fabs(xp - r[i].b));
    if (d < bd) bd = d, best = i;
  }
  return best;
}

void own_pixels(const Image& img, float s, int base, const std::vector<Run>& r, Owners& out) {
  const int w = img.w, h = img.h;
  std::vector<int16_t> lw(size_t(w) * h, -1);  // white pixels' letters
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      if (white(img.at(x, y))) lw[size_t(y) * w + x] = (int16_t)run_of(r, x - s * (base - y));
  auto letter_at = [&](int x, int y) { return x >= 0 && x < w && y >= 0 && y < h ? lw[size_t(y) * w + x] : -1; };
  out.letter = lw;
  out.px = img.px;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      size_t i = size_t(y) * w + x;
      uint32_t c = img.px[i], a = c >> 24;
      if (!a || lw[i] >= 0) continue;
      int lum = (int(c >> 16 & 255) * 3 + int(c >> 8 & 255) * 6 + int(c & 255)) / 10;
      int best = -1, bd = 1 << 30;
      if (lum < kDark) {  // a shadow: whose?
        for (int dy = 0; dy <= kShadowY; ++dy)
          for (int dx = 0; dx <= kShadowX; ++dx) {
            int k = letter_at(x - dx, y - dy), d = std::max(dx, dy) * 100 + dx + dy;
            if (k >= 0 && d < bd) bd = d, best = k;
          }
        out.letter[i] = (int16_t)best;
        continue;
      }
      for (int dy = -2; dy <= 2; ++dy)  // an edge: of the nearest white
        for (int dx = -2; dx <= 2; ++dx) {
          int k = letter_at(x + dx, y + dy), d = std::max(std::abs(dx), std::abs(dy)) * 100 + std::abs(dx) + std::abs(dy);
          if (k >= 0 && d < bd) bd = d, best = k;
        }
      out.letter[i] = (int16_t)best;
      if (best < 0) continue;
      bool own_shadow = false;
      for (int dy = 0; dy <= kShadowY && !own_shadow; ++dy)
        for (int dx = 0; dx <= kShadowX && !own_shadow; ++dx)
          own_shadow = (dx || dy) && letter_at(x - dx, y - dy) == best;
      if (!own_shadow) out.px[i] = std::min<uint32_t>(a, (uint32_t)lum) << 24 | 0xFFFFFF;
    }
}

// Smaller by `s` (< 1): each texel the average of what it covers, colour
// weighted by alpha.
Image shrink(const Image& src, float s) {
  Image out;
  out.w = std::max(1, (int)std::lround(src.w * s));
  out.h = std::max(1, (int)std::lround(src.h * s));
  out.px.assign(size_t(out.w) * out.h, 0);
  const float fx = float(src.w) / out.w, fy = float(src.h) / out.h;
  for (int y = 0; y < out.h; ++y)
    for (int x = 0; x < out.w; ++x) {
      const float x0 = x * fx, x1 = (x + 1) * fx, y0 = y * fy, y1 = (y + 1) * fy;
      double a = 0, r = 0, g = 0, b = 0, area = 0;
      for (int sy = (int)y0; sy < std::min(src.h, (int)std::ceil(y1)); ++sy) {
        double wy = std::min(y1, sy + 1.0f) - std::max(y0, (float)sy);
        for (int sx = (int)x0; sx < std::min(src.w, (int)std::ceil(x1)); ++sx) {
          double wgt = wy * (std::min(x1, sx + 1.0f) - std::max(x0, (float)sx));
          uint32_t c = src.at(sx, sy);
          double ca = (c >> 24) * wgt;
          a += ca, area += wgt;
          r += (c >> 16 & 255) * ca, g += (c >> 8 & 255) * ca, b += (c & 255) * ca;
        }
      }
      auto u8 = [](double v) { return (uint32_t)std::clamp((int)std::lround(v), 0, 255); };
      out.px[size_t(y) * out.w + x] = a > 0 ? u8(a / area) << 24 | u8(r / a) << 16 | u8(g / a) << 8 | u8(b / a) : 0;
    }
  return out;
}

// --- letters a lettering lacks, made of its own ---------------------------------
// The story submenu's lettering has no Ч or Ы. Ч is its Н without the left stem
// under the bar (as the smaller letterings draw it), Ы its Р upside down (a Ь)
// with the Р's stem beside it. The main menu's has no Ь: its Ы without the right
// stem. Made upright (the slant taken out), then slanted again; the shadow is
// drawn as both fonts have it: the white moved 2 right, 4 down.
constexpr int kMadeShadowX = 2, kMadeShadowY = 4;

// A glyph's white as coverage 0..1 on an upright grid (columns from u0).
struct Upright {
  int u0 = 0, w = 0, h = 0;
  std::vector<float> c;
  float& at(int u, int y) { return c[size_t(y) * w + u]; }
  float at(int u, int y) const { return c[size_t(y) * w + u]; }
  // Columns white (> 0.7) in more than `rows` rows: [first, last] of the first
  // run from `from` (-1 none).
  bool tall_run(int rows, int from, int& first, int& last) const {
    first = last = -1;
    for (int u = from; u < w; ++u) {
      int n = 0;
      for (int y = 0; y < h; ++y) n += at(u, y) > 0.7f;
      if (n > rows) {
        if (first < 0) first = u;
        last = u;
      } else if (first >= 0) {
        break;
      }
    }
    return first >= 0;
  }
};

float coverage(uint32_t c) {
  float a = (c >> 24) / 255.0f;
  float lum = ((c >> 16 & 255) * 3 + (c >> 8 & 255) * 6 + (c & 255)) / 10.0f;
  return a * std::clamp((lum - 40.0f) / 215.0f, 0.0f, 1.0f);
}

Upright upright(const Glyph& g) {
  const Image& img = *g.img;
  Upright m;
  m.u0 = g.body0 - 4;
  m.w = g.body1 + 4 - m.u0;
  m.h = img.h;
  m.c.assign(size_t(m.w) * m.h, 0.0f);
  for (int y = 0; y < m.h; ++y)
    for (int u = 0; u < m.w; ++u) {
      float x = m.u0 + u + g.shear * (g.base - y), v = 0;
      int xi = (int)std::floor(x);
      for (int k = 0; k < 2; ++k) {
        int xx = xi + k;
        float wgt = k ? x - xi : 1 - (x - xi);
        size_t i = size_t(y) * img.w + xx;
        if (xx >= 0 && xx < img.w && g.owners->letter[i] == g.letter) v += wgt * coverage(g.owners->px[i]);
      }
      m.at(u, y) = std::min(v, 1.0f);
    }
  return m;
}

// The white rows of an upright glyph: top and bottom.
void white_rows(const Upright& m, int& top, int& bottom) {
  top = m.h, bottom = -1;
  for (int y = 0; y < m.h; ++y)
    for (int u = 0; u < m.w; ++u)
      if (m.at(u, y) > 0.7f) top = std::min(top, y), bottom = std::max(bottom, y);
}

// An upright white made a glyph: slanted like `like`, white over its shadow;
// its own picture (kept in g_images / g_owners) with all its pixels its own.
bool made_glyph(const Upright& m, const Glyph& like, Glyph& out) {
  constexpr int kLeft = 8;
  Image img;
  img.w = m.w + (int)std::ceil(std::fabs(like.shear) * m.h) + 2 * kLeft;
  img.h = m.h;
  std::vector<float> wm(size_t(img.w) * img.h, 0.0f);
  for (int y = 0; y < m.h; ++y)
    for (int u = 0; u < m.w; ++u) {
      float v = m.at(u, y);
      if (v <= 0) continue;
      float x = u + like.shear * (like.base - y) + kLeft;
      int xi = (int)std::floor(x);
      for (int k = 0; k < 2; ++k) {
        int xx = xi + k;
        float wgt = k ? x - xi : 1 - (x - xi);
        if (xx >= 0 && xx < img.w) wm[size_t(y) * img.w + xx] = std::min(1.0f, wm[size_t(y) * img.w + xx] + v * wgt);
      }
    }
  img.px.assign(size_t(img.w) * img.h, 0);
  for (int y = 0; y < img.h; ++y)
    for (int x = 0; x < img.w; ++x) {
      float w = wm[size_t(y) * img.w + x];
      int sx = x - kMadeShadowX, sy = y - kMadeShadowY;
      float s = sx >= 0 && sy >= 0 ? wm[size_t(sy) * img.w + sx] : 0.0f;
      float a = w + s * (1 - w);
      if (a < 0.004f) continue;
      uint32_t c = (uint32_t)std::lround(255 * w / a), al = (uint32_t)std::lround(255 * a);
      img.px[size_t(y) * img.w + x] = al << 24 | c << 16 | c << 8 | c;
    }
  int first = -1, last = -1;  // the body: white columns at the base row's slant
  for (int u = 0; u < m.w; ++u)
    for (int y = 0; y < m.h; ++y)
      if (m.at(u, y) > 0.7f) {
        if (first < 0) first = u;
        last = u;
        break;
      }
  if (first < 0 || g_images.size() == g_images.capacity() || g_owners.size() == g_owners.capacity()) return false;
  g_images.push_back(std::move(img));
  const Image& kept = g_images.back();
  g_owners.emplace_back();
  Owners& o = g_owners.back();
  o.px = kept.px;
  o.letter.assign(kept.px.size(), -1);
  for (size_t i = 0; i < kept.px.size(); ++i)
    if (kept.px[i] >> 24) o.letter[i] = 0;
  out = like;
  out.img = &kept;
  out.owners = &o;
  out.letter = 0;
  out.body0 = first + kLeft;
  out.body1 = last + 1 + kLeft;
  out.x0 = out.body0 - 4, out.x1 = out.body1 + 4;
  return true;
}

// Ч of Н: the left stem cut under the bar, the bar's corner there rounded.
bool make_che(const Glyph& n, Glyph& out) {
  Upright m = upright(n);
  int top, bottom, l0, l1, r0 = -1, r1 = -1, a, b;
  white_rows(m, top, bottom);
  int rows = (bottom - top + 1) * 2 / 3;
  if (bottom < 0 || !m.tall_run(rows, 0, l0, l1)) return false;
  for (int from = l1 + 1; m.tall_run(rows, from, a, b); from = b + 1) r0 = a, r1 = b;
  if (r0 < 0) return false;
  int mid = (l1 + 1 + r0) / 2, bar_bottom = -1;
  for (int y = top; y <= bottom; ++y)
    if (m.at(mid, y) > 0.5f) bar_bottom = y;
  if (bar_bottom < top || bar_bottom >= bottom) return false;
  for (int y = bar_bottom + 1; y < m.h; ++y)
    for (int u = 0; u <= std::min(l1 + 1, m.w - 1); ++u) m.at(u, y) = 0;
  constexpr int kR = 3;  // the bar's outer corner
  for (int y = bar_bottom - kR + 1; y <= bar_bottom; ++y)
    for (int u = l0; u < l0 + kR && u < m.w; ++u) {
      float dy = y - (bar_bottom - kR + 0.5f), du = (l0 + kR - 0.5f) - u;
      if (dy > 0 && du > 0 && dy * dy + du * du > kR * kR) m.at(u, y) = 0;
    }
  return made_glyph(m, n, out);
}

// Ь of Ы: the stem on its right taken away.
bool make_soft_sign(const Glyph& yery, Glyph& out) {
  Upright m = upright(yery);
  int top, bottom, a, b, last = -1;
  white_rows(m, top, bottom);
  int rows = (bottom - top + 1) * 2 / 3;
  if (bottom < 0 || !m.tall_run(rows, 0, a, b)) return false;
  for (int from = b + 1; m.tall_run(rows, from, a, b); from = b + 1) last = a;
  if (last < 0) return false;  // (one stem only: not an Ы)
  for (int y = 0; y < m.h; ++y)
    for (int u = std::max(0, last - 1); u < m.w; ++u) m.at(u, y) = 0;
  return made_glyph(m, yery, out);
}

// Ы of Р: upside down within its height (a Ь), its stem again on the right.
bool make_yery(const Glyph& r, Glyph& out) {
  Upright m = upright(r);
  int top, bottom, s0, s1;
  white_rows(m, top, bottom);
  if (bottom < 0 || !m.tall_run((bottom - top + 1) * 2 / 3, 0, s0, s1)) return false;
  int right = -1;  // the bowl's right edge
  for (int u = 0; u < m.w; ++u)
    for (int y = 0; y < m.h; ++y)
      if (m.at(u, y) > 0.5f) right = u;
  constexpr int kGap = 3;
  const int width = s1 - s0 + 1, at = right + kGap + 1;
  Upright y;
  y.u0 = m.u0;
  y.h = m.h;
  y.w = at + width + 3;
  y.c.assign(size_t(y.w) * y.h, 0.0f);
  for (int row = top; row <= bottom; ++row)
    for (int u = 0; u < m.w; ++u) y.at(u, top + bottom - row) = m.at(u, row);
  for (int row = 0; row < m.h; ++row)
    for (int k = -1; k <= width; ++k)
      if (s0 + k >= 0 && s0 + k < m.w && at + k >= 0 && at + k < y.w)
        y.at(at + k, row) = std::max(y.at(at + k, row), m.at(s0 + k, row));
  return made_glyph(y, r, out);
}

void cut(const Image& img, const Source& src, Font& font) {
  std::vector<uint32_t> text = codepoints(src.text);
  int letters = 0;
  for (uint32_t c : text) letters += c != ' ';
  int base = bottom_row(img);
  float best_s = 0;
  std::vector<Run> best;
  int best_score = 1 << 30;
  for (int k = 0; k < 40; ++k) {
    float s = k / 100.0f;
    std::vector<Run> r = runs(img, s, base);
    int score = std::abs((int)r.size() - letters);
    if (score < best_score || (score == best_score && score == 0 && s > 0.1f && best_s < 0.1f)) {
      best_score = score, best_s = s, best = std::move(r);
    }
  }
  // Two round letters may touch ("ОЛ" in УМОЛЧАНИЮ): one short, split the
  // widest run where it is thinnest.
  if ((int)best.size() == letters - 1 && !best.empty()) {
    size_t wi = 0;
    for (size_t i = 1; i < best.size(); ++i)
      if (best[i].b - best[i].a > best[wi].b - best[wi].a) wi = i;
    Run r = best[wi];
    int lo = r.a + (r.b - r.a) / 4, hi = r.b - (r.b - r.a) / 4, cut_at = lo, least = 1 << 30;
    for (int xp = lo; xp < hi; ++xp) {
      int n = column(img, best_s, base, xp);
      if (n < least) least = n, cut_at = xp;
    }
    best[wi] = Run{r.a, cut_at};
    best.insert(best.begin() + wi + 1, Run{cut_at + 1, r.b});
    best_score = 0;
  }
  if (best_score != 0) {
    LOGW("game text: %s: %d letters found, %d expected", src.texture, (int)best.size(), letters);
    return;
  }
  // The baseline of the word: the most common bottom of its white letters.
  std::vector<int> bottoms;
  for (const Run& r : best) {
    int bottom = 0;
    for (int y = 0; y < img.h; ++y)
      for (int xp = r.a; xp < r.b; ++xp) {
        int x = (int)std::lround(xp + best_s * (base - y));
        if (x >= 0 && x < img.w && white(img.at(x, y))) bottom = std::max(bottom, y);
      }
    bottoms.push_back(bottom);
  }
  std::vector<int> sorted = bottoms;
  std::sort(sorted.begin(), sorted.end());
  int baseline = sorted[sorted.size() / 2];
  g_owners.emplace_back();
  Owners& owners = g_owners.back();
  own_pixels(img, best_s, base, best, owners);
  size_t li = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == ' ') continue;
    const Run& r = best[li];
    if (li + 1 < best.size()) {
      if (i + 1 < text.size() && text[i + 1] == ' ') {
        if (text[i] < '0' || text[i] > '9') font.spaces.push_back(best[li + 1].a - r.b);  // (a digit's is wider)
      } else {
        font.gaps.push_back(best[li + 1].a - r.b);
      }
    }
    int lo = li == 0 ? r.a - 3 : (best[li - 1].b + r.a) / 2;
    int hi = li + 1 == best.size() ? r.b + 5 : (r.b + best[li + 1].a) / 2;
    Glyph g{&img, best_s, base, lo, hi, r.a, r.b, baseline, &owners, (int)li};
    font.glyphs.emplace(text[i], g);  // the first source of a letter wins
    ++li;
  }
}

void recolour_cyan(Image& img) {
  for (uint32_t& c : img.px) {
    uint32_t a = c & 0xFF000000u, r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    uint32_t m = std::max(r, std::max(g, b));
    c = a | m << 16 | m << 8 | m;
  }
}

// Raw dictionaries, kept for picture() (option_pc.txd ~6.5 MB, menu_pc.txd ~1.3 MB,
// gameinfopc.txd ~1.7 MB: the play screen's panels).
std::vector<uint8_t> g_option_txd, g_menu_txd, g_gameinfo_txd;

bool read_dictionaries() {
  if (!g_option_txd.empty()) return true;
  std::wstring path = module_dir(nullptr) + L"TMNT.DAT";
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    LOGW("game text: cannot open %ls", path.c_str());
    return false;
  }
  bool ok = read_entry(f, "option_pc.txd", g_option_txd);
  read_entry(f, "menu_pc.txd", g_menu_txd);
  read_entry(f, "gameinfopc.txd", g_gameinfo_txd);
  CloseHandle(f);
  if (!ok) {
    g_option_txd.clear();
    LOGW("game text: option_pc.txd not found in TMNT.DAT");
  }
  return ok;
}

// --- the plain font ---------------------------------------------------------------

struct Cell {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
Cell g_cells[128];
Cell g_russian[32];  // А..Я (none: the picture is not laid out as expected)
Image g_font;  // white glyphs, alpha from the mask picture
bool g_font_ok = false;

bool decode_bmp(const std::vector<uint8_t>& d, Image& img, bool as_alpha) {
  if (d.size() < 54 || d[0] != 'B' || d[1] != 'M') return false;
  uint32_t off, hsize;
  int32_t w, h;
  uint16_t bpp;
  memcpy(&off, &d[10], 4), memcpy(&hsize, &d[14], 4), memcpy(&w, &d[18], 4), memcpy(&h, &d[22], 4), memcpy(&bpp, &d[28], 2);
  bool flip = h > 0;
  h = std::abs(h);
  if (w <= 0 || w > 4096 || h > 4096 || (bpp != 8 && bpp != 4)) return false;
  const uint8_t* pal = &d[14 + hsize];
  size_t stride = ((size_t(w) * bpp + 31) / 32) * 4;
  if (off + stride * h > d.size()) return false;
  img.w = w, img.h = h;
  img.px.assign(size_t(w) * h, 0);
  for (int y = 0; y < h; ++y) {
    const uint8_t* row = &d[off + stride * (flip ? h - 1 - y : y)];
    for (int x = 0; x < w; ++x) {
      int i = bpp == 8 ? row[x] : (x & 1 ? row[x / 2] & 15 : row[x / 2] >> 4);
      const uint8_t* c = pal + i * 4;  // B G R x
      uint32_t lum = std::max(c[0], std::max(c[1], c[2]));
      img.px[size_t(y) * w + x] = as_alpha ? lum << 24 : 0xFF000000u | uint32_t(c[2]) << 16 | uint32_t(c[1]) << 8 | c[0];
    }
  }
  return true;
}

bool load_font() {
  if (g_font_ok) return true;
  std::wstring path = module_dir(nullptr) + L"TMNT.DAT";
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  std::vector<uint8_t> met, colour, mask;
  bool ok = read_entry(f, "font.met", met) && read_entry(f, "font.bmp", colour) && read_entry(f, "fontm.bmp", mask);
  CloseHandle(f);
  Image c, m;
  if (!ok || !decode_bmp(colour, c, false) || !decode_bmp(mask, m, true) || c.w != m.w || c.h != m.h) return false;
  g_font = c;
  for (size_t i = 0; i < g_font.px.size(); ++i) g_font.px[i] = (m.px[i] & 0xFF000000u) | (c.px[i] & 0xFFFFFF);
  // Metrics as the game has them (digits and signs where ASCII puts them)...
  std::string text(met.begin(), met.end());
  Cell metrics[128];
  for (size_t p = 0; (p = text.find('\n', p)) != std::string::npos;) {
    ++p;
    int code, x0, y0, x1, y1;
    if (sscanf_s(text.c_str() + p, "%d %d %d %d %d", &code, &x0, &y0, &x1, &y1) == 5 && code >= 32 && code < 128)
      g_cells[code] = metrics[code] = Cell{x0, y0, x1, y1};
  }
  // ...and the Latin capitals from the picture itself: rows 1 and 2 of its
  // 24-texel grid end with A..I and hold J..Z, one glyph after another.
  auto row = [&](int r) {
    std::vector<std::pair<int, int>> segs;
    int y0 = r * 24, y1 = std::min(y0 + 23, g_font.h);
    int start = -1;
    for (int x = 0; x <= g_font.w; ++x) {
      bool ink = false;
      for (int y = y0; x < g_font.w && y < y1 && !ink; ++y) ink = (g_font.at(x, y) >> 24) > 0;
      if (ink && start < 0) start = x;
      if (!ink && start >= 0) segs.push_back({start, x}), start = -1;
    }
    return segs;
  };
  auto r1 = row(1), r2 = row(2);
  if (r1.size() == 20 && r2.size() == 17) {
    for (int i = 0; i < 9; ++i) g_cells['A' + i] = Cell{r1[11 + i].first, 24, r1[11 + i].second, 47};
    for (int i = 0; i < 17; ++i) g_cells['J' + i] = Cell{r2[i].first, 48, r2[i].second, 71};
    // The Russian metrics: codes 65..96 are А..Я, 97..122 the Latin capitals
    // and 123..127 the signs ASCII has at 91..95 ([ \ ] ^ _).
    for (int i = 0; i < 32; ++i) g_russian[i] = metrics[65 + i];
    for (int i = 0; i < 5; ++i) g_cells['[' + i] = metrics[123 + i];
    g_cells['`'] = metrics['\''];
  } else {
    LOGW("game text: the plain font's Latin letters were not found (%zu/%zu)", r1.size(), r2.size());
  }
  g_font_ok = true;
  return true;
}

}  // namespace

// A letter's place in the plain font's picture (null: a space).
const Cell* plain_cell(uint32_t cp) {
  const Cell* cell = nullptr;
  if (cp == ' ') return nullptr;
  if (cp < 128) cell = &g_cells[std::toupper((int)cp)];
  else if (cp >= 0x410 && cp <= 0x42F) cell = &g_russian[cp - 0x410];  // А..Я
  else if (cp >= 0x430 && cp <= 0x44F) cell = &g_russian[cp - 0x430];  // а..я
  else if (cp == 0x451) cell = &g_russian[5];                          // ё (Ё is Е already)
  if (!cell || cell->x1 <= cell->x0) cell = &g_cells['?'];
  return cell;
}

bool plain(const std::string& utf8, Bitmap& out) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (!load_font()) return false;
  constexpr int kGap = 2;
  std::vector<uint32_t> text = codepoints(utf8);
  int w = 2;
  for (uint32_t cp : text) {
    const Cell* cell = plain_cell(cp);
    w += (cell ? cell->x1 - cell->x0 : 7) + kGap;
  }
  out.w = 1;
  while (out.w < w + 2) out.w *= 2;
  out.h = 32, out.text_w = w;
  out.bgra.assign(size_t(out.w) * out.h, 0);
  int x = 2;
  for (uint32_t cp : text) {
    const Cell* cell = plain_cell(cp);
    if (cell)
      for (int y = cell->y0; y < cell->y1 && y < g_font.h; ++y)
        for (int sx = cell->x0; sx < cell->x1 && sx < g_font.w; ++sx) {
          int ty = 4 + (y - cell->y0), tx = x + (sx - cell->x0);
          if (ty < out.h && tx < out.w) out.bgra[size_t(ty) * out.w + tx] = g_font.at(sx, y);
        }
    x += (cell ? cell->x1 - cell->x0 : 7) + kGap;
  }
  return true;
}

bool picture(const char* name, Bitmap& out) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (!read_dictionaries()) return false;
  std::map<std::string, size_t> want{{name, 0}};
  std::vector<Image> images;
  std::vector<int> index(1, -1);
  decode_txd(g_option_txd, want, images, index);
  if (index[0] < 0) decode_txd(g_menu_txd, want, images, index);
  if (index[0] < 0) decode_txd(g_gameinfo_txd, want, images, index);
  if (index[0] < 0) return false;
  const Image& img = images[index[0]];
  out.w = img.w, out.h = img.h, out.text_w = img.w;
  out.bgra = img.px;
  return true;
}

int white_rows(const char* name) {
  Bitmap b;
  if (!picture(name, b)) return 0;
  int top = -1, bottom = -1;
  for (int y = 0; y < b.h; ++y)
    for (int x = 0; x < b.w; ++x)
      if (white(b.bgra[size_t(y) * b.w + x])) {
        if (top < 0) top = y;
        bottom = y;
        break;
      }
  return top < 0 ? 0 : bottom - top + 1;
}

bool pictures_in(const char* dictionary, const std::vector<std::string>& names, std::vector<Bitmap>& out) {
  out.clear();
  std::wstring path = module_dir(nullptr) + L"TMNT.DAT";
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  std::vector<uint8_t> txd;
  bool ok = read_entry(f, dictionary, txd);
  CloseHandle(f);
  if (!ok) return false;
  std::map<std::string, size_t> want;
  for (size_t i = 0; i < names.size(); ++i) want[names[i]] = i;
  std::vector<Image> images;
  std::vector<int> index(names.size(), -1);
  decode_txd(txd, want, images, index);
  for (size_t i = 0; i < names.size(); ++i) {
    if (index[i] < 0) return false;
    const Image& img = images[index[i]];
    Bitmap b;
    b.w = img.w, b.h = img.h, b.text_w = img.w;
    b.bgra = img.px;
    out.push_back(std::move(b));
  }
  return true;
}

bool load() {
  std::lock_guard<std::mutex> lock(g_lock);
  if (g_ready) return true;
  if (!read_dictionaries()) return false;
  const size_t n_options = sizeof(kSources) / sizeof(kSources[0]);
  const size_t n_menu = sizeof(kMenuSources) / sizeof(kMenuSources[0]);
  const size_t n_story = sizeof(kStorySources) / sizeof(kStorySources[0]);
  const size_t n_all = n_options + n_menu + n_story + 3;  // (+ the story's Ч and Ы, the main menu's Ь)
  g_images.clear();
  g_images.reserve(n_all);
  g_owners.clear();
  g_owners.reserve(n_all);
  // Spacing as the game's labels have it: the gaps between letters and words.
  auto median = [](std::vector<int>& v, int fallback) {
    if (v.empty()) return fallback;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  };
  auto cut_all = [&](const std::vector<uint8_t>& txd, const Source* sources, size_t n, Font& font) {
    std::map<std::string, size_t> want;
    for (size_t i = 0; i < n; ++i) want[sources[i].texture] = i;
    std::vector<int> index(n, -1);
    decode_txd(txd, want, g_images, index);
    for (size_t i = 0; i < n; ++i) {
      if (index[i] < 0) continue;
      Image& img = g_images[index[i]];
      if (sources[i].scale < 0.999f) img = shrink(img, sources[i].scale);
      if (sources[i].cyan) recolour_cyan(img);
      cut(img, sources[i], font);
    }
    font.gap = median(font.gaps, font.gap);
    font.space = median(font.spaces, font.space);
  };
  Font& options = g_fonts[(int)Style::Options];
  Font& menu = g_fonts[(int)Style::MainMenu];
  Font& story = g_fonts[(int)Style::Story];
  cut_all(g_option_txd, kSources, n_options, options);
  if (!g_menu_txd.empty()) {
    cut_all(g_menu_txd, kMenuSources, n_menu, menu);
    cut_all(g_menu_txd, kStorySources, n_story, story);
    Glyph made;
    auto yery = menu.glyphs.find(0x42B);  // Ы
    if (yery != menu.glyphs.end() && make_soft_sign(yery->second, made)) menu.glyphs.emplace(0x42C, made);  // Ь
    auto n = story.glyphs.find(0x41D), r = story.glyphs.find(0x420);  // Н, Р
    if (n != story.glyphs.end() && make_che(n->second, made)) story.glyphs.emplace(0x427, made);  // Ч
    if (r != story.glyphs.end() && make_yery(r->second, made)) story.glyphs.emplace(0x42B, made);  // Ы
  }
  LOGI("game text: %zu letters from the option labels (gap %d, space %d), %zu from the main menu's (gap %d, space %d), "
       "%zu from the story menu's (gap %d)",
       options.glyphs.size(), options.gap, options.space, menu.glyphs.size(), menu.gap, menu.space, story.glyphs.size(),
       story.gap);
  g_ready = !options.glyphs.empty();
  return g_ready;
}

bool ready() { return g_ready.load(); }

std::string letters() {
  std::lock_guard<std::mutex> lock(g_lock);
  std::string out;
  for (const auto& kv : g_fonts[(int)Style::Options].glyphs) {
    uint32_t c = kv.first;
    if (c < 0x80) {
      out += char(c);
    } else {
      out += char(0xC0 | (c >> 6));
      out += char(0x80 | (c & 63));
    }
  }
  return out;
}

bool compose(const std::string& utf8, Bitmap& out, int margin, Style style) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (!g_ready) return false;
  const Font& font = g_fonts[(int)style];
  std::vector<uint32_t> text = codepoints(utf8);
  // Layout: where each letter's white body starts.
  std::vector<std::pair<const Glyph*, int>> placed;
  int cursor = margin, right = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == ' ') {
      cursor += font.space - font.gap;
      continue;
    }
    auto it = font.glyphs.find(text[i]);
    if (it == font.glyphs.end()) return false;
    const Glyph& g = it->second;
    placed.push_back({&g, cursor});
    cursor += (g.body1 - g.body0) + font.gap;
    right = cursor - font.gap;
  }
  int w = 64;
  while (w < right + 32) w *= 2;
  out.w = w, out.h = kCanvasH, out.text_w = right;
  out.bgra.assign(size_t(w) * kCanvasH, 0);
  for (const auto& pg : placed) {
    const Glyph& g = *pg.first;
    const Image& img = *g.img;
    int dy = font.baseline - g.baseline;
    int dx = pg.second - g.body0;  // unslanted column -> destination column
    for (int y = 0; y < img.h; ++y) {
      int ty = y + dy;
      if (ty < 0 || ty >= kCanvasH) continue;
      float off = g.shear * (g.base - y);
      for (int x = 0; x < img.w; ++x) {
        uint32_t c;
        if (g.owners) {  // its own pixels only
          size_t i = size_t(y) * img.w + x;
          if (g.owners->letter[i] != g.letter) continue;
          c = g.owners->px[i];
        } else {
          float xp = x - off;
          if (xp < g.x0 - 0.5f || xp >= g.x1 + 0.5f) continue;
          c = img.at(x, y);
        }
        uint32_t a = c >> 24;
        if (!a) continue;
        int tx = x + dx;
        if (tx < 0 || tx >= w) continue;
        uint32_t& d = out.bgra[size_t(ty) * w + tx];
        uint32_t da = d >> 24;
        auto mix = [a](uint32_t s, uint32_t t) { return (s * a + t * (255 - a)) / 255; };
        uint32_t r = mix((c >> 16) & 255, (d >> 16) & 255), gg = mix((c >> 8) & 255, (d >> 8) & 255),
                 b = mix(c & 255, d & 255);
        d = std::max(a, da) << 24 | r << 16 | gg << 8 | b;
      }
    }
  }
  return true;
}

}  // namespace game_text
