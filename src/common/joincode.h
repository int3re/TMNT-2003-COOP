// Join codes: an IPv4 address (+ port if not the default) as a short code a
// player can read out, e.g. "TMNT-4KQ7-9XB" or "TMNT-4KQ7-9XB-7790".
// Crockford base32 (no I, L, O, U), case-insensitive when decoding.
#pragma once
#include <cstdint>
#include <string>

namespace joincode {

// ip in network byte order (as in sockaddr_in::sin_addr.s_addr).
std::string encode(uint32_t ip, uint16_t port, uint16_t default_port);

// Accepts a join code, "1.2.3.4" or "1.2.3.4:port". Returns false if neither.
bool decode(const std::string& text, uint16_t default_port, uint32_t& ip, uint16_t& port);

}  // namespace joincode
