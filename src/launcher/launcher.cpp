// tmntnet-launcher.exe: the mod's front door, and its installer in one file.
//   - finds the game folder, reads the mod's version in it;
//   - carries tmntnet-setup.exe of its own version inside: a first install
//     needs no network;
//   - asks GitHub for the latest release (update.h says how) and shows both
//     versions; a newer release is taken as that release's launcher (checked
//     by SHA-256), which replaces this file, starts and installs what it
//     carries; where this file cannot be replaced, the release's installer is
//     downloaded and run instead;
//   - at its start takes a newer launcher by itself (Обновлять лаунчер при
//     запуске), leaving the mod in the game folder as it is;
//   - shows the game's picture and sound settings (game_settings.h) on two
//     pages of its settings;
//   - starts the game.
// Its settings: %APPDATA%\tmntnet\launcher.ini.
//
// Lab (no window on the screen, nothing of the user's touched):
//   /ini <file>                 another settings file ([launcher] base=..., no_play=1)
//   /shot <png> /state <name>   a canned state's picture (update, install, downloading, ready,
//                               nogame, offline, settings, graphics, sound, controls, controls-key,
//                               selfupdate, updated, error, checking, bundle, ...);
//                               /scale <n>, /accent <0..3>, /was <the version before, in the picture>
//   /snap <png> /after <ms>     runs hidden, saves the live state's picture after <ms>, exits
//   /do <what>@<ms>             presses a control then (primary, secondary, cancel, settings,
//                               tab_graphics, tab_sound, tab_controls, reset, self, a row's part:
//                               display.next, wide.switch, music.b2, stereo.b1; the keys: player2,
//                               layout.next, key.jump (its cell), then press.1E (a key by its
//                               DirectInput code, hex) or mouse.1 (a mouse button), ...)
//   /at <x>,<y>                 shows the window there, without taking the keyboard
//   /log <file>                 what happened, line by line
//   /fonts                      the fonts found, into the log
//   /feed <file> <tag>          reads a saved releases.atom, the release into the log
#include <windows.h>

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "common/game_dir.h"
#include "common/version.h"
#include "launcher/game_settings.h"
#include "launcher/gfx.h"
#include "launcher/net.h"
#include "launcher/resource.h"
#include "launcher/update.h"
#include "launcher/view.h"

namespace {

const wchar_t* kDefaultBase = L"https://github.com/int3re/TMNT-2003-COOP";
const wchar_t* kClass = L"tmntnet-launcher";
const wchar_t* kTitle = L"TMNT 2003 Co-op";

constexpr UINT kCheckDone = WM_APP + 1, kJobDone = WM_APP + 2;
constexpr UINT_PTR kJobTimer = 1, kDisarmTimer = 2, kCloseTimer = 3, kLabTimer = 4, kBusyTimer = 5;

enum class Check { NotAsked, Asking, Known, Failed };
enum class Job { None, Download, Verify, WaitGame, Install };
enum class Action { None, PickFolder, Update, UpdateSelf, Play, Ask };
// Where a version to put in comes from.
enum class Source { None, Bundle, Launcher, Setup };  // inside this file; the release's launcher; the release's installer
// What a job does.
enum class Kind { Bundle, Setup, ViaLauncher, SelfOnly, Uninstall };

struct Settings {
  std::wstring file;
  std::wstring game_dir;
  bool check_at_start = true, auto_install = false;
  bool auto_self = true;  // a newer launcher found at the start is taken without a question
  net::Proxy proxy;
  int accent = 3;
  std::wstring base = kDefaultBase;
  bool no_play = false;  // lab: the game is never started, no page is opened, no restart
};

struct Model {
  Settings set;
  std::wstring installed;
  Check check = Check::NotAsked;
  update::Release latest;
  net::Failure check_failure;
  std::wstring checked_at;  // 18:42
  Job job = Job::None;
  Kind job_kind = Kind::Bundle;
  bool job_uninstall = false, job_self = false;
  std::wstring error, error_host;  // a job that failed, until the next action
  bool settings_open = false;
  int tab = 0;              // the settings' page: 0 the launcher's own, 1 ГРАФИКА, 2 ЗВУК, 3 УПРАВЛЕНИЕ
  int keys_player = 0;      // УПРАВЛЕНИЕ: whose keys are shown
  int capture = -1;         // the action whose cell waits for a key, or -1
  std::wstring capture_note;  // why the key just pressed was not taken
  bool game_busy = false;   // the game is running: its settings are not to be changed under it
  bool self_tried = false;  // the launcher's own update was started once in this run
  bool just_updated = false;  // started by the launcher before it (/updated)
  bool launching = false;
  bool uninstall_armed = false;
  float flow = 0.0f;
  // /shot: a canned state stands in for the real one.
  int fake_game = -1;      // 0 no game, 1 a game, -1 the real folder
  int fake_bundle = -1;    // 0 no installer inside, 1 one, -1 the real resource
  std::wstring fake_own;   // this launcher's version
};

Model m;
HWND g_hwnd = nullptr, g_edit = nullptr;
HBRUSH g_edit_brush = nullptr;
HFONT g_edit_font = nullptr;
WNDPROC g_edit_proc = nullptr;
ID2D1HwndRenderTarget* g_rt = nullptr;
float g_scale = 1.0f;
std::vector<view::Hit> g_hits;
int g_hover = 0, g_down = 0, g_focus = 0;
bool g_tracking = false;
Action g_primary = Action::None, g_secondary = Action::None;
std::wstring g_paper_url;

// The worker's side of a job.
std::atomic<bool> g_cancel{false};
std::atomic<int> g_stage{0};
std::atomic<bool> g_self_phase{false};
std::atomic<uint64_t> g_got{0}, g_total{0};

// --- lab ------------------------------------------------------------------------

std::wstring g_log_file;

void lab(const std::wstring& line) {
  if (g_log_file.empty()) return;
  FILE* f = _wfopen(g_log_file.c_str(), L"ab");
  if (!f) return;
  const std::string s = update::narrow(L"[" + std::to_wstring(GetTickCount()) + L"] " + line + L"\n");
  fwrite(s.data(), 1, s.size(), f);
  fclose(f);
}

// --- settings ---------------------------------------------------------------------

std::wstring default_ini() {
  PWSTR roaming = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) dir = roaming;
  CoTaskMemFree(roaming);
  dir = gamedir::join(dir, L"tmntnet");
  CreateDirectoryW(dir.c_str(), nullptr);
  return gamedir::join(dir, L"launcher.ini");
}

std::wstring ini_get(const wchar_t* key, const wchar_t* fallback) {
  wchar_t buf[2048];
  GetPrivateProfileStringW(L"launcher", key, fallback, buf, 2048, m.set.file.c_str());
  return buf;
}

void ini_set(const wchar_t* key, const std::wstring& value) {
  if (!gamedir::exists(m.set.file)) {  // (UTF-16 with its mark: folders have any letters)
    HANDLE f = CreateFileW(m.set.file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      const wchar_t head[] = L"\xFEFF; tmntnet-launcher: its own settings\r\n";
      DWORD n = 0;
      WriteFile(f, head, (DWORD)(wcslen(head) * sizeof(wchar_t)), &n, nullptr);
      CloseHandle(f);
    }
  }
  WritePrivateProfileStringW(L"launcher", key, value.c_str(), m.set.file.c_str());
}

void load_settings() {
  m.set.game_dir = ini_get(L"game_dir", L"");
  m.set.check_at_start = ini_get(L"check_at_start", L"1") != L"0";
  m.set.auto_install = ini_get(L"auto_install", L"0") == L"1";
  m.set.auto_self = ini_get(L"auto_self", L"1") != L"0";
  const std::wstring proxy = ini_get(L"proxy", L"system");
  m.set.proxy.mode = proxy == L"none" ? net::Proxy::None : proxy == L"own" ? net::Proxy::Own : net::Proxy::System;
  m.set.proxy.address = ini_get(L"proxy_address", L"");
  m.set.accent = _wtoi(ini_get(L"accent", L"3").c_str());
  if (m.set.accent < 0 || m.set.accent > 3) m.set.accent = 3;
  m.set.base = ini_get(L"base", kDefaultBase);
  while (!m.set.base.empty() && m.set.base.back() == L'/') m.set.base.pop_back();
  m.set.no_play = ini_get(L"no_play", L"0") == L"1";
}

void save_proxy() {
  ini_set(L"proxy", m.set.proxy.mode == net::Proxy::None ? L"none" : m.set.proxy.mode == net::Proxy::Own ? L"own" : L"system");
  ini_set(L"proxy_address", m.set.proxy.address);
}

// --- words ------------------------------------------------------------------------

std::wstring megabytes(uint64_t bytes) {
  wchar_t b[32];
  swprintf(b, 32, L"%.1f", (double)bytes / 1e6);
  for (wchar_t* p = b; *p; ++p)
    if (*p == L'.') *p = L',';
  return b;
}

std::wstring repo_text() {
  size_t at = m.set.base.find(L"://");
  return at == std::wstring::npos ? m.set.base : m.set.base.substr(at + 3);
}

std::wstring describe(const net::Failure& why) {
  if (why.cancelled) return L"";
  if (why.host == L"(disk)") return L"Файл не записался на диск (ошибка " + std::to_wstring(why.error) + L").";
  if (why.status == 404) return L"В релизе нет такого файла (" + why.host + L").";
  if (why.status) return L"Сервер " + why.host + L" ответил ошибкой " + std::to_wstring(why.status) + L".";
  if (why.host.find(L"github.com") == std::wstring::npos)  // (the files come from another server than the site)
    return L"Сервер загрузок GitHub не отвечает: включите прокси в настройках.";
  return L"Сервер " + why.host + L" не отвечает: проверьте связь или включите прокси в настройках.";
}

bool game_ok() { return m.fake_game >= 0 ? m.fake_game == 1 : gamedir::has_game(m.set.game_dir); }

std::wstring self_path() {
  wchar_t self[MAX_PATH * 2];
  GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
  return self;
}

std::wstring own_version() { return m.fake_own.empty() ? std::wstring(TEXT(TMNTNET_VERSION)) : m.fake_own; }

// tmntnet-setup.exe of this launcher's version, inside the file.
bool has_bundle() {
  if (m.fake_bundle >= 0) return m.fake_bundle == 1;
  static const bool there = FindResourceW(nullptr, MAKEINTRESOURCEW(RES_SETUP), RT_RCDATA) != nullptr;
  return there;
}

