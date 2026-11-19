// The game's own lettering for tmntnet's labels. The Russian menus of TMNT
// are pictures (texture dictionaries menu_pc.txd / option_pc.txd inside
// TMNT.DAT): white italic letters with a dark outline and a drop shadow.
// The letters are cut out of the game's labels with known text ("АВТОЗАПИСЬ",
// "СТЕРЕО"...) and new words are put together from them, so tmntnet's items
// look like the game's own. Nothing of the game is shipped with the mod: the
// pictures are read from the player's TMNT.DAT. See re/notes.md §15.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace game_text {

// Reads the dictionaries and cuts the letters (a few hundred ms; any thread,
// once). False when TMNT.DAT is missing or not the Russian version.
bool load();
bool ready();

struct Bitmap {
  int w = 0, h = 0;           // h is 64 like the game's labels
  int text_w = 0;             // width of the lettering itself
  std::vector<uint32_t> bgra;  // straight alpha, rows top to bottom
};
// The game's letterings: its option pages' labels (cap height 24 of 64), its
// main menu's items (cap height 21, narrower and heavier letters) and its story
// submenu's (1 ИГРОК, 2 ИГРОКА, КЕЙСИ, СПЛИНТЕР: cap height 39; it has no Ч or
// Ы, these are made of its Н and Р). The main menu's Ь is made of its Ы.
enum class Style { Options, MainMenu, Story };
// Composes UTF-8 text (upper case Russian, digits 1-2, space) in that style,
// the first letter's body `margin` texels from the left (a slanted top reaches
// further left: 16 keeps a Т or Ч whole). False if a letter is missing (the
// main menu's labels have fewer letters: А-Я but Й Л Ф Ц Ч Ш Щ Ъ Э).
bool compose(const std::string& utf8, Bitmap& out, int margin = 4, Style style = Style::Options);

// Letters the game's labels provide (for wording new items).
std::string letters();

// Text in the game's plain font (fonts/font.bmp, the one it writes
// "1920 X 1080 X 32" with), white, 23 texels high: digits, signs, Latin and
// Russian letters, all as capitals (names: "Raph_77", "Донни"); any other
// letter is a "?". UTF-8. The Russian version points A-Z of that font at
// Russian letters, so the Latin ones are taken from where they sit in its
// picture and the Russian ones from where its metrics put them.
bool plain(const std::string& utf8, Bitmap& out);

// Any picture of the game's menu dictionaries (option_pc.txd, menu_pc.txd)
// or of its play screen (gameinfopc.txd), e.g. "op_bg", "op_cursor",
// "g_face_story". Straight alpha BGRA.
bool picture(const char* name, Bitmap& out);
// The rows of such a picture with white in them: its letters' height (0: no
// such picture).
int white_rows(const char* name);

// Pictures of any dictionary in TMNT.DAT (e.g. "chrsel_sl_pc.txd"), read and
// decoded now, not kept; all or nothing. Any thread.
bool pictures_in(const char* dictionary, const std::vector<std::string>& names, std::vector<Bitmap>& out);

}  // namespace game_text
