// A movie's end in a game together. The game ends a movie when its Sofdec
// player says "played to the end", which every PC hears by its own clock: a
// movie watched to its end (not skipped) ended some frames apart on the PCs,
// and the games went on apart (0.13.7, a real game of three: DESYNC right
// after the long movie before the second map). Now:
// - each PC says with a bit of its buttons that its own movie is over (they
//   come with the frame on every PC);
// - the movie is over for every game in the frame all the players in the
//   session have said so (a PC with a shorter movie, or none, waits on its
//   last picture), or ten seconds after the first of them;
// - skipping it (decide or start of players 1 and 2) is as it was: buttons.
// re/notes.md §36.
#pragma once
#include "common/config.h"

namespace movie_sync {

void install(const Config& cfg);

}  // namespace movie_sync
