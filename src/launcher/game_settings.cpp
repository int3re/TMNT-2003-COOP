#include "launcher/game_settings.h"

#include <windows.h>

#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

#include "common/config.h"
#include "common/game_dir.h"
#include "launcher/update.h"

namespace gset {
namespace {

// savesetting.dat (TMNT.EXE bc49a82c..., re/notes.md §11, §17): 932 bytes,
// int32 version 2 at 0, the keyboard keys of player 1 and of player 2 from 4
// (int32 DirectInput codes, 16 slots each), the sound options at 0x330 (int32
// music, effects, voices: 0..4; stereo: 1 or 0), a byte sum of everything
// before it at 0x3A0.
constexpr size_t kSaveSize = 0x3A4, kSaveSum = 0x3A0, kSaveSound = 0x330, kSaveKeys = 4;
constexpr int kSlots = 16;

// The key table's actions and their slots (0x5bdbc8), a column of the page at a time.
const struct {
  const wchar_t* label;
  const char* key;
  int slot;
} kActions[kKeyActions] = {
    {L"Вверх", "up", 12},          {L"Вниз", "down", 14},          {L"Влево", "left", 15},
    {L"Вправо", "right", 13},      {L"Прыжок", "jump", 5},         {L"Слабая атака", "attack", 6},
    {L"Сильная атака", "strong", 7}, {L"Сюрикен", "shuriken", 4},  {L"Толчок", "dash", 0},
    {L"Особая атака", "special", 2}, {L"Финты в воздухе", "juggle", 1}, {L"Смена сюрикена", "change", 3},
};
// The ready layouts (src/mod/binds.cpp), slots: dash, juggle, special, shuriken
// change, shuriken, jump, attack, strong, pause, -, -, start, up, right, down, left.
constexpr int kLayouts[2][2][kSlots] = {
    {{0x12, 0x13, 0x1D, 0x2A, 0x1F, 0x39, 0x21, 0x20, 0x01, -1, -1, 0x1C, 0xC8, 0xCD, 0xD0, 0xCB},
     {0x4E, 0x4D, 0x49, 0x48, 0x47, 0x52, 0x4B, 0x4C, 0x01, -1, -1, 0x9C, 0xC7, 0xD1, 0xCF, 0xD3}},
    {{0x16, 0x17, 0x18, 0x23, 0x26, 0x39, 0x24, 0x25, 0x01, -1, -1, 0x1C, 0x11, 0x20, 0x1F, 0x1E},
     {0x4F, 0x50, 0x48, 0x51, 0x4D, 0x52, 0x4B, 0x4C, 0x01, -1, -1, 0x9C, 0xC8, 0xCD, 0xD0, 0xCB}}};
// The codes the game's key table takes (its allowed[256], 0x5bd918).
constexpr unsigned char kAllowed[] = {
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
    0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B,
    0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,
    0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51,
    0x52, 0x53, 0x57, 0x58, 0x9D, 0xB5, 0xB8, 0xC7, 0xC8, 0xC9, 0xCB, 0xCD, 0xCF, 0xD0, 0xD1, 0xD2, 0xD3, 0xDB, 0xDC};

struct Mode {
  int kind;  // 0 the desktop's, 1 the game's own setting, 2 w x h
  int w, h;
};

struct Info {
  const wchar_t* label;
  const char* key;
  Kind kind;
  bool in_ini;  // tmntnet.ini; false: savesetting.dat
};
const Info kInfo[kRows] = {
    {L"Окно игры", "display", Kind::Choice, true},
    {L"Разрешение", "resolution", Kind::Choice, true},
    {L"Широкий формат", "wide", Kind::Switch, true},
    {L"По бокам от 4:3", "sides", Kind::Choice, true},
    {L"Игра идёт в фоне", "background", Kind::Switch, true},
    {L"Обзор камеры", "fov", Kind::Choice, true},
    {L"Панели игроков", "hud", Kind::Choice, true},
    {L"Качество", "quality", Kind::Choice, true},
    {L"Лимит кадров", "fps", Kind::Choice, true},
    {L"Синхронизация", "vsync", Kind::Choice, true},
    {L"Музыка", "music", Kind::Level, false},
    {L"Звуки", "effects", Kind::Level, false},
    {L"Голоса", "voices", Kind::Level, false},
    {L"Ролики", "movies", Kind::Choice, true},
    {L"Режим аудио", "stereo", Kind::Pair, false},
    {L"Заставка", "intro", Kind::Pair, true},
};

// The values the game's own pages offer (src/mod/mod_pages.cpp).
constexpr int kFovValues[] = {100, 110, 120, 130, 140, 150};
constexpr int kHudValues[] = {100, 90, 80, 70, 60, 50};
constexpr int kFpsValues[] = {0, 30, 60, 120, 144};
struct Quality {
  int msaa, aniso;
};
constexpr Quality kQualityValues[] = {{0, 0}, {4, 16}, {8, 16}};
constexpr int kMoviesOriginal = 6;  // movies as loud as the original has them ([audio] movie_follow=0)

struct State {
  std::wstring dir, ini, save;  // the folder; the two files where the game reads them
  bool mod = false, levels = false, canned = false;
  Config cfg;
  int level[3] = {4, 4, 4};  // music, effects, voices
  int stereo = 1;
  int key[2][kSlots];        // the keyboard keys of players 1 and 2
  bool own = false;          // own_key: the player's own keys, kept while a ready layout is on
  int own_key[2][kSlots];
  std::vector<Mode> modes;
  std::vector<int> fps;
  std::wstring error;
  bool kept_ini = false, kept_save = false;  // a copy was put aside before this run's first change
};
State g;

std::wstring env(const wchar_t* name) {
  wchar_t b[MAX_PATH * 2];
  const DWORD n = GetEnvironmentVariableW(name, b, MAX_PATH * 2);
  return n && n < MAX_PATH * 2 ? std::wstring(b, n) : std::wstring();
}

// --- the game's own view of its folder ----------------------------------------------
// The game has no manifest, so in a folder its user may not write to (Program
// Files) Windows gives it copies of the files it writes, under
// <LocalAppData>\VirtualStore\<the path without its drive>, and shows it those
// copies from then on. The launcher reads and writes the files the game sees.

std::wstring store_root() {
  const std::wstring lab = env(L"TMNTNET_LAUNCHER_VSTORE");
  if (!lab.empty()) return lab;
  PWSTR local = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) dir = local;
  CoTaskMemFree(local);
  return dir.empty() ? dir : gamedir::join(dir, L"VirtualStore");
}

std::wstring stored(const std::wstring& dir, const wchar_t* name) {
  const std::wstring root = store_root();
  if (root.empty() || dir.size() < 3 || dir[1] != L':') return L"";
  return gamedir::join(gamedir::join(root, dir.substr(3)), name);
}

bool is_protected(const std::wstring& dir) {
  if (!env(L"TMNTNET_LAUNCHER_VSTORE").empty()) return true;  // (lab: any folder)
  for (const wchar_t* name : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432", L"ProgramData", L"SystemRoot"}) {
    const std::wstring p = env(name);
    if (p.empty() || dir.size() <= p.size() || _wcsnicmp(dir.c_str(), p.c_str(), p.size()) != 0) continue;
    if (dir[p.size()] == L'\\' || dir[p.size()] == L'/') return true;
  }
  return false;
}

bool writable(const std::wstring& file) {
  HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  CloseHandle(f);
  return true;
}

std::wstring seen(const std::wstring& dir, const wchar_t* name) {
  const std::wstring copy = stored(dir, name);
  return !copy.empty() && gamedir::exists(copy) ? copy : gamedir::join(dir, name);
}

// Before a change: where it goes, and a copy of the file as it was (once a run).
bool ready(std::wstring& file, const wchar_t* name, bool& kept) {
  const std::wstring own = gamedir::join(g.dir, name);
  if (file == own && is_protected(g.dir) && !(writable(own) && update::can_write(g.dir))) {
    // The folder is closed to this user. A game that keeps its settings in it all the
    // same is started as an administrator; otherwise the change goes where Windows
    // would put the game's own.
    const std::wstring save_copy = stored(g.dir, L"savesetting.dat");
    if (gamedir::exists(gamedir::join(g.dir, L"savesetting.dat")) && !gamedir::exists(save_copy)) {
      g.error = L"Игра хранит настройки в защищённой папке: запустите лаунчер от имени администратора, чтобы менять их отсюда.";
      return false;
    }
    const std::wstring copy = stored(g.dir, name);
    const size_t slash = copy.find_last_of(L'\\');
    if (copy.empty() || slash == std::wstring::npos) {
      g.error = L"Папка игры закрыта для записи: запустите лаунчер от имени администратора.";
      return false;
    }
    SHCreateDirectoryExW(nullptr, copy.substr(0, slash).c_str(), nullptr);
    if (!CopyFileW(own.c_str(), copy.c_str(), TRUE)) {
      g.error = L"Папка игры закрыта для записи: запустите лаунчер от имени администратора.";
      return false;
    }
    file = copy;
  }
  if (!kept) {
    kept = true;
    CopyFileW(file.c_str(), gamedir::join(update::cache_dir(), std::wstring(name) + L".before-launcher").c_str(), FALSE);
  }
  return true;
}

std::string read_all(const std::wstring& file) {
  std::string out;
  HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return out;
  const DWORD size = GetFileSize(f, nullptr);
  DWORD got = 0;
  if (size != INVALID_FILE_SIZE && size < (1u << 20)) {
    out.resize(size);
    if (!ReadFile(f, out.data(), size, &got, nullptr)) got = 0;
    out.resize(got);
  }
  CloseHandle(f);
  return out;
}

uint8_t save_sum(const std::string& s) {
  uint8_t v = 0;
  for (size_t i = 0; i < kSaveSum; ++i) v = (uint8_t)(v + (uint8_t)s[i]);
  return v;
}

bool save_whole(const std::string& s) {
  int32_t version = 0;
  if (s.size() != kSaveSize) return false;
  memcpy(&version, s.data(), 4);
  return version == 2 && save_sum(s) == (uint8_t)s[kSaveSum];
}

bool put(const char* section, const char* name, const std::string& value) {
  if (g.canned) return true;
  if (!ready(g.ini, L"tmntnet.ini", g.kept_ini)) return false;
  if (::ini_set(g.ini, section, name, value)) return true;
  g.error = L"tmntnet.ini не записался (ошибка " + std::to_wstring(GetLastError()) + L").";
  return false;
}

// Writes one part of what the page holds into savesetting.dat: the sound
// levels or the keys; the rest of the file stays as it is on the disk.
bool put_save(bool keys) {
  if (g.canned) return true;
  if (!ready(g.save, L"savesetting.dat", g.kept_save)) return false;
  std::string s = read_all(g.save);
  if (!save_whole(s)) {
    g.error = L"savesetting.dat изменился, пока страница была открыта: откройте её ещё раз.";
    return false;
  }
  if (keys) {
    int32_t v[2 * kSlots];
    for (int p = 0; p < 2; ++p)
      for (int i = 0; i < kSlots; ++i) v[p * kSlots + i] = g.key[p][i];
    memcpy(&s[kSaveKeys], v, sizeof(v));
  } else {
    const int32_t v[4] = {g.level[0], g.level[1], g.level[2], g.stereo};
    memcpy(&s[kSaveSound], v, sizeof(v));
  }
  s[kSaveSum] = (char)save_sum(s);
  const std::wstring tmp = g.save + L".tmp";
  HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  DWORD n = 0;
  bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, s.data(), (DWORD)s.size(), &n, nullptr) && n == s.size();
  if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
  ok = ok && MoveFileExW(tmp.c_str(), g.save.c_str(), MOVEFILE_REPLACE_EXISTING);
  if (!ok) {
    g.error = L"savesetting.dat не записался (ошибка " + std::to_wstring(GetLastError()) + L").";
    DeleteFileW(tmp.c_str());
  }
  return ok;
}

