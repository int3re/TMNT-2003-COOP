#include "mod/revive.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "common/log.h"
#include "common/net.h"
#include "mod/four_players.h"
#include "mod/nametags.h"
#include "mod/native_menu.h"
#include "mod/patch.h"

namespace revive {
namespace {

// A turtle knocked down (action 0x12) lands and lies (0x15). When its lying
// animation is over (body +4 == 1) 0x419256(turtle, data, action) sends it
// on: 0x41921d(turtle, data) says whether it is dead (life <= 0, the game not
// in a no-death mode): 0x1D, which turns 0x1E 24 frames on, and the turtle's
// character removes it (0x431f6f); else 0x16, getting up. A turtle the game
// would let die there lies on instead while another one is up.
constexpr const char* kLyingSig =
    "55 8B EC 56 8B 75 08 56 E8 ?? ?? ?? ?? 83 F8 01 59 75 21 FF 75 0C 56 E8 ?? ?? ?? ?? 85 C0 59 59 6A 00 74 04 "
    "6A 1D EB 02 6A 16";
constexpr size_t kIsDeadCall = 0x17;
// A hit on a turtle: its character's hit handler asks 0x432e20(data, who,
// attack) first; 1 = it is not hurt (as in action 9).
constexpr const char* kIgnoreSig =
    "FF 74 24 04 E8 ?? ?? ?? ?? 85 C0 59 74 04 33 C0 40 C3 FF 74 24 0C FF 74 24 0C E8";
// Every frame a turtle puts its body's volumes up for the collisions
// (0x418618(turtle, hit handler), from its character's update; not while it
// is dying, action 0x1D): enemies see and go for what is up there. A stunned
// one puts up none either, so they leave it for those still standing.
constexpr const char* kVolumesSig =
    "55 8B EC 83 EC 14 56 8B 75 08 8B 46 60 8B 80 54 01 00 00 83 78 10 1D 89 45 08 74";

// Player data: +0x2C full life, +0x30 life. Turtle (actor): +0x60 body; body
// +4 its animation is over (1), +0x7C position (feet; y up).
constexpr size_t kFull = 0x2C, kLife = 0x30, kBody = 0x60, kAnimOver = 0x04, kPos = 0x7C;

// After the first real game (0.11.10, the players: too quick, too much life,
// and the one raising could fight on): four seconds of holding, started over
// when the one raising takes a hit, no attacks meanwhile, a fifth of the life.
constexpr float kReach = 1.6f;    // world units (x, z) from the stunned turtle's place
constexpr int kHoldFrames = 240;  // four seconds of holding the button
constexpr int kShare = 5;         // raised with a fifth of its full life
// The key table's attacks (slots): juggle, special, shuriken, attack, strong.
constexpr uint16_t kAttacks = (1u << 1) | (1u << 2) | (1u << 4) | (1u << 6) | (1u << 7);

using Lying_t = void(__cdecl*)(uint8_t* turtle, uint8_t* data, void* action);
using IsDead_t = int(__cdecl*)(uint8_t* turtle, uint8_t* data);
using Ignore_t = int(__cdecl*)(uint8_t* data, void* who, void* attack);
Lying_t oLying = nullptr;
IsDead_t g_is_dead = nullptr;
Ignore_t oIgnore = nullptr;
using Volumes_t = void(__cdecl*)(uint8_t* turtle, void* handler);
Volumes_t oVolumes = nullptr;

bool g_own_on = true;  // [mod] revive
int g_own_button = 3;  // [mod] revive_button
bool g_session = false;  // a game together: the host's
bool g_session_on = true;
int g_session_button = 3;
float g_reach = kReach;  // ([debug] revive_reach in the lab)

struct Down {
  uint8_t* turtle = nullptr;  // lying stunned (null: not)
  int progress = 0;           // frames of being raised
};
Down g_down[4];
uint8_t* g_let_die[4] = {};  // released: the game's own end goes on
int g_helping[4] = {-1, -1, -1, -1};  // whom each player is raising this frame (-1: nobody)

uint8_t* turtle_of(int p) { return static_cast<uint8_t*>(four::turtle(p)); }
uint8_t* data_of(int p) { return static_cast<uint8_t*>(four::player_data(p)); }
int32_t& i32(uint8_t* d, size_t off) { return *reinterpret_cast<int32_t*>(d + off); }
uint8_t* body_of(uint8_t* turtle) { return turtle ? *reinterpret_cast<uint8_t**>(turtle + kBody) : nullptr; }
const float* pos_of(uint8_t* turtle) {
  uint8_t* b = body_of(turtle);
  return b ? reinterpret_cast<const float*>(b + kPos) : nullptr;
}

int player_of(const uint8_t* turtle) {
  for (int p = 0; turtle && p < 4; ++p)
    if (turtle_of(p) == turtle) return p;
  return -1;
}

// In play with some life and not stunned: can raise the others.
bool up(int p) {
  uint8_t* d = data_of(p);
  return turtle_of(p) && d && !g_down[p].turtle && i32(d, kLife) > 0;
}

bool someone_else_up(int p) {
  for (int q = 0; q < 4; ++q)
    if (q != p && up(q)) return true;
  return false;
}

void __cdecl hkLying(uint8_t* turtle, uint8_t* data, void* action) {
  int p = team() ? player_of(turtle) : -1;
  if (p < 0) return oLying(turtle, data, action);
  if (g_down[p].turtle == turtle) return;  // stunned: it lies on
  uint8_t* body = body_of(turtle);
  if (g_let_die[p] != turtle && body && i32(body, kAnimOver) == 1 && g_is_dead(turtle, data)) {
    if (someone_else_up(p)) {
      g_down[p] = Down{};
      g_down[p].turtle = turtle;
      LOGI("revive: player %d is down, stunned (life %d); a turtle next to it holding the button raises it", p + 1,
           i32(data, kLife));
      return;
    }
    // The last one up: the level is lost (no hearts for the team).
    four::take_credits();
    g_let_die[p] = turtle;
    LOGI("revive: player %d was the last one up, the level is lost", p + 1);
  }
  oLying(turtle, data, action);
}

bool g_trace = false;  // [debug] actions=1: hits that reach players 3 and 4

int __cdecl hkIgnore(uint8_t* data, void* who, void* attack) {
  for (int p = 0; p < 4; ++p)
    if (g_down[p].turtle && data && data == data_of(p)) return 1;  // a stunned turtle is left alone
  int r = oIgnore(data, who, attack);
  if (!r && data)  // a hit taken by one raising another: the raising starts over
    for (int q = 0; q < 4; ++q)
      if (data == data_of(q) && g_helping[q] >= 0 && g_down[g_helping[q]].turtle && g_down[g_helping[q]].progress) {
        LOGI("revive: player %d was hit while raising player %d: it starts over", q + 1, g_helping[q] + 1);
        g_down[g_helping[q]].progress = 0;
      }
  if (g_trace && data)
    for (int p = 0; p < 4; ++p)
      if (data == data_of(p)) {
        // who hit it: the attack's owner (attack +0x48), its brain (body +0x12C), once per brain
        uint8_t* atk = attack ? *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(attack) + 0x48) : nullptr;
        uint8_t* wb = atk ? body_of(atk) : nullptr;
        void* brain = wb ? *reinterpret_cast<void**>(wb + 0x12C) : nullptr;
        static void* seen[32];
        static int n = 0;
        bool known = false;
        for (int i = 0; i < n; ++i) known |= seen[i] == brain;
        if (!known && n < 32) seen[n++] = brain;
        if (!known || p >= 2) {
          const uint32_t* cb = wb ? reinterpret_cast<const uint32_t*>(wb + 0x140) : nullptr;  // its own callbacks
          LOGI("revive (lab): a hit reaches player %d (%s) from %p, brain %p, body+0x140: %08X %08X %08X %08X %08X %08X",
               p + 1, r ? "ignored" : "taken", atk, brain, cb ? cb[0] : 0, cb ? cb[1] : 0, cb ? cb[2] : 0,
               cb ? cb[3] : 0, cb ? cb[4] : 0, cb ? cb[5] : 0);
        }
      }
  return r;
}

void __cdecl hkVolumes(uint8_t* turtle, void* handler) {
  for (const Down& d : g_down)
    if (d.turtle && d.turtle == turtle) return;  // stunned: nothing to see or hit
  oVolumes(turtle, handler);
}

void raise(int p) {
  uint8_t* d = data_of(p);
  int full = d ? i32(d, kFull) : 0;
  int life = std::max(1, (full + kShare - 1) / kShare);
  if (d) i32(d, kLife) = life;  // its lying ends with getting up (0x16)
  g_down[p] = Down{};
  g_let_die[p] = nullptr;
  LOGI("revive: player %d raised with %d of %d life", p + 1, life, full);
}

void let_die(int p, const char* why) {
  g_let_die[p] = g_down[p].turtle;
  g_down[p] = Down{};
  LOGI("revive: player %d is not raised (%s)", p + 1, why);
}

// Everybody is down: the level is lost. The hearts are not the team's: none
// are left, so the game ends there (0x411c48: nobody in play, no hearts).
void lose() {
  four::take_credits();
  for (int p = 0; p < 4; ++p)
    if (g_down[p].turtle) let_die(p, "everybody is down");
  LOGI("revive: everybody is down, the level is lost");
}

}  // namespace

