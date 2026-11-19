#include "mod/net_pages.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "common/joincode.h"
#include "common/log.h"
#include "common/net.h"
#include "common/protocol.h"
#include "mod/lockstep.h"
#include "mod/native_menu.h"
#include "mod/net_host.h"

namespace net_pages {
namespace {

Config g_cfg;
int g_code = 0;  // which network adapter's join code is shown
std::vector<uint64_t> g_invited;  // friends invited from the page (game thread)

// The states' colours: done / waited for / not there / the one who leads.
constexpr unsigned kGreen = 0x7CFF5A, kAmber = 0xFFB432, kGrey = 0xA8B4A8, kWhite = 0xFFFFFF, kBlue = 0x64DCFF;

size_t pick(size_t n) { return size_t(((g_code % int(n)) + int(n)) % int(n)); }

std::vector<std::string> join_codes() {
  std::vector<std::string> out;
  HostStatus st = host_status();
  if (!st.enet) return out;
  for (const auto& a : net::local_ipv4()) out.push_back(joincode::encode(a.ip, st.port, tmnp::kDefaultPort));
  return out;
}

void copy_to_clipboard(const std::string& text, const char* letters) {
  if (g_cfg.debug_autostart) {  // (lab runs press keys by themselves: the PC's clipboard is left alone)
    LOGI("network page (lab): %s not copied", text.c_str());
    native_menu::notify(letters, text);
    return;
  }
  if (!OpenClipboard(nullptr)) return;
  EmptyClipboard();
  if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1)) {
    memcpy(GlobalLock(h), text.c_str(), text.size() + 1);
    GlobalUnlock(h);
    if (!SetClipboardData(CF_TEXT, h)) GlobalFree(h);
  }
  CloseClipboard();
  LOGI("network page: %s copied to the clipboard", text.c_str());
  native_menu::notify(letters, text);
}

using native_menu::latin;

std::string short_name(const std::string& utf8, size_t max = 14) {
  std::string n = latin(utf8);
  if (n.size() > max) n.resize(max);
  return n;
}

std::function<std::string()> hint(const char* text) {
  return [text] { return std::string(text); };
}

bool all_ready(const HostStatus& st) {
  if (st.guests.empty()) return false;
  for (const HostStatus::Guest& g : st.guests)
    if (!g.ready) return false;
  return true;
}

bool is_guest_of(const HostStatus& st, const std::string& name) {
  for (const HostStatus::Guest& g : st.guests)
    if (g.name == name) return true;
  return false;
}

// --- the players of the game together (the list at the top right) ------------------
// Four places. In our own game: we lead (player 1), the guests as the network
// thread has them. In a friend's game: the names its host sent, who pressed
// ГОТОВ as it says, our own from here.
native_menu::Panel players_panel() {
  using native_menu::PanelLine;
  HostStatus st = host_status();
  native_menu::Panel p;
  p.title = "ИГРОКИ";
  PanelLine line[4];
  for (int k = 0; k < 4; ++k) {
    line[k].number = std::to_string(k + 1);
    line[k].state = "СВОБОДНО";
    line[k].rgb = kGrey;
    line[k].free = true;
  }
  auto ready = [](PanelLine& l, bool said, bool own) {
    l.state = own ? (said ? "ВЫ ГОТОВЫ" : "ВЫ НЕ ГОТОВЫ") : (said ? "ГОТОВ" : "НЕ ГОТОВ");
    l.rgb = said ? kGreen : kAmber;
    l.icon = said ? native_menu::kIconTick : native_menu::kIconDots;
    l.pulse = !said;
    l.own = own;
  };
  int n = 0;
  if (st.away) {
    const int me = lockstep::my_player();
    std::vector<std::string> names = st.away_names;
    if (names.empty()) names.push_back(st.away_host);  // (its names have not come yet)
    for (int k = 0; k < 4 && k < (int)names.size(); ++k, ++n) {
      PanelLine& l = line[k];
      l.free = false;
      l.name = short_name(names[k]);
      if (k == 0) {
        l.state = st.away_up ? "ВЕДЕТ ИГРУ" : "ПОДКЛЮЧЕНИЕ";
        l.rgb = st.away_up ? kBlue : kAmber;
        l.icon = st.away_up ? native_menu::kIconStar : native_menu::kIconDots;
        l.pulse = !st.away_up;
        if (st.away_up) l.detail = std::to_string(st.away_rtt) + " MS";
      } else {
        ready(l, k == me ? lockstep::player_ready() : ((lockstep::ready_mask() >> k) & 1) != 0, k == me);
      }
    }
  } else {
    line[0].free = false;
    line[0].name = short_name(st.name);
    line[0].state = "ВЫ ВЕДЕТЕ ИГРУ";
    line[0].rgb = kBlue;
    line[0].icon = native_menu::kIconStar;
    line[0].own = true;
    n = 1;
    for (const HostStatus::Guest& g : st.guests) {
      if (g.player < 2 || g.player > 4) continue;
      PanelLine& l = line[g.player - 1];
      l.free = false;
      l.name = short_name(g.name);
      l.detail = std::to_string(g.rtt_ms) + " MS";
      ready(l, g.ready, false);
      ++n;
    }
  }
  p.count = std::to_string(n) + " / 4";
  p.lines.assign(line, line + 4);
  return p;
}

