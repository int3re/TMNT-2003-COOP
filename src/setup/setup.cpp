// tmntnet-setup.exe: installs / uninstalls tmntnet on any PC.
//   host:   Ultimate ASI Loader (winmm.dll) + tmntnet.asi + tmntnet.ini into
//           the game folder (TMNT.EXE itself is never modified)
//   client: tmntnet-client.exe + tmntnet.ini (role=client) into any folder
// Every installed file and every backup is listed in tmntnet-install.txt in
// the target folder; uninstall removes exactly those and restores backups.
//
// Command line (no UI): /host <dir> | /client <dir> | /uninstall <dir>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common/game_dir.h"
#include "common/version.h"
#include "setup/payload_ids.h"

namespace {

const wchar_t* kTitle = L"TMNT Netplay " TEXT(TMNTNET_VERSION);
const wchar_t* kManifest = L"tmntnet-install.txt";
const wchar_t* kBackupDir = L"tmntnet-backup";
bool g_quiet = false;

// --- small helpers ----------------------------------------------------------

std::wstring join(const std::wstring& dir, const std::wstring& name) {
  if (dir.empty()) return name;
  wchar_t last = dir.back();
  return (last == L'\\' || last == L'/') ? dir + name : dir + L"\\" + name;
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// A quiet run started by tmntnet-launcher.exe has no console to print to: with
// TMNTNET_SETUP_LOG set, what it would say goes into that file as well (UTF-8).
void log_for_launcher(const std::wstring& text) {
  wchar_t path[MAX_PATH * 2];
  DWORD n = GetEnvironmentVariableW(L"TMNTNET_SETUP_LOG", path, MAX_PATH * 2);
  if (!n || n >= MAX_PATH * 2) return;
  HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  std::wstring line = text + L"\n";
  int bytes = WideCharToMultiByte(CP_UTF8, 0, line.data(), (int)line.size(), nullptr, 0, nullptr, nullptr);
  std::string utf8(bytes, '\0');
  WideCharToMultiByte(CP_UTF8, 0, line.data(), (int)line.size(), &utf8[0], bytes, nullptr, nullptr);
  DWORD written = 0;
  WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
  CloseHandle(f);
}

void message(const std::wstring& text, bool error = false) {
  if (g_quiet) {
    fwprintf(error ? stderr : stdout, L"%ls\n", text.c_str());
    log_for_launcher(text);
    return;
  }
  MessageBoxW(nullptr, text.c_str(), kTitle, MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}

std::wstring last_error_text(DWORD err) {
  wchar_t* buf = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, err, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
  std::wstring s = buf ? buf : L"";
  if (buf) LocalFree(buf);
  while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
  return s + L" (" + std::to_wstring(err) + L")";
}

bool read_resource(int id, const void*& data, DWORD& size) {
  HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
  HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
  data = g ? LockResource(g) : nullptr;
  size = r ? SizeofResource(nullptr, r) : 0;
  return data && size;
}

bool write_file(const std::wstring& path, const void* data, DWORD size, DWORD& err) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    err = GetLastError();
    return false;
  }
  DWORD written = 0;
  bool ok = WriteFile(f, data, size, &written, nullptr) && written == size;
  err = ok ? 0 : GetLastError();
  CloseHandle(f);
  return ok;
}

std::string read_all(const std::wstring& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool file_contains(const std::wstring& path, const char* needle) {
  return read_all(path).find(needle) != std::string::npos;
}

// --- manifest ------------------------------------------------------------------

struct Manifest {
  std::vector<std::wstring> files;                                  // installed by us
  std::vector<std::pair<std::wstring, std::wstring>> backups;       // original -> backup

  bool load(const std::wstring& dir) {
    std::wifstream in(join(dir, kManifest));
    if (!in) return false;
    std::wstring line;
    while (std::getline(in, line)) {
      if (line.rfind(L"file\t", 0) == 0) files.push_back(line.substr(5));
      if (line.rfind(L"backup\t", 0) == 0) {
        size_t tab = line.find(L'\t', 7);
        if (tab != std::wstring::npos)
          backups.emplace_back(line.substr(7, tab - 7), line.substr(tab + 1));
      }
    }
    return true;
  }
  bool save(const std::wstring& dir, const wchar_t* kind) const {
    std::wofstream out(join(dir, kManifest));
    out << L"# tmntnet " << TEXT(TMNTNET_VERSION) << L" " << kind
        << L" install. tmntnet-setup.exe /uninstall uses this list.\n";
    for (auto& f : files) out << L"file\t" << f << L"\n";
    for (auto& b : backups) out << L"backup\t" << b.first << L"\t" << b.second << L"\n";
    return static_cast<bool>(out);
  }
};

// --- install / uninstall ------------------------------------------------------

enum class Result { Ok, Failed, AccessDenied };

// Puts one payload file into dir. An existing file we did not install is moved
// to tmntnet-backup\ first (and restored on uninstall).
Result put(const std::wstring& dir, int id, const std::wstring& name, Manifest& m,
           std::wstring& log, const std::string* override_data = nullptr) {
  std::wstring dst = join(dir, name);
  bool ours = false;
  for (auto& f : m.files) ours = ours || _wcsicmp(f.c_str(), name.c_str()) == 0;
  if (exists(dst) && !ours) {
    std::wstring bdir = join(dir, kBackupDir);
    CreateDirectoryW(bdir.c_str(), nullptr);
    std::wstring bak = join(bdir, name);
    if (!MoveFileExW(dst.c_str(), bak.c_str(), MOVEFILE_REPLACE_EXISTING)) {
      DWORD e = GetLastError();
      log += L"не удалось сохранить старый " + name + L": " + last_error_text(e) + L"\n";
      return e == ERROR_ACCESS_DENIED ? Result::AccessDenied : Result::Failed;
    }
    m.backups.emplace_back(name, std::wstring(kBackupDir) + L"\\" + name);
    log += L"старый " + name + L" сохранён в " + kBackupDir + L"\\\n";
  }
  const void* data = nullptr;
  DWORD size = 0, err = 0;
  if (override_data) {
    data = override_data->data();
    size = (DWORD)override_data->size();
  } else if (!read_resource(id, data, size)) {
    log += L"в установщике нет файла " + name + L"\n";
    return Result::Failed;
  }
  if (!write_file(dst, data, size, err)) {
    log += L"не удалось записать " + name + L": " + last_error_text(err) + L"\n";
    return err == ERROR_ACCESS_DENIED ? Result::AccessDenied : Result::Failed;
  }
  if (!ours) m.files.push_back(name);
  log += name + L"\n";
  return Result::Ok;
}

std::string payload_text(int id) {
  const void* data = nullptr;
  DWORD size = 0;
  return read_resource(id, data, size) ? std::string(static_cast<const char*>(data), size) : "";
}

// key=value in the ini's text (the first line that starts with "key="): the
// value replaced, a comment after it kept. False if there is no such line.
bool set_ini_value(std::string& ini, const char* key, const std::string& value, std::string* old = nullptr) {
  std::string k = std::string("\n") + key + "=";
  size_t p = ini.find(k);
  if (p == std::string::npos) return false;
  size_t v = p + k.size(), e = ini.find_first_of(" \t;\r\n", v);
  if (e == std::string::npos) e = ini.size();
  if (old) *old = ini.substr(v, e - v);
  ini.replace(v, e - v, value);
  return true;
}

std::string with_role(std::string ini, const char* role) {
  set_ini_value(ini, "role", role);
  return ini;
}

// The recommended settings go into a tmntnet.ini kept from an earlier install
// once (a new one has them; afterwards they are the player's): [video]
// display=borderless — the whole screen like full screen, but Alt+Tab is
// instant, Steam's and Discord's windows open over the game, and a game
// together does not stop when someone looks away. The rest stays (resolution,
// wide screen, quality are tmntnet's recommended ones from the start). The file
// before is kept in tmntnet-backup (uninstall removes it).
constexpr const char* kRecommendedMark = "recommended-settings=1";
const wchar_t* kIniBefore = L"tmntnet-backup\\tmntnet.ini.before-recommended";

void apply_recommended(const std::wstring& dir, Manifest& m, std::wstring& log) {
  std::wstring path = join(dir, L"tmntnet.ini");
  std::string ini = read_all(path);
  if (ini.empty() || ini.find(kRecommendedMark) != std::string::npos) return;
  // Full screen becomes borderless (a window or borderless chosen stays).
  std::string out = ini, old, probe = ini;
  const char* nl = ini.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  bool display = set_ini_value(probe, "display", "borderless", &old) && (old == "fullscreen" || old == "exclusive");
  if (display) out = probe;
  // The mark, after the comments at the file's top.
  size_t at = out.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
  while (at < out.size() && out[at] == ';') {
    size_t e = out.find('\n', at);
    at = e == std::string::npos ? out.size() : e + 1;
  }
  out.insert(at, std::string("; ") + kRecommendedMark + " (установщик ставит рекомендованные настройки один раз; дальше они ваши)" + nl);
  CreateDirectoryW(join(dir, kBackupDir).c_str(), nullptr);
  DWORD err = 0;
  if (!CopyFileW(path.c_str(), join(dir, kIniBefore).c_str(), FALSE) || !write_file(path, out.data(), (DWORD)out.size(), err)) {
    log += L"tmntnet.ini: рекомендованные настройки не записаны: " + last_error_text(err ? err : GetLastError()) + L"\n";
    return;
  }
  bool listed = false;
  for (auto& f : m.files) listed = listed || _wcsicmp(f.c_str(), kIniBefore) == 0;
  if (!listed) m.files.push_back(kIniBefore);
  if (display) log += L"tmntnet.ini: экран — без рамки на весь монитор (рекомендуется); прежний файл: " + std::wstring(kIniBefore) + L"\n";
}

// Steam transport files, only when this installer carries steam_api.dll (a
// build with the Steamworks SDK). steam_appid.txt = 480: tmntnet runs as
// Valve's test app "Spacewar" in Steam.
Result put_steam(const std::wstring& dir, Manifest& m, std::wstring& log) {
  const void* data = nullptr;
  DWORD size = 0;
  if (!read_resource(RES_STEAM, data, size)) return Result::Ok;
  Result r = put(dir, RES_STEAM, L"steam_api.dll", m, log);
  std::string app_id = "480";
  if (r == Result::Ok) r = put(dir, 0, L"steam_appid.txt", m, log, &app_id);
  return r;
}

// savesetting.dat (932 bytes): version 2, int32 keyboard keys of player 1
// and 2 (16 slots each) from offset 4, a byte sum at 0x3A0.
constexpr size_t kSettingsSize = 0x3A4, kSettingsSum = 0x3A0, kSettingsKeys = 4;
constexpr int kMouseKey = 0xF0, kMouseButtons = 5;
// The game's keys (Базовые Установки), slots in its order.
const int32_t kOriginalKeys[2][16] = {
    {0x12, 0x13, 0x1D, 0x2A, 0x1F, 0x39, 0x21, 0x20, 0x01, -1, -1, 0x1C, 0xC8, 0xCD, 0xD0, 0xCB},
    {0x4E, 0x4D, 0x49, 0x48, 0x47, 0x52, 0x4B, 0x4C, 0x01, -1, -1, 0x9C, 0xC7, 0xD1, 0xCF, 0xD3}};

uint8_t settings_sum(const std::string& s) {
  uint8_t v = 0;
  for (size_t i = 0; i < kSettingsSum; ++i) v = (uint8_t)(v + (uint8_t)s[i]);
  return v;
}

bool settings_ok(const std::string& s) {
  int32_t version = 0;
  if (s.size() != kSettingsSize) return false;
  memcpy(&version, s.data(), 4);
  return version == 2 && settings_sum(s) == (uint8_t)s[kSettingsSum];
}

int32_t key_at(const std::string& s, int player, int slot) {
  int32_t code = 0;
  memcpy(&code, s.data() + kSettingsKeys + (player * 16 + slot) * 4, 4);
  return code;
}

bool is_mouse_key(int32_t code) { return code >= kMouseKey && code < kMouseKey + kMouseButtons; }

// Installed again after Удалить: the mouse buttons the uninstall took out of
// the key table (savesetting.dat.tmntnet: the file as it was) come back where
// the slot still has the key the uninstall put there (a real case, 0.11.10:
// LMB / RMB attacks became F / D, D being the player's "right"). The file
// before goes to savesetting.dat.tmntnet in turn.
void bring_back_mouse_keys(const std::wstring& dir, std::wstring& log) {
  std::wstring path = join(dir, L"savesetting.dat"), kept_path = join(dir, L"savesetting.dat.tmntnet");
  std::string now = read_all(path), kept = read_all(kept_path);
  if (!settings_ok(now) || !settings_ok(kept)) return;
  int changed = 0;
  for (int p = 0; p < 2; ++p)
    for (int slot = 0; slot < 16; ++slot) {
      int32_t was = key_at(kept, p, slot), is = key_at(now, p, slot);
      if (!is_mouse_key(was) || is_mouse_key(is) || was == is) continue;
      // (only what the uninstall did: the game's own key or a spare one, none
      // of the player's other keys changed since)
      bool others_same = true;
      for (int k = 0; k < 16; ++k)
        if (k != slot && !is_mouse_key(key_at(kept, p, k)) && key_at(kept, p, k) != key_at(now, p, k)) others_same = false;
      if (!others_same) continue;
      memcpy(&now[kSettingsKeys + (p * 16 + slot) * 4], &was, 4);
      ++changed;
    }
  if (!changed) return;
  now[kSettingsSum] = (char)settings_sum(now);
  DWORD err = 0;
  if (!CopyFileW(path.c_str(), kept_path.c_str(), FALSE) || !write_file(path, now.data(), (DWORD)now.size(), err)) {
    log += L"savesetting.dat: кнопки мыши в таблицу клавиш не вернулись: " + last_error_text(err ? err : GetLastError()) + L"\n";
    return;
  }
  log += L"savesetting.dat: кнопки мыши снова в таблице клавиш, как до удаления мода (" + std::to_wstring(changed) +
         L"; прежний файл: savesetting.dat.tmntnet)\n";
}

Result install_host(const std::wstring& dir, std::wstring& log) {
  if (!exists(join(dir, L"TMNT.EXE"))) {
    log += L"в папке нет TMNT.EXE\n";
    return Result::Failed;
  }
  Manifest m;
  m.load(dir);
  Result r = Result::Ok;
  // An Ultimate ASI Loader that is already there (for another mod) is reused.
  std::wstring loader = join(dir, L"winmm.dll");
  bool ours = false;
  for (auto& f : m.files) ours = ours || _wcsicmp(f.c_str(), L"winmm.dll") == 0;
  if (exists(loader) && !ours && file_contains(loader, "IsUltimateASILoader")) {
    log += L"winmm.dll: уже стоит Ultimate ASI Loader, используется он\n";
  } else {
    r = put(dir, RES_UAL, L"winmm.dll", m, log);
  }
  if (r == Result::Ok) r = put(dir, RES_ASI, L"tmntnet.asi", m, log);
  if (r == Result::Ok) r = put_steam(dir, m, log);
  if (r == Result::Ok) {
    std::wstring ini = join(dir, L"tmntnet.ini");
    bool ini_ours = false;
    for (auto& f : m.files) ini_ours = ini_ours || _wcsicmp(f.c_str(), L"tmntnet.ini") == 0;
    if (exists(ini) && ini_ours) {
      log += L"tmntnet.ini: оставлены ваши настройки\n";
      apply_recommended(dir, m, log);
    } else {
      std::string text = with_role(payload_text(RES_INI), "host");
      r = put(dir, RES_INI, L"tmntnet.ini", m, log, &text);
    }
  }
  if (r == Result::Ok) r = put(dir, RES_README, L"tmntnet-README.md", m, log);
  if (r == Result::Ok) bring_back_mouse_keys(dir, log);
  // tmntnet 0.1 used its own dinput8.dll loader; it is no longer needed.
  std::wstring old_shim = join(dir, L"dinput8.dll");
  if (r == Result::Ok && exists(old_shim) && file_contains(old_shim, "[dinput8 shim]")) {
    DeleteFileW(old_shim.c_str());
    log += L"удалён старый загрузчик dinput8.dll (tmntnet 0.1)\n";
  }
  m.save(dir, L"host");
  return r;
}

Result install_client(const std::wstring& dir, std::wstring& log) {
  if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
    DWORD e = GetLastError();
    log += L"не удалось создать папку: " + last_error_text(e) + L"\n";
    return e == ERROR_ACCESS_DENIED ? Result::AccessDenied : Result::Failed;
  }
  Manifest m;
  m.load(dir);
  Result r = put(dir, RES_CLIENT, L"tmntnet-client.exe", m, log);
  if (r == Result::Ok) r = put_steam(dir, m, log);
  if (r == Result::Ok) {
    bool ini_ours = false;
    for (auto& f : m.files) ini_ours = ini_ours || _wcsicmp(f.c_str(), L"tmntnet.ini") == 0;
    if (exists(join(dir, L"tmntnet.ini")) && ini_ours) {
      log += L"tmntnet.ini: оставлены ваши настройки\n";
    } else {
      std::string text = with_role(payload_text(RES_INI), "client");
      r = put(dir, RES_INI, L"tmntnet.ini", m, log, &text);
    }
  }
  if (r == Result::Ok) r = put(dir, RES_README, L"tmntnet-README.md", m, log);
  m.save(dir, L"client");
  return r;
}

// Mouse buttons set in the game's key table are stored in savesetting.dat as
// codes 0xF0..0xF4, which only tmntnet presses: without it those actions would
// have no key (and Слабая Атака is also "decide" in the menus). They get the
// game's own key back. The file (932 bytes: version 2, int32 keyboard keys of
// player 1 and 2 from offset 4, a byte sum at 0x3A0) is copied first.
void restore_mouse_keys(const std::wstring& dir, std::wstring& log) {
  std::wstring path = join(dir, L"savesetting.dat");
  std::string s = read_all(path);
  if (!settings_ok(s)) return;
  // A key no other action of either player has: the game's own for the slot,
  // else one of these (the game's own was taken: WASD players have D, the
  // game's strong attack, as "right", 0.11.10).
  static const int32_t kSpare[] = {0x24, 0x25, 0x26, 0x16, 0x17, 0x18, 0x23, 0x22, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32};
  auto taken = [&](int32_t code) {
    for (int p = 0; p < 2; ++p)
      for (int slot = 0; slot < 16; ++slot)
        if (key_at(s, p, slot) == code) return true;
    return false;
  };
  int changed = 0;
  for (int p = 0; p < 2; ++p)
    for (int slot = 0; slot < 16; ++slot) {
      if (!is_mouse_key(key_at(s, p, slot))) continue;
      int32_t key = kOriginalKeys[p][slot];
      for (size_t i = 0; key > 0 && taken(key) && i < sizeof(kSpare) / sizeof(kSpare[0]); ++i)
        if (!taken(kSpare[i])) key = kSpare[i];
      memcpy(&s[kSettingsKeys + (p * 16 + slot) * 4], &key, 4);
      ++changed;
    }
  if (!changed) return;
  s[kSettingsSum] = (char)settings_sum(s);
  std::wstring copy = join(dir, L"savesetting.dat.tmntnet");
  DWORD err = 0;
  if (!CopyFileW(path.c_str(), copy.c_str(), FALSE)) {
    log += L"savesetting.dat: кнопки мыши в таблице клавиш оставлены (не удалось сделать копию: " +
           last_error_text(GetLastError()) + L")\n";
    return;
  }
  if (!write_file(path, s.data(), (DWORD)s.size(), err)) {
    log += L"savesetting.dat: не удалось записать: " + last_error_text(err) + L"\n";
    return;
  }
  log += L"savesetting.dat: вместо кнопок мыши в таблице клавиш снова клавиши игры (" + std::to_wstring(changed) +
         L"; прежний файл: savesetting.dat.tmntnet)\n";
}

Result uninstall(const std::wstring& dir, std::wstring& log) {
  Manifest m;
  if (!m.load(dir)) {
    log += L"в папке нет " + std::wstring(kManifest) + L": tmntnet здесь не установлен\n";
    return Result::Failed;
  }
  Result r = Result::Ok;
  for (auto& f : m.files) {
    std::wstring p = join(dir, f);
    if (exists(p) && !DeleteFileW(p.c_str())) {
      DWORD e = GetLastError();
      log += L"не удалось удалить " + f + L": " + last_error_text(e) + L"\n";
      r = e == ERROR_ACCESS_DENIED ? Result::AccessDenied : Result::Failed;
    } else {
      log += L"удалён " + f + L"\n";
    }
  }
  for (auto& b : m.backups) {
    if (MoveFileExW(join(dir, b.second).c_str(), join(dir, b.first).c_str(),
                    MOVEFILE_REPLACE_EXISTING))
      log += L"восстановлен " + b.first + L"\n";
  }
  for (const wchar_t* extra : {L"tmntnet.log", L"tmntnet-client.log"})
    DeleteFileW(join(dir, extra).c_str());
  RemoveDirectoryW(join(dir, kBackupDir).c_str());  // only if empty
  // (Only once tmntnet.asi is gone: a game still running with it would write
  // its keys back at exit.)
  if (r == Result::Ok) restore_mouse_keys(dir, log);
  if (r == Result::Ok) {
    DeleteFileW(join(dir, kManifest).c_str());
    RemoveDirectoryW(dir.c_str());  // a client folder we created; a game folder is never empty
  }
  return r;
}

// --- finding the game ---------------------------------------------------------

// (The registry's uninstall entries, or the folder this program lies in: the
// launcher looks the same way, common/game_dir.h.)
std::wstring find_game_dir() { return gamedir::find(); }

bool pick_folder(const wchar_t* title, const std::wstring& initial, std::wstring& out, HWND owner = nullptr) {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dlg))))
    return false;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dlg->SetTitle(title);
  IShellItem* init = nullptr;
  if (!initial.empty() &&
      SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&init)))) {
    dlg->SetFolder(init);
    init->Release();
  }
  bool ok = false;
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        out = path;
        CoTaskMemFree(path);
        ok = true;
      }
      item->Release();
    }
  }
  dlg->Release();
  return ok;
}

