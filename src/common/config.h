// tmntnet.ini, shared by the mod and the client. Missing keys keep defaults.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "common/log.h"
#include "common/protocol.h"

struct Config {
  // [net]
  std::string role = "host";  // host | client
  std::string peer;           // client: host address (empty = LAN discovery)
                              // host: if set, only this address may be P2
  uint16_t port = tmnp::kDefaultPort;
  uint16_t discovery_port = tmnp::kDiscoveryPort;
  int input_delay = 2;    // frames; used by v2 lockstep, ignored by v1
  int timeout_ms = 1000;  // no packets for this long = peer gone
  std::string name;       // shown to the other side; default = Steam name, else computer name
  bool name_set = false;  // [net] name= is in the ini
  // auto: ENet always, plus Steam when it is running. steam: Steam, ENet only
  // as a fallback. enet: never touch Steam.
  std::string transport = "auto";
  std::string lobby_type = "friends";  // host's Steam lobby: friends | public
  uint64_t host_steamid = 0;           // client: join this host directly (SteamID64)
  std::vector<uint64_t> allow_steamid;  // host: also accept these outside the lobby
  // Lab only, transport=mock: Steam stand-in for two players on one PC
  // (steam_mock.cpp). Fake SteamID per slot, localhost UDP, lobbies as files.
  int mock_slot = 1;
  std::string mock_dir;      // shared by all mock players; default %TEMP%	mntnet-mock-steam
  int mock_loss_pct = 0;     // dropped unreliable messages
  int mock_latency_ms = 0;   // one way
  bool mock_lobby = true;    // 0: join a host by SteamID without its lobby (a stranger)
  int mock_member_delay_ms = 0;  // the host learns of our lobby join this much later

  // [log]
  tlog::Level log_level = tlog::Info;

  // [mod] (host side, inside tmnt.exe)
  bool p2_binds_auto = true;  // read P2 keyboard binds from game memory
  int p2_keys[tmnp::kNumSlots];  // DIK code per slot, -1 = none
  // РАСКЛАДКА: СВОЯ, the keys a player set in the game's key table, kept when
  // a preset replaces them ([mod] custom_keys_1 / custom_keys_2).
  bool custom_keys = false;
  int custom_key[2][tmnp::kNumSlots];
  // Story Mode character select: with no remote P2 connected, pick for P2.
  bool p2_lock = true;  // netplay: the host's own P2 keys and P2 gamepad do nothing
  bool p2_autopick = true;
  int p2_autopick_delay_ms = 1500;
  int p2_character = -1;  // menu value to prefer; -1 = a random free turtle, -2 = the first free
  bool autosave = true;   // save the story after every cleared stage (+ the game's АВТОЗАПИСЬ)
  // Players in the story (four_players.cpp): 2, 3 or 4 until the story's menu
  // says otherwise (story_menu.cpp). Players 3 and 4 at this keyboard: a DIK
  // code per slot of the game's key table (like p2_keys), -1 = none.
  int players = 2;
  int extra_keys[2][tmnp::kNumSlots];
  // Their gamepads: -1 = auto (players 3 and 4 take the last gamepads, before
  // players 1 and 2), 0 = none, N = the Nth gamepad in the game's list.
  int extra_pad[2] = {-1, -1};
  // Down, not out (revive.cpp): revive=0 is the game's own death and hearts;
  // revive_button = the key-table slot that raises a stunned turtle.
  bool revive = true;
  int revive_button = 3;  // shuriken_change

  // [video] (inside tmnt.exe, any role)
  bool widescreen = true;
  int res_mode = 0;  // 0 = desktop, 1 = game's own setting, 2 = res_w x res_h
  int res_w = 0, res_h = 0;
  int refresh_hz = 60;  // preferred refresh rate, 0 = desktop's
  bool hud_center = true;          // squeeze 2D HUD into the 4:3 centre
  bool fullscreen_pillarbox = true;  // full-screen pictures/movies keep 4:3
  bool side_bars_blur = true;        // the bars beside them: blurred picture, not black
  bool wide_menus = true;            // the menus' own backgrounds go on to the screen's edges (no bars there)
  bool hud_corners = true;  // with the HUD in the 4:3 middle: the corner pieces at the screen's edges
  int fov = 100;          // the 3D view, % of the game's (above 100 wider; only what is shown)
  bool own_camera = true;  // in a game together the world is drawn around this PC's own turtle (own_camera.cpp)
  int hud_size = 100;     // the play screen's panels at the top (life, score) and hearts, %
  int msaa = 4;           // anti-aliasing samples: 0 off, 2, 4, 8
  int anisotropy = 16;    // anisotropic texture filtering: 0 = the game's, 2..16
  // How the picture reaches the screen (display.cpp).
  std::string display = "borderless";  // fullscreen (exclusive) | borderless | windowed
  int window_w = 0, window_h = 0;      // windowed: client size, 0 = from the render size
  bool window_resize = true;           // windowed: resizable (aspect kept, black bars)
  bool background_run = false;         // borderless/windowed: keep running without focus
  int vsync = -1;                      // -1 = as the game asks, 0 = off, 1 = on
  int fps_limit = 0;                   // 0 = off

