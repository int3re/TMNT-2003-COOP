#include "mod/audio.h"

#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

#include "common/log.h"
#include "common/net.h"
#include "mod/main_menu.h"
#include "mod/patch.h"

namespace audio {
namespace {

Config g_cfg;
std::atomic<int> g_movie_mb{0};  // extra attenuation for movie buffers, 1/100 dB
std::atomic<bool> g_follow{true};  // [audio] movie_follow
std::atomic<int> g_level{-1};      // [audio] movie_level: 0..4, -1 = the music's
bool g_global_focus = false;    // sound keeps playing in the background

using DSCreate_t = HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);
using CreateBuffer_t = HRESULT(STDMETHODCALLTYPE*)(IDirectSound*, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*,
                                                   LPUNKNOWN);
using SetVolume_t = HRESULT(STDMETHODCALLTYPE*)(IDirectSoundBuffer*, LONG);
using Release_t = ULONG(STDMETHODCALLTYPE*)(IDirectSoundBuffer*);
DSCreate_t oDSCreate = nullptr;
CreateBuffer_t oCreateBuffer = nullptr;
SetVolume_t oSetVolume = nullptr;
Release_t oRelease = nullptr;

// The game's movies are CRI Sofdec; each creates its own DirectSound buffer
// right after mwPlyCreateSofdec (the game's sound pool is made once at start).
// Buffers made within this window after a movie is created are movie buffers.
constexpr const char* kCreateSofdecSig =
    "83 EC 18 53 8B 5C 24 20 55 56 85 DB 57 75 ?? 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 04 33 C0 5F 5E 5D 5B "
    "83 C4 18 C3 53 E8";
constexpr uint32_t kMovieWindowMs = 3000;
using CreateSofdec_t = void*(__cdecl*)(void* cprm);
CreateSofdec_t oCreateSofdec = nullptr;
std::atomic<uint32_t> g_movie_until{0};

int music_mb();

void* __cdecl hkCreateSofdec(void* cprm) {
  LOGI("sound: a movie starts: %s%.1f dB", g_follow.load() ? "at the music's volume, " : "",
       (music_mb() + g_movie_mb.load()) / 100.0);
  g_movie_until = net::now_ms() + kMovieWindowMs;
  return oCreateSofdec(cprm);
}

// The game's own music volume (TMNT.EXE bc49a82c..., re/notes.md "Sound").
// Its options keep 5 levels; the sound object turns the music level into a
// gain 0.2..1.0 at +0x24, and a music track plays at T[trunc(base * gain)] - 5
// (CRI volume, 1/10 dB) with T a 128-entry table and a per-track base of
// 45..127, mostly 50..80. Movies are never given any of it: the movie task
// sets 0 dB every frame. They get the music's level here, with base 70.
constexpr const char* kSoundObjSig = "A1 ?? ?? ?? ?? D9 E8 D9 58 24 6A 04 59 D9 E8";  // imm +1
constexpr const char* kDbTableSig =
    "8B 44 24 04 85 C0 7D 02 33 C0 83 F8 7F 7E 03 6A 7F 58 8B 04 85 ?? ?? ?? ?? C3";  // imm +21
constexpr size_t kMusicGain = 0x24;
constexpr int kMovieBase = 70;
uint8_t** g_sound_obj = nullptr;
const int32_t* g_db = nullptr;

// The music's gain now, 0.2..1.0 (its level 0..4 + 1, times 0.2); -1 unknown.
float music_gain() {
  if (!g_sound_obj) return -1;
  const uint8_t* obj = *g_sound_obj;
  if (!patch::in_image(reinterpret_cast<uintptr_t>(obj), 0x40)) return -1;
  float gain = *reinterpret_cast<const float*>(obj + kMusicGain);
  return gain >= 0.0f && gain <= 1.0f ? gain : -1;  // (not set up yet)
}

// The movies' volume now, in 1/100 dB (0 = unknown or full): their own level
// (РОЛИКИ), or the music's.
int music_mb() {
  if (!g_db) return 0;
  int level = g_level.load();
  float gain = level >= 0 ? (level + 1) * 0.2f : g_follow.load() ? music_gain() : -1;
  if (gain < 0) return 0;
  int i = std::clamp((int)(kMovieBase * gain), 0, 127);
  return (g_db[i] - 5) * 10;
}

// Movie buffers, with the volume the game asked for last (DirectSound
// buffers can be used from the game's sound thread).
struct Tracked {
  IDirectSoundBuffer* buf;
  LONG asked;
};
std::mutex g_lock;
std::vector<Tracked> g_movie;
std::vector<Tracked> g_others;  // every other buffer of the game's (for set_silent)
bool g_silent = false;          // (under g_lock)

LONG attenuate(LONG asked) {
  return std::clamp<LONG>(asked + music_mb() + g_movie_mb.load(), DSBVOLUME_MIN, DSBVOLUME_MAX);
}

int percent_to_mb(int percent) {
  if (percent >= 100) return 0;
  if (percent <= 0) return DSBVOLUME_MIN;
  return (int)std::lround(2000.0 * std::log10(percent / 100.0));
}

HRESULT STDMETHODCALLTYPE hkSetVolume(IDirectSoundBuffer* b, LONG vol) {
  {
    std::lock_guard<std::mutex> lock(g_lock);
    for (Tracked& t : g_movie)
      if (t.buf == b) {
        t.asked = vol;
        return oSetVolume(b, g_silent ? DSBVOLUME_MIN : attenuate(vol));
      }
    for (Tracked& t : g_others)
      if (t.buf == b) {
        t.asked = vol;
        return oSetVolume(b, g_silent ? DSBVOLUME_MIN : vol);
      }
  }
  return oSetVolume(b, vol);
}

ULONG STDMETHODCALLTYPE hkRelease(IDirectSoundBuffer* b) {
  ULONG left = oRelease(b);
  if (left == 0) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_movie.erase(std::remove_if(g_movie.begin(), g_movie.end(), [b](const Tracked& t) { return t.buf == b; }),
                  g_movie.end());
    g_others.erase(std::remove_if(g_others.begin(), g_others.end(), [b](const Tracked& t) { return t.buf == b; }),
                   g_others.end());
  }
  return left;
}

