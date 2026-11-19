#include "mod/settings_ui.h"

#include <windows.h>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "common/log.h"
#include "mod/ambient.h"
#include "mod/autosave.h"
#include "mod/audio.h"
#include "mod/binds.h"
#include "mod/csel.h"
#include "mod/dinput_hook.h"
#include "mod/display.h"
#include "mod/net_host.h"
#include "mod/ui.h"
#include "mod/widescreen.h"

namespace settings_ui {
namespace {

Config g_cfg;
std::wstring g_ini;
bool g_restart = false;      // something waits for the next start
bool g_save_failed = false;  // tmntnet.ini is not writable (Program Files?)

const ImVec4 kGreen(0.36f, 0.72f, 0.24f, 1.0f);
const ImVec4 kAmber(0.98f, 0.75f, 0.25f, 1.0f);
const ImVec4 kMuted(0.70f, 0.72f, 0.68f, 1.0f);

#define AUTOSAVE_NOTE                                                                                     \
  "Сама игра сохраняется только в конце главы и спрашивает «ВЫ ХОТИТЕ СОХРАНИТЬСЯ?» с курсором на «НЕТ». "  \
  "С галочкой мод записывает savegame.dat после каждого уровня (внизу появится «Игра сохранена»), а "     \
  "АВТОЗАПИСЬ игры (ПАРАМЕТРЫ → ПРОЧИЕ ПАРАМЕТРЫ) включена: в конце главы игра сохраняется без вопроса. " \
  "Прошлое сохранение остаётся рядом как savegame.dat.bak."

void save(const char* section, const char* key, const std::string& value) {
  if (ini_set(g_ini, section, key, value)) {
    LOGI("settings: [%s] %s=%s", section, key, value.c_str());
  } else {
    g_save_failed = true;
    LOGW("settings: cannot write [%s] %s to tmntnet.ini", section, key);
  }
}

void note(const char* text) {
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 36.0f);
  ImGui::TextColored(kMuted, "%s", text);
  ImGui::PopTextWrapPos();
}

// Labels to the left, the controls of a page lined up in one column.
float g_column = 0.0f;
void set_column(std::initializer_list<const char*> labels) {
  float w = 0.0f;
  for (const char* l : labels) w = std::max(w, ImGui::CalcTextSize(l).x);
  g_column = w + ImGui::GetStyle().ItemSpacing.x * 3.0f;
}
void label(const char* text) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(text);
  ImGui::SameLine(g_column);
}

// A row of radio buttons; true when the choice changed.
bool choice(const char* row, int* value, std::initializer_list<const char*> options) {
  label(row);
  bool changed = false;
  int i = 0;
  ImGui::PushID(row);
  for (const char* o : options) {
    if (i) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.5f);
    changed |= ImGui::RadioButton(o, value, i++);
  }
  ImGui::PopID();
  return changed;
}

std::string size_text(int w, int h) { return std::to_string(w) + "x" + std::to_string(h); }

void footer() {
  if (g_restart) {
    ImGui::Spacing();
    ImGui::TextColored(kAmber, "Часть изменений вступит в силу после перезапуска игры.");
  }
  if (g_save_failed)
    ImGui::TextColored(kAmber, "Не удалось сохранить tmntnet.ini: нет прав на запись в папку игры.");
}

// --- Screen --------------------------------------------------------------------