// --- the pages under the host's ---------------------------------------------------

// Steam friends who are online: decide sends a lobby invite. Those with the
// game running first (only they can accept it).
void open_invites() {
  native_menu::Page p;
  p.title = "ПРИГЛАСИТЬ ДРУГА";
  HostStatus st = host_status();
  std::vector<lobby::Friend> friends = st.friends;
  std::stable_sort(friends.begin(), friends.end(),
                   [](const lobby::Friend& a, const lobby::Friend& b) { return a.playing_480 && !b.playing_480; });
  size_t shown = std::min<size_t>(friends.size(), 7);
  for (size_t i = 0; i < shown; ++i) {
    const lobby::Friend f = friends[i];
    native_menu::Row r;
    std::string name = short_name(f.name, 16), full = f.name;
    r.plain_label = name;
    uint64_t id = f.id;
    bool playing = f.playing_480;
    auto state = [id, playing, full] {  // 0 game off, 1 game on, 2 invited, 3 in our game
      if (is_guest_of(host_status(), full)) return 3;
      if (std::find(g_invited.begin(), g_invited.end(), id) != g_invited.end()) return 2;
      return playing ? 1 : 0;
    };
    r.text = [state] {
      static const char* const kText[] = {"ИГРА ВЫКЛЮЧЕНА", "ИГРА ВКЛЮЧЕНА", "ПРИГЛАШЕН", "ПОДКЛЮЧЕН"};
      return std::string(kText[state()]);
    };
    r.text_rgb = [state] {
      static const unsigned kRgb[] = {kGrey, kWhite, kAmber, kGreen};
      return kRgb[state()];
    };
    r.hint = [state] {
      static const char* const kHint[] = {
          "ДРУГ ДОЛЖЕН ЗАПУСТИТЬ ИГРУ, ЗАТЕМ ПРИНЯТЬ ПРИГЛАШЕНИЕ В STEAM",
          "ВЫБРАТЬ - ОТПРАВИТЬ ПРИГЛАШЕНИЕ. ДРУГ ПРИНИМАЕТ ЕГО В STEAM",
          "ПРИГЛАШЕНИЕ ОТПРАВЛЕНО. ЖДЁМ, КОГДА ДРУГ ПРИМЕТ ЕГО В STEAM",
          "ЭТОТ ИГРОК УЖЕ В ВАШЕЙ ИГРЕ"};
      return std::string(kHint[state()]);
    };
    r.action = [id, name, state] {
      if (state() == 3) return;
      if (std::find(g_invited.begin(), g_invited.end(), id) == g_invited.end()) g_invited.push_back(id);
      host_invite(id);
      LOGI("network page: Steam invite sent to %s", name.c_str());
      native_menu::notify("ПРИГЛАШЕНИЕ ОТПРАВЛЕНО", name);
    };
    p.rows.push_back(r);
  }
  if (shown == 0) {
    native_menu::Row r;
    r.label = "НЕТ ДРУЗЕЙ В СЕТИ";
    r.selectable = false;
    p.rows.push_back(r);
    p.ok_hint = "ДРУЗЬЯ ПОЯВЯТСЯ ЗДЕСЬ, КОГДА БУДУТ В СЕТИ STEAM";
  } else {
    p.ok_hint = "НАЗАД, К ИГРОКАМ";
  }
  native_menu::open(std::move(p));
}

