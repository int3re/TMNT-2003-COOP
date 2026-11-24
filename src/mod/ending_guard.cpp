#include "mod/ending_guard.h"

#include <cstdint>
#include <vector>

#include "common/log.h"
#include "mod/patch.h"

namespace ending_guard {
namespace {

// TMNT.EXE bc49a82c...; each pattern has one match in .text (re/signatures.md,
// re/notes.md §38).
//
// The story's flow starts one of two ending scenes (0x49a86c / 0x49a87b):
// - the turtles' (make 0x49ac3e, step 0x49ad5d, destroy 0x49a88a): two
//   cutscenes on a stage of its own, the staff roll (the third cutscene made
//   and waiting), that cutscene, the last picture;
// - the one with a movie first (make 0x49abab, step 0x49b174, destroy
//   0x49a8a5): the movie, the staff roll, the last picture.
// A scene's data (0x2C bytes, not cleared when made):
//   +0x00 the state (the same numbers in both)
//   +0x08 the 2D view of the roll or of the last picture (0x4acfc9)
//   +0x0C, +0x10, +0x14 the three cutscenes' tasks (0x44b540)      turtles' only
//   +0x18 the staff roll's task (0x49b855)
//   +0x1C the stage behind the cutscenes (make 0x49a8b3)           turtles' only
//   +0x20 the last picture's task (0x49abf2)
// States 0xD..0xF have the roll and its view (0xF ends them: 0x49b3b9(roll),
// 0x4acff8(view)), 0x15 and 0x16 the last picture and its view (0x16 ends the
// stage, the view and the picture, in that order). The turtles' make clears
// +0x0C..+0x1C; +8 and +0x20 (and +0x18 of the other) hold what the memory had
// until their state.
constexpr const char* kDestroySig[2] = {
    "56 E8 ?? ?? ?? ?? 8B 74 24 08 83 7E 28 00 74 09 E8 ?? ?? ?? ?? 83 66 28 00 5E C3",  // 0x49a88a
    "E8 ?? ?? ?? ?? 6A 01 E8 ?? ?? ?? ?? 59 C3"};                                       // 0x49a8a5
constexpr const char* kDestroyName[2] = {"ending_destroy", "ending_movie_destroy"};
// 0x49b3b9(task): the staff roll is over: its lines draw no more ([0x5e6e80] =
// 0) and leave at 448 a frame, the roll's task is ended.
constexpr const char* kRollStopSig = "83 25 ?? ?? ?? ?? 00 C7 05 ?? ?? ?? ?? 00 00 E0 43 E9 ?? ?? ?? ??";
// 0x49b5e9: the step of a line of the roll (a task each, made by 0x49b740;
// its draw 0x49b681 sets the game's font up for a line of text).
constexpr const char* kLineStepSig =
    "55 8B EC 83 EC 10 53 8B 5D 08 D9 43 40 D8 1D ?? ?? ?? ?? DF E0 F6 C4 41 75 0C E8 ?? ?? ?? ?? 50 E8";
// 0x44f4e3: the step of a cutscene's actor (the tasks a cutscene's script
// makes when it starts: models, the camera, a fade...; data +0x18 the
// cutscene's task). A cutscene's destroy (0x44ef3e) only sends them a message;
// they end themselves a frame later, and some then ask for things of the scene
// that made the cutscene (the fade's, 0x44ecb8, for the scene's fade task).
constexpr const char* kActorStepSig =
    "55 8B EC 83 EC 0C 56 E8 ?? ?? ?? ?? 8D 4D FC 51 8D 4D F8 51 50 89 45 F4 E8 ?? ?? ?? ?? 8B 75 08 83 C4 0C 85 C0 74 7C 53";
// 0x4ac4b5(task): ends a task (at once outside the tasks' passes: its
// "destroy" is called, it leaves the lists).
constexpr const char* kEndTaskSig =
    "E8 ?? ?? ?? ?? 85 C0 74 11 6A 00 6A 00 FF 74 24 0C E8 ?? ?? ?? ?? 83 C4 0C C3 56 8B 74 24 08 E8 ?? ?? ?? ?? 5E C3";
// 0x4acff8(view): lets a 2D view of 0x4acfc9 go.
constexpr const char* kViewFreeSig = "56 8B 74 24 08 85 F6 74 32 57 6A 00 E8 ?? ?? ?? ?? 6A 00 6A 00 E8 ?? ?? ?? ?? 56 E8";
// The list of all tasks (as lockstep.cpp: 0x4aca5f walks it; a node: +0 the
// task, +8 the next node; a task: +0x1C its step). +3 = &head.
constexpr const char* kTaskListSig = "56 8B 35 ?? ?? ?? ?? EB 36 8B 06 83 78 10 03 74 2B";

constexpr size_t kState = 0x00, kView = 0x08, kCutscene[3] = {0x0C, 0x10, 0x14}, kRoll = 0x18, kStage = 0x1C,
                 kLast = 0x20;
constexpr int32_t kRollFrom = 0xD, kRollTo = 0xF, kLastFrom = 0x15, kLastTo = 0x16;
constexpr size_t kTaskStep = 0x1C, kTaskData = 0x34, kActorOwner = 0x18;

using Destroy_t = void(__cdecl*)(uint8_t* scene);
using Task_t = void(__cdecl*)(uint8_t* task);

Destroy_t oDestroy[2] = {nullptr, nullptr};
Task_t g_roll_stop = nullptr, g_end_task = nullptr, g_view_free = nullptr;
uintptr_t g_line_step = 0, g_actor_step = 0;
uint8_t** g_task_head = nullptr;

// The game's tasks now; `step` not 0: those with that step only.
std::vector<uint8_t*> tasks(uintptr_t step) {
  std::vector<uint8_t*> out;
  uint8_t* node = *g_task_head;
  for (int i = 0; node && i < 4096; ++i, node = *reinterpret_cast<uint8_t**>(node + 8)) {
    uint8_t* task = *reinterpret_cast<uint8_t**>(node);
    if (task && (!step || *reinterpret_cast<uintptr_t*>(task + kTaskStep) == step)) out.push_back(task);
  }
  return out;
}

bool alive(const uint8_t* task) {
  if (!task) return false;
  for (const uint8_t* t : tasks(0))
    if (t == task) return true;
  return false;
}

// Ends each of `list` that is still a task (one's destroy may have ended
// another); how many.
int end_all(const std::vector<uint8_t*>& list) {
  int n = 0;
  for (uint8_t* task : list)
    if (alive(task)) {
      g_end_task(task);
      ++n;
    }
  return n;
}

// The actors of a cutscene, in the order the tasks' pass would end them.
std::vector<uint8_t*> actors_of(const uint8_t* cutscene) {
  std::vector<uint8_t*> out;
  for (uint8_t* task : tasks(g_actor_step)) {
    const uint8_t* data = *reinterpret_cast<uint8_t**>(task + kTaskData);
    if (data && *reinterpret_cast<const uint8_t* const*>(data + kActorOwner) == cutscene) out.push_back(task);
  }
  return out;
}

// What the scene still has is ended, as its own states end it, and all of it
// here: while the scene's own things (its fade task, the game's font) are
// there. At the scene's own end nothing is left and nothing is done.
void take_down(uint8_t* scene, bool turtles) {
  auto field = [scene](size_t at) -> uint8_t*& { return *reinterpret_cast<uint8_t**>(scene + at); };
  const int32_t state = *reinterpret_cast<int32_t*>(scene + kState);
  const bool roll = state >= kRollFrom && state <= kRollTo && alive(field(kRoll));
  const bool last = state >= kLastFrom && state <= kLastTo && alive(field(kLast));
  int lines = 0, cutscenes = 0, actors = 0;
  bool stage = false;

  if (roll) {
    g_roll_stop(field(kRoll));
    // Its lines would leave by themselves in a few frames; the game's way out
    // has only a few. They are found by their step.
    lines = end_all(tasks(g_line_step));
  }
  if (turtles) {
    for (size_t at : kCutscene)
      if (alive(field(at))) {
        // The cutscene, then its actors: the game's order, without the frame
        // between them.
        const std::vector<uint8_t*> its = actors_of(field(at));
        g_end_task(field(at));
        actors += end_all(its);
        field(at) = nullptr;
        ++cutscenes;
      }
    stage = alive(field(kStage));
    if (stage) g_end_task(field(kStage));
    field(kStage) = nullptr;
  }
  const bool view = (roll || last) && field(kView);
  if (view) g_view_free(field(kView));
  if (last) g_end_task(field(kLast));
  if (roll || last) field(kView) = field(kRoll) = field(kLast) = nullptr;

  if (roll || last || cutscenes || stage)
    LOGI("ending: the scene is ended in state 0x%X, before its own end (the game is closing or leaves the story): "
         "ended with it:%s%s%s%s lines of the roll: %d, cutscenes: %d, their actors: %d",
         state, roll ? " the staff roll," : "", stage ? " the stage behind it," : "", last ? " the last picture," : "",
         view ? " their view," : "", lines, cutscenes, actors);
}

void __cdecl hkDestroyTurtles(uint8_t* scene) {
  if (scene) take_down(scene, true);
  oDestroy[0](scene);
}

void __cdecl hkDestroyMovie(uint8_t* scene) {
  if (scene) take_down(scene, false);
  oDestroy[1](scene);
}

}  // namespace

void install(const Config& cfg) {
  if (!cfg.debug_ending_guard) {
    LOGI("ending: [debug] ending_guard=0: the ending's tasks are left as the game leaves them");
    return;
  }
  uintptr_t destroy[2];
  for (int i = 0; i < 2; ++i) destroy[i] = patch::find(kDestroyName[i], kDestroySig[i]);
  const uintptr_t stop = patch::find("staff_roll_stop", kRollStopSig), line = patch::find("staff_roll_line_step", kLineStepSig),
                  actor = patch::find("cutscene_actor_step", kActorStepSig), end = patch::find("task_end", kEndTaskSig),
                  view = patch::find("view_2d_free", kViewFreeSig), list = patch::find("task_list", kTaskListSig);
  const uintptr_t head = list ? patch::read<uint32_t>(list + 3) : 0;
  if (!destroy[0] || !destroy[1] || !stop || !line || !actor || !end || !view || !head || !patch::in_image(head, 4)) {
    LOGW("ending: the game's ending scenes are not where expected; closing the window during the ending may crash the game");
    return;
  }
  g_roll_stop = reinterpret_cast<Task_t>(stop);
  g_end_task = reinterpret_cast<Task_t>(end);
  g_view_free = reinterpret_cast<Task_t>(view);
  g_line_step = line;
  g_actor_step = actor;
  g_task_head = reinterpret_cast<uint8_t**>(head);
  const bool ok = patch::hook(kDestroyName[0], destroy[0], (void*)&hkDestroyTurtles, (void**)&oDestroy[0]) &&
                  patch::hook(kDestroyName[1], destroy[1], (void*)&hkDestroyMovie, (void**)&oDestroy[1]);
  LOGI("ending: a scene of the ending that is ended before its own end takes its tasks with it%s", ok ? "" : ": not in place");
}

}  // namespace ending_guard