// What the version inside this file is about (dist/release.txt, in the resources).
update::Release g_canned_carried;  // /shot
const update::Release& carried() {
  if (!m.fake_own.empty()) return g_canned_carried;
  static update::Release mine;
  static bool read = false;
  if (!read) {
    read = true;
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(RES_NOTES), RT_RCDATA);
    HGLOBAL g = res ? LoadResource(nullptr, res) : nullptr;
    const char* data = g ? static_cast<const char*>(LockResource(g)) : nullptr;
    if (data) update::from_lines(std::string(data, SizeofResource(nullptr, res)), mine);
    mine.version = TEXT(TMNTNET_VERSION);
    mine.tag = L"v" + mine.version;
  }
  return mine;
}

// The version that can be put into the game now, where it comes from, and
// whether it is newer than what is there.
struct Offer {
  Source source = Source::None;
  std::wstring version;
  bool on = false;
};

Offer offer() {
  Offer o;
  const bool known = m.check == Check::Known;
  if (known && update::compare(m.latest.version, own_version()) > 0) {
    o.version = m.latest.version;
    o.source = !m.latest.launcher.empty() && !m.latest.launcher_sha256.empty() ? Source::Launcher : Source::Setup;
  } else if (has_bundle()) {
    o.version = own_version();
    o.source = Source::Bundle;
  } else if (known) {
    o.version = m.latest.version;
    o.source = Source::Setup;
  }
  o.on = o.source != Source::None && (m.installed.empty() || update::compare(o.version, m.installed) > 0);
  return o;
}

// The release has a launcher newer than this one.
bool newer_self() {
  return m.check == Check::Known && !m.latest.launcher.empty() && !m.latest.launcher_sha256.empty() &&
         update::compare(m.latest.version, own_version()) > 0;
}

// --- the picture --------------------------------------------------------------------

void offline_sheet(view::View& v, const std::wstring& host) {
  v.paper_tab = L"НЕТ СВЯЗИ";
  // (a long server name takes the whole line by itself)
  v.paper_meta = host.empty() ? L"github.com не отвечает" : host.size() > 20 ? host : host + L" не отвечает";
  v.paper_title = L"Что можно сделать";
  v.paper_items = {L"Повторить позже: лаунчер проверяет обновления при каждом запуске.",
                   L"Включить прокси в настройках — системный или свой.",
                   L"Скачать новый tmntnet-launcher.exe в браузере: установщик своей версии у него внутри."};
  v.paper_link = L"Страница релиза в браузере";
  g_paper_url = m.set.base + L"/releases/latest";
}

// ГРАФИКА and ЗВУК: the rows of the game's settings, and the line under them.
void fill_page(view::View& v, bool game) {
  const bool sound = m.tab == 2;
  auto add = [&](view::Group& to, gset::Row r) {
    view::Row row;
    row.number = r;
    row.label = gset::label(r);
    row.usable = game && !m.game_busy && gset::usable(r);
    const int now = gset::value(r);
    switch (gset::kind(r)) {
      case gset::Kind::Choice:
        row.kind = view::RowKind::Choice;
        row.value = gset::name(r, now);
        break;
      case gset::Kind::Switch:
        row.kind = view::RowKind::Switch;
        row.on = now == 1;
        break;
      case gset::Kind::Level:
        row.kind = view::RowKind::Level;
        row.level = now;
        break;
      case gset::Kind::Pair:
        row.kind = view::RowKind::Pair;
        row.options[0] = gset::name(r, 0);
        row.options[1] = gset::name(r, 1);
        row.chosen = now;
        break;
    }
    to.rows.push_back(row);
  };
  if (!sound) {
    v.groups[0].name = L"ЭКРАН";
    for (gset::Row r : {gset::kDisplay, gset::kResolution, gset::kWide, gset::kSides, gset::kBackground}) add(v.groups[0], r);
    v.groups[1].name = L"КАРТИНКА";
    for (gset::Row r : {gset::kFov, gset::kHud, gset::kQuality, gset::kFpsLimit, gset::kVsync}) add(v.groups[1], r);
  } else {
    v.groups[0].name = L"ГРОМКОСТЬ";
    for (gset::Row r : {gset::kMusic, gset::kEffects, gset::kVoices, gset::kMovies}) add(v.groups[0], r);
    v.groups[1].name = L"ЗВУЧАНИЕ";
    for (gset::Row r : {gset::kStereo, gset::kIntro}) add(v.groups[1], r);
    v.groups[1].note = L"В оригинале заставка гремит громче всей игры. С модом ролики звучат как музыка, пока им не задана "
                       L"своя громкость.";
  }
  for (const view::Group& g : v.groups)
    for (const view::Row& row : g.rows) v.can_reset = v.can_reset || row.usable;
  v.page_warn = true;
  if (!game) v.page_note = L"Игра не найдена: укажите её папку на вкладке ЛАУНЧЕР.";
  else if (m.game_busy) v.page_note = L"Игра запущена: закройте её, чтобы менять настройки отсюда.";
  else if (!gset::error().empty()) v.page_note = gset::error();
  else if (!sound && !gset::has_mod()) v.page_note = L"Мод не установлен: настройки графики появятся после его установки.";
  else if (sound && !gset::has_levels())
    v.page_note = L"Игра ещё не сохраняла свои настройки: громкость можно будет менять после её первого запуска.";
  else if (sound && !gset::has_mod()) v.page_note = L"Мод не установлен: «Ролики» и «Заставка» появятся после его установки.";
  else {
    v.page_warn = false;
    v.page_note = sound ? L"Громкость — та же, что в игре: ПАРАМЕТРЫ → ОПЦИИ ЗВУКА. Действует со следующего запуска игры."
                        : L"Настройки мода из tmntnet.ini — те же строки, что в игре: ПАРАМЕТРЫ → ПАРАМЕТРЫ ГРАФИКИ. "
                          L"Действуют со следующего запуска игры.";
  }
}

// УПРАВЛЕНИЕ: the keys of one player, the ready layout, the line under them.
void fill_controls(view::View& v, bool game) {
  const bool usable = game && !m.game_busy && gset::has_keys();
  v.keys_usable = usable;
  v.keys_player = m.keys_player;
  for (int a = 0; a < gset::kKeyActions; ++a) {
    v.keys[a].label = gset::action_label(a);
    v.keys[a].key = gset::has_keys() ? gset::key_text(m.keys_player, a) : std::wstring(L"—");
    v.keys[a].waiting = usable && m.capture == a;
  }
  static const wchar_t* kLayouts[3] = {L"Как в игре", L"Два игрока", L"Своя"};
  v.layout = gset::has_keys() ? kLayouts[gset::layout()] : L"—";
  v.can_reset = usable && gset::layout() != 0;
  v.page_warn = true;
  if (!game) v.page_note = L"Игра не найдена: укажите её папку на вкладке ЛАУНЧЕР.";
  else if (m.game_busy) v.page_note = L"Игра запущена: закройте её, чтобы менять настройки отсюда.";
  else if (!gset::error().empty()) v.page_note = gset::error();
  else if (!gset::has_keys())
    v.page_note = L"Игра ещё не сохраняла свои настройки: клавиши можно будет менять после её первого запуска.";
  else if (m.capture >= 0) {
    v.page_warn = false;
    v.page_bright = true;
    v.page_note = !m.capture_note.empty() ? m.capture_note
                  : gset::has_mod() ? L"Нажмите клавишу — или щёлкните по этой же ячейке кнопкой мыши, чтобы поставить её. Esc — отмена."
                                    : L"Нажмите клавишу. Esc — отмена.";
  } else {
    v.page_warn = false;
    v.page_note = L"Клавиши игроков 1 и 2 — те же, что в игре: ПАРАМЕТРЫ УПРАВЛЕНИЯ → КЛАВИАТУРА. По сети каждый играет "
                  L"клавишами игрока 1. Кнопки геймпада настраиваются в игре.";
  }
}

