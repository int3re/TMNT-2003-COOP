// tmntnet-client core: gamepad/keyboard reading, LAN discovery, Steam lobbies
// and the session with the host (common/transport.h). Runs on its own
// thread (all Steam calls happen there); the window (gui.cpp) and the console
// mode (main.cpp) talk to it through ClientCore's thread-safe API.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/config.h"
#include "common/lobby_steam.h"
#include "common/net.h"

enum class ClientState { Idle, Discovering, Connecting, Handshake, Connected, Waiting, Rejected };
const char* client_state_name(ClientState s);

struct FoundHost {
  net::Addr addr;  // ENet address (host's port)
  std::string name;
  uint16_t version = 0;
  uint32_t seen_ms = 0;
};

struct ClientSnapshot {
  ClientState state = ClientState::Idle;
  std::string server;     // what we connect to: "ip:port" or "Steam: name"
  std::string last_key;   // for [client] last=: "ip:port" or "steam:<SteamID64>"
  std::string via;        // "ENet" / "Steam" while connected
  std::string host_name;  // from HelloAck
  uint32_t rtt_ms = 0;
  uint16_t buttons = 0;   // what is being sent right now
  std::string pad;        // gamepad name, empty = none
  std::string error;      // last rejection / failure, for the UI
  std::vector<FoundHost> lan;
  // Steam
  bool steam = false;       // Steam is up in this client
  std::string steam_name;   // own Steam name
  std::string steam_note;   // Russian: why Steam is off ("" if on or not wanted)
  std::vector<lobby::Found> lobbies;
};

struct ClientOptions {
  bool auto_connect_first = false;  // console mode without peer=: join the first host found
  bool test_pattern = false;        // scripted P2 input (--test)
  bool no_input = false;            // always send "nothing pressed"
  uint64_t join_lobby = 0;          // +connect_lobby <id> / a lobby id on the command line
};

class ClientCore {
 public:
  ClientCore(const Config& cfg, const ClientOptions& opt) : cfg_(cfg), opt_(opt) {}
  ~ClientCore() { stop(); }

  bool start();  // networking + input thread
  void stop();   // graceful disconnect, joins the thread

  // Any thread.
  void connect(const net::Addr& host);   // LAN / IP (ENet)
  void join_lobby(uint64_t lobby);       // a Steam lobby from the list
  void connect_steam(uint64_t steam_id); // a host by SteamID64
  void disconnect();
  void set_typing(bool typing) { typing_ = typing; }  // UI text box focused: no keyboard P2
  ClientSnapshot snapshot();

  const Config& config() const { return cfg_; }

 private:
  void run();

  enum class Cmd { None, Connect, JoinLobby, ConnectSteam };

  Config cfg_;
  ClientOptions opt_;
  std::thread thread_;
  std::atomic<bool> quit_{false};
  std::atomic<bool> typing_{false};
  std::mutex m_;
  ClientSnapshot snap_;
  Cmd cmd_ = Cmd::None;
  bool cmd_disconnect_ = false;
  net::Addr cmd_addr_;
  uint64_t cmd_id_ = 0;
};
