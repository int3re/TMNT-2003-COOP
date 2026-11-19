#include "mod/lockstep.h"

#include <windows.h>
#include <intrin.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/log.h"
#include "mod/csel.h"
#include "mod/four_players.h"
#include "mod/game_options.h"
#include "mod/main_menu.h"
#include "mod/native_menu.h"
#include "mod/net_host.h"
#include "mod/net_peer.h"
#include "mod/patch.h"

namespace lockstep {
namespace {

// CRT rand() / srand() in TMNT.EXE (0x565dbd / 0x565db0, re/notes.md §18): the
// state is the calling thread's (_getptd() + 0x14); the game's logic runs on
// the main thread and reseeds only with srand(rand()).
constexpr const char* kRandSig = "E8 ?? ?? ?? ?? 8B 48 14 69 C9 FD 43 03 00 81 C1 C3 9E 26 00 89 48 14";
constexpr const char* kSrandSig = "E8 ?? ?? ?? ?? 8B 4C 24 04 89 48 14 C3";
constexpr size_t kHoldRand = 0x14;
// WinMain's single-instance check (0x4b0109): CreateMutexA(0, 0, name) +
// WaitForSingleObject(INFINITE), then FindWindowA(name) → focus it and quit.
// The name is also the window class. +1 = &"TMNT_WIN32".
constexpr const char* kInstanceSig = "BE ?? ?? ?? ?? 56 33 FF 57 57 89 85 AC 00 00 00 FF 15";

// The game's loader (0x4a6fde, once a frame in the main loop) reads files
// through CRI's ADX file system in the background and asks every frame
// whether the read is done (0x4a6d85: ADXF_GetStat(handle), 2 = reading): how
// many frames a load takes depends on the disk. That call goes to loader_stat,
// which (in a session) waits for the read to end, so every load takes the
// same frames.
constexpr const char* kLoaderStatSig = "FF B7 84 2F 00 00 C1 E6 04 81 C6 ?? ?? ?? ?? E8";  // +15 call
constexpr int kAdxfReading = 2;
constexpr DWORD kLoadWaitMaxMs = 10000;
// Sound banks (0x4aeaa6, a step a frame from the sound update; state at
// [0x69eb5c]): stop every sound, wait until the 8 ADX voices are stopped,
// read the bank: frames depend on the PC. The logic asks "loaded?" (0x4ae594:
// state == 0) before it goes on. A bank started in a frame is finished at
// that frame's end (finish_bank, at the keyboard poll, game thread), stepping
// the loader the way the game's own synchronous load does (0x4ae96d: wait for
// CRI's tick 0x4af69d, run CRI 0x4af6b1), so the next frame always finds it.
constexpr const char* kBankDoneSig = "33 C0 39 05 ?? ?? ?? ?? 0F 94 C0 C3";  // +4 &state
constexpr const char* kBankStepSig = "A1 ?? ?? ?? ?? 48 56 0F 84 ?? ?? ?? ?? 48 0F 84 ?? ?? ?? ?? 48 0F 84";
constexpr const char* kCriSig = "E9 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? E9";  // tick; +20 run
// The sound update (0x4af0f0) steps the banks only once the sound system is
// up: call getter (0x4af450: mov eax, 0x69eb78), [eax + 8] != 0.
constexpr const char* kSoundUpdateSig = "E8 ?? ?? ?? ?? 83 78 08 00 74 0F E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 33 C0 C3";
// [debug] light (lab): the game waits for its next frame by reading the
// performance counter in a loop (0x4b006d), a CPU core at 100% a copy; with
// light=1 that read sleeps 1 ms first. +5 = the call in the loop.
constexpr const char* kFrameSpinSig = "39 7D EC 73 0F E8 ?? ?? ?? ?? 3B D6 7C F7 7F 04 3B C7 72 F1";
// The per-player masks (0x4b2a2f, end of the input step, before the pad slots
// 0x4a984d read them): {held, pressed, released} a player at 0x69f590 (+20),
// the sticks' bytes 4 a player 8 bytes before.
constexpr const char* kMasksSig = "55 8B EC 83 EC 0C 53 56 57 E8 ?? ?? ?? ?? 6A 06 33 C0 59 BF ?? ?? ?? ?? F3 AB";
constexpr size_t kMaskStride = 0x0C, kSticksBack = 8;
constexpr int kGamePlayers = 2;
// The story progress (re/notes.md §14): serialize(buf) 0x4aa585 builds the
// savegame.dat image from memory, deserialize(buf) 0x4aa5d1 puts one back
// (0 = ok). Both on the game thread, outside a stage.
constexpr const char* kSerializeSig = "56 8B 74 24 08 8D 46 08 57 50 C6 06 03 E8 ?? ?? ?? ?? 8D 86 30 01 00 00";
constexpr const char* kDeserializeSig = "56 8B 74 24 08 80 3E 03 74 05 33 C0 40 5E C3 57 8D BE 60 11 00 00";
// The main loop's "go on?" (0x4ab20f: [0x638c64] != 0). Its first 0 starts the
// way out: the settings are saved (0x4b3607) and the game's parts torn down
// before the window gets WM_DESTROY, so what must be given back goes here.
constexpr const char* kRunningSig = "33 C0 39 05 ?? ?? ?? ?? 0F 95 C0 C3";
// "Skip this frame's drawing?" (0x4af87e: a 12-frame pattern for the level of
// [0x69f554], raised when the PC is slow). It skips the tasks' draw pass too
// (0x4ac3a3), and some draw callbacks draw rand(): a PC that skips a frame the
// other draws has had one rand() less (seen at frame 2875 of a lab session).
// In a game together nothing is skipped; a slow PC is just slower.
constexpr const char* kSkipSig = "A1 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 8D 04 40 8D 04 81 8B 04 85";
// The task list the update pass walks (0x4aca5f: mov esi, [head]; a node:
// +0 task, +8 next; a task: +0x10 state, +0x1C update, +0x34 data). +3 = &head.
constexpr const char* kTaskListSig = "56 8B 35 ?? ?? ?? ?? EB 36 8B 06 83 78 10 03 74 2B";
// The game's input devices (0x4b08ad walks them from [0x69f570]; a node: +0xC
// state size, +0x10 this poll's state, +0x14 the last one, +0x1C next):
// keyboard, mouse, gamepads. In a session they are emptied after the masks,
// so code that reads a device itself (the key table's capture 0x4b0df7) finds
// nothing, on both PCs alike; the players' buttons come with the masks.
constexpr const char* kDevicesSig = "85 C9 A1 ?? ?? ?? ?? 7E 06 49 8B 40 1C 75 FA C3";  // +3 &head
constexpr int32_t kJoyState = 80, kJoyPov = 32;  // DIJOYSTATE: rgdwPOV[4] at 32 (0xFFFFFFFF = centred)

using Rand_t = int(__cdecl*)();
using Srand_t = void(__cdecl*)(unsigned seed);
using Ptd_t = uint8_t*(__cdecl*)();
using Stat_t = int(__cdecl*)(void* handle);
using Void_t = void(__cdecl*)();
using Qpc_t = int64_t(__cdecl*)();
using Image_t = int(__cdecl*)(uint8_t* buf);

Rand_t oRand = nullptr;
Srand_t g_srand = nullptr;
Ptd_t g_ptd = nullptr;
Stat_t g_adxf_stat = nullptr;
Void_t g_bank_step = nullptr, g_cri_tick = nullptr, g_cri_run = nullptr;
Void_t oMasks = nullptr;
volatile int32_t* g_bank_state = nullptr;
volatile int32_t* g_sound_up = nullptr;
Qpc_t g_qpc = nullptr;
Image_t g_serialize = nullptr, g_deserialize = nullptr;
using Running_t = int(__cdecl*)();
Running_t oRunning = nullptr;
Running_t oSkip = nullptr;
uint32_t* g_masks = nullptr;  // [player * 3]: held, pressed, released
uint8_t** g_dev_head = nullptr;
uint8_t** g_task_head = nullptr;
uint8_t* g_sticks = nullptr;  // [player * 4]
uint32_t g_load_waits = 0, g_bank_waits = 0;

Config g_cfg;
DWORD g_game_thread = 0;
bool g_exact_loads = false;  // loads end within their frame (probe or session)

// --- the lab probe ------------------------------------------------------------
int g_probe_frame = -1;  // frames since the common start, -1 = before it
FILE* g_log = nullptr;
// rand() on the game thread this frame: count and callers.
uint32_t g_calls = 0;
constexpr int kMaxTrace = 300;
uintptr_t g_callers[kMaxTrace];

// --- the session ----------------------------------------------------------------
constexpr int kMaxPlayers = 4;
constexpr uint32_t kRing = 8192;  // frames kept a player (a PC coming back gets up to half ahead)
constexpr DWORD kFirstWaitMs = 60000;  // until the other PC has begun too
constexpr DWORD kStallEndMs = 10000;   // later: the session ends
constexpr int kHashEvery = 60;

struct Ring {
  std::atomic<uint32_t> tag[kRing];  // frame + 1 when buttons[] holds that frame
  uint16_t buttons[kRing];
};
Ring g_ring[kMaxPlayers];

SRWLOCK g_lock = SRWLOCK_INIT;  // g_start, g_out, g_hash_out, g_remote_hash
Start g_start;
std::atomic<int> g_state{0};  // State
std::atomic<uint32_t> g_session{0};
std::atomic<uint32_t> g_now{0};  // the session's frame, for the pages
tmnp::LsInputPacket g_out{};
uint32_t g_out_version = 0;
tmnp::LsHashPacket g_hash_out{};
bool g_hash_new = false;
struct HashAt {
  uint32_t frame = 0xFFFFFFFF, hash = 0;
};
HashAt g_local_hash[8], g_remote_hash[8];
std::atomic<bool> g_ready_here{false}, g_ready_there{false};  // pending: each game can begin
// While pending: where this game is (game thread, every frame) and when it
// last said so; where the other is (the host: its first guest not ready).
std::atomic<uint8_t> g_where_here{0}, g_where_there{0}, g_there_player{0};
std::atomic<uint32_t> g_where_ms{0};
constexpr uint32_t kWhereStaleMs = 400;  // no frame for that long: the game is paused (window away)
std::atomic<bool> g_close_pages{false};  // a session was just set: our pages close (game thread)
std::atomic<bool> g_player_ready{false};  // this PC's player pressed ГОТОВ (a guest)
std::atomic<uint8_t> g_ready_mask{0};     // the host's: who pressed it (bit p = player p + 1)
std::atomic<int> g_auto_players{0};       // the host's НАЧАТЬ ИГРУ: the story for that many
// ... said to every game as a word in the host's buttons of one frame (see
// publish_local): bit 9, a slot of the key table no key of the game's has.
constexpr uint16_t kStoryWord = 1u << 9;
// The pause: slot 8 of the key table (Esc). The game itself asks the keyboard
// raw for it (0x4b0d74), which a session blanks: here it goes with the
// buttons, and every PC sees whose it was in the same frame.
constexpr uint16_t kPauseSlot = 1u << 8;
constexpr uint32_t kStoryFrames = 900;  // how long the start may take (the menu comes up, the screen goes over)
bool g_auto_said = false;               // the word went out
uint32_t g_auto_from = 0;
uint32_t g_story_cmd = 0;               // the frame + 1 the host's word came in (0: none); game thread
std::atomic<bool> g_ended_notice{false};  // a session ended: say so (game thread)
std::atomic<bool> g_waits_for_buttons{false};  // the game thread stands in wait_all
int g_players_now = 2;  // players of the session that runs or ran last (game thread)
uint8_t g_options[24] = {};  // this game's options, for the host's start packet
uint32_t g_options_at = 0;
uint8_t g_save[tmnp::kSaveImage];  // this game's progress, for the host's LsSave
bool g_save_ok = false;
// The game data the save image leaves out ([0x60eae4] +0x10E8..+0x1157): this
// game's (for the host's start), and the other PC's own while it plays with
// the host's.
uint8_t** g_game_data = nullptr;
uint8_t g_data[tmnp::kGameData];
bool g_data_ok = false;
uint8_t g_own_data[tmnp::kGameData];
bool g_own_data_kept = false;
constexpr size_t kGameDataAt = 0x10E8;
uint8_t g_remote_save[tmnp::kSaveImage];  // the host's, on the other PC
bool g_have_remote_save = false;
// The other PC's own progress and options while it plays with the host's.
uint8_t g_own_save[tmnp::kSaveImage];
uint8_t g_own_options[24];
bool g_own_kept = false;
std::atomic<bool> g_restore{false};  // the session is over: give them back (game thread, main menu)
std::atomic<uint32_t> g_set_ms{0};     // when the session was set (net_start)
constexpr uint32_t kSaveWaitMs = 5000;  // then begin without the host's progress
std::wstring g_save_path, g_save_backup, g_save_shared;

// The session's buttons of every frame, and its state hash every kHashEvery
// frames (game thread appends; the host's network thread reads them for a PC
// coming back, and checks that PC's hashes against them).
SRWLOCK g_hist_lock = SRWLOCK_INIT;
std::vector<uint16_t> g_hist;       // [frame * kMaxPlayers + player]
std::vector<uint32_t> g_hash_hist;  // [frame / kHashEvery]
std::atomic<uint32_t> g_hist_frames{0};
bool g_replay_desync = false;

// Coming back (Start::catchup): the frames the host sent so far (0..have-1),
// and the frame + 1 from which our player's buttons are ours again (LsReturn).
std::atomic<bool> g_catchup{false};
std::atomic<uint32_t> g_have{0};
std::atomic<uint32_t> g_return{0};
std::atomic<int> g_back_notice{0};  // a player who came back (player + 1), for the notice
uint32_t g_catchup_ms = 0;          // when the catching up began (game thread)
uint32_t g_catchup_note = 0;        // its last notice

// Game thread only.
uint32_t g_n = 0;  // the next frame to play
uint16_t g_prev[kMaxPlayers] = {};
uint16_t g_row_prev[kMaxPlayers] = {};  // everybody's buttons of the frame before (the pause key's edge)
int g_pause_by = -1;                    // whose pause key came down in this frame
std::atomic<bool> g_esc_down{false};    // this keyboard's Esc, as the game's poll has it
bool g_heard = false;  // the other PC's buttons have come at least once
bool g_desync_logged = false;
uint32_t g_stalls = 0, g_stall_ms = 0, g_stall_max = 0, g_stat_at = 0;

bool g_probe_synced = false;  // (the lab probe below)

int __cdecl hkSkip() {
  if (g_probe_synced || g_state.load() == (int)State::Running) return 0;
  return oSkip();
}

int __cdecl hkRunning() {
  int r = oRunning();
  if (!r) {
    on_exit();  // (lockstep.h)
    // Steam (lobby, sessions) closed now, while the game's window and its
    // Direct3D are still whole: at WM_DESTROY (0.11.9) the exit once crashed
    // inside Steam right after the lobby was left.
    stop_net_host();
    stop_net_peer();
  }
  return r;
}

int __cdecl hkRand() {
  if (g_game_thread && GetCurrentThreadId() == g_game_thread) {
    if (g_calls < kMaxTrace) g_callers[g_calls] = reinterpret_cast<uintptr_t>(_ReturnAddress());
    ++g_calls;
  }
  return oRand();
}

uint32_t hold_rand() { return g_ptd ? *reinterpret_cast<uint32_t*>(g_ptd() + kHoldRand) : 0; }

void set_hold_rand(uint32_t v) {
  if (g_ptd) *reinterpret_cast<uint32_t*>(g_ptd() + kHoldRand) = v;
}

int __cdecl loader_stat(void* handle) {
  int s = g_adxf_stat(handle);
  if (s != kAdxfReading || !g_exact_loads) return s;
  DWORD t0 = GetTickCount();
  while (s == kAdxfReading && GetTickCount() - t0 < kLoadWaitMaxMs) {
    Sleep(1);
    s = g_adxf_stat(handle);
  }
  ++g_load_waits;
  if (s == kAdxfReading) LOGW("lockstep: a file read did not end in %lu ms", (unsigned long)(GetTickCount() - t0));
  return s;
}

// The bank's step 2 (0x4aeaa6 → 0x4ae735): every one of the 8 ADX voices
// stopped or played to its end. In a game together the voices end by the
// frame (voice_sync.cpp): inside one frame a voice that is still on by its
// frame count never ends, so there the bank waits for the next frames, which
// see it end at the same frame on every PC (0.11.10, a real game: after its
// 4th stage each frame waited the whole 10 s here, the game stood still).
constexpr int32_t kBankVoices = 2;
uint32_t g_bank_held = 0;  // frames the bank waited for voices (game thread)

void finish_bank() {
  if (!g_exact_loads || !g_bank_state || *g_bank_state == 0 || !g_sound_up || *g_sound_up == 0) return;
  uint32_t seed = hold_rand();  // (whatever the extra steps draw is not the logic's)
  bool by_frame = g_state.load() == (int)State::Running;
  bool held = false;
  DWORD t0 = GetTickCount();
  while (*g_bank_state != 0 && GetTickCount() - t0 < kLoadWaitMaxMs) {
    int32_t before = *g_bank_state;
    g_cri_tick();
    g_cri_run();
    g_bank_step();
    if (by_frame && before == kBankVoices && *g_bank_state == kBankVoices) {  // (asked once this frame)
      held = true;
      break;
    }
  }
  set_hold_rand(seed);
  ++g_bank_waits;
  if (held) {
    ++g_bank_held;
    return;
  }
  if (g_bank_held) {
    LOGI("lockstep: a sound bank waited %u frames for voices to end", g_bank_held);
    g_bank_held = 0;
  }
  if (*g_bank_state != 0) LOGW("lockstep: a sound bank did not load in %lu ms", (unsigned long)(GetTickCount() - t0));
}

// The loader's question "is the read done?" gets loader_stat; sound banks are
// finished at the end of their frame (finish_bank).
void sync_loads() {
  uintptr_t at = patch::find("loader_read_stat", kLoaderStatSig);
  if (!at) {
    LOGW("lockstep: the loader's read check not found, loads may take different frames");
  } else {
    uintptr_t call = at + 15;
    g_adxf_stat = reinterpret_cast<Stat_t>(call + 5 + patch::read<int32_t>(call + 1));
    int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(&loader_stat) - (call + 5));
    patch::write(call + 1, &rel, 4);
  }
  uintptr_t done = patch::find("sound_bank_done", kBankDoneSig);
  uintptr_t step = patch::find("sound_bank_step", kBankStepSig);
  uintptr_t cri = patch::find("cri_tick_run", kCriSig);
  uintptr_t update = patch::find("sound_update", kSoundUpdateSig);
  uintptr_t getter = update ? update + 5 + patch::read<int32_t>(update + 1) : 0;
  if (getter && patch::read<uint8_t>(getter) == 0xB8 && patch::in_image(patch::read<uint32_t>(getter + 1) + 8, 4))
    g_sound_up = reinterpret_cast<volatile int32_t*>(patch::read<uint32_t>(getter + 1) + 8);
  if (!done || !step || !cri || !g_sound_up || patch::read<uint32_t>(step + 1) != patch::read<uint32_t>(done + 4)) {
    LOGW("lockstep: the sound bank loader not found, level starts may take different frames");
    return;
  }
  g_bank_state = reinterpret_cast<volatile int32_t*>(patch::read<uint32_t>(done + 4));
  g_bank_step = reinterpret_cast<Void_t>(step);
  g_cri_tick = reinterpret_cast<Void_t>(cri);
  g_cri_run = reinterpret_cast<Void_t>(cri + 20);
}

bool g_light = false;

// The clock the game's frame wait spins on (0x4b0068: until the frame's time
// has come). Coming back and behind: the time has come at once (the frame's
// schedule starts again from the real clock every 12 frames, 0x4b00d4).
int64_t __cdecl frame_qpc() {
  if (fast()) return INT64_MAX / 4;
  if (g_light) Sleep(1);
  return g_qpc();
}

void frame_wait() {
  uintptr_t at = patch::find("frame_spin", kFrameSpinSig);
  if (!at) {
    LOGW("lockstep: the frame wait loop not found: a PC coming back catches up at the normal speed");
    return;
  }
  uintptr_t call = at + 5;
  g_qpc = reinterpret_cast<Qpc_t>(call + 5 + patch::read<int32_t>(call + 1));
  int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(&frame_qpc) - (call + 5));
  if (g_light) timeBeginPeriod(1);
  if (patch::write(call + 1, &rel, 4) && g_light) LOGI("lockstep: the frame wait sleeps instead of spinning (light)");
}

