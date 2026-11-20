#include "mod/own_camera.h"

#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "common/log.h"
#include "mod/four_players.h"
#include "mod/lockstep.h"
#include "mod/nametags.h"
#include "mod/patch.h"
#include "mod/widescreen.h"

namespace own_camera {
namespace {

// --- the game's camera (TMNT.EXE bc49a82c..., re/notes.md §31) -------------------
// Its state is at the holder of the RenderWare camera (0x61ced8; 0x4a25a5
// returns its address): +0 the RenderWare camera, +0xC the eye, +0x18 the
// target, +0x24 the view window's x for a zoom of 1, +0x28 its x now
// (0x61cf00), +0x48 the distance a turtle is seen at a zoom of 1, +0x4C the
// zoom (0.5 wide .. 2 close).
// The camera's task (0x403ff4; its data, 0xAC bytes: +8 where it is on the
// stage's camera path, +0xC the lift over a turtle, +0x10 the stretch of the
// path, +0x1C its mode, +0x20 / +0x24 the stage's own touch and its argument,
// +0x4C.. how fast it follows, +0x64.. a shake) every frame: for a path in
// stretches the stretch goes on with player 1 (0x4030ec(data)); in mode 0 the
// follow camera (0x403bb0, data in ebx): aims at the players (0x402f8f: one
// player, or the middle of two), moves its eye to the path's point next to
// that (0x4a6aac / 0x4a65ec, or the stage's fixed spots), sets the camera's
// frame (0x4038e3: the stage's touch, the shake, look-at 0x4a25bd), takes the
// zoom that holds the players (0x403735: |turtle - eye| / distance for one,
// less for two apart) and sets the view window (0x4a2b14: x = base / zoom,
// y = 0.75x). The path's code (0x4a6382..0x4a6e00) keeps nothing between calls.
constexpr const char* kStateSig = "B8 ?? ?? ?? ?? C3 A1 ?? ?? ?? ?? C3";
constexpr size_t kEye = 0x0C, kTarget = 0x18, kBaseWindow = 0x24, kWindowNow = 0x28, kDistance = 0x48, kZoom = 0x4C,
                 kStateEnd = 0x50;
// In the task, after its messages: [path type() == 3 -> stretch(data)], then
// by the mode: another camera (push ebx; call; pop ecx; jmp) or the follow
// camera, then the state's address is asked (0x4a25a5) for what comes after.
constexpr const char* kTaskSig =
    "E8 ?? ?? ?? ?? 83 F8 03 75 07 53 E8 ?? ?? ?? ?? 59 8B 43 1C 85 C0 74 13 83 F8 02 7E 05 83 F8 04 7E 0E 53 E8 ?? ?? "
    "?? ?? 59 EB 05 E8 ?? ?? ?? ?? E8";
constexpr size_t kTaskPathType = 0, kTaskStretch = 11, kTaskFollow = 43, kTaskAfter = 48;
constexpr size_t kTaskSize = 0xAC, kTaskPathAt = 0x08, kTaskStretchAt = 0x10, kTaskTouch = 0x20, kTaskShake = 0x64,
                 kTaskShakeEnd = 0x70;

// The camera's task is made anew for every scene (0x404178(a, b, c): a level,
// an area of it, the menus' scenes), and the stage's camera path is loaded
// with the stage (0x4a638b(file), its data at [0x6228cc]). What a player's own
// camera keeps from frame to frame is then of the scene before: its place on
// a path that is gone.
constexpr const char* kTaskMakeSig =
    "55 8B EC 83 EC 2C 8B 45 0C 83 65 D8 00 89 45 F4 8B 45 08 89 45 F8 8B 45 10 89 45 FC";
constexpr const char* kPathLoadSig =
    "56 57 FF 74 24 0C E8 ?? ?? ?? ?? 8B F0 33 C0 66 8B 46 2E 50 33 C0 66 8B 46 24 50 E8";

// The game holds the turtles near its camera. Every frame a turtle's step
// (0x43271f) gives its actor a circle, 0x4a4c6c(actor, centre, radius) (actor
// data +0xE8 the centre, +0xF4 the radius): the camera's target (state +0x18)
// and the stage's radius (0x40494f: [info +0x118], 5 at a level's start, 3..100
// set by the stages, 0x40493f). A move (0x4a36e5, actors with flag bit 3: the
// turtles, set when they are made if the radius is not 0) that ends outside
// the circle and leads away from its centre loses its x and z: "a wall nobody
// sees" between players who part. One player alone never meets it (the target
// is over the turtle), so the stages do not lean on it as a wall.
// Here: the step's call of the radius (+0), ..., its call of 0x4a4c6c (+20).
constexpr const char* kHoldSig = "E8 ?? ?? ?? ?? 8B 45 FC 8B 7D 08 51 83 C0 18 D9 1C 24 50 57 E8 ?? ?? ?? ?? 53 57 E8";
constexpr size_t kHoldCall = 20;
constexpr float kNoHold = 1.0e30f;  // a circle no move leaves

// A character (actor) is drawn by 0x4a5012(scene, object): its data at object
// +0x60; +0 flags: bit 0 hidden, bit 6 "on the screen", cleared and set here
// by RwCameraFrustumTestSphere (0x4bd990) with the camera being drawn and the
// sphere {[data +0xFC] +0x30 (its place), [data +0x100]}; then its draw
// function ([data +0x150]) is called. That function also plays the game: its
// shadow's ground check throws dice for a splash on water (0x4189d3: rand()),
// which makes a new object. The same test is asked by 0x424bc1 (an enemy's
// own "off the screen" flag) and 0x44e62e (off the screen (point, radius)).
// After the sphere's test the draw asks a second thing: is it in front of the
// camera, [data +0x64] - near > radius + [data +0x68]. +0x64 is the actor's
// depth in the game's camera, times window now / window for a zoom of 1
// (0x4a25f7), written every frame by the actor's step (0x4a4fbe..: its place
// through the camera's view matrix, 0x4b8eb0; the same number orders the
// draws, [object +0x10]). That camera looks at the middle of all the players:
// a player far behind the others is behind it, and its turtle was drawn on no
// PC (0.13.7, a real game of three: "the characters vanish when we part";
// lab: 62 apart, the one behind has no turtle in its own picture).
constexpr const char* kActorDrawSig = "55 8B EC 83 EC 10 A1 ?? ?? ?? ?? 53 8B 5D 0C 56 8B 73 60 33 C9 57 8B 38 83 26 BF";
constexpr size_t kActorDrawTest = 0x5C;  // its call of the test
constexpr size_t kObjectData = 0x60;
constexpr size_t kDataDepth = 0x64, kDataMargin = 0x68;
constexpr uint32_t kOnScreen = 0x40;
constexpr float kInFront = 1.0e9f;  // a depth the draw's second test always passes

// --- RenderWare ---------------------------------------------------------------------
// RwCamera: +4 its frame, +0x10 its sync (the view matrix and the frustum from
// the frame and the view window: 0x4bdc30), +0x18 / +0x1C begin / end update
// (0x4bea00 / 0x4be9d0 for every camera), +0x20 the view matrix, +0x60 / +0x64
// frame and depth buffer, +0x68 view window, +0x70 1 / view window, +0x78
// offset, +0x80 near, +0x84 far, +0x94 the frustum, up to +0x184. RwFrame:
// +0x10 its matrix, +0x50 its matrix in the world (the camera's frame has no
// parent: the same), up to +0x90. A matrix: right, up, at, pos, 16 bytes each
// (the fourth: flags, padding). The begin update first syncs the frames
// changed since the last one (0x4cab90), then starts the picture.
constexpr const char* kBeginSig = "A1 ?? ?? ?? ?? 56 8B 74 24 08 89 30 E8 ?? ?? ?? ?? 8B 0D";
constexpr size_t kBeginSyncCall = 12;
constexpr const char* kEndSig = "A1 ?? ?? ?? ?? 56 8B 74 24 08 6A 00 8B 40 70 56 6A 00 FF D0";
constexpr size_t kCamFrame = 0x04, kCamSync = 0x10, kCamView = 0x20, kCamBuffers = 0x60, kCamWindow = 0x68,
                 kCamRecip = 0x70, kCamNear = 0x80, kCamFar = 0x84, kCamSize = 0x184;
constexpr size_t kFrameMatrix = 0x10, kFrameWorld = 0x50, kFrameSize = 0x90;

using SyncDirty_t = void(__cdecl*)();
using CamSync_t = void*(__cdecl*)(void* camera);
using Update_t = void*(__cdecl*)(void* camera);
using Test_t = int(__cdecl*)(void* camera, const float* sphere);
using ActorDraw_t = void(__cdecl*)(void* scene, uint8_t* object);
using PathType_t = int(__cdecl*)();
using Stretch_t = void(__cdecl*)(uint8_t* task);
using TaskMake_t = void*(__cdecl*)(void* a, void* b, void* c);
using PathLoad_t = int(__cdecl*)(void* file);
using Hold_t = void(__cdecl*)(void* actor, const float* centre, float radius);

uint8_t* g_state = nullptr;  // the game's camera state
SyncDirty_t g_sync_dirty = nullptr;
Update_t oBegin = nullptr, oEnd = nullptr;
Test_t oTest = nullptr;
ActorDraw_t oActorDraw = nullptr;
void* g_follow = nullptr;  // the follow camera (its task's data in ebx)
PathType_t g_path_type = nullptr;
Stretch_t g_stretch = nullptr;
uintptr_t g_test_from_draw = 0;     // the return address of the actor draw's test
uintptr_t g_test_from_flags[4] = {};  // ... of the tests that only set an "off the screen" flag
bool g_swaps = false, g_rules = false;
TaskMake_t oTaskMake = nullptr;
PathLoad_t oPathLoad = nullptr;
Hold_t g_hold = nullptr;  // the game's 0x4a4c6c
bool g_free = false;      // the turtles' step asks hold_turtle

bool g_enabled = true;
int g_debug_player = -1;  // [debug] camera_player (lab: without a session too)
bool g_trace = false;
bool g_keep_over_scenes = false;  // [debug] camera_keep (lab: as 0.13.4 was)

void* main_camera() { return g_state ? *reinterpret_cast<void**>(g_state) : nullptr; }
float state_float(size_t at) { return *reinterpret_cast<const float*>(g_state + at); }

float dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float length(const float* a) { return std::sqrt(dot(a, a)); }
bool unit(float* a) {
  float l = length(a);
  if (!(l > 1e-5f) || !std::isfinite(l)) return false;
  a[0] /= l, a[1] /= l, a[2] /= l;
  return true;
}
void cross(const float* a, const float* b, float* out) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}
float mix(float a, float b, float t) { return a + (b - a) * t; }

// --- the views the game may ask about: the same on every PC ---------------------------
// A view: where it looks from and how, and half its window at a distance of 1
// for the game's own 4:3 picture. What a screen really shows of it is wider
// (Hor+, [video] fov), each PC's own; the game is told one answer:
// - "seen": inside the window of a 16:9 picture (what most screens show);
// - "drawn": inside a window wide enough for any screen (a 32:9 picture, a
//   view of 150 %): the draw functions run for everything any PC can show.
struct View {
  bool on = false;
  float eye[3], right[3], up[3], at[3];
  float x = 0, y = 0;
};
constexpr float kSeenX = (16.0f / 9.0f) / (4.0f / 3.0f), kSeenY = 1.0f;
constexpr float kDrawnX = 4.0f, kDrawnY = 2.0f;
constexpr int kPlayers = 4;

View g_shared;             // the game's camera as of its picture (the one kept below)
View g_player[kPlayers];   // each player's own, as of the game's camera's last step

// A session's rule (protocol 7): the views above decide what is on the screen.
bool rule() { return g_rules && (lockstep::state() == lockstep::State::Running || g_debug_player >= 0); }

bool basis(const float* eye, const float* target, float* right, float* up, float* at) {
  for (int i = 0; i < 3; ++i) at[i] = target[i] - eye[i];
  if (!unit(at)) return false;
  const float world_up[3] = {0, 1, 0};
  cross(world_up, at, right);
  if (!unit(right)) return false;
  cross(at, right, up);
  return unit(up);
}

// What a player's own camera keeps from frame to frame: the rest is the
// game's camera's (how it is set for this place of the stage).
struct Own {
  bool have = false;
  float eye[3], target[3], zoom;
  uint32_t path_at, stretch;
};
Own g_own[kPlayers];
bool g_working_out = false;
bool g_followed = false;  // the follow camera ran in this step of the camera's task

void call_follow(uint8_t* task) {
  void* follow = g_follow;
  __asm {
    push ebx
    mov ebx, task
    call follow
    pop ebx
  }
}

// Right after the game's follow camera: the same for every player alone.
void __cdecl after_follow(uint8_t* task) {
  g_followed = true;
  uint8_t* cam = static_cast<uint8_t*>(main_camera());
  uint8_t* frame = cam ? *reinterpret_cast<uint8_t**>(cam + kCamFrame) : nullptr;
  if (!rule() || !task || !frame) {
    for (int p = 0; p < kPlayers; ++p) g_player[p].on = false, g_own[p].have = false;
    return;
  }
  uint8_t kept_state[kStateEnd - kEye], kept_task[kTaskSize], kept_matrix[0x40];
  std::memcpy(kept_state, g_state + kEye, sizeof(kept_state));
  std::memcpy(kept_task, task, sizeof(kept_task));
  std::memcpy(kept_matrix, frame + kFrameMatrix, sizeof(kept_matrix));
  g_working_out = true;
  for (int p = 0; p < kPlayers; ++p) {
    View& v = g_player[p];
    Own& o = g_own[p];
    if (!four::position(p)) {
      v.on = false;
      o.have = false;
      continue;
    }
    // The game's camera as it is set now, with this player's own place in it.
    std::memcpy(g_state + kEye, kept_state, sizeof(kept_state));
    std::memcpy(task, kept_task, sizeof(kept_task));
    if (o.have) {
      std::memcpy(g_state + kEye, o.eye, 12);
      std::memcpy(g_state + kTarget, o.target, 12);
      std::memcpy(g_state + kZoom, &o.zoom, 4);
      std::memcpy(task + kTaskPathAt, &o.path_at, 4);
      std::memcpy(task + kTaskStretchAt, &o.stretch, 4);
    }
    std::memset(task + kTaskTouch, 0, 4);  // (the stage's own touch is for the game's camera)
    std::memset(task + kTaskShake, 0, kTaskShakeEnd - kTaskShake);
    four::camera_solo(p);
    if (g_path_type() == 3) g_stretch(task);
    call_follow(task);
    four::camera_solo(-1);
    std::memcpy(o.eye, g_state + kEye, 12);
    std::memcpy(o.target, g_state + kTarget, 12);
    std::memcpy(&o.zoom, g_state + kZoom, 4);
    std::memcpy(&o.path_at, task + kTaskPathAt, 4);
    std::memcpy(&o.stretch, task + kTaskStretchAt, 4);
    o.have = true;
    std::memcpy(v.eye, o.eye, 12);
    v.x = state_float(kWindowNow);
    v.y = v.x * 0.75f;
    v.on = v.x > 0.001f && std::isfinite(v.x) && basis(o.eye, o.target, v.right, v.up, v.at);
  }
  g_working_out = false;
  std::memcpy(g_state + kEye, kept_state, sizeof(kept_state));
  std::memcpy(task, kept_task, sizeof(kept_task));
  std::memcpy(frame + kFrameMatrix, kept_matrix, sizeof(kept_matrix));
}

// In place of the task's call of the follow camera (its data in ebx).
__declspec(naked) void follow_and_more() {
  __asm {
    call g_follow
    push ebx
    call after_follow
    add esp, 4
    ret
  }
}

// In place of the task's next call (the state's address): the task's step is
// over; without the follow camera in it (a scene's camera) nobody has a view
// of their own.
uint8_t* __cdecl task_step_done() {
  if (!g_followed)
    for (int p = 0; p < kPlayers; ++p) g_player[p].on = false, g_own[p].have = false;
  g_followed = false;
  return g_state;
}

// In place of the turtle's step's call: in a game together every player has
// a camera of their own, and the game's camera (the middle of all) holds
// nobody. The same on every PC (protocol 10).
void __cdecl hold_turtle(void* actor, const float* centre, float radius) {
  g_hold(actor, centre, rule() ? kNoHold : radius);
}

bool in_view(const View& v, const float* sphere, float kx, float ky, float near_plane, float far_plane) {
  const float d[3] = {sphere[0] - v.eye[0], sphere[1] - v.eye[1], sphere[2] - v.eye[2]};
  const float cx = std::fabs(dot(d, v.right)), cy = std::fabs(dot(d, v.up)), cz = dot(d, v.at), r = sphere[3];
  if (cz + r < near_plane || cz - r > far_plane) return false;
  const float X = v.x * kx, Y = v.y * ky;
  if (cx - X * cz > r * std::sqrt(1.0f + X * X)) return false;
  if (cy - Y * cz > r * std::sqrt(1.0f + Y * Y)) return false;
  return true;
}

// The actor draw's second test for a view: the sphere is in front of it by
// more than its radius and the actor's own margin (the game's rule, with the
// view's own depth and window).
bool in_front(const View& v, const float* sphere, float near_plane, float margin) {
  const float d[3] = {sphere[0] - v.eye[0], sphere[1] - v.eye[1], sphere[2] - v.eye[2]};
  const float base = state_float(kBaseWindow);
  const float depth = dot(d, v.at) * (base > 0.001f ? v.x / base : 1.0f);
  return depth - near_plane > sphere[3] + margin;
}

// By any of the views: is the sphere seen, is it to be drawn. `margin`: the
// actor being drawn asks (its second test goes by the views too); null: one of
// the game's other tests.
void ask(const float* sphere, float near_plane, float far_plane, const float* margin, bool& seen, bool& drawn) {
  seen = drawn = false;
  auto one = [&](const View& v) {
    if (!v.on) return;
    if (margin && !in_front(v, sphere, near_plane, *margin)) return;
    drawn = drawn || in_view(v, sphere, kDrawnX, kDrawnY, near_plane, far_plane);
    seen = seen || in_view(v, sphere, kSeenX, kSeenY, near_plane, far_plane);
  };
  one(g_shared);
  for (const View& v : g_player) one(v);
}

bool g_draw_tested = false, g_draw_seen = true, g_draw_drawn = true;
bool g_draw_by_views = false;  // the actor being drawn: its second test is the views' (hkActorDraw)
float g_draw_margin = 0;
// The camera a picture is being drawn with ([[0x69fdfc]], as the actor draw reads it).
void*** g_current_camera = nullptr;
void* current_camera() { return g_current_camera && *g_current_camera ? **g_current_camera : nullptr; }

// Lab ([debug] camera_trace=1): what became of each player's turtle in the
// actor draw since the last line: calls, not tested (hidden), drawn, seen.
struct DrawTrace {
  uint32_t calls = 0, untested = 0, drawn = 0, seen = 0, flags = 0;
};
DrawTrace g_draw_trace[4];

int __cdecl hkTest(void* camera, const float* sphere) {
  const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress());
  bool flag_only = false;
  for (uintptr_t a : g_test_from_flags) flag_only = flag_only || (a && a == from);
  const bool draw = from == g_test_from_draw;
  if ((!draw && !flag_only) || !camera || !sphere || camera != main_camera() || !g_shared.on || !rule())
    return oTest(camera, sphere);
  const auto* cam = static_cast<const uint8_t*>(camera);
  bool seen, drawn;
  ask(sphere, *reinterpret_cast<const float*>(cam + kCamNear), *reinterpret_cast<const float*>(cam + kCamFar),
      draw && g_draw_by_views ? &g_draw_margin : nullptr, seen, drawn);
  if (!draw) return seen ? 1 : 0;
  g_draw_tested = true;
  g_draw_seen = seen;
  g_draw_drawn = drawn;
  return drawn ? 1 : 0;
}