  // [audio]
  // [mouse]: mouse buttons added to a player's key for a slot of the game's
  // bind table (left, right, middle, side 1, side 2): whose (1 or 2) and which
  // slot (-1 none); "F +ЛКМ" in the key table. [mouse] player=0 turns these
  // off. (Buttons set in the game's key table are keys of their own.)
  bool mouse_on = true;
  int mouse_player[5] = {1, 1, 1, 1, 1};
  int mouse_slot[5] = {-1, -1, -1, -1, -1};

  bool movie_follow = true;  // movies as loud as the game's music (its sound options)
  int movie_level = -1;      // or a level of their own, 0..4 like the game's sliders (РОЛИКИ)
  int movie_volume = 100;    // and then this % of that
  bool intro_skip = false;   // press Start through the intro to the main menu

  // [ui]
  std::string menu_label = "СЕТЕВАЯ ИГРА";  // the main menu item's text
  bool debug_window = false;  // F2 opens tmntnet's ImGui window (the pages are in the game's look)
  bool nametags = true;       // a game together: each player's name over their turtle

  // [client]
  int pad = -1;  // XInput user index 0..3, -1 = first connected
  bool keyboard = true;
  int send_hz = 120;
  int stick_deadzone = 16000;  // of 32767
  // [pad] / [keys]: per slot, which XInput buttons / virtual keys press it.
  uint32_t pad_map[tmnp::kNumSlots];  // XINPUT_GAMEPAD_* bits, plus kPadLT/kPadRT
  std::vector<int> key_map[tmnp::kNumSlots];