void install(const Config& cfg) {
  g_own_on = cfg.revive;
  g_trace = cfg.debug_actions;
  g_own_button = cfg.revive_button;
  if (cfg.debug_revive_reach > 0) g_reach = cfg.debug_revive_reach;
  if (!four::installed()) {  // (its frame and the players' records are four_players.cpp's)
    LOGW("revive: needs the players' hooks of four_players.cpp; a turtle out of life dies as in the game");
    return;
  }
  uintptr_t lying = patch::find("turtle_lying", kLyingSig);
  uintptr_t ignore = patch::find("turtle_hit_ignored", kIgnoreSig);
  uintptr_t volumes = patch::find("turtle_volumes", kVolumesSig);
  if (!lying || !ignore || !volumes) {
    LOGW("revive: the game is not laid out as expected; a turtle out of life dies as in the game");
    return;
  }
  uintptr_t at = lying + kIsDeadCall;
  if (patch::read<uint8_t>(at) == 0xE8) {
    uintptr_t t = at + 5 + patch::read<int32_t>(at + 1);
    if (patch::in_image(t, 1)) g_is_dead = reinterpret_cast<IsDead_t>(t);
  }
  if (!g_is_dead || !patch::hook("turtle_hit_ignored", ignore, (void*)&hkIgnore, (void**)&oIgnore) ||
      !patch::hook("turtle_volumes", volumes, (void*)&hkVolumes, (void**)&oVolumes) ||
      !patch::hook("turtle_lying", lying, (void*)&hkLying, (void**)&oLying)) {
    LOGW("revive: hooks not in place; a turtle out of life dies as in the game");
    oLying = nullptr;
    return;
  }
  LOGI("revive: %s (button: slot %d, reach %.1f)", g_own_on ? "on" : "off", g_own_button, g_reach);
}

