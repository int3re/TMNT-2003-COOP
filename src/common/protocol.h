// Wire protocol shared by tmntnet.asi (host) and tmntnet-client.exe.
// Transport (common/transport.h): ENet over UDP for LAN / direct IP, or Steam
// networking messages; connect/disconnect, RTT and timeouts are the transport's.
// Payloads are little-endian, packed, no pointers or std:: types. Bump
// kVersion on ANY change to a struct layout, channel use or field meaning.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tmnp {

constexpr uint32_t kMagic = 0x504E4D54;  // bytes 'T','M','N','P'
constexpr uint16_t kVersion = 12;  // 1 = raw UDP (tmntnet 0.1), 2 = ENet, 3 = up to four PCs (0.10), 4 = back in a running game (0.11), 5 = ГОТОВ (0.11.13), 6 = costumes (0.12.0: a step more in the select), 7 = own cameras (0.13.0: "on the screen" by every player's view, one view for the game's reckoning), 8 = the story for as many as play (0.13.1: the story menu's cursor), 9 = the host's start as a word in its buttons (0.13.2: bit 9), 10 = nobody held near the game's camera (0.13.5: the turtles' circle), 11 = the pause (0.13.6: bit 8 of anybody's buttons stops every game), 12 = a movie's end (0.13.8: bit 10 of the buttons; everybody in the stages for one turtle)
constexpr uint16_t kDefaultPort = 7777;
constexpr uint16_t kDiscoveryPort = 7778;  // raw UDP broadcast, not ENet
constexpr int kInputHistory = 8;  // frames repeated in every input packet

// Channels (transport::Channel uses the same numbers).
constexpr uint8_t kChannelControl = 0;  // reliable: Hello, HelloAck, Reject, Barrier, config
constexpr uint8_t kChannelInput = 1;    // unreliable sequenced: Input, StateHash
constexpr uint8_t kChannelCount = 2;

enum PacketType : uint8_t {
  PT_Hello = 1,     // client -> host once the transport link is up
  PT_HelloAck = 2,  // host -> client
  PT_Reject = 3,    // host -> client, with a reason; host then disconnects
  PT_Input = 4,     // client -> host, last N frames
  PT_Ping = 5,      // reserved for v2 frame-time sync (the transport measures RTT)
  PT_Pong = 6,      // reserved
  PT_StateHash = 7, // v2 (lockstep) desync detection
  PT_Barrier = 8,   // v2 (lockstep): LsReadyPacket, "my game can begin" while a session waits
  PT_Bye = 9,       // reserved (ENet disconnect is used instead)
  // v2 lockstep: both PCs run the game (Hello/HelloAck with kFlagLockstep).
  PT_LsStart = 10,  // host -> peer, reliable: the session starts (seed, delay, options)
  PT_LsInput = 11,  // both ways, unreliable: the sender's player, its last frames
  PT_LsHash = 12,   // both ways, unreliable: the game state at a frame, to catch drift
  PT_LsSave = 13,   // host -> peer, reliable, right after LsStart: the host's story progress
  // Up to four PCs: the host passes each peer's LsInput on to the others.
  PT_LsNames = 14,  // host -> peers, reliable: every player's name (the name tags)
  PT_LsLeave = 15,  // host -> peers, reliable: a peer left; its player's buttons are none from `frame` on
  // A PC coming back into a running game (its player had left): it plays the
  // session from frame 0 with everybody's buttons, faster than the others, and
  // takes its player again once it has caught up.
  PT_LsHistory = 16,  // host -> that PC, reliable: everybody's buttons, frame by frame, in pieces
  PT_LsCatchup = 17,  // that PC -> host, every 100 ms: how far its game is
  PT_LsReturn = 18,   // host -> every PC, reliable: the player is back; its buttons are its PC's from `frame` on
};
constexpr uint8_t kLastType = PT_LsReturn;  // every type up to it needs a min_size (below)

