// In a game together only the host (player 1) drives the main menu, the story
// submenu and the level map; each player still picks its own turtle.
#pragma once
#include "common/config.h"

namespace host_menus {
void install(const Config& cfg);
}  // namespace host_menus