// Several copies at once (lab): the instance name becomes "TMNT_" + 5 hex
// digits of the process id, as long as the original.
void allow_instances() {
  uintptr_t at = patch::find("single_instance", kInstanceSig);
  uintptr_t name = at ? patch::read<uint32_t>(at + 1) : 0;
  if (!name || !patch::in_image(name, 11) || std::string(reinterpret_cast<const char*>(name)) != "TMNT_WIN32") {
    LOGW("lockstep: single-instance name not found, a second copy will wait for the first");
    return;
  }
  char now[11];
  snprintf(now, sizeof(now), "TMNT_%05X", (unsigned)(GetCurrentProcessId() & 0xFFFFF));
  if (patch::write(name, now, 10)) LOGI("lockstep: several copies allowed (this one is %s)", now);
}

// The two pad slots the menus and the game read (0x638a80, 0xAC each), as a hash.
uint32_t slots_hash() {
  uint8_t* s = main_menu::pad_slots();
  if (!s) return 0;
  uint32_t h = 2166136261u;
  for (int i = 0; i < 2 * 0xAC; ++i) h = (h ^ s[i]) * 16777619u;
  return h;
}

// [debug] task_log (lab): the tasks that came and went since the last frame.
void log_tasks() {
  struct T {
    uintptr_t task, update, data;
  };
  static std::vector<T> known;
  std::vector<T> now;
  uint8_t* node = g_task_head ? *g_task_head : nullptr;
  for (int i = 0; node && i < 1024; ++i, node = *reinterpret_cast<uint8_t**>(node + 8)) {
    uint8_t* task = *reinterpret_cast<uint8_t**>(node);
    if (!task) continue;
    now.push_back(T{reinterpret_cast<uintptr_t>(task), *reinterpret_cast<uintptr_t*>(task + 0x1C),
                    *reinterpret_cast<uintptr_t*>(task + 0x34)});
  }
  auto same = [](const T& a, const T& b) { return a.task == b.task && a.update == b.update; };
  for (const T& t : now)
    if (std::none_of(known.begin(), known.end(), [&](const T& k) { return same(k, t); }))
      LOGI("task +%08X update %08X data %08X", (unsigned)t.task, (unsigned)t.update, (unsigned)t.data);
  for (const T& t : known)
    if (std::none_of(now.begin(), now.end(), [&](const T& k) { return same(k, t); }))
      LOGI("task -%08X update %08X", (unsigned)t.task, (unsigned)t.update);
  known.swap(now);
}

