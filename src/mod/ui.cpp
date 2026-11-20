#include "mod/ui.h"

#include <windows.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <vector>

#include "common/joincode.h"
#include "common/log.h"
#include "common/net.h"
#include "common/protocol.h"
#include "common/version.h"
#include "mod/lockstep.h"
#include "mod/net_host.h"
#include "mod/main_menu.h"
#include "mod/native_menu.h"

namespace ui {
namespace {

Config g_cfg;
bool g_open = false;
bool g_focus_pending = false;
ImFont* g_bold = nullptr;
ImFont* g_mono = nullptr;
ImFont* g_menu = nullptr;  // the main menu item: bold italic like the game's own
bool g_scaled = false;

bool g_force_main_menu = false;
bool in_main_menu() { return g_force_main_menu || main_menu::on_screen(); }

// --- notifications -----------------------------------------------------------

struct Toast {
  std::string text;
  uint32_t until;
};
SRWLOCK g_toast_lock = SRWLOCK_INIT;
std::vector<Toast> g_toasts;

// --- palette -----------------------------------------------------------------

const ImVec4 kGreen(0.36f, 0.72f, 0.24f, 1.0f);
const ImVec4 kAmber(0.98f, 0.75f, 0.25f, 1.0f);
const ImVec4 kMuted(0.70f, 0.72f, 0.68f, 1.0f);

void dot(const ImVec4& color) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  float r = ImGui::GetFontSize() * 0.28f;
  ImGui::GetWindowDrawList()->AddCircleFilled(
      ImVec2(p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f), r, ImGui::GetColorU32(color));
  ImGui::Dummy(ImVec2(r * 2.4f, ImGui::GetTextLineHeight()));
  ImGui::SameLine();
}

// --- tmntnet's window: the network page and the settings pages -----------------

HostStatus g_st;  // this frame's, for the pages

void draw_network_page() {
  const HostStatus& st = g_st;
  if (!g_cfg.is_host()) {
    ImGui::TextWrapped(
        "На этом ПК tmntnet настроен как клиент (role=client в tmntnet.ini). Чтобы принимать "
        "второго игрока, установите мод на этот ПК как хост (tmntnet-setup.exe).");
  } else if (!st.started) {
    ImGui::TextColored(kMuted, "Запускаем сеть...");
  } else if (!st.running) {
    ImGui::TextColored(kAmber, "Сеть не запущена: порт %u занят или недоступен.", g_cfg.port);
    if (!st.steam_note.empty()) ImGui::TextColored(kMuted, "Steam: %s.", st.steam_note.c_str());
    ImGui::TextColored(kMuted, "Подробности в tmntnet.log.");
  } else {
    section("Второй игрок");
    if (st.p2) {
      dot(st.p2_input_live ? kGreen : kAmber);
      ImGui::Text("%s подключён", st.p2_name.c_str());
      ImGui::TextColored(kMuted, "%s %s   пинг %u мс   %s", st.p2_via == "Steam" ? "через" : "адрес",
                         st.p2_addr.c_str(), st.rtt_ms, st.p2_input_live ? "ввод идёт" : "нет ввода");
      if (ImGui::Button("Отключить игрока")) host_kick(-1);
    } else {
      int dots = (int)(net::now_ms() / 400 % 4);
      dot(kMuted);
      ImGui::Text("Ждём второго игрока%.*s", dots, "...");
    }

    section("Как подключиться другу");
    if (st.steam) {
      ImGui::TextWrapped(
          "Через интернет: друг запускает tmntnet-client при включённом Steam, эта игра будет у "
          "него в списке Steam. %s",
          g_cfg.lobby_type == "public" ? "Лобби видно всем." : "Лобби видно только вашим друзьям в Steam.");
      ImGui::TextColored(kMuted, "Вы в Steam: %s   SteamID64 %llu", st.steam_name.c_str(),
                         (unsigned long long)st.steam_id);
      ImGui::SameLine();
      if (ImGui::SmallButton("Копировать SteamID")) {
        std::string id = std::to_string(st.steam_id);
        ImGui::SetClipboardText(id.c_str());
        toast("SteamID64 скопирован: " + id);
      }
      if (!st.lobby_id) {
        ImGui::TextColored(kAmber, "Создаём лобби Steam...");
      } else {
        ImGui::TextColored(kMuted, "Лобби %llu%s", (unsigned long long)st.lobby_id,
                           st.steam_mock ? "   (тестовый мок Steam)" : "");
        ImGui::SameLine();
        if (ImGui::SmallButton("Копировать ID лобби")) {
          std::string id = std::to_string(st.lobby_id);
          ImGui::SetClipboardText(id.c_str());
          toast("ID лобби скопирован: " + id);
        }
        // Steam's own invite dialog needs the overlay, which tmntnet keeps off.
        if (!st.friends.empty()) {
          ImGui::TextColored(kMuted, "Пригласить друга (придёт обычное приглашение Steam):");
          size_t shown = std::min<size_t>(st.friends.size(), 6);
          for (size_t i = 0; i < shown; ++i) {
            const lobby::Friend& f = st.friends[i];
            ImGui::PushID((int)i);
            if (ImGui::SmallButton("Пригласить")) {
              host_invite(f.id);
              toast("Приглашение отправлено: " + f.name);
            }
            ImGui::SameLine();
            if (f.playing_480)
              ImGui::TextColored(kGreen, "%s   (запущен tmntnet / Spacewar)", f.name.c_str());
            else
              ImGui::TextUnformatted(f.name.c_str());
            ImGui::PopID();
          }
        }
      }
      ImGui::Spacing();
    } else if (!st.steam_note.empty()) {
      ImGui::TextColored(kMuted, "Steam выключен: %s.", st.steam_note.c_str());
    }
  }
  if (g_cfg.is_host() && st.running && st.enet) {
    ImGui::TextWrapped(
        "В одной сети (или через VPN) игра будет в списке клиента сама. Иначе введите код:");
    ImGui::Spacing();
    static std::vector<net::LocalAddr> addrs;
    static uint32_t addrs_at = 0;
    if (addrs.empty() || net::now_ms() - addrs_at > 5000) {
      addrs = net::local_ipv4();
      addrs_at = net::now_ms();
    }
    if (ImGui::BeginTable("addrs", 3, ImGuiTableFlags_SizingFixedFit)) {
      for (size_t i = 0; i < addrs.size(); ++i) {
        const auto& a = addrs[i];
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &a.ip, ip, sizeof(ip));
        std::string code = joincode::encode(a.ip, st.port, tmnp::kDefaultPort);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushFont(g_mono, ImGui::GetStyle().FontSizeBase * 1.15f);
        ImGui::TextColored(kAmber, "%s", code.c_str());
        ImGui::PopFont();
        ImGui::TableNextColumn();
        ImGui::TextColored(kMuted, "%s  %s", ip, a.adapter.c_str());
        ImGui::TableNextColumn();
        ImGui::PushID((int)i);
        if (ImGui::SmallButton("Копировать")) {
          ImGui::SetClipboardText(code.c_str());
          toast("Код скопирован: " + code);
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (addrs.empty()) ImGui::TextColored(kAmber, "Нет активных сетевых адаптеров.");
  }
  if (g_cfg.is_host() && st.running) {
    section("Дальше");
    ImGui::TextWrapped(
        "Закройте это окно и выберите Story Mode на двоих. P1 выбирает свою черепашку, затем "
        "второй игрок выбирает свою со своего ПК. Если он не подключён, мод выберет за него.");
  }
}

struct Tab {
  const char* name;
  void (*draw)();
};
std::vector<Tab> g_tabs{{"Сетевая игра", &draw_network_page}};
int g_tab = 0;             // the page on screen
int g_select = -1;         // bring this page up on the next frame
int g_escape_frame = -10;  // a page used Esc itself in this frame

void draw_window(float W, float H) {
  const ImGuiStyle& style = ImGui::GetStyle();
  ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(W * 0.92f, H * 1.22f), H * 0.88f), ImGuiCond_Always);
  if (g_focus_pending) ImGui::SetNextWindowFocus();
  g_focus_pending = false;
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
  bool open = true;
  ImGui::PushFont(g_bold, 0.0f);
  bool visible = ImGui::Begin("Сетевая игра и настройки###tmntnet", &open, flags);
  ImGui::PopFont();
  if (visible) {
    // The buttons stay at the bottom; a long page scrolls above them.
    float footer = ImGui::GetFrameHeight() + style.ItemSpacing.y * 4.0f + 2.0f;
    if (ImGui::BeginTabBar("pages")) {
      for (int i = 0; i < (int)g_tabs.size(); ++i) {
        ImGuiTabItemFlags f = g_select == i ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem(g_tabs[i].name, nullptr, f)) {
          g_tab = i;
          if (ImGui::BeginChild("page", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened))
            g_tabs[i].draw();
          ImGui::EndChild();
          ImGui::EndTabItem();
        }
      }
      g_select = -1;
      ImGui::EndTabBar();
    }
    // Gamepad: shoulder buttons go through the pages.
    int n = (int)g_tabs.size();
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) g_select = (g_tab + n - 1) % n;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) g_select = (g_tab + 1) % n;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Закрыть")) open = false;
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "Esc или F2   ·   F12 скриншот   ·   tmntnet " TMNTNET_VERSION);
    bool escape_free = ImGui::GetFrameCount() - g_escape_frame > 2;
    if ((escape_free && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
      open = false;
  }
  ImGui::End();
  if (!open && g_open) toggle();
}