std::wstring default_client_dir() {
  PWSTR docs = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) dir = docs;
  CoTaskMemFree(docs);
  return join(dir, L"TMNT Netplay Client");
}

// --- UI -------------------------------------------------------------------------

enum Action { kNone, kHost, kClient, kUninstall };

int task_dialog(const wchar_t* main, const wchar_t* content,
                const std::vector<std::pair<int, std::wstring>>& buttons, bool links) {
  std::vector<TASKDIALOG_BUTTON> b;
  for (auto& x : buttons) b.push_back({x.first, x.second.c_str()});
  TASKDIALOGCONFIG c{sizeof(c)};
  c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | (links ? TDF_USE_COMMAND_LINKS : 0);
  c.pszWindowTitle = kTitle;
  c.pszMainInstruction = main;
  c.pszContent = content;
  c.cButtons = (UINT)b.size();
  c.pButtons = b.data();
  c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
  int pressed = IDCANCEL;
  TaskDialogIndirect(&c, &pressed, nullptr, nullptr);
  return pressed;
}

// The window: two buttons and the game folder (found by itself: the
// registry, or the installer lying next to TMNT.EXE; the link picks another).
constexpr int kInstallButton = 100, kRemoveButton = 101;
const wchar_t* kPickTitle = L"Папка игры (где лежит TMNT.EXE)";

