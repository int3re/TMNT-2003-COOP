#include "mod/net_peer.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>

#include "common/log.h"
#include "common/net.h"
#include "common/steam_boot.h"
#include "common/transport_enet.h"
#include "common/transport_steam.h"
#include "mod/ls_guest.h"

namespace {

using transport::EventType;
using transport::PeerId;

constexpr uint32_t kRetryMs = 3000;  // a new try to reach the host

Config g_cfg;
uint8_t g_exe_hash[32];
std::atomic<bool> g_quit{false};
HANDLE g_thread = nullptr;

// Network thread only (kept alive past DLL unload, like net_host's).
struct Link {
  std::unique_ptr<transport::ITransport> t;
  PeerId host;
  LsGuest guest;
  uint32_t tried_at = 0;
};
Link* const g_link = new Link;

bool open_transport() {
  if (g_cfg.host_steamid && g_cfg.steam_wanted() && steam::boot(g_cfg)) {
    uint64_t host = g_cfg.host_steamid;
    g_link->t = transport::make_steam_transport([host](uint64_t id) { return id == host; });
    if (g_link->t) {
      g_link->host = PeerId::steam(host);
      LOGI("lockstep peer: joining the host over Steam (%llu)", (unsigned long long)host);
      return true;
    }
  }
  // peer= address[:port]
  std::string host = g_cfg.peer;
  uint16_t port = g_cfg.port;
  size_t colon = host.rfind(':');
  if (colon != std::string::npos && colon + 1 < host.size() &&
      host.find_first_not_of("0123456789", colon + 1) == std::string::npos) {
    port = (uint16_t)std::stoi(host.substr(colon + 1));
    host.resize(colon);
  }
  net::Addr a;
  if (host.empty() || !net::resolve(host, port, a)) {
    LOGE("lockstep peer: no host to join (set [net] peer= or host_steamid=)");
    return false;
  }
  auto t = std::make_unique<transport::EnetTransport>(uint16_t{0}, 1, (uint32_t)g_cfg.timeout_ms);
  if (!t->Init()) {
    LOGE("lockstep peer: cannot open ENet");
    return false;
  }
  g_link->t = std::move(t);
  g_link->host = PeerId::enet(a.sa.sin_addr.s_addr, ntohs(a.sa.sin_port));
  LOGI("lockstep peer: joining the host at %s (ENet)", a.str().c_str());
  return true;
}

DWORD WINAPI peer_thread(void*) {
  if (!open_transport()) return 0;
  Link& L = *g_link;
  L.guest.reset(L.t.get(), L.host, g_cfg, g_exe_hash);
  while (!g_quit) {
    uint32_t now = net::now_ms();
    if (!L.guest.up() && !L.guest.refused() && now - L.tried_at >= kRetryMs) {
      L.tried_at = now;
      L.t->Connect(L.host);
    }
    if (L.host.kind == transport::Kind::Steam) steam::pump();
    L.t->Poll(1);
    transport::Event evs[16];
    for (int n; (n = L.t->Receive(evs, 16)) > 0;)
      for (int i = 0; i < n; ++i) {
        if (evs[i].peer != L.host) continue;
        if (evs[i].type == EventType::Connected)
          L.guest.on_connected();
        else if (evs[i].type == EventType::Disconnected)
          L.guest.on_disconnected(evs[i].reason);
        else
          L.guest.on_data(evs[i]);
      }
    L.guest.tick(net::now_ms());
  }
  bool up = L.guest.up();
  L.guest.clear();
  if (up) L.t->Disconnect(L.host);
  L.t->Poll(50);
  L.t->Shutdown();
  if (L.host.kind == transport::Kind::Steam) steam::shutdown();
  return 0;
}

}  // namespace

bool start_net_peer(const Config& cfg, const uint8_t exe_sha256[32]) {
  g_cfg = cfg;
  std::memcpy(g_exe_hash, exe_sha256, 32);
  if (!net::startup()) {
    LOGE("WSAStartup failed");
    return false;
  }
  g_thread = CreateThread(nullptr, 0, peer_thread, nullptr, 0, nullptr);
  return g_thread != nullptr;
}

void stop_net_peer() {
  if (!g_thread || g_quit.exchange(true)) return;
  if (WaitForSingleObject(g_thread, 700) == WAIT_TIMEOUT) LOGW("lockstep peer: network thread did not stop in time");
}
