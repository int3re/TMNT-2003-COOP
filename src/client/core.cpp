#include "client/core.h"

#include <windows.h>
#include <timeapi.h>
#include <xinput.h>  // XINPUT_GAMEPAD_* bit values used by [pad] in tmntnet.ini

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "common/log.h"
#include "common/protocol.h"
#include "common/sha256.h"
#include "common/steam_boot.h"
#include "common/transport_enet.h"
#include "common/transport_steam.h"

using namespace tmnp;
using transport::Channel;
using transport::Event;
using transport::EventType;
using transport::PeerId;

const char* client_state_name(ClientState s) {
  switch (s) {
    case ClientState::Idle: return "не подключён";
    case ClientState::Discovering: return "поиск в LAN";
    case ClientState::Connecting: return "подключение";
    case ClientState::Handshake: return "подключение";
    case ClientState::Connected: return "подключён";
    case ClientState::Waiting: return "переподключение";
    default: return "отказ";
  }
}

namespace {

// --- gamepad (SDL2 GameController: Xbox, DualShock/DualSense, Switch, ...) --

class Pads {
 public:
  explicit Pads(const Config& cfg) : cfg_(cfg) {}

  bool init() {
    // The player looks at the stream window, not at this one.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
      LOGW("SDL_Init: %s; keyboard only", SDL_GetError());
      return false;
    }
    return true;
  }
  void shutdown() { SDL_Quit(); }
  std::string name() const { return pad_ ? SDL_GameControllerName(pad_) : ""; }

  // Pumps SDL events (hot-plug) and returns the pad as XINPUT_GAMEPAD_* bits
  // plus kPadLT/kPadRT, with the left stick in lx/ly.
  uint32_t poll(int16_t& lx, int16_t& ly) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_CONTROLLERDEVICEADDED && !pad_) open(e.cdevice.which);
      if (e.type == SDL_CONTROLLERDEVICEREMOVED && pad_ && e.cdevice.which == id_) {
        LOGW("gamepad disconnected");
        SDL_GameControllerClose(pad_);
        pad_ = nullptr;
        for (int i = 0; i < SDL_NumJoysticks() && !pad_; ++i) open(i);
      }
    }
    lx = ly = 0;
    if (!pad_) return 0;
    static const struct {
      SDL_GameControllerButton b;
      uint32_t bit;
    } kMap[] = {
        {SDL_CONTROLLER_BUTTON_A, XINPUT_GAMEPAD_A},
        {SDL_CONTROLLER_BUTTON_B, XINPUT_GAMEPAD_B},
        {SDL_CONTROLLER_BUTTON_X, XINPUT_GAMEPAD_X},
        {SDL_CONTROLLER_BUTTON_Y, XINPUT_GAMEPAD_Y},
        {SDL_CONTROLLER_BUTTON_BACK, XINPUT_GAMEPAD_BACK},
        {SDL_CONTROLLER_BUTTON_START, XINPUT_GAMEPAD_START},
        {SDL_CONTROLLER_BUTTON_LEFTSTICK, XINPUT_GAMEPAD_LEFT_THUMB},
        {SDL_CONTROLLER_BUTTON_RIGHTSTICK, XINPUT_GAMEPAD_RIGHT_THUMB},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER},
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT},
    };
    uint32_t bits = 0;
    for (const auto& m : kMap)
      if (SDL_GameControllerGetButton(pad_, m.b)) bits |= m.bit;
    constexpr int16_t kTrigger = 8000;
    if (SDL_GameControllerGetAxis(pad_, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > kTrigger) bits |= kPadLT;
    if (SDL_GameControllerGetAxis(pad_, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > kTrigger) bits |= kPadRT;
    lx = SDL_GameControllerGetAxis(pad_, SDL_CONTROLLER_AXIS_LEFTX);
    ly = SDL_GameControllerGetAxis(pad_, SDL_CONTROLLER_AXIS_LEFTY);  // SDL: +Y is down
    return bits;
  }

 private:
  void open(int device_index) {
    if (!SDL_IsGameController(device_index)) return;
    if (cfg_.pad >= 0 && device_index != cfg_.pad) return;
    pad_ = SDL_GameControllerOpen(device_index);
    if (!pad_) return;
    id_ = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad_));
    LOGI("gamepad #%d: %s", device_index, SDL_GameControllerName(pad_));
  }

  const Config& cfg_;
  SDL_GameController* pad_ = nullptr;
  SDL_JoystickID id_ = -1;
};