bool game_in(const std::wstring& dir) { return !dir.empty() && exists(join(dir, L"TMNT.EXE")); }
bool installed_in(const std::wstring& dir) { return !dir.empty() && exists(join(dir, kManifest)); }

struct MainDialog {
  std::wstring dir;
  std::wstring text;  // the content shown (kept alive for the dialog)
  void describe() {
    text = game_in(dir) ? L"Папка игры: " + dir + L"\n<a href=\"pick\">Другая папка</a>"
                        : L"Папка игры не найдена. <a href=\"pick\">Указать папку игры</a>";
  }
};

HRESULT CALLBACK main_dialog_proc(HWND hwnd, UINT msg, WPARAM, LPARAM, LONG_PTR data) {
  MainDialog& d = *reinterpret_cast<MainDialog*>(data);
  if (msg == TDN_HYPERLINK_CLICKED) {
    std::wstring picked;
    if (pick_folder(kPickTitle, d.dir, picked, hwnd)) d.dir = picked;
    d.describe();
    SendMessageW(hwnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, reinterpret_cast<LPARAM>(d.text.c_str()));
  }
  if (msg == TDN_CREATED || msg == TDN_HYPERLINK_CLICKED)
    SendMessageW(hwnd, TDM_ENABLE_BUTTON, kRemoveButton, installed_in(d.dir) ? TRUE : FALSE);
  return S_OK;
}