bool put_levels() { return put_save(false); }

void use_layout(int which) {
  for (int p = 0; p < 2; ++p)
    for (int i = 0; i < kSlots; ++i) g.key[p][i] = kLayouts[which][p][i];
}

bool is_layout(int which) {
  for (int p = 0; p < 2; ++p)
    for (int i = 0; i < kSlots; ++i)
      if (i != 9 && i != 10 && g.key[p][i] != kLayouts[which][p][i]) return false;  // (9 and 10: unused by the game)
  return true;
}

// "0x12,0x13,...,-1": the way tmntnet.ini keeps a player's own keys ([mod] custom_keys_1 / 2).
std::string key_list(const int keys[kSlots]) {
  std::string out;
  char buf[8];
  for (int i = 0; i < kSlots; ++i) {
    if (keys[i] > 0) snprintf(buf, sizeof(buf), "0x%02X", keys[i]);
    else snprintf(buf, sizeof(buf), "-1");
    out += (i ? "," : "") + std::string(buf);
  }
  return out;
}

int nearest(const int* list, int n, int v) {
  int best = 0;
  for (int i = 1; i < n; ++i)
    if (abs(list[i] - v) < abs(list[best] - v)) best = i;
  return best;
}

// The values of РАЗРЕШЕНИЕ and ЛИМИТ КАДРОВ: the lists, and what the file has if it is none of them.
void make_lists() {
  std::vector<std::pair<int, int>> sizes;
  if (g.canned) {
    sizes = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
  } else {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); ++i)
      if (dm.dmBitsPerPel == 32 && dm.dmPelsWidth >= 640 && dm.dmPelsHeight >= 480)
        sizes.push_back({(int)dm.dmPelsWidth, (int)dm.dmPelsHeight});
  }
  if (g.cfg.res_mode == 2) sizes.push_back({g.cfg.res_w, g.cfg.res_h});
  std::sort(sizes.begin(), sizes.end());
  sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
  g.modes.clear();
  g.modes.push_back({0, 0, 0});
  if (g.cfg.res_mode == 1) g.modes.push_back({1, 0, 0});
  for (const auto& s : sizes) g.modes.push_back({2, s.first, s.second});

  g.fps.assign(std::begin(kFpsValues), std::end(kFpsValues));
  if (std::find(g.fps.begin(), g.fps.end(), g.cfg.fps_limit) == g.fps.end()) g.fps.push_back(g.cfg.fps_limit);
  std::sort(g.fps.begin(), g.fps.end());
}