void write_frame(int frame) {
  if (!g_log) return;
  unsigned short cw = 0;
  __asm fnstcw cw
  fprintf(g_log, "%d %08X %u%s%s w%u b%u p%04X,%04X s%08X f%04X", frame, hold_rand(), g_calls,
          main_menu::on_screen() ? " m" : "", csel::on_screen() ? " c" : "", g_load_waits, g_bank_waits,
          g_masks ? g_masks[0] & 0xFFFF : 0, g_masks ? g_masks[3] & 0xFFFF : 0, slots_hash(), cw);
  if (g_cfg.debug_rand_trace && g_calls) {
    fputs(" :", g_log);
    for (uint32_t i = 0; i < g_calls && i < (uint32_t)kMaxTrace; ++i) fprintf(g_log, " %X", (unsigned)g_callers[i]);
  }
  fputc('\n', g_log);
  if (frame % 60 == 0) fflush(g_log);
  g_calls = 0;
}

// --- the session, game thread -------------------------------------------------------

// A player whose PC left: none from that frame on (frame + 1; 0 = still
// playing), until the frame it came back (frame + 1; 0 = not back).
std::atomic<uint32_t> g_gone[kMaxPlayers];
std::atomic<uint32_t> g_back[kMaxPlayers];
bool gone_at(int player, uint32_t f) {
  uint32_t g = g_gone[player].load(std::memory_order_acquire);
  if (!g || f + 1 < g) return false;
  uint32_t b = g_back[player].load(std::memory_order_acquire);
  return !(b && f + 1 >= b);
}

