#include "mod/game_options.h"

#include <cstdint>
#include <cstring>

#include "common/log.h"
#include "mod/binds.h"
#include "mod/patch.h"

namespace game_options {
namespace {

// Getter / setter of the options' +0x6C (0x410b77 / 0x410b8c): both copy the
// 0x70-byte options block out of the config object; the setter writes it back
// and re-applies it (0x410ad5), as the game's options screen does.
constexpr const char* kGetSig = "55 8B EC 83 EC 70 8D 45 90 50 E8 ?? ?? ?? ?? 8B 45 FC 59 C9 C3";
constexpr const char* kSetSig =
    "55 8B EC 83 EC 70 8D 45 90 50 E8 ?? ?? ?? ?? 8B 45 08 89 45 FC 8D 45 90 50 E8 ?? ?? ?? ?? 59 59 C9 C3";
constexpr int kAutosaveBit = 1;
// The whole block: get 0x410abb(out) copies the 0x70 bytes out, set
// 0x410ad5(in) writes them back and re-applies them.
constexpr const char* kBlockGetSig = "E8 ?? ?? ?? ?? 8B 45 FC 59 C9 C3";  // inside kGetSig: +0 call get
constexpr size_t kBlockSize = 0x70, kPlayAt = 0x58;

using Get_t = int(__cdecl*)();
using Set_t = void(__cdecl*)(int);
Get_t g_get = nullptr;
Set_t g_set = nullptr;
using Block_t = void(__cdecl*)(uint8_t* block);
Block_t g_block_get = nullptr, g_block_set = nullptr;

}  // namespace

void init() {
  if (uintptr_t a = patch::find("options_misc_get", kGetSig)) {
    g_get = reinterpret_cast<Get_t>(a);
    g_block_get = reinterpret_cast<Block_t>(a + 15 + patch::read<int32_t>(a + 11));  // its call (+10)
  }
  if (uintptr_t a = patch::find("options_misc_set", kSetSig)) {
    g_set = reinterpret_cast<Set_t>(a);
    g_block_set = reinterpret_cast<Block_t>(a + 30 + patch::read<int32_t>(a + 26));  // its second call (+25)
  }
  LOGI("game options: autosave switch %s", ready() ? "available" : "not found");
}

bool ready() { return g_get && g_set; }

bool autosave() { return g_get && (g_get() & kAutosaveBit) != 0; }

void set_autosave(bool on) {
  if (!ready()) return;
  int v = g_get();
  g_set(on ? v | kAutosaveBit : v & ~kAutosaveBit);
  binds::save_settings();
  LOGI("game options: autosave %s (saved to savesetting.dat)", on ? "on" : "off");
}

bool get_play(uint8_t out[kPlayBytes]) {
  if (!g_block_get) return false;
  uint8_t block[kBlockSize];
  g_block_get(block);
  std::memcpy(out, block + kPlayAt, kPlayBytes);
  return true;
}

bool set_play(const uint8_t in[kPlayBytes]) {
  if (!g_block_get || !g_block_set) return false;
  uint8_t block[kBlockSize];
  g_block_get(block);
  std::memcpy(block + kPlayAt, in, kPlayBytes);
  g_block_set(block);
  return true;
}

}  // namespace game_options