void read_files() {
  g.cfg = Config();
  g.mod = !g.dir.empty() && gamedir::exists(g.ini) && load_config(g.ini, g.cfg);
  g.levels = false;
  g.level[0] = g.level[1] = g.level[2] = 4;
  g.stereo = 1;
  const std::string s = g.dir.empty() ? std::string() : read_all(g.save);
  if (save_whole(s)) {
    int32_t v[4];
    memcpy(v, s.data() + kSaveSound, sizeof(v));
    for (int i = 0; i < 3; ++i) g.level[i] = std::clamp((int)v[i], 0, 4);
    g.stereo = v[3] ? 1 : 0;
    g.levels = true;
    int32_t k[2 * kSlots];
    memcpy(k, s.data() + kSaveKeys, sizeof(k));
    for (int p = 0; p < 2; ++p)
      for (int i = 0; i < kSlots; ++i) g.key[p][i] = k[p * kSlots + i];
  } else {
    use_layout(0);
  }
  if (g.cfg.custom_keys) {  // (kept by the game's own РАСКЛАДКА row, or by this page)
    g.own = true;
    for (int p = 0; p < 2; ++p)
      for (int i = 0; i < kSlots; ++i) g.own_key[p][i] = g.cfg.custom_key[p][i];
  }
  make_lists();
}

}  // namespace