// Friends' tmntnet games over Steam (the game on every PC): decide joins one.
// Once its session is set, the pages close and the page of that game opens.
void open_games() {
  native_menu::Page p;
  p.title = "ВОЙТИ К ДРУГУ";
  HostStatus st = host_status();
  size_t shown = std::min<size_t>(st.games.size(), 7);
  for (size_t i = 0; i < shown; ++i) {
    const lobby::Found g = st.games[i];
    native_menu::Row r;
    std::string name = short_name(g.host.empty() ? g.name : g.host, 16);
    r.plain_label = name;
    bool playing = g.state == "ingame";
    r.text = [playing] { return std::string(playing ? "УЖЕ ИГРАЕТ" : "ЖДЕТ ИГРОКОВ"); };
    r.text_rgb = [playing] { return playing ? kAmber : kGreen; };
    r.hint = hint(playing ? "ДРУГ УЖЕ ИГРАЕТ: ВЫ ВОЙДЁТЕ, КОГДА ОН ВЕРНЁТСЯ В ГЛАВНОЕ МЕНЮ"
                          : "ВЫБРАТЬ - ВОЙТИ В ИГРУ ДРУГА. ИГРУ ВЕДЁТ ОН, ЧЕРЕПАХА У ВАС СВОЯ");
    uint64_t lobby_id = g.lobby;
    r.action = [lobby_id, name] {
      host_join(lobby_id);
      LOGI("network page: joining %s's game", name.c_str());
    };
    p.rows.push_back(r);
  }
  if (shown == 0) {
    native_menu::Row r;
    r.label = "НЕТ ИГР ДРУЗЕЙ";
    r.selectable = false;
    p.rows.push_back(r);
  }
  p.status = [] {
    HostStatus s = host_status();
    if (s.away) return native_menu::Status{"ПОДКЛЮЧЕНИЕ", kAmber};
    if (!s.away_error.empty()) return native_menu::Status{"НЕ УДАЛОСЬ ВОЙТИ", kAmber};
    return native_menu::Status{};
  };
  p.ok_hint = shown ? "НАЗАД, К ИГРОКАМ" : "ИГРА ДРУГА ПОЯВИТСЯ ЗДЕСЬ, КОГДА ОН ЗАПУСТИТ ИГРУ С МОДОМ";
  native_menu::open(std::move(p));
}

// Our guests: decide removes one.
void open_kick() {
  native_menu::Page p;
  p.title = "ВЫГНАТЬ ИГРОКА";
  HostStatus st = host_status();
  for (const HostStatus::Guest& g : st.guests) {
    native_menu::Row r;
    int player = g.player;
    std::string name = g.name;
    r.plain_label = std::to_string(player) + "  " + short_name(name, 16);
    r.shown = [player, name] {  // (gone meanwhile)
      for (const HostStatus::Guest& o : host_status().guests)
        if (o.player == player && o.name == name) return true;
      return false;
    };
    r.hint = hint("ВЫБРАТЬ - УБРАТЬ ЭТОГО ИГРОКА ИЗ ВАШЕЙ ИГРЫ");
    r.action = [player] {
      host_kick(player);
      native_menu::back();
    };
    p.rows.push_back(r);
  }
  p.ok_hint = "НАЗАД, НИКОГО НЕ ВЫГОНЯТЬ";
  native_menu::open(std::move(p));
}