// --- small always-on elements ----------------------------------------------------

ImGuiWindowFlags overlay_flags() {
  return ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
}

void draw_main_menu_entry(const HostStatus& st, float W, float H) {
  ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.955f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
  ImGui::SetNextWindowBgAlpha(0.80f);
  if (ImGui::Begin("##netentry", nullptr, overlay_flags())) {
    ImGui::PushFont(g_bold, ImGui::GetStyle().FontSizeBase * 1.1f);
    if (ImGui::Button("СЕТЕВАЯ ИГРА  [F2]")) toggle();
    ImGui::PopFont();
    ImGui::SameLine();
    if (!g_cfg.is_host())
      ImGui::TextColored(kMuted, "клиент");
    else if (st.p2)
      ImGui::TextColored(kGreen, "игрок 2: %s", st.p2_name.c_str());
    else
      ImGui::TextColored(kMuted, "ждём игрока 2");
  }
  ImGui::End();
}

// The label of the native NETWORK item, drawn where the game would have drawn
// the item's text sprite (menu space: 640x480, origin at the centre, y up).
void draw_native_label(float W, float H) {
  main_menu::Label l = main_menu::label();
  if (!l.visible) return;
  // The main menu lays out in a 512 x 480 space (origin at the centre, y up),
  // i.e. W/512 per unit across and H/480 down (measured from the game's own
  // Im2D calls, tmntnet 0.6). With the HUD squeezed into 4:3 the width becomes
  // that of the 4:3 area: H*4/3/512 = H/384.
  bool centred = g_cfg.widescreen && g_cfg.hud_center && W / H > 4.0f / 3.0f + 0.01f;
  float sy = H / 480.0f, sx = centred ? H / 384.0f : W / 512.0f;
  float size = std::round(21.0f * sy);  // cap height like the game's item textures
  ImFont* font = g_menu;
  const char* text = g_cfg.menu_label.c_str();
  ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  // The game's item textures start their text a few units in from the left.
  ImVec2 pos(std::round(W * 0.5f + (l.x + 3.0f) * sx), std::round(H * 0.5f - l.y * sy - ts.y * 0.5f));
  int a = (int)(l.alpha * 255.0f);
  ImU32 outline = IM_COL32(16, 22, 16, a);
  // The game marks the selected item with its cursor only, not a colour.
  ImU32 color = l.selected ? IM_COL32(255, 255, 255, a) : IM_COL32(238, 238, 232, a);
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  float o = std::max(1.0f, std::round(2.5f * sy / 2.25f));
  for (int dx = -1; dx <= 1; ++dx)
    for (int dy = -1; dy <= 1; ++dy)
      if (dx || dy) dl->AddText(font, size, ImVec2(pos.x + dx * o, pos.y + dy * o), outline, text);
  dl->AddText(font, size, pos, color, text);
}