void __cdecl hkActorDraw(void* scene, uint8_t* object) {
  g_draw_tested = false;
  // In a game together "in front of the camera" is asked of every view with
  // the sphere (hkTest), the same on every PC: the game's own depth, of the
  // camera in the middle of all, stands aside for the draw.
  float* depth = nullptr;
  float kept = 0;
  if (object && rule() && g_shared.on && current_camera() == main_camera()) {
    if (auto* data = *reinterpret_cast<uint8_t**>(object + kObjectData)) {
      depth = reinterpret_cast<float*>(data + kDataDepth);
      kept = *depth;
      g_draw_margin = *reinterpret_cast<const float*>(data + kDataMargin);
      *depth = kInFront;
      g_draw_by_views = true;
    }
  }
  oActorDraw(scene, object);
  g_draw_by_views = false;
  if (depth) *depth = kept;
  if (g_trace && object)
    for (int p = 0; p < 4; ++p)
      if (object == four::turtle(p)) {
        DrawTrace& t = g_draw_trace[p];
        ++t.calls;
        if (!g_draw_tested) ++t.untested;
        if (g_draw_tested && g_draw_drawn) ++t.drawn;
        if (g_draw_tested && g_draw_seen) ++t.seen;
        if (auto* data = *reinterpret_cast<uint32_t**>(object + kObjectData)) t.flags = *data;
      }
  // Drawn for a screen wider than most: "on the screen" only if seen.
  if (g_draw_tested && !g_draw_seen && object) {
    if (auto* data = *reinterpret_cast<uint32_t**>(object + kObjectData)) *data &= ~kOnScreen;
  }
}

