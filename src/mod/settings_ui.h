// tmntnet's settings pages in its in-game window: Screen, Sound, Controls.
// Each change is applied at once where the game allows it and written to
// tmntnet.ini (keys that need a restart say so on the page).
#pragma once
#include "common/config.h"

namespace settings_ui {

void install(const Config& cfg);  // registers the pages with ui.cpp

}  // namespace settings_ui
