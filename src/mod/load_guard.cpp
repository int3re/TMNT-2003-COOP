#include "mod/load_guard.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/log.h"
#include "mod/patch.h"

namespace load_guard {
namespace {

// RwTexDictionarySetCurrent (0x4bb280): the current dictionary is
// [engine] + [offset] + 0x10 (engine at +2, offset at +8).
constexpr const char* kSetCurrentSig = "8B 0D ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? 8B 44 24 04 89 44 0A 10 C3";
// RenderWare 3: a dictionary +8 the head of its textures' list; a texture +8
// its link in that list, +0x10 its name (+0x54 its references, 0x4bb2b0).
constexpr size_t kDictTextures = 0x08, kTextureLink = 0x08, kTextureName = 0x10;
// 0x4aa445(file): the dictionary in the game's loader's buffer of that file
// (entries 0x6228d0 + file * 16: +0 status, +4 buffer, +8 size; the files of
// TMNT.DAT by number). Its first call (+0x0D) reads the entry's buffer: movzx,
// shl 4, mov eax, [eax + entries + 4] (that address at +10 of it).
constexpr const char* kDictSig = "55 8B EC 83 EC 1C 56 57 FF 75 08 33 FF E8 ?? ?? ?? ?? FF 75 08 89 45 F8 E8";
constexpr size_t kBufferCall = 0x0D;
constexpr size_t kEntrySize = 16, kEntryBuffer = 4, kEntryLength = 8;

using Dict_t = void*(__cdecl*)(int file);
using SetCurrent_t = void*(__cdecl*)(void* dict);
Dict_t g_dict_from_file = nullptr;
SetCurrent_t g_set_current = nullptr;
uint8_t* const* g_engine = nullptr;
const int32_t* g_offset = nullptr;
uint8_t* g_entries = nullptr;
bool g_break = false;  // [debug] break_dict

void* current() { return *g_engine ? *reinterpret_cast<void**>(*g_engine + *g_offset + 0x10) : nullptr; }

bool has_texture(const void* dict, const char* name) {
  if (!dict) return false;
  const uint8_t* head = static_cast<const uint8_t*>(dict) + kDictTextures;
  const uint8_t* link = *reinterpret_cast<const uint8_t* const*>(head);
  for (int n = 0; link && link != head && n < 4096; ++n) {
    if (strncmp(reinterpret_cast<const char*>(link - kTextureLink + kTextureName), name, 32) == 0) return true;
    link = *reinterpret_cast<const uint8_t* const*>(link);
  }
  return false;
}

// A file of TMNT.DAT by name (CRI AFS: "AFS\0", count, count x {offset, size},
// then the directory's {offset, size}: 0x30 bytes a file, its name first).
bool read_file(const char* want, int& number, std::vector<uint8_t>& out) {
  std::wstring path = module_dir(nullptr) + L"TMNT.DAT";
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  auto read_at = [f](uint32_t at, void* to, DWORD n) {
    LARGE_INTEGER pos;
    pos.QuadPart = at;
    DWORD got = 0;
    return SetFilePointerEx(f, pos, nullptr, FILE_BEGIN) && ReadFile(f, to, n, &got, nullptr) && got == n;
  };
  uint32_t head[2] = {}, dir[2] = {};
  std::vector<uint32_t> toc;
  std::vector<char> names;
  bool ok = read_at(0, head, 8) && std::memcmp(head, "AFS", 4) == 0 && head[1] > 0 && head[1] < 65536;
  if (ok) {
    toc.resize(size_t(head[1]) * 2);
    names.resize(size_t(head[1]) * 0x30);
    ok = read_at(8, toc.data(), DWORD(toc.size() * 4)) && read_at(8 + 8 * head[1], dir, 8) && dir[0] &&
         read_at(dir[0], names.data(), DWORD(names.size()));
  }
  number = -1;
  for (uint32_t i = 0; ok && i < head[1]; ++i)
    if (_strnicmp(&names[size_t(i) * 0x30], want, 32) == 0) number = (int)i;
  ok = ok && number >= 0 && toc[size_t(number) * 2 + 1] > 0 && toc[size_t(number) * 2 + 1] < (64u << 20);
  if (ok) {
    out.resize(toc[size_t(number) * 2 + 1]);
    ok = read_at(toc[size_t(number) * 2], out.data(), DWORD(out.size()));
  }
  CloseHandle(f);
  return ok;
}

}  // namespace

void install(const Config& cfg) {
  g_break = cfg.debug_break_dict != 0;
  if (uintptr_t a = patch::find("texture_dictionary_set_current", kSetCurrentSig)) {
    g_engine = reinterpret_cast<uint8_t* const*>(patch::read<uint32_t>(a + 2));
    g_offset = reinterpret_cast<const int32_t*>(patch::read<uint32_t>(a + 8));
    if (patch::in_image((uintptr_t)g_engine, 4) && patch::in_image((uintptr_t)g_offset, 4))
      g_set_current = reinterpret_cast<SetCurrent_t>(a);
  }
  if (uintptr_t a = patch::find("texture_dictionary_from_loader", kDictSig)) {
    uintptr_t get = a + kBufferCall;
    get = patch::read<uint8_t>(get) == 0xE8 ? get + 5 + patch::read<int32_t>(get + 1) : 0;
    if (get && patch::in_image(get, 16) &&
        std::memcmp(reinterpret_cast<const void*>(get), "\x0F\xB7\x44\x24\x04\xC1\xE0\x04\x8B\x80", 10) == 0) {
      uintptr_t entries = patch::read<uint32_t>(get + 10) - kEntryBuffer;
      if (patch::in_image(entries, kEntrySize)) {
        g_entries = reinterpret_cast<uint8_t*>(entries);
        g_dict_from_file = reinterpret_cast<Dict_t>(a);
      }
    }
  }
  if (!g_set_current || !g_dict_from_file) LOGW("load guard: RenderWare's dictionaries not found; the menu is not looked at");
}

void ensure(const char* texture, const char* file) {
  if (!g_set_current || !g_dict_from_file) return;
  if (g_break) {  // (lab) as the crash had it
    g_break = false;
    g_set_current(nullptr);
    LOGI("load guard: [debug] break_dict: no current dictionary");
  }
  void* dict = current();
  if (has_texture(dict, texture)) return;
  LOGW("load guard: %s is not in the current texture dictionary (%p); %s again from TMNT.DAT", texture, dict, file);
  int number = -1;
  std::vector<uint8_t> data;
  if (!read_file(file, number, data) || data.size() < 12 || (data[0] != 0x16 && data[0] != 0x23)) {
    LOGW("load guard: %s not read from TMNT.DAT", file);
    return;
  }
  // The game's own reading on these bytes: the loader's entry for that file
  // lent them for the call.
  uint8_t* e = g_entries + size_t(number) * kEntrySize;
  uint8_t*& buffer = *reinterpret_cast<uint8_t**>(e + kEntryBuffer);
  uint32_t& length = *reinterpret_cast<uint32_t*>(e + kEntryLength);
  uint8_t* kept_buffer = buffer;
  uint32_t kept_length = length;
  buffer = data.data();
  length = (uint32_t)data.size();
  void* made = g_dict_from_file(number);
  buffer = kept_buffer;
  length = kept_length;
  if (made) g_set_current(made);
  LOGI("load guard: %s made again (file %d): %s", file, number,
       made && has_texture(made, texture) ? "the menu has its textures" : "STILL WITHOUT");
}

}  // namespace load_guard