uint16_t read_input(const Config& cfg, Pads* pads, bool keyboard, int8_t& lx8, int8_t& ly8) {
  int16_t lx = 0, ly = 0;
  uint32_t pressed = pads ? pads->poll(lx, ly) : 0;
  uint16_t buttons = 0;
  for (int i = 0; i < kNumSlots; ++i)
    if (cfg.pad_map[i] & pressed) buttons |= (uint16_t)(1u << i);
  if (pressed & XINPUT_GAMEPAD_DPAD_UP) buttons |= BTN_UP;
  if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) buttons |= BTN_DOWN;
  if (pressed & XINPUT_GAMEPAD_DPAD_LEFT) buttons |= BTN_LEFT;
  if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) buttons |= BTN_RIGHT;
  int dz = cfg.stick_deadzone;
  if (lx < -dz) buttons |= BTN_LEFT;
  if (lx > dz) buttons |= BTN_RIGHT;
  if (ly < -dz) buttons |= BTN_UP;
  if (ly > dz) buttons |= BTN_DOWN;
  lx8 = (int8_t)(lx / 258);
  ly8 = (int8_t)(ly / 258);
  if (keyboard) {
    for (int i = 0; i < kNumSlots; ++i)
      for (int vk : cfg.key_map[i])
        if (GetAsyncKeyState(vk) & 0x8000) buttons |= (uint16_t)(1u << i);
  }
  return buttons;
}

// --- session with the host ------------------------------------------------------

constexpr uint32_t kHandshakeTimeoutMs = 15000;  // Steam: a host that never accepts

struct Session {
  const Config& cfg;
  transport::ITransport* enet = nullptr;   // LAN / IP; may be missing (transport=steam)
  transport::ITransport* steam = nullptr;  // may be missing (no Steam)
  transport::ITransport* t = nullptr;      // the one in use
  PeerId target;                           // whom we (re)connect to
  PeerId peer;                             // the live link; invalid = none
  std::string label;                       // "ip:port" / "Steam: name"
  ClientState state = ClientState::Idle;
  uint32_t seq = 0, frame = 0;
  InputFrame history[kInputHistory] = {};
  int history_count = 0;
  uint8_t exe_hash[32] = {};
  uint32_t retry_at_ms = 0, handshake_at_ms = 0;
  std::string host_name, error;

  explicit Session(const Config& c) : cfg(c) {}

  template <typename T>
  void send(T& p, PacketType type, Channel ch, size_t size = sizeof(T)) {
    make_header(p.h, type, ++seq);
    t->Send(peer, &p, size, ch);
  }

  // (Re)opens the link to target over its transport.
  void connect(transport::ITransport* via, const PeerId& to, const std::string& what) {
    drop();
    t = via;
    target = to;
    label = what;
    peer = t && t->Connect(target) ? target : PeerId{};
    state = peer.valid() ? ClientState::Connecting : ClientState::Waiting;
    retry_at_ms = net::now_ms() + 1000;
    history_count = 0;
    frame = 0;
    error.clear();
    LOGI("connecting to %s ...", label.c_str());
  }
  void reconnect() { connect(t, target, label); }

  void drop() {  // immediate, no handshake (used before switching hosts)
    if (peer.valid() && t) t->Close(peer);
    peer = PeerId{};
  }

  void on_receive(const std::vector<uint8_t>& pk) {
    PacketHeader h;
    ParseResult r = parse(pk.data(), pk.size(), h);
    if (r == ParseResult::BadVersion) {
      error = "У хоста другая версия tmntnet: обновите мод на обоих ПК.";
      state = ClientState::Rejected;
      return;
    }
    if (r != ParseResult::Ok) return;
    if (h.type == PT_HelloAck && state == ClientState::Handshake) {
      HelloAckPacket ack;
      std::memcpy(&ack, pk.data(), sizeof(ack));
      ack.name[sizeof(ack.name) - 1] = '\0';
      host_name = ack.name;
      state = ClientState::Connected;
      LOGI("connected to '%s' as player %u (%s)", ack.name, ack.player, t->Name());
    } else if (h.type == PT_Reject) {
      RejectPacket rj;
      std::memcpy(&rj, pk.data(), sizeof(rj));
      rj.message[sizeof(rj.message) - 1] = '\0';
      LOGE("host rejected us (reason %u): %s", rj.reason, rj.message);
      switch (rj.reason) {
        case RR_Busy: error = "У хоста уже есть второй игрок."; break;
        case RR_Kicked: error = "Хост отключил вас от игры."; break;
        case RR_Version: error = "У хоста другая версия tmntnet: обновите мод на обоих ПК."; break;
        case RR_NotAllowed: error = "Хост принимает подключения только с другого адреса (peer= в его tmntnet.ini)."; break;
        case RR_ExeMismatch:
          error = "У вас и у хоста разные tmnt.exe: нужна одна и та же версия игры на обоих ПК (" +
                  std::string(rj.message) + ").";
          break;
        default: error = rj.message;
      }
      state = ClientState::Rejected;
    }
  }