// Header flag in Hello / HelloAck: the sender runs the game itself (lockstep).
constexpr uint8_t kFlagLockstep = 0x01;
constexpr int kLsHistory = 16;  // frames repeated in every LsInput
constexpr uint32_t kSaveImage = 0x1168;  // the game's savegame.dat image (re/notes.md §14)
constexpr uint8_t kStartSave = 0x01;     // LsStartPacket.flags: an LsSave follows
constexpr uint8_t kStartRevive = 0x02;   // ... the host's [mod] revive is on; its button slot in bits 4..7
constexpr uint8_t kStartCatchup = 0x04;  // ... a PC coming back: begin at once, the frames come as LsHistory
constexpr uint8_t kStartData = 0x08;     // ... `data` holds the host's game data past the save image
constexpr int kGameData = 0x70;          // that data: +0x10E8..+0x1157 (pads' settings, effects flags)
constexpr int kHistoryFrames = 240;      // frames in one LsHistory

enum RejectReason : uint8_t {
  RR_Version = 1,      // protocol version mismatch
  RR_Busy = 2,         // host already has a P2
  RR_NotAllowed = 3,   // host restricts peer= to another address
  RR_ExeMismatch = 4,  // v2: both sides must run the same tmnt.exe
  RR_Kicked = 5,       // the host removed this player; do not reconnect
};

// One bit per game input slot. The order is the game's own: bit i is slot i
// of the per-player binding table (see re/notes.md, "Input slots").
enum Button : uint16_t {
  BTN_DASH = 1u << 0,
  BTN_JUGGLE = 1u << 1,
  BTN_SPECIAL = 1u << 2,
  BTN_SHURIKEN_CHANGE = 1u << 3,
  BTN_SHURIKEN = 1u << 4,
  BTN_JUMP = 1u << 5,
  BTN_ATTACK = 1u << 6,  // weak attack / menu "decide"
  BTN_STRONG = 1u << 7,  // strong attack / menu "cancel"
  BTN_PAUSE = 1u << 8,   // Esc, shared by both players
  BTN_START = 1u << 11,  // Enter / Numpad Enter
  BTN_UP = 1u << 12,
  BTN_RIGHT = 1u << 13,
  BTN_DOWN = 1u << 14,
  BTN_LEFT = 1u << 15,
};
constexpr int kNumSlots = 16;
// Config/action names indexed by slot; nullptr = slot unused by the game.
constexpr const char* kSlotNames[kNumSlots] = {
    "dash", "juggle", "special", "shuriken_change", "shuriken", "jump",
    "attack", "strong", "pause", nullptr, nullptr, "start",
    "up", "right", "down", "left"};

#pragma pack(push, 1)
struct PacketHeader {
  uint32_t magic;
  uint16_t version;
  uint8_t type;
  uint8_t flags;
  uint32_t seq;
};

struct HelloPacket {
  PacketHeader h;
  uint8_t exe_sha256[32];  // all zero if the sender has no tmnt.exe (v1 client)
  char name[32];
};

struct HelloAckPacket {
  PacketHeader h;
  uint8_t player;       // 2 = you control P2
  uint8_t input_delay;  // frames, used by v2
  uint16_t reserved;
  uint8_t exe_sha256[32];
  char name[32];
};

struct RejectPacket {
  PacketHeader h;
  uint8_t reason;
  char message[63];
};

struct InputFrame {
  uint32_t frame;    // sender's monotonically increasing frame counter
  uint16_t buttons;  // Button bits
  int8_t lx, ly;     // analog stick, -127..127 (game only uses digital; kept for v2)
};

struct InputPacket {
  PacketHeader h;
  uint8_t count;  // 1..kInputHistory, frames[0] is the newest
  InputFrame frames[kInputHistory];
};

struct PingPacket {
  PacketHeader h;
  uint32_t sender_ms;  // sender clock, echoed back in Pong
  uint32_t echo_ms;
};

struct ByePacket {
  PacketHeader h;
};

struct LsStartPacket {
  PacketHeader h;
  uint32_t session;   // tags every LsInput / LsHash of this session
  uint32_t seed;      // the game's rand() at frame 0
  uint8_t delay;      // input delay in frames
  uint8_t players;    // 2 for now
  uint8_t you;        // the receiver's player (1 = P2)
  uint8_t flags;      // kStartSave, kStartRevive | revive button << 4
  uint8_t options[24];  // the host's game options +0x58..+0x6F (difficulty...), game_options
  uint8_t data[kGameData];  // with kStartData: the host's game data +0x10E8.. (lockstep.cpp)
};