// The network side may fill a frame only where the ring's slot holds one
// already played (the PC coming back gets frames far ahead of its game).
bool fits(uint32_t f) { return f < g_now.load(std::memory_order_relaxed) + kRing - 8; }

bool has(int player, uint32_t f) {
  return gone_at(player, f) || g_ring[player].tag[f % kRing].load(std::memory_order_acquire) == f + 1;
}

uint16_t buttons_at(int player, uint32_t f) { return gone_at(player, f) ? 0 : g_ring[player].buttons[f % kRing]; }

void put(int player, uint32_t f, uint16_t b) {
  g_ring[player].buttons[f % kRing] = b;
  g_ring[player].tag[f % kRing].store(f + 1, std::memory_order_release);
}

void clear_rings() {
  for (Ring& r : g_ring)
    for (auto& t : r.tag) t.store(0, std::memory_order_relaxed);
  for (auto& g : g_gone) g.store(0, std::memory_order_relaxed);
  for (auto& b : g_back) b.store(0, std::memory_order_relaxed);
}

void append_history(const uint16_t row[kMaxPlayers], bool with_hash, uint32_t hash) {
  AcquireSRWLockExclusive(&g_hist_lock);
  g_hist.insert(g_hist.end(), row, row + kMaxPlayers);
  if (with_hash) g_hash_hist.push_back(hash);
  g_hist_frames.store((uint32_t)(g_hist.size() / kMaxPlayers), std::memory_order_release);
  ReleaseSRWLockExclusive(&g_hist_lock);
}

void clear_history() {
  AcquireSRWLockExclusive(&g_hist_lock);
  g_hist.clear();
  g_hash_hist.clear();
  g_hist_frames.store(0);
  g_replay_desync = false;
  ReleaseSRWLockExclusive(&g_hist_lock);
}

uint32_t fnv(uint32_t h, uint32_t v) {
  for (int i = 0; i < 4; ++i) h = (h ^ ((v >> (i * 8)) & 0xFF)) * 16777619u;
  return h;
}

