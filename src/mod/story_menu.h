// The story's "how many play" menu (1 ИГРОК, 2 ИГРОКА; Splinter and Casey
// when the game has them open): tmntnet adds ТРИ ИГРОКА and ЧЕТЫРЕ ИГРОКА
// under 2 ИГРОКА. They start the story as 2 ИГРОКА does, and players 3 (and
// 4) take the turtles nobody picked (four_players.cpp). Their labels are put
// together from the game's own letters (game_text; it has no 3 or 4).
// re/notes.md §21. Game thread.
#pragma once
#include "common/config.h"

struct IDirect3DTexture9;

namespace story_menu {

void install(const Config& cfg);
// The live story submenu's menu object (with our rows), or null (game thread).
uint8_t* menu_object();
// The story is started without it (main_menu.cpp): an open submenu takes no
// more buttons, as after its own choice.
void stand_by();

// A sprite is being drawn (0x4abab2, hooked by main_menu.cpp): the picture
// for one of tmntnet's items, or null.
IDirect3DTexture9* texture_for(const void* quad);

}  // namespace story_menu