void display_page() {
  static int mode = -1, window = -1, res = -1, vsync = -2, fps = -1;
  static bool wide = true, resize = true, background = false;
  static int hud = 0, pics = 0, bars = 0;
  if (mode < 0) {  // the values the game started with
    mode = g_cfg.display == "borderless" ? 1 : g_cfg.display == "windowed" ? 2 : 0;
    wide = g_cfg.widescreen;
    resize = g_cfg.window_resize;
    background = g_cfg.background_run;
    hud = g_cfg.hud_center ? 0 : 1;
    pics = g_cfg.fullscreen_pillarbox ? 0 : 1;
    bars = g_cfg.side_bars_blur ? 0 : 1;
    vsync = g_cfg.vsync;
    fps = g_cfg.fps_limit;
  }
  set_column({"Режим", "Размер окна", "Разрешение", "Интерфейс в игре", "Меню, ролики, картинки",
              "Поля по бокам", "Вертикальная синхронизация", "Ограничение кадров"});
  const float combo = ImGui::GetFontSize() * 11.0f;

  ui::section("Окно");
  int before = mode;
  if (choice("Режим", &mode, {"Полный экран", "Без рамок", "В окне"}) && mode != before) {
    static const char* kNames[] = {"fullscreen", "borderless", "windowed"};
    save("video", "display", kNames[mode]);
    if (!display::switch_mode(mode)) g_restart = true;
  }
  if (mode == 2) {
    static const int kSizes[][2] = {{0, 0}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
    if (window < 0) {
      window = 0;
      for (int i = 1; i < 5; ++i)
        if (kSizes[i][0] == g_cfg.window_w && kSizes[i][1] == g_cfg.window_h) window = i;
    }
    label("Размер окна");
    std::string cur = window ? size_text(kSizes[window][0], kSizes[window][1]) : "по разрешению игры";
    ImGui::SetNextItemWidth(combo);
    if (ImGui::BeginCombo("##window", cur.c_str())) {
      for (int i = 0; i < 5; ++i) {
        std::string text = i ? size_text(kSizes[i][0], kSizes[i][1]) : "по разрешению игры";
        if (ImGui::Selectable(text.c_str(), window == i)) {
          window = i;
          display::set_window_size(kSizes[i][0], kSizes[i][1]);
          save("video", "window_size", i ? text : "");
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.5f);
    if (ImGui::Checkbox("тянуть мышью", &resize)) {
      display::set_window_resize(resize);
      save("video", "window_resize", resize ? "1" : "0");
    }
  }
  note("«Без рамок» — окно во весь экран: Alt+Tab без мигания. Alt+Enter в игре переключает «без рамок» "
       "и «в окне».");

  ui::section("Изображение");
  std::vector<std::pair<int, int>> modes = widescreen::video_modes();
  std::pair<int, int> now = widescreen::current_mode();
  if (res < 0) {
    res = 0;  // desktop
    if (g_cfg.res_mode == 2)
      for (size_t i = 0; i < modes.size(); ++i)
        if (modes[i].first == g_cfg.res_w && modes[i].second == g_cfg.res_h) res = (int)i + 1;
  }
  label("Разрешение");
  std::string cur = res ? size_text(modes[res - 1].first, modes[res - 1].second) : "как у рабочего стола";
  ImGui::SetNextItemWidth(combo);
  if (ImGui::BeginCombo("##resolution", cur.c_str())) {
    if (ImGui::Selectable("как у рабочего стола", res == 0)) {
      res = 0;
      save("video", "resolution", "desktop");
      g_restart = true;
    }
    for (size_t i = 0; i < modes.size(); ++i) {
      std::string text = size_text(modes[i].first, modes[i].second);
      if (ImGui::Selectable(text.c_str(), res == (int)i + 1)) {
        res = (int)i + 1;
        save("video", "resolution", text);
        g_restart = true;
      }
    }
    ImGui::EndCombo();
  }
  if (now.first) {
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.5f);
    ImGui::TextColored(kMuted, "сейчас %s", size_text(now.first, now.second).c_str());
  }
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + g_column);
  if (ImGui::Checkbox("Широкий экран: 16:9 без растяжения", &wide)) {
    save("video", "widescreen", wide ? "1" : "0");
    g_restart = true;
  }
  if (choice("Интерфейс в игре", &hud, {"по центру 4:3", "растянуть"})) {
    widescreen::set_hud_center(hud == 0);
    save("video", "hud", hud == 0 ? "center" : "stretch");
  }
  if (choice("Меню, ролики, картинки", &pics, {"4:3 без искажений", "на весь экран"})) {
    widescreen::set_pillarbox(pics == 0);
    save("video", "fullscreen_images", pics == 0 ? "pillarbox" : "stretch");
  }
  if (pics == 0 && choice("Поля по бокам", &bars, {"размытые", "чёрные"})) {
    ambient::set_blur(bars == 0);
    save("video", "side_bars", bars == 0 ? "blur" : "black");
  }

  ui::section("Кадры");
  static const char* kVsync[] = {"как в игре", "выключена", "включена"};
  int vs = vsync + 1;
  label("Вертикальная синхронизация");
  ImGui::SetNextItemWidth(combo);
  if (ImGui::Combo("##vsync", &vs, kVsync, 3)) {
    vsync = vs - 1;
    save("video", "vsync", vsync < 0 ? "game" : vsync ? "on" : "off");
    g_restart = true;
  }
  label("Ограничение кадров");
  ImGui::SetNextItemWidth(combo);
  ImGui::SliderInt("##fps", &fps, 0, 240, fps ? "%d в секунду" : "нет");
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    display::set_fps_limit(fps);
    save("video", "fps_limit", std::to_string(fps));
  }
  if (ImGui::Checkbox("Игра не встаёт на паузу, когда окно не активно", &background)) {
    display::set_background_run(background);
    save("video", "background", background ? "run" : "pause");
    g_restart = true;  // sound buffers pick it up at the next start
  }
  note("Для «без рамок» и «в окне». Удобно хосту сетевой игры: можно переключиться в другое окно, а "
       "второй игрок продолжит играть.");
  footer();
}

