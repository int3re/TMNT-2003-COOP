#include "mod/mod_pages.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "common/log.h"
#include "mod/ambient.h"
#include "mod/audio.h"
#include "mod/binds.h"
#include "mod/csel.h"
#include "mod/dinput_hook.h"
#include "mod/display.h"
#include "mod/four_hud.h"
#include "mod/native_menu.h"
#include "mod/own_camera.h"
#include "mod/quality.h"
#include "mod/widescreen.h"

namespace mod_pages {
namespace {

Config g_cfg;    // the values as they are now
Config g_start;  // as the game started: some only change at a start
std::wstring g_ini;

void save(const char* section, const char* key, const std::string& value) {
  if (ini_set(g_ini, section, key, value))
    LOGI("settings: [%s] %s=%s", section, key, value.c_str());
  else
    LOGW("settings: cannot write [%s] %s to tmntnet.ini", section, key);
}

// A setting that only changes at the next start: a notice when it differs
// from how the game started.
void restart_notice(bool differs) {
  if (differs) native_menu::notify("НУЖЕН ПЕРЕЗАПУСК ИГРЫ");
}

// Words only from letters the game's labels have (no Х Щ Ъ Э), numbers in
// its plain font.
const std::vector<std::string> kOffOn = {"ВЫК", "ВКЛ"};

// The place of the value nearest to v among n.
int nearest(const int* values, int n, int v) {
  int best = 0;
  for (int i = 1; i < n; ++i)
    if (std::abs(values[i] - v) < std::abs(values[best] - v)) best = i;
  return best;
}

native_menu::Row choice(const char* label, std::vector<std::string> values, std::function<int()> get,
                        std::function<void(int)> set, int default_value = native_menu::kNoDefault) {
  native_menu::Row r;
  r.label = label;
  r.values = std::move(values);
  r.get = std::move(get);
  r.set = std::move(set);
  r.default_value = default_value;
  return r;
}

}  // namespace

void install(const Config& cfg) {
  g_cfg = cfg;
  g_start = cfg;
  g_ini = module_dir(nullptr) + L"tmntnet.ini";
}

// --- ПРОЧИЕ ПАРАМЕТРЫ: the second player -------------------------------------------

std::vector<native_menu::Row> other_rows() {
  std::vector<native_menu::Row> rows;
  rows.push_back(choice(
      "ВЫБОР ЗА ИГРОКА 2", kOffOn, [] { return g_cfg.p2_autopick ? 1 : 0; },
      [](int v) {
        g_cfg.p2_autopick = v == 1;
        csel::set_autopick(g_cfg.p2_autopick, g_cfg.p2_autopick_delay_ms, g_cfg.p2_character);
        save("mod", "p2_autopick", v ? "1" : "0");
      },
      1));
  rows.push_back(choice(
      "ГЕРОЙ ИГРОКА 2", {"НАУГАД", "ПЕРВЫЙ СВОБОДНЫЙ"}, [] { return g_cfg.p2_character == -2 ? 1 : 0; },
      [](int v) {
        g_cfg.p2_character = v ? -2 : -1;
        csel::set_autopick(g_cfg.p2_autopick, g_cfg.p2_autopick_delay_ms, g_cfg.p2_character);
        save("mod", "p2_character", v ? "first" : "auto");
      },
      0));
  rows.push_back(choice(
      "ИГРОК 2 ПО СЕТИ", kOffOn, [] { return g_cfg.p2_lock ? 1 : 0; },
      [](int v) {
        g_cfg.p2_lock = v == 1;
        set_p2_lock(g_cfg.p2_lock);
        save("mod", "p2_lock", v ? "1" : "0");
      },
      1));
  // КАМЕРА ПО СЕТИ: in a game together this PC draws the world around its own
  // turtle (СВОЯ), or with the game's one camera for all (КАК В ИГРЕ). Only
  // what this PC shows: friends may each have their own choice.
  rows.push_back(choice(
      "КАМЕРА ПО СЕТИ", {"КАК В ИГРЕ", "СВОЯ"}, [] { return g_cfg.own_camera ? 1 : 0; },
      [](int v) {
        g_cfg.own_camera = v == 1;
        own_camera::set_enabled(g_cfg.own_camera);
        save("video", "own_camera", v ? "1" : "0");
      },
      1));
  return rows;
}

// --- ПАРАМЕТРЫ ЗВУКА: the movies -----------------------------------------------

std::vector<native_menu::Row> sound_rows() {
  std::vector<native_menu::Row> rows;
  // РОЛИКИ: a slider like МУЗЫКА; until it is moved the movies follow the
  // music (and the knob shows the music's level). ПО УМОЛЧАНИЮ: follow again.
  native_menu::Row movies;
  movies.label = "РОЛИКИ";
  movies.slider = 5;
  movies.get = [] { return audio::movie_level(); };
  movies.raw = [] { return audio::movie_level_follows() ? -1 : audio::movie_level(); };
  movies.set = [](int v) {
    audio::set_movie_level(v);
    save("audio", "movie_level", std::to_string(v < 0 ? -1 : v));
  };
  movies.default_value = -1;
  rows.push_back(movies);
  rows.push_back(choice(
      "ЗАСТАВКА", {"ПОКАЗЫВАТЬ", "ПРОПУСКАТЬ"}, [] { return g_cfg.intro_skip ? 1 : 0; },
      [](int v) {
        g_cfg.intro_skip = v == 1;
        save("audio", "intro", v ? "skip" : "play");
      },
      0));
  return rows;
}

// --- ПАРАМЕТРЫ ГРАФИКИ: the window, the wide screen, the view, the quality --------

void resolution_chosen(int w, int h) {
  if (w <= 0 || h <= 0) return;
  save("video", "resolution", std::to_string(w) + "x" + std::to_string(h));
}

std::vector<native_menu::Row> graphics_rows() {
  std::vector<native_menu::Row> rows;
  rows.push_back(choice(
      "ОКНО ИГРЫ", {"ПОЛНЫЙ", "БЕЗ РАМКИ", "В ОКНЕ"},
      [] { return g_cfg.display == "borderless" ? 1 : g_cfg.display == "windowed" ? 2 : 0; },
      [](int v) {
        static const char* kNames[] = {"fullscreen", "borderless", "windowed"};
        g_cfg.display = kNames[v];
        save("video", "display", kNames[v]);
        if (!display::switch_mode(v)) restart_notice(true);
      }));
  rows.push_back(choice(
      "ШИРОКИЙ ФОРМАТ", kOffOn, [] { return g_cfg.widescreen ? 1 : 0; },
      [](int v) {
        g_cfg.widescreen = v == 1;
        save("video", "widescreen", v ? "1" : "0");
        restart_notice(g_cfg.widescreen != g_start.widescreen);
      }));
  // The 4:3 pictures (menus, movies) and the HUD on a wide screen, in one row:
  // the side bars blurred or dark, or the pictures stretched, or all of it.
  rows.push_back(choice(
      "ПО БОКАМ", {"РАЗМЫТО", "ТЕМНО", "РАСТЯНУТЬ ФОН", "РАСТЯНУТЬ ВСЕ"},
      [] {
        if (!g_cfg.fullscreen_pillarbox) return g_cfg.hud_center ? 2 : 3;
        return g_cfg.side_bars_blur ? 0 : 1;
      },
      [](int v) {
        g_cfg.fullscreen_pillarbox = v <= 1;
        g_cfg.hud_center = v <= 2;
        if (v <= 1) g_cfg.side_bars_blur = v == 0;
        widescreen::set_pillarbox(g_cfg.fullscreen_pillarbox);
        widescreen::set_hud_center(g_cfg.hud_center);
        ambient::set_blur(g_cfg.side_bars_blur);
        save("video", "fullscreen_images", g_cfg.fullscreen_pillarbox ? "pillarbox" : "stretch");
        save("video", "hud", g_cfg.hud_center ? "center" : "stretch");
        save("video", "side_bars", g_cfg.side_bars_blur ? "blur" : "black");
      }));
  // ОБЗОР: the 3D view wider than the game's (what is shown: the game's own
  // reckoning of the screen stays, so friends' games stay in step).
  static const int kFov[] = {100, 110, 120, 130, 140, 150};
  rows.push_back(choice(
      "ОБЗОР", {"КАК В ИГРЕ", "110%", "120%", "130%", "140%", "150%"}, [] { return nearest(kFov, 6, g_cfg.fov); },
      [](int v) {
        g_cfg.fov = kFov[v];
        widescreen::set_fov(g_cfg.fov);
        save("video", "fov", std::to_string(g_cfg.fov));
      }));
  // ПАНЕЛИ: the panels at the top (life, score, the hearts) smaller.
  static const int kHud[] = {100, 90, 80, 70, 60, 50};
  rows.push_back(choice(
      "ПАНЕЛИ", {"КАК В ИГРЕ", "90%", "80%", "70%", "60%", "50%"}, [] { return nearest(kHud, 6, g_cfg.hud_size); },
      [](int v) {
        g_cfg.hud_size = kHud[v];
        four_hud::set_size(g_cfg.hud_size);
        save("video", "hud_size", std::to_string(g_cfg.hud_size));
      }));
  // КАЧЕСТВО: anti-aliasing and anisotropic filtering, both at once, as two
  // steps up from the game's; a notice says what the picture has now.
  struct Quality {
    int msaa, aniso;
  };
  static const Quality kQuality[] = {{0, 0}, {4, 16}, {8, 16}};
  rows.push_back(choice(
      "КАЧЕСТВО", {"КАК В ИГРЕ", "ВЫСОКОЕ", "МАКСИМУМ"},
      [] { return g_cfg.msaa == 0 && g_cfg.anisotropy == 0 ? 0 : g_cfg.msaa >= 8 ? 2 : 1; },
      [](int v) {
        g_cfg.msaa = kQuality[v].msaa;
        g_cfg.anisotropy = kQuality[v].aniso;
        quality::set_anisotropy(g_cfg.anisotropy);
        save("video", "antialiasing", std::to_string(g_cfg.msaa));
        save("video", "anisotropy", std::to_string(g_cfg.anisotropy));
        int now = quality::set_msaa(g_cfg.msaa);
        if (now < 0)
          restart_notice(true);
        else if (now > 0)
          native_menu::notify("СГЛАЖИВАНИЕ", std::to_string(now) + "X");
        else
          native_menu::notify(g_cfg.msaa ? "СГЛАЖИВАНИЕ НЕДОСТУПНО" : "СГЛАЖИВАНИЕ ВЫКЛЮЧЕНО");
      }));
  static const int kFps[] = {0, 30, 60, 120, 144};
  rows.push_back(choice(
      "ЛИМИТ КАДРОВ", {"НЕТ", "30", "60", "120", "144"},
      [] {
        for (int i = 0; i < 5; ++i)
          if (kFps[i] == g_cfg.fps_limit) return i;
        return 0;
      },
      [](int v) {
        g_cfg.fps_limit = kFps[v];
        display::set_fps_limit(kFps[v]);
        save("video", "fps_limit", std::to_string(kFps[v]));
      }));
  rows.push_back(choice(
      "ИГРА В ФОНЕ", kOffOn, [] { return g_cfg.background_run ? 1 : 0; },
      [](int v) {
        g_cfg.background_run = v == 1;
        display::set_background_run(g_cfg.background_run);
        save("video", "background", v ? "run" : "pause");
        restart_notice(g_cfg.background_run != g_start.background_run);
      }));
  return rows;
}

// --- ПАРАМЕТРЫ УПРАВЛЕНИЯ: the keyboard layout ---------------------------------------

namespace {

int layout_now() {
  if (binds::matches(binds::Preset::TwoOnOneKeyboard)) return 1;
  if (binds::matches(binds::Preset::Original)) return 0;
  return 2;  // СВОЯ
}

std::string key_list(const int keys[tmnp::kNumSlots]) {
  std::string s;
  char buf[8];
  for (int i = 0; i < tmnp::kNumSlots; ++i) {
    if (keys[i] > 0)
      snprintf(buf, sizeof(buf), "0x%02X", keys[i]);
    else
      snprintf(buf, sizeof(buf), "-1");
    s += (i ? "," : "") + std::string(buf);
  }
  return s;
}

// The player's own keys, before a preset replaces them: into tmntnet.ini.
void keep_custom() {
  binds::Snapshot now;
  if (!binds::snapshot(now)) return;
  for (int p = 0; p < 2; ++p)
    for (int i = 0; i < tmnp::kNumSlots; ++i) g_cfg.custom_key[p][i] = now.key[p][i];
  g_cfg.custom_keys = true;
  save("mod", "custom_keys_1", key_list(g_cfg.custom_key[0]));
  save("mod", "custom_keys_2", key_list(g_cfg.custom_key[1]));
}

}  // namespace

std::vector<native_menu::Row> controls_rows() {
  // Both players' keys at once: the game's own, two players at one keyboard
  // (WASD and the arrows), or СВОЯ, the keys set in the game's key table. СВОЯ
  // is kept when a preset replaces it (custom_keys_1/2), and comes back.
  std::vector<native_menu::Row> rows;
  rows.push_back(choice("РАСКЛАДКА", {"КАК В ИГРЕ", "ДВА ИГРОКА", "СВОЯ"}, &layout_now, [](int v) {
    int now = layout_now();
    if (v == now) return;
    if (v == 2 && !g_cfg.custom_keys) v = now == 0 ? 1 : 0;  // (nothing of one's own yet)
    if (now == 2) keep_custom();
    if (v == 2) {
      for (int p = 0; p < 2; ++p) binds::set_keys(p, g_cfg.custom_key[p]);
      LOGI("settings: keyboard layout of one's own");
      return;
    }
    binds::apply_preset(v == 1 ? binds::Preset::TwoOnOneKeyboard : binds::Preset::Original);
    LOGI("settings: keyboard layout %s", v == 1 ? "two players" : "the game's");
  }));
  return rows;
}

}  // namespace mod_pages