// A line under the main menu saying where the settings are, fading with it.
void draw_menu_hint(float W, float H) {
  main_menu::Label l = main_menu::label();
  if (!l.visible) return;
  const char* key = "F2";
  const char* text = "  сетевая игра и настройки: экран, звук, управление";
  float size = std::round(std::max(14.0f, H / 40.0f));
  ImFont* font = g_bold;
  ImVec2 ks = font->CalcTextSizeA(size, FLT_MAX, 0.0f, key);
  ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  float pad = std::round(size * 0.45f);
  ImVec2 pos(std::round((W - ks.x - ts.x) * 0.5f), std::round(H * 0.962f - ts.y));
  int a = (int)(l.alpha * 255.0f);
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  dl->AddRectFilled(ImVec2(pos.x - pad, pos.y - pad * 0.5f), ImVec2(pos.x + ks.x + ts.x + pad, pos.y + ts.y + pad * 0.5f),
                    IM_COL32(6, 12, 6, a * 6 / 10), size * 0.5f);
  dl->AddText(font, size, pos, IM_COL32(250, 191, 64, a), key);
  dl->AddText(font, size, ImVec2(pos.x + ks.x, pos.y), IM_COL32(236, 240, 230, a), text);
}

void draw_badge(const HostStatus& st, float W) {
  ImGui::SetNextWindowPos(ImVec2(W - 12.0f, 12.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.45f);
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.75f);
  if (ImGui::Begin("##p2badge", nullptr, overlay_flags() | ImGuiWindowFlags_NoInputs)) {
    dot(st.p2_input_live ? kGreen : kAmber);
    ImGui::Text("P2 %s · %u мс", st.p2_name.c_str(), st.rtt_ms);
  }
  ImGui::End();
  ImGui::PopFont();
}

