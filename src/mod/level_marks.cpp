#include "mod/level_marks.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "common/log.h"
#include "mod/lockstep.h"
#include "mod/patch.h"

namespace level_marks {
namespace {

// The story flow (0x494503(task data)): state 9: 0x4049ab(0); 0x49493a([data
// +0xC]) starts the level's scene (0x4ab5bd(0x5e61d8, arg)); state 10: when
// 0x4ab428() says that scene is over, state 11 (what the flags say: on to the
// next stage, back to the title, ...).
constexpr const char* kStartSig = "6A 00 E8 ?? ?? ?? ?? FF 76 0C E8 ?? ?? ?? ?? C7 46 04 0A 00 00 00";
constexpr size_t kStartCall = 10;
constexpr const char* kEndSig = "E8 ?? ?? ?? ?? 85 C0 0F 84 ?? ?? ?? ?? C7 46 04 0B 00 00 00";
// (lab) The game's info ([0x60a438], 0x1D0 bytes) and config ([0x60eae4],
// 0x1158 bytes): heap blocks that live from the start to the end.
constexpr uintptr_t kInfoHolder = 0x60a438, kConfigHolder = 0x60eae4;
constexpr size_t kInfoSize = 0x1D0, kConfigSize = 0x1158;

using Start_t = void(__cdecl*)(void* arg);
using Ended_t = int(__cdecl*)();
Start_t g_start = nullptr;
Ended_t g_ended = nullptr;
bool g_dump = false;
int g_dumps = 0;
std::wstring g_dir;

void put(FILE* f, uintptr_t addr, size_t size) {
  const uint32_t head[2] = {(uint32_t)addr, (uint32_t)size};
  fwrite(head, 4, 2, f);
  fwrite(reinterpret_cast<const void*>(addr), 1, size, f);
}

void dump(const char* what) {
  wchar_t name[96];
  swprintf(name, 96, L"tmntnet-dump-%02d-%hs.bin", g_dumps, what);
  FILE* f = _wfopen((g_dir + name).c_str(), L"wb");
  if (!f) return;
  // The exe's writable sections (its statics).
  auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
  auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  size_t total = 0;
  for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    if (sec[i].Characteristics & IMAGE_SCN_MEM_WRITE) {
      put(f, reinterpret_cast<uintptr_t>(base) + sec[i].VirtualAddress, sec[i].Misc.VirtualSize);
      total += sec[i].Misc.VirtualSize;
    }
  for (auto [holder, size] : {std::pair<uintptr_t, size_t>{kInfoHolder, kInfoSize}, {kConfigHolder, kConfigSize}}) {
    const uintptr_t block = patch::in_image(holder, 4) ? patch::read<uint32_t>(holder) : 0;
    if (block && !IsBadReadPtr(reinterpret_cast<const void*>(block), size)) put(f, block, size), total += size;
  }
  fclose(f);
  LOGI("level marks (lab): %hs of a level at frame %u: %u bytes in %ls", what, lockstep::frame(), (unsigned)total, name);
}

void __cdecl start_level(void* arg) {
  if (g_dump) ++g_dumps, dump("start");
  g_start(arg);
}

int __cdecl level_ended() {
  const int r = g_ended();
  if (r && g_dump) dump("end");
  return r;
}

}  // namespace

void install(const Config& cfg) {
  g_dump = cfg.debug_level_dump;
  if (!g_dump) return;  // (nothing else yet)
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  g_dir = path;
  g_dir.resize(g_dir.find_last_of(L'\\') + 1);
  const uintptr_t start = patch::find("story_level_start", kStartSig), end = patch::find("story_level_end", kEndSig);
  auto target_of = [](uintptr_t call) { return call + 5 + patch::read<int32_t>(call + 1); };
  if (!start || !end) return;
  g_start = reinterpret_cast<Start_t>(target_of(start + kStartCall));
  g_ended = reinterpret_cast<Ended_t>(target_of(end));
  if (!patch::in_image((uintptr_t)g_start, 1) || !patch::in_image((uintptr_t)g_ended, 1)) return;
  const int32_t rel_start = (int32_t)(reinterpret_cast<uintptr_t>(&start_level) - (start + kStartCall + 5));
  const int32_t rel_end = (int32_t)(reinterpret_cast<uintptr_t>(&level_ended) - (end + 5));
  const bool ok = patch::write(start + kStartCall + 1, &rel_start, 4) && patch::write(end + 1, &rel_end, 4);
  LOGI("level marks (lab): the story's levels' starts and ends are dumped%s", ok ? "" : ": not in place");
}

}  // namespace level_marks
