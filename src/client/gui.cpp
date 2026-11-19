#include "client/gui.h"

#include <windows.h>
#include <d3d9.h>

#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "common/joincode.h"
#include "common/log.h"
#include "common/protocol.h"
#include "common/screenshot.h"
#include "common/steam_boot.h"
#include "common/version.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace gui {
namespace {

IDirect3D9* g_d3d = nullptr;
IDirect3DDevice9* g_dev = nullptr;
D3DPRESENT_PARAMETERS g_pp{};
UINT g_resize_w = 0, g_resize_h = 0;
ImFont* g_bold = nullptr;
ImFont* g_mono = nullptr;

const ImVec4 kGreen(0.36f, 0.72f, 0.24f, 1.0f);
const ImVec4 kAmber(0.98f, 0.75f, 0.25f, 1.0f);
const ImVec4 kMuted(0.70f, 0.72f, 0.68f, 1.0f);

bool create_device(HWND hwnd) {
  if (!(g_d3d = Direct3DCreate9(D3D_SDK_VERSION))) return false;
  g_pp.Windowed = TRUE;
  g_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  g_pp.BackBufferFormat = D3DFMT_UNKNOWN;
  g_pp.EnableAutoDepthStencil = FALSE;
  g_pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;  // vsync: the UI needs no more
  return SUCCEEDED(g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                       D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_pp, &g_dev)) ||
         SUCCEEDED(g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_pp, &g_dev));
}

void reset_device() {
  ImGui_ImplDX9_InvalidateDeviceObjects();
  if (g_dev->Reset(&g_pp) == D3D_OK) ImGui_ImplDX9_CreateDeviceObjects();
}

LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return TRUE;
  switch (msg) {
    case WM_SIZE:
      if (wp != SIZE_MINIMIZED) {
        g_resize_w = LOWORD(lp);
        g_resize_h = HIWORD(lp);
      }
      return 0;
    case WM_SYSCOMMAND:
      if ((wp & 0xfff0) == SC_KEYMENU) return 0;  // no Alt menu beep
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void setup_style(float scale) {
  ImGuiIO& io = ImGui::GetIO();
  char windir[MAX_PATH];
  GetWindowsDirectoryA(windir, MAX_PATH);
  std::string fonts = std::string(windir) + "\\Fonts\\";
  float size = std::round(18.0f * scale);
  ImFont* regular = io.Fonts->AddFontFromFileTTF((fonts + "segoeui.ttf").c_str(), size);
  if (!regular) regular = io.Fonts->AddFontDefault();
  g_bold = io.Fonts->AddFontFromFileTTF((fonts + "segoeuib.ttf").c_str(), size);
  g_mono = io.Fonts->AddFontFromFileTTF((fonts + "consolab.ttf").c_str(), size);
  if (!g_bold) g_bold = regular;
  if (!g_mono) g_mono = regular;

  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 0.0f;
  s.FrameRounding = 6.0f;
  s.WindowPadding = ImVec2(20, 16);
  s.FramePadding = ImVec2(12, 6);
  s.ItemSpacing = ImVec2(10, 8);
  s.ScaleAllSizes(scale);
  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = ImVec4(0.95f, 0.96f, 0.92f, 1.0f);
  c[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.08f, 0.05f, 1.0f);
  c[ImGuiCol_Button] = ImVec4(0.20f, 0.45f, 0.14f, 1.0f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.60f, 0.20f, 1.0f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.15f, 0.34f, 0.10f, 1.0f);
  c[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.18f, 0.11f, 1.0f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.26f, 0.14f, 1.0f);
  c[ImGuiCol_Separator] = ImVec4(0.36f, 0.72f, 0.24f, 0.35f);
  c[ImGuiCol_Header] = ImVec4(0.20f, 0.45f, 0.14f, 0.6f);
  c[ImGuiCol_NavCursor] = kAmber;
}

void section(const char* title) {
  ImGui::Spacing();
  ImGui::PushFont(g_bold, 0.0f);
  ImGui::TextColored(kGreen, "%s", title);
  ImGui::PopFont();
  ImGui::Separator();
}

void dot(const ImVec4& color) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  float r = ImGui::GetFontSize() * 0.28f;
  ImGui::GetWindowDrawList()->AddCircleFilled(
      ImVec2(p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f), r, ImGui::GetColorU32(color));
  ImGui::Dummy(ImVec2(r * 2.4f, ImGui::GetTextLineHeight()));
  ImGui::SameLine();
}

