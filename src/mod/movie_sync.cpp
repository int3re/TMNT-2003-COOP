#include "mod/movie_sync.h"

#include <cstdint>

#include "common/log.h"
#include "mod/lockstep.h"
#include "mod/patch.h"

namespace movie_sync {
namespace {

// The movie task (TMNT.EXE bc49a82c..., re/notes.md §36). Made by
// 0x490740(task) (0xBC bytes; a Sofdec player at +0x28). Its step
// 0x4908c2(task):
// - decide or start held in pad slot 0 or 1 (0x4a9595(p) & 0x10800) stops a
//   movie that may be skipped ([0x61cd7c] & 1), sets +0xAC and
// - with +0xAC == 1 the task is over at once (jmp 0x4ab3e9);
// - else the player's next picture is taken, and when there is none its status
//   is asked (vtable +0x20): 3 (played to the end) or 4 (an error) set +0xAC,
//   for the next step. The task's draw (0x490957) asks the same and sets it too.
// The status follows the PC's clock; the buttons are the session's.
constexpr const char* kStepSig = "53 33 DB 56 43 53 E8 ?? ?? ?? ?? 6A 00 8B F0 E8 ?? ?? ?? ?? 0B F0 59 F7 C6 00 08 01 00";
constexpr const char* kMakeSig = "53 56 57 8B 7C 24 10 68 BC 00 00 00 33 DB 53 57 89 3D";
constexpr size_t kOver = 0xAC;
// A movie that was skipped may leave its PC saying "over" for the frames
// already on their way (the buttons' delay, 12 frames at most): the bits of a
// movie's first frames are not about it.
constexpr uint32_t kGrace = 30;
// Not everybody has said so (a movie that hangs on one PC): the movie is over
// this many frames after the first.
constexpr uint32_t kPatience = 600;

using Step_t = void(__cdecl*)(uint8_t* task);
using Make_t = int(__cdecl*)(uint8_t* task);
Step_t oStep = nullptr;
Make_t oMake = nullptr;

bool g_sync = true;   // [debug] movie_sync (lab: 0 = as before 0.13.8)
uint32_t g_late = 0;  // [debug] movie_late (lab: this PC hears its movie's end that many frames later)

uint8_t* g_task = nullptr;
uint32_t g_steps = 0;       // steps of this movie in the session
uint32_t g_over_at = 0;     // the step + 1 this PC's movie was over at (0: it plays)
uint32_t g_first_at = 0;    // the step + 1 the first player's word came with
bool g_caught_up = false;   // this PC ran through a part of it coming back into the game

void forget() {
  g_task = nullptr;
  g_steps = g_over_at = g_first_at = 0;
  g_caught_up = false;
  lockstep::set_movie_over(false);
}

int __cdecl hkMake(uint8_t* task) {
  forget();  // (a new movie: nothing of the one before)
  return oMake(task);
}

void __cdecl hkStep(uint8_t* task) {
  if (!task || !g_sync || lockstep::state() != lockstep::State::Running) {
    if (g_task) forget();
    // Lab (movie_sync=0 with movie_late=N): the movie ends by this PC's own
    // clock as it did, N frames later than its player said (a PC behind).
    if (task && !g_sync && g_late && lockstep::state() == lockstep::State::Running) {
      static uint32_t held = 0;
      int32_t& over = *reinterpret_cast<int32_t*>(task + kOver);
      if (over != 1) held = 0;
      else if (++held <= g_late) over = 0;
    }
    return oStep(task);
  }
  if (task != g_task) {
    forget();
    g_task = task;
  }
  ++g_steps;
  int32_t& over = *reinterpret_cast<int32_t*>(task + kOver);
  // This PC's movie: over by its player's word. A PC that came back into the
  // game in the middle of it ran through its frames faster than the movie
  // plays: it has nothing to wait for.
  g_caught_up = g_caught_up || lockstep::catching_up();
  if ((over == 1 || g_caught_up) && !g_over_at) {
    g_over_at = g_steps + 1;
    LOGI("movie: this PC's is over at frame %u (step %u)%s", lockstep::frame(), g_steps,
         g_caught_up && over != 1 ? ": this PC came into the game during it" : "");
  }
  over = 0;  // (the session says when)
  lockstep::set_movie_over(g_over_at && g_steps + 1 >= g_over_at + g_late);

  // Every game's: by the words in this frame's buttons.
  const uint32_t said = lockstep::movie_over_mask(), present = lockstep::present_mask();
  if (g_steps > kGrace && present) {
    if ((said & present) && !g_first_at) g_first_at = g_steps + 1;
    const bool all = (said & present) == present;
    const bool long_enough = g_first_at && g_steps + 1 - g_first_at >= kPatience;
    if (all || long_enough) {
      LOGI("movie: over for every game at frame %u (step %u; this PC's own %s)%s", lockstep::frame(), g_steps,
           g_over_at ? "was over" : "still played", all ? "" : ": not every PC said so in ten seconds");
      over = 1;
      forget();
    }
  }
  oStep(task);
}

}  // namespace

void install(const Config& cfg) {
  g_sync = cfg.debug_movie_sync;
  g_late = (uint32_t)cfg.debug_movie_late;
  const uintptr_t step = patch::find("movie_step", kStepSig), make = patch::find("movie_make", kMakeSig);
  if (!step || !make) {
    LOGW("movie: the game's movie task is not where expected; a movie watched to its end may part the games");
    return;
  }
  const bool ok = patch::hook("movie_make", make, (void*)&hkMake, (void**)&oMake) &&
                  patch::hook("movie_step", step, (void*)&hkStep, (void**)&oStep);
  LOGI("movie: in a game together a movie is over for every game in the same frame%s%s",
       ok ? "" : ": not in place", g_sync ? "" : " (off: [debug] movie_sync=0)");
}

}  // namespace movie_sync
