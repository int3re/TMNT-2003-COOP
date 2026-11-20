#include "mod/dojo_art.h"

#include <windows.h>

#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "common/log.h"
#include "mod/game_text.h"
#include "mod/lockstep.h"
#include "mod/overlay.h"

namespace dojo_art {
namespace {

constexpr int kSize = 1024;
// The old line in each of the three pictures (measured: its white is in rows
// 697..740, columns 294..712), with room for its soft edge.
constexpr int kX0 = 282, kX1 = 724, kY0 = 688, kY1 = 750;
constexpr int kReach = 14;         // how far above and below the box the ground is looked for
constexpr float kScale = 1.5f;     // the option labels' letters (caps 24 texels) at the old line's size
constexpr int kMiddleX = 520, kMiddleY = 718;
constexpr const char* kLine = "ДЛЯ ВСЕЙ КОМАНДЫ";
constexpr int kMargin = 16;

game_text::Bitmap g_bitmaps[3];
std::atomic<int> g_ready{0};  // 1: the three copies are made, -1: they could not be
IDirect3DTexture9* g_textures[3] = {};
IDirect3DDevice9* g_dev = nullptr;

// A pixel of the old line: the ground there is black or the sign's dark green
// (red and blue 0..1); the line is white with a grey edge.
bool written(uint32_t bgra) { return ((bgra >> 16) & 0xFF) > 10 || (bgra & 0xFF) > 10; }

// The old line out: each of its pixels gets the nearest pixel of the ground
// above or below it in its column.
void take_out(game_text::Bitmap& b) {
  const std::vector<uint32_t> was = b.bgra;
  auto at = [&](int x, int y) { return was[size_t(y) * kSize + x]; };
  for (int x = kX0; x <= kX1; ++x)
    for (int y = kY0; y <= kY1; ++y) {
      if (!written(at(x, y))) continue;
      uint32_t ground = 0xFF000000u;
      for (int d = 1; d <= (kY1 - kY0) + kReach; ++d) {
        const int up = y - d, down = y + d;
        if (up >= kY0 - kReach && !written(at(x, up))) {
          ground = at(x, up);
          break;
        }
        if (down <= kY1 + kReach && !written(at(x, down))) {
          ground = at(x, down);
          break;
        }
      }
      b.bgra[size_t(y) * kSize + x] = ground;
    }
}

// The new line in: the lettering, kScale times its size, its middle at the
// old line's middle.
void write_in(game_text::Bitmap& b, const game_text::Bitmap& line) {
  const float left = kMiddleX - (kMargin + line.text_w * 0.5f) * kScale, top = kMiddleY - line.h * 0.5f * kScale;
  const int x0 = std::max(0, (int)left), x1 = std::min(kSize - 1, (int)(left + line.w * kScale) + 1);
  const int y0 = std::max(0, (int)top), y1 = std::min(kSize - 1, (int)(top + line.h * kScale) + 1);
  auto sample = [&](float u, float v, int shift) {  // bilinear, one channel
    const int ix = (int)u, iy = (int)v;
    const float fx = u - ix, fy = v - iy;
    auto ch = [&](int x, int y) -> float {
      if (x < 0 || y < 0 || x >= line.w || y >= line.h) return 0.0f;
      const uint32_t p = line.bgra[size_t(y) * line.w + x];
      const float a = float(p >> 24) / 255.0f;
      return shift == 24 ? a : float((p >> shift) & 0xFF) * a;  // (colours weighed by their alpha)
    };
    return (ch(ix, iy) * (1 - fx) + ch(ix + 1, iy) * fx) * (1 - fy) + (ch(ix, iy + 1) * (1 - fx) + ch(ix + 1, iy + 1) * fx) * fy;
  };
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const float u = (x - left) / kScale, v = (y - top) / kScale;
      const float a = sample(u, v, 24);
      if (a <= 0.004f) continue;
      uint32_t& d = b.bgra[size_t(y) * kSize + x];
      uint32_t out = 0xFF000000u;
      for (int shift = 0; shift <= 16; shift += 8) {
        const float c = sample(u, v, shift) + float((d >> shift) & 0xFF) * (1.0f - a);
        out |= uint32_t(std::clamp(c, 0.0f, 255.0f) + 0.5f) << shift;
      }
      d = out;
    }
}

void make() {
  std::vector<game_text::Bitmap> got;
  if (!game_text::pictures_in("dojopr_pc.txd", {"dojo_title1", "dojo_title2", "dojo_title3"}, got) || got.size() != 3) {
    g_ready = -1;
    LOGW("dojo: the title cards' pictures are not available; in a game together they still say player 1 only");
    return;
  }
  // (the labels' letters are read at the game's start, maybe after this thread began)
  game_text::Bitmap line;
  bool have = false;
  for (int i = 0; i < 40 && !have; ++i) {
    have = game_text::compose(kLine, line, kMargin, game_text::Style::Options) && line.w > 0 && line.h > 0;
    if (!have) Sleep(500);
  }
  for (int i = 0; i < 3 && have; ++i) have = got[i].w == kSize && got[i].h == kSize;
  if (!have) {
    g_ready = -1;
    LOGW("dojo: the title cards are not as expected; in a game together they still say player 1 only");
    return;
  }
  for (int i = 0; i < 3; ++i) {
    take_out(got[i]);
    write_in(got[i], line);
    g_bitmaps[i] = std::move(got[i]);
  }
  g_ready = 1;
  LOGI("dojo: in a game together the three title cards say \"ДЛЯ ВСЕЙ КОМАНДЫ\" (the game's: \"Только для Игрока 1\")");
}

IDirect3DTexture9* texture(int i) {
  IDirect3DDevice9* dev = overlay::device();
  if (!dev) return nullptr;
  if (g_dev != dev) {
    for (IDirect3DTexture9*& t : g_textures)
      if (t) t->Release(), t = nullptr;
    g_dev = dev;
  }
  if (g_textures[i]) return g_textures[i];
  const game_text::Bitmap& b = g_bitmaps[i];
  IDirect3DTexture9* t = nullptr;
  if (b.bgra.empty() || FAILED(dev->CreateTexture(b.w, b.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) return nullptr;
  D3DLOCKED_RECT lr{};
  if (FAILED(t->LockRect(0, &lr, nullptr, 0))) {
    t->Release();
    return nullptr;
  }
  for (int y = 0; y < b.h; ++y)
    std::memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, &b.bgra[size_t(y) * b.w], size_t(b.w) * 4);
  t->UnlockRect(0);
  return g_textures[i] = t;
}

}  // namespace

void install() { std::thread(make).detach(); }

IDirect3DTexture9* instead_of(const uint8_t* rw_texture) {
  if (!rw_texture || g_ready.load() != 1) return nullptr;
  const char* name = reinterpret_cast<const char*>(rw_texture + 0x10);
  if (std::strncmp(name, "dojo_title", 10) != 0 || name[10] < '1' || name[10] > '3' || name[11]) return nullptr;
  // Everybody takes part only in a game together (four_players.cpp).
  if (lockstep::state() != lockstep::State::Running || lockstep::players() < 2) return nullptr;
  return texture(name[10] - '1');
}

}  // namespace dojo_art