// The ways to join by hand: the join code (LAN, VPN), the Steam ID and the
// lobby's ID (decide copies them).
void open_codes() {
  native_menu::Page p;
  p.title = "КОДЫ";
  native_menu::Row code;
  code.label = "КОД";
  code.text = [] {
    std::vector<std::string> c = join_codes();
    return c.empty() ? std::string("-") : c[pick(c.size())];
  };
  code.values = {"", "", ""};  // (left/right step through the codes)
  code.get = [] { return 1; };
  code.set = [](int v) { g_code += v - 1; };
  code.action = [] {
    std::vector<std::string> c = join_codes();
    if (!c.empty()) copy_to_clipboard(c[pick(c.size())], "КОД СКОПИРОВАН");
  };
  code.hint = hint("КОД ЭТОГО ПК В СЕТИ (LAN, VPN). ВЫБРАТЬ - СКОПИРОВАТЬ");
  p.rows.push_back(code);
  HostStatus st = host_status();
  if (st.steam) {
    native_menu::Row steam;
    steam.label = "СТИМ";
    std::string name = latin(st.steam_name);
    steam.text = [name] { return name; };
    steam.action = [] {
      HostStatus s = host_status();
      if (s.steam_id) copy_to_clipboard(std::to_string(s.steam_id), "СКОПИРОВАНО");
    };
    steam.hint = hint("ВАШ STEAM ID. ВЫБРАТЬ - СКОПИРОВАТЬ");
    p.rows.push_back(steam);
    native_menu::Row lobby;
    lobby.label = "ЛОББИ";
    lobby.text = [] {
      HostStatus s = host_status();
      return s.lobby_id ? std::to_string(s.lobby_id) : std::string("...");
    };
    lobby.action = [] {
      HostStatus s = host_status();
      if (s.lobby_id) copy_to_clipboard(std::to_string(s.lobby_id), "СКОПИРОВАНО");
    };
    lobby.hint = hint("НОМЕР ВАШЕГО ЛОББИ STEAM. ВЫБРАТЬ - СКОПИРОВАТЬ");
    p.rows.push_back(lobby);
  }
  p.ok_hint = "НАЗАД, К ИГРОКАМ";
  native_menu::open(std::move(p));
}

