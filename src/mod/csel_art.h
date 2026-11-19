// The character select's pictures for players 3 and 4, made at run time from
// the game's own (chrsel_sl_pc.txd in the player's TMNT.DAT), each with its 2
// (or 1) replaced by a 3 / 4 (2 / 3) drawn with the same stroke, slant and glow:
// the banner "ИГРОК 3/4" at the start of a turn (csel_st_p2; the white neon
// letters kept, the glow blue / orange), the centre plate "ИГРОК 3/4 ПОМОЩЬ"
// (csel_pl2), and the tag of a taken turtle "ИГРОК 2/3" (csel_selct). Same
// sizes as the game's: the tags are drawn instead at draw time (main_menu.cpp's
// sprite hook, Im2D); the banner and the plate have their texture's own pixels
// swapped for the turns of players 3 and 4 and put back after. Nothing of the
// game is shipped.
#pragma once
#include <d3d9.h>

#include <cstdint>
#include <string>

namespace csel_art {

void install();  // starts making them (a thread)
// For the sprite hook (rw_texture = the sprite's RwTexture: +0 raster, +0x10
// name): the picture to draw instead right now, or null. Game thread.
IDirect3DTexture9* instead_of(const uint8_t* rw_texture);
// The game's own "ИГРОК 2" again (the character select: player 2's turn again,
// or all have picked). Game thread.
void restore();

// The players' names in the select (a game together): who took a turtle
// (under its tag, csel.cpp) and who picks now (under the centre plate). A name
// is a picture of the tag's shape: the game's plain letters in the colour of
// the turtle's mask (the one taken; for who picks, the one pointed at, white on
// the plate) with a dark rim. name_next: the next sprite drawn shows that name
// instead of its own picture; name_done: back to normal. Game thread.
void name_next(const std::string& utf8, int character);
void name_done();
// The sprite hook, after a sprite was drawn: under the centre plate (csel_pl1
// / csel_pl2), the name of who picks now. `draw` = the game's sprite draw.
void after_sprite(uint8_t* sprite, void(__cdecl* draw)(uint8_t*));

}  // namespace csel_art
