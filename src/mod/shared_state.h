// State shared between the network thread (writer) and the game thread
// (DirectInput hook, reader). Lock-free: the game thread never waits on the
// network.
#pragma once
#include <atomic>
#include <cstdint>

struct RemotePlayer {
  std::atomic<uint32_t> buttons{0};       // newest received tmnp::Button mask
  std::atomic<uint32_t> latch{0};         // presses seen since the last game frame
  std::atomic<uint32_t> last_input_ms{0}; // net::now_ms() of the newest input
  std::atomic<uint32_t> rtt_ms{0};
  std::atomic<bool> connected{false};
};

extern RemotePlayer g_p2;
extern std::atomic<uint32_t> g_frame;  // keyboard polls = game frames