// --- the picture ---------------------------------------------------------------------
// This PC's own view of it (never read by the game).
bool g_advanced = false;  // this frame's step is done
float g_weight = 0;       // 0 the game's camera .. 1 this PC's player's
constexpr float kIn = 1.0f / 24.0f, kOut = 1.0f / 14.0f;

// The picture being drawn: what the game's camera had.
bool g_swapped = false;
uint8_t g_kept_camera[kCamSize];
uint8_t g_kept_frame[kFrameSize];
// ... and what it was drawn with (for the tags over it).
bool g_picture = false;
float g_picture_matrix[16];
float g_picture_recip[2];

int player() {
  if (!g_enabled) return -1;
  if (g_debug_player >= 0) return g_debug_player;
  if (lockstep::state() != lockstep::State::Running) return -1;
  return lockstep::my_player();
}

void trace(const uint8_t* cam, const float* m, int me) {
  static int n = 0;
  if (!g_trace || ++n % 30) return;
  char players[200] = "";
  size_t at = 0;
  for (int p = 0; p < kPlayers; ++p)
    if (const float* v = four::position(p))
      at += std::snprintf(players + at, sizeof(players) - at, " p%d(%.2f %.2f %.2f)%s", p + 1, v[0], v[1], v[2],
                          g_player[p].on ? "*" : "");
  const View* own = me >= 0 && me < kPlayers && g_player[me].on ? &g_player[me] : nullptr;
  for (int p = 0; p < 4; ++p) {
    DrawTrace& t = g_draw_trace[p];
    if (four::turtle(p))
      LOGI("camera: player %d's turtle in the actor draw: %u calls, %u not tested, %u drawn, %u seen, flags %08X", p + 1,
           t.calls, t.untested, t.drawn, t.seen, t.flags);
    t = DrawTrace{};
  }
  LOGI("camera: eye(%.2f %.2f %.2f) zoom %.3f window %.3f (base %.3f, distance %.2f)%s; own: weight %.2f eye(%.2f %.2f "
       "%.2f) window %.3f; the camera's window %.3f",
       m[12], m[13], m[14], state_float(kZoom), state_float(kWindowNow), state_float(kBaseWindow),
       state_float(kDistance), players, g_weight, own ? own->eye[0] : 0.0f, own ? own->eye[1] : 0.0f,
       own ? own->eye[2] : 0.0f, own ? own->x : 0.0f, *reinterpret_cast<const float*>(cam + kCamWindow));
}

