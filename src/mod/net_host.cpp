#include "mod/net_host.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

#include "common/lobby_steam.h"
#include "common/log.h"
#include "common/net.h"
#include "common/protocol.h"
#include "common/sha256.h"
#include "common/steam_boot.h"
#include "common/transport_enet.h"
#include "common/transport_steam.h"
#include "mod/lockstep.h"
#include "mod/ls_guest.h"
#include "mod/main_menu.h"
#include "mod/shared_state.h"
#include "mod/revive.h"

namespace {

using namespace tmnp;
using transport::Channel;
using transport::Event;
using transport::EventType;
using transport::ITransport;
using transport::PeerId;

Config g_cfg;
uint8_t g_exe_hash[32];
// Created and destroyed on the network thread only (stop_transports). Kept in
// a never-freed heap object: as plain globals their destructors would run at
// DLL unload, under the loader lock and after ExitProcess has killed the
// network thread, and call into ENet and Steam from there.
struct Owned {
  std::unique_ptr<ITransport> enet, steam;  // either may be missing
  std::unique_ptr<lobby::IHost> lobby;
  std::unique_ptr<lobby::IBrowser> browser;  // friends' games, invites (with Steam)
  LsGuest guest;                             // playing in a friend's game
};
Owned* const g_owned = new Owned;
std::unique_ptr<ITransport>& g_enet = g_owned->enet;
std::unique_ptr<ITransport>& g_steam = g_owned->steam;
std::unique_ptr<lobby::IHost>& g_lobby = g_owned->lobby;
std::unique_ptr<lobby::IBrowser>& g_browser = g_owned->browser;
LsGuest& g_guest = g_owned->guest;

// The name the other player sees (Hello, the name tags): [net] name= if the
// ini has one, else the Steam name, else the computer's.
std::string shown_name() {
  if (!g_cfg.name_set && g_steam) {
    std::string s = steam::my_name();
    if (!s.empty()) return s;
  }
  return g_cfg.name;
}

// v2: this game can also play in a friend's game (the game on every PC): join
// its Steam lobby (from the network page, an accepted Steam invite or "Join
// game" in the friends list), then its session as player 2. Meanwhile our own
// lobby is closed and nobody can join us.
enum class Away { No, Joining, Joined };
Away g_away = Away::No;
uint64_t g_away_host = 0;
uint64_t g_away_lobby = 0;  // the friend's lobby we are in (or joining)
std::string g_away_name, g_away_error;
uint32_t g_away_tried = 0;
std::atomic<uint64_t> g_join_cmd{0};
std::atomic<bool> g_set_again{false};  // host_set_again
std::atomic<bool> g_leave_cmd{false};
std::vector<lobby::Found> g_found;
std::string g_steam_note;
SOCKET g_disc = INVALID_SOCKET;
net::Addr g_allowed;  // valid() only when [net] peer= is set on the host
uint32_t g_seq = 0;
std::atomic<int> g_kick{0};  // the player to remove (2..4; -1 everybody)
std::atomic<bool> g_quit{false};
std::atomic<uint64_t> g_invite{0};
std::vector<lobby::Friend> g_friends;
uint32_t g_friends_at = 0;
HANDLE g_thread = nullptr;
SRWLOCK g_status_lock = SRWLOCK_INIT;
HostStatus g_status;

// Our game's guests: players 2..4. One streaming P2 (v1: the game only here),
// or up to three PCs that run the game too (v2, lockstep).
constexpr int kGuests = 3;
struct Session {
  ITransport* t = nullptr;  // accepted (after Hello): its transport and peer
  PeerId peer;
  char name[32] = {};
  bool have_seq = false, have_frame = false, stale = false;
  uint32_t last_seq = 0, newest_frame = 0;
  uint32_t packets = 0, lost = 0;
  uint16_t last_buttons = 0;
  // v2: the other PC runs the game too (lockstep.cpp); its LsInput, not Input.
  bool lockstep = false;
  int player = -1;       // its player in our session (1..3), -1 until one starts with it
  bool ready = false;    // its game can begin (LsReady)
  uint8_t where = 0;     // where its game is while the session waits (lockstep::Where)
  bool said_ready = false;  // its player pressed ГОТОВ (kept while it stays)
  bool have_input = false;
  uint32_t newest_input = 0;  // the newest frame of its buttons we passed on
  uint32_t ls_version = 0, ls_sent_ms = 0, ls_ready_ms = 0;
  // Coming back into the running session: the frames sent to it so far, how
  // far its game is (LsCatchup), and the frame its player is its own again.
  bool catching = false;
  uint32_t sent = 0, at = 0, have = 0, back_at = 0;
};
Session g_ss[kGuests];
uint32_t g_session_id = 0;  // the session we started last
// The running session's start as sent (for a PC that comes back), and the
// players whose PCs left it (bit per player).
LsStartPacket g_start_sent{};
LsSavePacket g_save_sent{};  // (4.4 KB)
bool g_start_save = false, g_start_data = false;
unsigned g_left = 0;
void come_back(struct Session& s);
constexpr uint32_t kCatchWindow = 4096;  // frames sent ahead of a PC coming back (lockstep's ring holds 8192)
constexpr uint32_t kNearFrames = 90;     // that close behind us, it gets its player back
constexpr uint32_t kBackAhead = 60;      // that many frames after our frame
bool g_restart = false;     // our guests changed: a session with them all, at the next main menu
bool g_names_out = false;   // the names go out again

int find_guest(ITransport* t, const PeerId& peer) {
  for (int i = 0; i < kGuests; ++i)
    if (g_ss[i].t == t && g_ss[i].peer == peer) return i;
  return -1;
}
int free_slot() {
  for (int i = 0; i < kGuests; ++i)
    if (!g_ss[i].t) return i;
  return -1;
}
bool has_guest() {
  for (const Session& s : g_ss)
    if (s.t) return true;
  return false;
}
int lockstep_guests() {
  int n = 0;
  for (const Session& s : g_ss) n += s.t && s.lockstep;
  return n;
}
bool streaming_guest() {
  for (const Session& s : g_ss)
    if (s.t && !s.lockstep) return true;
  return false;
}
// In the session that runs or waits: its player and its link.
bool in_session(const Session& s) { return s.t && s.lockstep && s.player > 0; }

// Rejected peers on their way out: anything else they send is ignored.
std::vector<PeerId> g_rejected;
bool is_rejected(const PeerId& p) {
  return std::find(g_rejected.begin(), g_rejected.end(), p) != g_rejected.end();
}
void forget_rejected(const PeerId& p) {
  g_rejected.erase(std::remove(g_rejected.begin(), g_rejected.end(), p), g_rejected.end());
}

template <typename T>
void send_packet(ITransport* t, const PeerId& peer, T& p, PacketType type, Channel ch, uint8_t flags = 0) {
  make_header(p.h, type, ++g_seq);
  p.h.flags = flags;
  t->Send(peer, &p, sizeof(p), ch);
}

uint32_t random32() {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  uint32_t x = (uint32_t)c.QuadPart ^ (uint32_t)(c.QuadPart >> 32) ^ GetCurrentProcessId() * 2654435761u ^ GetTickCount();
  x ^= x >> 16, x *= 0x7feb352d, x ^= x >> 15, x *= 0x846ca68b, x ^= x >> 16;
  return x;
}

// Every player's name, to every guest of the session (the name tags).
void send_names() {
  LsNamesPacket p{};
  p.session = g_session_id;
  set_name(p.names[0], shown_name().c_str());
  int n = 1;
  for (const Session& s : g_ss)
    if (in_session(s) && s.player < 4) {
      std::memcpy(p.names[s.player], s.name, sizeof(p.names[0]));
      p.names[s.player][31] = '\0';
      n = std::max(n, s.player + 1);
    }
  p.players = (uint8_t)n;
  for (Session& s : g_ss)
    if (in_session(s)) send_packet(s.t, s.peer, p, PT_LsNames, Channel::Control);
}

// v2: a session with the host and every PC that runs the game too, in the
// order they came (players 2..4). All games begin together at the main menu
// with this seed and delay; the delay covers the way between two guests
// (their buttons come through the host) plus a frame.
// The input delay for a way (frames): the guests' buttons come through the
// host (guest -> host -> guest with three or four PCs).
int delay_for(int players, uint32_t rtt_max) {
  int way = players > 2 ? (int)rtt_max : (int)rtt_max / 2;
  return std::clamp(std::max(g_cfg.input_delay, (way + 16) / 17 + 1), 1, 12);
}

uint32_t g_session_at = 0;  // when start_session ran last (net::now_ms)

void start_session() {
  g_restart = false;
  g_session_at = net::now_ms();
  int n = 1;
  uint32_t rtt_max = 0;
  for (Session& s : g_ss) {
    s.player = -1;
    s.ready = s.have_input = false;
    s.where = 0;
    s.ls_version = 0;
    if (!s.t || !s.lockstep) continue;
    s.player = n++;
    rtt_max = std::max(rtt_max, s.t->RttMs(s.peer));
  }
  if (n < 2) return;
  lockstep::Start st;
  st.session = random32() | 1;
  st.seed = g_cfg.debug_session_seed ? g_cfg.debug_session_seed : random32() & 0x7FFFFFFF;
  st.delay = delay_for(n, rtt_max);
  st.players = n;
  st.me = 0;
  g_session_id = st.session;
  g_left = 0;
  LsSavePacket& save = g_save_sent;  // (kept for a PC coming back)
  bool have_save = lockstep::host_save(save.data);
  g_start_save = have_save;
  for (Session& s : g_ss) {
    if (!in_session(s)) continue;
    LsStartPacket p{};
    p.session = st.session;
    p.seed = st.seed;
    p.delay = (uint8_t)st.delay;
    p.players = (uint8_t)n;
    p.you = (uint8_t)s.player;
    lockstep::host_options(p.options);
    p.flags = uint8_t((have_save ? kStartSave : 0) | (revive::enabled() ? kStartRevive : 0) | (revive::button() << 4));
    if (lockstep::host_data(p.data)) p.flags |= kStartData;
    g_start_data = (p.flags & kStartData) != 0;
    g_start_sent = p;
    send_packet(s.t, s.peer, p, PT_LsStart, Channel::Control);
    if (have_save) {
      save.session = st.session;
      save.size = kSaveImage;
      send_packet(s.t, s.peer, save, PT_LsSave, Channel::Control);
    }
  }
  lockstep::net_start(st);
  send_names();
  LOGI("lockstep: start sent to %d other PC%s (rtt up to %u ms, delay %d frames)", n - 1, n > 2 ? "s" : "", rtt_max,
       st.delay);
}

// Every guest of the session can begin (or not): the host's game hears it as one.
void pass_ready() {
  bool all = true, any = false;
  LsReadyPacket r{};
  for (const Session& s : g_ss) {
    if (!in_session(s)) continue;
    any = true;
    if (all && !s.ready) r.where = s.where, r.player = (uint8_t)(s.player + 1);  // (the first that cannot)
    all = all && s.ready;
  }
  r.session = g_session_id;
  r.ready = any && all ? 1 : 0;
  if (r.ready) r.where = (uint8_t)lockstep::Where::Ready, r.player = 0;
  lockstep::net_ready(r);
}

void reject(ITransport* t, const PeerId& peer, RejectReason reason, const char* msg) {
  RejectPacket r{};
  r.reason = reason;
  set_name(r.message, msg);
  send_packet(t, peer, r, PT_Reject, Channel::Control);
  t->Disconnect(peer);
  if (!is_rejected(peer)) g_rejected.push_back(peer);
  LOGW("rejected %s: %s", t->Describe(peer).c_str(), msg);
}

void release_p2() {
  g_p2.buttons.store(0, std::memory_order_release);
  g_p2.latch.store(0, std::memory_order_release);
}

// A guest is gone. In a running session the others play on: its player's
// buttons are none from the frame after the last we passed on (everybody's
// copy of them came from us), and the others hear that with its last ones.
void drop_guest(int i, const char* why) {
  Session& s = g_ss[i];
  if (!s.t) return;
  if (in_session(s) && s.catching && !s.back_at) {
    g_left |= 1u << s.player;  // left again before it was back: its player stays gone
  } else if (in_session(s)) {
    // (also the last one: the session goes on here, its player can come back,
    // until our game is at the main menu again)
    if (lockstep::state() == lockstep::State::Running) {
      g_left |= 1u << s.player;
      LsLeavePacket lp{};
      lp.session = g_session_id;
      lp.player = (uint8_t)s.player;
      lp.frame = s.have_input ? s.newest_input + 1 : s.back_at;
      lp.count = (uint8_t)lockstep::net_history(s.player, lp.frame, lp.buttons, kLsHistory);
      lockstep::net_leave(lp);
      for (int j = 0; j < kGuests; ++j)
        if (j != i && in_session(g_ss[j])) send_packet(g_ss[j].t, g_ss[j].peer, lp, PT_LsLeave, Channel::Control);
    } else if (lockstep_guests() > 1) {
      g_restart = true;  // (not begun yet: again without it)
    } else {
      lockstep::net_end(why);  // the last one
    }
  }
  if (!s.lockstep) {
    g_p2.connected.store(false, std::memory_order_release);
    release_p2();
  }
  LOGI("player %d '%s' (%s) disconnected: %s; input packets=%u lost=%u", s.player > 0 ? s.player + 1 : i + 2, s.name,
       s.t->Describe(s.peer).c_str(), why, s.packets, s.lost);
  s = Session{};
  g_names_out = true;
}

void drop_all(const char* why) {
  for (int i = 0; i < kGuests; ++i) drop_guest(i, why);
}

void handle_hello(ITransport* t, const PeerId& peer, const uint8_t* buf) {
  HelloPacket hello;
  std::memcpy(&hello, buf, sizeof(hello));
  hello.name[sizeof(hello.name) - 1] = '\0';

  int slot = find_guest(t, peer);
  if (g_away != Away::No) {
    reject(t, peer, RR_Busy, "this player is in a friend's game now");
    return;
  }
  // Both sides must run the same tmnt.exe (a client without the game sends zeros).
  if (!is_zero(hello.exe_sha256, 32) && !is_zero(g_exe_hash, 32) &&
      std::memcmp(hello.exe_sha256, g_exe_hash, 32) != 0) {
    char msg[63];
    snprintf(msg, sizeof(msg), "different tmnt.exe: host %s, yours %s", to_hex(g_exe_hash, 4).c_str(),
             to_hex(hello.exe_sha256, 4).c_str());
    reject(t, peer, RR_ExeMismatch, msg);
    return;
  }
  bool ls = (hello.h.flags & kFlagLockstep) != 0;
  if (ls && is_zero(hello.exe_sha256, 32)) {
    reject(t, peer, RR_ExeMismatch, "the game on every PC needs tmnt.exe on both PCs");
    return;
  }
  if (slot < 0) {
    // A streaming P2 (the game only here) plays alone with the host; PCs that
    // run the game too can be three.
    if (!ls && has_guest()) {
      reject(t, peer, RR_Busy, "host already has players: streaming is for one player 2");
      return;
    }
    if (ls && streaming_guest()) {
      reject(t, peer, RR_Busy, "host already has a streaming player 2");
      return;
    }
    slot = free_slot();
    if (slot < 0) {
      reject(t, peer, RR_Busy, "the game is full: four players");
      return;
    }
    Session& s = g_ss[slot];
    s = Session{};
    s.t = t;
    s.peer = peer;
    s.lockstep = ls;
    std::memcpy(s.name, hello.name, sizeof(s.name));
    if (ls && g_left && lockstep::state() == lockstep::State::Running) {
      come_back(s);
      return;
    }
    if (!ls) {
      release_p2();
      g_p2.last_input_ms.store(net::now_ms());
      g_p2.connected.store(true, std::memory_order_release);  // (v1: its buttons go into our keyboard)
    }
    LOGI("player %d '%s' connected from %s (%s)%s", slot + 2, s.name, t->Describe(peer).c_str(), t->Name(),
         ls ? ", running the game too (lockstep)" : "");
  } else {
    // The same guest restarted its session (Steam keeps the peer id): its
    // frame numbers start over.
    g_ss[slot].have_seq = g_ss[slot].have_frame = false;
    LOGI("player %d '%s' said Hello again (session restarted)", slot + 2, g_ss[slot].name);
  }
  Session& s = g_ss[slot];
  HelloAckPacket ack{};
  ack.player = (uint8_t)(slot + 2);
  ack.input_delay = (uint8_t)g_cfg.input_delay;
  std::memcpy(ack.exe_sha256, g_exe_hash, 32);
  set_name(ack.name, shown_name().c_str());
  send_packet(t, peer, ack, PT_HelloAck, Channel::Control, s.lockstep ? kFlagLockstep : 0);
  if (s.lockstep) g_restart = true;  // (a session with it, now or at the next main menu)
}

// --- a PC coming back into the running session ---------------------------------------
// It takes the first player whose PC left. It gets the session's start as it
// was (seed, options, the host's progress then), plays from frame 0 with the
// frames we send it (LsHistory, up to kCatchWindow ahead of its game), and
// once it is near our frame, every PC hears that its player is its own again a
// little ahead (LsReturn); until then that player's buttons stay none.
void come_back(Session& s) {
  int player = 1;
  while (player < 4 && !(g_left & (1u << player))) ++player;
  g_left &= ~(1u << player);
  s.player = player;
  s.catching = true;
  HelloAckPacket ack{};
  ack.player = (uint8_t)(player + 1);
  ack.input_delay = (uint8_t)g_cfg.input_delay;
  std::memcpy(ack.exe_sha256, g_exe_hash, 32);
  set_name(ack.name, shown_name().c_str());
  send_packet(s.t, s.peer, ack, PT_HelloAck, Channel::Control, kFlagLockstep);
  LsStartPacket p = g_start_sent;
  p.you = (uint8_t)player;
  p.flags |= kStartCatchup;
  send_packet(s.t, s.peer, p, PT_LsStart, Channel::Control);
  if (g_start_save) {  // the progress the session began with
    g_save_sent.session = g_session_id;
    g_save_sent.size = kSaveImage;
    send_packet(s.t, s.peer, g_save_sent, PT_LsSave, Channel::Control);
  }
  g_names_out = true;
  LOGI("player %d '%s' comes back into the running game (from %s): it catches up from frame 0 to %u", player + 1,
       s.name, s.t->Describe(s.peer).c_str(), lockstep::frame());
}

void handle_input(Session& g_s, const uint8_t* buf, size_t len, const PacketHeader& h, uint32_t now) {
  // ENet drops reordered packets on the input channel, Steam may deliver them
  // late: the frame numbers below sort that out; seq only feeds the loss stat.
  ++g_s.packets;
  int32_t gap = (int32_t)(h.seq - g_s.last_seq);
  if (!g_s.have_seq || gap > 0) {
    if (g_s.have_seq && gap > 1) g_s.lost += (uint32_t)gap - 1;
    g_s.have_seq = true;
    g_s.last_seq = h.seq;
  }

  InputPacket p{};
  std::memcpy(&p, buf, std::min(len, sizeof(p)));
  uint32_t latch = 0, newest = g_s.newest_frame;
  uint16_t newest_buttons = 0;
  bool any = false;
  for (int i = 0; i < p.count; ++i) {
    const InputFrame& f = p.frames[i];
    if (g_s.have_frame && (int32_t)(f.frame - g_s.newest_frame) <= 0) continue;
    latch |= f.buttons;  // a tap shorter than a game frame still registers
    if (!any || (int32_t)(f.frame - newest) > 0) {
      newest = f.frame;
      newest_buttons = f.buttons;
    }
    any = true;
  }
  if (!any) return;
  g_s.have_frame = true;
  g_s.newest_frame = newest;
  g_p2.latch.fetch_or(latch, std::memory_order_acq_rel);
  g_p2.buttons.store(newest_buttons, std::memory_order_release);
  g_p2.last_input_ms.store(now, std::memory_order_release);
  if (g_s.stale) {
    g_s.stale = false;
    LOGI("P2 input resumed");
  }
  if (newest_buttons != g_s.last_buttons) {
    LOGD("P2 buttons 0x%04X (client frame %u)", newest_buttons, newest);
    g_s.last_buttons = newest_buttons;
  }
}

void handle_receive(ITransport* t, const Event& ev, uint32_t now) {
  if (is_rejected(ev.peer)) return;  // disconnecting
  const uint8_t* data = ev.data.data();
  size_t len = ev.data.size();
  PacketHeader h;
  ParseResult r = parse(data, len, h);
  if (r == ParseResult::BadVersion) {
    char msg[63];
    snprintf(msg, sizeof(msg), "protocol v%u, host speaks v%u: update tmntnet", h.version, kVersion);
    reject(t, ev.peer, RR_Version, msg);
    return;
  }
  if (r != ParseResult::Ok) return;
  if (h.type == PT_Hello) {
    handle_hello(t, ev.peer, data);
    return;
  }
  int i = find_guest(t, ev.peer);
  if (i < 0) return;  // not one of ours (yet)
  Session& s = g_ss[i];
  if (h.type == PT_Input && !s.lockstep) handle_input(s, data, len, h, now);
  if (h.type == PT_LsInput && in_session(s)) {
    LsInputPacket p{};
    std::memcpy(&p, data, std::min(len, sizeof(p)));
    if (p.session != g_session_id || p.player != s.player) return;
    lockstep::net_input(p);
    ++s.packets;
    if (!s.have_input || (int32_t)(p.frame - s.newest_input) > 0) s.newest_input = p.frame, s.have_input = true;
    // The others run the game too: they get its buttons from us.
    for (int j = 0; j < kGuests; ++j)
      if (j != i && in_session(g_ss[j])) g_ss[j].t->Send(g_ss[j].peer, data, len, Channel::Input);
    if (i == 0) g_p2.last_input_ms.store(now, std::memory_order_release);
  }
  if (h.type == PT_LsHash && in_session(s)) {
    LsHashPacket p;
    std::memcpy(&p, data, sizeof(p));
    lockstep::net_hash(p);
  }
  if (h.type == PT_LsCatchup && in_session(s) && s.catching) {
    LsCatchupPacket p;
    std::memcpy(&p, data, sizeof(p));
    if (p.session != g_session_id) return;
    s.at = p.frame;
    s.have = p.have;
    uint32_t ours = lockstep::frame();
    if (!s.back_at && s.at + kNearFrames >= ours) {
      LsReturnPacket rp{};
      rp.session = g_session_id;
      rp.player = (uint8_t)s.player;
      rp.frame = ours + kBackAhead;
      s.back_at = rp.frame;
      lockstep::net_return(rp);
      for (Session& o : g_ss)
        if (in_session(o)) send_packet(o.t, o.peer, rp, PT_LsReturn, Channel::Control);
      LOGI("player %d '%s' is back from frame %u (its game at %u, ours at %u)", s.player + 1, s.name, rp.frame, s.at,
           ours);
    }
    if (s.back_at && s.at >= s.back_at) {
      s.catching = false;
      LOGI("player %d '%s' plays again (frame %u)", s.player + 1, s.name, s.at);
    }
  }
  if (h.type == PT_Barrier && in_session(s)) {
    LsReadyPacket p;
    std::memcpy(&p, data, sizeof(p));
    if (p.session == g_session_id) {
      s.ready = p.ready != 0;
      s.where = p.where;
      s.said_ready = (p.flags & 1) != 0;
      pass_ready();
    }
  }
}

// A PC coming back: the session's frames, up to kCatchWindow ahead of its game.
void send_history(Session& s) {
  uint32_t limit = std::min(lockstep::history_frames(), s.at + kCatchWindow);
  int players = std::clamp<int>(g_start_sent.players, 1, 4);
  static LsHistoryPacket hp;  // (network thread only)
  for (int budget = 8; s.sent < limit && budget > 0; --budget) {
    int n = lockstep::history(s.sent, (int)std::min<uint32_t>(kHistoryFrames, limit - s.sent), players, hp.buttons);
    if (n <= 0) break;
    make_header(hp.h, PT_LsHistory, ++g_seq);
    hp.session = g_session_id;
    hp.from = s.sent;
    hp.count = (uint16_t)n;
    hp.players = (uint8_t)players;
    s.t->Send(s.peer, &hp, offsetof(LsHistoryPacket, buttons) + size_t(n) * players * sizeof(uint16_t), Channel::Control);
    s.sent += (uint32_t)n;
  }
}

// v2: our player's buttons (again every 16 ms, also when nothing is new) and
// the state hashes go to P2.
void send_lockstep() {
  if (!lockstep_guests()) return;
  LsInputPacket in;
  uint32_t version = 0, now = net::now_ms();
  bool have_in = lockstep::net_outgoing(in, version);
  if (have_in) make_header(in.h, PT_LsInput, ++g_seq);
  LsHashPacket hp;
  bool have_hash = lockstep::net_take_hash(hp);
  LsReadyPacket rp{};
  bool have_ready = lockstep::net_ready_out(rp);
  rp.flags = 0;  // who pressed ГОТОВ, for the guests' pages
  for (const Session& s : g_ss)
    if (in_session(s) && s.said_ready && s.player > 0 && s.player < 4) rp.flags |= (uint8_t)(1u << s.player);
  if (g_names_out) {
    g_names_out = false;
    send_names();
  }
  for (Session& s : g_ss) {
    if (!in_session(s)) continue;
    if (s.catching && s.back_at && lockstep::frame() > s.back_at + 120) s.catching = false;  // (well past it)
    if (s.catching) send_history(s);
    if (have_in && (version != s.ls_version || now - s.ls_sent_ms >= 16)) {
      s.ls_version = version;
      s.ls_sent_ms = now;
      s.t->Send(s.peer, &in, offsetof(LsInputPacket, buttons) + in.count * sizeof(uint16_t), Channel::Input);
    }
    if (have_hash) send_packet(s.t, s.peer, hp, PT_LsHash, Channel::Input);
    if (have_ready && now - s.ls_ready_ms >= 100) {
      s.ls_ready_ms = now;
      send_packet(s.t, s.peer, rp, PT_Barrier, Channel::Input);
    }
  }
}

// --- playing in a friend's game (v2 guest) ------------------------------------------

void make_lobby() {
  if (g_steam && !g_lobby && g_away == Away::No) g_lobby = lobby::make_host(g_cfg, lobby::exe16(g_exe_hash));
}

// Back to our own game: leave the friend's, open our lobby again.
void back_home(const char* why) {
  if (g_away == Away::No) return;
  if (g_guest.active()) {
    if (g_guest.up()) g_guest.transport()->Disconnect(g_guest.host());
    g_guest.clear();
  }
  if (g_browser) g_browser->Leave();
  g_away = Away::No;
  g_away_lobby = 0;
  g_away_host = 0;
  g_away_name.clear();
  make_lobby();
  LOGI("network: back to our own game (%s)", why);
}

void begin_join(uint64_t lobby_id) {
  if (!g_browser) return;
  if (g_away != Away::No && g_away_lobby == lobby_id) {
    // (decide on it again while it comes up: leaving and joining again would
    // only start its session over)
    LOGI("network: already in that friend's game (lobby %llu)", (unsigned long long)lobby_id);
    return;
  }
  if (has_guest()) {
    g_away_error = "сначала отключите своих игроков";
    LOGW("network: not joining a friend's game: our players are still here");
    return;
  }
  if (g_away != Away::No) back_home("joining another game");
  g_away = Away::Joining;
  g_away_lobby = lobby_id;
  g_away_error.clear();
  g_lobby.reset();  // nobody joins us while we play in a friend's game
  LOGI("network: joining a friend's game (lobby %llu)", (unsigned long long)lobby_id);
  g_browser->Join(lobby_id);
}

void on_join_result(const lobby::JoinResult& jr, uint32_t now) {
  if (g_away == Away::No) {  // an accepted Steam invite / "Join game" in the friends list
    if (has_guest()) {
      LOGW("network: a Steam invite left aside: our players are still here");
      g_browser->Leave();
      return;
    }
    LOGI("network: joining a friend's game from Steam (invite or the friends list)");
    g_away = Away::Joining;
    g_away_error.clear();
    g_lobby.reset();
  }
  if (!jr.ok || !jr.host_id) {
    g_away_error = jr.error.empty() ? "не удалось войти в игру друга" : jr.error;
    back_home("could not join");
    return;
  }
  g_away = Away::Joined;
  g_away_lobby = jr.lobby;
  g_away_host = jr.host_id;
  g_away_name = jr.host;
  Config as_me = g_cfg;
  as_me.name = shown_name();
  g_guest.reset(g_steam.get(), PeerId::steam(jr.host_id), as_me, g_exe_hash);
  g_away_tried = now;
  g_steam->Connect(PeerId::steam(jr.host_id));
  LOGI("network: in the lobby of '%s', opening the session", jr.host.c_str());
}

void service_away(uint32_t now) {
  if (!g_browser) return;
  g_browser->Tick(now, g_away == Away::No && !has_guest());
  g_found = g_browser->List();
  if (uint64_t id = g_join_cmd.exchange(0)) begin_join(id);
  if (g_leave_cmd.exchange(false)) back_home("left by the player");
  lobby::JoinResult jr;
  if (g_browser->TakeJoinResult(jr)) on_join_result(jr, now);
  if (g_away != Away::Joined) return;
  if (g_guest.refused()) {
    g_away_error = g_guest.refusal();
    back_home("the friend's game refused us");
    return;
  }
  if (!g_guest.up() && now - g_away_tried >= 3000) {  // (again after a drop)
    g_away_tried = now;
    g_steam->Connect(g_guest.host());
  }
  g_guest.tick(now);
}

void on_connect(ITransport* t, const PeerId& peer) {
  forget_rejected(peer);  // a new connection from a rejected address starts over
  if (peer.kind == transport::Kind::Enet && g_allowed.valid() &&
      peer.enet_ip() != g_allowed.sa.sin_addr.s_addr) {
    reject(t, peer, RR_NotAllowed, "host accepts only the peer= address from its tmntnet.ini");
    return;
  }
  LOGD("%s connection from %s, waiting for Hello", t->Name(), t->Describe(peer).c_str());
}

void on_disconnect(ITransport* t, const Event& ev) {
  int i = find_guest(t, ev.peer);
  if (i >= 0) drop_guest(i, ev.reason.c_str());
  forget_rejected(ev.peer);
}

void pump(ITransport* t, uint32_t wait_ms) {
  t->Poll(wait_ms);
  Event evs[16];
  for (int n; (n = t->Receive(evs, 16)) > 0;) {
    uint32_t now = net::now_ms();
    for (int i = 0; i < n; ++i) {
      if (g_guest.active() && t == g_guest.transport() && evs[i].peer == g_guest.host()) {
        switch (evs[i].type) {  // the friend's game we play in
          case EventType::Connected: g_guest.on_connected(); break;
          case EventType::Data: g_guest.on_data(evs[i]); break;
          case EventType::Disconnected: g_guest.on_disconnected(evs[i].reason); break;
        }
        continue;
      }
      switch (evs[i].type) {
        case EventType::Connected: on_connect(t, evs[i].peer); break;
        case EventType::Data: handle_receive(t, evs[i], now); break;
        case EventType::Disconnected: on_disconnect(t, evs[i]); break;
      }
    }
  }
}

void handle_discovery_socket() {
  if (g_disc == INVALID_SOCKET) return;
  uint8_t buf[64];
  net::Addr from;
  int n;
  while ((n = net::recv_from(g_disc, buf, sizeof(buf), from)) > 0) {
    if (n != sizeof(kDiscoveryRequest) || std::memcmp(buf, kDiscoveryRequest, n) != 0) continue;
    DiscoveryReply reply{};
    std::memcpy(reply.tag, "TMNP!", 5);
    reply.version = kVersion;
    reply.port = g_cfg.port;
    set_name(reply.name, g_cfg.name.c_str());
    net::send_to(g_disc, &reply, sizeof(reply), from);
    LOGD("discovery request from %s answered", from.str().c_str());
  }
}

// Steam sessions: lobby members, plus [net] allow_steamid for direct joins.
bool steam_accept(uint64_t id) {
  if (g_away != Away::No) return id == g_away_host;  // only the friend whose game we are in
  if (g_lobby && g_lobby->IsMember(id)) return true;
  return std::find(g_cfg.allow_steamid.begin(), g_cfg.allow_steamid.end(), id) != g_cfg.allow_steamid.end();
}

bool start_enet() {
  // A few peer slots so a second client gets a clean Reject instead of silence.
  auto t = std::make_unique<transport::EnetTransport>(g_cfg.port, 4, (uint32_t)g_cfg.timeout_ms);
  if (!t->Init()) {
    LOGE("cannot open ENet host on UDP %u", g_cfg.port);
    return false;
  }
  g_enet = std::move(t);
  g_disc = net::udp_socket(g_cfg.discovery_port, true);
  if (g_disc == INVALID_SOCKET)
    LOGW("cannot bind discovery port %u; clients must enter a code or IP", g_cfg.discovery_port);
  LOGI("host listening on UDP %u (ENet, discovery %u), protocol v%u, name '%s'", g_cfg.port,
       g_cfg.discovery_port, kVersion, g_cfg.name.c_str());
  return true;
}

// On the network thread, as Steam requires (steam_boot.h).
bool start_steam() {
  if (!steam::boot(g_cfg)) {
    g_steam_note = steam::unavailable_reason();
    return false;
  }
  g_steam = transport::make_steam_transport(steam_accept);
  if (g_steam) g_lobby = lobby::make_host(g_cfg, lobby::exe16(g_exe_hash));
  if (g_steam) g_browser = lobby::make_browser(lobby::exe16(g_exe_hash));
  g_steam_note = g_steam ? "" : "не удалось запустить сеть Steam (см. tmntnet.log)";
  return g_steam != nullptr;
}

// Steam was not running when the game started: it is asked again every few
// seconds and taken up once it runs (invites, friends' games), without a
// restart of the game.
constexpr const char* kSteamNotRunning = "Steam не запущен";
uint32_t g_steam_tried = 0;

void start_transports() {
  if (g_cfg.steam_wanted()) start_steam();
  if (g_cfg.transport != "steam" || !g_steam) {
    if (g_cfg.transport == "steam") LOGW("transport=steam but Steam is unavailable: falling back to ENet");
    start_enet();
  }
}

void stop_transports() {
  LOGI("network: closing (the game is on its way out)");
  drop_all("the host is closing the game");
  if (g_guest.active()) {
    if (g_guest.up()) g_guest.transport()->Disconnect(g_guest.host());
    g_guest.clear();
  }
  g_browser.reset();  // (leaves a friend's lobby)
  g_lobby.reset();
  if (g_steam) g_steam->Shutdown();
  g_steam.reset();
  // No SteamAPI_Shutdown: the game's process ends right after and Steam sees
  // that (0.11.9 crashed once inside Steam right after leaving the lobby at the
  // game's exit, before "Steam shut down").
  if (g_enet) g_enet->Shutdown();
  g_enet.reset();
  LOGI("network: closed (lobby left, sessions closed)");
}

void publish_status() {
  HostStatus st;
  st.started = true;
  st.running = g_enet || g_steam;
  st.port = g_cfg.port;
  st.name = shown_name();
  st.enet = g_enet != nullptr;
  st.steam = g_steam != nullptr;
  if (g_steam) {
    st.steam_name = steam::my_name();
    st.steam_id = steam::my_id();
    st.lobby_id = g_lobby ? g_lobby->LobbyId() : 0;
  }
  st.steam_note = g_steam_note;
  st.steam_mock = steam::is_mock();
  st.friends = g_friends;
  st.away = (int)g_away;
  // (the name its game gave in HelloAck, as its own tags show it)
  st.away_host = g_guest.active() && !g_guest.host_name().empty() ? g_guest.host_name() : g_away_name;
  st.away_error = g_away_error;
  st.away_up = g_guest.active() && g_guest.accepted();
  st.away_rtt = g_guest.active() && g_guest.up() ? g_guest.transport()->RttMs(g_guest.host()) : 0;
  for (const lobby::Found& f : g_found)
    if (f.compatible) st.games.push_back(f);
  for (int i = 0; i < kGuests; ++i) {
    const Session& s = g_ss[i];
    if (!s.t) continue;
    HostStatus::Guest gs;
    gs.player = s.player > 0 ? s.player + 1 : i + 2;
    gs.name = s.name;
    gs.rtt_ms = s.t->RttMs(s.peer);
    gs.ready = s.said_ready;
    st.guests.push_back(gs);
    if (st.p2) continue;  // (the first one also as "player 2", as the pages had it)
    st.p2 = true;
    st.p2_name = s.name;
    st.p2_addr = s.t->Describe(s.peer);
    st.p2_via = s.t->Name();
    st.rtt_ms = s.t->RttMs(s.peer);
    st.p2_input_live = !s.stale;
    st.packets = s.packets;
    st.lost = s.lost;
  }
  if (g_guest.active()) st.away_names = g_guest.names();
  AcquireSRWLockExclusive(&g_status_lock);
  g_status = std::move(st);
  ReleaseSRWLockExclusive(&g_status_lock);
}

DWORD WINAPI net_thread(void*) {
  LOGI("network thread %lu", GetCurrentThreadId());
  start_transports();
  publish_status();
  if (!g_enet && !g_steam) {
    LOGE("no network transport: netplay disabled");
    return 0;
  }
  uint32_t last_publish = 0, last_steam = 0;
  bool lobby_in_game = false;
  while (!g_quit) {
    // ENet waits for traffic; with Steam polled too, or in a lockstep
    // session, only briefly.
    if (g_enet) pump(g_enet.get(), g_steam || lockstep_guests() ? 1 : 5);
    if (g_steam) {
      uint32_t now = net::now_ms();
      if (now - last_steam >= 10) {  // Steam callbacks at ~100 Hz
        last_steam = now;
        steam::pump();
        if (g_lobby) g_lobby->Tick(now);
      }
      pump(g_steam.get(), g_enet ? 0 : 5);
    }
    handle_discovery_socket();
    // Guests changed: a session with them all, at once if none runs, else
    // when the games are at the main menu again (all of them are then).
    if (g_restart && lockstep_guests() &&
        (lockstep::state() != lockstep::State::Running || main_menu::on_screen()))
      start_session();
    if (g_set_again.exchange(false) && lockstep_guests()) start_session();
    // A session set before our game had its data (pads, effects: the game
    // makes them a little after it starts): again with them, before it begins.
    if (lockstep::state() == lockstep::State::Pending && !g_start_data && lockstep_guests()) {
      static uint8_t data[kGameData];
      if (lockstep::host_data(data)) start_session();
    }
    // A session not begun yet, set while the way to a guest was long (Steam
    // starts on its relays and finds a direct way a few seconds later, 150 ->
    // 14 ms in a real test): again with the delay of the way it has now. Not
    // when every guest can begin already (it is about to).
    if (lockstep::state() == lockstep::State::Pending && lockstep_guests() &&
        net::now_ms() - g_session_at >= 3000) {
      int n = 1;
      uint32_t rtt_max = 0;
      bool all_ready = true, measured = true;
      for (const Session& s : g_ss) {
        if (!in_session(s)) continue;
        uint32_t rtt = s.t->RttMs(s.peer);
        measured = measured && rtt > 0;
        rtt_max = std::max(rtt_max, rtt);
        all_ready = all_ready && s.ready;
        ++n;
      }
      int want = delay_for(n, rtt_max);
      if (n > 1 && measured && !all_ready && std::abs(want - (int)g_start_sent.delay) >= 2) {
        LOGI("lockstep: the way to the other PCs is now %u ms (delay %d frames, the session's %d): set again", rtt_max,
             want, (int)g_start_sent.delay);
        start_session();
      }
    }
    // Everybody left a running session of ours: it goes on here (they may come
    // back) until our game is at the main menu again. (Not a friend's session
    // we play in: there we have no guests, and it begins at the main menu.)
    if (g_away == Away::No && lockstep::state() == lockstep::State::Running && !lockstep_guests() &&
        main_menu::on_screen())
      lockstep::net_end("everybody left, and our game is at the main menu");
    send_lockstep();
    if (g_steam) service_away(net::now_ms());
    if (!g_steam && g_cfg.steam_wanted() && g_steam_note == kSteamNotRunning && net::now_ms() - g_steam_tried >= 5000) {
      g_steam_tried = net::now_ms();
      if (start_steam()) LOGI("network: Steam runs now: invites and friends' games are on");
    }

    if (int k = g_kick.exchange(0)) {
      for (int i = 0; i < kGuests; ++i) {
        Session& s = g_ss[i];
        if (!s.t || (k > 0 && (s.player > 0 ? s.player + 1 : i + 2) != k)) continue;
        ITransport* t = s.t;
        PeerId peer = s.peer;
        drop_guest(i, "removed by the host");
        // A Reject (not a plain disconnect) so the client does not reconnect.
        reject(t, peer, RR_Kicked, "the host removed you from the game");
      }
    }
    if (g_lobby) {
      // ISteamNetworkingMessages does not say when a peer leaves: the lobby does.
      uint64_t left = 0;
      while (g_lobby->TakeLeft(left))
        for (int i = 0; i < kGuests; ++i)
          if (g_ss[i].t == g_steam.get() && g_ss[i].peer.id == left) {
            PeerId peer = g_ss[i].peer;
            drop_guest(i, "left the Steam lobby");
            g_steam->Close(peer);
          }
      if (uint64_t who = g_invite.exchange(0)) g_lobby->Invite(who);
      uint32_t now = net::now_ms();
      if (now - g_friends_at >= 3000) {
        g_friends_at = now;
        g_friends = g_lobby->OnlineFriends();
      }
    }
    if (g_lobby && lobby_in_game != has_guest()) {
      lobby_in_game = has_guest();
      g_lobby->SetInGame(lobby_in_game);
    }
    if (net::now_ms() - last_publish >= 100) {
      publish_status();
      last_publish = net::now_ms();
    }

    if (Session& g_s = g_ss[0]; g_s.t) {
      g_p2.rtt_ms.store(g_s.t->RttMs(g_s.peer), std::memory_order_relaxed);
      uint32_t now = net::now_ms();
      if (!g_s.lockstep && !g_s.stale && now - g_p2.last_input_ms.load() > (uint32_t)g_cfg.timeout_ms) {
        g_s.stale = true;
        release_p2();  // P2 just stands; the session stays until the transport times out
        LOGW("no P2 input for %d ms: P2 released", g_cfg.timeout_ms);
      }
    }
  }
  stop_transports();
  return 0;
}

}  // namespace

