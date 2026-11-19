// tmntnet-client.exe: the second player's side. Reads a gamepad (SDL2) or the
// keyboard and streams it to the host's tmntnet.asi as player 2 over ENet
// (LAN / IP) or Steam.
//
//   tmntnet-client.exe                     window: Steam and LAN lists, join code, status
//   tmntnet-client.exe <code-ip-steamid>   window, connects right away
//   tmntnet-client.exe --nogui [ip] [--test] [--no-input] [--seconds N]
//                                          console mode (scripts, tests)
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "client/core.h"
#include "client/gui.h"
#include "common/config.h"
#include "common/joincode.h"
#include "common/log.h"
#include "common/protocol.h"
#include "common/steam_boot.h"
#include "common/version.h"

using namespace tmnp;

namespace {

volatile bool g_quit = false;

BOOL WINAPI on_console_ctrl(DWORD) {
  g_quit = true;
  return TRUE;
}

void print_status(const ClientSnapshot& s) {
  char held[96] = "";
  for (int i = 0; i < kNumSlots; ++i) {
    if ((s.buttons & (1u << i)) && kSlotNames[i]) {
      strncat(held, kSlotNames[i], sizeof(held) - strlen(held) - 2);
      strncat(held, " ", sizeof(held) - strlen(held) - 1);
    }
  }
  printf("\r[%s] %s  rtt=%3u ms  P2: %-40.40s", client_state_name(s.state),
         s.state == ClientState::Connected ? s.host_name.c_str() : s.server.c_str(), s.rtt_ms, held);
  fflush(stdout);
}

int run_console(ClientCore& core, uint32_t run_seconds) {
  SetConsoleCtrlHandler(on_console_ctrl, TRUE);
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  if (GetConsoleMode(out, &mode)) SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  printf("Controls: gamepad or keyboard ([keys] in tmntnet.ini). Ctrl+C to quit.\n");
  uint32_t start = net::now_ms();
  bool rejected = false;
  while (!g_quit) {
    if (run_seconds && net::now_ms() - start >= run_seconds * 1000) break;
    Sleep(100);
    ClientSnapshot s = core.snapshot();
    print_status(s);
    if (s.state == ClientState::Rejected) {
      rejected = true;
      break;
    }
  }
  printf("\n");
  core.stop();
  return rejected ? 2 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::wstring dir = module_dir(nullptr);
  Config cfg;
  load_config(dir + L"tmntnet.ini", cfg);

  bool console = false;
  uint32_t run_seconds = 0;
  std::wstring shot_path;  // --screenshot <png>: hidden window, save, exit
  unsigned shot_delay = 1500;
  ClientOptions opt;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--nogui")
      console = true;
    else if (a == "--seconds" && i + 1 < argc)
      run_seconds = (uint32_t)parse_int(argv[++i], 0);
    else if (a == "--test")
      opt.test_pattern = true;  // scripted P2 input, for checks without a gamepad
    else if (a == "--no-input")
      opt.no_input = true;  // always send "nothing pressed"
    else if (a == "--screenshot" && i + 1 < argc)
      shot_path = std::wstring(argv[i + 1], argv[i + 1] + strlen(argv[i + 1])), ++i;
    else if (a == "--screenshot-delay" && i + 1 < argc)
      shot_delay = (unsigned)parse_int(argv[++i], 1500);
    else if (a == "+connect_lobby" && i + 1 < argc)
      opt.join_lobby = steam::parse_lobby_id(argv[++i]);  // what Steam passes for "Join game"
    else if (a[0] != '-' && steam::parse_lobby_id(a))
      opt.join_lobby = steam::parse_lobby_id(a);
    else if (a[0] != '-' && parse_steamid(a))
      cfg.host_steamid = parse_steamid(a);  // SteamID64 or a profile link
    else if (a[0] != '-')
      cfg.peer = a;  // join code, IP or host name
  }
  // A join code on the command line or in peer= becomes a plain address.
  if (!cfg.peer.empty()) {
    uint32_t ip = 0;
    uint16_t port = 0;
    if (joincode::decode(cfg.peer, cfg.port, ip, port)) {
      char buf[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &ip, buf, sizeof(buf));
      cfg.peer = buf;
      cfg.port = port;
    }
  }
  if (console) {
    if (!GetStdHandle(STD_OUTPUT_HANDLE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
      FILE* f;
      freopen_s(&f, "CONOUT$", "w", stdout);
    }
    opt.auto_connect_first = cfg.peer.empty() && !cfg.host_steamid && !opt.join_lobby;
  }

  tlog::open(dir + L"tmntnet-client.log", cfg.log_level, console);
  LOGI("tmntnet-client " TMNTNET_VERSION " (protocol v%u), name '%s', %s", kVersion,
       cfg.name.c_str(), console ? "console" : "window");

  ClientCore core(cfg, opt);
  if (!core.start()) return 1;
  int rc = console ? run_console(core, run_seconds) : gui::run(core, shot_path, shot_delay);
  core.stop();
  return rc;
}