// The other PC after a session: its own progress (memory and savegame.dat)
// and options again; the session's progress stays as savegame-netplay.dat.
void give_back_file() {
  CopyFileW(g_save_path.c_str(), g_save_shared.c_str(), FALSE);
  if (GetFileAttributesW(g_save_backup.c_str()) != INVALID_FILE_ATTRIBUTES)
    MoveFileExW(g_save_backup.c_str(), g_save_path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void give_back() {
  if (!g_own_kept) return;
  give_back_file();
  int r = g_deserialize ? g_deserialize(g_own_save) : -1;
  game_options::set_play(g_own_options);
  if (g_own_data_kept && g_game_data && *g_game_data)
    std::memcpy(*g_game_data + kGameDataAt, g_own_data, sizeof(g_own_data));
  g_own_data_kept = false;
  g_own_kept = false;
  LOGI("lockstep: our own story progress and options are back (%d); the shared one is savegame-netplay.dat", r);
}

void end_session(const char* why, bool notice = true) {
  int was = g_state.exchange((int)State::Off);
  if (was == (int)State::Off) return;
  if (was == (int)State::Running) g_auto_players = 0, g_auto_said = false;
  if (was == (int)State::Running && notice) g_ended_notice = true;
  g_restore = true;  // (the other PC: its own progress and options at the next main menu)
  LOGW("lockstep: the session ends at frame %u (%s); this game goes on by itself", g_n, why);
}

// Pending → Running: at the main menu, taking buttons. Both PCs begin from
// the same place: cursor on the first row, rand() seeded, options the host's.
void try_begin() {
  Start s;
  bool have_save;
  AcquireSRWLockShared(&g_lock);
  s = g_start;
  have_save = g_have_remote_save;
  ReleaseSRWLockShared(&g_lock);
  bool save_late = s.expect_save && !have_save && GetTickCount() - g_set_ms.load() > kSaveWaitMs;
  static bool save_late_logged = false;
  if (save_late && !save_late_logged) {
    save_late_logged = true;
    LOGW("lockstep: the host's story progress did not come: beginning with ours (the games may part)");
  }
  // A guest's player must have pressed ГОТОВ (not when coming back: the
  // others play already).
  bool said = !s.apply_options || s.catchup || g_player_ready.load() || g_cfg.debug_auto_ready;
  bool steady = main_menu::steady();
  bool here = steady && said && (!s.expect_save || have_save || save_late);
  g_ready_here.store(here, std::memory_order_relaxed);
  Where w = here                                              ? Where::Ready
            : steady && !said                                 ? Where::NotReady
            : steady                                          ? Where::Progress
            : main_menu::on_screen() && native_menu::active() ? Where::Page
                                                              : Where::Elsewhere;
  g_where_here.store((uint8_t)w, std::memory_order_relaxed);
  g_where_ms.store(GetTickCount(), std::memory_order_release);
  // (coming back: the others began long ago; this game begins by itself)
  if (!here || !(s.catchup || g_ready_there.load(std::memory_order_acquire))) return;
  if (s.apply_options) {
    // The other PC plays the host's game: keep its own progress and options
    // (still kept from a session that has not given them back yet).
    g_restore = false;
    if (!g_own_kept) {
      g_own_kept = game_options::get_play(g_own_options) && g_serialize && (g_serialize(g_own_save), true);
      if (g_own_kept && !CopyFileW(g_save_path.c_str(), g_save_backup.c_str(), FALSE) &&
          GetLastError() != ERROR_FILE_NOT_FOUND)
        LOGW("lockstep: savegame.dat could not be copied (error %lu)", GetLastError());
    }
    if (!game_options::set_play(s.options)) LOGW("lockstep: the host's game options could not be applied");
    if (s.have_data && g_game_data && *g_game_data) {
      if (!g_own_data_kept) {
        std::memcpy(g_own_data, *g_game_data + kGameDataAt, sizeof(g_own_data));
        g_own_data_kept = true;
      }
      std::memcpy(*g_game_data + kGameDataAt, s.data, sizeof(s.data));
    } else {
      LOGW("lockstep: the host's game data (pads, effects) did not come: the games may part");
    }
    if (s.expect_save && have_save && g_deserialize) {
      uint8_t img[tmnp::kSaveImage];
      AcquireSRWLockShared(&g_lock);
      std::memcpy(img, g_remote_save, sizeof(img));
      ReleaseSRWLockShared(&g_lock);
      int r = g_deserialize(img);
      LOGI("lockstep: playing with the host's story progress (%s); ours is kept", r == 0 ? "ok" : "refused");
    }
  }
  main_menu::reset_cursor();
  g_srand(s.seed);
  g_n = 0;
  for (uint16_t& p : g_prev) p = 0;
  for (uint16_t& p : g_row_prev) p = 0;
  g_pause_by = -1;
  g_heard = false;
  g_desync_logged = false;
  g_stalls = g_stall_ms = 0;
  g_stat_at = GetTickCount();
  for (HashAt& h : g_local_hash) h = HashAt{};
  clear_history();
  g_now.store(0);
  g_catchup_ms = s.catchup ? GetTickCount() : 0;
  g_exact_loads = true;
  int pending = (int)State::Pending;
  if (!g_state.compare_exchange_strong(pending, (int)State::Running)) return;  // (ended meanwhile)
  LOGI("lockstep: session %08X begins at the main menu: player %d of %d, delay %d frames, seed %u", s.session, s.me + 1,
       s.players, s.delay, s.seed);
  if (g_cfg.debug_frame_log) {  // (lab) the game data past the save image, at the start
    static uintptr_t getter = patch::find("effects_flags", "A1 ?? ?? ?? ?? 8B 80 54 11 00 00 C3");
    uint8_t* data = getter ? **reinterpret_cast<uint8_t***>(getter + 1) : nullptr;
    if (data) {
      char hex[0x70 * 3 + 1] = "";
      for (int i = 0; i < 0x70; ++i) snprintf(hex + i * 3, 4, "%02X ", data[0x10E8 + i]);
      LOGI("lockstep lab: game data +0x10E8: %s", hex);
    }
  }
  g_players_now = s.players;
  if (!s.catchup)  // (coming into a running game: the screen's cover says so, overlay.cpp)
    native_menu::notify(s.players >= 4 ? "ИГРА ВЧЕТВЕРОМ НАЧАЛАСЬ" : s.players == 3 ? "ИГРА ВТРОЁМ НАЧАЛАСЬ"
                                                                                  : "ИГРА ВДВОЁМ НАЧАЛАСЬ");
}

// The host's НАЧАТЬ ИГРУ goes to every game as a word in the host's buttons
// of one frame: bit 9, a slot of the key table no key of the game's has. The
// buttons of a frame are the same on every PC, so every game hears it in the
// same frame (hkMasks: g_story_cmd) and its main menu then starts the story by
// the game's own way (main_menu.cpp: the choice sent to the menu screen). No
// buttons are pressed for it.
void publish_local(const Start& s, uint16_t local) {
  uint32_t f = g_n + (uint32_t)s.delay;
  local &= (uint16_t)~kStoryWord;  // (only the start below says it)
  if (int n = g_auto_players.load()) {
    // Until the character select is up this player's own buttons do not go
    // out: they would cross the start (0.13.0: a decide of the host's then
    // took 1 ИГРОК).
    local = 0;
    if (!g_auto_said && s.me == 0) {
      g_auto_said = true;
      g_auto_from = g_n;
      local = kStoryWord;
      LOGI("lockstep: НАЧАТЬ ИГРУ: the story for %d, said to every game with frame %u", n, f);
    }
    const bool there = csel::on_screen();
    if (there || g_n - g_auto_from > kStoryFrames) {
      g_auto_players = 0;
      g_auto_said = false;
      if (there)
        LOGI("lockstep: НАЧАТЬ ИГРУ: the character select for %d is up (frame %u)", n, g_n);
      else
        LOGW("lockstep: НАЧАТЬ ИГРУ: no character select came (frame %u)", g_n);
    }
  }
  if (g_catchup.load()) {
    // Coming back: our player's buttons before the return are the host's
    // (none since it left); ours from the return on (a frame missed before it
    // arrived: none, so nobody waits for it).
    uint32_t r = g_return.load();
    if (!r || f + 1 < r) return;
    for (uint32_t m = r - 1; m < f; ++m)
      if (!has(s.me, m)) put(s.me, m, 0);
  }
  put(s.me, f, local);
  tmnp::LsInputPacket p{};
  p.session = s.session;
  p.frame = f;
  p.player = (uint8_t)s.me;
  int count = 0;
  for (; count < tmnp::kLsHistory && f >= (uint32_t)count + (uint32_t)s.delay; ++count) {
    uint32_t g = f - (uint32_t)count;
    p.buttons[count] = has(s.me, g) ? g_ring[s.me].buttons[g % kRing] : 0;
  }
  p.count = (uint8_t)(count ? count : 1);
  AcquireSRWLockExclusive(&g_lock);
  g_out = p;
  ++g_out_version;
  ReleaseSRWLockExclusive(&g_lock);
}

// Everybody's buttons for frame g_n (frames before the delay are empty).
bool wait_all(const Start& s) {
  auto ready = [&] {
    if (g_n < (uint32_t)s.delay) return true;
    for (int p = 0; p < s.players; ++p)
      if (!has(p, g_n)) return false;
    return true;
  };
  if (ready()) return true;
  int late = 0;  // whose buttons are not here
  for (int p = 0; p < s.players; ++p)
    if (!has(p, g_n)) {
      late = p;
      break;
    }
  DWORD t0 = GetTickCount(), peek = t0;
  struct Waits {  // (for the watchdog: this stand-still is a wait)
    Waits() { g_waits_for_buttons = true; }
    ~Waits() { g_waits_for_buttons = false; }
  } waits;
  for (;;) {
    if (g_state.load() != (int)State::Running) return false;
    DWORD now = GetTickCount();
    if (now - t0 > (g_heard ? kStallEndMs : kFirstWaitMs)) {
      end_session(g_heard ? "the other PC stopped sending" : "the other PC never began");
      return false;
    }
    if (now - peek >= 100) {  // the window stays "responding"
      MSG m;
      PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE);
      peek = now;
    }
    Sleep(now - t0 < 2 ? 0 : 1);
    if (ready()) {
      const DWORD took = GetTickCount() - t0;
      ++g_stalls;
      g_stall_ms += took;
      g_stall_max = std::max<uint32_t>(g_stall_max, took);
      if (took >= 1500) {  // (that game stood still: its window away, its PC busy, the way to it)
        LOGW("lockstep: frame %u waited %u ms for player %d's buttons", g_n, (unsigned)took, late + 1);
        native_menu::notify("ЖДАЛИ ИГРОКА", std::to_string(late + 1) + ": " + std::to_string((took + 500) / 1000) + " С");
      }
      return true;
    }
  }
}

void blank_devices() {
  uint8_t* n = g_dev_head ? *g_dev_head : nullptr;
  for (int i = 0; n && i < 16; ++i, n = *reinterpret_cast<uint8_t**>(n + 0x1C)) {
    int32_t size = *reinterpret_cast<int32_t*>(n + 0x0C);
    uint8_t* states[2] = {*reinterpret_cast<uint8_t**>(n + 0x10), *reinterpret_cast<uint8_t**>(n + 0x14)};
    if (size <= 0 || size > 0x1000) continue;
    for (uint8_t* st : states) {
      if (!st) continue;
      std::memset(st, 0, size);
      if (size >= kJoyState) std::memset(st + kJoyPov, 0xFF, 16);  // hats centred
    }
  }
}

void check_hashes() {
  AcquireSRWLockShared(&g_lock);
  for (const HashAt& l : g_local_hash)
    for (const HashAt& r : g_remote_hash)
      if (l.frame != 0xFFFFFFFF && l.frame == r.frame && l.hash != r.hash && !g_desync_logged) {
        g_desync_logged = true;
        LOGE("lockstep: DESYNC at frame %u (here %08X, there %08X): the games are no longer the same", l.frame,
             l.hash, r.hash);
      }
  ReleaseSRWLockShared(&g_lock);
}

// After the game's own masks: everybody's buttons of this frame instead.
void __cdecl hkMasks() {
  oMasks();
  if (g_state.load() == (int)State::Pending) try_begin();
  if (g_state.load() != (int)State::Running) return;
  Start s;
  AcquireSRWLockShared(&g_lock);
  s = g_start;
  ReleaseSRWLockShared(&g_lock);
  uint16_t local = (uint16_t)g_masks[0];  // this PC's own player 1: its keys, gamepad, mouse
  if (g_esc_down.load(std::memory_order_relaxed)) local |= kPauseSlot;  // (whatever the key table says of slot 8)
  publish_local(s, local);
  if (!wait_all(s)) return;
  if (g_n >= (uint32_t)s.delay) {
    for (int p = 0; p < s.players; ++p)
      if (p != s.me && has(p, g_n)) g_heard = true;
  }
  uint32_t h = fnv(2166136261u, hold_rand());
  uint16_t row[kMaxPlayers] = {};
  for (int p = 0; p < kGamePlayers; ++p) {
    uint16_t held = (p < s.players && g_n >= (uint32_t)s.delay) ? buttons_at(p, g_n) : 0;
    row[p] = held;
    h = fnv(h, held);
    // The host's word (НАЧАТЬ ИГРУ) is not a button of the game's.
    if (p == 0 && (held & kStoryWord)) g_story_cmd = g_n + 1;
    held &= (uint16_t)~kStoryWord;
    g_masks[p * 3 + 0] = held;
    g_masks[p * 3 + 1] = held & ~g_prev[p];
    g_masks[p * 3 + 2] = g_prev[p] & ~held;
    g_prev[p] = held;
    std::memset(g_sticks + p * 4, 0x80, 4);  // directions come with the buttons
  }
  // Players 3 and 4 (a game of three or four PCs): their pad slots are
  // four_players.cpp's, filled right after these masks (0x4a984d).
  for (int p = kGamePlayers; p < kMaxPlayers; ++p) {
    uint16_t held = (p < s.players && g_n >= (uint32_t)s.delay) ? buttons_at(p, g_n) : 0;
    row[p] = held;
    four::set_buttons(p, held & (uint16_t)~kStoryWord);
    if (p < s.players) h = fnv(h, held);
  }
  g_pause_by = -1;
  for (int p = 0; p < kMaxPlayers; ++p) {
    const uint16_t now = p < s.players ? row[p] : 0;
    if (g_pause_by < 0 && (now & ~g_row_prev[p] & kPauseSlot)) g_pause_by = p;
    g_row_prev[p] = now;
  }
  append_history(row, g_n % kHashEvery == 0, h);
  blank_devices();
  if (g_n % kHashEvery == 0) {
    AcquireSRWLockExclusive(&g_lock);
    g_local_hash[(g_n / kHashEvery) % 8] = HashAt{g_n, h};
    g_hash_out.session = s.session;
    g_hash_out.frame = g_n;
    g_hash_out.hash = h;
    g_hash_new = true;
    ReleaseSRWLockExclusive(&g_lock);
    check_hashes();
  }
  if (g_log) write_frame((int)g_n);
  g_now.store(g_n, std::memory_order_relaxed);
  ++g_n;
  if (g_catchup.load()) {
    uint32_t r = g_return.load();
    if (r && g_n + 1 >= r) {  // our player is ours again from here
      g_catchup = false;
      LOGI("lockstep: back in the game at frame %u (caught up in %u s)", g_n, (GetTickCount() - g_catchup_ms) / 1000);
      native_menu::notify("СНОВА В ИГРЕ");
    }  // (meanwhile the screen says so: overlay.cpp's cover)
  }
  DWORD now = GetTickCount();
  if (now - g_stat_at >= 10000) {
    LOGI("lockstep: frame %u, waited for the other PC %u times, %u ms in all, %u ms at the longest (10 s)", g_n,
         g_stalls, g_stall_ms, g_stall_max);
    g_stalls = g_stall_ms = g_stall_max = 0;
    g_stat_at = now;
  }
}

}  // namespace

