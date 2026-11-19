#include "mod/four_hud.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "common/log.h"
#include "mod/four_players.h"
#include "mod/native_menu.h"
#include "mod/patch.h"
#include "mod/revive.h"
#include "mod/widescreen.h"

namespace four_hud {
namespace {

// The play screen's sprites are drawn by 0x4abab2(sprite) every frame (hooked
// by main_menu.cpp, which tells on_sprite); player 1's portrait is the sprite
// at +0x3C of its panel block (0x60da88 + p*0x670, 0x4065be): while it is
// drawn, the panels are on the screen.
constexpr const char* kPanelSig =
    "55 8B EC 51 51 A1 ?? ?? ?? ?? 89 45 FC 0F B6 45 08 53 56 8B F0 69 F6 70 06 00 00 50 81 C6";
constexpr size_t kBlocksAt = 0x1E;     // add esi, imm32: the panel blocks
constexpr size_t kPortrait = 0x3C;     // player 1's portrait sprite in its block
// The hearts at the top (0x406d46): the heart (mov eax, imm at +5) and its
// number (mov [current], imm at +0x2C, the value at +0x32), both drawn with
// 0x4abab2. The enemies' count (0x406e0c) is drawn the same way: the hearts'
// one reads them at +0x45 (0x40427a: mov eax, [info]; mov eax, [eax + 0x24]).
constexpr const char* kHeartsSig =
    "55 8B EC 51 B8 ?? ?? ?? ?? 53 50 A3 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? ?? ?? ?? ?? E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? "
    "C1 E0 04 05 ?? ?? ?? ?? C7 05";
constexpr size_t kHeartAt = 0x05, kHeartNumberAt = 0x32, kHeartsGetCall = 0x45;

// Player data (four::player_data): +0x2C full life, +0x30 life (a hit takes
// from it: 0x419d56). The game's bar is 170 * shown / full, the shown life
// (panel block +0x654) following the life every frame: down 1, up 8 (0x409f98).
constexpr size_t kFull = 0x2C, kLife = 0x30, kScore = 0x5C, kKind = 0x84;
constexpr size_t kShurikens[3] = {0x88, 0x90, 0x8C};

// Player 1's panel, as the game lays it out (0x5dfda0..: the 512x480 play
// screen, x right, y down, 0,0 the middle; centres; sizes from the
// pictures' descriptors at 0x5dfa00).
struct Piece {
  const char* picture;
  float x, y, w, h;
};
constexpr Piece kBack{"g_back", -127, -177, 256, 128};
constexpr Piece kBarBase{"g_tairyoku_sita", -98, -195, 256, 32};
constexpr Piece kBar{"g_tairyoku2", -102, -194, 170, 32};
constexpr Piece kFace{"g_face_story", -221, -177, 85, 128};  // 85 of 512 a face
constexpr Piece kShuriken{"g_syuriken", -164, -178, 40, 32};  // 40 of 128 a kind
constexpr Piece kShurikenNum{"g_syuriken_num", 0, -168, 14, 16};  // 14 of 256 a digit
constexpr float kShurikenUnitsX = -146, kShurikenStep = 10;
constexpr Piece kScoreLabel{"g_score", -99, -186, 64, 16};
constexpr Piece kScoreNum{"g_score_num", 0, -174, 18, 16};  // 18 of 256 a digit
// Down: "ПРОДОЛЖИТЬ?" (g_cont, 256x64) small over the bar while hearts are left.
constexpr Piece kContinue{"g_cont", -110, -188, 256 * 0.55f, 64 * 0.55f};
constexpr uint32_t kDim = 0x707070;
constexpr float kScoreUnitsX = -55, kScoreStep = 15;

// Players 3 and 4: this panel smaller, under players 1 and 2's.
constexpr float kScale = 0.66f, kBelow = 130.0f;
constexpr size_t kBlockSize = 0x670;

// [video] hud_size. The sprites (0x4abab2) are placed in a space the game
// sets with 0x4ab8fc(x0, y0, width, height): screen x = (x - x0) * W / width,
// y up from the bottom at y0 (the play screen: 0, 0, 640, 480). Sprite: +0 x,
// +4 y, +0xC and +0x10 the size (in that space, or pixels in other modes),
// the anchor a share of the size. A panel's sprites (all in its block) shrink
// toward its top corner, the hearts toward the top middle. The health bars
// are drawn by 0x4abc7a(sprite, x0, y0, ... x3, y3): four corners, same space.
constexpr const char* kQuadSig =
    "55 8B EC 81 EC 80 00 00 00 8B 45 08 83 78 38 00 0F 84 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 8B 09 8B 49 60 DB 41 0C";
constexpr const char* kSpaceSig =
    "8B 44 24 04 A3 ?? ?? ?? ?? 8B 44 24 08 A3 ?? ?? ?? ?? 8B 44 24 0C A3 ?? ?? ?? ?? 8B 44 24 10 A3 ?? ?? ?? ?? C3";
constexpr size_t kSpaceAt[4] = {0x05, 0x0E, 0x17, 0x20};
// The radar (0x407405 to its ret at +0x3C4): its frame (g_radar), its icons
// (g_radar_icon) and the enemies' count (g_combo_num) beside it, bottom right.
constexpr const char* kRadarSig =
    "55 8B EC 83 EC 4C 53 56 57 E8 ?? ?? ?? ?? 8B D8 E8 ?? ?? ?? ?? 89 45 F0 E8 ?? ?? ?? ?? 8B F8 E8";
constexpr size_t kRadarSize = 0x3C6;
uintptr_t g_radar = 0;
constexpr int kX = 0, kY = 1, kW = 3, kH = 4;  // the sprite's floats
float g_size = 1.0f;
bool g_trace = false;  // [debug] hud_trace
const float* g_space[4] = {};  // x0, y0, width, height

uint8_t* g_blocks = nullptr;
const void* g_heart = nullptr;
const void* g_heart_number = nullptr;
bool g_seen = false;  // player 1's panel was drawn since the last Present
uint32_t g_presents = 0, g_seen_at = 0;  // Presents so far; the last one with the panel
std::vector<native_menu::HudSprite> g_sprites;
int g_shown[2] = {-1, -1};  // players 3 and 4's shown life (-1: none yet)

int face_of(int character) {  // as 0x4065be: the portraits' order Leo, Mike, Raph, Don
  switch (character) {
    case 1: return 2;
    case 2: return 1;
    case 3: return 3;
    case 4: return 0;
    default: return -1;
  }
}

// Which piece of the screen a sprite is: 0 player 1's panel (left), 1 player
// 2's (right), 2 the hearts (top middle), 3 the radar (bottom right), -1 none.
enum Side { kNone = -1, kLeft = 0, kRight = 1, kMiddle = 2, kRadar = 3 };
int side_of(const void* sprite, const void* site) {
  auto* s = static_cast<const uint8_t*>(sprite);
  if (!s) return kNone;
  if (g_blocks && s >= g_blocks && s < g_blocks + 2 * kBlockSize) return s < g_blocks + kBlockSize ? kLeft : kRight;
  if (sprite == g_heart || sprite == g_heart_number) return kMiddle;
  auto at = reinterpret_cast<uintptr_t>(site);
  if (g_radar && at >= g_radar && at < g_radar + kRadarSize) return kRadar;
  return kNone;
}

int edge_of(int side) { return side == kLeft ? -1 : side == kRight || side == kRadar ? 1 : 0; }

// The corner a panel's sprite shrinks toward, in the sprites' space (the
// radar keeps its size).
bool corner_of(int side, float& cx, float& cy) {
  if (g_size >= 0.999f || !g_space[0] || side < kLeft || side > kMiddle) return false;
  const float x0 = *g_space[0], y0 = *g_space[1], w = *g_space[2], h = *g_space[3];
  cx = side == kLeft ? x0 : side == kRight ? x0 + w : x0 + w * 0.5f;
  cy = y0 + h;  // the top
  return true;
}

using Quad_t = void(__cdecl*)(void* sprite, float x0, float y0, float x1, float y1, float x2, float y2, float x3,
                              float y3);
Quad_t oQuad = nullptr;

void __cdecl hkQuad(void* sprite, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3) {
  trace(sprite, _ReturnAddress(), "quad");
  int side = side_of(sprite, _ReturnAddress()), edge = edge_of(side);
  if (edge) widescreen::set_edge(edge);
  float cx, cy;
  if (!corner_of(side, cx, cy)) {
    oQuad(sprite, x0, y0, x1, y1, x2, y2, x3, y3);
  } else {
    auto X = [&](float x) { return cx + (x - cx) * g_size; };
    auto Y = [&](float y) { return cy + (y - cy) * g_size; };
    float* f = static_cast<float*>(sprite);
    const float w = f[kW], h = f[kH];  // (the anchor's share of them)
    f[kW] = w * g_size, f[kH] = h * g_size;
    oQuad(sprite, X(x0), Y(y0), X(x1), Y(y1), X(x2), Y(y2), X(x3), Y(y3));
    f[kW] = w, f[kH] = h;
  }
  if (edge) widescreen::set_edge(0);
}

struct Panel {
  bool right = false;  // player 4: on the right, mirrored like player 2's
  std::vector<native_menu::HudSprite>* out = nullptr;