void draw_toasts(float W) {
  uint32_t now = net::now_ms();
  std::vector<Toast> live;
  AcquireSRWLockExclusive(&g_toast_lock);
  g_toasts.erase(std::remove_if(g_toasts.begin(), g_toasts.end(),
                                [now](const Toast& t) { return (int32_t)(t.until - now) <= 0; }),
                 g_toasts.end());
  live = g_toasts;
  ReleaseSRWLockExclusive(&g_toast_lock);
  if (live.empty()) return;
  ImGui::SetNextWindowPos(ImVec2(W * 0.5f, 24.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.85f);
  if (ImGui::Begin("##toasts", nullptr, overlay_flags() | ImGuiWindowFlags_NoInputs)) {
    for (const auto& t : live) ImGui::TextUnformatted(t.text.c_str());
  }
  ImGui::End();
}

void watch_connections(const HostStatus& st) {
  static bool was_away = false;
  static int had_steam = -1;  // (-1 until the network has started)
  if (st.started) {
    if (had_steam == 0 && st.steam) native_menu::notify("СТИМ ПОДКЛЮЧЕН", st.steam_name);
    had_steam = st.steam ? 1 : 0;
  }
  static std::vector<HostStatus::Guest> had;
  static std::string last_host, last_error;
  // Players 2..4 in our game: who came, who left (the number in the plain
  // font: the game's letters have no 3 and 4).
  auto in = [](const std::vector<HostStatus::Guest>& list, const HostStatus::Guest& g) {
    for (const HostStatus::Guest& o : list)
      if (o.player == g.player && o.name == g.name) return true;
    return false;
  };
  auto same = [](const HostStatus::Guest& a, const HostStatus::Guest& b) { return a.player == b.player && a.name == b.name; };
  // (a page of ours has the players and their states on it: no notices over it)
  const bool quiet = native_menu::covers_screen();
  for (const HostStatus::Guest& g : st.guests) {
    if (!in(had, g) && !quiet) native_menu::notify("ИГРОК ПОДКЛЮЧИЛСЯ", std::to_string(g.player) + "  " + g.name);
    for (const HostStatus::Guest& o : had)
      if (same(o, g) && g.ready && !o.ready && !quiet)
        native_menu::notify("ИГРОК ГОТОВ", std::to_string(g.player) + "  " + g.name);
  }
  for (const HostStatus::Guest& g : had)
    if (!in(st.guests, g) && !quiet) native_menu::notify("ИГРОК ВЫШЕЛ", std::to_string(g.player) + "  " + g.name);
  had = st.guests;
  // This game in a friend's (the game on every PC).
  bool away = st.away == 2 && st.away_up;
  if (away && !was_away) native_menu::notify("ВЫ ПОДКЛЮЧИЛИСЬ", native_menu::latin(st.away_host));
  if (!away && was_away) native_menu::notify("ИГРА ДРУГА ЗАКОНЧЕНА", native_menu::latin(last_host));
  was_away = away;
  if (away) last_host = st.away_host;
  if (st.away_error != last_error) {
    if (!st.away_error.empty()) native_menu::notify("НЕ УДАЛОСЬ ПОДКЛЮЧИТЬСЯ");
    last_error = st.away_error;
  }
}

// A game together that is set and has not begun: what it waits for, in a
// line at the top (lockstep::waiting). At the main menu all the time; away
// from it (playing, another screen) at first and then now and then.
void show_waiting(const HostStatus& st) {
  using lockstep::Where;
  lockstep::Waiting w = lockstep::waiting();
  std::string letters, who;
  if (w.pending) {
    if (st.away) {
      who = st.away_host;
    } else {
      for (const HostStatus::Guest& g : st.guests)
        if (g.player == w.there_player) who = g.name;
      if (who.empty()) who = st.p2_name;
    }
    if (w.here == Where::Elsewhere) {
      uint32_t t = GetTickCount() - w.since_ms;
      if (t < 8000 || t % 30000 < 5000) letters = w.there == Where::Ready ? "ДРУГ ЖДЁТ В ГЛАВНОМ МЕНЮ" : "ВЫЙДИТЕ В ГЛАВНОЕ МЕНЮ";
      if (w.there != Where::Ready) who.clear();
    } else if (w.here == Where::NotReady) {
      letters = "НАЖМИТЕ ГОТОВ В СЕТЕВОЙ ИГРЕ";
      who.clear();
    } else if (w.here == Where::Ready || w.here == Where::Progress) {
      // (paused for a moment while it starts or loads: said only when it lasts)
      static uint32_t paused_since = 0;
      uint32_t now = GetTickCount();
      if (w.there != Where::Paused) paused_since = 0;
      else if (!paused_since) paused_since = now;
      letters = paused_since && now - paused_since >= 1500 ? "ИГРА ДРУГА НА ПАУЗЕ"
                : w.there == Where::Elsewhere             ? "ДРУГ НЕ В ГЛАВНОМ МЕНЮ"
                : w.there == Where::NotReady              ? "ЖДЁМ ГОТОВНОСТИ"
                                                          : "ЖДЁМ ДРУГА";
      if (w.there == Where::NotReady && w.there_player > 0) who = std::to_string(w.there_player) + "  " + who;
    }
  }
  // The host's НАЧАТЬ ИГРУ at work: its game goes into the story by itself.
  if (!w.pending && lockstep::starting_story()) letters = "НАЧИНАЕМ ИГРУ", who.clear();
  if (native_menu::covers_screen()) letters.clear();  // (a page of ours says it itself, at its bottom)
  native_menu::set_status(letters, letters.empty() ? std::string() : who);
}

}  // namespace