// --- Sound -----------------------------------------------------------------------

void sound_page() {
  static int volume = -1;
  static bool skip = false, follow = true;
  if (volume < 0) {
    volume = g_cfg.movie_volume;
    skip = g_cfg.intro_skip;
    follow = g_cfg.movie_follow;
  }
  set_column({"Ещё тише"});
  ui::section("Ролики");
  note("Игра не применяет к роликам свои настройки звука: заставка при запуске звучит на полную, громче "
       "музыки даже на максимуме.");
  if (ImGui::Checkbox("Громкость роликов — как у музыки в настройках игры", &follow)) {
    audio::set_movie_follow(follow);
    save("audio", "movie_follow", follow ? "1" : "0");
  }
  label("Ещё тише");
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
  ImGui::SliderInt("##movie", &volume, 0, 100, volume >= 100 ? "не убавлять" : "%d%%");
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    audio::set_movie_volume(volume);
    save("audio", "movie_volume", std::to_string(volume));
  }
  if (ImGui::Checkbox("Пропускать заставку при запуске: сразу в главное меню", &skip)) {
    save("audio", "intro", skip ? "skip" : "play");
    g_restart = true;
  }
  ui::section("Музыка, звуки, голоса");
  note("Их громкость — в настройках самой игры: главное меню, ПАРАМЕТРЫ.");
  footer();
}

// --- Game ------------------------------------------------------------------------

void game_page() {
  static bool loaded = false, autopick = true;
  static float delay_s = 1.5f;
  static int pick = 0;  // 0 random, 1 the first free
  if (!loaded) {
    autopick = g_cfg.p2_autopick;
    delay_s = g_cfg.p2_autopick_delay_ms / 1000.0f;
    pick = g_cfg.p2_character == -2 ? 1 : 0;
    loaded = true;
  }
  set_column({"Через", "Черепашка"});

  ui::section("Сохранение");
  bool on = autosave::enabled();
  if (ImGui::Checkbox("Сохранять игру после каждого пройденного уровня", &on)) {
    autosave::set_enabled(on);
    save("mod", "autosave", on ? "1" : "0");
  }
  note(AUTOSAVE_NOTE);

  ui::section("Второй игрок не подключён");
  bool changed = ImGui::Checkbox("Мод выбирает черепашку за второго игрока и отвечает «ДА»", &autopick);
  if (autopick) {
    label("Через");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11.0f);
    ImGui::SliderFloat("##delay", &delay_s, 0.5f, 10.0f, "%.1f с");
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    changed |= choice("Черепашка", &pick, {"случайная", "первая свободная"});
  }
  if (changed) {
    int ms = (int)(delay_s * 1000.0f + 0.5f);
    csel::set_autopick(autopick, ms, pick ? -2 : -1);
    save("mod", "p2_autopick", autopick ? "1" : "0");
    save("mod", "p2_autopick_delay_ms", std::to_string(ms));
    save("mod", "p2_character", pick ? "first" : "auto");
  }
  note("Story Mode на двоих без второго игрока по сети. Если второй игрок сидит рядом, он успевает выбрать "
       "сам: мод ничего не делает, пока его курсор кто-то двигает.");
  footer();
}

