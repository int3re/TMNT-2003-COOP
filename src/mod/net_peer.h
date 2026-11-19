// The other PC of a lockstep session (v2: the game on every PC). With [net]
// role=client in the game's tmntnet.ini the game itself joins the host: over
// Steam to host_steamid=, else ENet to peer= (address[:port]). It says Hello
// as a lockstep player and passes the session's packets to lockstep.cpp.
// Network thread; never touches game memory.
#pragma once
#include <cstdint>

#include "common/config.h"

bool start_net_peer(const Config& cfg, const uint8_t exe_sha256[32]);
void stop_net_peer();