void install(const Config& cfg) {
  g_cfg = cfg;
  if (cfg.debug_multi_instance) allow_instances();
  g_light = cfg.debug_light;
  frame_wait();
  uintptr_t rand = patch::find("crt_rand", kRandSig);
  uintptr_t srand = patch::find("crt_srand", kSrandSig);
  uintptr_t masks = patch::find("player_masks", kMasksSig);
  if (!rand || !srand || !masks) {
    LOGW("lockstep: rand()/srand() or the player masks not found: no game on every PC");
    return;
  }
  g_ptd = reinterpret_cast<Ptd_t>(rand + 5 + patch::read<int32_t>(rand + 1));
  g_srand = reinterpret_cast<Srand_t>(srand);
  uintptr_t m = patch::read<uint32_t>(masks + 20);
  if (!patch::in_image(m - kSticksBack, kSticksBack + kGamePlayers * kMaskStride)) {
    LOGW("lockstep: the player masks look different: no game on every PC");
    return;
  }
  g_masks = reinterpret_cast<uint32_t*>(m);
  g_sticks = reinterpret_cast<uint8_t*>(m - kSticksBack);
  if (uintptr_t a = patch::find("task_list", kTaskListSig)) {
    uintptr_t head = patch::read<uint32_t>(a + 3);
    if (patch::in_image(head, 4)) g_task_head = reinterpret_cast<uint8_t**>(head);
  }
  if (uintptr_t a = patch::find("input_devices", kDevicesSig)) {
    uintptr_t head = patch::read<uint32_t>(a + 3);
    if (patch::in_image(head, 4)) g_dev_head = reinterpret_cast<uint8_t**>(head);
  }
  if (uintptr_t a = patch::find("main_loop_running", kRunningSig))
    patch::hook("main_loop_running", a, (void*)&hkRunning, (void**)&oRunning);
  if (uintptr_t a = patch::find("frame_skip", kSkipSig)) patch::hook("frame_skip", a, (void*)&hkSkip, (void**)&oSkip);
  if (uintptr_t a = patch::find("save_serialize", kSerializeSig)) g_serialize = reinterpret_cast<Image_t>(a);
  // The game data's getter of its effects flags (0x410b6b: mov eax, [data]; mov eax, [eax + 0x1154]).
  if (uintptr_t a = patch::find("game_data", "A1 ?? ?? ?? ?? 8B 80 54 11 00 00 C3")) {
    uintptr_t p = patch::read<uint32_t>(a + 1);
    if (patch::in_image(p, 4)) g_game_data = reinterpret_cast<uint8_t**>(p);
  }
  if (uintptr_t a = patch::find("save_deserialize", kDeserializeSig)) g_deserialize = reinterpret_cast<Image_t>(a);
  std::wstring dir = module_dir(nullptr);
  g_save_path = dir + L"savegame.dat";
  g_save_backup = dir + L"savegame.dat.netplay";
  g_save_shared = dir + L"savegame-netplay.dat";
  // A session on this PC ended without giving our progress back (the game
  // closed or crashed): our savegame.dat first, before the game loads it.
  if (GetFileAttributesW(g_save_backup.c_str()) != INVALID_FILE_ATTRIBUTES) {
    CopyFileW(g_save_path.c_str(), g_save_shared.c_str(), FALSE);
    if (MoveFileExW(g_save_backup.c_str(), g_save_path.c_str(), MOVEFILE_REPLACE_EXISTING))
      LOGI("lockstep: our own savegame.dat is back from the last game together (savegame.dat.netplay)");
  }
  sync_loads();
  patch::hook("player_masks", masks, (void*)&hkMasks, (void**)&oMasks);
  if (cfg.debug_sync_menu || cfg.debug_frame_log) {
    if (cfg.debug_rand_trace) patch::hook("crt_rand", rand, (void*)&hkRand, (void**)&oRand);
    if (cfg.debug_frame_log) {
      std::wstring path = module_dir(nullptr) + L"tmntnet-frames.log";
      g_log = _wfopen(path.c_str(), L"w");
    }
    LOGI("lockstep lab: %s, seed %u, %u frame keys%s", cfg.debug_sync_menu ? "probe (no network)" : "session log",
         cfg.debug_seed, (unsigned)cfg.debug_frame_keys.size(), g_log ? ", frame log" : "");
  }
}

