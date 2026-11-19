#pragma once
#include <cstdint>

#include "common/config.h"
#include "common/lobby_steam.h"

#include <string>
#include <vector>

// Starts the host network thread: accepts a streaming P2, or up to three PCs
// that run the game too (players 2..4), over ENet (LAN / IP) and/or Steam
// (lobby), answers LAN discovery, and publishes a streaming P2's buttons into
// g_p2. Never touches game memory.
bool start_net_host(const Config& cfg, const uint8_t exe_sha256[32]);
// Game window closing: leave the Steam lobby and shut Steam down (waits
// briefly for the network thread).
void stop_net_host();

// Snapshot for the in-game UI (any thread).
struct HostStatus {
  bool started = false;  // the network thread has set up its transports
  bool running = false;
  uint16_t port = 0;
  std::string name;  // this host's name as shown to clients
  bool enet = false;  // listening on UDP port (LAN / IP join codes work)
  bool steam = false;  // Steam transport up
  std::string steam_name;  // own Steam name
  uint64_t steam_id = 0, lobby_id = 0;
  std::string steam_note;  // Russian: why Steam is off ("" if on or not wanted)
  bool steam_mock = false;  // the lab mock, not real Steam
  std::vector<lobby::Friend> friends;  // online Steam friends, for invites
  bool p2 = false;   // a guest has joined (the first: p2_*)
  std::string p2_name, p2_addr, p2_via;  // p2_via: "ENet" / "Steam"
  struct Guest {
    int player = 0;  // 2..4
    std::string name;
    uint32_t rtt_ms = 0;
    bool ready = false;  // pressed ГОТОВ in its network page
  };
  std::vector<Guest> guests;  // everybody in our game
  uint32_t rtt_ms = 0;
  bool p2_input_live = false;  // input arrived within timeout_ms
  uint32_t packets = 0, lost = 0;
  // v2: this game in a friend's game (0 = no, 1 = joining, 2 = in its lobby).
  int away = 0;
  std::string away_host, away_error;  // the friend's Steam name; why joining failed
  bool away_up = false;               // its game took us as player 2
  uint32_t away_rtt = 0;
  std::vector<lobby::Found> games;    // friends' tmntnet games we can join (Steam)
  std::vector<std::string> away_names;  // in a friend's game: every player's name (player 1..), from its host
};
HostStatus host_status();
void host_kick(int player);  // disconnect player 2..4, -1 everybody (from any thread)
void host_invite(uint64_t steam_id);  // Steam lobby invite (from any thread)
// v2: play in a friend's game (its Steam lobby) / come back (any thread).
void host_join(uint64_t lobby_id);
void host_leave();
// The host opened its network page in a running game: a new session with every
// guest (any thread).
void host_set_again();
