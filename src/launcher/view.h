#pragma once
// What the launcher's window shows (View) and how it is drawn (paint): night
// roofs, ink and halftone, 960 x 600 units. The window's logic fills a View;
// paint() knows nothing of networks or files.
#include <d2d1.h>

#include <cstdint>
#include <string>
#include <vector>

namespace view {

enum Id {
  kNone = 0,
  kSettings, kMinimize, kClose,
  kPrimary, kSecondary, kCancel, kPick,
  kPaperLink, kRepoLink,
  kBack, kSwitchCheck, kSwitchAuto,
  kProxyNone, kProxySystem, kProxyOwn, kProxyEdit,
  kPickSettings, kOpenLog, kUninstall,
  kSwatch0, kSwatch1, kSwatch2, kSwatch3,
  kTab0, kTab1, kTab2, kTab3,  // the settings' pages: the launcher's own, the picture, the sound, the keys
  kSwitchSelf, kReset,
  kPlayer1, kPlayer2,          // УПРАВЛЕНИЕ: whose keys are shown
  kLayoutPrev, kLayoutNext,    //             the ready layouts
  kKey0, kKeyLast = kKey0 + 11,  //           the twelve actions' keys
  // The game's settings: a row's controls are kRowFirst + its number * kRowParts + a part.
  kRowFirst = 100,
};
constexpr int kRowParts = 16;
enum RowPart { kPartPrev = 0, kPartNext = 1, kPartSwitch = 2, kPartPiece = 3 };  // kPartPiece + i: a level's block, a pair's button

enum class Icon { None, Download, Play, Folder, Retry };
enum class Mark { None, Ok, Warn, Bad, Busy };  // the line under the versions

struct Step {
  std::wstring label;
  int state = 0;  // 0 to do, 1 now, 2 done
};

// A row of the game's settings (the ГРАФИКА and ЗВУК pages).
enum class RowKind { Choice, Switch, Level, Pair };  // a value between two arrows; on / off; five blocks; two buttons
struct Row {
  int number = 0;  // (its controls' ids)
  RowKind kind = RowKind::Choice;
  std::wstring label, value;          // Choice: the value's words
  bool on = false;                    // Switch
  int level = 0;                      // Level: 0..4
  std::wstring options[2];            // Pair
  int chosen = 0;
  bool usable = true;                 // false: drawn pale, takes no clicks
};
struct Group {
  std::wstring name, note;
  std::vector<Row> rows;
};
// An action of the game's key table and its key (the УПРАВЛЕНИЕ page).
struct KeyCell {
  std::wstring label, key;
  bool waiting = false;  // the next key pressed becomes its key
};

struct View {
  uint32_t accent = 0xF7941D;
  bool settings = false;

  // The two versions.
  bool tickets = false;
  std::wstring installed, installed_note, latest, latest_note;
  std::wstring latest_label = L"НА GITHUB";  // or В ЛАУНЧЕРЕ: the version the launcher carries
  bool latest_bright = false;  // framed in the accent
  bool is_new = false;         // the НОВАЯ tag
  bool equal = false;          // "=" between them instead of the arrow
  bool ahead = false;          // the installed one is the newer: nothing between them
  Mark mark = Mark::None;
  std::wstring mark_text;

  bool no_game = false;

  // A job under way.
  bool job = false;
  Step steps[3];
  int percent = 0;
  std::wstring stage, source;
  float flow = 0.0f;  // the stripes' travel
  bool can_cancel = true;

  // The buttons under them.
  std::wstring primary;
  Icon icon = Icon::None;
  bool primary_on = true;
  std::wstring secondary;
  std::wstring hint;
  std::wstring game_path;

  // The sheet on the right.
  std::wstring paper_tab, paper_meta, paper_title, paper_link;
  std::vector<std::wstring> paper_items;

  // The bottom line.
  std::wstring net;
  uint32_t net_color = 0x8BE836;
  std::wstring repo;  // github.com/owner/name

  // A job's last line: what it will do and what it will not touch.
  std::wstring job_note;

  // Settings: the page shown, and the game's settings on pages 1 and 2.
  int tab = 0;
  Group groups[2];
  std::wstring page_note;   // under the groups
  bool page_warn = false;   // the note says why nothing can be changed
  bool page_bright = false; // the note says what to do now (a key is waited for)
  bool can_reset = false;
  // Page 3: the keyboard keys of one player at a time.
  int keys_player = 0;
  KeyCell keys[12];         // a column at a time: moving, fighting, the rest
  std::wstring layout;      // the ready layout's name
  bool keys_usable = false;
  bool check_at_start = true, auto_install = false, auto_self = true;
  int proxy = 1;  // 0 none, 1 Windows', 2 own
  std::wstring proxy_address;
  bool draw_address = true;  // false: an edit box of the window covers the field
  std::wstring uninstall_label;
  bool can_uninstall = false;

  // The pointer and the keyboard.
  int hover = kNone, down = kNone, focus = kNone;
};

struct Hit {
  int id;
  D2D1_RECT_F rect;
};

extern const uint32_t kAccents[4];  // blue, red, purple, orange
constexpr float kWidth = 960.0f, kHeight = 600.0f, kCaption = 48.0f;

// Draws the window; the places that take clicks go to `hits`.
void paint(ID2D1RenderTarget* rt, const View& v, std::vector<Hit>& hits);

}  // namespace view
