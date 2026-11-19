// Runtime patching helpers for tmnt.exe: signature scan over .text, guarded
// byte writes and MinHook hooks. Nothing here touches the exe on disk.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace patch {

// Finds the unique match of an IDA-style pattern ("8B 44 24 ?? C3") in the
// exe's .text. Returns 0 (and logs) if there are zero or several matches.
uintptr_t find(const char* name, const char* pattern);

// Every match of a pattern in .text (for code the game duplicates per menu).
std::vector<uintptr_t> find_all(const char* pattern);

// Every direct call (E8 rel32) in .text to `target`.
std::vector<uintptr_t> find_calls(uintptr_t target);

// Points every direct call to `target` at `detour` instead (for a function a
// hook at its start would break, e.g. one that loops back into its first five
// bytes); `target` itself stays as it is. Returns how many were changed.
int redirect_calls(const char* name, uintptr_t target, void* detour);

// True if [addr, addr+size) lies inside the exe image.
bool in_image(uintptr_t addr, size_t size);

// Writes bytes into code/rdata (VirtualProtect + FlushInstructionCache).
bool write(uintptr_t addr, const void* bytes, size_t n);

template <typename T>
T read(uintptr_t addr) {
  T v;
  std::memcpy(&v, reinterpret_cast<const void*>(addr), sizeof(T));
  return v;
}

// MinHook create+enable (initializes MinHook once). Logs the outcome.
bool hook(const char* name, uintptr_t target, void* detour, void** original);

}  // namespace patch