  void on_event(const Event& ev, uint32_t now) {
    switch (ev.type) {
      case EventType::Connected: {
        state = ClientState::Handshake;
        handshake_at_ms = now;
        HelloPacket p{};
        std::memcpy(p.exe_sha256, exe_hash, 32);
        set_name(p.name, cfg.name.c_str());
        send(p, PT_Hello, Channel::Control);
        break;
      }
      case EventType::Data:
        on_receive(ev.data);
        break;
      case EventType::Disconnected:
        peer = PeerId{};
        if (state == ClientState::Connecting) {
          LOGW("no answer from %s, retrying", label.c_str());
          state = ClientState::Waiting;
          retry_at_ms = now + 1000;
        } else if (state != ClientState::Rejected && state != ClientState::Idle) {
          LOGW("connection to %s lost (%s), retrying", label.c_str(), ev.reason.c_str());
          state = ClientState::Waiting;
          retry_at_ms = now + 1000;
        }
        break;
    }
  }

  void service(uint32_t now) {
    for (transport::ITransport* x : {enet, steam}) {
      if (!x) continue;
      x->Poll(0);
      Event evs[16];
      for (int n; (n = x->Receive(evs, 16)) > 0;)
        for (int i = 0; i < n; ++i)
          if (x == t && peer.valid() && evs[i].peer == peer) on_event(evs[i], now);  // else: an old host
    }
    if (state == ClientState::Handshake && now - handshake_at_ms > kHandshakeTimeoutMs) {
      LOGW("%s did not answer the Hello for %u s, retrying", label.c_str(), kHandshakeTimeoutMs / 1000);
      error = "Хост не отвечает.";
      drop();
      state = ClientState::Waiting;
      retry_at_ms = now + 1000;
    }
  }

  void send_input(uint16_t buttons, int8_t lx, int8_t ly) {
    // history[0] is the newest frame; older ones ride along against packet loss.
    std::memmove(&history[1], &history[0], sizeof(InputFrame) * (kInputHistory - 1));
    history[0] = InputFrame{++frame, buttons, lx, ly};
    if (history_count < kInputHistory) ++history_count;
    InputPacket p{};
    p.count = (uint8_t)history_count;
    std::memcpy(p.frames, history, sizeof(history));
    send(p, PT_Input, Channel::Input,
         offsetof(InputPacket, frames) + history_count * sizeof(InputFrame));
  }

  void bye() {
    if (!peer.valid() || !t) return;
    t->Disconnect(peer);
    for (uint32_t start = net::now_ms(); net::now_ms() - start < 500;) {
      t->Poll(50);
      Event evs[16];
      for (int n; (n = t->Receive(evs, 16)) > 0;)
        for (int i = 0; i < n; ++i)
          if (evs[i].type == EventType::Disconnected && evs[i].peer == peer) {
            peer = PeerId{};
            return;
          }
    }
    t->Close(peer);
    peer = PeerId{};
  }
};

// How well a host address fits this PC: same subnet as the adapter with the
// default route (the real LAN) beats another local subnet beats anything else.
int address_score(const net::Addr& a, const std::vector<net::LocalAddr>& locals) {
  uint32_t ip = ntohl(a.sa.sin_addr.s_addr);
  for (size_t i = 0; i < locals.size(); ++i)
    if ((ntohl(locals[i].ip) & 0xFFFFFF00u) == (ip & 0xFFFFFF00u)) return i == 0 ? 2 : 1;
  return 0;
}