void load(const std::wstring& game_dir) {
  if (g.dir != game_dir) g.kept_ini = g.kept_save = g.own = false;
  g.canned = false;
  g.dir = game_dir;
  g.ini = seen(game_dir, L"tmntnet.ini");
  g.save = seen(game_dir, L"savesetting.dat");
  g.error.clear();
  read_files();
}

void load_canned() {
  g = State();
  g.canned = g.mod = g.levels = true;
  g.level[0] = 3;
  use_layout(0);
  make_lists();
}

bool has_mod() { return g.mod; }
bool has_levels() { return g.levels; }
const std::wstring& error() { return g.error; }
std::wstring ini_file() { return g.ini; }
std::wstring levels_file() { return g.save; }

Kind kind(Row r) { return kInfo[r].kind; }
const wchar_t* label(Row r) { return kInfo[r].label; }
const char* key(Row r) { return kInfo[r].key; }
bool usable(Row r) { return kInfo[r].in_ini ? g.mod : g.levels; }

int count(Row r) {
  switch (r) {
    case kDisplay: case kQuality: case kVsync: return 3;
    case kResolution: return (int)g.modes.size();
    case kSides: return 4;
    case kFov: case kHud: return 6;
    case kFpsLimit: return (int)g.fps.size();
    case kMusic: case kEffects: case kVoices: return 5;
    case kMovies: return g.cfg.movie_level < 0 && !g.cfg.movie_follow ? kMoviesOriginal + 1 : kMoviesOriginal;
    default: return 2;
  }
}

