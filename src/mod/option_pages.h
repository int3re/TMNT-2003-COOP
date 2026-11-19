// tmntnet's settings inside the game's own option pages: rows added to
// ПРОЧИЕ ПАРАМЕТРЫ (игрок 2), ПАРАМЕТРЫ ЗВУКА (ролики, заставка) and
// ПАРАМЕТРЫ ГРАФИКИ (окно, широкий формат, кадры, поля). The game's own
// cursor, sounds, fades, OK, cancel and ПО УМОЛЧАНИЮ cover them; tmntnet draws
// their words and values like the page's own (native_menu::embed). See
// re/notes.md §16.
#pragma once
#include "common/config.h"

namespace option_pages {

void install(const Config& cfg);  // InitializeASI, after main_menu::install

}  // namespace option_pages