void on_keyboard(uint8_t* keys) {
  if (!g_srand) return;
  if (!g_game_thread) g_game_thread = GetCurrentThreadId();
  if (g_cfg.debug_stall_ms > 0) {  // (lab: a game that stands still, for the watchdog)
    static int polls = 0;
    if (++polls == g_cfg.debug_stall_at) Sleep((DWORD)g_cfg.debug_stall_ms);
  }
  if (int p = g_back_notice.exchange(0)) native_menu::notify("СНОВА В ИГРЕ", "ИГРОК " + std::to_string(p));
  if (g_cfg.debug_task_log) log_tasks();
  finish_bank();
  if (g_ended_notice.exchange(false)) {
    int n = g_players_now;
    native_menu::notify(n >= 4 ? "ИГРА ВЧЕТВЕРОМ ЗАКОНЧЕНА" : n == 3 ? "ИГРА ВТРОЁМ ЗАКОНЧЕНА" : "ИГРА ВДВОЁМ ЗАКОНЧЕНА");
    four::set_buttons(2, 0);  // (players 3 and 4 let go of what the session held)
    four::set_buttons(3, 0);
  }
  if (g_restore.load() && g_state.load() == (int)State::Off && main_menu::on_screen()) {
    g_restore = false;
    give_back();
  }
  if (g_state.load() == (int)State::Off && g_serialize && g_options_at % 60 == 1 && main_menu::on_screen()) {
    uint8_t img[tmnp::kSaveImage] = {};
    g_serialize(img);
    AcquireSRWLockExclusive(&g_lock);
    std::memcpy(g_save, img, sizeof(img));
    g_save_ok = img[0] == 3;
    ReleaseSRWLockExclusive(&g_lock);
  }
  if (++g_options_at % 60 == 1) {
    uint8_t o[24];
    if (game_options::get_play(o)) {
      AcquireSRWLockExclusive(&g_lock);
      std::memcpy(g_options, o, sizeof(o));
      ReleaseSRWLockExclusive(&g_lock);
    }
    if (g_game_data && *g_game_data && g_state.load() != (int)State::Running && !g_own_data_kept) {
      AcquireSRWLockExclusive(&g_lock);
      std::memcpy(g_data, *g_game_data + kGameDataAt, sizeof(g_data));
      g_data_ok = true;
      ReleaseSRWLockExclusive(&g_lock);
    }
  }
  int state = g_state.load();
  // [debug] frame_keys: this PC's keys by frame, the session's or the probe's.
  int frame = state == (int)State::Running ? (int)g_n : g_probe_synced ? g_probe_frame + 1 : -1;
  if (frame >= 0)
    for (const Config::FrameKey& k : g_cfg.debug_frame_keys)
      if (frame >= k.frame && frame < k.frame + k.frames) keys[k.dik] = 0x80;
  if (frame > 0 && frame == g_cfg.debug_stage_over_at) four::debug_stage_over();  // (lab: on to the next level)
  g_esc_down.store((keys[0x01] & 0x80) != 0, std::memory_order_relaxed);
  if (!g_cfg.debug_sync_menu || state != (int)State::Off) return;
  // The probe: the main menu's first frame is the common start.
  if (!g_probe_synced) {
    g_calls = 0;
    if (main_menu::updates() == 0) return;
    g_probe_synced = true;
    g_exact_loads = true;
    g_probe_frame = 0;
    g_srand(g_cfg.debug_seed);
    LOGI("lockstep probe: common start (frame 0), rand() seeded with %u", g_cfg.debug_seed);
    write_frame(0);
    return;
  }
  write_frame(++g_probe_frame);
}

// --- network side ---------------------------------------------------------------

void net_start(const Start& s) {
  if (!g_srand || !g_masks) return;
  if (g_state.load() != (int)State::Off) end_session("a new session starts");
  clear_rings();
  g_now.store(0);
  g_catchup = s.catchup;
  g_have = 0;
  g_return = 0;
  AcquireSRWLockExclusive(&g_lock);
  g_start = s;
  g_start.players = s.players < 1 ? 1 : s.players > kMaxPlayers ? kMaxPlayers : s.players;
  g_out_version = 0;
  g_hash_new = false;
  g_have_remote_save = false;
  for (HashAt& h : g_remote_hash) h = HashAt{};
  ReleaseSRWLockExclusive(&g_lock);
  g_session = s.session;
  g_set_ms = GetTickCount();
  g_ready_here = g_ready_there = false;
  g_where_here = (uint8_t)Where::Unknown;
  g_where_there = (uint8_t)Where::Unknown;
  g_there_player = 0;
  g_where_ms = GetTickCount();
  g_state = (int)State::Pending;
  // (after the state: the game thread takes the flag and looks at the session)
  if (!s.catchup && s.apply_options) g_close_pages = true;  // (a guest's: the host's stay, to invite more)
  LOGI("lockstep: session %08X set (player %d of %d, delay %d): %s", s.session, s.me + 1, s.players, s.delay,
       s.catchup ? "coming back: it begins at the main menu and catches up" : "begins at the main menu");
}

void net_end(const char* why) { end_session(why); }

void net_set_again(const char* why) { end_session(why, false); }

void net_leave(const tmnp::LsLeavePacket& p) {
  if (g_state.load() == (int)State::Off || p.session != g_session.load() || p.player >= kMaxPlayers) return;
  int me;
  AcquireSRWLockShared(&g_lock);
  me = g_start.me;
  ReleaseSRWLockShared(&g_lock);
  if (p.player == me) return;
  for (int i = 0; i < p.count && i < tmnp::kLsHistory && p.frame >= (uint32_t)i + 1; ++i)
    if (!has(p.player, p.frame - 1 - i) && fits(p.frame - 1 - i)) put(p.player, p.frame - 1 - i, p.buttons[i]);
  g_back[p.player].store(0, std::memory_order_release);
  g_gone[p.player].store(p.frame + 1, std::memory_order_release);
  LOGW("lockstep: player %d left; its buttons are none from frame %u on", p.player + 1, p.frame);
}

int net_history(int player, uint32_t frame, uint16_t* out, int max) {
  if (player < 0 || player >= kMaxPlayers) return 0;
  int n = 0;
  for (; n < max && frame >= (uint32_t)n + 1; ++n) {
    uint32_t f = frame - 1 - n;
    if (!has(player, f)) break;
    out[n] = buttons_at(player, f);
  }
  return n;
}

bool net_active() { return g_state.load() != (int)State::Off; }

void net_input(const tmnp::LsInputPacket& p) {
  if (g_state.load() == (int)State::Off || p.session != g_session.load() || p.player >= kMaxPlayers) return;
  int me;
  AcquireSRWLockShared(&g_lock);
  me = g_start.me;
  ReleaseSRWLockShared(&g_lock);
  if (p.player == me) return;
  for (int i = 0; i < p.count && i < tmnp::kLsHistory && p.frame >= (uint32_t)i; ++i)
    if (!has(p.player, p.frame - i) && fits(p.frame - i)) put(p.player, p.frame - i, p.buttons[i]);
}

uint32_t history_frames() { return g_hist_frames.load(std::memory_order_acquire); }

int history(uint32_t from, int max, int players, uint16_t* out) {
  AcquireSRWLockShared(&g_hist_lock);
  uint32_t n = (uint32_t)(g_hist.size() / kMaxPlayers);
  int c = 0;
  for (; c < max && from + (uint32_t)c < n; ++c)
    for (int p = 0; p < players && p < kMaxPlayers; ++p) out[c * players + p] = g_hist[(from + c) * kMaxPlayers + p];
  ReleaseSRWLockShared(&g_hist_lock);
  return c;
}

void net_history_in(const tmnp::LsHistoryPacket& p) {
  if (g_state.load() == (int)State::Off || p.session != g_session.load() || !g_catchup.load()) return;
  int players = std::min<int>(p.players, kMaxPlayers);
  uint32_t have = g_have.load();
  if (p.from > have) return;  // (they come in order: one missing would leave a hole)
  int c = 0;
  for (; c < p.count && c < tmnp::kHistoryFrames; ++c) {
    uint32_t f = p.from + (uint32_t)c;
    if (!fits(f)) break;
    for (int q = 0; q < players; ++q)
      if (!has(q, f)) put(q, f, p.buttons[c * p.players + q]);
  }
  if (p.from + (uint32_t)c > have) g_have.store(p.from + (uint32_t)c);
}

bool net_catchup_out(tmnp::LsCatchupPacket& out) {
  if (!g_catchup.load() || g_state.load() == (int)State::Off) return false;
  out.session = g_session.load();
  out.frame = g_now.load(std::memory_order_relaxed);
  out.have = g_have.load();
  return true;
}

void net_return(const tmnp::LsReturnPacket& p) {
  if (g_state.load() == (int)State::Off || p.session != g_session.load() || p.player >= kMaxPlayers) return;
  int me;
  AcquireSRWLockShared(&g_lock);
  me = g_start.me;
  ReleaseSRWLockShared(&g_lock);
  if (p.player == me) {
    if (g_catchup.load()) g_return.store(p.frame + 1);
    LOGI("lockstep: our player is ours again from frame %u", p.frame);
    return;
  }
  g_back[p.player].store(p.frame + 1, std::memory_order_release);
  g_back_notice = p.player + 1;
  LOGI("lockstep: player %d is back from frame %u", p.player + 1, p.frame);
}

bool catching_up() {
  return g_catchup.load(std::memory_order_relaxed) && g_state.load() == (int)State::Running;
}