  // [debug] (lab only): press P1 start/decide until the main menu shows; log
  // the 2D primitives of one main menu frame.
  int debug_autostart = 0;  // 1 = to the main menu, 2 = on to the character select
  bool debug_log_2d = false;
  int debug_log_2d_at = 0;  // ms after start: log that frame's 2D instead of a main menu one
  // Steps, ms after the main menu first shows ("csel:" after the character
  // select): "menu:3000" frame capture, "page2:6000" tmntnet's window on page 2
  // (0 = network) and a capture, "preset_two:7000" / "preset_original:" key
  // preset, "press_1F:8000" key 0x1F held 200 ms as if on the keyboard
  // ("cpress_21:1000": the same, timed from the character select).
  std::vector<std::pair<std::string, int>> debug_shots;
  // Lockstep probe (lockstep.cpp): multi_instance=1 lets several copies run at
  // once; sync=menu starts the frame count at the main menu's first frame and
  // seeds the game's rand() with seed=; frame_keys=F:DIK[:N],... holds key DIK
  // (hex) for N frames (6) from frame F after that; frame_log=1 writes every
  // frame's rand() state to tmntnet-frames.log, rand_trace=1 also its callers.
  bool debug_multi_instance = false;
  bool debug_light = false;  // light=1: the frame wait sleeps instead of spinning a core
  bool debug_text_trace = false;  // text_trace=1: every text the game writes with its fonts (once each), to tmntnet.log
  bool debug_hud_trace = false;  // hud_trace=1: the play screen's sprites, once per call site
  int debug_stage = 0;           // stage=N: the level map's choice starts story stage N (lab)
  bool debug_auto_ready = false; // auto_ready=1 (default with autostart): a guest is ГОТОВ by itself (lab)
  bool debug_costume_step = true;  // costume_step=0: no costume step in the character select (lab: old key scripts)
  int debug_costume_default = 0;   // costume_default=N: the costume every player starts on (lab)
  int debug_camera_player = 0;     // camera_player=N: the own camera follows player N, without a session too (lab)
  bool debug_camera_trace = false;  // camera_trace=1: the camera and the players' places in the log twice a second (lab)
  int debug_voice_extra = 0;     // voice_extra=N: in a game together voices end N frames later (lab: a bank waits for them)
  int debug_break_dict = 0;      // break_dict=1: no current texture dictionary at the first main menu (load_guard)
  bool debug_sync_menu = false;
  uint32_t debug_seed = 12345;
  struct FrameKey {
    int frame = 0, dik = 0, frames = 6;
  };
  std::vector<FrameKey> debug_frame_keys;
  bool debug_frame_log = false;
  bool debug_rand_trace = false;
  bool debug_task_log = false;  // task_log=1: every task the game starts or ends, to tmntnet.log
  int debug_stage_over_at = 0;     // stage_over_at=N: at frame N (the session's or the probe's) the stage is over, as if cleared (lab: the next level)
  bool debug_camera_keep = false;  // camera_keep=1: the players' own views are kept over a new scene (lab: the fault of 0.13.4)
  uint32_t debug_session_seed = 0;  // session_seed=N: the host's sessions start rand() with N (lab: a run again)
  bool debug_level_dump = false;  // level_dump=1: the game's statics at every story level's start and end, to files (lab)
  bool debug_profile = false;  // profile=1: where the game's thread is while this PC catches up with a game, sampled (lab)
  bool debug_movie_sync = true;  // movie_sync=0: a movie ends by each PC's own clock, as before 0.13.8 (lab: the fault)
  int debug_movie_late = 0;      // movie_late=N: this PC hears its movie's end N frames later (lab: PCs apart)
  int debug_stall_at = 0;  // stall_at=N, stall_ms=M: the game's thread sleeps M ms at its Nth keyboard poll (lab: the watchdog)
  int debug_stall_ms = 0;
  // actions=1: the players' actions, their life and what changes in their
  // data while it is 0, and who removes a player's turtle, to tmntnet.log
  // (four_players.cpp); shots "down:N" are timed from the first life of 0.
  bool debug_actions = false;
  // Name tags and the level map's second turtle (lab): nametags=A,B shows
  // these names without a game together; tag_height=play,map (world units
  // over the feet); map_offset=x,z puts player 2's map turtle there from
  // player 1's.
  std::vector<std::string> debug_nametags;
  float debug_tag_height[2] = {-1.0f, -1.0f};
  float debug_map_offset[2] = {-1.15f, 0.0f};  // (the map's camera looks along +z: -x is to the right)
  float debug_revive_reach = 0;                 // revive_reach=N: world units (lab; 0 = the mod's)

  bool is_host() const { return role == "host"; }
  bool steam_wanted() const { return transport != "enet"; }
};

// [mouse] action names <-> game slots ("attack" = 6 ...; "" = -1).
int mouse_action_slot(const std::string& name);
const char* mouse_action_name(int slot);
// A [mouse] button's value: "2:attack" (player 2's attack), "" = none.
std::string mouse_button_value(int player, int slot);

// Pseudo-buttons for triggers (XInput reports them as analog values).
constexpr uint32_t kPadLT = 0x10000;
constexpr uint32_t kPadRT = 0x20000;

// Loads <path>; returns false if the file does not exist (defaults are kept).
bool load_config(const std::wstring& path, Config& cfg);

// Sets [section] key=value in tmntnet.ini, keeping the line's own comment and
// every other line untouched (the file is UTF-8 with Russian comments, which
// WritePrivateProfileString would not keep). Adds the key or section if needed.
bool ini_set(const std::wstring& path, const char* section, const char* key, const std::string& value);

// Directory of the given module (nullptr = the exe), with trailing backslash.
std::wstring module_dir(void* module);

// Name parsers (keynames.cpp). Return -1 / 0 on unknown names.
int parse_vk(const std::string& name);      // "A", "SPACE", "NUMPAD4", "0x41"
uint32_t parse_pad_button(const std::string& name);  // "A", "LB", "RT", "START"
int parse_int(const std::string& s, int fallback);   // decimal or 0x hex
// SteamID64 of a user from "7656119...", "steam:..." or a profile link; 0 if none.
uint64_t parse_steamid(const std::string& s);