struct LsInputPacket {
  PacketHeader h;
  uint32_t session;
  uint32_t frame;      // buttons[0] is for this frame, buttons[i] for frame - i
  uint8_t player;
  uint8_t count;       // 1..kLsHistory
  uint16_t buttons[kLsHistory];
};

struct LsNamesPacket {
  PacketHeader h;
  uint32_t session;
  uint8_t players;
  uint8_t reserved[3];
  char names[4][32];  // player 1..4, UTF-8, zero-terminated
};

struct LsLeavePacket {
  PacketHeader h;
  uint32_t session;
  uint32_t frame;      // its buttons are none from this frame on
  uint8_t player;
  uint8_t count;       // buttons[i] is for frame - 1 - i (the last it sent, in case they were lost)
  uint16_t buttons[kLsHistory];
};

struct LsHashPacket {
  PacketHeader h;
  uint32_t session;
  uint32_t frame;
  uint32_t hash;
};

struct LsHistoryPacket {  // sent as long as its buttons are
  PacketHeader h;
  uint32_t session;
  uint32_t from;       // the first frame
  uint16_t count;      // 1..kHistoryFrames frames
  uint8_t players;     // 1..4 buttons a frame
  uint8_t reserved;
  uint16_t buttons[kHistoryFrames * 4];  // [frame - from][player]
};

struct LsCatchupPacket {
  PacketHeader h;
  uint32_t session;
  uint32_t frame;  // the frame its game plays now
  uint32_t have;   // frames it has everybody's buttons of (0..have-1)
};

struct LsReturnPacket {
  PacketHeader h;
  uint32_t session;
  uint32_t frame;  // the player's buttons are its PC's from this frame on
  uint8_t player;
  uint8_t reserved[3];
};

struct LsSavePacket {
  PacketHeader h;
  uint32_t session;
  uint32_t size;  // kSaveImage
  uint8_t data[kSaveImage];
};

struct LsReadyPacket {  // PT_Barrier, both ways, unreliable, every 100 ms until the session runs
  PacketHeader h;
  uint32_t session;
  uint8_t ready;   // at the main menu and taking buttons
  uint8_t where;   // where the game is (lockstep::Where; 0 = not said: before tmntnet 0.11.10)
  uint8_t player;  // host -> peer: the player (2..4) its "where" is of, the first that cannot begin; 0 = not said
  // guest -> host: 1 = its player pressed ГОТОВ (the network page); host -> peer:
  // bit p = player p + 1 pressed it (p 1..3).
  uint8_t flags;
};

// LAN discovery on kDiscoveryPort: request is the 5 bytes "TMNP?".
struct DiscoveryReply {
  char tag[5];  // "TMNP!"
  uint16_t version;
  uint16_t port;
  char name[32];
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 12, "wire layout");
static_assert(sizeof(InputFrame) == 8, "wire layout");
static_assert(sizeof(InputPacket) == 12 + 1 + 8 * kInputHistory, "wire layout");
static_assert(sizeof(HelloPacket) == 76, "wire layout");
static_assert(sizeof(HelloAckPacket) == 80, "wire layout");
static_assert(sizeof(DiscoveryReply) == 41, "wire layout");
static_assert(sizeof(LsStartPacket) == 12 + 12 + 24 + kGameData, "wire layout");
static_assert(sizeof(LsInputPacket) == 12 + 10 + 2 * kLsHistory, "wire layout");
static_assert(sizeof(LsHashPacket) == 24, "wire layout");
static_assert(sizeof(LsReadyPacket) == 20, "wire layout");
static_assert(sizeof(LsNamesPacket) == 12 + 8 + 4 * 32, "wire layout");
static_assert(sizeof(LsLeavePacket) == 12 + 10 + 2 * kLsHistory, "wire layout");
static_assert(sizeof(LsHistoryPacket) == 12 + 12 + 2 * 4 * kHistoryFrames, "wire layout");
static_assert(sizeof(LsCatchupPacket) == 24, "wire layout");
static_assert(sizeof(LsReturnPacket) == 24, "wire layout");

constexpr char kDiscoveryRequest[5] = {'T', 'M', 'N', 'P', '?'};

inline void make_header(PacketHeader& h, PacketType type, uint32_t seq) {
  h.magic = kMagic;
  h.version = kVersion;
  h.type = type;
  h.flags = 0;
  h.seq = seq;
}

