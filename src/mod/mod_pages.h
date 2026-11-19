// tmntnet's settings as rows of the game's own option pages (option_pages.cpp
// puts them there): ПРОЧИЕ ПАРАМЕТРЫ (игрок 2), ПАРАМЕТРЫ ЗВУКА (ролики,
// заставка), ПАРАМЕТРЫ ГРАФИКИ (окно, широкий формат, кадры, поля). Every
// change is applied at once where the game allows it and written to
// tmntnet.ini; cancel on the page puts the values back, ПО УМОЛЧАНИЮ resets
// them with the game's. Game thread.
#pragma once
#include <vector>

#include "common/config.h"
#include "mod/native_menu.h"

namespace mod_pages {

void install(const Config& cfg);

std::vector<native_menu::Row> other_rows();     // ПРОЧИЕ ПАРАМЕТРЫ
std::vector<native_menu::Row> sound_rows();     // ПАРАМЕТРЫ ЗВУКА: the slider first
std::vector<native_menu::Row> graphics_rows();  // ПАРАМЕТРЫ ГРАФИКИ
std::vector<native_menu::Row> controls_rows();  // ПАРАМЕТРЫ УПРАВЛЕНИЯ (before the key table)
// РАЗРЕШЕНИЕ: the game applied a mode from its list; kept for the next start.
void resolution_chosen(int w, int h);

}  // namespace mod_pages
