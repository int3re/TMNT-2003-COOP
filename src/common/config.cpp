#include "common/config.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace {

// The game's own default P2 keyboard binds (table at 0x5f4bb0 + 0x40 in
// TMNT.EXE bc49a82c...): NumPad+, NumPad6, NumPad9, NumPad8, NumPad7, NumPad0,
// NumPad4, NumPad5, Esc, -, -, NumPadEnter, Home, PgDn, End, Delete.
constexpr int kDefaultP2Keys[tmnp::kNumSlots] = {
    0x4E, 0x4D, 0x49, 0x48, 0x47, 0x52, 0x4B, 0x4C,
    0x01, -1,   -1,   0x9C, 0xC7, 0xD1, 0xCF, 0xD3};

std::string to_utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring to_wide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
  return w;
}

std::string trim_lower(std::string s, bool lower) {
  // Strip inline comments ("; ...") and whitespace.
  size_t c = s.find(';');
  if (c != std::string::npos) s.resize(c);
  auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  if (lower)
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char ch) { return (char)std::tolower(ch); });
  return s;
}

std::vector<std::string> split_list(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : s) {
    if (ch == ',' || ch == ' ' || ch == '\t') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += ch;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

class Ini {
 public:
  explicit Ini(const std::wstring& path) : path_(path) { read_utf8(); }

  bool has(const char* section, const char* key) const {
    wchar_t buf[8];
    DWORD n = GetPrivateProfileStringW(to_wide(section).c_str(), to_wide(key).c_str(),
                                       L"\x01", buf, 8, path_.c_str());
    return !(n == 1 && buf[0] == L'\x01');
  }
  std::string get(const char* section, const char* key, const std::string& def,
                  bool lower = true) const {
    if (!has(section, key)) return def;
    std::string raw;
    if (utf8_ && raw_value(section, key, raw)) return trim_lower(raw, lower);
    // Long values (the lab's [debug] shots) get a bigger buffer: the call
    // returns size - 1 when it had to cut the value.
    std::vector<wchar_t> buf(512);
    for (;;) {
      DWORD n = GetPrivateProfileStringW(to_wide(section).c_str(), to_wide(key).c_str(), L"", buf.data(),
                                         (DWORD)buf.size(), path_.c_str());
      if (n + 1 < buf.size() || buf.size() >= 65536) break;
      buf.resize(buf.size() * 2);
    }
    return trim_lower(to_utf8(buf.data()), lower);
  }
  int get_int(const char* section, const char* key, int def) const {
    return parse_int(get(section, key, ""), def);
  }

 private:
  // The profile API reads a file without a BOM as ANSI, but tmntnet.ini is
  // UTF-8 (and Notepad saves so): Russian values (a name) would come out
  // garbled. From such a file the values are taken from its text.
  void read_utf8() {
    HANDLE f = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    std::string t;
    if (size != INVALID_FILE_SIZE && size < (4u << 20)) {
      t.resize(size);
      if (!ReadFile(f, t.data(), size, &got, nullptr)) got = 0;
      t.resize(got);
    }
    CloseHandle(f);
    if (t.size() >= 2 && (unsigned char)t[0] == 0xFF && (unsigned char)t[1] == 0xFE) return;  // UTF-16: read right
    if (t.compare(0, 3, "\xEF\xBB\xBF") == 0) t.erase(0, 3);
    bool wide = std::any_of(t.begin(), t.end(), [](char ch) { return (unsigned char)ch >= 0x80; });
    if (!wide || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, t.data(), (int)t.size(), nullptr, 0) <= 0) return;
    text_ = std::move(t);
    utf8_ = true;
  }

  // As the profile API finds it: the first key of that name in the first
  // section of that name (names without case), the rest of the line, quotes
  // around it dropped.
  bool raw_value(const char* section, const char* key, std::string& out) const {
    auto lower = [](std::string s) {
      for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
      return s;
    };
    auto trim = [](const std::string& s) {
      size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
      return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    const std::string want_section = lower(section), want_key = lower(key);
    bool in = false, seen_section = false;
    for (size_t p = 0; p < text_.size();) {
      size_t e = text_.find('\n', p);
      std::string line = text_.substr(p, e == std::string::npos ? std::string::npos : e - p);
      p = e == std::string::npos ? text_.size() : e + 1;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::string t = trim(line);
      if (!t.empty() && t[0] == '[') {
        if (seen_section && in) return false;
        size_t close = t.find(']');
        in = close != std::string::npos && lower(trim(t.substr(1, close - 1))) == want_section;
        seen_section = seen_section || in;
        continue;
      }
      size_t eq = t.find('=');
      if (!in || eq == std::string::npos || lower(trim(t.substr(0, eq))) != want_key) continue;
      out = trim(t.substr(eq + 1));
      if (out.size() >= 2 && out.front() == '"' && out.back() == '"') out = out.substr(1, out.size() - 2);
      return true;
    }
    return false;
  }

  std::wstring path_;
  std::string text_;
  bool utf8_ = false;
};

void set_default_maps(Config& cfg) {
  using namespace tmnp;
  for (int i = 0; i < kNumSlots; ++i) {
    cfg.p2_keys[i] = kDefaultP2Keys[i];
    cfg.pad_map[i] = 0;
    cfg.key_map[i].clear();
  }
  auto slot = [](Button b) {
    int i = 0;
    while (!((1u << i) & b)) ++i;
    return i;
  };
  // XInput defaults (directions always also come from the d-pad and left stick).
  cfg.pad_map[slot(BTN_JUMP)] = parse_pad_button("A");
  cfg.pad_map[slot(BTN_ATTACK)] = parse_pad_button("X");
  cfg.pad_map[slot(BTN_STRONG)] = parse_pad_button("Y");
  cfg.pad_map[slot(BTN_SHURIKEN)] = parse_pad_button("B");
  cfg.pad_map[slot(BTN_DASH)] = parse_pad_button("RB");
  cfg.pad_map[slot(BTN_SHURIKEN_CHANGE)] = parse_pad_button("LB");
  cfg.pad_map[slot(BTN_SPECIAL)] = kPadRT;
  cfg.pad_map[slot(BTN_JUGGLE)] = kPadLT;
  cfg.pad_map[slot(BTN_START)] = parse_pad_button("START");
  cfg.pad_map[slot(BTN_PAUSE)] = parse_pad_button("BACK");
  // Keyboard defaults avoid the game's P1 keys (arrows, E R S D F Space LCtrl
  // LShift Enter) so the client can be tested on the same PC as the game.
  cfg.key_map[slot(BTN_UP)] = {parse_vk("I")};
  cfg.key_map[slot(BTN_DOWN)] = {parse_vk("K")};
  cfg.key_map[slot(BTN_LEFT)] = {parse_vk("J")};
  cfg.key_map[slot(BTN_RIGHT)] = {parse_vk("L")};
  cfg.key_map[slot(BTN_JUMP)] = {parse_vk("Z")};
  cfg.key_map[slot(BTN_ATTACK)] = {parse_vk("A")};
  cfg.key_map[slot(BTN_STRONG)] = {parse_vk("X")};
  cfg.key_map[slot(BTN_SHURIKEN)] = {parse_vk("Q")};
  cfg.key_map[slot(BTN_DASH)] = {parse_vk("W")};
  cfg.key_map[slot(BTN_SPECIAL)] = {parse_vk("C")};
  cfg.key_map[slot(BTN_JUGGLE)] = {parse_vk("V")};
  cfg.key_map[slot(BTN_SHURIKEN_CHANGE)] = {parse_vk("B")};
  cfg.key_map[slot(BTN_START)] = {parse_vk("T")};
  cfg.key_map[slot(BTN_PAUSE)] = {parse_vk("P")};
}

}  // namespace