void before_picture(uint8_t* cam) {
  g_picture = false;
  if (g_swapped) return;  // (a begin without an end: left as it is)
  uint8_t* frame = *reinterpret_cast<uint8_t**>(cam + kCamFrame);
  uintptr_t sync = *reinterpret_cast<uintptr_t*>(cam + kCamSync);
  if (!frame || !patch::in_image(sync, 1)) return;

  // The game's camera as its begin update would have it: kept.
  g_sync_dirty();
  std::memcpy(g_kept_camera, cam, kCamSize);
  std::memcpy(g_kept_frame, frame, kFrameSize);
  const float* m = reinterpret_cast<const float*>(g_kept_frame + kFrameWorld);  // right, up, at, pos

  // ... as a view the game may ask about: its window is the game's own
  // number (0x61cf00), not what this PC's camera holds.
  std::memcpy(g_shared.right, m + 0, 12);
  std::memcpy(g_shared.up, m + 4, 12);
  std::memcpy(g_shared.at, m + 8, 12);
  std::memcpy(g_shared.eye, m + 12, 12);
  g_shared.x = state_float(kWindowNow);
  g_shared.y = g_shared.x * 0.75f;
  g_shared.on = g_shared.x > 0.001f && std::isfinite(g_shared.x);

  // This PC's picture: its player's view, coming in and going softly.
  const int me = player();
  const View* own = me >= 0 && me < kPlayers && g_player[me].on ? &g_player[me] : nullptr;
  if (!g_advanced) {
    g_advanced = true;
    g_weight = std::clamp(g_weight + (own ? kIn : -kOut), 0.0f, 1.0f);
  }
  trace(cam, m, me);
  const float w = own ? g_weight * g_weight * (3 - 2 * g_weight) : 0.0f;  // (eased)

  // The window the game asked for this camera (x, y), or this player's.
  float asked[2];
  if (!widescreen::asked_window(cam, asked)) std::memcpy(asked, g_kept_camera + kCamWindow, sizeof(asked));
  if (own && w > 0) {
    asked[0] = mix(asked[0], own->x, w);
    asked[1] = mix(asked[1], own->y, w);
  }
  float window[2];
  if (!widescreen::shown_window(cam, asked, window) || !(window[0] > 0.001f) || !(window[1] > 0.001f)) return;

  uint8_t local[0x40];
  std::memcpy(local, g_kept_frame + kFrameWorld, sizeof(local));  // (its flags and padding)
  if (own && w > 0) {
    float eye[3], at[3], right[3], up[3];
    for (int i = 0; i < 3; ++i) {
      eye[i] = mix(m[12 + i], own->eye[i], w);
      at[i] = mix(m[8 + i], own->at[i], w);
    }
    const float world_up[3] = {0, 1, 0};
    bool ok = unit(at);
    if (ok) {
      cross(world_up, at, right);
      ok = unit(right);
    }
    if (ok) {
      if (dot(right, m + 0) < 0) right[0] = -right[0], right[1] = -right[1], right[2] = -right[2];
      cross(at, right, up);
      ok = unit(up);
    }
    if (ok) {
      if (dot(up, m + 4) < 0) up[0] = -up[0], up[1] = -up[1], up[2] = -up[2];
      auto* l = reinterpret_cast<float*>(local);
      std::memcpy(l + 0, right, 12);
      std::memcpy(l + 4, up, 12);
      std::memcpy(l + 8, at, 12);
      std::memcpy(l + 12, eye, 12);
    }
  }
  std::memcpy(frame + kFrameMatrix, local, sizeof(local));
  std::memcpy(frame + kFrameWorld, local, sizeof(local));
  auto* vw = reinterpret_cast<float*>(cam + kCamWindow);
  vw[0] = window[0], vw[1] = window[1];
  vw[2] = 1.0f / window[0], vw[3] = 1.0f / window[1];
  reinterpret_cast<CamSync_t>(sync)(cam);  // (the view matrix and the frustum from them)
  g_swapped = true;

  g_picture = true;
  std::memcpy(g_picture_matrix, local, sizeof(g_picture_matrix));
  std::memcpy(g_picture_recip, cam + kCamRecip, sizeof(g_picture_recip));
}