HostStatus host_status() {
  AcquireSRWLockShared(&g_status_lock);
  HostStatus st = g_status;
  ReleaseSRWLockShared(&g_status_lock);
  return st;
}

void host_kick(int player) { g_kick = player; }
void host_join(uint64_t lobby_id) { g_join_cmd = lobby_id; }
void host_set_again() { g_set_again = true; }
void host_leave() { g_leave_cmd = true; }
void host_invite(uint64_t steam_id) { g_invite = steam_id; }

bool start_net_host(const Config& cfg, const uint8_t exe_sha256[32]) {
  g_cfg = cfg;
  std::memcpy(g_exe_hash, exe_sha256, 32);
  if (!net::startup()) {
    LOGE("WSAStartup failed");
    return false;
  }
  if (!cfg.peer.empty()) {
    if (net::resolve(cfg.peer, 0, g_allowed))
      LOGI("only accepting P2 from %s (ENet)", cfg.peer.c_str());
    else
      LOGW("cannot resolve peer=%s; accepting any P2", cfg.peer.c_str());
  }
  g_thread = CreateThread(nullptr, 0, net_thread, nullptr, 0, nullptr);
  if (!g_thread) {
    LOGE("CreateThread(net) failed");
    return false;
  }
  return true;
}

void stop_net_host() {
  if (!g_thread || g_quit.exchange(true)) return;
  // Short: the game is closing. Leaving the lobby lets the friend see it at once.
  if (WaitForSingleObject(g_thread, 700) == WAIT_TIMEOUT) LOGW("network thread did not stop in time");
}
