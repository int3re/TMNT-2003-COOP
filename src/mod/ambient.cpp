#include "mod/ambient.h"

#include <windows.h>
#include <d3d9.h>

#include "common/log.h"

namespace ambient {
namespace {

bool g_enabled = true;
bool g_pillarboxed = false;  // set by the Im2D hook during the current frame
IDirect3DDevice9* g_dev = nullptr;
// Two steps down (1/8, then 1/32 of the frame): StretchRect's linear filter
// only averages neighbours, so one big step would shimmer instead of blur.
IDirect3DTexture9* g_mid = nullptr;
IDirect3DTexture9* g_small = nullptr;
IDirect3DSurface9* g_plain = nullptr;  // a multisampled frame resolved (StretchRect may not shrink it)
UINT g_w = 0, g_h = 0;
bool g_failed = false;  // the device cannot do it: stay with black bars

struct Vertex {
  float x, y, z, rhw;
  D3DCOLOR color;
  float u, v;
};
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
constexpr D3DCOLOR kShade = D3DCOLOR_XRGB(110, 110, 110);  // darker than the picture itself

bool make_targets(IDirect3DDevice9* dev, UINT w, UINT h) {
  release();
  UINT mw = w / 8 ? w / 8 : 1, mh = h / 8 ? h / 8 : 1, sw = w / 32 ? w / 32 : 1, sh = h / 32 ? h / 32 : 1;
  if (FAILED(dev->CreateTexture(mw, mh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_mid,
                                nullptr)) ||
      FAILED(dev->CreateTexture(sw, sh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_small,
                                nullptr))) {
    release();
    return false;
  }
  g_dev = dev;
  g_w = w;
  g_h = h;
  return true;
}

bool blur_frame(IDirect3DDevice9* dev, IDirect3DSurface9* bb, const RECT& picture) {
  IDirect3DSurface9 *mid = nullptr, *tiny = nullptr;
  bool ok = SUCCEEDED(g_mid->GetSurfaceLevel(0, &mid)) && SUCCEEDED(g_small->GetSurfaceLevel(0, &tiny)) &&
            SUCCEEDED(dev->StretchRect(bb, &picture, mid, nullptr, D3DTEXF_LINEAR)) &&
            SUCCEEDED(dev->StretchRect(mid, nullptr, tiny, nullptr, D3DTEXF_LINEAR));
  if (mid) mid->Release();
  if (tiny) tiny->Release();
  return ok;
}

// The blurred picture, scaled to cover the whole frame (cropped top and
// bottom), drawn only where the bars are.
void draw_bars(IDirect3DDevice9* dev, float W, float H, float bar) {
  float pic_w = W, pic_h = W * 3.0f / 4.0f;  // the 4:3 picture scaled to the frame width
  float v0 = (pic_h - H) * 0.5f / pic_h, v1 = 1.0f - v0;
  auto quad = [&](float x0, float x1, Vertex* q) {
    float u0 = x0 / pic_w, u1 = x1 / pic_w;
    q[0] = {x0 - 0.5f, -0.5f, 0, 1, kShade, u0, v0};
    q[1] = {x1 - 0.5f, -0.5f, 0, 1, kShade, u1, v0};
    q[2] = {x0 - 0.5f, H - 0.5f, 0, 1, kShade, u0, v1};
    q[3] = {x1 - 0.5f, H - 0.5f, 0, 1, kShade, u1, v1};
  };
  Vertex left[4], right[4];
  quad(0, bar, left);
  quad(W - bar, W, right);

  IDirect3DStateBlock9* saved = nullptr;
  if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved))) return;
  if (SUCCEEDED(dev->BeginScene())) {
    D3DVIEWPORT9 vp{0, 0, (DWORD)W, (DWORD)H, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    dev->SetPixelShader(nullptr);
    dev->SetVertexShader(nullptr);
    dev->SetFVF(kFvf);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetTexture(0, g_small);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, left, sizeof(Vertex));
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, right, sizeof(Vertex));
    dev->EndScene();
  }
  saved->Apply();
  saved->Release();
}

}  // namespace

void install(const Config& cfg) {
  g_enabled = cfg.side_bars_blur && cfg.widescreen && cfg.fullscreen_pillarbox;
}

void note_pillarbox() { g_pillarboxed = true; }

void before_present(IDirect3DDevice9* dev) {
  bool needed = g_pillarboxed;
  g_pillarboxed = false;
  if (!g_enabled || !needed || g_failed || !dev) return;
  IDirect3DSurface9* bb = nullptr;
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
  D3DSURFACE_DESC d{};
  bb->GetDesc(&d);
  float W = (float)d.Width, H = (float)d.Height, w43 = H * 4.0f / 3.0f;
  if (W > w43 + 2.0f) {
    if (dev != g_dev || d.Width != g_w || d.Height != g_h || !g_mid) {
      if (!make_targets(dev, d.Width, d.Height)) {
        g_failed = true;
        LOGW("side bars: cannot create render targets, they stay black");
      }
    }
    LONG bar = (LONG)((W - w43) * 0.5f);
    RECT picture{bar, 0, (LONG)d.Width - bar, (LONG)d.Height};
    IDirect3DSurface9* from = bb;
    if (!g_failed && d.MultiSampleType != D3DMULTISAMPLE_NONE) {
      if (!g_plain && FAILED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                     &g_plain, nullptr)))
        g_plain = nullptr;
      if (g_plain && SUCCEEDED(dev->StretchRect(bb, nullptr, g_plain, nullptr, D3DTEXF_NONE))) from = g_plain;
    }
    if (!g_failed) {
      if (blur_frame(dev, from, picture)) {
        draw_bars(dev, W, H, (float)bar);
      } else {
        g_failed = true;
        LOGW("side bars: StretchRect from the back buffer failed, they stay black");
      }
    }
  }
  bb->Release();
}

void release() {
  if (g_plain) g_plain->Release();
  g_plain = nullptr;
  if (g_mid) g_mid->Release();
  if (g_small) g_small->Release();
  g_mid = g_small = nullptr;
  g_dev = nullptr;
  g_w = g_h = 0;
}

void set_blur(bool on) {
  g_enabled = on;
  g_failed = false;
}

}  // namespace ambient