// --- the host's page ----------------------------------------------------------------
// The players at the right; start, invite, join a friend's game instead, the
// codes, remove a player. At the bottom: what is waited for, what the row does.
native_menu::Page host_page() {
  lockstep::host_autostart(0);  // (a start planned before and not begun: not any more)
  native_menu::Page p;
  p.title = "СЕТЕВАЯ ИГРА";
  p.panel = players_panel;
  p.status = [] {
    HostStatus s = host_status();
    if (!s.running) return native_menu::Status{"НЕТ СЕТИ", kAmber};
    if (s.guests.empty())
      return s.steam ? native_menu::Status{"ПРИГЛАСИТЕ ДРУЗЕЙ", kWhite} : native_menu::Status{"ЗАПУСТИТЕ СТИМ", kAmber};
    if (!all_ready(s)) return native_menu::Status{"ЖДЕМ ГОТОВНОСТИ ИГРОКОВ", kAmber};
    return native_menu::Status{"ВСЕ ГОТОВЫ - НАЧИНАЙТЕ ИГРУ", kGreen};
  };
  p.ok_hint = "НАЗАД В ГЛАВНОЕ МЕНЮ. ИГРОКИ ОСТАЮТСЯ ПОДКЛЮЧЕНЫ";
  HostStatus st = host_status();
  p.first = st.guests.empty() ? 1 : 0;  // (nobody yet: on ПРИГЛАСИТЬ ДРУГА)
  auto dim_without_steam = [] { return host_status().steam ? kWhite : kGrey; };

  native_menu::Row start;
  start.label = "НАЧАТЬ ИГРУ";
  start.label_rgb = [] {
    HostStatus s = host_status();
    if (s.guests.empty() || !all_ready(s)) return kGrey;
    // (everybody is ready: it glows, green to white and back)
    const float t = 0.5f + 0.5f * std::sin(float(GetTickCount() % 100000) * 0.005f);
    auto ch = [t](int sh) { return unsigned(float((kGreen >> sh) & 255) * (1 - t * 0.6f) + 255.0f * t * 0.6f) << sh; };
    return ch(16) | ch(8) | ch(0);
  };
  start.hint = [] {
    HostStatus s = host_status();
    return std::string(s.guests.empty()  ? "СНАЧАЛА ПРИГЛАСИТЕ ДРУЗЕЙ"
                       : !all_ready(s)   ? "ИГРА НАЧНЁТСЯ, КОГДА ВСЕ ИГРОКИ НАЖМУТ ГОТОВ"
                                         : "СЮЖЕТ ДЛЯ ВСЕЙ КОМАНДЫ. ЧЕРЕПАХУ КАЖДЫЙ ВЫБИРАЕТ САМ");
  };
  start.action = [] {
    HostStatus s = host_status();
    if (s.guests.empty()) {
      native_menu::notify("НЕТ ИГРОКОВ");
      return;
    }
    if (!all_ready(s)) {
      native_menu::notify("НЕ ВСЕ ГОТОВЫ");
      return;
    }
    int players = 1;
    for (const HostStatus::Guest& g : s.guests) players = std::max(players, g.player);
    lockstep::host_autostart(players);
    native_menu::close_all();  // (the game together begins at the main menu, then the story)
  };
  p.rows.push_back(start);

  native_menu::Row invite;
  invite.label = "ПРИГЛАСИТЬ ДРУГА";
  invite.label_rgb = dim_without_steam;
  invite.hint = [] {
    return std::string(host_status().steam ? "ДРУГ ПОЛУЧИТ ПРИГЛАШЕНИЕ В STEAM. МОЖНО ПОЗВАТЬ ДО ТРЁХ ДРУЗЕЙ"
                                           : "STEAM НЕ ЗАПУЩЕН. ЗАПУСТИТЕ ЕГО - ПРИГЛАШЕНИЯ ЗАРАБОТАЮТ САМИ");
  };
  invite.action = [] {
    if (host_status().steam)
      open_invites();
    else
      native_menu::notify("СТИМ ВЫКЛЮЧЕН");
  };
  p.rows.push_back(invite);

  native_menu::Row games;
  games.label = "ВОЙТИ К ДРУГУ";
  games.label_rgb = [] {
    HostStatus s = host_status();
    return s.steam && s.guests.empty() ? kWhite : kGrey;
  };
  games.hint = [] {
    HostStatus s = host_status();
    return std::string(!s.steam            ? "STEAM НЕ ЗАПУЩЕН. ЗАПУСТИТЕ ЕГО - ИГРЫ ДРУЗЕЙ ПОЯВЯТСЯ САМИ"
                       : !s.guests.empty() ? "У ВАС УЖЕ ЕСТЬ ИГРОКИ. СНАЧАЛА ОТКЛЮЧИТЕ ИХ"
                                           : "ИГРАТЬ В ИГРЕ ДРУГА, А НЕ В СВОЕЙ. ИГРУ ВЕДЁТ ОН");
  };
  games.action = [] {
    HostStatus s = host_status();
    if (!s.steam)
      native_menu::notify("СТИМ ВЫКЛЮЧЕН");
    else if (!s.guests.empty())
      native_menu::notify("СНАЧАЛА ОТКЛЮЧИТЕ ИГРОКОВ");
    else
      open_games();
  };
  p.rows.push_back(games);

  native_menu::Row codes;
  codes.label = "КОДЫ";
  codes.hint = hint("ДЛЯ ВХОДА БЕЗ ПРИГЛАШЕНИЯ: КОД СЕТИ, STEAM ID, НОМЕР ЛОББИ");
  codes.action = [] { open_codes(); };
  p.rows.push_back(codes);

  native_menu::Row out;
  out.label = "ВЫГНАТЬ ИГРОКА";
  out.shown = [] { return !host_status().guests.empty(); };
  out.hint = hint("УБРАТЬ ИГРОКА ИЗ ВАШЕЙ ИГРЫ");
  out.action = [] { open_kick(); };
  p.rows.push_back(out);
  return p;
}

