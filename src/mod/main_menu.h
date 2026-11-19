// A native 9th item in the game's main menu ("NETWORK", between LOAD and
// QUIT): the game's own cursor, navigation, sounds and fades; selecting it
// opens tmntnet's network window. The label text is drawn by ui.cpp at the
// item's position, since the game has no texture for it.
#pragma once
#include <cstdint>

#include "common/config.h"

namespace main_menu {

void install(const Config& cfg);  // InitializeASI, before the menu is built

bool on_screen();  // the main menu task ran during the last few frames
uint32_t updates();  // how many frames the main menu task has run (game thread)
// The menu is up and taking buttons, no page of ours over it (game thread).
bool steady();
// The cursor back on the first row (РЕЖИМ СЮЖЕТА), for a common start (game thread).
bool reset_cursor();
// The buttons taken from the pad slots while a page of ours is open (game
// thread).
uint32_t take_page_buttons();
// The game's two pad slots (0xAC each: +0xC held, +0x10 pressed), or null.
uint8_t* pad_slots();
// One of the menus' sounds (0x263 move, 0x264 decide, 0x265 cancel, 0x266 invalid).
void play_sound(int id);
// The live main menu's menu object (0x40f867's), or null (game thread).
uint8_t* menu_object();
// The story can be started from here by the game's own way (the host's
// НАЧАТЬ ИГРУ: lockstep's story command).
bool starts_story();
bool native();     // the 9th item is patched in (else ui.cpp shows a button)

// Where to draw the label, in the menu's 640x480 space (origin at the centre,
// y up), with the game's fade alpha (0..1) and whether the cursor is on it.
struct Label {
  bool visible = false;
  float x = 0, y = 0;  // left edge, vertical centre
  float alpha = 0;
  bool selected = false;
};
Label label();

// Previews only (tests/ui_preview.cpp): report a fake native item.
void debug_fake(const Label& l);

}  // namespace main_menu