// --- Controls --------------------------------------------------------------------

struct Row {
  int slot;
  const char* name;
};
const Row kRows[] = {{12, "Вверх"},           {14, "Вниз"},          {15, "Влево"},
                     {13, "Вправо"},          {6, "Атака / выбрать"}, {7, "Сильная атака / отмена"},
                     {5, "Прыжок"},           {4, "Сюрикен"},        {3, "Смена сюрикена"},
                     {0, "Рывок"},            {1, "Удар в воздухе"}, {2, "Спецприём"},
                     {11, "Старт"},           {8, "Пауза"}};

const char* slot_name(int slot) {
  for (const Row& r : kRows)
    if (r.slot == slot) return r.name;
  return "—";
}

// --- mouse buttons ([mouse])
const char* kMouseButtons[5] = {"Левая кнопка", "Правая кнопка", "Средняя кнопка", "Боковая «назад»",
                                "Боковая «вперёд»"};
const char* kMouseKeys[5] = {"left", "right", "middle", "x1", "x2"};
const int kMouseActions[] = {-1, 6, 7, 5, 4, 3, 0, 1, 2, 11, 8};  // "nothing" first
int g_mouse_player = -1;
int g_mouse_slot[5];

void mouse_section() {
  if (g_mouse_player < 0) {
    g_mouse_player = g_cfg.mouse_on ? g_cfg.mouse_player[0] : 0;
    for (int i = 0; i < 5; ++i) g_mouse_slot[i] = g_cfg.mouse_slot[i];
  }
  ui::section("Мышь");
  set_column({"Кнопки мыши", kMouseButtons[3], kMouseButtons[4]});
  const float combo = ImGui::GetFontSize() * 11.0f;
  static const char* kWho[] = {"выключены", "у игрока 1", "у игрока 2"};
  label("Кнопки мыши");
  ImGui::SetNextItemWidth(combo);
  bool changed = false;
  if (ImGui::Combo("##mouse_player", &g_mouse_player, kWho, 3)) {
    save("mouse", "player", std::to_string(g_mouse_player));
    changed = true;
  }
  if (g_mouse_player > 0) {
    for (int i = 0; i < 5; ++i) {
      label(kMouseButtons[i]);
      ImGui::SetNextItemWidth(combo);
      ImGui::PushID(i);
      if (ImGui::BeginCombo("##mouse_button", g_mouse_slot[i] < 0 ? "—" : slot_name(g_mouse_slot[i]))) {
        for (int a : kMouseActions)
          if (ImGui::Selectable(a < 0 ? "—" : slot_name(a), g_mouse_slot[i] == a)) {
            g_mouse_slot[i] = a;
            save("mouse", kMouseKeys[i], mouse_button_value(g_mouse_player, a));
            changed = true;
          }
        ImGui::EndCombo();
      }
      ImGui::PopID();
    }
    note("Кнопки мыши нажимают клавиши этого игрока, пока окно игры активно и указатель над ним. Щелчок, "
         "которым вы переключились в игру, не считается.");
  }
  if (changed) {
    int players[5];
    for (int& p : players) p = g_mouse_player == 2 ? 2 : 1;
    set_mouse_binds(g_mouse_player != 0, players, g_mouse_slot);
  }
}

