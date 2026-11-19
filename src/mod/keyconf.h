// The game's key table (ПАРАМЕТРЫ УПРАВЛЕНИЯ → КЛАВИАТУРА) with mouse
// buttons: they are keys there. While a cell waits for a key, a click puts
// that button in the cell instead of a key ("ЛКМ", "ПКМ", "СКМ", "М4", "М5"),
// and the game binds, swaps, saves and resets it like any key (codes
// binds::kMouseKey.., pressed by dinput_hook.cpp). A button added to a key
// through [mouse] in tmntnet.ini is shown after the key's name ("F +ЛКМ");
// the table only takes those off (a button set as a key replaces its "+",
// Базовые Установки clears them), OK writes [mouse].
// Also the game's names of R, A, G, B and DOWN, which the Russian font drew in
// Cyrillic ("ГОЦН"). See re/notes.md §17. Game thread.
#pragma once
#include "common/config.h"

namespace keyconf {

void install(const Config& cfg);  // InitializeASI

}  // namespace keyconf