void install(const Config& cfg) { g_cfg = cfg; }

void section(const char* title) {
  ImGui::Spacing();
  ImGui::PushFont(g_bold, 0.0f);
  ImGui::TextColored(kGreen, "%s", title);
  ImGui::PopFont();
  ImGui::Separator();
}

ImFont* bold_font() { return g_bold; }

void setup_style() {
  ImGuiIO& io = ImGui::GetIO();
  char windir[MAX_PATH];
  GetWindowsDirectoryA(windir, MAX_PATH);
  std::string fonts = std::string(windir) + "\\Fonts\\";
  // ImGui >= 1.92 rasterizes glyphs on demand: Cyrillic needs no glyph ranges.
  ImFont* regular = io.Fonts->AddFontFromFileTTF((fonts + "segoeui.ttf").c_str(), 20.0f);
  if (!regular) regular = io.Fonts->AddFontFromFileTTF((fonts + "arial.ttf").c_str(), 20.0f);
  if (!regular) {
    LOGW("overlay: no Segoe UI/Arial font, Cyrillic text will not render");
    regular = io.Fonts->AddFontDefault();
  }
  g_bold = io.Fonts->AddFontFromFileTTF((fonts + "segoeuib.ttf").c_str(), 20.0f);
  g_mono = io.Fonts->AddFontFromFileTTF((fonts + "consolab.ttf").c_str(), 20.0f);
  for (const char* f : {"seguibli.ttf", "segoeuiz.ttf"})  // Segoe UI Black Italic / Bold Italic
    if (!g_menu && GetFileAttributesA((fonts + f).c_str()) != INVALID_FILE_ATTRIBUTES)
      g_menu = io.Fonts->AddFontFromFileTTF((fonts + f).c_str(), 40.0f);
  if (!g_bold) g_bold = regular;
  if (!g_mono) g_mono = regular;
  if (!g_menu) g_menu = g_bold;

  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 10.0f;
  s.FrameRounding = 6.0f;
  s.GrabRounding = 6.0f;
  s.WindowPadding = ImVec2(18, 14);
  s.FramePadding = ImVec2(14, 6);
  s.ItemSpacing = ImVec2(12, 8);
  s.WindowBorderSize = 1.0f;
  s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
  s.TabRounding = 6.0f;
  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = ImVec4(0.95f, 0.96f, 0.92f, 1.0f);
  c[ImGuiCol_TextDisabled] = kMuted;
  c[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.08f, 0.05f, 0.97f);
  c[ImGuiCol_PopupBg] = ImVec4(0.06f, 0.10f, 0.06f, 0.98f);
  c[ImGuiCol_Border] = ImVec4(0.36f, 0.72f, 0.24f, 0.55f);
  c[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.30f, 0.09f, 1.0f);
  c[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.40f, 0.11f, 1.0f);
  c[ImGuiCol_Button] = ImVec4(0.20f, 0.45f, 0.14f, 1.0f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.60f, 0.20f, 1.0f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.15f, 0.34f, 0.10f, 1.0f);
  c[ImGuiCol_Separator] = ImVec4(0.36f, 0.72f, 0.24f, 0.35f);
  c[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.19f, 0.10f, 1.0f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.29f, 0.14f, 1.0f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.36f, 0.17f, 1.0f);
  c[ImGuiCol_CheckMark] = ImVec4(0.62f, 0.95f, 0.38f, 1.0f);
  c[ImGuiCol_SliderGrab] = ImVec4(0.45f, 0.80f, 0.30f, 1.0f);
  c[ImGuiCol_SliderGrabActive] = ImVec4(0.62f, 0.95f, 0.38f, 1.0f);
  c[ImGuiCol_Header] = ImVec4(0.20f, 0.45f, 0.14f, 0.75f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.60f, 0.20f, 0.85f);
  c[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.60f, 0.20f, 1.0f);
  c[ImGuiCol_Tab] = ImVec4(0.11f, 0.24f, 0.08f, 1.0f);
  c[ImGuiCol_TabHovered] = ImVec4(0.30f, 0.60f, 0.20f, 1.0f);
  c[ImGuiCol_TabSelected] = ImVec4(0.22f, 0.50f, 0.15f, 1.0f);
  c[ImGuiCol_TabSelectedOverline] = kAmber;
  c[ImGuiCol_TabDimmed] = c[ImGuiCol_Tab];
  c[ImGuiCol_TabDimmedSelected] = c[ImGuiCol_TabSelected];
  c[ImGuiCol_TabDimmedSelectedOverline] = kAmber;
  c[ImGuiCol_TableHeaderBg] = ImVec4(0.11f, 0.24f, 0.08f, 1.0f);
  c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.035f);
  c[ImGuiCol_ScrollbarGrab] = ImVec4(0.25f, 0.45f, 0.18f, 1.0f);
  c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.32f, 0.58f, 0.22f, 1.0f);
  c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.38f, 0.68f, 0.26f, 1.0f);
  c[ImGuiCol_NavCursor] = kAmber;
  c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
}