int value(Row r) {
  const Config& c = g.cfg;
  switch (r) {
    case kDisplay: return c.display == "borderless" ? 1 : c.display == "windowed" ? 2 : 0;
    case kResolution:
      for (size_t i = 0; i < g.modes.size(); ++i) {
        const Mode& m = g.modes[i];
        if (m.kind == c.res_mode && (m.kind != 2 || (m.w == c.res_w && m.h == c.res_h))) return (int)i;
      }
      return 0;
    case kWide: return c.widescreen ? 1 : 0;
    case kSides: return !c.fullscreen_pillarbox ? (c.hud_center ? 2 : 3) : (c.side_bars_blur ? 0 : 1);
    case kBackground: return c.background_run ? 1 : 0;
    case kFov: return nearest(kFovValues, 6, c.fov);
    case kHud: return nearest(kHudValues, 6, c.hud_size);
    case kQuality: return c.msaa == 0 && c.anisotropy == 0 ? 0 : c.msaa >= 8 ? 2 : 1;
    case kFpsLimit: return (int)(std::find(g.fps.begin(), g.fps.end(), c.fps_limit) - g.fps.begin()) % (int)g.fps.size();
    case kVsync: return c.vsync < 0 ? 0 : c.vsync ? 1 : 2;
    case kMusic: case kEffects: case kVoices: return g.level[r - kMusic];
    case kMovies: return c.movie_level >= 0 ? c.movie_level + 1 : c.movie_follow ? 0 : kMoviesOriginal;
    case kStereo: return g.stereo ? 0 : 1;
    case kIntro: return c.intro_skip ? 1 : 0;
    default: return 0;
  }
}

