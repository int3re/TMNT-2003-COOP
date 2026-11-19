#include "common/joincode.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace joincode {
namespace {

constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

int digit(char c) {
  c = (char)std::toupper((unsigned char)c);
  if (c == 'O') c = '0';                // Crockford: O reads as 0
  if (c == 'I' || c == 'L') c = '1';    //           I and L read as 1
  for (int i = 0; i < 32; ++i)
    if (kAlphabet[i] == c) return i;
  return -1;
}

}  // namespace

std::string encode(uint32_t ip, uint16_t port, uint16_t default_port) {
  uint32_t v = ntohl(ip);
  char body[8];
  for (int i = 6; i >= 0; --i) {  // 7 digits * 5 bits = 35 >= 32
    body[i] = kAlphabet[v & 31];
    v >>= 5;
  }
  body[7] = '\0';
  std::string code = "TMNT-" + std::string(body, 4) + "-" + std::string(body + 4, 3);
  if (port != default_port) code += "-" + std::to_string(port);
  return code;
}

bool decode(const std::string& text, uint16_t default_port, uint32_t& ip, uint16_t& port) {
  std::string t;
  for (char c : text)
    if (!std::isspace((unsigned char)c)) t += c;
  port = default_port;

  // Plain address: "1.2.3.4" or "1.2.3.4:7777".
  std::string host = t;
  size_t colon = t.find(':');
  if (colon != std::string::npos) {
    host = t.substr(0, colon);
    int p = std::atoi(t.c_str() + colon + 1);
    if (p <= 0 || p > 65535) return false;
    port = (uint16_t)p;
  }
  in_addr a{};
  if (inet_pton(AF_INET, host.c_str(), &a) == 1) {
    ip = a.s_addr;
    return true;
  }

  // Join code: optional "TMNT" prefix, 7 base32 digits, optional "-port".
  std::string s = t;
  for (auto& c : s) c = (char)std::toupper((unsigned char)c);
  if (s.rfind("TMNT", 0) == 0) s = s.substr(4);
  std::string digits, port_part;
  size_t i = 0;
  for (; i < s.size() && digits.size() < 7; ++i) {
    if (s[i] == '-') continue;
    if (digit(s[i]) < 0) return false;
    digits += s[i];
  }
  if (digits.size() != 7) return false;
  for (; i < s.size(); ++i)
    if (s[i] != '-') port_part += s[i];
  uint64_t v = 0;
  for (char c : digits) v = (v << 5) | (uint64_t)digit(c);
  if (v > 0xFFFFFFFFull) return false;
  if (!port_part.empty()) {
    int p = std::atoi(port_part.c_str());
    if (p <= 0 || p > 65535) return false;
    port = (uint16_t)p;
  }
  ip = htonl((uint32_t)v);
  return true;
}

}  // namespace joincode