void frame() {
  ImGuiIO& io = ImGui::GetIO();
  float W = io.DisplaySize.x, H = io.DisplaySize.y;
  if (W <= 0 || H <= 0) return;
  if (!g_scaled) {  // spacing follows the resolution (fonts are sized per frame)
    ImGui::GetStyle().ScaleAllSizes(std::max(1.0f, H / 900.0f));
    g_scaled = true;
  }
  ImGui::PushFont(nullptr, std::clamp(std::round(H / 34.0f), 16.0f, 64.0f));

  g_st = host_status();
  const HostStatus& st = g_st;
  watch_connections(st);
  show_waiting(st);
  if (main_menu::native()) draw_native_label(W, H);
  if (g_open)
    draw_window(W, H);
  else if (in_main_menu() && !main_menu::native())
    draw_main_menu_entry(st, W, H);  // fallback when the menu could not be patched

  // The corner's badge: the guests and the way to each. One guest: its name
  // too; more (until 0.13.9 the badge spoke of player 2 alone): each
  // player's number and milliseconds.
  std::string badge;
  if (st.p2 && !in_main_menu() && !native_menu::active()) {
    if (st.guests.size() > 1) {
      for (const HostStatus::Guest& g : st.guests)
        badge += (badge.empty() ? "" : "   ") + ("P" + std::to_string(g.player) + " " + std::to_string(g.rtt_ms) + " MS");
    } else {
      badge = "P2 " + native_menu::latin(st.p2_name) + "  " + std::to_string(st.rtt_ms) + " MS";
    }
  }
  native_menu::set_badge(badge);
  if (g_open) draw_toasts(W);  // (the F2 window's own messages)

  ImGui::PopFont();
}

void toggle() {
  g_open = !g_open;
  g_focus_pending = g_open;
  LOGI("tmntnet window %s", g_open ? "opened" : "closed");
}

void open_page(int page) {
  if (page >= 0 && page < (int)g_tabs.size()) g_select = page;
  if (!g_open) toggle();
}

void claim_escape() { g_escape_frame = ImGui::GetFrameCount(); }

bool wants_input() { return g_open; }

void debug_force_main_menu(bool on) { g_force_main_menu = on; }

void toast(const std::string& text) {
  AcquireSRWLockExclusive(&g_toast_lock);
  g_toasts.push_back({text, net::now_ms() + 4000});
  if (g_toasts.size() > 4) g_toasts.erase(g_toasts.begin());
  ReleaseSRWLockExclusive(&g_toast_lock);
}

int add_tab(const char* name, void (*draw)()) {
  g_tabs.push_back(Tab{name, draw});
  return (int)g_tabs.size() - 1;
}

}  // namespace ui
