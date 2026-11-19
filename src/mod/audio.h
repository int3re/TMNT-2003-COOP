// Sound: the volume of the game's movies (CRI Sofdec: the game never gives
// them its own sound settings, the intro plays at full volume, louder than the
// music even at its loudest), skipping the intro, and sound that keeps playing
// in the background (borderless/windowed with [video] background=run).
#pragma once
#include "common/config.h"

namespace audio {

void install(const Config& cfg);  // InitializeASI, before the game creates DirectSound

// Settings window (game thread): movies follow the game's music volume, and
// then this much of that (0..100 %).
// Every sound buffer of the game's at no volume while set (a PC catching up
// with a game plays its frames many times as fast), and back as the game
// asked when cleared. Any thread.
void set_silent(bool on);

void set_movie_follow(bool on);
void set_movie_volume(int percent);

// РОЛИКИ in the game's ОПЦИИ ЗВУКА: the movies' own level 0..4 like the game's
// sliders, -1 = the music's level. movie_level() is the level as shown (the
// music's while it follows), 0..4.
void set_movie_level(int level);
int movie_level();
bool movie_level_follows();

// True until the intro is over: the main menu has not been shown yet.
bool in_intro();

}  // namespace audio