view::View make_view() {
  using view::Icon;
  using view::Mark;
  view::View v;
  v.accent = view::kAccents[m.set.accent];
  v.settings = m.settings_open;
  v.repo = repo_text();
  v.hover = g_hover;
  v.down = g_down;
  v.focus = g_focus;
  const bool game = game_ok(), have = !m.installed.empty(), known = m.check == Check::Known;
  v.game_path = game ? m.set.game_dir : m.set.game_dir.empty() ? L"(папка не выбрана)" : m.set.game_dir;
  g_primary = g_secondary = Action::None;
  g_paper_url = m.set.base + L"/releases";

  // The bottom line.
  const bool self_only = m.job_kind == Kind::SelfOnly;
  if (m.job != Job::None)
    v.net = m.job == Job::Download ? L"GitHub: идёт загрузка" : self_only ? L"Обновление лаунчера" : m.job_uninstall ? L"Удаление" : L"Установка";
  else if (m.check == Check::Asking) v.net = L"GitHub: проверяю";
  else if (known) v.net = L"GitHub: проверено в " + m.checked_at;
  else if (m.check == Check::Failed) v.net = L"GitHub: нет ответа", v.net_color = 0xEF5A5A;
  else v.net = L"GitHub: не проверялось", v.net_color = 0x4A6B59;

  // Settings.
  v.check_at_start = m.set.check_at_start;
  v.auto_install = m.set.auto_install;
  v.auto_self = m.set.auto_self;
  v.tab = m.tab;
  if (m.settings_open && m.tab == 3) fill_controls(v, game);
  else if (m.settings_open && m.tab != 0) fill_page(v, game);
  v.proxy = (int)m.set.proxy.mode;
  v.proxy_address = m.set.proxy.address;
  v.draw_address = !(g_edit && IsWindowVisible(g_edit));
  v.can_uninstall = game && have && m.job == Job::None;
  v.uninstall_label = !have ? L"Мод не установлен" : m.uninstall_armed ? L"Точно удалить?" : L"Удалить мод";

  // The sheet: what is new, by default.
  v.paper_tab = L"ЧТО НОВОГО";
  v.paper_link = L"Все изменения на GitHub";
  if (known) {
    v.paper_meta = m.latest.version + (m.latest.date.empty() ? L"" : L" · " + m.latest.date);
    v.paper_title = m.latest.title.empty() ? L"tmntnet " + m.latest.version : m.latest.title;
    v.paper_items = m.latest.notes;
    if (m.latest.title.empty() && m.latest.notes.empty()) v.paper_items = {L"Описание изменений — на странице релиза."};
  } else if (m.check == Check::Asking) {
    v.paper_meta = L"github.com";
    v.paper_title = L"Спрашиваю GitHub о последней версии";
  } else if (m.check == Check::NotAsked) {
    v.paper_meta = L"github.com";
    v.paper_title = L"Проверка обновлений выключена";
    v.paper_items = {L"Кнопка «Проверить обновления» спросит GitHub о последней версии."};
  }
  if (m.check == Check::Failed) offline_sheet(v, m.check_failure.host);
  if (!m.error_host.empty()) offline_sheet(v, m.error_host);

  // A job under way.
  if (m.job != Job::None) {
    v.job = true;
    const int stage = m.job == Job::Download ? 0 : m.job == Job::Verify ? 1 : 2;
    const wchar_t* names[3] = {L"Загрузка", L"Проверка SHA-256",
                               m.job_uninstall ? L"Удаление" : self_only ? L"Перезапуск" : L"Установка"};
    for (int i = 0; i < 3; ++i) {
      v.steps[i].label = names[i];
      v.steps[i].state = i < stage ? 2 : i == stage ? 1 : 0;
    }
    const uint64_t got = g_got, total = g_total;
    v.percent = m.job == Job::Download ? (total ? (int)(got * 100 / total) : 0) : 100;
    if (v.percent > 100) v.percent = 100;
    if (m.job == Job::Download) v.stage = total ? megabytes(got) + L" из " + megabytes(total) + L" МБ" : megabytes(got) + L" МБ";
    else if (m.job == Job::Verify) v.stage = L"сверяю SHA-256";
    else if (m.job == Job::WaitGame) v.stage = L"закройте игру — продолжу сам";
    else v.stage = m.job_self ? L"обновляю лаунчер" : m.job_uninstall ? L"убираю мод из папки игры" : L"ставлю файлы в папку игры";
    v.source = repo_text() + (known ? L" · " + m.latest.tag : L"");
    v.flow = m.flow;
    v.can_cancel = m.job == Job::Download || m.job == Job::WaitGame;
    if (self_only)
      v.job_note = L"На GitHub вышла версия " + (known ? m.latest.version : std::wstring(L"новее")) +
                   L": лаунчер обновляет себя и перезапустится сам. Мод в папке игры при этом не меняется.";
    else if (m.job_uninstall) v.job_note = L"Игра остаётся такой, какой была до мода. Сохранения не трогаются.";
    else v.job_note = L"Перед установкой лаунчер попросит закрыть игру. Настройки, клавиши и сохранения остаются.";
    return v;
  }

  // No game here.
  if (!game) {
    v.no_game = true;
    v.primary = L"ВЫБРАТЬ ПАПКУ ИГРЫ";
    v.icon = Icon::Folder;
    g_primary = Action::PickFolder;
    v.hint = L"Нужна своя копия игры: файлы игры в мод не входят.";
    v.paper_tab = L"ТРИ ШАГА";
    v.paper_meta = L"первый запуск";
    v.paper_title = L"Как это работает";
    v.paper_items = {L"Укажите папку игры.", L"Лаунчер поставит мод: установщик у него внутри.",
                     L"Запустите игру: в главном меню появится СЕТЕВАЯ ИГРА."};
    v.paper_link = L"Инструкция на GitHub";
    g_paper_url = m.set.base;
    return v;
  }

  // The two versions and what to do about them.
  const Offer o = offer();
  v.tickets = true;
  v.installed = have ? m.installed : L"нет";
  // The release the second ticket is about: GitHub's latest, or the one this file carries.
  const bool own_load = o.on && o.source == Source::Bundle && !(known && update::compare(m.latest.version, o.version) == 0);
  const update::Release& about = own_load ? carried() : m.latest;
  const bool told = own_load || known;
  const bool says_protocol = told && about.protocol > 0 && !about.protocol_since.empty();
  const bool same_protocol = says_protocol && have && update::compare(m.installed, about.protocol_since) >= 0;
  v.installed_note = !have ? L"мод не установлен" : same_protocol ? L"протокол " + std::to_wstring(about.protocol) : L"в папке игры";
  // (the first picture of a launcher that has just replaced the one before it says so)
  const std::wstring fresh = m.just_updated ? L"Лаунчер обновился до " + own_version() + L". " : L"";
  const std::wstring play_hint = fresh + L"Игра по сети — пункт СЕТЕВАЯ ИГРА в главном меню игры. Графика, звук и клавиши — в "
                                         L"настройках лаунчера, значок вверху справа.";
  // What GitHub is known to have.
  v.latest_label = L"НА GITHUB";
  if (known) {
    v.latest = m.latest.version;
    v.latest_note = m.latest.protocol ? L"протокол " + std::to_wstring(m.latest.protocol) : m.latest.date;
  } else if (m.check == Check::Asking) {
    v.latest = L"…";
    v.latest_note = L"проверяю";
  } else {
    v.latest = L"—";
    v.latest_note = m.check == Check::Failed ? L"нет ответа" : L"не проверялось";
  }

  if (o.on) {  // something newer can be put in
    v.latest_bright = v.is_new = true;
    v.icon = Icon::Download;
    g_primary = Action::Update;
    v.primary = (have ? L"ОБНОВИТЬ ДО " : L"УСТАНОВИТЬ МОД ") + o.version;
    if (have) v.secondary = L"ИГРАТЬ НА " + m.installed, g_secondary = Action::Play;
    if (o.source == Source::Bundle) {
      if (own_load) {  // (GitHub is silent or behind: the second ticket and the sheet are about the launcher's own load)
        v.latest_label = L"В ЛАУНЧЕРЕ";
        v.latest = o.version;
        v.latest_note = about.protocol ? L"протокол " + std::to_wstring(about.protocol) : L"скачивать не нужно";
        if (!about.title.empty() || !about.notes.empty()) {
          v.paper_tab = L"ЧТО НОВОГО";
          v.paper_meta = o.version + L" · уже в лаунчере";
          v.paper_title = about.title;
          v.paper_items = about.notes;
          v.paper_link = L"Все изменения на GitHub";
          g_paper_url = m.set.base + L"/releases";
        }
      }
      v.hint = fresh + L"Установщик этой версии внутри лаунчера: скачивать ничего не нужно.";
    } else if (o.source == Source::Launcher) {
      v.hint = m.latest.launcher + (m.latest.launcher_size ? L" · " + megabytes(m.latest.launcher_size) + L" МБ" : L"") +
               L" · после загрузки сверяется SHA-256";
    } else {
      v.hint = m.latest.setup + (m.latest.setup_size ? L" · " + megabytes(m.latest.setup_size) + L" МБ" : L"") +
               (m.latest.setup_sha256.empty() ? L" · контрольной суммы у этого релиза нет" : L" · после загрузки сверяется SHA-256");
    }
    if (!have) {
      v.mark = Mark::Ok;
      v.mark_text = L"Файлы игры не меняются: мод ставится рядом и убирается одной кнопкой.";
    } else if (says_protocol) {
      if (same_protocol) v.mark = Mark::Ok, v.mark_text = L"Протокол тот же: с друзьями на " + m.installed + L" играть можно.";
      else v.mark = Mark::Warn, v.mark_text = L"Протокол новый: обновиться нужно всем игрокам.";
    } else {
      v.mark_text = own_load ? L"В лаунчере версия мода новее, чем в папке игры." : L"Вышла новая версия мода.";
    }
  } else if (m.check == Check::Asking) {
    v.mark = Mark::Busy;
    v.mark_text = L"Спрашиваю GitHub о последней версии";
    if (have) {
      v.primary = L"ИГРАТЬ", v.icon = Icon::Play, g_primary = Action::Play;
      v.hint = play_hint;
    } else {
      v.primary = L"УСТАНОВИТЬ МОД", v.icon = Icon::Download, v.primary_on = false;
      v.hint = L"Жду ответа GitHub: какая версия последняя.";
    }
  } else if (known) {
    const int order = update::compare(m.installed, m.latest.version);
    v.equal = order == 0;
    v.ahead = order > 0;
    if (newer_self()) {
      v.mark = Mark::Warn;
      v.mark_text = L"Вышла новая версия лаунчера: " + m.latest.version + L".";
      v.primary = L"ОБНОВИТЬ ЛАУНЧЕР", v.icon = Icon::Download, g_primary = Action::UpdateSelf;
      v.secondary = L"ИГРАТЬ", g_secondary = Action::Play;
      v.hint = m.latest.launcher + (m.latest.launcher_size ? L" · " + megabytes(m.latest.launcher_size) + L" МБ" : L"") +
               L" · после загрузки сверяется SHA-256";
    } else {
      v.mark = Mark::Ok;
      v.mark_text = order > 0 ? L"У вас версия новее, чем последний релиз на GitHub." : L"У вас последняя версия. Новее на GitHub нет.";
      v.primary = L"ИГРАТЬ", v.icon = Icon::Play, g_primary = Action::Play;
      v.secondary = L"ПРОВЕРИТЬ ОБНОВЛЕНИЯ", g_secondary = Action::Ask;
      v.hint = play_hint;
      v.paper_tab = L"В ЭТОЙ ВЕРСИИ";
    }
  } else {
    const bool failed = m.check == Check::Failed;
    if (failed) v.mark = Mark::Bad, v.mark_text = L"GitHub не отвечает. Какая версия последняя — неизвестно.";
    else v.mark_text = L"Проверка при запуске выключена.";
    const wchar_t* ask = failed ? L"ПОВТОРИТЬ ПРОВЕРКУ" : L"ПРОВЕРИТЬ ОБНОВЛЕНИЯ";
    if (have) {
      v.primary = L"ИГРАТЬ НА " + m.installed, v.icon = Icon::Play, g_primary = Action::Play;
      v.secondary = ask, g_secondary = Action::Ask;
      v.hint = L"Проверка обновлений игре не нужна: мод работает как раньше.";
    } else {
      v.primary = ask, v.icon = Icon::Retry, g_primary = Action::Ask;
      v.hint = L"Чтобы поставить мод, лаунчеру нужен GitHub: проверьте связь или включите прокси в настройках.";
    }
  }
  if (m.launching && g_primary == Action::Play) v.primary = L"ИГРА ЗАПУСКАЕТСЯ", v.primary_on = false;
  if (!m.error.empty()) v.mark = Mark::Bad, v.mark_text = m.error;
  return v;
}

