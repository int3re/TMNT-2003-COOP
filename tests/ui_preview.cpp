// Renders tmntnet's in-game screens off-screen (hidden window, 1920x1080) with
// made-up network states and saves them as PNGs, to check the layout without
// running the game:   ui_preview.exe <output folder>
#include <windows.h>
#include <d3d9.h>

#include <imgui.h>
#include <imgui_impl_dx9.h>

#include <cstdio>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/log.h"
#include "common/screenshot.h"
#include "mod/net_host.h"
#include "mod/main_menu.h"
#include "mod/ui.h"

// ui.cpp reads the host state through these; the preview fakes them.
static HostStatus g_fake;
HostStatus host_status() { return g_fake; }
void host_kick(int) {}
void host_invite(uint64_t) {}

// The game-side modules ui.cpp talks to, faked for the preview.
#include "mod/native_menu.h"
namespace main_menu {
static Label g_fake_label;
static bool g_has_fake = false;
bool on_screen() { return g_has_fake; }
bool native() { return g_has_fake; }
Label label() { return g_fake_label; }
void debug_fake(const Label& l) {
  g_fake_label = l;
  g_has_fake = true;
}
}  // namespace main_menu
namespace native_menu {
bool active() { return false; }
bool covers_screen() { return false; }
void notify(const std::string&, const std::string&) {}
void set_badge(const std::string&) {}
void set_status(const std::string&, const std::string&) {}
std::string latin(const std::string& s) { return s; }
}  // namespace native_menu
#include "mod/lockstep.h"
namespace lockstep {
Waiting waiting() { return Waiting{}; }
bool starting_story() { return false; }
}  // namespace lockstep

namespace {

constexpr UINT kW = 1920, kH = 1080;

// The game's main menu layout (table 0x591400: sprite centres in a 640x480
// space, origin at the centre, y up), drawn with the same mapping tmntnet
// uses for its label, plus the new NETWORK row where the mod puts it.
struct Item {
  const char* text;
  int x, y, w;
};
const Item kItems[] = {{"STORY", -142, 182, 128},   {"VS", -142, 143, 128},
                       {"CHALLENGE", -78, 104, 256}, {"DATA", -142, 65, 128},
                       {"OPTION", -142, 26, 128},    {"SAVE", -174, -13, 64},
                       {"LOAD", -174, -52, 64},      {"QUIT", -82, -155, 128}};

void fake_game_background() {
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(kW, kH), IM_COL32(20, 34, 60, 255),
                              IM_COL32(20, 34, 60, 255), IM_COL32(8, 10, 18, 255),
                              IM_COL32(8, 10, 18, 255));
  float s = kH / 480.0f;
  for (const Item& it : kItems) {
    ImVec2 a(kW * 0.5f + (it.x - it.w * 0.5f) * s, kH * 0.5f - (it.y + 16) * s);
    ImVec2 b(kW * 0.5f + (it.x + it.w * 0.5f) * s, kH * 0.5f - (it.y - 16) * s);
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 50));  // the game's 128x32 label box
    dl->AddText(nullptr, 23.0f * s, ImVec2(a.x + 12 * s, a.y + 4 * s), IM_COL32(230, 230, 210, 255),
                it.text);
  }
  dl->AddRect(ImVec2((kW - kH * 4 / 3) / 2.0f, 0), ImVec2((kW + kH * 4 / 3) / 2.0f, kH - 1),
              IM_COL32(255, 255, 255, 40));
}

bool render_scene(IDirect3DDevice9* dev, const wchar_t* path) {
  ImGuiIO& io = ImGui::GetIO();
  for (int f = 0; f < 6; ++f) {  // auto-sized windows settle after a couple of frames
    io.DisplaySize = ImVec2((float)kW, (float)kH);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui_ImplDX9_NewFrame();
    ImGui::NewFrame();
    fake_game_background();
    ui::frame();
    ImGui::Render();
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    if (SUCCEEDED(dev->BeginScene())) {
      ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
      dev->EndScene();
    }
  }
  std::vector<uint8_t> px;
  unsigned w, h;
  bool ok = shot::capture(dev, px, w, h) && shot::save_png(path, px, w, h);
  wprintf(L"%ls %ls\n", ok ? L"saved" : L"FAILED", path);
  return ok;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::wstring out = argc > 1 ? argv[1] : L".";
  CreateDirectoryW(out.c_str(), nullptr);
  tlog::open(out + L"\\ui_preview.log", tlog::Info, false);

  HWND hwnd = CreateWindowW(L"STATIC", L"ui preview", WS_POPUP, 0, 0, kW, kH, nullptr, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
  IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp{};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferWidth = kW;
  pp.BackBufferHeight = kH;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  IDirect3DDevice9* dev = nullptr;
  if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev))) {
    printf("FAIL: no Direct3D 9 device\n");
    return 2;
  }
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui_ImplDX9_Init(dev);
  Config cfg;
  cfg.role = "host";
  ui::install(cfg);  // the main menu signature is absent here: only stores the config
  ui::setup_style();

  bool ok = true;
  g_fake.started = true;
  g_fake.running = true;
  g_fake.port = 7777;
  g_fake.name = "DESKTOP-HOST";
  g_fake.enet = true;
  g_fake.steam = true;
  g_fake.steam_name = "Leonardo";
  g_fake.steam_id = 76561198000000001ull;
  g_fake.lobby_id = 109775241234567890ull;
  g_fake.friends = {{76561198000000002ull, "Misha", true}, {76561198000000003ull, "Raph_Fan", false}};

  ui::debug_force_main_menu(true);
  main_menu::Label net;  // what main_menu::label() reports in the game
  net.visible = true;
  net.x = -142 - 64;  // sprite centre minus half its width
  net.y = -91;
  net.alpha = 1.0f;
  net.selected = true;
  main_menu::debug_fake(net);
  ok &= render_scene(dev, (out + L"\\1_main_menu.png").c_str());

  ui::debug_force_main_menu(false);
  ui::toggle();
  ok &= render_scene(dev, (out + L"\\2_network_waiting.png").c_str());

  g_fake.p2 = true;
  g_fake.p2_name = "Misha-PC";
  g_fake.p2_addr = "Steam 'Misha', relay";
  g_fake.p2_via = "Steam";
  g_fake.rtt_ms = 12;
  g_fake.p2_input_live = true;
  ok &= render_scene(dev, (out + L"\\3_network_connected.png").c_str());

  ui::toggle();
  ok &= render_scene(dev, (out + L"\\4_ingame_badge_toast.png").c_str());

  ImGui_ImplDX9_Shutdown();
  ImGui::DestroyContext();
  dev->Release();
  d3d->Release();
  return ok ? 0 : 1;
}