// A new scene's camera, a new stage's path: every player's own view starts
// anew from the game's camera (0.13.4, a real game: the next level began with
// the views kept from the level before, "the camera is behind the houses").
void start_anew(const char* why) {
  if (g_keep_over_scenes) return;
  bool had = false;
  for (int p = 0; p < kPlayers; ++p) {
    had = had || g_own[p].have;
    g_player[p].on = false, g_own[p].have = false;
  }
  g_weight = 0;
  if (had) LOGI("own camera: %s; every player's view starts anew", why);
}

void* __cdecl hkTaskMake(void* a, void* b, void* c) {
  start_anew("a new scene's camera");
  return oTaskMake(a, b, c);
}

int __cdecl hkPathLoad(void* file) {
  start_anew("a new stage's camera path");
  return oPathLoad(file);
}

void* __cdecl hkBegin(void* camera) {
  if (camera && camera == main_camera()) {
    nametags::on_world_picture();
    before_picture(static_cast<uint8_t*>(camera));
  }
  return oBegin(camera);
}

void* __cdecl hkEnd(void* camera) {
  void* r = oEnd(camera);
  if (g_swapped && camera && camera == main_camera()) {
    // The game's camera again, exactly as its own begin update left it.
    auto* cam = static_cast<uint8_t*>(camera);
    uint8_t* frame = *reinterpret_cast<uint8_t**>(cam + kCamFrame);
    std::memcpy(cam + kCamView, g_kept_camera + kCamView, kCamBuffers - kCamView);
    std::memcpy(cam + kCamWindow, g_kept_camera + kCamWindow, kCamSize - kCamWindow);
    if (frame) std::memcpy(frame + kFrameMatrix, g_kept_frame + kFrameMatrix, kFrameSize - kFrameMatrix);
    g_swapped = false;
    g_shared.on = false;
  }
  return r;
}

}  // namespace