uint64_t parse_steamid(const std::string& s) {
  // The first run of 17 digits: plain "7656119...", "steam:7656119..." or a
  // https://steamcommunity.com/profiles/7656119.../ link.
  for (size_t i = 0; i < s.size(); ++i) {
    size_t j = i;
    while (j < s.size() && std::isdigit((unsigned char)s[j])) ++j;
    if (j - i == 17) {
      uint64_t id = std::strtoull(s.substr(i, 17).c_str(), nullptr, 10);
      return (id >> 52) == 0x011 ? id : 0;  // public universe, individual account
    }
    if (j > i) i = j;
  }
  return 0;
}

int parse_int(const std::string& s, int fallback) {
  if (s.empty()) return fallback;
  char* end = nullptr;
  long v = std::strtol(s.c_str(), &end, 0);
  if (end == s.c_str() || *end != '\0') return fallback;
  return (int)v;
}

std::wstring module_dir(void* module) {
  wchar_t path[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(static_cast<HMODULE>(module), path, MAX_PATH * 2);
  std::wstring p(path, n);
  size_t slash = p.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L".\\" : p.substr(0, slash + 1);
}

bool load_config(const std::wstring& path, Config& cfg) {
  set_default_maps(cfg);
  {
    char host[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = sizeof(host);
    cfg.name = GetComputerNameA(host, &n) ? std::string(host, n) : "player";
  }
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

  Ini ini(path);
  cfg.role = ini.get("net", "role", cfg.role);
  cfg.peer = ini.get("net", "peer", cfg.peer);
  cfg.port = (uint16_t)ini.get_int("net", "port", cfg.port);
  cfg.discovery_port = (uint16_t)ini.get_int("net", "discovery_port", cfg.discovery_port);
  cfg.input_delay = std::clamp(ini.get_int("net", "input_delay", cfg.input_delay), 0, 10);
  cfg.timeout_ms = std::clamp(ini.get_int("net", "timeout_ms", cfg.timeout_ms), 200, 30000);
  std::string name = ini.get("net", "name", "", /*lower=*/false);
  if (!name.empty()) cfg.name = name, cfg.name_set = true;
  cfg.transport = ini.get("net", "transport", cfg.transport);
  if (cfg.transport != "steam" && cfg.transport != "enet" && cfg.transport != "mock") cfg.transport = "auto";
  cfg.lobby_type = ini.get("net", "lobby_type", cfg.lobby_type) == "public" ? "public" : "friends";
  cfg.host_steamid = parse_steamid(ini.get("net", "host_steamid", ""));
  for (const std::string& s : split_list(ini.get("net", "allow_steamid", "")))
    if (uint64_t id = parse_steamid(s)) cfg.allow_steamid.push_back(id);
  cfg.mock_slot = std::clamp(ini.get_int("net", "mock_slot", cfg.mock_slot), 1, 99);
  cfg.mock_dir = ini.get("net", "mock_dir", cfg.mock_dir, /*lower=*/false);
  cfg.mock_loss_pct = std::clamp(ini.get_int("net", "mock_loss_pct", 0), 0, 100);
  cfg.mock_latency_ms = std::clamp(ini.get_int("net", "mock_latency_ms", 0), 0, 2000);
  cfg.mock_lobby = ini.get_int("net", "mock_lobby", 1) != 0;
  cfg.mock_member_delay_ms = std::clamp(ini.get_int("net", "mock_member_delay_ms", 0), 0, 10000);

  cfg.log_level = tlog::parse_level(ini.get("log", "log_level", "info"), tlog::Info);
  cfg.debug_autostart = std::clamp(ini.get_int("debug", "autostart", 0), 0, 2);
  for (const std::string& s : split_list(ini.get("debug", "shots", ""))) {
    size_t c = s.find(':');
    if (c != std::string::npos) cfg.debug_shots.emplace_back(s.substr(0, c), parse_int(s.substr(c + 1), 0));
  }
  cfg.debug_multi_instance = ini.get_int("debug", "multi_instance", 0) != 0;
  cfg.debug_light = ini.get_int("debug", "light", 0) != 0;
  cfg.debug_hud_trace = ini.get_int("debug", "hud_trace", 0) != 0;
  cfg.debug_stage = std::clamp(ini.get_int("debug", "stage", 0), 0, 52);
  cfg.debug_voice_extra = std::clamp(ini.get_int("debug", "voice_extra", 0), 0, 36000);
  cfg.debug_costume_step = ini.get_int("debug", "costume_step", 1) != 0;
  cfg.debug_costume_default = std::clamp(ini.get_int("debug", "costume_default", 0), 0, 5);
  cfg.debug_camera_player = std::clamp(ini.get_int("debug", "camera_player", 0), 0, 4);
  cfg.debug_camera_trace = ini.get_int("debug", "camera_trace", 0) != 0;
  cfg.debug_auto_ready = ini.get_int("debug", "auto_ready", cfg.debug_autostart ? 1 : 0) != 0;
  cfg.debug_break_dict = std::max(0, ini.get_int("debug", "break_dict", 0));
  cfg.debug_sync_menu = ini.get("debug", "sync", "") == "menu";
  cfg.debug_seed = (uint32_t)ini.get_int("debug", "seed", 12345);
  for (const std::string& s : split_list(ini.get("debug", "frame_keys", ""))) {
    Config::FrameKey k;
    char* end = nullptr;
    k.frame = (int)strtol(s.c_str(), &end, 10);
    if (!end || *end != ':') continue;
    k.dik = (int)strtol(end + 1, &end, 16);
    if (end && *end == ':') k.frames = std::max(1, (int)strtol(end + 1, &end, 10));
    if (k.dik > 0 && k.dik < 256) cfg.debug_frame_keys.push_back(k);
  }
  cfg.debug_frame_log = ini.get_int("debug", "frame_log", 0) != 0;
  cfg.debug_rand_trace = ini.get_int("debug", "rand_trace", 0) != 0;
  cfg.debug_task_log = ini.get_int("debug", "task_log", 0) != 0;
  cfg.debug_stall_at = std::max(0, ini.get_int("debug", "stall_at", 0));
  cfg.debug_text_trace = ini.get_int("debug", "text_trace", 0) != 0;
  cfg.debug_movie_sync = ini.get_int("debug", "movie_sync", 1) != 0;
  cfg.debug_movie_late = std::clamp(ini.get_int("debug", "movie_late", 0), 0, 100000);
  cfg.debug_level_dump = ini.get_int("debug", "level_dump", 0) != 0;
  cfg.debug_profile = ini.get_int("debug", "profile", 0) != 0;
  cfg.debug_session_seed = (uint32_t)std::max(0, ini.get_int("debug", "session_seed", 0));
  cfg.debug_stage_over_at = std::max(0, ini.get_int("debug", "stage_over_at", 0));
  cfg.debug_camera_keep = ini.get_int("debug", "camera_keep", 0) != 0;
  cfg.debug_stall_ms = std::clamp(ini.get_int("debug", "stall_ms", 0), 0, 60000);
  cfg.debug_actions = ini.get_int("debug", "actions", 0) != 0;
  {
    std::string raw = ini.get("debug", "nametags", "", /*lower=*/false);  // names may have spaces
    for (size_t a = 0; !raw.empty() && a <= raw.size();) {
      size_t b = std::min(raw.find(',', a), raw.size());
      std::string n = raw.substr(a, b - a);
      n.erase(0, n.find_first_not_of(" \t"));
      n.erase(n.find_last_not_of(" \t") + 1);
      cfg.debug_nametags.push_back(n);
      a = b + 1;
    }
  }
  auto floats = [&](const char* key, float (&out)[2]) {
    auto items = split_list(ini.get("debug", key, ""));
    for (size_t i = 0; i < 2 && i < items.size(); ++i) out[i] = std::strtof(items[i].c_str(), nullptr);
  };
  floats("tag_height", cfg.debug_tag_height);
  floats("map_offset", cfg.debug_map_offset);
  cfg.debug_revive_reach = std::strtof(ini.get("debug", "revive_reach", "0").c_str(), nullptr);
  cfg.debug_log_2d = ini.get_int("debug", "log_2d", 0) != 0;
  cfg.debug_log_2d_at = ini.get_int("debug", "log_2d_at", 0);
  if (cfg.debug_log_2d_at > 0) cfg.debug_log_2d = true;

  {
    int n = ini.get_int("mod", "players", 2);
    cfg.players = n >= 4 ? 4 : n == 3 ? 3 : 2;
  }
  cfg.revive = ini.get_int("mod", "revive", 1) != 0;
  {
    std::string b = ini.get("mod", "revive_button", "shuriken_change");
    cfg.revive_button = 3;
    for (int i = 0; i < tmnp::kNumSlots; ++i)
      if (tmnp::kSlotNames[i] && b == tmnp::kSlotNames[i]) cfg.revive_button = i;
  }
  for (int x = 0; x < 2; ++x) {
    std::string pad = ini.get("mod", x ? "p4_pad" : "p3_pad", "auto");
    cfg.extra_pad[x] = pad.empty() || pad == "auto" ? -1 : std::clamp(parse_int(pad, -1), -1, 16);
  }
  for (int x = 0; x < 2; ++x) {
    auto items = split_list(ini.get("mod", x ? "p4_keys" : "p3_keys", ""));
    for (int i = 0; i < tmnp::kNumSlots; ++i) {
      int v = i < (int)items.size() ? parse_int(items[i], -1) : -1;
      cfg.extra_keys[x][i] = (v > 0 && v < 256) ? v : -1;
    }
  }
  cfg.p2_binds_auto = ini.get("mod", "p2_binds", "auto") != "ini";
  if (ini.has("mod", "p2_keys")) {
    auto items = split_list(ini.get("mod", "p2_keys", ""));
    for (int i = 0; i < tmnp::kNumSlots && i < (int)items.size(); ++i) {
      int v = parse_int(items[i], -1);
      cfg.p2_keys[i] = (v > 0 && v < 256) ? v : -1;
    }
  }

  if (ini.has("mod", "custom_keys_1") && ini.has("mod", "custom_keys_2")) {
    cfg.custom_keys = true;
    for (int p = 0; p < 2; ++p) {
      auto items = split_list(ini.get("mod", p ? "custom_keys_2" : "custom_keys_1", ""));
      for (int i = 0; i < tmnp::kNumSlots; ++i) {
        int v = i < (int)items.size() ? parse_int(items[i], -1) : -1;
        cfg.custom_key[p][i] = (v > 0 && v < 256) ? v : -1;
      }
    }
  }

  cfg.p2_autopick = ini.get_int("mod", "p2_autopick", cfg.p2_autopick ? 1 : 0) != 0;
  cfg.p2_lock = ini.get_int("mod", "p2_lock", 1) != 0;
  cfg.autosave = ini.get_int("mod", "autosave", 1) != 0;
  cfg.p2_autopick_delay_ms =
      std::clamp(ini.get_int("mod", "p2_autopick_delay_ms", cfg.p2_autopick_delay_ms), 0, 60000);
  {
    std::string c = ini.get("mod", "p2_character", "auto");
    cfg.p2_character = (c == "auto" || c == "random" || c.empty()) ? -1 : c == "first" ? -2 : parse_int(c, -1);
  }

  cfg.widescreen = ini.get_int("video", "widescreen", cfg.widescreen ? 1 : 0) != 0;
  {
    std::string r = ini.get("video", "resolution", "desktop");
    int w = 0, h = 0;
    if (r == "game") {
      cfg.res_mode = 1;
    } else if (sscanf(r.c_str(), "%dx%d", &w, &h) == 2 && w >= 640 && h >= 480) {
      cfg.res_mode = 2;
      cfg.res_w = w;
      cfg.res_h = h;
    } else {
      cfg.res_mode = 0;
    }
  }
  cfg.refresh_hz = std::clamp(ini.get_int("video", "refresh", cfg.refresh_hz), 0, 1000);
  cfg.hud_center = ini.get("video", "hud", "center") != "stretch";
  cfg.fullscreen_pillarbox = ini.get("video", "fullscreen_images", "pillarbox") != "stretch";
  cfg.side_bars_blur = ini.get("video", "side_bars", "blur") != "black";
  cfg.wide_menus = ini.get_int("video", "wide_menus", cfg.wide_menus ? 1 : 0) != 0;
  cfg.hud_corners = ini.get_int("video", "hud_corners", cfg.hud_corners ? 1 : 0) != 0;
  cfg.fov = std::clamp(ini.get_int("video", "fov", cfg.fov), 50, 200);
  cfg.own_camera = ini.get_int("video", "own_camera", cfg.own_camera ? 1 : 0) != 0;
  cfg.hud_size = std::clamp(ini.get_int("video", "hud_size", cfg.hud_size), 30, 100);
  {
    int ms = ini.get_int("video", "antialiasing", cfg.msaa);
    cfg.msaa = ms >= 8 ? 8 : ms >= 4 ? 4 : ms >= 2 ? 2 : 0;
  }
  cfg.anisotropy = std::clamp(ini.get_int("video", "anisotropy", cfg.anisotropy), 0, 16);
  {
    std::string d = ini.get("video", "display", cfg.display);
    if (d == "window" || d == "windowed") cfg.display = "windowed";
    else if (d == "borderless" || d == "noborder" || d == "fullscreen_window") cfg.display = "borderless";
    else if (d == "fullscreen" || d == "exclusive") cfg.display = "fullscreen";
    int w = 0, h = 0;
    if (sscanf(ini.get("video", "window_size", "").c_str(), "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) {
      cfg.window_w = w;
      cfg.window_h = h;
    }
    cfg.window_resize = ini.get_int("video", "window_resize", 1) != 0;
    cfg.background_run = ini.get("video", "background", "pause") == "run";
    std::string v = ini.get("video", "vsync", "game");
    cfg.vsync = (v == "on" || v == "1") ? 1 : (v == "off" || v == "0") ? 0 : -1;
    cfg.fps_limit = std::clamp(ini.get_int("video", "fps_limit", 0), 0, 1000);
  }
  {
    // player= is whose buttons are by default (0 = the mouse is off); a
    // button's own "2:attack" names its player.
    int who = std::clamp(ini.get_int("mouse", "player", 1), 0, 2);
    cfg.mouse_on = who != 0;
    static const char* kKeys[5] = {"left", "right", "middle", "x1", "x2"};
    for (int i = 0; i < 5; ++i) {
      cfg.mouse_player[i] = who ? who : 1;
      if (!ini.has("mouse", kKeys[i])) continue;
      std::string v = ini.get("mouse", kKeys[i], "");
      if (v.size() > 2 && (v[0] == '1' || v[0] == '2') && v[1] == ':') {
        cfg.mouse_player[i] = v[0] - '0';
        v = v.substr(2);
      }
      cfg.mouse_slot[i] = mouse_action_slot(v);
    }
  }
  cfg.movie_follow = ini.get_int("audio", "movie_follow", 1) != 0;
  cfg.movie_level = std::clamp(ini.get_int("audio", "movie_level", -1), -1, 4);
  cfg.movie_volume = std::clamp(ini.get_int("audio", "movie_volume", cfg.movie_volume), 0, 100);
  cfg.intro_skip = ini.get("audio", "intro", "play") == "skip";

  {
    cfg.debug_window = ini.get_int("ui", "debug_window", 0) != 0;
    cfg.nametags = ini.get_int("ui", "nametags", 1) != 0;
    std::string label = ini.get("ui", "menu_label", "", /*lower=*/false);
    // "NETWORK" was the default before 0.6: keep upgraded inis on the new one.
    if (!label.empty() && label != "NETWORK") cfg.menu_label = label;
  }

  cfg.pad = std::clamp(ini.get_int("client", "pad", cfg.pad), -1, 3);
  cfg.keyboard = ini.get_int("client", "keyboard", cfg.keyboard ? 1 : 0) != 0;
  cfg.send_hz = std::clamp(ini.get_int("client", "send_hz", cfg.send_hz), 30, 250);
  cfg.stick_deadzone = std::clamp(ini.get_int("client", "stick_deadzone", cfg.stick_deadzone), 1000, 32000);

  for (int i = 0; i < tmnp::kNumSlots; ++i) {
    const char* action = tmnp::kSlotNames[i];
    if (!action) continue;
    if (ini.has("pad", action)) {
      cfg.pad_map[i] = 0;
      for (auto& b : split_list(ini.get("pad", action, "")))
        cfg.pad_map[i] |= parse_pad_button(b);
    }
    if (ini.has("keys", action)) {
      cfg.key_map[i].clear();
      for (auto& k : split_list(ini.get("keys", action, ""))) {
        int vk = parse_vk(k);
        if (vk > 0) cfg.key_map[i].push_back(vk);
      }
    }
  }
  return true;
}

bool ini_set(const std::wstring& path, const char* section, const char* key, const std::string& value) {
  std::string text;
  HANDLE in = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (in != INVALID_HANDLE_VALUE) {
    DWORD size = GetFileSize(in, nullptr), got = 0;
    if (size != INVALID_FILE_SIZE) {
      text.resize(size);
      ReadFile(in, text.data(), size, &got, nullptr);
      text.resize(got);
    }
    CloseHandle(in);
  }
  const std::string eol = text.find("\r\n") != std::string::npos || text.empty() ? "\r\n" : "\n";
  std::vector<std::string> lines;
  for (size_t p = 0; p < text.size();) {
    size_t e = text.find('\n', p);
    std::string line = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
    if (e == std::string::npos) break;
    p = e + 1;
  }
  auto lower = [](std::string s) {
    for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
  };
  auto trim = [](const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
  };
  const std::string want_section = lower(section), want_key = lower(key);
  bool in_section = false, done = false;
  int section_end = -1;  // where a missing key goes: after the section's last line
  for (size_t i = 0; i < lines.size() && !done; ++i) {
    std::string t = trim(lines[i]);
    if (!t.empty() && t[0] == '[') {
      size_t close = t.find(']');
      in_section = close != std::string::npos && lower(trim(t.substr(1, close - 1))) == want_section;
      if (in_section) section_end = (int)i;
      continue;
    }
    if (!in_section) continue;
    if (!t.empty()) section_end = (int)i;
    size_t eq = t.find('=');
    if (t.empty() || t[0] == ';' || eq == std::string::npos || lower(trim(t.substr(0, eq))) != want_key) continue;
    // Keep the inline comment where it was.
    const std::string old = lines[i];
    size_t oeq = old.find('='), semi = old.find(';', oeq);
    std::string line = old.substr(0, oeq + 1) + value;
    if (semi != std::string::npos) {
      line.append(line.size() + 1 < semi ? semi - line.size() : 1, ' ');
      line += old.substr(semi);
    }
    lines[i] = line;
    done = true;
  }
  if (!done) {
    std::string line = std::string(key) + "=" + value;
    if (section_end >= 0) {
      lines.insert(lines.begin() + section_end + 1, line);
    } else {
      if (!lines.empty() && !lines.back().empty()) lines.push_back("");
      lines.push_back(std::string("[") + section + "]");
      lines.push_back(line);
    }
  }
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    out += lines[i];
    if (i + 1 < lines.size() || (!text.empty() && text.back() == '\n')) out += eol;
  }
  std::wstring tmp = path + L".tmp";
  HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD n = 0;
  BOOL ok = WriteFile(f, out.data(), (DWORD)out.size(), &n, nullptr) && n == out.size();
  CloseHandle(f);
  if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

namespace {
struct MouseAction {
  const char* name;
  int slot;
};
const MouseAction kMouseActions[] = {{"attack", 6},  {"strong", 7},          {"jump", 5},
                                     {"shuriken", 4}, {"shuriken_change", 3}, {"dash", 0},
                                     {"juggle", 1},  {"special", 2},         {"start", 11},
                                     {"pause", 8},   {"move_up", 12},        {"move_right", 13},
                                     {"move_down", 14}, {"move_left", 15}};
}  // namespace

int mouse_action_slot(const std::string& name) {
  for (const MouseAction& a : kMouseActions)
    if (name == a.name) return a.slot;
  return -1;
}

const char* mouse_action_name(int slot) {
  for (const MouseAction& a : kMouseActions)
    if (slot == a.slot) return a.name;
  return "";
}

std::string mouse_button_value(int player, int slot) {
  const char* name = mouse_action_name(slot);
  if (!*name) return "";
  return std::to_string(player == 2 ? 2 : 1) + ":" + name;
}