struct UiState {
  char code[64] = "";
  std::string code_error;
  bool saved_last = false;
  std::string last;  // [client] last= from tmntnet.ini
};

void draw(ClientCore& core, const ClientSnapshot& s, UiState& ui) {
  const Config& cfg = core.config();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::Begin("##client", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

  ImGui::PushFont(g_bold, ImGui::GetStyle().FontSizeBase * 1.35f);
  ImGui::TextUnformatted("TMNT Netplay: второй игрок");
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::TextColored(kMuted, "  tmntnet " TMNTNET_VERSION);

  bool connected = s.state == ClientState::Connected;
  bool busy = s.state == ClientState::Connecting || s.state == ClientState::Handshake ||
              s.state == ClientState::Waiting || s.state == ClientState::Discovering;
  dot(connected ? kGreen : busy ? kAmber : kMuted);
  if (connected)
    ImGui::Text("Подключён к игре «%s»   пинг %u мс%s", s.host_name.c_str(), s.rtt_ms,
                s.via == "Steam" ? "   через Steam" : "");
  else if (busy)
    ImGui::Text("Подключаемся к %s…", s.server.c_str());
  else
    ImGui::Text("Не подключён");
  if (!s.error.empty()) ImGui::TextColored(kAmber, "%s", s.error.c_str());

  if (connected && !ui.saved_last) {  // remember the game for next time
    std::wstring ini = module_dir(nullptr) + L"tmntnet.ini";
    std::wstring value(s.last_key.begin(), s.last_key.end());
    WritePrivateProfileStringW(L"client", L"last", value.c_str(), ini.c_str());
    ui.last = s.last_key;
    ui.saved_last = true;
  }
  if (!connected) ui.saved_last = false;

  if (connected) {
    section("Ваше управление (вы — игрок 2)");
    if (!s.pad.empty())
      ImGui::Text("Геймпад: %s", s.pad.c_str());
    else
      ImGui::TextColored(kAmber, "Геймпад не найден. Подключите его — он подхватится сам.");
    if (cfg.keyboard)
      ImGui::TextColored(kMuted, "Клавиатура: I J K L — движение, Z прыжок, A атака, X сильная атака, "
                                 "T старт (раскладка в tmntnet.ini)");
    ImGui::Spacing();
    ImGui::TextColored(kMuted, "Нажмите кнопки — здесь видно, что уходит в игру:");
    static const char* kLabels[tmnp::kNumSlots] = {
        "рывок", "удар в воздухе", "спецприём", "смена сюрикена", "сюрикен", "прыжок",
        "атака", "сильная атака", "пауза", nullptr, nullptr, "старт",
        "вверх", "вправо", "вниз", "влево"};
    float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    bool first = true;
    for (int i = 0; i < tmnp::kNumSlots; ++i) {
      if (!kLabels[i]) continue;
      bool on = (s.buttons >> i) & 1;
      float w = ImGui::CalcTextSize(kLabels[i]).x + ImGui::GetStyle().FramePadding.x * 2;
      if (!first) {  // wrap the row instead of running off the window
        ImGui::SameLine();
        if (ImGui::GetCursorScreenPos().x + w > right) ImGui::NewLine();
      }
      first = false;
      ImGui::PushStyleColor(ImGuiCol_Button, on ? kGreen : ImVec4(0.12f, 0.18f, 0.11f, 1.0f));
      ImGui::SmallButton(kLabels[i]);
      ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    if (ImGui::Button("Отключиться")) core.disconnect();
  } else {
    section("Игры в Steam");
    if (!s.steam) {
      ImGui::TextColored(kMuted, "Steam выключен: %s.",
                         s.steam_note.empty() ? "transport=enet в tmntnet.ini" : s.steam_note.c_str());
    } else if (s.lobbies.empty()) {
      ImGui::TextColored(kMuted, "Вы в Steam: %s. Ищем… Хост должен запустить игру с модом при "
                                 "включённом Steam, и вы должны быть друзьями в Steam.",
                         s.steam_name.c_str());
    } else if (ImGui::BeginTable("steam", 3, ImGuiTableFlags_SizingFixedFit)) {
      for (size_t i = 0; i < s.lobbies.size(); ++i) {
        const lobby::Found& f = s.lobbies[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(f.host.empty() ? f.name.c_str() : f.host.c_str());
        ImGui::TableNextColumn();
        ImGui::TextColored(kMuted, "%s%s%s", f.is_friend ? "друг" : "публичное лобби",
                           f.name.empty() ? "" : ", ПК ", f.name.c_str());
        ImGui::TableNextColumn();
        ImGui::PushID((int)(1000 + i));
        if (!f.compatible)
          ImGui::TextColored(kAmber, "%s", f.why_not.c_str());
        else if (f.state == "ingame")
          ImGui::TextColored(kMuted, "уже играет вдвоём");
        else if (ImGui::Button("Подключиться"))
          core.join_lobby(f.lobby);
        ImGui::PopID();
      }
      ImGui::EndTable();
    }

    section("Игры в локальной сети");
    if (s.lan.empty()) {
      ImGui::TextColored(kMuted, "Ищем… Хост должен запустить игру с модом. Если вы не в одной "
                                 "сети, подключитесь по коду ниже.");
    } else if (ImGui::BeginTable("lan", 3, ImGuiTableFlags_SizingFixedFit)) {
      for (size_t i = 0; i < s.lan.size(); ++i) {
        const FoundHost& h = s.lan[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(h.name.c_str());
        ImGui::TableNextColumn();
        ImGui::TextColored(kMuted, "%s", h.addr.str().c_str());
        ImGui::TableNextColumn();
        ImGui::PushID((int)i);
        if (h.version != tmnp::kVersion)
          ImGui::TextColored(kAmber, "другая версия tmntnet");
        else if (ImGui::Button("Подключиться"))
          core.connect(h.addr);
        ImGui::PopID();
      }
      ImGui::EndTable();
    }

    section("Подключиться по коду, адресу или SteamID");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 19);
    bool enter = ImGui::InputTextWithHint("##code", "TMNT-XXXX-XXX, 192.168.1.10 или SteamID", ui.code,
                                          sizeof(ui.code), ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemEdited()) ui.code_error.clear();
    ImGui::SameLine();
    if (ImGui::Button("Подключиться##code") || enter) {
      uint32_t ip = 0;
      uint16_t port = 0;
      net::Addr a;
      if (uint64_t lobby = steam::parse_lobby_id(ui.code)) {  // a Steam lobby id
        core.join_lobby(lobby);
      } else if (uint64_t id = parse_steamid(ui.code)) {  // SteamID64 or a profile link
        core.connect_steam(id);
      } else if (joincode::decode(ui.code, cfg.port, ip, port)) {
        a.sa.sin_family = AF_INET;
        a.sa.sin_addr.s_addr = ip;
        a.sa.sin_port = htons(port);
        core.connect(a);
      } else if (ui.code[0] && net::resolve(ui.code, cfg.port, a)) {  // a computer name
        core.connect(a);
      } else {
        ui.code_error = "Не похоже на код, адрес или SteamID. Пример: TMNT-4KQ7-9XB";
      }
    }
    if (!ui.code_error.empty()) ImGui::TextColored(kAmber, "%s", ui.code_error.c_str());
    if (!ui.last.empty()) {
      ImGui::TextColored(kMuted, "Прошлая игра: %s", ui.last.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Подключиться##last")) {
        net::Addr a;
        uint32_t ip = 0;
        uint16_t port = 0;
        if (ui.last.rfind("steam:", 0) == 0) {
          if (uint64_t id = parse_steamid(ui.last)) core.connect_steam(id);
        } else if (joincode::decode(ui.last, cfg.port, ip, port)) {
          a.sa.sin_family = AF_INET;
          a.sa.sin_addr.s_addr = ip;
          a.sa.sin_port = htons(port);
          core.connect(a);
        }
      }
    }
    if (busy && ImGui::Button("Отмена")) core.disconnect();
  }

  ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(),
                                ImGui::GetWindowHeight() - ImGui::GetTextLineHeightWithSpacing() * 2.2f));
  ImGui::Separator();
  ImGui::TextColored(kMuted, "Картинку игры смотрите через Moonlight/Parsec. Геймпад работает, "
                             "даже когда это окно свёрнуто.");
  ImGui::End();
  core.set_typing(ImGui::GetIO().WantTextInput);
}

}  // namespace

