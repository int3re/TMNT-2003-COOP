// tmntnet's in-game screens (drawn by overlay.cpp with Dear ImGui): the
// label of the native main menu item, tmntnet's window (F2) with the network
// page and the settings pages, a P2 status badge and short notifications.
#pragma once
#include <string>

#include "common/config.h"

struct ImFont;

namespace ui {

// More pages for tmntnet's window (settings_ui.cpp), after the network page
// (page 0). Returns the page's number. Game thread.
int add_tab(const char* name, void (*draw)());

// For the pages: a section heading like the network page's, and its font.
void section(const char* title);
ImFont* bold_font();

// Hooks the main menu task so the entry shows up there. Call from InitializeASI.
void install(const Config& cfg);

void setup_style();  // once, with the ImGui context current
void frame();        // build this frame's UI (render thread)
void toggle();               // open/close tmntnet's window (on the last page)
void open_page(int page);     // open it on this page (0 = network)
bool wants_input();          // the window is open and has focus
void claim_escape();         // a page used Esc this frame: the window stays open
void toast(const std::string& text);  // any thread

// Previews only (tests/ui_preview.cpp): pretend the main menu is on screen.
void debug_force_main_menu(bool on);

}  // namespace ui
