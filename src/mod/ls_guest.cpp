#include "mod/ls_guest.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "common/log.h"
#include "common/protocol.h"
#include "mod/lockstep.h"
#include "mod/revive.h"

using namespace tmnp;
using transport::Channel;

namespace {
constexpr uint32_t kResendMs = 16;   // our buttons again, also when nothing is new
constexpr uint32_t kReadyEveryMs = 100;
}  // namespace

void LsGuest::reset(transport::ITransport* t, const transport::PeerId& host, const Config& cfg, const uint8_t exe[32]) {
  clear();
  t_ = t;
  host_ = host;
  name_ = cfg.name;
  std::memcpy(exe_, exe, 32);
}

void LsGuest::clear() {
  if (t_) lockstep::net_end("left the host's game");
  t_ = nullptr;
  host_ = transport::PeerId{};
  up_ = accepted_ = refused_ = false;
  host_name_.clear();
  refusal_.clear();
  names_.clear();
  sent_at_ = sent_version_ = ready_at_ = 0;
}

template <typename T>
void LsGuest::send(T& p, uint8_t type, Channel ch, uint8_t flags) {
  make_header(p.h, (PacketType)type, ++seq_);
  p.h.flags = flags;
  t_->Send(host_, &p, sizeof(p), ch);
}

void LsGuest::on_connected() {
  up_ = true;
  HelloPacket h{};
  std::memcpy(h.exe_sha256, exe_, 32);
  set_name(h.name, name_.c_str());
  send(h, PT_Hello, Channel::Control, kFlagLockstep);
  LOGI("lockstep guest: link to the host up (%s), Hello sent", t_->Describe(host_).c_str());
}

void LsGuest::on_disconnected(const std::string& why) {
  up_ = accepted_ = false;
  lockstep::net_end("the link to the host is gone");
  LOGW("lockstep guest: the host is gone (%s)", why.c_str());
}

void LsGuest::on_data(const transport::Event& ev) {
  PacketHeader h;
  if (parse(ev.data.data(), ev.data.size(), h) != ParseResult::Ok) return;
  const uint8_t* d = ev.data.data();
  switch (h.type) {
    case PT_HelloAck: {
      HelloAckPacket a;
      std::memcpy(&a, d, sizeof(a));
      a.name[sizeof(a.name) - 1] = '\0';
      accepted_ = true;
      host_name_ = a.name;
      if (!(h.flags & kFlagLockstep))
        LOGW("lockstep guest: the host '%s' runs an older tmntnet (no game on every PC)", a.name);
      else
        LOGI("lockstep guest: the host '%s' took us as player %u", a.name, a.player);
      break;
    }
    case PT_Reject: {
      RejectPacket r;
      std::memcpy(&r, d, sizeof(r));
      r.message[sizeof(r.message) - 1] = '\0';
      refused_ = true;
      refusal_ = r.message;
      LOGE("lockstep guest: the host refused: %s", r.message);
      break;
    }
    case PT_LsStart: {
      LsStartPacket p;
      std::memcpy(&p, d, sizeof(p));
      lockstep::Start s;
      s.session = p.session;
      s.seed = p.seed;
      s.delay = p.delay;
      s.players = p.players;
      s.me = p.you;
      std::memcpy(s.options, p.options, sizeof(s.options));
      s.apply_options = true;
      s.expect_save = (p.flags & kStartSave) != 0;
      s.catchup = (p.flags & kStartCatchup) != 0;
      s.have_data = (p.flags & kStartData) != 0;
      std::memcpy(s.data, p.data, sizeof(s.data));
      revive::set_session((p.flags & kStartRevive) != 0, p.flags >> 4);  // the host's rules
      lockstep::net_start(s);
      break;
    }
    case PT_LsSave: {
      static LsSavePacket p;  // (network thread only)
      std::memcpy(&p, d, sizeof(p));
      lockstep::net_save(p);
      break;
    }
    case PT_LsInput: {
      LsInputPacket p{};
      std::memcpy(&p, d, std::min(ev.data.size(), sizeof(p)));
      lockstep::net_input(p);
      break;
    }
    case PT_LsHash: {
      LsHashPacket p;
      std::memcpy(&p, d, sizeof(p));
      lockstep::net_hash(p);
      break;
    }
    case PT_LsNames: {
      LsNamesPacket p{};
      std::memcpy(&p, d, std::min(ev.data.size(), sizeof(p)));
      names_.clear();
      for (int i = 0; i < p.players && i < 4; ++i) {
        p.names[i][31] = '\0';
        names_.push_back(p.names[i]);
      }
      break;
    }
    case PT_LsLeave: {
      LsLeavePacket p{};
      std::memcpy(&p, d, std::min(ev.data.size(), sizeof(p)));
      lockstep::net_leave(p);
      break;
    }
    case PT_LsHistory: {
      static LsHistoryPacket p;  // (2 KB; network thread only)
      std::memset(&p, 0, sizeof(p));
      std::memcpy(&p, d, std::min(ev.data.size(), sizeof(p)));
      lockstep::net_history_in(p);
      break;
    }
    case PT_LsReturn: {
      LsReturnPacket p;
      std::memcpy(&p, d, sizeof(p));
      lockstep::net_return(p);
      break;
    }
    case PT_Barrier: {
      LsReadyPacket p;
      std::memcpy(&p, d, sizeof(p));
      lockstep::net_ready(p);
      break;
    }
    default: break;
  }
}

void LsGuest::tick(uint32_t now) {
  if (!t_ || !up_ || !accepted_) return;
  LsInputPacket in;
  uint32_t version = 0;
  if (lockstep::net_outgoing(in, version) && (version != sent_version_ || now - sent_at_ >= kResendMs)) {
    sent_version_ = version;
    sent_at_ = now;
    make_header(in.h, PT_LsInput, ++seq_);
    t_->Send(host_, &in, offsetof(LsInputPacket, buttons) + in.count * sizeof(uint16_t), Channel::Input);
  }
  LsHashPacket hp;
  if (lockstep::net_take_hash(hp)) send(hp, PT_LsHash, Channel::Input);
  LsReadyPacket rp{};
  if (now - ready_at_ >= kReadyEveryMs && lockstep::net_ready_out(rp)) {
    ready_at_ = now;
    send(rp, PT_Barrier, Channel::Input);
  }
  LsCatchupPacket cp{};
  if (now - catchup_at_ >= 100 && lockstep::net_catchup_out(cp)) {  // (coming back: how far)
    catchup_at_ = now;
    send(cp, PT_LsCatchup, Channel::Input);
  }
}
