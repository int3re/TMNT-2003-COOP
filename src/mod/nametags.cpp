#include "mod/nametags.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/log.h"
#include "common/net.h"
#include "mod/four_players.h"
#include "mod/map_turtles.h"
#include "mod/native_menu.h"
#include "mod/net_pause.h"
#include "mod/own_camera.h"
#include "mod/net_host.h"
#include "mod/patch.h"

namespace nametags {
namespace {

// The camera the game draws its world with (a RenderWare camera): [holder]
// (0x4a25ab returns it; 0x4a25a5 right before it returns &holder).
constexpr const char* kCameraSig = "B8 ?? ?? ?? ?? C3 A1 ?? ?? ?? ?? C3";
// RwCamera: +4 its frame, +0x18 beginUpdate (RwCameraBeginUpdate calls it),
// +0x70 1 / view window (x, y), +0x78 view offset, +0x80 near plane. RwFrame:
// +0x50 its matrix in the world (right, up, at, pos: 4 x 4 floats). The view
// window is half the picture at a distance of 1, and RenderWare's "right"
// points to the left of the screen.
constexpr size_t kCamFrame = 0x04, kCamBegin = 0x18, kCamRecipWindow = 0x70, kCamOffset = 0x78, kCamNear = 0x80;
constexpr size_t kFrameMatrix = 0x50;

// The player records ([info], see map_turtles.cpp): the turtle (actor) of
// player p at +0x44 + p*0x44, 0 while there is none; its body at +0x60, the
// body's position (feet; y up) at +0x7C.
constexpr const char* kCharacterSig = "8B 44 24 04 8B 0D ?? ?? ?? ?? 8D 44 80 19 8B 04 C1 C3";
constexpr size_t kInfoPtr = 6, kActorAt = 0x44, kActorStep = 0x44, kBody = 0x60, kPos = 0x7C;

constexpr size_t kMaxLetters = 20;

using Begin_t = void*(__cdecl*)(void* camera);
Begin_t oBegin = nullptr;
uint8_t** g_camera = nullptr;
uint8_t** g_info = nullptr;
bool g_hook_tried = false;
bool g_world_drawn = false;  // the game's camera drew a picture since the last Present
bool g_world_shown = false;  // ... as of this Present (prepare)

Config g_cfg;
bool g_on = true;
float g_height = 1.55f, g_map_height = 1.35f;  // the tag's bottom over the feet, world units
constexpr int kPlayers = 4;
std::string g_names[kPlayers];
uint32_t g_names_ms = 0;
// Since when a player's turtle is on the screen: its tag fades in after a
// moment, so camera cuts (the level's fly-in, a scene) do not flash it.
uint32_t g_since_ms[kPlayers] = {};
constexpr uint32_t kShowAfterMs = 400, kFadeMs = 300;
std::vector<native_menu::Tag> g_tags;

void* __cdecl hkBegin(void* camera) {
  if (g_camera && camera == *g_camera) g_world_drawn = true;
  return oBegin(camera);
}

void hook_begin(uint8_t* camera) {
  g_hook_tried = true;
  if (own_camera::swaps()) return;  // (it has the hook and tells: on_world_picture)
  uintptr_t f = *reinterpret_cast<uintptr_t*>(camera + kCamBegin);
  if (!patch::in_image(f, 1) || !patch::hook("camera_begin_update", f, (void*)&hkBegin, (void**)&oBegin))
    LOGW("name tags: no hook on the camera's begin update; no tags");
}

std::string shorten(const std::string& utf8) {
  size_t i = 0, letters = 0;
  while (i < utf8.size() && letters < kMaxLetters) {
    unsigned char c = (unsigned char)utf8[i];
    i += c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    ++letters;
  }
  return utf8.substr(0, std::min(i, utf8.size()));
}

// The names as the network page has them; the host's game is player 1.
void refresh_names() {
  uint32_t now = net::now_ms();
  if (g_names_ms && now - g_names_ms < 500) return;
  g_names_ms = now | 1;
  std::string n[kPlayers];
  if (!g_cfg.debug_nametags.empty()) {
    for (size_t i = 0; i < g_cfg.debug_nametags.size() && i < kPlayers; ++i) n[i] = g_cfg.debug_nametags[i];
  } else {
    HostStatus st = host_status();
    if (st.p2) {
      n[0] = st.name, n[1] = st.p2_name;
      for (const HostStatus::Guest& g : st.guests)
        if (g.player >= 2 && g.player <= kPlayers) n[g.player - 1] = g.name;
    } else if (st.away == 2 && st.away_up) {
      if (!st.away_names.empty()) {
        for (size_t i = 0; i < st.away_names.size() && i < (size_t)kPlayers; ++i) n[i] = st.away_names[i];
      } else {
        n[0] = st.away_host, n[1] = st.name;
      }
    }
  }
  for (int p = 0; p < kPlayers; ++p) {
    std::string s = shorten(n[p]);
    if (s != g_names[p]) {
      if (!s.empty()) LOGI("name tags: player %d is '%s'", p + 1, s.c_str());
      g_names[p] = s;
    }
  }
}

// World -> back buffer pixels with the camera's own numbers (as RenderWare's
// Direct3D 9 driver builds its view and projection).
bool project(const uint8_t* cam, const float p[3], float W, float H, float& sx, float& sy) {
  const uint8_t* frame = *reinterpret_cast<uint8_t* const*>(cam + kCamFrame);
  if (!frame) return false;
  // (The picture may have been drawn with this PC's own camera: own_camera.cpp.)
  const float* own = own_camera::picture_matrix();
  const float* m = own ? own : reinterpret_cast<const float*>(frame + kFrameMatrix);
  const float d[3] = {p[0] - m[12], p[1] - m[13], p[2] - m[14]};
  float cx = d[0] * m[0] + d[1] * m[1] + d[2] * m[2];
  float cy = d[0] * m[4] + d[1] * m[5] + d[2] * m[6];
  float cz = d[0] * m[8] + d[1] * m[9] + d[2] * m[10];
  float near_plane = *reinterpret_cast<const float*>(cam + kCamNear);
  if (!(cz > std::max(near_plane, 0.01f))) return false;  // behind the camera
  const float* rw = own ? own_camera::picture_recip_window() : reinterpret_cast<const float*>(cam + kCamRecipWindow);
  const float* off = reinterpret_cast<const float*>(cam + kCamOffset);
  float nx = (-cx / cz + off[0]) * rw[0], ny = (cy / cz + off[1]) * rw[1];
  sx = (nx + 1.0f) * 0.5f * W;
  sy = (1.0f - ny) * 0.5f * H;
  return std::isfinite(sx) && std::isfinite(sy);
}

bool feet(int player, float out[3]) {
  uint8_t* info = g_info ? *g_info : nullptr;
  uint8_t* actor = player >= 2 ? static_cast<uint8_t*>(four::turtle(player))
                   : info      ? *reinterpret_cast<uint8_t**>(info + kActorAt + player * kActorStep)
                               : nullptr;
  uint8_t* body = actor ? *reinterpret_cast<uint8_t**>(actor + kBody) : nullptr;
  if (!body) return false;
  std::memcpy(out, body + kPos, 3 * sizeof(float));
  return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

}  // namespace

void install(const Config& cfg) {
  g_cfg = cfg;
  g_on = cfg.nametags;
  if (cfg.debug_tag_height[0] > 0) g_height = cfg.debug_tag_height[0];
  if (cfg.debug_tag_height[1] > 0) g_map_height = cfg.debug_tag_height[1];
  uintptr_t cam = patch::find("world_camera", kCameraSig);
  uintptr_t ch = patch::find("player_records", kCharacterSig);
  if (!cam || !ch) return;
  g_camera = reinterpret_cast<uint8_t**>(patch::read<uint32_t>(cam + 1));
  g_info = reinterpret_cast<uint8_t**>(patch::read<uint32_t>(ch + kInfoPtr));
  if (!patch::in_image((uintptr_t)g_camera, 4) || !patch::in_image((uintptr_t)g_info, 4)) {
    g_camera = nullptr, g_info = nullptr;
    LOGW("name tags: the camera or the player records are not where expected; no tags");
    return;
  }
  LOGI("name tags: %s (over the feet: %.2f in play, %.2f on the level map)", g_on ? "on" : "off", g_height,
       g_map_height);
}

void set_enabled(bool on) { g_on = on; }

void on_world_picture() { g_world_drawn = true; }
bool world_drawn_now() { return g_world_drawn; }

unsigned mask_rgb(int character) {
  static const unsigned kMask[5] = {0xFFFFFF, 0xFF5A50, 0xFFA640, 0xB98CFF, 0x5AA0FF};
  return character >= 1 && character <= 4 ? kMask[character] : kMask[0];
}

std::string name(int player) {
  if (!g_on || player < 0 || player >= kPlayers) return std::string();
  refresh_names();
  return g_names[player];
}

bool prepare(IDirect3DDevice9* dev) {
  g_tags.clear();
  bool drawn = g_world_drawn;
  g_world_drawn = false;
  uint8_t* cam = g_camera ? *g_camera : nullptr;
  if (cam && !g_hook_tried) hook_begin(cam);  // (revive.cpp's stars need it too)
  g_world_shown = drawn && cam;
  if (!g_on || !cam || !dev) return false;
  refresh_names();
  bool have[kPlayers] = {};
  float pos[kPlayers][3] = {};
  bool map = false, named = false;
  for (const std::string& n : g_names) named |= !n.empty();
  if (drawn && named && !net_pause::up()) {  // (not over the pause's menu)
    for (int p = 0; p < kPlayers; ++p) map |= have[p] = map_turtles::position(p, pos[p]);
    if (!map)
      for (int p = 0; p < kPlayers; ++p) have[p] = feet(p, pos[p]);
  }
  IDirect3DSurface9* bb = nullptr;
  D3DSURFACE_DESC d{};
  bool any = false;
  for (bool h : have) any |= h;
  if (any && SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
    bb->GetDesc(&d);
    bb->Release();
  }
  const float W = (float)d.Width, H = (float)d.Height;
  const float size = std::clamp(H * 0.026f, 12.0f, 40.0f);
  const float lift = map ? g_map_height : g_height;
  uint32_t now = net::now_ms();
  for (int p = 0; p < kPlayers; ++p) {
    float x = 0, y = 0;
    const float head[3] = {pos[p][0], pos[p][1] + lift, pos[p][2]};
    if (!have[p] || g_names[p].empty() || W <= 0 || !project(cam, head, W, H, x, y)) {
      g_since_ms[p] = 0;
      continue;
    }
    if (!g_since_ms[p]) g_since_ms[p] = now | 1;
    if (x < -W * 0.05f || x > W * 1.05f || y < 0 || y > H + size * 4) {  // that turtle is off the screen
      g_since_ms[p] = 0;
      continue;
    }
    native_menu::Tag t;
    t.text = g_names[p];
    t.rgb = mask_rgb(four::character(p));
    t.size = size;
    t.x = std::clamp(x, W * 0.06f, W * 0.94f);
    t.y = std::clamp(y, size * 1.7f, H - 2.0f);  // (the plate is 1.6 x size high)
    uint32_t up = now - g_since_ms[p];
    if (up < kShowAfterMs) continue;
    t.alpha = (int)std::min<uint32_t>(255, (up - kShowAfterMs) * 255 / kFadeMs + 1);
    g_tags.push_back(t);
  }
  if (!g_cfg.debug_nametags.empty() && any) {  // lab: where the tags went
    static uint32_t logged = 0;
    if (now - logged >= 2000) {
      logged = now;
      const uint8_t* fr = *reinterpret_cast<uint8_t* const*>(cam + kCamFrame);
      const float* m = fr ? reinterpret_cast<const float*>(fr + kFrameMatrix) : nullptr;
      char line[640];
      int n = snprintf(line, sizeof(line), "name tags (%s, %.0fx%.0f): camera at (%.2f %.2f %.2f) looking (%.2f %.2f %.2f)",
                       map ? "map" : "play", W, H, m ? m[12] : 0, m ? m[13] : 0, m ? m[14] : 0, m ? m[8] : 0,
                       m ? m[9] : 0, m ? m[10] : 0);
      for (int p = 0; p < kPlayers && n > 0 && n < (int)sizeof(line) - 80; ++p) {
        float x = -1, y = -1;
        const float head[3] = {pos[p][0], pos[p][1] + lift, pos[p][2]};
        bool ok = have[p] && project(cam, head, W, H, x, y);
        n += snprintf(line + n, sizeof(line) - n, "; p%d %s (%.2f %.2f %.2f) -> %s(%.0f, %.0f)", p + 1,
                      have[p] ? "at" : "none", pos[p][0], pos[p][1], pos[p][2], ok ? "" : "off ", x, y);
      }
      LOGI("%s", line);
    }
  }
  return !g_tags.empty();
}

void draw(IDirect3DDevice9* dev) { native_menu::draw_tags(dev, g_tags); }

bool world_shown() { return g_world_shown; }

bool to_screen(const float world[3], float W, float H, float& x, float& y) {
  uint8_t* cam = g_camera ? *g_camera : nullptr;
  return cam && W > 0 && H > 0 && project(cam, world, W, H, x, y);
}

}  // namespace nametags
