#include "mod/p2_binds.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "common/log.h"
#include "mod/known_exe.h"
#include "mod/patch.h"

namespace p2binds {
namespace {

// Binding table getter in TMNT.EXE (0x4b2802 in bc49a82c...):
//   mov eax,[esp+8] ; mov ecx,[esp+4] ; lea eax,[eax+ecx*4] ; shl eax,6
//   add eax, <table> ; ret
// Table layout: int32 code[set][player][16], set 0 = keyboard, 1 = joystick.
constexpr const char* kSig = "8B 44 24 08 8B 4C 24 04 8D 04 88 C1 E0 06 05 ?? ?? ?? ?? C3";
constexpr size_t kSigImmOffset = 15;
constexpr uint32_t kP2KeyboardOffset = (1 /*player*/ + 0 /*set*/ * 4) * 0x40;

const int32_t* g_table_p2kb = nullptr;  // &code[0][1][0] in game memory
int g_fallback[tmnp::kNumSlots];
bool g_auto = true;
int g_last_source = -1;  // 0 = ini, 1 = game table (for one-shot logging)

bool table_valid(const int32_t* t) {
  for (int i = 0; i < tmnp::kNumSlots; ++i)
    if (t[i] != -1 && (t[i] < 1 || t[i] > 255)) return false;
  // Directions (slots 12..15) are always bound; zeros = not initialized yet.
  for (int i = 12; i < 16; ++i)
    if (t[i] < 1) return false;
  return true;
}

}  // namespace

void init(const Config& cfg, const uint8_t exe_sha256[32]) {
  std::memcpy(g_fallback, cfg.p2_keys, sizeof(g_fallback));
  g_auto = cfg.p2_binds_auto;
  if (!g_auto) {
    LOGI("P2 binds: [mod] p2_keys from ini (p2_binds=ini)");
    return;
  }

  uintptr_t table = 0;
  const char* how = "signature";
  if (uintptr_t sig = patch::find("binds_table_getter", kSig))
    table = patch::read<uint32_t>(sig + kSigImmOffset);
  if (!table) {
    if (const KnownExe* k = find_known_exe(exe_sha256)) {
      table = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + k->binds_table_rva;
      how = "known-exe RVA";
    }
  }
  if (!table || !patch::in_image(table, 0x100)) {
    LOGW("P2 binds: game table not found, using [mod] p2_keys from ini");
    return;
  }
  g_table_p2kb = reinterpret_cast<const int32_t*>(table + kP2KeyboardOffset);
  LOGI("P2 binds: game table at 0x%08X (%s), P2 keyboard at 0x%08X", (unsigned)table, how,
       (unsigned)reinterpret_cast<uintptr_t>(g_table_p2kb));
}

void current(int codes[tmnp::kNumSlots]) {
  int source = 0;
  if (g_auto && g_table_p2kb && table_valid(g_table_p2kb)) {
    for (int i = 0; i < tmnp::kNumSlots; ++i) codes[i] = g_table_p2kb[i];
    source = 1;
  } else {
    std::memcpy(codes, g_fallback, sizeof(g_fallback));
  }
  if (source != g_last_source) {
    g_last_source = source;
    char list[160] = "";
    for (int i = 0; i < tmnp::kNumSlots; ++i) {
      char item[8];
      snprintf(item, sizeof(item), i ? ",%d" : "%d", codes[i]);
      strncat(list, item, sizeof(list) - strlen(list) - 1);
    }
    LOGI("P2 keyboard codes now from %s: %s", source ? "game table" : "ini", list);
  }
}

}  // namespace p2binds
