#include "common/screenshot.h"

#include <windows.h>
#include <d3d9.h>
#include <wincodec.h>

#include <cstring>

namespace shot {

bool capture(IDirect3DDevice9* dev, std::vector<uint8_t>& px, unsigned& w, unsigned& h) {
  IDirect3DSurface9 *bb = nullptr, *rt = nullptr, *sys = nullptr;
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
  D3DSURFACE_DESC d;
  bb->GetDesc(&d);
  // 8 bits per channel, or 10 (RenderWare may pick A2R10G10B10 for "32-bit" modes).
  bool ten = d.Format == D3DFMT_A2R10G10B10 || d.Format == D3DFMT_A2B10G10R10;
  if (d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8 && !ten) {
    bb->Release();
    return false;
  }
  rt = bb;
  rt->AddRef();
  if (d.MultiSampleType != D3DMULTISAMPLE_NONE) {  // resolve MSAA first
    rt->Release();
    rt = nullptr;
    if (SUCCEEDED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0,
                                          FALSE, &rt, nullptr)))
      dev->StretchRect(bb, nullptr, rt, nullptr, D3DTEXF_NONE);
  }
  px.clear();
  D3DLOCKED_RECT lr;
  if (rt &&
      SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM,
                                                 &sys, nullptr)) &&
      SUCCEEDED(dev->GetRenderTargetData(rt, sys)) &&
      SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
    px.resize((size_t)d.Width * d.Height * 4);
    for (UINT y = 0; y < d.Height; ++y) {
      const uint8_t* row = (const uint8_t*)lr.pBits + (size_t)y * lr.Pitch;
      uint8_t* out = &px[(size_t)y * d.Width * 4];
      if (!ten) {
        std::memcpy(out, row, d.Width * 4);
        continue;
      }
      for (UINT x = 0; x < d.Width; ++x) {  // 10-bit channels to 8-bit BGRA
        uint32_t p;
        std::memcpy(&p, row + x * 4, 4);
        uint8_t hi = (uint8_t)((p >> 22) & 0xFF), mid = (uint8_t)((p >> 12) & 0xFF), lo = (uint8_t)((p >> 2) & 0xFF);
        bool rgb = d.Format == D3DFMT_A2R10G10B10;  // R in the high bits
        out[x * 4 + 0] = rgb ? lo : hi;  // B
        out[x * 4 + 1] = mid;            // G
        out[x * 4 + 2] = rgb ? hi : lo;  // R
      }
    }
    sys->UnlockRect();
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;  // X8R8G8B8 has no alpha
  }
  for (IDirect3DSurface9* s : {sys, rt, bb})
    if (s) s->Release();
  w = d.Width;
  h = d.Height;
  return !px.empty();
}

bool save_png(const std::wstring& path, const std::vector<uint8_t>& bgra, unsigned w, unsigned h) {
  HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IWICImagingFactory* f = nullptr;
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* enc = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
  bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&f))) &&
            SUCCEEDED(f->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
            SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
            SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
            SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h)) &&
            SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
            SUCCEEDED(frame->WritePixels(h, w * 4, (UINT)bgra.size(),
                                         const_cast<BYTE*>(bgra.data()))) &&
            SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
  for (IUnknown* u : {(IUnknown*)frame, (IUnknown*)enc, (IUnknown*)stream, (IUnknown*)f})
    if (u) u->Release();
  if (SUCCEEDED(co)) CoUninitialize();
  return ok;
}

}  // namespace shot
