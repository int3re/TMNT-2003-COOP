#include "mod/voice_sync.h"

#include <windows.h>

#include <cstring>

#include "common/log.h"
#include "mod/lockstep.h"
#include "mod/patch.h"

namespace voice_sync {
namespace {

// CRI ADX (ADXT_*): each function checks its handle first and reports a null
// one with its own message ("E0208.... ADXT_GetStat: parameter error"), the
// way they are told apart here. Statuses: 0 stop, 1 decoding the header, 2
// preparing, 3 playing, 4 decoded to the end, 5 played to the end, 6 error.
constexpr const char* kGetterSig =
    "8B 44 24 04 85 C0 75 11 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 04 83 C8 FF C3";
constexpr const char* kStartAfsSig = "83 EC 18 53 56 8B 74 24 24 57 85 F6 75 14 68 ?? ?? ?? ?? E8";
constexpr const char* kStartMemIdxSig = "53 8B 5C 24 08 56 57 85 DB 0F 84 ?? ?? ?? ?? 8B 74 24 14 85 F6 0F 84";
constexpr const char* kStopSig = "56 8B 74 24 08 85 F6 75 0E 5E C7 44 24 04 ?? ?? ?? ?? E9";
constexpr int kStop = 0, kPlaying = 3, kPlayEnd = 5, kError = 6;
// The library sits after the game's code: calls from below this distance
// under a function are the game's, the rest the library's own.
constexpr uintptr_t kLibrary = 0x20000;

using Get_t = int(__cdecl*)(void* adxt);
using Start_t = void(__cdecl*)(void* adxt, int a, int b);
using Stop_t = void(__cdecl*)(void* adxt);
Get_t g_stat = nullptr, g_samples = nullptr, g_rate = nullptr;
Start_t g_start_afs = nullptr, g_start_mem = nullptr;
Stop_t g_stop = nullptr;
bool g_trace = false;
uint32_t g_extra = 0;  // [debug] voice_extra (lab)

// A voice the game started in the running session: from which frame, until
// which (known once its header is read).
struct Track {
  void* adxt = nullptr;
  uint32_t session = 0, from = 0, until = 0;
  bool known = false;
};
Track g_tracks[32];

bool in_session() { return lockstep::state() == lockstep::State::Running; }
uint32_t now() { return lockstep::frame(); }

Track* track(void* adxt, bool make) {
  Track* free_one = nullptr;
  for (Track& t : g_tracks) {
    if (t.adxt == adxt) return &t;
    if (!t.adxt && !free_one) free_one = &t;
  }
  if (!make) return nullptr;
  Track* t = free_one ? free_one : &g_tracks[0];
  *t = Track{adxt};
  return t;
}

void started(void* adxt) {
  if (!adxt) return;
  if (!in_session()) {
    if (Track* t = track(adxt, false)) *t = Track{};
    return;
  }
  Track* t = track(adxt, true);
  t->from = now();
  t->known = false;
  t->until = 0;
  if (g_trace) LOGI("trace: voice %p started at frame %u", adxt, t->from);
}

void __cdecl hkStartAfs(void* adxt, int a, int b) {
  g_start_afs(adxt, a, b);
  started(adxt);
}
void __cdecl hkStartMem(void* adxt, int a, int b) {
  g_start_mem(adxt, a, b);
  started(adxt);
}
void __cdecl hkStop(void* adxt) {
  g_stop(adxt);
  if (Track* t = track(adxt, false)) *t = Track{};
}

int __cdecl hkStat(void* adxt) {
  int real = g_stat(adxt);
  if (!adxt || !in_session()) return real;
  Track* t = track(adxt, false);
  if (real == kStop || real == kError) {
    if (t) *t = Track{};
    return real;
  }
  if (!t) {  // (started before the session, or not by these calls): from now
    t = track(adxt, true);
    t->from = now();
  }
  if (!t->known) {
    int n = g_samples(adxt), rate = g_rate(adxt);
    if (n > 0 && rate > 0) {
      t->until = t->from + (uint32_t)(((uint64_t)n * 60 + (uint64_t)rate - 1) / (uint64_t)rate) + g_extra;
      t->known = true;
      if (g_trace) LOGI("trace: voice %p plays %d samples at %d Hz: frames %u..%u", adxt, n, rate, t->from, t->until);
    }
  }
  if (t->known && now() >= t->until) {
    if (g_trace && real != kPlayEnd) LOGI("trace: voice %p ends at frame %u (its sound: status %d)", adxt, now(), real);
    return kPlayEnd;
  }
  return kPlaying;
}

// The function of a getter-like ADXT entry whose message names `what`.
bool named(uintptr_t fn, size_t imm, const char* what) {
  uintptr_t msg = patch::read<uint32_t>(fn + imm);
  if (!patch::in_image(msg, 64)) return false;
  return std::strstr(reinterpret_cast<const char*>(msg), what) != nullptr;
}

uintptr_t named_getter(const char* what) {
  for (uintptr_t a : patch::find_all(kGetterSig))
    if (named(a, 9, what)) return a;
  return 0;
}

// Of several matches, the function whose parameter check within its first
// bytes names `what` (push imm32 / mov [esp+4], imm32 of its message).
uintptr_t named_function(const char* pattern, const char* what) {
  for (uintptr_t a : patch::find_all(pattern))
    for (size_t i = 0; i < 0x100; ++i) {
      size_t imm = 0;
      if (patch::read<uint8_t>(a + i) == 0x68) imm = i + 1;
      else if (patch::read<uint32_t>(a + i) == 0x042444C7u) imm = i + 4;
      if (imm && named(a, imm, what)) return a;
    }
  return 0;
}

// Points the game's own calls of `target` (not the library's) at `detour`.
int redirect_game_calls(const char* name, uintptr_t target, void* detour) {
  int n = 0;
  for (uintptr_t at : patch::find_calls(target)) {
    if (at + kLibrary > target) continue;  // the library's own
    int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(detour) - (at + 5));
    n += patch::write(at + 1, &rel, 4);
  }
  LOGD("voice sync: %d of the game's calls of %s are ours", n, name);
  return n;
}

}  // namespace