HRESULT STDMETHODCALLTYPE hkCreateBuffer(IDirectSound* ds, LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER* out,
                                         LPUNKNOWN outer) {
  bool primary = desc && (desc->dwFlags & DSBCAPS_PRIMARYBUFFER);
  bool movie = !primary && (int32_t)(g_movie_until.load() - net::now_ms()) > 0;
  DSBUFFERDESC d{};
  if (desc) d = *desc;
  if (!primary && g_global_focus) d.dwFlags |= DSBCAPS_GLOBALFOCUS;
  if (movie) d.dwFlags |= DSBCAPS_CTRLVOLUME;  // so its volume can be lowered
  HRESULT hr = oCreateBuffer(ds, desc ? &d : desc, out, outer);
  if (SUCCEEDED(hr) && out && *out && !primary) {
    IDirectSoundBuffer* b = *out;
    if (!oSetVolume) {
      void** vt = *reinterpret_cast<void***>(b);
      patch::hook("IDirectSoundBuffer::SetVolume", (uintptr_t)vt[15], (void*)&hkSetVolume, (void**)&oSetVolume);
      patch::hook("IDirectSoundBuffer::Release", (uintptr_t)vt[2], (void*)&hkRelease, (void**)&oRelease);
    }
    const WAVEFORMATEX* f = d.lpwfxFormat;
    LOGD("sound buffer %p: %lu bytes, %lu Hz %u ch %u bit, flags 0x%lX%s", (void*)b, d.dwBufferBytes,
         f ? f->nSamplesPerSec : 0, f ? f->nChannels : 0, f ? f->wBitsPerSample : 0, d.dwFlags,
         movie ? ", a movie's (movie volume applies)" : "");
    if (oSetVolume) {
      std::lock_guard<std::mutex> lock(g_lock);
      (movie ? g_movie : g_others).push_back(Tracked{b, 0});
      if (g_silent && (d.dwFlags & DSBCAPS_CTRLVOLUME))
        oSetVolume(b, DSBVOLUME_MIN);
      else if (movie)
        oSetVolume(b, attenuate(0));
    }
  }
  return hr;
}