// "мышь: левая — атака, правая — сильная атака"
std::string mouse_summary(int p) {
  if (g_mouse_player != p + 1) return "";
  static const char* kShort[5] = {"левая", "правая", "средняя", "«назад»", "«вперёд»"};
  static const struct {
    int slot;
    const char* name;
  } kActions[] = {{6, "атака"},          {7, "сильная атака"}, {5, "прыжок"},    {4, "сюрикен"},
                  {3, "смена сюрикена"}, {0, "рывок"},         {1, "удар в воздухе"}, {2, "спецприём"},
                  {11, "старт"},         {8, "пауза"}};
  std::string s;
  for (int i = 0; i < 5; ++i)
    for (const auto& a : kActions)
      if (a.slot == g_mouse_slot[i]) s += (s.empty() ? ";  мышь: " : ", ") + std::string(kShort[i]) + " — " + a.name;
  return s;
}

int g_capture_player = -1, g_capture_slot = -1;
int g_page_frame = -10;    // the controls page was last drawn in this frame
int g_capture_end = -100;  // and a key capture ended in this one

// "ходьба ↑ ↓ ← →,  атака F,  прыжок Пробел,  старт Enter"
std::string main_keys(const binds::Snapshot& s, int p) {
  auto k = [&](int slot) { return binds::key_name(s.key[p][slot]); };
  return "ходьба " + k(12) + " " + k(14) + " " + k(15) + " " + k(13) + ",  атака " + k(6) + ",  прыжок " + k(5) +
         ",  старт " + k(11);
}

void player_line(int p, const std::string& text) {
  ImGui::PushFont(ui::bold_font(), 0.0f);
  ImGui::TextColored(p ? kAmber : kGreen, "Игрок %d", p + 1);
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::TextWrapped("%s", text.c_str());
}

bool preset_button(const char* text, binds::Preset p) {
  bool on = binds::matches(p);
  if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  bool pressed = ImGui::Button(on ? (std::string(text) + "  (сейчас)").c_str() : text);
  if (on) ImGui::PopStyleColor();
  if (pressed) binds::apply_preset(p);
  return pressed;
}

