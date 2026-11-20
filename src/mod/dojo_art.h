// The dojo's title cards in a game together. The three cards (dojopr_pc.txd:
// dojo_title1..3, 1024 x 1024, one for each dojo) have "Только для Игрока 1"
// drawn into the picture itself; since 0.13.8 everybody takes part in the dojo
// in a game together, so there the card says "ДЛЯ ВСЕЙ КОМАНДЫ" instead: a
// copy of the game's picture with the old line taken out (the ground behind it
// put back) and the new one in the letters of the game's option labels.
// Outside a game together the game's own picture is drawn, as it is.
#pragma once
#include <cstdint>

struct IDirect3DTexture9;

namespace dojo_art {

void install();
// The texture to draw instead of this RenderWare texture (the game's sprite
// draw, main_menu.cpp), or null: not a dojo title, not a game together, or
// the copy is not ready.
IDirect3DTexture9* instead_of(const uint8_t* rw_texture);

}  // namespace dojo_art