int catchup_percent() {
  if (!g_catchup.load(std::memory_order_relaxed) || g_state.load() != (int)State::Running) return -1;
  const uint32_t have = g_have.load(std::memory_order_relaxed);
  return have ? (int)std::min<uint64_t>(100, (uint64_t)g_now.load(std::memory_order_relaxed) * 100 / have) : 0;
}

bool fast() {
  return g_catchup.load(std::memory_order_relaxed) && g_state.load() == (int)State::Running &&
         g_now.load(std::memory_order_relaxed) + 30 < g_have.load(std::memory_order_relaxed);
}

void net_hash(const tmnp::LsHashPacket& p) {
  // (not begun here yet: the history and the hashes kept are the last
  // session's; a game set again at the main menu got a false "DESYNC at frame
  // 0" from the others' first hash)
  if (g_state.load() != (int)State::Running || p.session != g_session.load()) return;
  // A frame this game has played: its hash is kept (a PC coming back sends
  // the hashes of the frames it plays again, long after the others).
  if (p.frame % kHashEvery == 0 && p.frame < g_hist_frames.load()) {
    AcquireSRWLockShared(&g_hist_lock);
    size_t i = p.frame / kHashEvery;
    bool differs = i < g_hash_hist.size() && g_hash_hist[i] != p.hash;
    uint32_t mine = i < g_hash_hist.size() ? g_hash_hist[i] : 0;
    ReleaseSRWLockShared(&g_hist_lock);
    if (differs && !g_replay_desync) {
      g_replay_desync = true;
      LOGE("lockstep: DESYNC at frame %u (here %08X, there %08X): a game is no longer the same", p.frame, mine, p.hash);
    }
    return;
  }
  AcquireSRWLockExclusive(&g_lock);
  g_remote_hash[(p.frame / kHashEvery) % 8] = HashAt{p.frame, p.hash};
  ReleaseSRWLockExclusive(&g_lock);
}

bool net_outgoing(tmnp::LsInputPacket& out, uint32_t& version) {
  if (g_state.load() != (int)State::Running) return false;
  AcquireSRWLockShared(&g_lock);
  bool any = g_out_version != 0;
  if (any) out = g_out, version = g_out_version;
  ReleaseSRWLockShared(&g_lock);
  return any;
}

bool net_take_hash(tmnp::LsHashPacket& out) {
  AcquireSRWLockExclusive(&g_lock);
  bool any = g_hash_new;
  if (any) out = g_hash_out, g_hash_new = false;
  ReleaseSRWLockExclusive(&g_lock);
  return any;
}

bool borrowed() { return g_own_kept || g_state.load() != (int)State::Off; }

void on_exit() {
  end_session("the game is closing");
  give_back();  // before the game writes savesetting.dat on its way out
}

void on_window_gone() {
  // Normally given back already (on_exit); this late only the file is safe.
  if (!g_own_kept) return;
  give_back_file();
  g_own_kept = false;
  LOGW("lockstep: our own savegame.dat is back (the game closed in a game together)");
}

bool host_data(uint8_t out[tmnp::kGameData]) {
  AcquireSRWLockShared(&g_lock);
  bool ok = g_data_ok;
  if (ok) std::memcpy(out, g_data, tmnp::kGameData);
  ReleaseSRWLockShared(&g_lock);
  return ok;
}

bool host_save(uint8_t out[tmnp::kSaveImage]) {
  AcquireSRWLockShared(&g_lock);
  bool ok = g_save_ok;
  if (ok) std::memcpy(out, g_save, tmnp::kSaveImage);
  ReleaseSRWLockShared(&g_lock);
  return ok;
}

void net_save(const tmnp::LsSavePacket& p) {
  if (g_state.load() == (int)State::Off || p.session != g_session.load() || p.size != tmnp::kSaveImage) return;
  AcquireSRWLockExclusive(&g_lock);
  std::memcpy(g_remote_save, p.data, tmnp::kSaveImage);
  g_have_remote_save = true;
  ReleaseSRWLockExclusive(&g_lock);
  LOGI("lockstep: the host's story progress arrived");
}

bool net_ready_out(tmnp::LsReadyPacket& out) {
  int st = g_state.load();
  if (st == (int)State::Off) return false;
  out.session = g_session.load();
  // A game that has not run a frame for a while (its window away) cannot
  // begin, whatever it said last.
  bool fresh = GetTickCount() - g_where_ms.load(std::memory_order_acquire) <= kWhereStaleMs;
  bool running = st == (int)State::Running;
  out.ready = running || (fresh && g_ready_here.load(std::memory_order_relaxed)) ? 1 : 0;
  out.where = running ? (uint8_t)Where::Ready : fresh ? g_where_here.load() : (uint8_t)Where::Paused;
  out.flags = g_player_ready.load() || g_cfg.debug_auto_ready ? 1 : 0;  // (the host's own: net_host.cpp)
  return true;
}

void net_ready(const tmnp::LsReadyPacket& p) {
  if (g_state.load() == (int)State::Off || p.session != g_session.load()) return;
  g_where_there.store(p.where, std::memory_order_relaxed);
  g_there_player.store(p.player, std::memory_order_relaxed);
  g_ready_mask.store(p.flags, std::memory_order_relaxed);
  g_ready_there.store(p.ready != 0, std::memory_order_release);
}

void host_options(uint8_t out[24]) {
  AcquireSRWLockShared(&g_lock);
  std::memcpy(out, g_options, sizeof(g_options));
  ReleaseSRWLockShared(&g_lock);
}

State state() { return (State)g_state.load(); }

uint32_t frame() { return g_now.load(std::memory_order_relaxed); }

Waiting waiting() {
  Waiting w;
  w.pending = g_state.load() == (int)State::Pending;
  if (!w.pending) return w;
  w.here = (Where)g_where_here.load(std::memory_order_relaxed);
  w.there = (Where)g_where_there.load(std::memory_order_relaxed);
  w.there_player = g_there_player.load(std::memory_order_relaxed);
  w.since_ms = g_set_ms.load();
  return w;
}

bool take_close_pages() { return g_close_pages.exchange(false); }

void set_player_ready(bool ready) {
  if (g_player_ready.exchange(ready) != ready) LOGI("lockstep: this player %s", ready ? "is ГОТОВ" : "is not ГОТОВ");
}
bool player_ready() { return g_player_ready.load() || g_cfg.debug_auto_ready; }  // (the lab: by itself)

bool is_guest() {
  if (g_state.load() == (int)State::Off) return false;
  AcquireSRWLockShared(&g_lock);
  bool guest = g_start.apply_options;
  ReleaseSRWLockShared(&g_lock);
  return guest;
}

int my_player() {
  if (g_state.load() == (int)State::Off) return -1;
  AcquireSRWLockShared(&g_lock);
  int me = g_start.me;
  ReleaseSRWLockShared(&g_lock);
  return me;
}

uint8_t ready_mask() { return g_ready_mask.load(); }

int players() {
  if (g_state.load() != (int)State::Running) return 0;
  AcquireSRWLockShared(&g_lock);
  int n = g_start.players;
  ReleaseSRWLockShared(&g_lock);
  return n;
}

int pause_asked_by() { return g_state.load() == (int)State::Running ? g_pause_by : -1; }

bool waiting_for_buttons() { return g_waits_for_buttons.load(); }
unsigned long game_thread() { return g_game_thread; }

bool starting_story() { return g_auto_players.load() != 0 && g_state.load() == (int)State::Running; }

bool take_story_command() {
  const uint32_t at = g_story_cmd;
  if (!at) return false;
  g_story_cmd = 0;
  return g_state.load() == (int)State::Running && g_n - (at - 1) <= kStoryFrames;
}

void host_autostart(int players) {
  if (players < 2) {  // (not any more)
    g_auto_players = 0;
    return;
  }
  if (!main_menu::starts_story()) {  // (the game's menu is not as known: the host goes into the story itself)
    LOGW("lockstep: НАЧАТЬ ИГРУ: the story cannot be started from here; the games are brought together only");
    return;
  }
  g_auto_said = false;
  g_auto_players = std::clamp(players, 2, 4);
  LOGI("lockstep: НАЧАТЬ ИГРУ: once the game together begins, the story for %d", players);
}

}  // namespace lockstep