PeerId enet_peer(const net::Addr& a) { return PeerId::enet(a.sa.sin_addr.s_addr, ntohs(a.sa.sin_port)); }

}  // namespace

bool ClientCore::start() {
  if (!net::startup()) {
    LOGE("WSAStartup failed");
    return false;
  }
  thread_ = std::thread(&ClientCore::run, this);
  return true;
}

void ClientCore::stop() {
  quit_ = true;
  if (thread_.joinable()) thread_.join();
}

void ClientCore::connect(const net::Addr& host) {
  std::lock_guard<std::mutex> lock(m_);
  cmd_ = Cmd::Connect;
  cmd_addr_ = host;
}

void ClientCore::join_lobby(uint64_t lobby) {
  std::lock_guard<std::mutex> lock(m_);
  cmd_ = Cmd::JoinLobby;
  cmd_id_ = lobby;
}

void ClientCore::connect_steam(uint64_t steam_id) {
  std::lock_guard<std::mutex> lock(m_);
  cmd_ = Cmd::ConnectSteam;
  cmd_id_ = steam_id;
}

void ClientCore::disconnect() {
  std::lock_guard<std::mutex> lock(m_);
  cmd_disconnect_ = true;
}

ClientSnapshot ClientCore::snapshot() {
  std::lock_guard<std::mutex> lock(m_);
  return snap_;
}

