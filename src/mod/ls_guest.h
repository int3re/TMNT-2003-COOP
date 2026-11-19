// The joining side of a lockstep session (the game on every PC), over any
// transport: Hello as a player who runs the game, the host's start, then both
// ways our buttons, state hashes and "ready" (lockstep.cpp does the game
// side). Used by net_peer.cpp ([net] role=client: ENet or a Steam ID from the
// ini) and by net_host.cpp (a friend's game joined from the network page or
// from a Steam invite). Network thread only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/transport.h"

class LsGuest {
 public:
  // A new host to join over t (the link is opened by the caller).
  void reset(transport::ITransport* t, const transport::PeerId& host, const Config& cfg, const uint8_t exe[32]);
  void clear();  // no host any more (ends a session)

  bool active() const { return t_ != nullptr; }
  const transport::PeerId& host() const { return host_; }
  transport::ITransport* transport() const { return t_; }

  void on_connected();                         // the link is up: Hello
  void on_data(const transport::Event& ev);    // a packet from the host
  void on_disconnected(const std::string& why);
  void tick(uint32_t now_ms);                  // buttons, hashes, ready

  bool up() const { return up_; }
  bool accepted() const { return accepted_; }
  bool refused() const { return refused_; }
  const std::string& host_name() const { return host_name_; }
  // Every player's name (player 1.., from the host's LsNames; empty until then).
  std::vector<std::string> names() const { return names_; }
  const std::string& refusal() const { return refusal_; }  // the host's reason, as sent

 private:
  template <typename T>
  void send(T& p, uint8_t type, transport::Channel ch, uint8_t flags = 0);

  transport::ITransport* t_ = nullptr;
  transport::PeerId host_;
  std::string name_;
  uint8_t exe_[32] = {};
  bool up_ = false, accepted_ = false, refused_ = false;
  std::string host_name_, refusal_;
  std::vector<std::string> names_;
  uint32_t catchup_at_ = 0;
  uint32_t seq_ = 0, sent_at_ = 0, sent_version_ = 0, ready_at_ = 0;
};