void repaint() {
  if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

// --- GitHub: the latest release ------------------------------------------------------

struct CheckResult {
  bool ok = false;
  update::Release release;
  net::Failure why;
};

void start_check() {
  if (m.check == Check::Asking || m.job != Job::None) return;
  m.check = Check::Asking;
  m.error.clear();
  m.error_host.clear();
  lab(L"check: asking " + m.set.base);
  const std::wstring base = m.set.base;
  const net::Proxy proxy = m.set.proxy;
  const HWND hwnd = g_hwnd;
  std::thread([base, proxy, hwnd] {
    CheckResult* r = new CheckResult;
    r->ok = update::latest(base, proxy, r->release, r->why);
    if (!PostMessageW(hwnd, kCheckDone, 0, reinterpret_cast<LPARAM>(r))) delete r;
  }).detach();
  repaint();
}

// --- a job: download, check, install ----------------------------------------------------

struct JobPlan {
  Kind kind = Kind::Bundle;
  std::wstring base, dir, self, own;
  net::Proxy proxy;
  update::Release release;
};

struct JobResult {
  bool ok = false, cancelled = false;
  bool restart = false, apply = false;  // start the new launcher; have it install what it carries
  std::wstring error, host;
};

// The newest installer kept from an earlier job, or empty.
std::wstring cached_setup() {
  const std::wstring dir = update::cache_dir();
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(gamedir::join(dir, L"tmntnet-setup-*.exe").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return L"";
  std::wstring best, best_version;
  do {
    std::wstring name = fd.cFileName, version = name.substr(14, name.size() - 14 - 4);
    if (best.empty() || update::compare(version, best_version) > 0) best = name, best_version = version;
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return gamedir::join(dir, best);
}

// The installer this file carries, written out; empty when there is none or
// it could not be written.
std::wstring unpack_bundle(const std::wstring& own) {
  HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(RES_SETUP), RT_RCDATA);
  HGLOBAL g = res ? LoadResource(nullptr, res) : nullptr;
  const void* data = g ? LockResource(g) : nullptr;
  const DWORD size = res ? SizeofResource(nullptr, res) : 0;
  if (!data || !size) return L"";
  const std::wstring file = gamedir::join(update::cache_dir(), L"tmntnet-setup-" + own + L".exe");
  HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return L"";
  DWORD written = 0;
  const bool ok = WriteFile(f, data, size, &written, nullptr) && written == size;
  CloseHandle(f);
  if (!ok) DeleteFileW(file.c_str());
  return ok ? file : L"";
}

void run_job(JobPlan p, HWND hwnd) {
  JobResult* r = new JobResult;
  auto fetch = [&](const std::wstring& name, const std::wstring& to, const std::wstring& sha, uint64_t size) {
    g_stage = (int)Job::Download;
    g_got = 0;
    g_total = size;
    net::Failure why;
    const std::wstring url = p.base + L"/releases/download/" + p.release.tag + L"/" + name;
    lab(L"job: download " + url);
    if (!net::download(url, to, p.proxy,
                       [](uint64_t got, uint64_t total) {
                         g_got = got;
                         if (total) g_total = total;
                       },
                       g_cancel, why)) {
      r->cancelled = why.cancelled;
      r->error = describe(why);
      if (!why.cancelled && !why.status && why.host != L"(disk)") r->host = why.host;
      lab(L"job: download failed: " + why.text());
      return false;
    }
    g_stage = (int)Job::Verify;
    if (!sha.empty()) {
      const std::wstring got = update::sha256_of(to);
      if (got != sha) {
        DeleteFileW(to.c_str());
        r->error = L"Контрольная сумма не совпала: файл повреждён в пути. Попробуйте ещё раз.";
        lab(L"job: sha256 " + got + L", the release says " + sha);
        return false;
      }
      lab(L"job: sha256 matches");
    }
    return true;
  };
  // The installer's run: waits for the game to close first.
  auto install = [&](const std::wstring& setup, bool uninstall) {
    g_stage = (int)Job::WaitGame;
    if (update::game_running(p.dir)) lab(L"job: the game is running, waiting for it to close");
    while (update::game_running(p.dir)) {
      if (g_cancel) {
        r->cancelled = true;
        return false;
      }
      Sleep(500);
    }
    g_stage = (int)Job::Install;
    std::wstring what;
    bool ok = update::run_setup(setup, uninstall ? L"/uninstall" : L"/host", p.dir, what);
    lab(std::wstring(L"job: setup ") + (uninstall ? L"/uninstall" : L"/host") + (ok ? L" ok" : L" failed: " + what));
    // An installer of 0.13.9 or older, started without a console, ends with an
    // error after its work is done: the game folder says how it went.
    const std::wstring now = update::installed_version(p.dir);
    if (!ok && (uninstall ? now.empty() : now == p.release.version)) {
      lab(L"job: the installer's code is not believed: the folder has " + (now.empty() ? std::wstring(L"no mod") : now));
      ok = true;
    }
    if (!ok) r->error = L"Не получилось: " + what;
    return ok;
  };
  // The release's installer, downloaded: the way for a launcher that cannot replace itself.
  auto by_setup = [&] {
    const std::wstring setup = gamedir::join(update::cache_dir(), L"tmntnet-setup-" + p.release.version + L".exe");
    if (!fetch(p.release.setup, setup, p.release.setup_sha256, p.release.setup_size) || !install(setup, false)) return false;
    WIN32_FIND_DATAW fd;  // (older installers are not needed any more)
    const std::wstring dir = update::cache_dir();
    HANDLE h = FindFirstFileW(gamedir::join(dir, L"tmntnet-setup-*.exe").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        const std::wstring other = gamedir::join(dir, fd.cFileName);
        if (_wcsicmp(other.c_str(), setup.c_str()) != 0) DeleteFileW(other.c_str());
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
    return true;
  };

  bool ok = false;
  switch (p.kind) {
    case Kind::Bundle: {
      g_stage = (int)Job::Verify;
      const std::wstring setup = unpack_bundle(p.own);
      lab(L"job: the installer inside the launcher (" + p.own + L")" + (setup.empty() ? L" could not be written out" : L""));
      if (setup.empty()) r->error = L"Установщик из лаунчера не записался на диск.";
      else ok = install(setup, false);
      break;
    }
    case Kind::Setup:
      ok = by_setup();
      break;
    case Kind::ViaLauncher:
    case Kind::SelfOnly: {
      const std::wstring fresh = p.self + L".new", old = p.self + L".old";
      if (!fetch(p.release.launcher, fresh, p.release.launcher_sha256, p.release.launcher_size)) break;
      g_self_phase = true;
      g_stage = (int)Job::Install;
      DeleteFileW(old.c_str());
      // (a running program's file can be renamed, not written over)
      bool replaced = false;
      if (MoveFileExW(p.self.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        replaced = MoveFileExW(fresh.c_str(), p.self.c_str(), 0) != FALSE;
        if (!replaced) MoveFileExW(old.c_str(), p.self.c_str(), 0);
      }
      lab(replaced ? L"job: the launcher's file replaced" : L"job: the launcher's file not replaced");
      if (replaced) {
        r->restart = true;
        r->apply = p.kind == Kind::ViaLauncher;
        ok = true;
      } else {
        DeleteFileW(fresh.c_str());
        g_self_phase = false;
        if (p.kind == Kind::ViaLauncher) ok = by_setup();
        else r->error = L"Лаунчер не обновился: в его папку нельзя писать.";
      }
      break;
    }
    case Kind::Uninstall: {
      g_stage = (int)Job::Verify;
      std::wstring setup = unpack_bundle(p.own);
      if (setup.empty()) setup = cached_setup();
      if (setup.empty() && !p.release.tag.empty()) {
        setup = gamedir::join(update::cache_dir(), L"tmntnet-setup-" + p.release.version + L".exe");
        if (!fetch(p.release.setup, setup, p.release.setup_sha256, p.release.setup_size)) break;
      }
      if (setup.empty()) r->error = L"Установщика под рукой нет: чтобы скачать его, нужна связь с GitHub.";
      else ok = install(setup, true);
      break;
    }
  }
  r->ok = ok;
  if (!PostMessageW(hwnd, kJobDone, 0, reinterpret_cast<LPARAM>(r))) delete r;
}

void start_job(Kind kind) {
  if (m.job != Job::None) return;
  JobPlan p;
  p.kind = kind;
  p.base = m.set.base;
  p.dir = m.set.game_dir;
  p.self = self_path();
  p.own = own_version();
  p.proxy = m.set.proxy;
  if (m.check == Check::Known) p.release = m.latest;
  g_cancel = false;
  g_self_phase = false;
  const bool download = kind == Kind::Setup || kind == Kind::ViaLauncher || kind == Kind::SelfOnly;
  g_stage = (int)(download ? Job::Download : Job::Verify);
  g_got = 0;
  g_total = kind == Kind::Setup ? p.release.setup_size : download ? p.release.launcher_size : 0;
  m.job = (Job)g_stage.load();
  m.job_kind = kind;
  m.job_uninstall = kind == Kind::Uninstall;
  m.job_self = false;
  m.error.clear();
  m.error_host.clear();
  m.settings_open = false;
  KillTimer(g_hwnd, kBusyTimer);
  static const wchar_t* kNames[] = {L"bundle", L"setup", L"via the launcher", L"the launcher alone", L"uninstall"};
  lab(std::wstring(L"job: start, ") + kNames[(int)kind]);
  SetTimer(g_hwnd, kJobTimer, 50, nullptr);
  std::thread(run_job, p, g_hwnd).detach();
  repaint();
}

// --- actions -------------------------------------------------------------------------

bool pick_folder(std::wstring& out) {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dlg->SetTitle(L"Папка игры (где лежит TMNT.EXE)");
  IShellItem* init = nullptr;
  if (!m.set.game_dir.empty() && SUCCEEDED(SHCreateItemFromParsingName(m.set.game_dir.c_str(), nullptr, IID_PPV_ARGS(&init)))) {
    dlg->SetFolder(init);
    init->Release();
  }
  bool ok = false;
  if (SUCCEEDED(dlg->Show(g_hwnd))) {
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

void set_game_dir(const std::wstring& dir) {
  m.set.game_dir = dir;
  ini_set(L"game_dir", dir);
  m.installed = update::installed_version(dir);
  m.error.clear();
  m.error_host.clear();
  lab(L"game folder: " + dir + L", mod " + (m.installed.empty() ? L"none" : m.installed));
}

void choose_folder() {
  if (m.job != Job::None) return;
  for (;;) {
    std::wstring dir;
    if (!pick_folder(dir)) return;
    if (gamedir::has_game(dir)) {
      set_game_dir(dir);
      if (m.check == Check::NotAsked && m.set.check_at_start) start_check();
      return;
    }
    MessageBoxW(g_hwnd, (L"В этой папке нет TMNT.EXE:\n" + dir + L"\n\nУкажите папку, в которую установлена игра.").c_str(),
                kTitle, MB_OK | MB_ICONWARNING);
  }
}

void play() {
  if (m.launching || !game_ok()) return;
  lab(L"play: " + gamedir::join(m.set.game_dir, L"TMNT.EXE") + (m.set.no_play ? L" (lab: not started)" : L""));
  if (!m.set.no_play) {
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.hwnd = g_hwnd;
    sei.lpVerb = L"open";
    const std::wstring exe = gamedir::join(m.set.game_dir, L"TMNT.EXE");
    sei.lpFile = exe.c_str();
    sei.lpDirectory = m.set.game_dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
      m.error = L"Игра не запустилась (ошибка " + std::to_wstring(GetLastError()) + L").";
      return;
    }
  }
  m.launching = true;
  SetTimer(g_hwnd, kCloseTimer, 2500, nullptr);  // (the launcher has done its part)
}

void open_url(const std::wstring& url) {
  lab(L"open: " + url);
  if (!m.set.no_play) ShellExecuteW(g_hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void open_log() {
  const std::wstring log = gamedir::join(m.set.game_dir, L"tmntnet.log");
  lab(L"open log: " + log);
  if (m.set.no_play || !game_ok()) return;
  if (gamedir::exists(log)) ShellExecuteW(g_hwnd, L"open", L"explorer.exe", (L"/select,\"" + log + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
  else ShellExecuteW(g_hwnd, L"open", m.set.game_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void sync_edit();
int hit_at(float x, float y);

bool game_running_now() { return m.fake_game < 0 && game_ok() && update::game_running(m.set.game_dir); }

// A page of the settings comes up: the game's own settings are read again
// (the game may have written them since).
void open_tab(int tab) {
  m.tab = tab;
  m.capture = -1;
  m.capture_note.clear();
  KillTimer(g_hwnd, kBusyTimer);
  if (tab == 0 || m.fake_game >= 0) return;
  gset::load(game_ok() ? m.set.game_dir : std::wstring());
  m.game_busy = game_running_now();
  SetTimer(g_hwnd, kBusyTimer, 1500, nullptr);
  lab(L"page " + std::to_wstring(tab) + L": tmntnet.ini " + (gset::has_mod() ? gset::ini_file() : std::wstring(L"none")) +
      L", savesetting.dat " + (gset::has_levels() ? gset::levels_file() : std::wstring(L"none")) +
      (m.game_busy ? L", the game is running" : L""));
}

// A control of a row of the game's settings.
void change_row(int number, int part) {
  if (number < 0 || number >= gset::kRows || !m.settings_open || (m.tab != 1 && m.tab != 2) || m.game_busy) return;
  const gset::Row r = (gset::Row)number;
  if ((r >= gset::kFirstSound) != (m.tab == 2) || !gset::usable(r)) return;
  const int now = gset::value(r);
  int next = now;
  switch (gset::kind(r)) {
    case gset::Kind::Choice: next = now + (part == view::kPartNext ? 1 : part == view::kPartPrev ? -1 : 0); break;
    case gset::Kind::Switch: next = 1 - now; break;
    case gset::Kind::Level:
      next = part == view::kPartPrev ? now - 1 : part == view::kPartNext ? now + 1 : part - view::kPartPiece;
      next = next < 0 ? 0 : next > 4 ? 4 : next;
      break;
    case gset::Kind::Pair: next = part == view::kPartPiece + 1 ? 1 : 0; break;
  }
  if (next == now) return;
  const bool ok = gset::set(r, next);
  lab(L"set " + update::widen(gset::key(r)) + L" = " + gset::name(r, gset::value(r)) + (ok ? L"" : L" FAILED: " + gset::error()));
}

// УПРАВЛЕНИЕ is open and its keys may be changed.
bool keys_live() { return m.settings_open && m.tab == 3 && !m.game_busy && game_ok() && gset::has_keys(); }

// The ready layouts, a step at a time: the game's, two players, the player's own (when there is one).
void step_layout(int step) {
  if (!keys_live()) return;
  const int now = gset::layout();
  int next = (now + step + 3) % 3;
  if (next == 2 && now != 2 && !gset::has_own_layout()) next = (next + step + 3) % 3;
  if (next == now) return;
  const bool ok = gset::set_layout(next);
  static const wchar_t* kNames[3] = {L"the game's", L"two players", L"own"};
  lab(std::wstring(L"layout: ") + kNames[gset::layout()] + (ok ? L"" : L" FAILED: " + gset::error()));
}

// A key (a DirectInput code; a mouse button: gset::kMouseKey + n) while a cell of the keys waits for one.
void capture_key(int code) {
  if (m.capture < 0 || !keys_live()) return;
  if (code == 0x01) {  // Esc
    m.capture = -1;
    m.capture_note.clear();
    lab(L"key: the wait is over, nothing changed");
  } else if (!gset::key_allowed(code)) {
    m.capture_note = L"Клавишу «" + gset::key_name(code) + L"» игра в таблицу не берёт: нажмите другую. Esc — отмена.";
    lab(L"key: " + gset::key_name(code) + L" is not taken by the game's table");
  } else {
    const int action = m.capture;
    m.capture = -1;
    m.capture_note.clear();
    const bool ok = gset::set_key(m.keys_player, action, code);
    lab(L"key: player " + std::to_wstring(m.keys_player + 1) + L" " + update::widen(gset::action_key(action)) + L" = " +
        gset::key_name(gset::key_code(m.keys_player, action)) + (ok ? L"" : L" FAILED: " + gset::error()));
  }
  repaint();
}

// The key of a WM_KEYDOWN as DirectInput names it: the scan code, +0x80 for an extended key.
int dik_of(WPARAM vk, LPARAM lp) {
  if (vk == VK_ESCAPE) return 0x01;
  if (vk == VK_NUMLOCK) return 0x45;  // (the two that the message and DirectInput tell apart the other way round)
  if (vk == VK_PAUSE) return 0xC5;
  int scan = (int)((lp >> 16) & 0xFF);
  if (!scan) scan = (int)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC) & 0xFF;
  return scan ? (scan | ((lp >> 24) & 1 ? 0x80 : 0)) : 0;
}

// A mouse button pressed while a cell waits: on that very cell it becomes the cell's key.
bool capture_mouse(int button, LPARAM lp) {
  if (m.capture < 0) return false;
  const bool on_cell = hit_at(GET_X_LPARAM(lp) / g_scale, GET_Y_LPARAM(lp) / g_scale) == view::kKey0 + m.capture;
  if (on_cell && gset::key_allowed(gset::kMouseKey + button)) {
    capture_key(gset::kMouseKey + button);
    return true;
  }
  if (button != 0 && on_cell) return true;  // (no mod: no mouse buttons in the table; the wait goes on)
  m.capture = -1;  // a press elsewhere: the wait is over, the press goes its usual way
  m.capture_note.clear();
  repaint();
  return false;
}

void perform(Action a) {
  switch (a) {
    case Action::PickFolder: choose_folder(); break;
    case Action::Update: {
      const Offer o = offer();
      if (!o.on) break;
      start_job(o.source == Source::Bundle ? Kind::Bundle : o.source == Source::Launcher ? Kind::ViaLauncher : Kind::Setup);
      break;
    }
    case Action::UpdateSelf: start_job(Kind::SelfOnly); break;
    case Action::Play: play(); break;
    case Action::Ask: start_check(); break;
    case Action::None: break;
  }
}

void act(int id) {
  using namespace view;
  lab(L"act " + std::to_wstring(id));
  if (id != kUninstall && m.uninstall_armed) {
    m.uninstall_armed = false;
    KillTimer(g_hwnd, kDisarmTimer);
  }
  if (m.capture >= 0) {  // (anything else pressed: the cell stops waiting)
    m.capture = -1;
    m.capture_note.clear();
  }
  switch (id) {
    case kSettings:
      if (m.job == Job::None) m.settings_open = !m.settings_open;
      if (m.settings_open) open_tab(m.tab);
      else KillTimer(g_hwnd, kBusyTimer);
      break;
    case kBack:
      m.settings_open = false;
      KillTimer(g_hwnd, kBusyTimer);
      break;
    case kTab0: case kTab1: case kTab2: case kTab3:
      if (m.settings_open) open_tab(id - kTab0);
      break;
    case kPlayer1: case kPlayer2:
      if (m.settings_open && m.tab == 3) m.keys_player = id - kPlayer1;
      break;
    case kLayoutPrev: step_layout(-1); break;
    case kLayoutNext: step_layout(1); break;
    case kSwitchSelf:
      m.set.auto_self = !m.set.auto_self;
      ini_set(L"auto_self", m.set.auto_self ? L"1" : L"0");
      break;
    case kReset:
      if (keys_live()) {  // the game's own keys (the player's own are kept for СВОЯ)
        const bool ok = gset::set_layout(0);
        lab(std::wstring(L"reset keys") + (ok ? L"" : L" FAILED: " + gset::error()));
      } else if (m.settings_open && (m.tab == 1 || m.tab == 2) && !m.game_busy) {
        const bool ok = gset::reset(m.tab == 2);
        lab(std::wstring(L"reset ") + (m.tab == 2 ? L"sound" : L"graphics") + (ok ? L"" : L" FAILED: " + gset::error()));
      }
      break;
    case kMinimize: ShowWindow(g_hwnd, SW_MINIMIZE); break;
    case kClose: PostMessageW(g_hwnd, WM_CLOSE, 0, 0); break;
    case kPrimary: perform(g_primary); break;
    case kSecondary: perform(g_secondary); break;
    case kCancel: g_cancel = true; break;
    case kPick: case kPickSettings: choose_folder(); break;
    case kPaperLink: open_url(g_paper_url); break;
    case kRepoLink: open_url(m.set.base); break;
    case kSwitchCheck:
      m.set.check_at_start = !m.set.check_at_start;
      ini_set(L"check_at_start", m.set.check_at_start ? L"1" : L"0");
      break;
    case kSwitchAuto:
      m.set.auto_install = !m.set.auto_install;
      ini_set(L"auto_install", m.set.auto_install ? L"1" : L"0");
      break;
    case kProxyNone: case kProxySystem: case kProxyOwn:
      m.set.proxy.mode = (net::Proxy::Mode)(id - kProxyNone);
      save_proxy();
      break;
    case kProxyEdit:
      if (g_edit) SetFocus(g_edit);
      break;
    case kOpenLog: open_log(); break;
    case kUninstall:
      if (!m.uninstall_armed) {
        m.uninstall_armed = true;
        SetTimer(g_hwnd, kDisarmTimer, 5000, nullptr);
      } else {
        m.uninstall_armed = false;
        KillTimer(g_hwnd, kDisarmTimer);
        start_job(Kind::Uninstall);
      }
      break;
    case kSwatch0: case kSwatch1: case kSwatch2: case kSwatch3:
      m.set.accent = id - kSwatch0;
      ini_set(L"accent", std::to_wstring(m.set.accent));
      break;
    default:
      if (id >= kKey0 && id <= kKeyLast) {
        if (keys_live()) m.capture = id - kKey0;
      } else if (id >= kRowFirst) {
        change_row((id - kRowFirst) / kRowParts, (id - kRowFirst) % kRowParts);
      }
      break;
  }
  sync_edit();
  repaint();
}

// --- the window ----------------------------------------------------------------------

float dpi_of(HWND hwnd) {
  typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
  static const GetDpiForWindowFn fn =
      reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  if (fn && hwnd) return (float)fn(hwnd);
  HDC dc = GetDC(nullptr);
  const float dpi = (float)GetDeviceCaps(dc, LOGPIXELSX);
  ReleaseDC(nullptr, dc);
  return dpi;
}

int hit_at(float x, float y) {
  for (size_t i = g_hits.size(); i-- > 0;) {
    const D2D1_RECT_F& r = g_hits[i].rect;
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return g_hits[i].id;
  }
  return 0;
}

// The proxy address is typed into a real edit box laid over its drawn field.
void sync_edit() {
  if (!g_edit) return;
  const bool want = m.settings_open && m.set.proxy.mode == net::Proxy::Own && m.job == Job::None;
  const view::Hit* field = nullptr;
  for (const view::Hit& h : g_hits)
    if (h.id == view::kProxyEdit) field = &h;
  if (!want || !field) {
    if (IsWindowVisible(g_edit)) {
      if (GetFocus() == g_edit) SetFocus(g_hwnd);
      ShowWindow(g_edit, SW_HIDE);
    }
    return;
  }
  const int line = (int)(18 * g_scale);
  const int x = (int)((field->rect.left + 12) * g_scale), w = (int)((field->rect.right - field->rect.left - 24) * g_scale);
  const int y = (int)((field->rect.top + field->rect.bottom) * 0.5f * g_scale) - line / 2;
  RECT now;
  GetWindowRect(g_edit, &now);
  MapWindowPoints(nullptr, g_hwnd, reinterpret_cast<POINT*>(&now), 2);
  if (now.left != x || now.top != y || now.right - now.left != w || !IsWindowVisible(g_edit))
    SetWindowPos(g_edit, nullptr, x, y, w, line, SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOACTIVATE);
}

LRESULT CALLBACK edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB)) {
    SetFocus(g_hwnd);
    return 0;
  }
  if (msg == WM_CHAR && (wp == L'\r' || wp == L'\t' || wp == 27)) return 0;
  return CallWindowProcW(g_edit_proc, hwnd, msg, wp, lp);
}

HFONT edit_font() {
  return CreateFontW(-(int)(13 * g_scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
}

void make_edit() {
  g_edit = CreateWindowExW(0, L"EDIT", m.set.proxy.address.c_str(), WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, g_hwnd, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
  if (!g_edit) return;
  g_edit_font = edit_font();
  SendMessageW(g_edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_edit_font), TRUE);
  SendMessageW(g_edit, EM_SETLIMITTEXT, 200, 0);
  SendMessageW(g_edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"адрес:порт"));
  g_edit_brush = CreateSolidBrush(RGB(0x0A, 0x13, 0x10));
  g_edit_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(edit_proc)));
}

bool make_target() {
  if (g_rt) return true;
  RECT rc;
  GetClientRect(g_hwnd, &rc);
  const float dpi = 96.0f * g_scale;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
  return SUCCEEDED(gfx::d2d()->CreateHwndRenderTarget(
      props, D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)), &g_rt));
}

void on_paint() {
  if (!make_target()) return;
  const view::View v = make_view();
  g_rt->BeginDraw();
  g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
  view::paint(g_rt, v, g_hits);
  const HRESULT hr = g_rt->EndDraw();
  static int told = 0;
  if (told < 2 || FAILED(hr)) {
    if (told++ < 6) lab(L"paint: the window's picture, result " + std::to_wstring((unsigned)hr));
  }
  if (hr == D2DERR_RECREATE_TARGET) {
    g_rt->Release();
    g_rt = nullptr;
  }
  ValidateRect(g_hwnd, nullptr);
  sync_edit();
}

void move_focus(bool back) {
  if (g_hits.empty()) return;
  int at = -1;
  for (size_t i = 0; i < g_hits.size(); ++i)
    if (g_hits[i].id == g_focus) at = (int)i;
  const int n = (int)g_hits.size();
  at = at < 0 ? (back ? n - 1 : 0) : (at + (back ? n - 1 : 1)) % n;
  g_focus = g_hits[at].id;
  repaint();
}

void on_check_done(CheckResult* r) {
  if (r->ok) {
    m.check = Check::Known;
    m.latest = r->release;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t at[16];
    swprintf(at, 16, L"%02d:%02d", t.wHour, t.wMinute);
    m.checked_at = at;
    lab(L"check: latest " + m.latest.version + L" (" + m.latest.tag + L"), protocol " + std::to_wstring(m.latest.protocol) +
        L" since " + m.latest.protocol_since + L", setup " + m.latest.setup + L" " + std::to_wstring(m.latest.setup_size) +
        L" sha " + m.latest.setup_sha256 + L", launcher " + m.latest.launcher + L", title \"" + m.latest.title + L"\", notes " +
        std::to_wstring(m.latest.notes.size()) + L", date " + m.latest.date);
    for (const std::wstring& n : m.latest.notes) lab(L"check: note \"" + n + L"\"");
    if (m.set.auto_install && game_ok() && !m.installed.empty() && offer().on && m.job == Job::None &&
        !update::game_running(m.set.game_dir)) {
      perform(Action::Update);
    } else if (m.set.auto_self && !m.self_tried && newer_self() && m.job == Job::None && !m.settings_open) {
      // A newer launcher: this one takes it and hands over to it, once a run. The
      // mod in the game folder stays; the new launcher offers what it carries.
      m.self_tried = true;
      const std::wstring self = self_path();
      if (update::can_write(self.substr(0, self.find_last_of(L'\\')))) {
        lab(L"self: a newer launcher (" + m.latest.version + L"), taking it");
        start_job(Kind::SelfOnly);
      } else {
        lab(L"self: a newer launcher (" + m.latest.version + L"), but this one's folder cannot be written to");
      }
    }
  } else {
    m.check = Check::Failed;
    m.check_failure = r->why;
    lab(L"check: failed: " + r->why.text());
  }
  delete r;
  repaint();
}

void on_job_done(JobResult* r) {
  KillTimer(g_hwnd, kJobTimer);
  m.job = Job::None;
  m.installed = update::installed_version(m.set.game_dir);
  lab(std::wstring(L"job: done ") + (r->ok ? L"ok" : r->cancelled ? L"cancelled" : L"failed: " + r->error) + L"; mod now " +
      (m.installed.empty() ? L"none" : m.installed));
  if (!r->ok && !r->cancelled) {
    m.error = r->error;
    m.error_host = r->host;
  }
  const bool restart = r->restart, apply = r->apply;
  delete r;
  if (restart) {  // the new launcher takes over
    std::wstring cmd = L"\"" + self_path() + L"\" /updated";
    if (apply) cmd += L" /apply";
    if (m.set.file != default_ini()) cmd += L" /ini \"" + m.set.file + L"\"";
    if (!g_log_file.empty()) cmd += L" /log \"" + g_log_file + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    lab(L"restart: " + cmd);
    if (m.set.no_play) {  // (lab: the new file is in place; nothing more to see)
      DestroyWindow(g_hwnd);
      return;
    }
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      DestroyWindow(g_hwnd);
      return;
    }
    m.error = L"Новый лаунчер на месте, но не запустился: откройте его ещё раз.";
  }
  // (started to install what it carries: GitHub is asked once that is done)
  if (m.check == Check::NotAsked && m.set.check_at_start) start_check();
  repaint();
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_NCHITTEST: {
      POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hwnd, &p);
      const float x = p.x / g_scale, y = p.y / g_scale;
      return (y < view::kCaption && x < 824.0f) ? HTCAPTION : HTCLIENT;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: on_paint(); return 0;
    case WM_DISPLAYCHANGE: repaint(); return 0;
    case WM_DPICHANGED: {
      g_scale = HIWORD(wp) / 96.0f;
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd, nullptr, r->left, r->top, (int)(view::kWidth * g_scale), (int)(view::kHeight * g_scale),
                   SWP_NOZORDER | SWP_NOACTIVATE);
      if (g_rt) {
        g_rt->Release();
        g_rt = nullptr;
      }
      if (g_edit_font) {
        DeleteObject(g_edit_font);
        g_edit_font = edit_font();
        if (g_edit) SendMessageW(g_edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_edit_font), TRUE);
      }
      repaint();
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!g_tracking) {
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&t);
        g_tracking = true;
      }
      const int id = hit_at(GET_X_LPARAM(lp) / g_scale, GET_Y_LPARAM(lp) / g_scale);
      if (id != g_hover) {
        g_hover = id;
        repaint();
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      g_tracking = false;
      if (g_hover) {
        g_hover = 0;
        repaint();
      }
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT && reinterpret_cast<HWND>(wp) == hwnd) {
        SetCursor(LoadCursorW(nullptr, g_hover == view::kProxyEdit ? IDC_IBEAM : g_hover ? IDC_HAND : IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_LBUTTONDOWN:
      SetFocus(hwnd);
      if (capture_mouse(0, lp)) return 0;
      g_down = hit_at(GET_X_LPARAM(lp) / g_scale, GET_Y_LPARAM(lp) / g_scale);
      g_focus = 0;
      if (g_down) SetCapture(hwnd);
      repaint();
      return 0;
    case WM_LBUTTONUP: {
      const int was = g_down;
      g_down = 0;
      if (GetCapture() == hwnd) ReleaseCapture();
      if (was && hit_at(GET_X_LPARAM(lp) / g_scale, GET_Y_LPARAM(lp) / g_scale) == was) act(was);
      repaint();
      return 0;
    }
    case WM_RBUTTONDOWN:
      if (capture_mouse(1, lp)) return 0;
      break;
    case WM_MBUTTONDOWN:
      if (capture_mouse(2, lp)) return 0;
      break;
    case WM_XBUTTONDOWN:
      if (capture_mouse(HIWORD(wp) == XBUTTON2 ? 4 : 3, lp)) return TRUE;
      break;
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == SC_KEYMENU) return 0;  // (no menu here: Alt is a key like any other)
      break;
    case WM_SYSKEYDOWN:
      if (m.capture < 0) break;
      [[fallthrough]];
    case WM_KEYDOWN:
      if (m.capture >= 0) {  // a cell of the keys waits: this key is its key
        if (!(lp & (1 << 30))) capture_key(dik_of(wp, lp));
        return 0;
      }
      if (wp == VK_TAB) move_focus(GetKeyState(VK_SHIFT) < 0);
      else if ((wp == VK_RETURN || wp == VK_SPACE) && g_focus) act(g_focus);
      else if (wp == VK_ESCAPE && m.settings_open) act(view::kBack);
      return 0;
    case WM_COMMAND:
      if (reinterpret_cast<HWND>(lp) == g_edit && HIWORD(wp) == EN_CHANGE) {
        wchar_t buf[256];
        GetWindowTextW(g_edit, buf, 256);
        m.set.proxy.address = buf;
        save_proxy();
      }
      return 0;
    case WM_CTLCOLOREDIT:
      SetTextColor(reinterpret_cast<HDC>(wp), RGB(0xEE, 0xF3, 0xE2));
      SetBkColor(reinterpret_cast<HDC>(wp), RGB(0x0A, 0x13, 0x10));
      return reinterpret_cast<LRESULT>(g_edit_brush);
    case WM_TIMER:
      if (wp == kJobTimer) {
        m.job = (Job)g_stage.load();
        m.job_self = g_self_phase;
        m.flow += 1.6f;
        repaint();
      } else if (wp == kDisarmTimer) {
        KillTimer(hwnd, kDisarmTimer);
        m.uninstall_armed = false;
        repaint();
      } else if (wp == kCloseTimer) {
        KillTimer(hwnd, kCloseTimer);
        DestroyWindow(hwnd);
      } else if (wp == kBusyTimer) {  // a page of the game's settings is open
        const bool busy = game_running_now();
        if (busy != m.game_busy) {
          m.game_busy = busy;
          if (!busy) gset::load(m.set.game_dir);  // (the game wrote its settings on its way out)
          repaint();
        }
      }
      return 0;
    case kCheckDone: on_check_done(reinterpret_cast<CheckResult*>(lp)); return 0;
    case kJobDone: on_job_done(reinterpret_cast<JobResult*>(lp)); return 0;
    case WM_CLOSE:
      g_cancel = true;  // (a download stops; an installer already running finishes by itself)
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      if (g_rt) {
        g_rt->Release();
        g_rt = nullptr;
      }
      PostQuitMessage(0);
      return 0;
    default: break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// `at`: a place for the window instead of the screen's middle (lab), shown
// there without taking the keyboard.
bool make_window(bool visible, const POINT* at) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_DROPSHADOW;
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_LAUNCHER));
  wc.hIconSm = wc.hIcon;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);
  g_scale = dpi_of(nullptr) / 96.0f;
  const int w = (int)(view::kWidth * g_scale), h = (int)(view::kHeight * g_scale);
  RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int x = at ? at->x : work.left + (work.right - work.left - w) / 2;
  const int y = at ? at->y : work.top + (work.bottom - work.top - h) / 2;
  g_hwnd = CreateWindowExW(at ? WS_EX_NOACTIVATE : 0, kClass, kTitle, WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN,
                           x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
  if (!g_hwnd) return false;
  const float real = dpi_of(g_hwnd) / 96.0f;  // (the monitor it landed on)
  if (real != g_scale) {
    g_scale = real;
    SetWindowPos(g_hwnd, nullptr, 0, 0, (int)(view::kWidth * g_scale), (int)(view::kHeight * g_scale),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  const int round = 2;  // DWMWCP_ROUND (Windows 11; older ones ignore it)
  DwmSetWindowAttribute(g_hwnd, 33, &round, sizeof(round));
  make_edit();
  if (visible) {
    ShowWindow(g_hwnd, at ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
  }
  return true;
}

// --- lab: canned states and their pictures ------------------------------------------------

std::wstring today_text() {
  static const wchar_t* kMonths[] = {L"января", L"февраля", L"марта", L"апреля", L"мая", L"июня",
                                     L"июля", L"августа", L"сентября", L"октября", L"ноября", L"декабря"};
  SYSTEMTIME t;
  GetLocalTime(&t);
  return std::to_wstring(t.wDay) + L" " + kMonths[t.wMonth - 1] + L" " + std::to_wstring(t.wYear);
}

// The pictures are about the release this file is built for: its version and
// notes (dist/release.txt) stand as the latest on GitHub, `was` as the mod
// found in the game folder and as the version of the launcher shown.
void canned(const std::wstring& state, std::wstring was) {
  update::Release mine = carried();  // (read before the stand-ins below are set)
  if (was.empty()) was = mine.protocol_since.empty() || mine.protocol_since == mine.version ? L"0.1.0" : mine.protocol_since;
  m.fake_game = state == L"nogame" ? 0 : 1;
  m.fake_bundle = 1;
  m.fake_own = was;
  m.set.game_dir = state == L"nogame" ? L"" : L"D:\\Games\\Teenage Mutant Ninja Turtles";
  m.check = Check::Known;
  m.checked_at = L"18:42";
  m.installed = was;
  update::Release& r = m.latest;
  r = mine;
  r.date = today_text();
  if (HRSRC inside = FindResourceW(nullptr, MAKEINTRESOURCEW(RES_SETUP), RT_RCDATA)) r.setup_size = SizeofResource(nullptr, inside);
  r.setup_sha256 = std::wstring(64, L'0');
  r.launcher = L"tmntnet-launcher.exe";
  WIN32_FILE_ATTRIBUTE_DATA self{};
  if (GetFileAttributesExW(self_path().c_str(), GetFileExInfoStandard, &self)) r.launcher_size = self.nFileSizeLow;
  r.launcher_sha256 = std::wstring(64, L'0');
  g_canned_carried = r;  // (what the launcher of the "bundle" pictures carries)
  if (state == L"install") m.installed.clear(), m.fake_own = r.version;  // (a first start of this release's launcher)
  else if (state == L"ready") m.installed = r.version, m.fake_own = r.version;
  else if (state == L"offline") {
    m.check = Check::Failed;
    m.check_failure.host = L"github.com";
  } else if (state == L"bundle") {  // nothing installed, GitHub silent: the launcher's own load
    m.installed.clear();
    m.fake_own = r.version;
    m.check = Check::Failed;
    m.check_failure.host = L"github.com";
  } else if (state == L"bundle-update") {  // an older mod, GitHub not asked
    m.fake_own = r.version;
    m.check = Check::NotAsked;
  } else if (state == L"checking") m.check = Check::Asking;
  else if (state == L"settings") m.settings_open = true;
  else if (state == L"graphics" || state == L"sound" || state == L"controls" || state == L"controls-key") {
    m.settings_open = true;
    m.tab = state == L"sound" ? 2 : state == L"graphics" ? 1 : 3;
    gset::load_canned();
    if (state == L"controls-key") m.capture = 5;  // (the weak attack's cell waits for a key)
  } else if (state == L"selfupdate") {  // at its start: a newer launcher is being taken
    m.installed = was;
    m.job = Job::Download;
    m.job_kind = Kind::SelfOnly;
    g_total = r.launcher_size;
    g_got = r.launcher_size / 100 * 62;
    m.flow = 7.0f;
  } else if (state == L"updated") {  // the new launcher's first picture
    m.fake_own = r.version;
    m.just_updated = true;
  }
  else if (state == L"settings-own") {
    m.settings_open = true;
    m.set.proxy.mode = net::Proxy::Own;
    m.set.proxy.address = L"127.0.0.1:8080";
  } else if (state == L"error") {
    m.error = L"Сервер загрузок GitHub не отвечает: включите прокси в настройках.";
    m.error_host = L"release-assets.githubusercontent.com";
  } else if (state == L"downloading") {
    m.job = Job::Download;
    g_total = r.launcher_size;
    g_got = r.launcher_size / 100 * 62;
    m.flow = 7.0f;
  } else if (state == L"installing") {
    m.job = Job::Install;
  } else if (state == L"protocol") {  // a release whose protocol is new
    r.protocol += 1;
    r.protocol_since = r.version;
  } else if (state == L"self") {  // the mod is the latest, the launcher is not
    m.installed = r.version;
  }
}

void draw_once(ID2D1RenderTarget* rt, void*) {
  std::vector<view::Hit> hits;
  view::paint(rt, make_view(), hits);
  if (!g_log_file.empty())
    for (const view::Hit& h : hits)
      lab(L"hit " + std::to_wstring(h.id) + L": " + std::to_wstring((int)h.rect.left) + L"," + std::to_wstring((int)h.rect.top) +
          L" - " + std::to_wstring((int)h.rect.right) + L"," + std::to_wstring((int)h.rect.bottom));
}

struct LabStep {
  DWORD at;
  std::wstring what;  // a control's name, or "snap"
};
std::vector<LabStep> g_lab_steps;
std::wstring g_snap_file;
DWORD g_started = 0;

int control_of(const std::wstring& name) {
  static const struct {
    const wchar_t* name;
    int id;
  } kNames[] = {{L"primary", view::kPrimary}, {L"secondary", view::kSecondary}, {L"cancel", view::kCancel},
                {L"settings", view::kSettings}, {L"back", view::kBack}, {L"check", view::kSwitchCheck},
                {L"auto", view::kSwitchAuto}, {L"proxy_none", view::kProxyNone}, {L"proxy_system", view::kProxySystem},
                {L"proxy_own", view::kProxyOwn}, {L"uninstall", view::kUninstall}, {L"log", view::kOpenLog},
                {L"swatch0", view::kSwatch0}, {L"swatch1", view::kSwatch1}, {L"swatch2", view::kSwatch2},
                {L"swatch3", view::kSwatch3}, {L"paper", view::kPaperLink}, {L"repo", view::kRepoLink},
                {L"tab_launcher", view::kTab0}, {L"tab_graphics", view::kTab1}, {L"tab_sound", view::kTab2},
                {L"tab_controls", view::kTab3}, {L"self", view::kSwitchSelf}, {L"reset", view::kReset},
                {L"player1", view::kPlayer1}, {L"player2", view::kPlayer2}, {L"layout.prev", view::kLayoutPrev},
                {L"layout.next", view::kLayoutNext}};
  for (auto& n : kNames)
    if (name == n.name) return n.id;
  if (name.compare(0, 4, L"key.") == 0) {  // an action's cell on УПРАВЛЕНИЕ: key.jump
    for (int a = 0; a < gset::kKeyActions; ++a)
      if (name.substr(4) == update::widen(gset::action_key(a))) return view::kKey0 + a;
    return 0;
  }
  // A part of a row of the game's settings: display.next, wide.switch, music.b2.
  const size_t dot = name.find(L'.');
  if (dot == std::wstring::npos) return 0;
  const std::wstring row = name.substr(0, dot), part = name.substr(dot + 1);
  for (int r = 0; r < gset::kRows; ++r) {
    if (row != update::widen(gset::key((gset::Row)r))) continue;
    const int p = part == L"prev" ? view::kPartPrev : part == L"next" ? view::kPartNext : part == L"switch" ? view::kPartSwitch :
                  part.size() == 2 && part[0] == L'b' ? view::kPartPiece + (part[1] - L'0') : -1;
    return p < 0 ? 0 : view::kRowFirst + r * view::kRowParts + p;
  }
  return 0;
}

void CALLBACK lab_tick(HWND, UINT, UINT_PTR, DWORD) {
  const DWORD now = GetTickCount() - g_started;
  while (!g_lab_steps.empty() && g_lab_steps.front().at <= now) {
    const LabStep step = g_lab_steps.front();
    g_lab_steps.erase(g_lab_steps.begin());
    if (step.what == L"snap") {
      m.job = m.job == Job::None ? Job::None : (Job)g_stage.load();
      const bool ok = gfx::render_png(g_snap_file, view::kWidth, view::kHeight, 1.0f, draw_once, nullptr);
      lab(std::wstring(L"snap: ") + (ok ? L"saved " : L"NOT saved ") + g_snap_file);
      DestroyWindow(g_hwnd);
      return;
    }
    // A key or a mouse button for the cell that waits: press.1E, mouse.1.
    if (step.what.compare(0, 6, L"press.") == 0 || step.what.compare(0, 6, L"mouse.") == 0) {
      const int n = (int)wcstol(step.what.c_str() + 6, nullptr, 16);
      lab(L"lab: " + step.what + (m.capture >= 0 ? L"" : L" — no cell waits"));
      capture_key(step.what[0] == L'm' ? gset::kMouseKey + n : n);
      continue;
    }
    // A press goes the way a click does: only a control that is on the screen now takes it.
    std::vector<view::Hit> hits;
    gfx::render_png(gamedir::join(update::cache_dir(), L"lab-hits.png"), view::kWidth, view::kHeight, 1.0f,
                    [](ID2D1RenderTarget* rt, void* out) { view::paint(rt, make_view(), *static_cast<std::vector<view::Hit>*>(out)); },
                    &hits);
    const int id = control_of(step.what);
    bool there = false;
    for (const view::Hit& h : hits) there = there || h.id == id;
    lab(L"lab: press " + step.what + (there ? L"" : L" — NOT on the screen"));
    if (there) act(id);
  }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  // (an installer that fails must not put a system box on the screen)
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::wstring shot, state = L"update", was, feed, feed_tag;
  float scale = 1.0f;
  int accent = -1;
  DWORD snap_after = 0;
  bool fonts = false, apply = false, placed = false;
  POINT place{};
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    const bool more = i + 1 < argc;
    if (a == L"/ini" && more) m.set.file = argv[++i];
    else if (a == L"/log" && more) g_log_file = argv[++i];
    else if (a == L"/shot" && more) shot = argv[++i];
    else if (a == L"/state" && more) state = argv[++i];
    else if (a == L"/was" && more) was = argv[++i];
    else if (a == L"/scale" && more) scale = (float)_wtof(argv[++i]);
    else if (a == L"/accent" && more) accent = _wtoi(argv[++i]);
    else if (a == L"/snap" && more) g_snap_file = argv[++i];
    else if (a == L"/after" && more) snap_after = (DWORD)_wtoi(argv[++i]);
    else if (a == L"/fonts") fonts = true;
    else if (a == L"/apply") apply = true;
    else if (a == L"/updated") m.just_updated = m.self_tried = true;  // (and no second update in a row)
    else if (a == L"/at" && more) {
      const std::wstring s = argv[++i];
      size_t comma = s.find(L',');
      if (comma != std::wstring::npos) {
        place.x = _wtoi(s.c_str());
        place.y = _wtoi(s.c_str() + comma + 1);
        placed = true;
      }
    } else if (a == L"/feed" && i + 2 < argc) feed = argv[i + 1], feed_tag = argv[i + 2], i += 2;
    else if (a == L"/do" && more) {
      const std::wstring s = argv[++i];
      size_t at = s.find(L'@');
      if (at != std::wstring::npos) g_lab_steps.push_back({(DWORD)_wtoi(s.c_str() + at + 1), s.substr(0, at)});
    }
  }
  LocalFree(argv);

  std::wstring problem;
  if (!gfx::init(&problem)) {
    MessageBoxW(nullptr, (L"Не удалось запустить " + problem + L": лаунчеру нужна Windows 7 или новее.").c_str(), kTitle,
                MB_OK | MB_ICONERROR);
    return 3;
  }
  if (fonts) {
    lab(gfx::fonts_report());
    gfx::shutdown();
    return 0;
  }
  if (!feed.empty()) {
    FILE* f = _wfopen(feed.c_str(), L"rb");
    std::string text;
    if (f) {
      char buf[8192];
      for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
      fclose(f);
    }
    update::Release r;
    const bool found = update::from_feed(text, feed_tag, r);
    lab(std::wstring(L"feed: ") + (found ? L"found" : L"NOT found") + L"; date " + r.date + L"; protocol " + std::to_wstring(r.protocol) +
        L" since " + r.protocol_since + L"; setup " + r.setup + L" " + std::to_wstring(r.setup_size) + L" " + r.setup_sha256 +
        L"; launcher " + r.launcher + L" " + std::to_wstring(r.launcher_size) + L" " + r.launcher_sha256 + L"; title \"" + r.title + L"\"");
    for (const std::wstring& n : r.notes) lab(L"feed: note \"" + n + L"\"");
    gfx::shutdown();
    return found ? 0 : 1;
  }
  if (!shot.empty()) {  // a canned state's picture, no window
    canned(state, was);
    if (accent >= 0 && accent <= 3) m.set.accent = accent;
    const bool ok = gfx::render_png(shot, view::kWidth, view::kHeight, scale, draw_once, nullptr);
    gfx::shutdown();
    return ok ? 0 : 1;
  }

  const bool lab_run = !g_snap_file.empty() || !g_lab_steps.empty();
  if (m.set.file.empty()) m.set.file = default_ini();
  load_settings();
  DeleteFileW((self_path() + L".old").c_str());  // (what an update of the launcher left)
  HANDLE once = nullptr;
  if (!lab_run && !placed) {  // one launcher at a time: the second shows the first
    once = CreateMutexW(nullptr, TRUE, L"tmntnet-launcher-running");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
      if (HWND other = FindWindowW(kClass, nullptr)) {
        if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
        SetForegroundWindow(other);
      }
      gfx::shutdown();
      return 0;
    }
  }
  if (!gamedir::has_game(m.set.game_dir)) {
    const std::wstring found = gamedir::find();
    if (!found.empty()) {
      m.set.game_dir = found;
      ini_set(L"game_dir", found);
    }
  }
  m.installed = update::installed_version(m.set.game_dir);
  lab(L"start: launcher " + own_version() + (has_bundle() ? L" with its installer" : L" without an installer inside") +
      L", game folder \"" + m.set.game_dir + L"\"" + (game_ok() ? L"" : L" (no game)") + L", mod " +
      (m.installed.empty() ? L"none" : m.installed) + L", base " + m.set.base);
  if (!make_window(!lab_run || placed, placed ? &place : nullptr)) return 3;
  // Started by the launcher before it, to put in what this one carries.
  if (apply && game_ok() && has_bundle() && (m.installed.empty() || update::compare(own_version(), m.installed) > 0))
    start_job(Kind::Bundle);
  else if (m.set.check_at_start) start_check();
  if (lab_run) {
    g_started = GetTickCount();
    if (!g_snap_file.empty()) g_lab_steps.push_back({snap_after, L"snap"});
    std::stable_sort(g_lab_steps.begin(), g_lab_steps.end(), [](const LabStep& a, const LabStep& b) { return a.at < b.at; });
    SetTimer(nullptr, kLabTimer, 50, lab_tick);
  }
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (g_edit_brush) DeleteObject(g_edit_brush);
  if (g_edit_font) DeleteObject(g_edit_font);
  gfx::shutdown();
  if (once) CloseHandle(once);
  CoUninitialize();
  return 0;
}
