// Voices and sounds end on the same frame on every PC of a game together.
// The game plays its voices (dialogues) and streamed sounds with CRI ADX
// (eight ADXT handles) and asks every frame whether they still play: the next
// queued voice starts when one ends, a dialogue goes on when all are quiet. A
// voice ends when the sound card has played it, which is wall-clock time: one
// PC a frame later than another, and the games part (re/notes.md §25). In a
// running session the game's own calls (not the library's) hear "playing"
// from the frame it started the voice until its length in samples has passed
// in frames, then "ended". Game thread.
#pragma once
#include "common/config.h"

namespace voice_sync {

void install(const Config& cfg);

}  // namespace voice_sync