void install(const Config& cfg) {
  g_enabled = cfg.own_camera;
  g_debug_player = cfg.debug_camera_player - 1;
  g_trace = cfg.debug_camera_trace;
  g_keep_over_scenes = cfg.debug_camera_keep;
  uintptr_t state = patch::find("camera_state", kStateSig), begin = patch::find("camera_begin_update", kBeginSig);
  uintptr_t end = patch::find("camera_end_update", kEndSig), task = patch::find("camera_task", kTaskSig);
  uintptr_t draw = patch::find("actor_draw", kActorDrawSig);
  if (!state || !begin || !end || patch::read<uint8_t>(begin + kBeginSyncCall) != 0xE8) {
    LOGW("own camera: the game's camera is not where expected; one camera for all, as before");
    return;
  }
  uintptr_t sync = begin + kBeginSyncCall + 5 + patch::read<int32_t>(begin + kBeginSyncCall + 1);
  auto* holder = reinterpret_cast<uint8_t*>(patch::read<uint32_t>(state + 1));
  if (!patch::in_image(sync, 1) || !patch::in_image((uintptr_t)holder, 0x50)) {
    LOGW("own camera: the game's camera is not as expected; one camera for all, as before");
    return;
  }
  g_sync_dirty = reinterpret_cast<SyncDirty_t>(sync);
  g_state = holder;
  if (!patch::hook("camera_end_update", end, (void*)&hkEnd, (void**)&oEnd) ||
      !patch::hook("camera_begin_update", begin, (void*)&hkBegin, (void**)&oBegin)) {
    LOGW("own camera: no hooks on the camera's pictures; one camera for all, as before");
    g_state = nullptr;
    return;
  }
  g_swaps = true;

  // What is on the screen, the same on every PC; every player's own view.
  auto target_of = [](uintptr_t call) { return call + 5 + patch::read<int32_t>(call + 1); };
  uintptr_t test = 0;
  if (task && draw && patch::read<uint8_t>(draw + kActorDrawTest) == 0xE8) test = target_of(draw + kActorDrawTest);
  if (test && patch::in_image(test, 1)) {
    g_test_from_draw = draw + kActorDrawTest + 5;
    if (const uintptr_t current = patch::read<uint32_t>(draw + 7); patch::in_image(current, 4))
      g_current_camera = reinterpret_cast<void***>(current);
    // The game's other tests that end in "off the screen = !result" (neg eax; sbb eax, eax).
    size_t n = 0;
    for (uintptr_t call : patch::find_calls(test)) {
      if (call >= test || call + 5 == g_test_from_draw) continue;  // (RenderWare's own are after it)
      bool flag = false;
      for (size_t i = 5; i < 12 && !flag; ++i)
        flag = patch::read<uint8_t>(call + i) == 0xF7 && patch::read<uint8_t>(call + i + 1) == 0xD8 &&
               patch::read<uint8_t>(call + i + 2) == 0x1B && patch::read<uint8_t>(call + i + 3) == 0xC0;
      if (flag && n < 4) g_test_from_flags[n++] = call + 5;
    }
    g_path_type = reinterpret_cast<PathType_t>(target_of(task + kTaskPathType));
    g_stretch = reinterpret_cast<Stretch_t>(target_of(task + kTaskStretch));
    g_follow = reinterpret_cast<void*>(target_of(task + kTaskFollow));
    const bool known = patch::in_image((uintptr_t)g_path_type, 1) && patch::in_image((uintptr_t)g_stretch, 1) &&
                       patch::in_image((uintptr_t)g_follow, 1) && target_of(task + kTaskAfter) == state;
    if (known && patch::hook("actor_draw", draw, (void*)&hkActorDraw, (void**)&oActorDraw) &&
        patch::hook("frustum_test_sphere", test, (void*)&hkTest, (void**)&oTest)) {
      const int32_t follow = (int32_t)(reinterpret_cast<uintptr_t>(&follow_and_more) - (task + kTaskFollow + 5));
      const int32_t after = (int32_t)(reinterpret_cast<uintptr_t>(&task_step_done) - (task + kTaskAfter + 5));
      g_rules = patch::write(task + kTaskFollow + 1, &follow, 4) && patch::write(task + kTaskAfter + 1, &after, 4);
    }
    LOGI("own camera: every player's own view from the game's follow camera at %p; what is on the screen is asked of "
         "them (%u more tests)%s",
         g_follow, (unsigned)n, g_rules ? "" : ": not in place");
  }
  if (g_rules) {
    // A new scene: the views start anew. (Both are tried: either one is enough.)
    const uintptr_t make = patch::find("camera_task_make", kTaskMakeSig);
    const uintptr_t path = patch::find("camera_path_load", kPathLoadSig);
    const bool by_task = make && patch::hook("camera_task_make", make, (void*)&hkTaskMake, (void**)&oTaskMake);
    const bool by_path = path && patch::hook("camera_path_load", path, (void*)&hkPathLoad, (void**)&oPathLoad);
    if (!by_task && !by_path) LOGW("own camera: a new scene's camera is not where expected; the views are kept over it");
    // Nobody is held near the game's camera.
    const uintptr_t hold = patch::find("turtle_hold", kHoldSig);
    if (hold && patch::read<uint8_t>(hold + kHoldCall) == 0xE8) {
      const uintptr_t held_by = target_of(hold + kHoldCall);
      if (patch::in_image(held_by, 1)) {
        g_hold = reinterpret_cast<Hold_t>(held_by);
        const int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(&hold_turtle) - (hold + kHoldCall + 5));
        g_free = patch::write(hold + kHoldCall + 1, &rel, 4);
      }
    }
    LOGI("own camera: in a game together no turtle is held near the game's camera%s",
         g_free ? "" : ": not in place (the game's wall between players who part stays)");
  }
  if (!g_rules) LOGW("own camera: the game's screen tests are not where expected; one camera for all in a game together");
  LOGI("own camera: %s (in a game together every PC draws the world around its own turtle)",
       g_enabled && g_rules ? "on" : "off");
}