int run(ClientCore& core, const std::wstring& shot_path, unsigned shot_delay_ms) {
  ImGui_ImplWin32_EnableDpiAwareness();
  float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(
      MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
  WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wnd_proc, 0, 0, GetModuleHandleW(nullptr), nullptr,
                 LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"tmntnet-client", nullptr};
  wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"TMNT Netplay — клиент", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, (int)(780 * scale), (int)(600 * scale),
                            nullptr, nullptr, wc.hInstance, nullptr);
  if (!create_device(hwnd)) {
    LOGE("Direct3D 9 is not available; use tmntnet-client.exe --nogui");
    MessageBoxW(nullptr, L"Не удалось создать окно (Direct3D 9). Запустите tmntnet-client.exe --nogui.",
                L"TMNT Netplay", MB_ICONERROR);
    return 1;
  }
  bool preview = !shot_path.empty();
  if (!preview) {
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
  } else {  // hidden: size the back buffer like the window would be
    RECT rc;
    GetClientRect(hwnd, &rc);
    g_pp.BackBufferWidth = rc.right - rc.left;
    g_pp.BackBufferHeight = rc.bottom - rc.top;
    g_dev->Reset(&g_pp);
  }
  uint32_t started = net::now_ms();

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  setup_style(scale);
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX9_Init(g_dev);

  UiState ui;
  {
    wchar_t last[128] = L"";
    GetPrivateProfileStringW(L"client", L"last", L"", last, 128,
                             (module_dir(nullptr) + L"tmntnet.ini").c_str());
    for (wchar_t* p = last; *p; ++p) ui.last += (char)*p;
  }

  bool done = false;
  while (!done) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) done = true;
    }
    if (done) break;
    if (g_resize_w && g_resize_h) {
      g_pp.BackBufferWidth = g_resize_w;
      g_pp.BackBufferHeight = g_resize_h;
      g_resize_w = g_resize_h = 0;
      reset_device();
    }
    if (!preview && IsIconic(hwnd)) {  // keep networking, skip drawing
      Sleep(50);
      continue;
    }
    ClientSnapshot snap = core.snapshot();
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw(core, snap, ui);
    ImGui::EndFrame();
    g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(13, 20, 13), 1.0f, 0);
    if (g_dev->BeginScene() >= 0) {
      ImGui::Render();
      ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
      g_dev->EndScene();
    }
    if (preview && net::now_ms() - started >= shot_delay_ms) {
      std::vector<uint8_t> px;
      unsigned w, h;
      bool ok = shot::capture(g_dev, px, w, h) && shot::save_png(shot_path, px, w, h);
      LOGI("window screenshot %s: %ls", ok ? "saved" : "FAILED", shot_path.c_str());
      break;
    }
    if (g_dev->Present(nullptr, nullptr, nullptr, nullptr) == D3DERR_DEVICELOST &&
        g_dev->TestCooperativeLevel() == D3DERR_DEVICENOTRESET)
      reset_device();
  }

  ImGui_ImplDX9_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  g_dev->Release();
  g_d3d->Release();
  DestroyWindow(hwnd);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  return 0;
}

}  // namespace gui