void ClientCore::run() {
  Session s(cfg_);
  sha256_file(module_dir(nullptr) + L"TMNT.EXE", s.exe_hash);  // a client without the game sends zeros

  // Steam first (on this thread, as Steam requires), then ENet unless
  // transport=steam already has Steam.
  std::unique_ptr<transport::ITransport> steam_t;
  std::unique_ptr<lobby::IBrowser> browser;
  std::string steam_note;
  uint64_t steam_host = 0;  // the only SteamID we take sessions from
  if (cfg_.steam_wanted()) {
    if (steam::boot(cfg_)) {
      steam_t = transport::make_steam_transport([&steam_host](uint64_t id) { return id == steam_host; });
      if (steam_t) browser = lobby::make_browser(lobby::exe16(s.exe_hash));
      if (!steam_t) steam_note = "не удалось запустить сеть Steam (см. tmntnet-client.log)";
    } else {
      steam_note = steam::unavailable_reason();
    }
  }
  std::unique_ptr<transport::ITransport> enet_t;
  if (cfg_.transport != "steam" || !steam_t) {
    // Any local port, one peer, the same timeouts as the host.
    enet_t = std::make_unique<transport::EnetTransport>(uint16_t{0}, 1, (uint32_t)cfg_.timeout_ms);
    if (!enet_t->Init()) {
      LOGE("cannot create ENet client host");
      enet_t.reset();
    }
  }
  s.enet = enet_t.get();
  s.steam = steam_t.get();
  if (!s.enet && !s.steam) {
    std::lock_guard<std::mutex> lock(m_);
    snap_.error = "Не удалось открыть сетевой сокет.";
    snap_.state = ClientState::Rejected;
    return;
  }

  SOCKET disc = s.enet ? net::udp_socket(0, true) : INVALID_SOCKET;  // LAN discovery
  std::vector<FoundHost> lan;
  std::vector<net::LocalAddr> locals = net::local_ipv4();  // default-route adapter first
  uint32_t last_discover = 0, last_steam = 0;
  bool joining = false;    // a lobby join / SteamID lookup is under way
  bool rejoining = false;  // ... to get back into the lobby after the host went quiet
  uint64_t steam_lobby = 0;  // the lobby we play in (0: direct session)

  auto connect_enet = [&](const net::Addr& a) {
    if (!s.enet) {
      s.error = "Подключение по адресу выключено (transport=steam).";
      s.state = ClientState::Rejected;
      return;
    }
    if (browser) browser->Leave();
    s.connect(s.enet, enet_peer(a), a.str());
  };
  auto start_steam_join = [&](bool lobby, uint64_t id) {
    if (!browser) {
      s.error = "Steam недоступен: " + (steam_note.empty() ? std::string("выключен в tmntnet.ini") : steam_note) + ".";
      s.state = ClientState::Rejected;
      return;
    }
    s.drop();
    s.state = ClientState::Connecting;
    s.label = lobby ? "лобби Steam" : "Steam " + std::to_string(id);
    s.error.clear();
    joining = true;
    if (lobby)
      browser->Join(id);
    else
      browser->JoinHost(id);
  };

  if (!cfg_.peer.empty()) {
    net::Addr a;
    if (net::resolve(cfg_.peer, cfg_.port, a)) {
      connect_enet(a);
    } else {
      s.error = "Не найден адрес " + cfg_.peer;
      LOGE("cannot resolve host '%s'", cfg_.peer.c_str());
    }
  } else if (opt_.join_lobby) {
    start_steam_join(true, opt_.join_lobby);  // +connect_lobby <id>
  } else if (cfg_.host_steamid) {
    start_steam_join(false, cfg_.host_steamid);
  } else if (opt_.auto_connect_first) {
    s.state = ClientState::Discovering;
    LOGI("searching the LAN (UDP broadcast to port %u)", cfg_.discovery_port);
  }

  Pads pads(cfg_);
  bool have_pads = !opt_.no_input && pads.init();
  timeBeginPeriod(1);
  LARGE_INTEGER freq, t0, t;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);
  const double period = 1.0 / cfg_.send_hz;
  uint64_t tick = 0;
  uint32_t connected_at = 0, last_publish = 0;
  uint16_t last_logged = 0;
  if (opt_.test_pattern) LOGI("--test: P2 walks right 1s, left 1s, jumps; repeats every 4s");

  while (!quit_) {
    uint32_t now = net::now_ms();

    // Commands from the window.
    {
      std::lock_guard<std::mutex> lock(m_);
      if (cmd_disconnect_) {
        cmd_disconnect_ = false;
        s.bye();
        if (browser) browser->Leave();
        joining = rejoining = false;
        steam_lobby = 0;
        s.state = ClientState::Idle;
        s.error.clear();
        LOGI("disconnected by the user");
      }
      if (cmd_ != Cmd::None) {
        if (s.state == ClientState::Connected) s.bye();
        joining = rejoining = false;
        steam_lobby = 0;
        if (cmd_ == Cmd::Connect) connect_enet(cmd_addr_);
        if (cmd_ == Cmd::JoinLobby) start_steam_join(true, cmd_id_);
        if (cmd_ == Cmd::ConnectSteam) start_steam_join(false, cmd_id_);
        cmd_ = Cmd::None;
      }
    }

    // Input.
    int8_t lx = 0, ly = 0;
    bool keyboard = cfg_.keyboard && !opt_.no_input && !typing_;
    uint16_t buttons = opt_.no_input ? 0 : read_input(cfg_, have_pads ? &pads : nullptr, keyboard, lx, ly);
    if (s.state != ClientState::Connected) connected_at = 0;
    else if (!connected_at) connected_at = now;
    if (opt_.test_pattern && connected_at) {
      uint32_t phase = (now - connected_at) % 4000;
      buttons = phase < 1000 ? BTN_RIGHT : (phase >= 1500 && phase < 2500) ? BTN_LEFT
                : (phase >= 3000 && phase < 3100) ? BTN_JUMP : 0;
    }
    if (buttons != last_logged) {
      LOGD("buttons 0x%04X", buttons);
      last_logged = buttons;
    }

    bool searching = s.state == ClientState::Idle || s.state == ClientState::Discovering ||
                     s.state == ClientState::Rejected;

    // LAN discovery: keep the list fresh while not connected.
    if (disc != INVALID_SOCKET) {
      uint8_t buf[64];
      net::Addr from;
      int n;
      while ((n = net::recv_from(disc, buf, sizeof(buf), from)) > 0) {
        DiscoveryReply r;
        if (n != sizeof(r) || std::memcmp(buf, "TMNP!", 5) != 0) continue;
        std::memcpy(&r, buf, sizeof(r));
        r.name[sizeof(r.name) - 1] = '\0';
        net::Addr host = from;
        host.sa.sin_port = htons(r.port);
        // A host with several adapters (LAN, VPN, ...) answers once per
        // adapter: keep one entry per game, on the address that suits us best.
        auto it = std::find_if(lan.begin(), lan.end(), [&](const FoundHost& f) {
          return f.name == r.name && f.addr.sa.sin_port == host.sa.sin_port;
        });
        if (it == lan.end()) {
          LOGI("found host '%s' at %s (protocol v%u)", r.name, host.str().c_str(), r.version);
          lan.push_back({host, r.name, r.version, now});
        } else {
          if (it->addr != host && address_score(host, locals) > address_score(it->addr, locals))
            it->addr = host;
          it->seen_ms = now;
        }
        if (s.state == ClientState::Discovering && r.version == kVersion) connect_enet(host);
      }
      lan.erase(std::remove_if(lan.begin(), lan.end(),
                               [now](const FoundHost& f) { return now - f.seen_ms > 4000; }),
                lan.end());
      if (searching && now - last_discover >= 1000) {
        last_discover = now;
        for (const net::Addr& bc : net::broadcast_addrs(cfg_.discovery_port))
          net::send_to(disc, kDiscoveryRequest, sizeof(kDiscoveryRequest), bc);
      }
    }

    // Steam: callbacks ~100 Hz, the lobby list while not connected, joins.
    if (s.steam && now - last_steam >= 10) {
      last_steam = now;
      steam::pump();
      browser->Tick(now, searching);
      lobby::JoinResult jr;
      if (browser->TakeJoinResult(jr)) {
        if (!joining) {  // "Join game" in the Steam friends list
          if (s.state == ClientState::Connected) s.bye();
          LOGI("joining from the Steam friends list");
        }
        joining = false;
        if (jr.ok) {
          rejoining = false;
          steam_host = jr.host_id;
          steam_lobby = jr.lobby;
          s.connect(s.steam, PeerId::steam(jr.host_id),
                    "Steam: " + (jr.host.empty() ? std::to_string(jr.host_id) : jr.host));
        } else if (rejoining && steam_host) {
          // The old lobby is gone (the host restarted the game?): find its new one.
          rejoining = false;
          joining = true;
          LOGI("Steam: lobby %llu is gone, looking for the host's new lobby", (unsigned long long)jr.lobby);
          browser->JoinHost(steam_host);
        } else {
          rejoining = false;
          s.error = jr.error;
          s.state = ClientState::Rejected;
        }
      }
    }

    // Session.
    s.service(now);
    if (s.state == ClientState::Waiting && !joining && (int32_t)(now - s.retry_at_ms) >= 0) {
      if (s.target.kind == transport::Kind::Steam && steam_lobby && browser) {
        // The host went quiet: get back into its lobby rather than knock on a
        // session it may have dropped (coop-anything Spacewar notes).
        LOGI("Steam: rejoining lobby %llu", (unsigned long long)steam_lobby);
        rejoining = joining = true;
        s.state = ClientState::Connecting;
        browser->Join(steam_lobby);
      } else {
        s.reconnect();
      }
    }
    if (s.state == ClientState::Connected) s.send_input(buttons, lx, ly);

    if (now - last_publish >= 50) {
      last_publish = now;
      std::lock_guard<std::mutex> lock(m_);
      snap_.state = s.state;
      snap_.server = s.label;
      snap_.last_key = s.target.kind == transport::Kind::Steam ? "steam:" + std::to_string(s.target.id) : s.label;
      snap_.via = s.t ? s.t->Name() : "";
      snap_.host_name = s.host_name;
      snap_.rtt_ms = s.peer.valid() ? s.t->RttMs(s.peer) : 0;
      snap_.buttons = buttons;
      snap_.pad = have_pads ? pads.name() : "";
      snap_.error = s.error;
      snap_.lan = lan;
      snap_.steam = s.steam != nullptr;
      snap_.steam_name = s.steam ? steam::my_name() : "";
      snap_.steam_note = steam_note;
      snap_.lobbies = browser ? browser->List() : std::vector<lobby::Found>{};
    }

    // Fixed-rate pacing against the QPC clock (no drift accumulation).
    ++tick;
    for (;;) {
      QueryPerformanceCounter(&t);
      double elapsed = double(t.QuadPart - t0.QuadPart) / freq.QuadPart;
      double wait = tick * period - elapsed;
      if (wait <= 0) break;
      if (wait > 0.002) Sleep(1);
    }
  }

  s.bye();
  LOGI("bye");
  timeEndPeriod(1);
  if (disc != INVALID_SOCKET) closesocket(disc);
  browser.reset();  // leaves the lobby
  if (steam_t) steam_t->Shutdown();
  steam_t.reset();
  steam::shutdown();
  if (enet_t) enet_t->Shutdown();
  if (have_pads) pads.shutdown();
}
