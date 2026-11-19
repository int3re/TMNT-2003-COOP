#include "mod/autosave.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "common/log.h"
#include "common/net.h"
#include "mod/game_options.h"
#include "mod/main_menu.h"
#include "mod/patch.h"
#include "mod/lockstep.h"
#include "mod/native_menu.h"

namespace autosave {
namespace {

// TMNT.EXE bc49a82c...: serialize(buf) (0x4aa585) builds the 0x1168-byte
// savegame.dat image from the progress in the config object, version byte and
// checksum included. The story flow, after a cleared stage (stage number
// already moved on), asks 0x494428 whether this is a save point (chapter end);
// that is the one place where every cleared stage passes.
constexpr const char* kSerializeSig =
    "56 8B 74 24 08 8D 46 08 57 50 C6 06 03 E8 ?? ?? ?? ?? 8D 86 30 01 00 00";
constexpr const char* kSavePointSig = "E8 ?? ?? ?? ?? 83 F8 34 74 6A E8 ?? ?? ?? ?? 83 F8 06 74 60";
constexpr size_t kSaveSize = 0x1168, kChecksumAt = 0x1160;
constexpr uint8_t kVersion = 3;

using Serialize_t = void(__cdecl*)(uint8_t* buf);
using SavePoint_t = int(__cdecl*)();
Serialize_t g_serialize = nullptr;
SavePoint_t oSavePoint = nullptr;

Config g_cfg;
std::atomic<bool> g_on{true};
int g_known = -1;  // the game's АВТОЗАПИСЬ as last seen (-1: not applied yet)
std::wstring g_path, g_tmp, g_bak;

bool image(std::vector<uint8_t>& buf) {
  if (!g_serialize) return false;
  buf.assign(kSaveSize, 0);
  g_serialize(buf.data());
  uint8_t sum = 0;
  for (size_t i = 0; i < kChecksumAt; ++i) sum = (uint8_t)(sum + buf[i]);
  return buf[0] == kVersion && buf[kChecksumAt] == sum;
}

int __cdecl hkSavePoint() {
  int chapter_end = oSavePoint();
  // At a chapter end with АВТОЗАПИСЬ on the game saves by itself right after.
  if (g_on.load() && !(chapter_end && game_options::autosave())) save_now("stage cleared");
  return chapter_end;
}

}  // namespace

void install(const Config& cfg) {
  g_cfg = cfg;
  g_on = cfg.autosave;
  std::wstring dir = module_dir(nullptr);
  g_path = dir + L"savegame.dat";
  g_tmp = g_path + L".tmp";
  g_bak = g_path + L".bak";
  if (uintptr_t a = patch::find("save_serialize", kSerializeSig)) g_serialize = reinterpret_cast<Serialize_t>(a);
  uintptr_t point = patch::find("story_save_point", kSavePointSig);
  if (!g_serialize || !point ||
      !patch::hook("story_save_point", point, (void*)&hkSavePoint, (void**)&oSavePoint)) {
    LOGW("autosave: game code not recognised, saving after each stage is off");
    return;
  }
  LOGI("autosave: %s", cfg.autosave ? "after every cleared stage" : "off ([mod] autosave=0)");
}

bool enabled() { return g_on.load(); }

void set_enabled(bool on) {
  g_on = on;
  if (game_options::ready() && game_options::autosave() != on) game_options::set_autosave(on);
  g_known = on ? 1 : 0;
}

void poll() {
  // (In a game together the options are the host's, or ours lent to it.)
  if (!game_options::ready() || lockstep::borrowed()) return;
  static uint32_t last = 0;
  uint32_t now = net::now_ms();
  if (now - last < 500) return;
  last = now;
  bool game = game_options::autosave();
  if (g_known < 0) {  // first look: the game has loaded its settings by now
    if (g_on.load() && !game) {
      game_options::set_autosave(true);
      LOGI("autosave: turned the game's АВТОЗАПИСЬ on ([mod] autosave=1)");
      game = true;
    }
    g_known = game ? 1 : 0;
    return;
  }
  if ((game ? 1 : 0) != g_known) {  // switched in the game's own ПРОЧИЕ ПАРАМЕТРЫ
    g_known = game ? 1 : 0;
    g_on = game;
    ini_set(module_dir(nullptr) + L"tmntnet.ini", "mod", "autosave", game ? "1" : "0");
    LOGI("autosave: %s in the game's options, tmntnet follows", game ? "on" : "off");
  }
}

bool save_now(const char* why) {
  std::vector<uint8_t> buf;
  if (!image(buf)) {
    LOGW("autosave (%s): the game's save image looks wrong, nothing written", why);
    return false;
  }
  // Write a new file and swap it in; the previous save stays as .bak.
  HANDLE f = CreateFileW(g_tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  DWORD n = 0;
  bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, buf.data(), (DWORD)buf.size(), &n, nullptr) &&
            n == buf.size() && FlushFileBuffers(f);
  if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
  if (ok) {
    if (GetFileAttributesW(g_path.c_str()) != INVALID_FILE_ATTRIBUTES) CopyFileW(g_path.c_str(), g_bak.c_str(), FALSE);
    ok = MoveFileExW(g_tmp.c_str(), g_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
  }
  if (!ok) {
    DWORD err = GetLastError();
    DeleteFileW(g_tmp.c_str());
    LOGW("autosave (%s): cannot write %ls (error %lu)", why, g_path.c_str(), err);
    native_menu::notify("АВТОЗАПИСЬ НЕ УДАЛАСЬ");
    return false;
  }
  LOGI("autosave (%s): savegame.dat written", why);
  native_menu::notify("АВТОЗАПИСЬ ГОТОВА");
  return true;
}

// Lab only: the "furthest stage" of the first character record (config+0x20)
// as a mark that tells a loaded save from a fresh profile.
int32_t* debug_mark_field() {
  static int32_t* field = nullptr;
  if (!field) {
    uintptr_t a = patch::find("config_ptr", "A1 ?? ?? ?? ?? 8B 80 54 11 00 00 C3");  // get options +0x6C
    uintptr_t p = a ? patch::read<uint32_t>(a + 1) : 0;
    uint8_t* cfg = p && patch::in_image(p, 4) ? *reinterpret_cast<uint8_t**>(p) : nullptr;
    if (cfg) field = reinterpret_cast<int32_t*>(cfg + 0x20);
  }
  return field;
}

void debug_mark(int value) {
  if (int32_t* f = debug_mark_field()) {
    LOGI("debug: progress mark %d -> %d", *f, value);
    *f = value;
  }
}

void debug_compare() {
  if (int32_t* f = debug_mark_field()) LOGI("debug: progress mark is %d", *f);
  std::vector<uint8_t> mem;
  if (!image(mem)) {
    LOGW("debug: savegame check: no save image");
    return;
  }
  std::vector<uint8_t> file(kSaveSize, 0);
  HANDLE f = CreateFileW(g_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  DWORD n = 0;
  bool read = f != INVALID_HANDLE_VALUE && ReadFile(f, file.data(), (DWORD)file.size(), &n, nullptr) &&
              n == file.size();
  if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
  if (!read) {
    LOGI("debug: savegame check: no savegame.dat");
    return;
  }
  size_t diff = 0, first = 0;
  for (size_t i = 0; i < kSaveSize; ++i)
    if (mem[i] != file[i] && !diff++) first = i;
  if (diff)
    LOGI("debug: savegame check: memory and savegame.dat differ in %zu bytes (first at 0x%zX)", diff, first);
  else
    LOGI("debug: savegame check: memory and savegame.dat are identical");
}

}  // namespace autosave