Action ask_action(std::wstring& dir) {
  MainDialog d;
  d.dir = find_game_dir();
  d.describe();
  TASKDIALOG_BUTTON b[] = {{kInstallButton, L"Установить мод"}, {kRemoveButton, L"Удалить"}};
  TASKDIALOGCONFIG c{sizeof(c)};
  c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_ENABLE_HYPERLINKS;
  c.pszWindowTitle = kTitle;
  c.pszMainInstruction = L"Сетевая игра для TMNT (2003)";
  c.pszContent = d.text.c_str();
  c.cButtons = 2;
  c.pButtons = b;
  c.nDefaultButton = kInstallButton;
  c.pfCallback = main_dialog_proc;
  c.lpCallbackData = reinterpret_cast<LONG_PTR>(&d);
  int pressed = IDCANCEL;
  if (FAILED(TaskDialogIndirect(&c, &pressed, nullptr, nullptr))) return kNone;
  dir = d.dir;
  return pressed == kInstallButton ? kHost : pressed == kRemoveButton ? kUninstall : kNone;
}

// No folder with TMNT.EXE (or with tmntnet in it, to remove): the player shows it.
bool pick_until(std::wstring& dir, bool (*good)(const std::wstring&), const wchar_t* missing) {
  while (!good(dir)) {
    if (!dir.empty()) message(std::wstring(missing) + L"\n" + dir + L"\n\nУкажите папку игры.", true);
    std::wstring picked;
    if (!pick_folder(kPickTitle, dir, picked)) return false;
    dir = picked;
  }
  return true;
}