HRESULT WINAPI hkDSCreate(LPCGUID guid, LPDIRECTSOUND* out, LPUNKNOWN outer) {
  HRESULT hr = oDSCreate(guid, out, outer);
  if (SUCCEEDED(hr) && out && *out && !oCreateBuffer) {
    void** vt = *reinterpret_cast<void***>(*out);
    patch::hook("IDirectSound::CreateSoundBuffer", (uintptr_t)vt[3], (void*)&hkCreateBuffer, (void**)&oCreateBuffer);
  }
  return hr;
}

}  // namespace

void install(const Config& cfg) {
  g_cfg = cfg;
  g_global_focus = cfg.background_run && cfg.display != "fullscreen";
  g_movie_mb = percent_to_mb(cfg.movie_volume);
  HMODULE ds = GetModuleHandleW(L"dsound.dll");  // imported by tmnt.exe
  auto create = ds ? (uintptr_t)GetProcAddress(ds, "DirectSoundCreate") : 0;
  if (!create || !patch::hook("DirectSoundCreate", create, (void*)&hkDSCreate, (void**)&oDSCreate)) {
    LOGW("sound: DirectSoundCreate not hooked; movie volume and background sound are off");
    return;
  }
  if (uintptr_t a = patch::find("mwPlyCreateSofdec", kCreateSofdecSig))
    patch::hook("mwPlyCreateSofdec", a, (void*)&hkCreateSofdec, (void**)&oCreateSofdec);
  if (uintptr_t a = patch::find("sound_object_ptr", kSoundObjSig)) {
    uintptr_t v = patch::read<uint32_t>(a + 1);
    if (patch::in_image(v, 4)) g_sound_obj = reinterpret_cast<uint8_t**>(v);
  }
  if (uintptr_t a = patch::find("volume_db_table", kDbTableSig)) {
    uintptr_t t = patch::read<uint32_t>(a + 21);
    if (patch::in_image(t, 128 * 4)) g_db = reinterpret_cast<const int32_t*>(t);
  }
  g_follow = cfg.movie_follow;
  g_level = cfg.movie_level;
  LOGI("sound: movies %s, %d%% of that%s%s%s",
       !cfg.movie_follow ? "at their own volume" : g_sound_obj && g_db ? "at the music's volume" :
                                                                         "at their own volume (music volume not found)",
       cfg.movie_volume, oCreateSofdec ? "" : " (movies not recognised)", cfg.intro_skip ? ", intro skipped" : "",
       g_global_focus ? ", plays in the background" : "");
}

void reapply() {
  std::lock_guard<std::mutex> lock(g_lock);
  if (oSetVolume && !g_silent)
    for (const Tracked& t : g_movie) oSetVolume(t.buf, attenuate(t.asked));
}

void set_silent(bool on) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (g_silent == on) return;
  g_silent = on;
  if (!oSetVolume) return;
  for (const Tracked& t : g_movie) oSetVolume(t.buf, on ? DSBVOLUME_MIN : attenuate(t.asked));
  for (const Tracked& t : g_others) oSetVolume(t.buf, on ? DSBVOLUME_MIN : t.asked);
  LOGI("sound: %s (%u buffers)", on ? "silent while this game catches up" : "back", (unsigned)(g_movie.size() + g_others.size()));
}

void set_movie_level(int level) {
  g_level = std::clamp(level, -1, 4);
  LOGI("sound: movies %s", level < 0 ? "at the music's level" : ("at level " + std::to_string(level)).c_str());
}

int movie_level() {
  int level = g_level.load();
  if (level >= 0) return level;
  float gain = music_gain();
  return gain < 0 ? 4 : std::clamp((int)std::lround(gain / 0.2f) - 1, 0, 4);
}

bool movie_level_follows() { return g_level.load() < 0; }

void set_movie_follow(bool on) {
  g_follow = on;
  reapply();
}

void set_movie_volume(int percent) {
  g_movie_mb = percent_to_mb(percent);
  reapply();
}

bool in_intro() {
  static std::atomic<bool> over{false};
  if (!over && main_menu::on_screen()) over = true;
  return !over;
}

}  // namespace audio
