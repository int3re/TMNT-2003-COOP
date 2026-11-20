#pragma once
// The game's settings on the launcher's ГРАФИКА, ЗВУК and УПРАВЛЕНИЕ pages: the
// mod's picture and movie settings of tmntnet.ini ([video], [audio]), the
// game's own sound levels and the keyboard keys of players 1 and 2 of
// savesetting.dat. The rows are those of the game's ПАРАМЕТРЫ ГРАФИКИ, ОПЦИИ
// ЗВУКА and its key table with tmntnet in it; a change is written at once and
// is read by the game at its next start.
#include <string>

namespace gset {

enum Row {
  kDisplay, kResolution, kWide, kSides, kBackground,  // ГРАФИКА: the screen
  kFov, kHud, kQuality, kFpsLimit, kVsync,            //          the picture
  kMusic, kEffects, kVoices, kMovies,                 // ЗВУК: how loud
  kStereo, kIntro,                                    //       how it sounds
  kRows
};
constexpr int kFirstSound = kMusic;

enum class Kind { Choice, Switch, Level, Pair };  // values with arrows; on / off; 0..4; two buttons

// Reads both files of the game folder (again: the game may have changed them).
void load(const std::wstring& game_dir);
// Values for a picture: no file is read or written.
void load_canned();

bool has_mod();     // tmntnet.ini is there
bool has_levels();  // savesetting.dat is there and whole (the game has run once)

Kind kind(Row r);
const wchar_t* label(Row r);
const char* key(Row r);            // its name for the lab: display, music, ...
bool usable(Row r);                // its file is there
// The row does something with the other rows as they are. Without the wide
// screen the mod leaves the picture to the game (4:3, the resolution of the
// game's own menu): РАЗРЕШЕНИЕ and ПО БОКАМ do nothing; in the exclusive full
// screen the game cannot go on in the background.
bool applies(Row r);
int count(Row r);                  // how many values it has now
int value(Row r);                  // the one set
std::wstring name(Row r, int v);   // a value's words

// Sets a value and writes its file. False: not written, error() says why.
bool set(Row r, int v);
// The mod's and the game's own defaults for the rows of a page (sound: from kFirstSound).
bool reset(bool sound);
const std::wstring& error();
// Where the two files are read and written (the log; a copy Windows keeps for
// the game of a protected folder's files stands in for the folder's own).
std::wstring ini_file();
std::wstring levels_file();

// --- УПРАВЛЕНИЕ: the keyboard keys of players 1 and 2 (savesetting.dat) ---------------
// The twelve actions of the game's key table (ПАРАМЕТРЫ УПРАВЛЕНИЯ → КЛАВИАТУРА):
// up, down, left, right; jump, weak attack, strong attack, shuriken; dash,
// special, air juggling, shuriken change. A key is a DirectInput code;
// kMouseKey + 0..4 are the mouse's buttons, which only tmntnet presses.
constexpr int kKeyActions = 12;
constexpr int kMouseKey = 0xF0, kMouseButtons = 5;

bool has_keys();                                // savesetting.dat is there and whole
const wchar_t* action_label(int action);
const char* action_key(int action);             // its name for the lab: up, jump, attack, ...
int key_code(int player, int action);
std::wstring key_name(int code);                // "Пробел", "Num 4", "ЛКМ"
std::wstring key_text(int player, int action);  // the name, and the mouse buttons tmntnet.ini adds to it ("F +ЛКМ")
bool key_allowed(int code);                     // the game takes it into its table (a mouse button: with the mod)
// Gives the action that key; the action that had it (either player's) takes
// this one's, as in the game's table. False: not written, error() says why.
bool set_key(int player, int action, int code);

// РАСКЛАДКА: 0 the game's own keys, 1 two players at one keyboard, 2 the player's own.
int layout();
bool has_own_layout();  // keys of the player's own are kept from before a ready layout
bool set_layout(int v);

}  // namespace gset
