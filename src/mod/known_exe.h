// Executables the RVA fallbacks were verified against (see re/notes.md).
// Signatures are tried first; RVAs are used only when the hash matches.
#pragma once
#include <cstdint>
#include <cstring>

struct KnownExe {
  const char* label;
  uint8_t sha256[32];
  uint32_t binds_table_rva;  // live binding table, [set][player][slot] int32
};

// TMNT.EXE, Konami 2003 US retail, 2150400 bytes, PE timestamp 0x3F6002F8.
// sha256 bc49a82c8234fb80ac071ad43698598cfa104c363688ed12d248b89b7573093e
constexpr KnownExe kKnownExes[] = {
    {"US retail 2003",
     {0xbc, 0x49, 0xa8, 0x2c, 0x82, 0x34, 0xfb, 0x80, 0xac, 0x07, 0x1a,
      0xd4, 0x36, 0x98, 0x59, 0x8c, 0xfa, 0x10, 0x4c, 0x36, 0x36, 0x88,
      0xed, 0x12, 0xd2, 0x48, 0xb8, 0x9b, 0x75, 0x73, 0x09, 0x3e},
     0x29F5F0},
};

inline const KnownExe* find_known_exe(const uint8_t sha256[32]) {
  for (const auto& k : kKnownExes)
    if (std::memcmp(k.sha256, sha256, 32) == 0) return &k;
  return nullptr;
}
