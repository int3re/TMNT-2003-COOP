#include "mod/quality.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <string>

#include "common/log.h"
#include "mod/patch.h"

namespace quality {
namespace {

using SetSamplerState_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
constexpr int kVtSetSamplerState = 69;  // IDirect3DDevice9
constexpr DWORD kStages = 8;
SetSamplerState_t oSetSamplerState = nullptr;

int g_want_msaa = 0;   // [video] antialiasing
int g_msaa = 0;        // the device's
int g_supported = -1;  // the levels the adapter has for the game's formats (bit n: n samples; -1 not known)
int g_want_aniso = 0;  // [video] anisotropy
int g_max_aniso = 0;   // the device's
int g_aniso = 0;       // in use (1 or less: the game's filtering)
IDirect3DDevice9* g_dev = nullptr;

// RenderWare's video mode change, 0x4c4fb0(mode): it releases its video
// memory, resets the device with its parameters for that mode (the Reset hook
// adds the anti-aliasing) and restores everything, as for the game's own
// resolution change (ПАРАМЕТРЫ ГРАФИКИ). It leaves the mode it already has
// alone ([current] at +0x2E), so that one is hidden from it for the call.
constexpr const char* kChangeModeSig =
    "A1 ?? ?? ?? ?? 83 EC 48 85 C0 53 55 56 57 0F 84 ?? ?? ?? ?? 8B 6C 24 5C 85 ED 0F 8C ?? ?? ?? ?? 3B 2D ?? ?? ?? ?? "
    "0F 8D ?? ?? ?? ?? 39 2D ?? ?? ?? ?? 0F 84";
constexpr size_t kChangeModeCurrent = 0x2E;
using ChangeMode_t = int(__cdecl*)(int mode);
ChangeMode_t g_change_mode = nullptr;
int32_t* g_rw_mode = nullptr;

// The level a wish gives on this adapter: the highest it has at or below it.
int level_for(int want) {
  for (int n = 8; n >= 2; n /= 2)
    if (n <= want && g_supported > 0 && (g_supported & (1 << n))) return n;
  return 0;
}

HRESULT STDMETHODCALLTYPE hkSetSamplerState(IDirect3DDevice9* dev, DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) {
  if (g_aniso > 1 && dev == g_dev) {
    if (t == D3DSAMP_MINFILTER && v == D3DTEXF_LINEAR)
      v = D3DTEXF_ANISOTROPIC;
    else if (t == D3DSAMP_MIPFILTER && v == D3DTEXF_POINT)
      v = D3DTEXF_LINEAR;  // between mip levels too
    else if (t == D3DSAMP_MAXANISOTROPY)
      v = std::max<DWORD>(v, (DWORD)g_aniso);
  }
  return oSetSamplerState(dev, s, t, v);
}

// The stages as they are now (the game sets a filter only when it changes).
void apply_now() {
  if (!g_dev || !oSetSamplerState) return;
  for (DWORD s = 0; s < kStages; ++s) {
    DWORD min = 0, mip = 0;
    if (FAILED(g_dev->GetSamplerState(s, D3DSAMP_MINFILTER, &min)) ||
        FAILED(g_dev->GetSamplerState(s, D3DSAMP_MIPFILTER, &mip)))
      return;
    oSetSamplerState(g_dev, s, D3DSAMP_MAXANISOTROPY, g_aniso > 1 ? (DWORD)g_aniso : 1);
    if (g_aniso > 1) {
      if (min == D3DTEXF_LINEAR) oSetSamplerState(g_dev, s, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
      if (mip == D3DTEXF_POINT) oSetSamplerState(g_dev, s, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    } else if (min == D3DTEXF_ANISOTROPIC) {
      oSetSamplerState(g_dev, s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    }
  }
}

// The device reset now with the anti-aliasing wished for. RenderWare thinks
// it runs a full screen (display.cpp makes the window) and fits the window to
// the mode after it: the window's place and size are put back.
bool reset_now() {
  if (!g_change_mode || !g_rw_mode || !g_dev) return false;
  int mode = *g_rw_mode;
  if (mode < 0) return false;
  D3DDEVICE_CREATION_PARAMETERS cp{};
  HWND wnd = SUCCEEDED(g_dev->GetCreationParameters(&cp)) ? cp.hFocusWindow : nullptr;
  RECT before{}, after{};
  bool had = wnd && GetWindowRect(wnd, &before);
  *g_rw_mode = -1;
  int ok = g_change_mode(mode);
  if (*g_rw_mode == -1) *g_rw_mode = mode;  // (not done: its mode as it was)
  if (had && GetWindowRect(wnd, &after) &&
      (after.left != before.left || after.top != before.top || after.right != before.right ||
       after.bottom != before.bottom))
    SetWindowPos(wnd, nullptr, before.left, before.top, before.right - before.left, before.bottom - before.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
  LOGI("anti-aliasing: the device reset at once%s, %dx now", ok ? "" : " FAILED", g_msaa);
  return ok != 0;
}

}  // namespace

void install(const Config& cfg) {
  g_want_msaa = cfg.msaa;
  g_want_aniso = cfg.anisotropy;
  if (uintptr_t a = patch::find("rw_change_video_mode", kChangeModeSig)) {
    g_rw_mode = reinterpret_cast<int32_t*>(patch::read<uint32_t>(a + kChangeModeCurrent));
    if (patch::in_image((uintptr_t)g_rw_mode, 4)) g_change_mode = reinterpret_cast<ChangeMode_t>(a);
    else g_rw_mode = nullptr;
  }
  if (!g_change_mode) LOGW("picture quality: anti-aliasing changes from the menu need a new start");
  if (cfg.msaa || cfg.anisotropy)
    LOGI("picture quality: anti-aliasing %dx, anisotropic filtering %dx asked", cfg.msaa, cfg.anisotropy);
}

bool adjust_present(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, D3DPRESENT_PARAMETERS& pp) {
  if (!d3d || pp.MultiSampleType != D3DMULTISAMPLE_NONE) return false;
  D3DFORMAT bb = pp.BackBufferFormat;
  if (bb == D3DFMT_UNKNOWN) {  // windowed: the desktop's
    D3DDISPLAYMODE m{};
    if (SUCCEEDED(d3d->GetAdapterDisplayMode(adapter, &m))) bb = m.Format;
  }
  g_supported = 0;
  for (int n = 2; n <= 8; n *= 2) {
    auto t = (D3DMULTISAMPLE_TYPE)n;
    if (SUCCEEDED(d3d->CheckDeviceMultiSampleType(adapter, type, bb, pp.Windowed, t, nullptr)) &&
        (!pp.EnableAutoDepthStencil ||
         SUCCEEDED(d3d->CheckDeviceMultiSampleType(adapter, type, pp.AutoDepthStencilFormat, pp.Windowed, t, nullptr))))
      g_supported |= 1 << n;
  }
  int n = level_for(g_want_msaa);
  if (g_want_msaa >= 2 && n != g_want_msaa) {
    static int told = -1;
    if (told != g_want_msaa) LOGW("anti-aliasing: %dx asked, the adapter has %s", g_want_msaa, n ? "less" : "none");
    told = g_want_msaa;
  }
  if (!n) return false;
  pp.MultiSampleType = (D3DMULTISAMPLE_TYPE)n;
  pp.MultiSampleQuality = 0;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;  // (the only one multisampling allows)
  pp.Flags &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
  return true;
}

void on_device(IDirect3DDevice9* dev, const D3DPRESENT_PARAMETERS& pp) {
  g_msaa = pp.MultiSampleType >= D3DMULTISAMPLE_2_SAMPLES ? (int)pp.MultiSampleType : 0;
  if (!dev) return;
  g_dev = dev;
  D3DCAPS9 caps{};
  g_max_aniso = SUCCEEDED(dev->GetDeviceCaps(&caps)) && (caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC)
                    ? (int)caps.MaxAnisotropy
                    : 0;
  if (!oSetSamplerState) {
    void** vt = *reinterpret_cast<void***>(dev);
    patch::hook("IDirect3DDevice9::SetSamplerState", (uintptr_t)vt[kVtSetSamplerState], (void*)&hkSetSamplerState,
                (void**)&oSetSamplerState);
  }
  if (g_msaa) dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
  if (g_msaa)
    LOGI("picture quality: anti-aliasing %dx, anisotropic filtering up to %dx", g_msaa, g_max_aniso);
  else
    LOGI("picture quality: no anti-aliasing, anisotropic filtering up to %dx", g_max_aniso);
  set_anisotropy(g_want_aniso);  // (a reset puts the device's defaults back)
}

int msaa() { return g_msaa; }

void set_anisotropy(int level) {
  g_want_aniso = level;
  int now = level > 1 ? std::min(level, g_max_aniso) : 0;
  if (now != g_aniso && g_dev) LOGI("anisotropic filtering: %s", now > 1 ? std::to_string(now).c_str() : "the game's");
  g_aniso = now;
  apply_now();
}

std::atomic<int> g_requested{-1};

void request_msaa(int samples) { g_requested = samples; }

void poll() {
  int n = g_requested.exchange(-1);
  if (n < 0) return;
  set_anisotropy(n ? 16 : 0);
  LOGI("debug: anti-aliasing %dx asked, %dx in use", n, set_msaa(n));
}

int set_msaa(int samples) {
  g_want_msaa = samples;
  if (g_supported < 0) return -1;
  if (level_for(samples) == g_msaa) return g_msaa;
  return reset_now() ? g_msaa : -1;
}

}  // namespace quality
