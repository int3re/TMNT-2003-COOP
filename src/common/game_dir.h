#pragma once
// Where the game is on this PC: the folder the program runs from, or the
// uninstall entries of the registry. Shared by tmntnet-setup.exe and
// tmntnet-launcher.exe.
#include <windows.h>

#include <string>

namespace gamedir {

inline std::wstring join(const std::wstring& dir, const std::wstring& name) {
  if (dir.empty()) return name;
  wchar_t last = dir.back();
  return (last == L'\\' || last == L'/') ? dir + name : dir + L"\\" + name;
}

inline bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

inline bool has_game(const std::wstring& dir) { return !dir.empty() && exists(join(dir, L"TMNT.EXE")); }

inline std::wstring reg_string(HKEY root, const std::wstring& key, const wchar_t* value, REGSAM view) {
  HKEY k;
  if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ | view, &k) != ERROR_SUCCESS) return L"";
  wchar_t buf[MAX_PATH * 2];
  DWORD size = sizeof(buf), type = 0;
  std::wstring out;
  if (RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS &&
      (type == REG_SZ || type == REG_EXPAND_SZ))
    out.assign(buf, wcsnlen(buf, size / sizeof(wchar_t)));
  RegCloseKey(k);
  return out;
}

inline std::wstring find() {
  const wchar_t* base = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
  for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
    for (REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
      HKEY k;
      if (RegOpenKeyExW(root, base, 0, KEY_READ | view, &k) != ERROR_SUCCESS) continue;
      wchar_t name[256];
      for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(k, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        std::wstring sub = std::wstring(base) + L"\\" + name;
        std::wstring display = reg_string(root, sub, L"DisplayName", view);
        if (display.find(L"Teenage Mutant Ninja Turtles") == std::wstring::npos &&
            display.find(L"TMNT") == std::wstring::npos)
          continue;
        for (const wchar_t* v : {L"InstallLocation", L"Inno Setup: App Path"}) {
          std::wstring dir = reg_string(root, sub, v, view);
          if (has_game(dir)) {
            RegCloseKey(k);
            return dir;
          }
        }
      }
      RegCloseKey(k);
    }
  }
  // Running from the game folder also counts.
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  std::wstring dir(self);
  dir.resize(dir.find_last_of(L'\\'));
  if (has_game(dir)) return dir;
  return L"";
}

}  // namespace gamedir