std::wstring name(Row r, int v) {
  auto of = [v](std::initializer_list<const wchar_t*> names) {
    return v >= 0 && v < (int)names.size() ? std::wstring(names.begin()[v]) : std::wstring();
  };
  switch (r) {
    case kDisplay: return of({L"Полный экран", L"Без рамки", L"В окне"});
    case kResolution: {
      if (v < 0 || v >= (int)g.modes.size()) return L"";
      const Mode& m = g.modes[v];
      return m.kind == 0 ? L"Рабочего стола" : m.kind == 1 ? L"Из меню игры" : std::to_wstring(m.w) + L" × " + std::to_wstring(m.h);
    }
    case kSides: return of({L"Размыто", L"Темно", L"Растянуть фон", L"Растянуть всё"});
    case kFov: return v <= 0 || v > 5 ? L"Как в игре" : std::to_wstring(kFovValues[v]) + L" %";
    case kHud: return v <= 0 || v > 5 ? L"Как в игре" : std::to_wstring(kHudValues[v]) + L" %";
    case kQuality: return of({L"Как в игре", L"Высокое", L"Максимум"});
    case kFpsLimit: return v < 0 || v >= (int)g.fps.size() || g.fps[v] == 0 ? L"Нет" : std::to_wstring(g.fps[v]);
    case kVsync: return of({L"Как в игре", L"Включена", L"Выключена"});
    case kMusic: case kEffects: case kVoices: return std::to_wstring(v + 1) + L" из 5";
    case kMovies: return v == 0 ? L"Как музыка" : v == kMoviesOriginal ? L"Как в оригинале" : std::to_wstring(v) + L" из 5";
    case kStereo: return of({L"Стерео", L"Моно"});
    case kIntro: return of({L"Показывать", L"Пропускать"});
    default: return of({L"Выкл", L"Вкл"});
  }
}

bool set(Row r, int v) {
  const int n = count(r);
  if (n <= 0 || !usable(r)) return false;
  v = ((v % n) + n) % n;
  g.error.clear();
  Config& c = g.cfg;
  bool ok = true;
  switch (r) {
    case kDisplay: {
      static const char* kNames[] = {"fullscreen", "borderless", "windowed"};
      c.display = kNames[v];
      ok = put("video", "display", kNames[v]);
      break;
    }
    case kResolution: {
      const Mode m = g.modes[v];
      c.res_mode = m.kind;
      c.res_w = m.w;
      c.res_h = m.h;
      ok = put("video", "resolution",
               m.kind == 0 ? "desktop" : m.kind == 1 ? "game" : std::to_string(m.w) + "x" + std::to_string(m.h));
      break;
    }
    case kWide:
      c.widescreen = v == 1;
      ok = put("video", "widescreen", v ? "1" : "0");
      break;
    case kSides:  // (one row for three keys, as in the game's page)
      c.fullscreen_pillarbox = v <= 1;
      c.hud_center = v <= 2;
      if (v <= 1) c.side_bars_blur = v == 0;
      ok = put("video", "fullscreen_images", c.fullscreen_pillarbox ? "pillarbox" : "stretch") &&
           put("video", "hud", c.hud_center ? "center" : "stretch") && put("video", "side_bars", c.side_bars_blur ? "blur" : "black");
      break;
    case kBackground:
      c.background_run = v == 1;
      ok = put("video", "background", v ? "run" : "pause");
      break;
    case kFov:
      c.fov = kFovValues[v];
      ok = put("video", "fov", std::to_string(c.fov));
      break;
    case kHud:
      c.hud_size = kHudValues[v];
      ok = put("video", "hud_size", std::to_string(c.hud_size));
      break;
    case kQuality:
      c.msaa = kQualityValues[v].msaa;
      c.anisotropy = kQualityValues[v].aniso;
      ok = put("video", "antialiasing", std::to_string(c.msaa)) && put("video", "anisotropy", std::to_string(c.anisotropy));
      break;
    case kFpsLimit:
      c.fps_limit = g.fps[v];
      ok = put("video", "fps_limit", std::to_string(c.fps_limit));
      break;
    case kVsync:
      c.vsync = v == 0 ? -1 : v == 1 ? 1 : 0;
      ok = put("video", "vsync", v == 0 ? "game" : v == 1 ? "on" : "off");
      break;
    case kMusic: case kEffects: case kVoices:
      g.level[r - kMusic] = v;
      ok = put_levels();
      break;
    case kMovies:
      if (v == kMoviesOriginal) break;  // (only what the file already says)
      c.movie_level = v - 1;
      ok = put("audio", "movie_level", std::to_string(v - 1));
      if (ok && v == 0 && !c.movie_follow) {
        c.movie_follow = true;
        ok = put("audio", "movie_follow", "1");
      }
      break;
    case kStereo:
      g.stereo = v == 0 ? 1 : 0;
      ok = put_levels();
      break;
    case kIntro:
      c.intro_skip = v == 1;
      ok = put("audio", "intro", v ? "skip" : "play");
      break;
    default: break;
  }
  if (!ok && !g.canned) {  // (the page shows what the files have)
    const std::wstring why = g.error;
    read_files();
    g.error = why;
  }
  return ok;
}