// Minimum wire size for a packet type; 0 = unknown type (parse() drops it).
constexpr size_t min_size(uint8_t type) {
  switch (type) {
    case PT_Hello: return sizeof(HelloPacket);
    case PT_HelloAck: return sizeof(HelloAckPacket);
    case PT_Reject: return sizeof(RejectPacket);
    case PT_Input: return offsetof(InputPacket, frames) + sizeof(InputFrame);
    case PT_Ping:
    case PT_Pong: return sizeof(PingPacket);
    case PT_Bye: return sizeof(ByePacket);
    case PT_StateHash: return sizeof(PacketHeader);
    case PT_Barrier: return sizeof(LsReadyPacket);
    case PT_LsStart: return sizeof(LsStartPacket);
    case PT_LsInput: return offsetof(LsInputPacket, buttons) + sizeof(uint16_t);
    case PT_LsHash: return sizeof(LsHashPacket);
    case PT_LsSave: return sizeof(LsSavePacket);
    case PT_LsNames: return sizeof(LsNamesPacket);
    case PT_LsLeave: return sizeof(LsLeavePacket);
    case PT_LsHistory: return offsetof(LsHistoryPacket, buttons) + sizeof(uint16_t);
    case PT_LsCatchup: return sizeof(LsCatchupPacket);
    case PT_LsReturn: return sizeof(LsReturnPacket);
    default: return 0;
  }
}

// A type left out of min_size is dropped as garbage on arrival (0.10.0's
// LsNames and LsLeave were, until a three-PC lab run stalled when one left).
constexpr bool every_type_sized() {
  for (int t = PT_Hello; t <= kLastType; ++t)
    if (min_size((uint8_t)t) == 0) return false;
  return true;
}
static_assert(every_type_sized(), "a packet type without a min_size");

enum class ParseResult { Ok, Garbage, BadVersion };

// Validates magic, version and size. On BadVersion the header is still filled
// so the host can answer with a Reject.
inline ParseResult parse(const void* buf, size_t len, PacketHeader& out) {
  if (len < sizeof(PacketHeader)) return ParseResult::Garbage;
  std::memcpy(&out, buf, sizeof(out));
  if (out.magic != kMagic) return ParseResult::Garbage;
  if (out.version != kVersion) return ParseResult::BadVersion;
  size_t need = min_size(out.type);
  if (need == 0 || len < need) return ParseResult::Garbage;
  if (out.type == PT_Input) {
    uint8_t count = static_cast<const uint8_t*>(buf)[offsetof(InputPacket, count)];
    if (count == 0 || count > kInputHistory) return ParseResult::Garbage;
    if (len < offsetof(InputPacket, frames) + count * sizeof(InputFrame))
      return ParseResult::Garbage;
  }
  if (out.type == PT_LsInput) {
    uint8_t count = static_cast<const uint8_t*>(buf)[offsetof(LsInputPacket, count)];
    if (count == 0 || count > kLsHistory) return ParseResult::Garbage;
    if (len < offsetof(LsInputPacket, buttons) + count * sizeof(uint16_t)) return ParseResult::Garbage;
  }
  if (out.type == PT_LsLeave && static_cast<const uint8_t*>(buf)[offsetof(LsLeavePacket, count)] > kLsHistory)
    return ParseResult::Garbage;
  if (out.type == PT_LsHistory) {
    uint16_t count;
    std::memcpy(&count, static_cast<const uint8_t*>(buf) + offsetof(LsHistoryPacket, count), 2);
    uint8_t players = static_cast<const uint8_t*>(buf)[offsetof(LsHistoryPacket, players)];
    if (count == 0 || count > kHistoryFrames || players == 0 || players > 4) return ParseResult::Garbage;
    if (len < offsetof(LsHistoryPacket, buttons) + size_t(count) * players * sizeof(uint16_t)) return ParseResult::Garbage;
  }
  return ParseResult::Ok;
}

// Copies a C string into a fixed wire field, always NUL-terminated.
template <size_t N>
inline void set_name(char (&dst)[N], const char* src) {
  std::memset(dst, 0, N);
  if (src) std::strncpy(dst, src, N - 1);
}

}  // namespace tmnp
