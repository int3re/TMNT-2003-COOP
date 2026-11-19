// tmntnet's pages in the game's own look (native_menu): the network page
// opened by the main menu's СЕТЕВАЯ ИГРА item. Game thread.
#pragma once
#include "common/config.h"

namespace net_pages {

void install(const Config& cfg);
// by_slot: whose decide opened it (0 player 1, 1 player 2: in a game together
// every PC sees it; only the host's own opens its page then).
void open_network(int by_slot = 0);
// In a friend's game: its page (the players, Я ГОТОВ) comes up, in the place
// of the pages open. False: not now (pages on their way in or out), again
// next frame. Game thread.
bool show_guest();

}  // namespace net_pages