void controls_page() {
  binds::Snapshot s{};
  bool have = binds::snapshot(s);
  HostStatus st = host_status();
  static bool lock = g_cfg.p2_lock;
  int frame = ImGui::GetFrameCount();
  if (g_capture_player >= 0 && frame - g_page_frame > 1) {  // the page was left mid-capture
    cancel_key_capture();
    g_capture_player = -1;
  }
  g_page_frame = frame;

  // A key being reassigned? Its Esc cancels the capture, not the window.
  if (have && g_capture_player >= 0) {
    ui::claim_escape();
    int code = take_captured_key();
    if (code == 0x01) {
      g_capture_player = -1;
      g_capture_end = frame;
    } else if (code > 0) {
      int keys[tmnp::kNumSlots];
      std::copy(s.key[g_capture_player], s.key[g_capture_player] + tmnp::kNumSlots, keys);
      keys[g_capture_slot] = code;
      binds::set_keys(g_capture_player, keys);
      binds::snapshot(s);
      g_capture_player = -1;
      g_capture_end = frame;
    }
  }

  if (g_mouse_player < 0) {
    g_mouse_player = g_cfg.mouse_on ? g_cfg.mouse_player[0] : 0;
    for (int i = 0; i < 5; ++i) g_mouse_slot[i] = g_cfg.mouse_slot[i];
  }
  ui::section("Кто чем управляет");
  bool remote = g_cfg.is_host() && st.p2 && lock;
  for (int p = 0; p < 2; ++p) {
    std::string text;
    if (p == 1 && remote) {
      text = "играет по сети: " + st.p2_name + ". Его клавиши и геймпад на этом ПК отключены.";
    } else {
      text = have ? "клавиатура: " + main_keys(s, p) : "клавиатура";
      int idx = s.pad_index[p];
      if (idx >= 0 && idx < (int)s.pads.size()) text += ";  геймпад: " + s.pads[idx];
      text += mouse_summary(p);
      if (p == 1 && st.p2) text += "  (и по сети: " + st.p2_name + ")";
    }
    player_line(p, text);
  }
  if (s.pads.empty()) note("Геймпадов нет. Игра видит только те, что подключены до её запуска.");
  if (g_cfg.is_host() &&
      ImGui::Checkbox("Пока второй игрок подключён по сети, клавиши и геймпад игрока 2 на этом ПК не работают",
                      &lock)) {
    set_p2_lock(lock);
    save("mod", "p2_lock", lock ? "1" : "0");
  }

  if (!have) {
    ImGui::Spacing();
    ImGui::TextColored(kAmber, "Таблица клавиш игры не найдена: в этой версии игры раскладку не показать.");
    footer();
    return;
  }

  if (binds::ready()) {
    ui::section("Готовые раскладки");
    preset_button("Двое за одной клавиатурой", binds::Preset::TwoOnOneKeyboard);
    ImGui::SameLine();
    preset_button("Как в оригинале", binds::Preset::Original);
    note("«Двое за одной клавиатурой»: игрок 1 слева — WASD, атака J, сильная K, прыжок Пробел, старт Enter; "
         "игрок 2 справа — стрелки и цифровой блок: атака Num 4, сильная Num 5, прыжок Num 0, старт Num Enter.");
  }

  mouse_section();

  ui::section("Клавиши");
  if (ImGui::BeginTable("keys", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("Действие");
    ImGui::TableSetupColumn("Игрок 1");
    ImGui::TableSetupColumn("Игрок 2");
    ImGui::TableSetupColumn("Геймпад");
    ImGui::TableHeadersRow();
    for (const Row& r : kRows) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(r.name);
      for (int p = 0; p < 2; ++p) {
        ImGui::TableNextColumn();
        int code = s.key[p][r.slot];
        bool clash = false;  // the same key does two things
        for (int q = 0; q < 2; ++q)
          for (int i = 0; i < tmnp::kNumSlots; ++i)
            if ((q != p || i != r.slot) && code > 0 && s.key[q][i] == code && !(r.slot == 8 && i == 8))
              clash = true;
        bool waiting = g_capture_player == p && g_capture_slot == r.slot;
        std::string text = waiting ? "нажмите клавишу…" : binds::key_name(code);
        text += "##k" + std::to_string(p) + "_" + std::to_string(r.slot);
        if (clash) ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        if (waiting) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        // (an Enter just captured must not press the button again)
        if (ImGui::Button(text.c_str(), ImVec2(ImGui::GetFontSize() * 7.5f, 0)) && binds::ready() &&
            frame - g_capture_end > 10) {
          g_capture_player = p;
          g_capture_slot = r.slot;
          begin_key_capture();
        }
        if (waiting) ImGui::PopStyleColor();
        if (clash) ImGui::PopStyleColor();
      }
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      if (r.slot >= 12) {
        ImGui::TextColored(kMuted, "стик / крестовина");
      } else {
        int a = s.pad[0][r.slot], b = s.pad[1][r.slot];
        if (a < 0 && b < 0)
          ImGui::TextColored(kMuted, "—");
        else if (a == b || b < 0)
          ImGui::TextColored(kMuted, "кнопка %d", a + 1);
        else
          ImGui::TextColored(kMuted, "кнопка %d / %d", a + 1, b + 1);
      }
    }
    ImGui::EndTable();
  }
  note("Нажмите на клавишу в таблице, затем новую клавишу (Esc — отмена). Жёлтым — клавиша, занятая дважды. "
       "Раскладка сохраняется в настройки самой игры: её меню клавиш показывает то же.");
  if (g_cfg.is_host())
    note("Второй игрок по сети управляет со своего ПК: его кнопки настраиваются в окне tmntnet-client.");
  footer();
}

}  // namespace

void install(const Config& cfg) {
  g_cfg = cfg;
  g_ini = module_dir(nullptr) + L"tmntnet.ini";
  ui::add_tab("Экран", &display_page);
  ui::add_tab("Звук", &sound_page);
  ui::add_tab("Управление", &controls_page);
  ui::add_tab("Игра", &game_page);
}

}  // namespace settings_ui