bool confirm_dir(const wchar_t* what, std::wstring& dir, const wchar_t* pick_title) {
  for (;;) {
    std::wstring text = std::wstring(what) + L"\n\n" + (dir.empty() ? L"(папка не выбрана)" : dir);
    std::vector<std::pair<int, std::wstring>> b = {{201, L"Выбрать другую папку"}};
    if (!dir.empty()) b.insert(b.begin(), {200, L"Продолжить"});
    int r = task_dialog(L"Папка", text.c_str(), b, false);
    if (r == 200) return true;
    if (r != 201) return false;
    std::wstring picked;
    if (pick_folder(pick_title, dir, picked)) dir = picked;
  }
}

// Restarts elevated with an explicit command line (game in Program Files).
bool relaunch_elevated(const std::wstring& args) {
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  SHELLEXECUTEINFOW sei{sizeof(sei)};
  sei.lpVerb = L"runas";
  sei.lpFile = self;
  sei.lpParameters = args.c_str();
  sei.nShow = SW_SHOWNORMAL;
  return ShellExecuteExW(&sei) != FALSE;
}

int run(Action action, std::wstring dir, bool interactive) {
  std::wstring log;
  Result r = Result::Failed;
  const wchar_t* flag = L"";
  if (action == kHost) {
    if (interactive && dir.empty()) dir = find_game_dir();
    if (interactive && !pick_until(dir, game_in, L"В этой папке нет TMNT.EXE:")) return 1;
    r = install_host(dir, log);
    flag = L"/host";
  } else if (action == kClient) {
    if (interactive && dir.empty()) dir = default_client_dir();
    if (interactive && !confirm_dir(L"Куда установить клиент:", dir, L"Папка для клиента"))
      return 1;
    r = install_client(dir, log);
    flag = L"/client";
  } else if (action == kUninstall) {
    if (interactive && dir.empty()) dir = find_game_dir();
    if (interactive && !pick_until(dir, installed_in, L"В этой папке мод не установлен:")) return 1;
    r = uninstall(dir, log);
    flag = L"/uninstall";
  }

  if (r == Result::AccessDenied && interactive) {
    if (relaunch_elevated(std::wstring(flag) + L" \"" + dir + L"\"")) return 0;
  }
  if (r == Result::Ok) {
    std::wstring done;
    if (action == kHost)
      done = L"Мод установлен.\n\n" + dir +
             L"\n\nЗапускайте игру как обычно. Игра по сети — пункт СЕТЕВАЯ ИГРА в главном меню.\n"
             L"Настройки уже рекомендованные: на весь экран без рамки, широкий формат, высокое качество "
             L"(меняются в игре: ПАРАМЕТРЫ → ПАРАМЕТРЫ ГРАФИКИ).";
    else if (action == kUninstall)
      done = L"Мод удалён: игра — как до установки.\n\n" + dir;
    else
      done = L"Готово. Установлено в:\n" + dir + L"\n\nЗапускайте tmntnet-client.exe, когда хост запустит игру.";
    if (g_quiet) done += L"\n\n" + log;  // (the files, for scripts)
    message(done);
    if (action == kClient && interactive)
      ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return 0;
  }
  message(L"Не получилось.\n\n" + log, true);
  return 2;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  Action action = kNone;
  std::wstring dir;
  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    if (a == L"/quiet") g_quiet = true;
    else if (a == L"/host") action = kHost;
    else if (a == L"/client") action = kClient;
    else if (a == L"/uninstall") action = kUninstall;
    else dir = a;
  }
  LocalFree(argv);
  // Print to the parent's console when there is one. Started without any (by
  // tmntnet-launcher.exe), stdout stays as it is and a print does nothing; a
  // freopen that fails would close it, and the next print would end the
  // program: 0.13.9 and older ended so, with the install already done.
  if (g_quiet && AttachConsole(ATTACH_PARENT_PROCESS)) {
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
  }

  int rc;
  if (action != kNone) {
    rc = run(action, dir, !g_quiet);
  } else {
    Action a = ask_action(dir);
    rc = a == kNone ? 1 : run(a, dir, true);
  }
  CoUninitialize();
  return rc;
}