void install(const Config& cfg) {
  g_trace = cfg.debug_actions;
  g_extra = (uint32_t)cfg.debug_voice_extra;
  uintptr_t stat = named_getter("ADXT_GetStat:");
  uintptr_t samples = named_getter("ADXT_GetNumSmpl:");
  uintptr_t rate = named_getter("ADXT_GetSfreq:");
  uintptr_t afs = named_function(kStartAfsSig, "ADXT_StartAfs:");
  uintptr_t mem = named_function(kStartMemIdxSig, "ADXT_StartMemIdx:");
  uintptr_t stop = named_function(kStopSig, "ADXT_Stop:");
  if (!stat || !samples || !rate || !afs || !mem || !stop) {
    LOGW("voice sync: CRI ADX not where expected; voices may part the games in a game together");
    return;
  }
  g_stat = reinterpret_cast<Get_t>(stat);
  g_samples = reinterpret_cast<Get_t>(samples);
  g_rate = reinterpret_cast<Get_t>(rate);
  g_start_afs = reinterpret_cast<Start_t>(afs);
  g_start_mem = reinterpret_cast<Start_t>(mem);
  g_stop = reinterpret_cast<Stop_t>(stop);
  int a = redirect_game_calls("ADXT_StartAfs", afs, (void*)&hkStartAfs);
  int b = redirect_game_calls("ADXT_StartMemIdx", mem, (void*)&hkStartMem);
  int c = redirect_game_calls("ADXT_Stop", stop, (void*)&hkStop);
  int d = redirect_game_calls("ADXT_GetStat", stat, (void*)&hkStat);
  LOGI("voice sync: voices end by the frame in a game together (%d starts, %d stops, %d status calls)", a + b, c, d);
}

}  // namespace voice_sync