// --- in a friend's game ---------------------------------------------------------------
// The players at the right; ГОТОВ (the game together begins once every player
// said it and the host starts), leave.
native_menu::Page guest_page() {
  native_menu::Page p;
  p.title = "СЕТЕВАЯ ИГРА";
  p.panel = players_panel;
  p.status = [] {
    HostStatus s = host_status();
    if (!s.away_up) return native_menu::Status{"ПОДКЛЮЧЕНИЕ", kAmber};
    if (!lockstep::player_ready()) return native_menu::Status{"ЖДЕМ ВАШЕЙ ГОТОВНОСТИ", kAmber};
    return native_menu::Status{"ЖДЕМ НАЧАЛА ИГРЫ", kGreen};
  };
  p.ok_hint = "НАЗАД В ГЛАВНОЕ МЕНЮ. ВЫ ОСТАЁТЕСЬ В ИГРЕ ДРУГА";

  native_menu::Row ready;
  ready.label_fn = [] { return std::string(lockstep::player_ready() ? "Я НЕ ГОТОВ" : "Я ГОТОВ"); };
  ready.label_rgb = [] { return lockstep::player_ready() ? kWhite : kGreen; };
  ready.hint = [] {
    return std::string(lockstep::player_ready() ? "ВЫ ГОТОВЫ: ЖДИТЕ, ИГРУ НАЧНЁТ ВЕДУЩИЙ. ВЫБРАТЬ - ОТМЕНИТЬ ГОТОВНОСТЬ"
                                                : "ВЫБЕРИТЕ, КОГДА ГОТОВЫ ИГРАТЬ. ИГРУ НАЧИНАЕТ ВЕДУЩИЙ");
  };
  ready.action = [] { lockstep::set_player_ready(!lockstep::player_ready()); };
  p.rows.push_back(ready);

  native_menu::Row out;
  out.label = "ОТКЛЮЧИТЬСЯ";
  out.hint = hint("ВЫЙТИ ИЗ ИГРЫ ДРУГА И ВЕРНУТЬСЯ В СВОЮ");
  out.action = [] {
    lockstep::set_player_ready(false);
    host_leave();
    native_menu::notify("ОТКЛЮЧЕНО");
    if (!native_menu::replace(host_page())) native_menu::close_all();  // (our own game's page again)
  };
  p.rows.push_back(out);
  return p;
}

// role=client in tmntnet.ini (this game joins a host by address): what the
// link is; nothing to set here.
void open_client_page() {
  native_menu::Page p;
  p.title = "СЕТЕВАЯ ИГРА";
  native_menu::Row r;
  r.label = "ИГРА ПО АДРЕСУ";
  r.selectable = false;
  p.rows.push_back(r);
  p.status = [] {
    return lockstep::state() == lockstep::State::Off ? native_menu::Status{"ПОДКЛЮЧЕНИЕ", kAmber}
                                                     : native_menu::Status{"ЖДЕМ НАЧАЛА ИГРЫ", kGreen};
  };
  p.ok_hint = "ИГРА ПОДКЛЮЧАЕТСЯ К АДРЕСУ ИЗ TMNTNET.INI (ROLE=CLIENT, PEER=)";
  native_menu::open(std::move(p));
}

}  // namespace

void install(const Config& cfg) { g_cfg = cfg; }

bool show_guest() {
  if (lockstep::state() == lockstep::State::Running) return true;  // (nothing to show any more)
  if (native_menu::active()) return native_menu::replace(guest_page());
  native_menu::open(guest_page());
  return true;
}

void open_network(int by_slot) {
  // In a game together every game takes the same buttons: a page here and not
  // there (or a different one) would part them. The host's own decide (player
  // 1's) still opens it at the main menu: the game together stops on every PC
  // (the host's word sets it again with everybody, also whoever joins
  // meanwhile) and begins again from the page.
  if (lockstep::state() == lockstep::State::Running) {
    HostStatus st = host_status();
    bool host = g_cfg.is_host() && !st.away;
    if (!host || by_slot != 0) {
      // (a guest's own decide: it says so; the other PCs' decide: nothing here)
      if (!host && by_slot != 0) native_menu::notify("ИДЁТ СЕТЕВАЯ ИГРА");
      return;
    }
    lockstep::net_set_again("the host opened its network page: set again when it closes");
    host_set_again();
  }
  HostStatus st = host_status();
  if (st.away) return native_menu::open(guest_page());
  if (g_cfg.is_host()) return native_menu::open(host_page());
  open_client_page();
}

}  // namespace net_pages