bool enabled() { return oLying && (g_session ? g_session_on : g_own_on); }
int button() { return g_session ? g_session_button : g_own_button; }
void set_session(bool on, int button) {
  g_session = true;
  g_session_on = on;
  g_session_button = button >= 0 && button < 16 ? button : 3;
  LOGI("revive: the host's setting for this game: %s, button slot %d", on ? "on" : "off", g_session_button);
}
void clear_session() { g_session = false; }

bool team() { return enabled() && four::team_size() >= 2; }

bool down(int player) { return player >= 0 && player < 4 && g_down[player].turtle != nullptr; }
bool any_down() {
  for (const Down& d : g_down)
    if (d.turtle) return true;
  return false;
}

void on_frame() {
  for (int p = 0; p < 4; ++p) {  // a turtle gone (the level over, ...): nothing to raise
    if (g_down[p].turtle && turtle_of(p) != g_down[p].turtle) g_down[p] = Down{};
    if (g_let_die[p] && turtle_of(p) != g_let_die[p]) g_let_die[p] = nullptr;
  }
  if (!any_down()) return;
  if (!team()) {
    for (int p = 0; p < 4; ++p)
      if (g_down[p].turtle) let_die(p, "revive is off");
    return;
  }
  bool anyone = false;
  for (int q = 0; q < 4; ++q) anyone |= up(q);
  if (!anyone) return lose();

  const uint16_t raw = uint16_t(1u << button());
  for (int& h : g_helping) h = -1;
  for (int p = 0; p < 4; ++p) {
    Down& d = g_down[p];
    const float* a = d.turtle ? pos_of(d.turtle) : nullptr;
    if (!a) continue;
    bool helped = false;
    for (int q = 0; q < 4; ++q) {
      if (q == p || !up(q)) continue;
      const float* b = pos_of(turtle_of(q));
      if (!b) continue;
      float dx = b[0] - a[0], dz = b[2] - a[2];
      if (dx * dx + dz * dz > g_reach * g_reach) continue;
      bool holding = (four::held(q) & raw) != 0;
      helped |= holding;
      if (holding) g_helping[q] = p;
      // Next to it the button raises (it does not do its own thing); while
      // raising, no attacks.
      four::mask(q, holding ? uint16_t(raw | kAttacks) : raw);
    }
    d.progress = helped ? d.progress + 1 : std::max(0, d.progress - 2);
    if (d.progress >= kHoldFrames) raise(p);
  }
}

