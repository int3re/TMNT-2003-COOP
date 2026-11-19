// tmntnet's own pages in the look of the game's ПАРАМЕТРЫ pages, laid out
// with the game's own numbers (re/notes.md §16): the options background and
// turtles, a title and rows written with the game's letters (game_text),
// values between the game's green arrows, the pulsing hexagon cursor and OK.
// Driven by the same buttons as the game's menus (the pad slots: up/down,
// left/right, decide, cancel) with the game's sounds and fades. Game thread
// only (notices: any thread).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common/config.h"

struct IDirect3DDevice9;

namespace native_menu {

constexpr int kNoDefault = -1000;

struct Row {
  std::string label;                       // in the game's letters (upper case)
  std::function<std::string()> label_fn;   // a label that changes ("" = not shown)
  std::string plain_label;                 // a label in the plain font (names)
  std::vector<std::string> values;         // choices shown with arrows; empty = no value
  std::function<int()> get;                // the current choice
  std::function<void(int)> set;            // left/right picked a new choice
  std::function<void()> action;            // decide on the row
  std::function<std::string()> text;       // a live value in the plain font (codes, names)
  bool selectable = true;
  bool note = false;  // the label in cyan, like ОПЦИИ ПО-УМОЛЧАНИЮ: a note, not a setting
  std::function<std::string()> hint;       // what the row does: a line at the bottom while it is selected (plain font)
  std::function<bool()> shown;             // false: not drawn and not selectable now (the rows under it move up)
  std::function<unsigned()> label_rgb;     // the label's colour (default white; a row not available now: dim)
  std::function<unsigned()> text_rgb;      // the colour of `text`
  int slider = 0;                          // > 0: a volume slider of this many steps (get/set 0..slider-1)
  std::function<int()> raw;                // the value cancel puts back with set() (default: get)
  int default_value = kNoDefault;          // what ПО УМОЛЧАНИЮ sets with set()
};

// A list beside the rows, at the top right of a page: the players of a game
// together. A title and a card a player (a bar leaning like the game's
// letters, lit in the player's state colour): its number in a hexagon, the
// name (plain font), its state (the game's letters, coloured) and an icon.
enum { kIconNone = 0, kIconTick, kIconDots, kIconStar };
struct PanelLine {
  std::string number;       // plain font, in the line's hexagon: "2"
  std::string name;         // plain font
  std::string detail;       // plain font, small, after the name: "13 MS"
  std::string state;        // the game's letters, at the right
  unsigned rgb = 0xFFFFFF;  // the line's colour: its state, its edge, its hexagon
  int icon = kIconNone;     // after the state: a tick (done), dots that run (waited for), a star (leads)
  bool own = false;         // this PC's player: lit
  bool free = false;        // nobody there: dim
  bool pulse = false;       // its edge pulses (something is waited for)
};
struct Panel {
  std::string title;        // the game's letters
  std::string count;        // plain font, right of the title: "2 / 4"
  std::vector<PanelLine> lines;
};
// What happens now: a line at the bottom of a page, over the selected row's hint.
struct Status {
  std::string letters;      // the game's letters
  unsigned rgb = 0xFFFFFF;
};

struct Page {
  std::string title;
  std::vector<Row> rows;                   // up to 8 (7 with hints: OK stays over the bottom lines)
  std::function<void()> on_close;          // OK / cancel left the page
  std::function<Panel()> panel;            // the list at the top right (rows then have no values)
  std::function<Status()> status;
  std::string ok_hint;                     // the hint while the cursor is on OK
  int first = 0;                           // the row the cursor starts on
};

void install(const Config& cfg);

// What the pages are shown over. MainMenu: they bring the options background
// and the screen fades through black, as when the game goes to ПАРАМЕТРЫ.
// Options: the game's ПАРАМЕТРЫ screen is behind and stays; only the words
// change, as between its own pages. Set before the first page opens;
// on_closed runs when the last page starts to leave.
enum class Host { MainMenu, Options };
void set_host(Host host, std::function<void()> on_closed = nullptr);

void open(Page page);    // over the open page (or the first one)
bool active();           // a page or its fade is up: the buttons are the pages'
bool covers_screen();    // MainMenu: a page hides the menu completely
void reset();            // the screen behind is gone: drop everything
void close_all();        // MainMenu: every page goes, under the black, as by cancel
void back();             // the top page goes, as by cancel (from a row's action)
// MainMenu: this page in the place of every page open (they fade into it, no
// black between). False when the pages are on their way in or out: again later.
bool replace(Page page);

// The host's update: the buttons pressed this frame (both pad slots, the
// game's logical bits). Returns true when the pages used them.
bool update(uint32_t pressed);

// The host's draw, after the game's: the pages.
void draw();

void set_play_sound(int (*play)(int id));

// tmntnet's rows inside one of the game's own pages (options_pages.cpp): drawn
// like the page's rows at the given layout y (label, value, arrows on the
// selected row) with the page's alpha, while the game's own cursor moves over
// them. One set per page id. Game thread.
// value_x: the page's column of values; letters_x: where its rows' letters start.
void embed(int page, std::vector<Row> rows, std::vector<float> ys, float value_x = 126.0f, float letters_x = -184.5f);
void embed_select(int page, int row);                   // the game's cursor is on our row (-1: elsewhere)
bool embed_input(int page, int row, uint32_t pressed);  // left/right/decide on our row; true = used
void embed_draw(int page, int alpha);                   // inside the page's draw, after the game's

// A picture of the game's letters as a texture (for the game's own sprites),
// with its size in texels; null if those letters are not available.
struct Picture {
  void* texture = nullptr;  // IDirect3DTexture9*
  int w = 0, h = 0, text_w = 0;
};
Picture letters_picture(const std::string& text);

// A short notice at the top of the screen for a few seconds: words in the
// game's letters and a value in its plain font (codes, names). Any thread.
void notify(const std::string& letters, const std::string& value = "");
// A small line at the top right during play (P2's name and ping), "" = none.
void set_badge(const std::string& plain);
// A line that stays at the top, above the notices, until changed ("" = none):
// what a game together waits for. Any thread.
void set_status(const std::string& letters, const std::string& value = "");
// A choice shown in the middle of the screen, over the character select's
// middle panel (the costume): a title and a count, the value between two
// arrows, hints. Shown while it is set again every frame (gone
// 150 ms after the last time). Any thread.
struct Prompt {
  std::string title;        // the game's letters
  std::string count;        // plain font, at the right: "2 / 6"
  std::string value;        // plain font, big
  std::string hint;         // plain font, small; '|' between its lines
  unsigned rgb = 0xFFFFFF;  // the value's and the plate's colour
  bool arrows = true;       // the two arrows beside the value (a choice)
  bool alone = false;       // nothing else of the notices is drawn with it (the cover of a PC catching up)
};
void show_prompt(const Prompt& p);
// Present, inside the overlay's scene: the notices and the badge.
void draw_notices(IDirect3DDevice9* dev);
bool has_notices();

// Name tags over the players' turtles (nametags.cpp): a name in the game's
// plain font on a dark plate whose bottom centre is at (x, y), back buffer
// pixels. Present, inside the overlay's scene.
struct Tag {
  std::string text;  // UTF-8
  float x = 0, y = 0;
  float size = 20;   // the letters' height, pixels
  int alpha = 255;
  unsigned rgb = 0xFFFFFF;  // the letters' colour
};
void draw_tags(IDirect3DDevice9* dev, const std::vector<Tag>& tags);

// Pieces of the play screen's panels (four_hud.cpp), laid out like the
// game's: its 512x480 space (x right, y down, 0,0 the middle), placed and
// squeezed as [video] hud says. A part (u0..u1, v0..v1) of a picture of
// gameinfopc.txd, centred at (x, y), w x h units. Present, inside the
// overlay's scene.
struct HudSprite {
  const char* picture;
  float u0, v0, u1, v1;
  float x, y, w, h;
  int alpha = 255;
  uint32_t rgb = 0xFFFFFF;  // the picture's colours times this
  int edge = 0;             // -1 / 1: a corner piece, at that edge of a wide screen ([video] hud_corners)
};
void draw_hud(IDirect3DDevice9* dev, const std::vector<HudSprite>& sprites);
// The same at back buffer pixels (centre x, y; w x h), a plain colour where
// picture is null: marks over the world (revive.cpp).
void draw_px(IDirect3DDevice9* dev, const std::vector<HudSprite>& sprites);
// Five-pointed stars at back buffer pixels, outlined dark (revive.cpp's stunned).
struct Star {
  float x = 0, y = 0, r = 10;  // centre, the points' radius
  float turn = 0;              // radians
  int alpha = 255;
  uint32_t rgb = 0xFFE040;
};
void draw_stars(IDirect3DDevice9* dev, const std::vector<Star>& stars);

// Text for the plain font, which only has Latin letters (Russian written out).
std::string latin(const std::string& utf8);

}  // namespace native_menu