bool reset(bool sound) {
  // The mod's defaults (dist/tmntnet.ini) and the game's own (4, 4, 4, stereo).
  static const struct {
    Row row;
    int value;
  } kDefaults[] = {{kDisplay, 1}, {kResolution, 0}, {kWide, 1}, {kSides, 0}, {kBackground, 0}, {kFov, 0},
                   {kHud, 0},     {kQuality, 1},    {kFpsLimit, 0}, {kVsync, 0}, {kMusic, 4},   {kEffects, 4},
                   {kVoices, 4},  {kMovies, 0},     {kStereo, 0},  {kIntro, 0}};
  bool ok = true;
  std::wstring why;
  for (const auto& d : kDefaults) {
    if ((d.row >= kFirstSound) != sound || !usable(d.row) || value(d.row) == d.value) continue;
    if (!set(d.row, d.value)) {
      ok = false;
      why = g.error;
      break;
    }
  }
  if (!ok) g.error = why;
  return ok;
}

// --- УПРАВЛЕНИЕ ---------------------------------------------------------------------------

bool has_keys() { return g.levels; }
const wchar_t* action_label(int action) { return kActions[action].label; }
const char* action_key(int action) { return kActions[action].key; }
int key_code(int player, int action) { return g.key[player][kActions[action].slot]; }

std::wstring key_name(int code) {
  static const struct {
    int code;
    const wchar_t* name;
  } kNames[] = {
      {0x01, L"Esc"},         {0x0C, L"-"},            {0x0D, L"="},          {0x0E, L"Backspace"},
      {0x0F, L"Tab"},         {0x1A, L"["},            {0x1B, L"]"},          {0x1C, L"Enter"},
      {0x1D, L"Левый Ctrl"},  {0x27, L";"},            {0x28, L"'"},          {0x29, L"`"},
      {0x2A, L"Левый Shift"}, {0x2B, L"\\"},           {0x33, L","},          {0x34, L"."},
      {0x35, L"/"},           {0x36, L"Правый Shift"}, {0x37, L"Num *"},      {0x38, L"Левый Alt"},
      {0x39, L"Пробел"},      {0x3A, L"Caps Lock"},    {0x45, L"Num Lock"},   {0x46, L"Scroll Lock"},
      {0x47, L"Num 7"},       {0x48, L"Num 8"},        {0x49, L"Num 9"},      {0x4A, L"Num -"},
      {0x4B, L"Num 4"},       {0x4C, L"Num 5"},        {0x4D, L"Num 6"},      {0x4E, L"Num +"},
      {0x4F, L"Num 1"},       {0x50, L"Num 2"},        {0x51, L"Num 3"},      {0x52, L"Num 0"},
      {0x53, L"Num ."},       {0x57, L"F11"},          {0x58, L"F12"},        {0x9C, L"Num Enter"},
      {0x9D, L"Правый Ctrl"}, {0xB5, L"Num /"},        {0xB8, L"Правый Alt"}, {0xC5, L"Pause"},
      {0xC7, L"Home"},        {0xC8, L"Стрелка вверх"},            {0xC9, L"Page Up"},    {0xCB, L"Стрелка влево"},
      {0xCD, L"Стрелка вправо"},           {0xCF, L"End"},          {0xD0, L"Стрелка вниз"},          {0xD1, L"Page Down"},
      {0xD2, L"Insert"},      {0xD3, L"Delete"},       {0xDB, L"Левый Win"},  {0xDC, L"Правый Win"},
      {kMouseKey, L"ЛКМ"},    {kMouseKey + 1, L"ПКМ"}, {kMouseKey + 2, L"СКМ"}, {kMouseKey + 3, L"Мышь 4"},
      {kMouseKey + 4, L"Мышь 5"}};
  for (const auto& k : kNames)
    if (k.code == code) return k.name;
  static const wchar_t kRows[] = L"QWERTYUIOP\0\0\0\0ASDFGHJKL";  // 0x10..0x26
  if (code >= 0x02 && code <= 0x0B) return std::wstring(1, L"1234567890"[code - 0x02]);
  if (code >= 0x10 && code <= 0x26 && kRows[code - 0x10]) return std::wstring(1, kRows[code - 0x10]);
  if (code >= 0x2C && code <= 0x32) return std::wstring(1, L"ZXCVBNM"[code - 0x2C]);
  if (code >= 0x3B && code <= 0x44) return L"F" + std::to_wstring(code - 0x3B + 1);
  if (code <= 0) return L"—";
  wchar_t buf[16];
  swprintf(buf, 16, L"код 0x%02X", code);
  return buf;
}