void set_enabled(bool on) { g_enabled = on; }
bool enabled() { return g_enabled; }
bool swaps() { return g_swaps; }
bool same_view_for_all() { return g_swaps && g_rules && lockstep::state() == lockstep::State::Running; }

void on_aim(const float* shared, float lift, const void* caller) {
  if (!g_state || !shared || !g_trace) return;
  static const void* seen[16];
  static int n = 0;
  for (int i = 0; i < n; ++i)
    if (seen[i] == caller) return;
  if (n >= 16) return;
  seen[n++] = caller;
  LOGI("camera: the players' middle asked from %p (lift %.2f)", caller, lift);
}

bool working_out() { return g_working_out; }

float zoom_for(const float* pos) {
  if (!g_state || !pos) return 1.0f;
  const float* eye = reinterpret_cast<const float*>(g_state + kEye);
  const float away[3] = {pos[0] - eye[0], pos[1] - eye[1], pos[2] - eye[2]};
  const float distance = state_float(kDistance);
  return distance > 0.01f ? length(away) / distance : 1.0f;
}

void frame_done() { g_advanced = false; }

const float* picture_matrix() { return g_picture ? g_picture_matrix : nullptr; }
const float* picture_recip_window() { return g_picture ? g_picture_recip : nullptr; }

}  // namespace own_camera