// --- the stars and the bar ---------------------------------------------------------
namespace {
std::vector<native_menu::HudSprite> g_bars;
std::vector<native_menu::Star> g_stars;
constexpr float kStarsLift = 0.85f;  // over the lying turtle's place, world units
}  // namespace

bool prepare(IDirect3DDevice9* dev) {
  g_bars.clear();
  g_stars.clear();
  if (!any_down() || !dev || !nametags::world_shown()) return false;
  IDirect3DSurface9* bb = nullptr;
  D3DSURFACE_DESC desc{};
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
  bb->GetDesc(&desc);
  bb->Release();
  const float W = (float)desc.Width, H = (float)desc.Height;
  const float t = (float)(net::now_ms() % 100000u) * 0.001f;
  for (int p = 0; p < 4; ++p) {
    const Down& d = g_down[p];
    const float* f = d.turtle ? pos_of(d.turtle) : nullptr;
    if (!f) continue;
    const float c[3] = {f[0], f[1] + kStarsLift, f[2]}, l[3] = {c[0] - 0.4f, c[1], c[2]}, r[3] = {c[0] + 0.4f, c[1], c[2]};
    float cx, cy, lx, ly, rx_, ry_;
    if (!nametags::to_screen(c, W, H, cx, cy)) continue;
    float rx = H * 0.05f;
    if (nametags::to_screen(l, W, H, lx, ly) && nametags::to_screen(r, W, H, rx_, ry_)) rx = std::fabs(rx_ - lx) * 0.5f;
    rx = std::clamp(rx, H * 0.02f, H * 0.12f);
    const float ry = rx * 0.35f;
    // Three gold stars going round over it, the far ones smaller and dimmer.
    native_menu::Star stars[3];
    float depth[3];
    for (int k = 0; k < 3; ++k) {
      float a = t * 3.0f + k * 2.0944f;
      depth[k] = (std::sin(a) + 1.0f) * 0.5f;  // 1 = in front
      stars[k].x = cx + std::cos(a) * rx;
      stars[k].y = cy + std::sin(a) * ry;
      stars[k].r = std::max(4.0f, rx * (0.28f + 0.12f * depth[k]));
      stars[k].turn = t * 2.5f + k;
      stars[k].alpha = 170 + int(85 * depth[k]);
      stars[k].rgb = 0xFFE040;
    }
    int order[3] = {0, 1, 2};
    std::sort(order, order + 3, [&](int a, int b) { return depth[a] < depth[b]; });
    for (int k : order) g_stars.push_back(stars[k]);
    if (d.progress > 0) {  // being raised: a bar over the stars
      const float w = rx * 2.6f, h = std::max(4.0f, rx * 0.2f), y = cy - ry - rx * 0.7f;
      float frac = std::min(1.0f, d.progress / float(kHoldFrames));
      native_menu::HudSprite back{nullptr, 0, 0, 1, 1, cx, y, w + 4, h + 4};
      back.alpha = 170;
      back.rgb = 0x000000;
      g_bars.push_back(back);
      native_menu::HudSprite fill{nullptr, 0, 0, 1, 1, cx - w * 0.5f + w * frac * 0.5f, y, w * frac, h};
      fill.alpha = 240;
      fill.rgb = 0x50E050;
      g_bars.push_back(fill);
    }
  }
  return !g_stars.empty() || !g_bars.empty();
}

void draw(IDirect3DDevice9* dev) {
  native_menu::draw_stars(dev, g_stars);
  native_menu::draw_px(dev, g_bars);
}

}  // namespace revive