  float k() const { return kScale * g_size; }
  float X(float x) const {
    float v = -256.0f + (x + 256.0f) * k();
    return right ? -v : v;
  }
  float Y(float y) const { return -240.0f + ((y + 240.0f) * kScale + kBelow) * g_size; }

  // A piece (or a part of its picture, u0..u1) at its place; mirrored on the right.
  void piece(const Piece& p, float u0 = 0, float u1 = 1, float x = 1e9f, float w = -1, bool mirror = true,
             uint32_t rgb = 0xFFFFFF) const {
    float px = x < 1e8f ? x : p.x, pw = w >= 0 ? w : p.w;
    native_menu::HudSprite s{p.picture, u0, 0, u1, 1, X(px), Y(p.y), pw * k(), p.h * k()};
    s.rgb = rgb;
    s.edge = right ? 1 : -1;
    if (right && mirror) std::swap(s.u0, s.u1);
    out->push_back(s);
  }

  // Digits read left to right on both sides: their places are mirrored as a block.
  void digits(const Piece& p, unsigned value, int max_digits, float units_x, float step, float frame) const {
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%u", value);
    if (n > max_digits) n = max_digits, std::memset(buf, '9', n);
    for (int i = 0; i < n; ++i) {
      int d = buf[i] - '0';
      float x = units_x - step * (n - 1 - i);  // as on the left
      if (right) x = units_x - step * i;          // mirrored: the units digit becomes the leftmost place
      native_menu::HudSprite s{p.picture, d * frame, 0, (d + 1) * frame, 1, X(x), Y(p.y), p.w * k(), p.h * k()};
      s.edge = right ? 1 : -1;
      out->push_back(s);
    }
  }
};

// A player who is down: the panel dimmed, "ПРОДОЛЖИТЬ?" while hearts are left
// (its decide brings it back).
void add_down_panel(int player, int face, std::vector<native_menu::HudSprite>& out) {
  Panel p;
  p.right = player == 3;
  p.out = &out;
  p.piece(kBack, 0, 1, 1e9f, -1, true, kDim);
  p.piece(kBarBase, 0, 1, 1e9f, -1, true, kDim);
  p.piece(kFace, face * 85.0f / 512.0f, (face + 1) * 85.0f / 512.0f, 1e9f, -1, true, kDim);
  if (four::credits() > 0) p.piece(kContinue, 0, 1, 1e9f, -1, false);
}

void add_panel(int player, std::vector<native_menu::HudSprite>& out) {
  const uint8_t* data = static_cast<const uint8_t*>(four::player_data(player));
  int face = face_of(four::character(player));
  if (face < 0) return;
  if (!data) {
    g_shown[player - 2] = -1;
    if (four::down(player)) add_down_panel(player, face, out);
    return;
  }
  auto i32 = [&](size_t off) { return *reinterpret_cast<const int32_t*>(data + off); };
  Panel p;
  p.right = player == 3;
  p.out = &out;

  int life = i32(kLife), full = i32(kFull);
  if (life < 0) life = 0;
  if (full <= 0) full = life > 0 ? life : 1;
  int& shown = g_shown[player - 2];
  if (shown < 0) shown = life;
  shown = shown > life ? shown - 1 : std::min(shown + 8, life);
  float ratio = shown <= 0 ? 0.0f : shown >= full ? 1.0f : float(shown) / float(full);

  p.piece(kBack);
  p.piece(kBarBase);
  if (ratio > 0) {  // the bar shortens from its tip
    const float left = kBar.x - kBar.w * 0.5f, w = kBar.w * ratio;
    p.piece(kBar, 0, ratio, left + w * 0.5f, w);
  }
  p.piece(kFace, face * 85.0f / 512.0f, (face + 1) * 85.0f / 512.0f);
  int kind = i32(kKind);
  if (kind < 0 || kind > 2) kind = 0;
  p.piece(kShuriken, kind * 40.0f / 128.0f, (kind + 1) * 40.0f / 128.0f);
  int count = i32(kShurikens[kind]);
  p.digits(kShurikenNum, count < 0 ? 0 : (unsigned)count, 3, kShurikenUnitsX, kShurikenStep, 14.0f / 256.0f);
  p.piece(kScoreLabel, 0, 1, 1e9f, -1, false);
  int score = i32(kScore);
  p.digits(kScoreNum, score < 0 ? 0 : (unsigned)score, 8, kScoreUnitsX, kScoreStep, 18.0f / 256.0f);
}

}  // namespace

void install(const Config& cfg) {
  g_trace = cfg.debug_hud_trace;
  set_size(cfg.hud_size);
  if (uintptr_t a = patch::find("sprite_space", kSpaceSig)) {
    for (int i = 0; i < 4; ++i) g_space[i] = reinterpret_cast<const float*>(patch::read<uint32_t>(a + kSpaceAt[i]));
    for (const float* f : g_space)
      if (!patch::in_image((uintptr_t)f, 4)) g_space[0] = nullptr;
  }
  if (!g_space[0]) LOGW("play screen panels: the sprites' space is not known, they keep the game's size");
  if (uintptr_t q = patch::find("draw_quad4", kQuadSig))
    patch::hook("draw_quad4", q, (void*)&hkQuad, (void**)&oQuad);
  if (uintptr_t r = patch::find("radar_draw", kRadarSig))
    if (patch::read<uint8_t>(r + kRadarSize - 2) == 0xC9 && patch::read<uint8_t>(r + kRadarSize - 1) == 0xC3) g_radar = r;
  if (!g_radar) LOGW("play screen: the radar's drawing was not recognised; it stays in the 4:3 middle");
  for (uintptr_t h : patch::find_all(kHeartsSig)) {
    uintptr_t at = h + kHeartsGetCall;
    if (patch::read<uint8_t>(at) != 0xE8) continue;
    uintptr_t get = at + 5 + patch::read<int32_t>(at + 1);
    if (!patch::in_image(get, 9) || patch::read<uint8_t>(get) != 0xA1 || patch::read<uint32_t>(get + 5) != 0xC324408Bu)
      continue;
    uintptr_t a = patch::read<uint32_t>(h + kHeartAt), b = patch::read<uint32_t>(h + kHeartNumberAt);
    if (patch::in_image(a, 0x40) && patch::in_image(b, 0x40))
      g_heart = reinterpret_cast<const void*>(a), g_heart_number = reinterpret_cast<const void*>(b);
  }
  if (!g_heart) LOGW("four players: the hearts at the top are not where expected; they stay");
  uintptr_t panel = patch::find("hud_panel_portrait", kPanelSig);
  if (!panel) return;
  g_blocks = reinterpret_cast<uint8_t*>(patch::read<uint32_t>(panel + kBlocksAt));
  if (!patch::in_image((uintptr_t)g_blocks, 2 * kBlockSize)) {
    g_blocks = nullptr;
    LOGW("four players: the play screen's panels are not where expected; no panels for players 3 and 4");
  }
}

bool on_sprite(const void* sprite) {
  if (g_blocks && sprite == g_blocks + kPortrait) g_seen = true;
  if (sprite && (sprite == g_heart || sprite == g_heart_number) && revive::team()) return false;
  return true;
}

void trace(const void* sprite, const void* site, const char* how) {
  static std::vector<const void*> seen;
  if (!g_trace || !sprite || seen.size() > 400) return;
  if (std::find(seen.begin(), seen.end(), site) != seen.end()) return;
  seen.push_back(site);
  auto* s = static_cast<const uint8_t*>(sprite);
  auto* f = reinterpret_cast<const float*>(s);
  const uint8_t* tex = *reinterpret_cast<const uint8_t* const*>(s + 0x18);
  char name[33] = "-";
  if (tex) {
    memcpy(name, tex + 0x10, 32);
    name[32] = 0;
  }
  float x0 = g_space[0] ? *g_space[0] : 0, y0 = g_space[1] ? *g_space[1] : 0, w = g_space[2] ? *g_space[2] : 0,
        h = g_space[3] ? *g_space[3] : 0;
  LOGI("hud trace: %s site %p sprite %p '%s' at %.3f %.3f size %.3f x %.3f anchor %.2f %.2f mode %d (space %.0f %.0f %.0f %.0f)",
       how, site, sprite, name, f[0], f[1], f[3], f[4], f[11], f[12], *reinterpret_cast<const int32_t*>(s + 0x34), x0, y0,
       w, h);
}

void set_size(int percent) {
  g_size = std::clamp(percent, 30, 100) / 100.0f;
  if (percent < 100) LOGI("play screen panels: %d%% of the game's size", percent);
}

Scaled::Scaled(void* sprite, const void* site) {
  int side = side_of(sprite, site);
  edge_ = edge_of(side);
  if (edge_) widescreen::set_edge(edge_);
  float cx, cy;
  if (!corner_of(side, cx, cy)) return;
  s_ = static_cast<float*>(sprite);
  saved_[0] = s_[kX], saved_[1] = s_[kY], saved_[2] = s_[kW], saved_[3] = s_[kH];
  s_[kX] = cx + (s_[kX] - cx) * g_size;
  s_[kY] = cy + (s_[kY] - cy) * g_size;
  s_[kW] *= g_size;
  s_[kH] *= g_size;
}

Scaled::~Scaled() {
  if (s_) s_[kX] = saved_[0], s_[kY] = saved_[1], s_[kW] = saved_[2], s_[kH] = saved_[3];
  if (edge_) widescreen::set_edge(0);
}

bool panels_shown() { return g_seen || (g_seen_at && g_presents - g_seen_at < 30); }

bool prepare(IDirect3DDevice9*) {
  g_sprites.clear();
  bool seen = g_seen;
  g_seen = false;
  ++g_presents;
  if (seen) g_seen_at = g_presents;
  if (!seen || !four::enabled()) return false;
  for (int p = 2; p < 4; ++p) add_panel(p, g_sprites);
  return !g_sprites.empty();
}

void draw(IDirect3DDevice9* dev) { native_menu::draw_hud(dev, g_sprites); }

}  // namespace four_hud
