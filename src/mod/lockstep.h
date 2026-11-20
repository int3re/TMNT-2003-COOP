// Netplay v2 (lockstep): every player runs the game on their own PC and the
// games go frame by frame with the same inputs. The game's logic is
// frame-locked (one task pass a loop, 60 a second, drawing skipped when slow;
// re/notes.md §18) and its randomness is the CRT rand() of the main thread,
// reseeded only from itself, so two copies started from the same state with
// the same seed and the same buttons stay the same. What made them drift were
// loads that end after a number of frames that depends on the PC: here every
// file read and sound bank ends within the frame that needs it.
//
// A session: the host sends a start (seed, input delay, its game options);
// each game begins at the main menu (cursor on the first row, rand() seeded,
// frame 0) once both games are there and taking buttons (LsReady). Every frame each PC sends its own player's buttons for frame +
// delay and waits for everybody's buttons of the frame, which then replace the
// game's own per-player masks (so keys, gamepads and binds on each PC do not
// matter). Every 60 frames the PCs compare a hash of the game state.
//
// Also the lab probe ([debug] sync=menu, frame_keys, frame_log) and several
// copies on one PC ([debug] multi_instance, light).
#pragma once
#include <cstdint>

#include "common/config.h"
#include "common/protocol.h"

namespace lockstep {

void install(const Config& cfg);  // InitializeASI, before the game's WinMain
void on_keyboard(uint8_t* keys);  // the game's keyboard poll: once a frame (game thread)

// --- network side (the host's or the peer's network thread) -------------------
struct Start {
  uint32_t session = 0, seed = 0;
  int delay = 2, players = 2, me = 0;
  uint8_t options[24] = {};
  bool apply_options = false;  // the peer takes the host's options
  bool expect_save = false;    // the host's story progress follows (LsSave)
  bool catchup = false;        // coming back into a running game: begin at once, play the LsHistory frames
  bool have_data = false;      // the host's game data past its save image (pads' settings, effects flags)
  uint8_t data[tmnp::kGameData] = {};
};
void net_start(const Start& s);
void net_end(const char* why);  // the link to the other PC is gone
// The host opened its network page in a running game (at the main menu): this
// session stops here without the "ended" notice; a new one with everybody is
// set by the network thread and begins when the page closes.
void net_set_again(const char* why);
bool net_active();              // a session is set (pending or running)
void net_input(const tmnp::LsInputPacket& p);
// A player's PC left a running session: its buttons are none from `frame` on
// (the host decides the frame; the others are told with LsLeave, with its last
// buttons). The host's side: what it has of that player before `frame`.
void net_leave(const tmnp::LsLeavePacket& p);
int net_history(int player, uint32_t frame, uint16_t* out, int max);
void net_hash(const tmnp::LsHashPacket& p);

// Coming back into a running game. Every PC keeps the session's buttons of
// every frame; the host sends them to a PC that comes back (LsHistory), which
// plays the session from frame 0 as fast as it can (no frame wait, few frames
// shown) and reports how far it is (LsCatchup); once it is near, the host
// gives the player back to it from a frame a little ahead (LsReturn: before it
// the player's buttons are none, as since it left).
uint32_t history_frames();  // frames kept (0..n-1)
int history(uint32_t from, int max, int players, uint16_t* out);  // [frame][player]
void net_history_in(const tmnp::LsHistoryPacket& p);
bool net_catchup_out(tmnp::LsCatchupPacket& out);  // this PC is coming back: its progress
void net_return(const tmnp::LsReturnPacket& p);
bool fast();  // coming back and behind: frames as fast as they go (game thread)
// ... and how far it is, 0..100 (-1: not coming back).
int catchup_percent();
// Coming into a running game, until its player is its own again: the screen
// is covered and the sound off meanwhile (overlay.cpp). Any thread.
bool catching_up();
// The newest packet of our own player's buttons (true when there is a session
// and something to send; the network thread sends it again every few ms).
bool net_outgoing(tmnp::LsInputPacket& out, uint32_t& version);
bool net_take_hash(tmnp::LsHashPacket& out);
// While a session waits to begin: whether this game can (true to send now,
// every 100 ms), and the other's answer. Both begin once both can.
bool net_ready_out(tmnp::LsReadyPacket& out);
void net_ready(const tmnp::LsReadyPacket& p);
// The host's game options for its start packet (kept fresh by the game thread).
void host_options(uint8_t out[24]);
// The host's story progress (savegame image) for LsSave; false if none yet.
bool host_save(uint8_t out[tmnp::kSaveImage]);
// The host's game data past the save image (+0x10E8: its pads' settings and
// the effects flags, which the save image leaves out): the other PCs play
// with it too (a flag there adds a second spray of water to every hit, with a
// rand() of its own). False if not found.
bool host_data(uint8_t out[tmnp::kGameData]);
// The other PC: the host's progress, to play with. Its own comes back (memory
// and savegame.dat) at the main menu after the session; a copy of its
// savegame.dat waits as savegame.dat.netplay until then, also over a crash.
void net_save(const tmnp::LsSavePacket& p);
// This game plays with the host's options and progress (until they are given
// back at the main menu after the session): nobody else should change them.
bool borrowed();
// The game is on its way out (the main loop ends, game thread): the session
// ends and the other PC gets its own progress and options back before the
// game saves its settings.
void on_exit();
// WM_DESTROY: too late for the game's memory; only savegame.dat comes back.
void on_window_gone();

// --- for the pages and notices (any thread) ----------------------------------
enum class State { Off, Pending, Running };
State state();
uint32_t frame();

// A session set and not begun yet: where each game is (LsReadyPacket.where; a
// tmntnet before 0.11.10 does not say: Unknown). Ready: at the main menu,
// taking buttons; Page: our pages over it; Elsewhere: playing, another screen;
// Paused: no frames for a while (its window in the background, Alt+Tab from
// full screen); Progress: at the main menu, the host's story progress coming.
// NotReady: at the main menu, its player has not pressed ГОТОВ (0.11.13).
enum class Where : uint8_t { Unknown = 0, Ready = 1, Page = 2, Elsewhere = 3, Paused = 4, Progress = 5, NotReady = 6 };
struct Waiting {
  bool pending = false;
  Where here = Where::Unknown;   // this game
  Where there = Where::Unknown;  // the other's (the host: its first guest that cannot begin)
  int there_player = 0;          // the host: that guest's player (2..4); 0 = not said
  uint32_t since_ms = 0;         // when the session was set (GetTickCount)
};
Waiting waiting();
// The session was just set: our pages over the main menu close, so that this
// game is at its main menu for it (game thread; true once a session).
bool take_close_pages();

// ГОТОВ (the network page): in a friend's game the session begins only once
// this player said so (every guest: the host's game waits for all of them).
// Kept while this PC stays in the friend's game.
void set_player_ready(bool ready);
bool player_ready();
bool is_guest();      // this PC plays in a session set by another's game
int my_player();      // its player (0-based), -1 without a session
// The host's word to the guests: bit p = player p + 1 pressed ГОТОВ (a guest
// reads the host's; the host's own: net_host.cpp).
uint8_t ready_mask();
// The host's НАЧАТЬ ИГРУ: once the session begins, its game says "the story
// now" to every game with its buttons of one frame (a bit no key has), so all
// hear it in the same frame; until the character select is up the host's own
// buttons do not go out.
void host_autostart(int players);
bool starting_story();  // ... and the start is under way (the host's game)
// For the watchdog (dllmain.cpp): the game's thread (0: not known yet), and
// whether it stands in a wait for another PC's buttons now. Any thread.
unsigned long game_thread();
bool waiting_for_buttons();
// The host's word came in with this frame's buttons (once; the same frame on
// every PC): the main menu starts the story for players() (main_menu.cpp).
// Game thread.
bool take_story_command();
// How many PCs play the running session (0: none runs). The story is for
// that many (story_menu.cpp).
int players();
// The pause key (key-table slot 8, Esc) newly down in a player's buttons of
// the frame being played: that player (the first, 0-based), else -1. The same
// on every PC. Game thread (net_pause.cpp).
int pause_asked_by();
// A movie's end (movie_sync.cpp). set_movie_over: this PC's own movie has
// played to the end (said to every game with this PC's buttons from the next
// frame on, until taken back). movie_over_mask: the players (bit p) whose
// buttons of the frame being played say so; present_mask: the players who are
// in the session in that frame. The same on every PC. Game thread.
void set_movie_over(bool over);
uint32_t movie_over_mask();
uint32_t present_mask();

}  // namespace lockstep