std::wstring key_text(int player, int action) {
  std::wstring text = key_name(key_code(player, action));
  if (!g.mod || !g.cfg.mouse_on) return text;
  static const wchar_t* kShort[kMouseButtons] = {L"ЛКМ", L"ПКМ", L"СКМ", L"М4", L"М5"};
  for (int b = 0; b < kMouseButtons; ++b)  // ([mouse] left=attack: the button works as well as the key)
    if (g.cfg.mouse_slot[b] == kActions[action].slot && g.cfg.mouse_player[b] == player + 1) text += std::wstring(L" +") + kShort[b];
  return text;
}

bool key_allowed(int code) {
  if (code >= kMouseKey && code < kMouseKey + kMouseButtons) return g.mod;
  return std::find(std::begin(kAllowed), std::end(kAllowed), code) != std::end(kAllowed);
}

bool set_key(int player, int action, int code) {
  if (!g.levels || player < 0 || player > 1 || action < 0 || action >= kKeyActions || !key_allowed(code)) return false;
  g.error.clear();
  const int slot = kActions[action].slot, old = g.key[player][slot];
  if (old == code) return true;
  for (int p = 0; p < 2; ++p)
    for (const auto& a : kActions)
      if (g.key[p][a.slot] == code) g.key[p][a.slot] = old;
  g.key[player][slot] = code;
  const bool ok = put_save(true);
  if (!ok && !g.canned) {
    const std::wstring why = g.error;
    read_files();
    g.error = why;
  }
  return ok;
}

int layout() { return is_layout(1) ? 1 : is_layout(0) ? 0 : 2; }
bool has_own_layout() { return g.own; }

bool set_layout(int v) {
  const int now = layout();
  if (!g.levels || v < 0 || v > 2 || v == now) return g.levels;
  if (v == 2 && !g.own) return false;
  g.error.clear();
  if (now == 2) {  // the player's own keys are kept: they come back with СВОЯ, here and in the game
    g.own = true;
    for (int p = 0; p < 2; ++p)
      for (int i = 0; i < kSlots; ++i) g.own_key[p][i] = g.key[p][i];
    if (g.mod) {
      put("mod", "custom_keys_1", key_list(g.own_key[0]));
      put("mod", "custom_keys_2", key_list(g.own_key[1]));
      g.error.clear();  // (not kept in the file: kept until the launcher is closed)
    }
  }
  if (v == 2) {
    for (int p = 0; p < 2; ++p)
      for (int i = 0; i < kSlots; ++i)
        if (i != 9 && i != 10) g.key[p][i] = g.own_key[p][i];
  } else {
    use_layout(v);
  }
  const bool ok = put_save(true);
  if (!ok && !g.canned) {
    const std::wstring why = g.error;
    read_files();
    g.error = why;
  }
  return ok;
}

}  // namespace gset
