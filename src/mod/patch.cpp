#include "mod/patch.h"

#include <windows.h>

#include <MinHook.h>

#include <cstdlib>
#include <vector>

#include "common/log.h"

namespace patch {
namespace {

struct Image {
  uintptr_t begin = 0, end = 0;
  const uint8_t* text = nullptr;
  size_t text_size = 0;
};

const Image& image() {
  static Image img = [] {
    Image i;
    auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    i.begin = reinterpret_cast<uintptr_t>(base);
    i.end = i.begin + nt->OptionalHeader.SizeOfImage;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
      if (std::memcmp(sec->Name, ".text", 5) == 0) {
        i.text = base + sec->VirtualAddress;
        i.text_size = sec->Misc.VirtualSize;
      }
    }
    return i;
  }();
  return img;
}

bool parse_pattern(const char* pattern, std::vector<int>& out) {
  out.clear();
  for (const char* p = pattern; *p;) {
    if (*p == ' ') {
      ++p;
    } else if (p[0] == '?') {
      out.push_back(-1);
      p += (p[1] == '?') ? 2 : 1;
    } else {
      char* end = nullptr;
      long v = std::strtol(p, &end, 16);
      if (end == p || v < 0 || v > 255) return false;
      out.push_back((int)v);
      p = end;
    }
  }
  return !out.empty();
}

}  // namespace

std::vector<uintptr_t> find_all(const char* pattern) {
  std::vector<uintptr_t> out;
  std::vector<int> pat;
  if (!parse_pattern(pattern, pat)) return out;
  const Image& img = image();
  for (size_t i = 0; img.text && i + pat.size() <= img.text_size; ++i) {
    size_t j = 0;
    while (j < pat.size() && (pat[j] < 0 || img.text[i + j] == pat[j])) ++j;
    if (j == pat.size()) out.push_back(reinterpret_cast<uintptr_t>(img.text + i));
  }
  return out;
}

std::vector<uintptr_t> find_calls(uintptr_t target) {
  std::vector<uintptr_t> out;
  const Image& img = image();
  for (size_t i = 0; img.text && i + 5 <= img.text_size; ++i) {
    if (img.text[i] != 0xE8) continue;
    uintptr_t at = reinterpret_cast<uintptr_t>(img.text + i);
    if (at + 5 + read<int32_t>(at + 1) == target) out.push_back(at);
  }
  return out;
}

int redirect_calls(const char* name, uintptr_t target, void* detour) {
  int n = 0;
  for (uintptr_t at : find_calls(target)) {
    int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(detour) - (at + 5));
    n += write(at + 1, &rel, 4) ? 1 : 0;
  }
  LOGI("redirected %d call(s) of %s at 0x%08X", n, name, (unsigned)target);
  return n;
}

uintptr_t find(const char* name, const char* pattern) {
  std::vector<uintptr_t> all = find_all(pattern);
  if (all.size() != 1) {
    LOGW("signature %s: %d matches (need exactly 1), feature disabled", name, (int)all.size());
    return 0;
  }
  LOGD("signature %s at 0x%08X", name, (unsigned)all[0]);
  return all[0];
}

bool in_image(uintptr_t addr, size_t size) {
  const Image& img = image();
  return addr >= img.begin && addr + size <= img.end;
}

bool write(uintptr_t addr, const void* bytes, size_t n) {
  DWORD old;
  void* p = reinterpret_cast<void*>(addr);
  if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return false;
  std::memcpy(p, bytes, n);
  VirtualProtect(p, n, old, &old);
  FlushInstructionCache(GetCurrentProcess(), p, n);
  return true;
}

bool hook(const char* name, uintptr_t target, void* detour, void** original) {
  static bool initialized = false;
  if (!initialized) {
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
      LOGE("MH_Initialize: %s", MH_StatusToString(st));
      return false;
    }
    initialized = true;
  }
  void* t = reinterpret_cast<void*>(target);
  MH_STATUS st = MH_CreateHook(t, detour, original);
  if (st == MH_OK) st = MH_EnableHook(t);
  if (st != MH_OK) {
    LOGE("hook %s at %p: %s", name, t, MH_StatusToString(st));
    return false;
  }
  LOGI("hooked %s at 0x%08X", name, (unsigned)target);
  return true;
}

}  // namespace patch
