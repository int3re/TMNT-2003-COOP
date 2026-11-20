#include "launcher/gfx.h"

#include <dwrite_1.h>
#include <wincodec.h>

#include <cmath>
#include <cstdlib>
#include <map>
#include <tuple>
#include <vector>

#include "launcher/resource.h"

namespace gfx {
namespace {

ID2D1Factory* g_d2d = nullptr;
IDWriteFactory* g_dw = nullptr;
IDWriteFontCollection* g_fonts = nullptr;  // the program's own faces; null: Windows' fonts
ID2D1StrokeStyle* g_round = nullptr;
ID2D1StrokeStyle* g_miter = nullptr;

// --- the fonts in the program's resources, as a DirectWrite collection --------

struct Blob {
  const void* data;
  UINT32 size;
};
std::vector<Blob> g_blobs;

class Stream final : public IDWriteFontFileStream {
 public:
  explicit Stream(Blob b) : b_(b) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteFontFileStream)) {
      *out = this;
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG n = InterlockedDecrement(&refs_);
    if (!n) delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE ReadFileFragment(const void** start, UINT64 offset, UINT64 size, void** context) override {
    *context = nullptr;
    if (offset > b_.size || size > b_.size - offset) {
      *start = nullptr;
      return E_FAIL;
    }
    *start = static_cast<const BYTE*>(b_.data) + offset;
    return S_OK;
  }
  void STDMETHODCALLTYPE ReleaseFileFragment(void*) override {}
  HRESULT STDMETHODCALLTYPE GetFileSize(UINT64* size) override {
    *size = b_.size;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetLastWriteTime(UINT64* t) override {
    *t = 0;
    return E_NOTIMPL;
  }

 private:
  LONG refs_ = 1;
  Blob b_;
};

// (The loaders live as long as the program: their counts are not kept.)
class FileLoader final : public IDWriteFontFileLoader {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteFontFileLoader)) {
      *out = this;
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE CreateStreamFromKey(const void* key, UINT32 key_size, IDWriteFontFileStream** stream) override {
    *stream = nullptr;
    if (key_size != sizeof(UINT32)) return E_INVALIDARG;
    UINT32 i = *static_cast<const UINT32*>(key);
    if (i >= g_blobs.size()) return E_INVALIDARG;
    *stream = new Stream(g_blobs[i]);
    return S_OK;
  }
};
FileLoader g_file_loader;

class Enumerator final : public IDWriteFontFileEnumerator {
 public:
  explicit Enumerator(IDWriteFactory* f) : factory_(f) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteFontFileEnumerator)) {
      *out = this;
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG n = InterlockedDecrement(&refs_);
    if (!n) delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE MoveNext(BOOL* has) override {
    ++at_;
    *has = at_ < (int)g_blobs.size();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetCurrentFontFile(IDWriteFontFile** file) override {
    UINT32 key = (UINT32)at_;
    return factory_->CreateCustomFontFileReference(&key, sizeof(key), &g_file_loader, file);
  }

 private:
  LONG refs_ = 1;
  IDWriteFactory* factory_;
  int at_ = -1;
};

class CollectionLoader final : public IDWriteFontCollectionLoader {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteFontCollectionLoader)) {
      *out = this;
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE CreateEnumeratorFromKey(IDWriteFactory* factory, const void*, UINT32,
                                                    IDWriteFontFileEnumerator** e) override {
    *e = new Enumerator(factory);
    return S_OK;
  }
};
CollectionLoader g_collection_loader;

// What the collection holds: (family, weight) of every font.
struct Known {
  std::wstring family;
  int weight;
};
std::vector<Known> g_known;

std::wstring family_name(IDWriteFontFamily* fam) {
  IDWriteLocalizedStrings* names = nullptr;
  std::wstring out;
  if (SUCCEEDED(fam->GetFamilyNames(&names)) && names) {
    UINT32 index = 0;
    BOOL found = FALSE;
    names->FindLocaleName(L"en-us", &index, &found);
    if (!found) index = 0;
    UINT32 len = 0;
    if (SUCCEEDED(names->GetStringLength(index, &len))) {
      out.resize(len + 1);
      names->GetString(index, &out[0], len + 1);
      out.resize(len);
    }
    names->Release();
  }
  return out;
}

void load_fonts() {
  for (int id = RES_FONT_FIRST; id <= RES_FONT_LAST; ++id) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const void* data = g ? LockResource(g) : nullptr;
    DWORD size = r ? SizeofResource(nullptr, r) : 0;
    if (data && size) g_blobs.push_back({data, size});
  }
  if (g_blobs.empty()) return;
  if (FAILED(g_dw->RegisterFontFileLoader(&g_file_loader))) return;
  if (FAILED(g_dw->RegisterFontCollectionLoader(&g_collection_loader))) return;
  UINT32 key = 1;
  if (FAILED(g_dw->CreateCustomFontCollection(&g_collection_loader, &key, sizeof(key), &g_fonts))) {
    g_fonts = nullptr;
    return;
  }
  for (UINT32 i = 0; i < g_fonts->GetFontFamilyCount(); ++i) {
    IDWriteFontFamily* fam = nullptr;
    if (FAILED(g_fonts->GetFontFamily(i, &fam))) continue;
    std::wstring name = family_name(fam);
    for (UINT32 k = 0; k < fam->GetFontCount(); ++k) {
      IDWriteFont* font = nullptr;
      if (FAILED(fam->GetFont(k, &font))) continue;
      if (font->GetSimulations() == DWRITE_FONT_SIMULATIONS_NONE) g_known.push_back({name, (int)font->GetWeight()});
      font->Release();
    }
    fam->Release();
  }
}

// The family and weight to ask DirectWrite for: the program's own font
// nearest to the weight wanted, else one of Windows'.
struct Pick {
  std::wstring family;
  int weight;
  bool own;
};

Pick pick(Face face, int weight) {
  const wchar_t* prefix = face == Face::Display ? L"Dela" : face == Face::Mono ? L"JetBrains" : L"Onest";
  const Known* best = nullptr;
  for (const Known& k : g_known) {
    if (k.family.compare(0, wcslen(prefix), prefix) != 0) continue;
    if (!best || std::abs(k.weight - weight) < std::abs(best->weight - weight)) best = &k;
  }
  if (best) return {best->family, best->weight, true};
  return {face == Face::Display ? L"Impact" : face == Face::Mono ? L"Consolas" : L"Segoe UI",
          face == Face::Display ? 400 : weight, false};
}

std::map<std::tuple<int, int, int>, IDWriteTextFormat*> g_formats;

IDWriteTextFormat* format(const Type& t) {
  auto key = std::make_tuple((int)t.face, (int)std::lround(t.size * 100.0f), t.weight);
  auto it = g_formats.find(key);
  if (it != g_formats.end()) return it->second;
  Pick p = pick(t.face, t.weight);
  IDWriteTextFormat* f = nullptr;
  g_dw->CreateTextFormat(p.family.c_str(), p.own ? g_fonts : nullptr, (DWRITE_FONT_WEIGHT)p.weight, DWRITE_FONT_STYLE_NORMAL,
                         DWRITE_FONT_STRETCH_NORMAL, t.size, L"ru-ru", &f);
  g_formats[key] = f;
  return f;
}

IDWriteTextLayout* layout(const std::wstring& s, float w, float h, const Type& t) {
  IDWriteTextFormat* f = format(t);
  if (!f) return nullptr;
  IDWriteTextLayout* l = nullptr;
  if (FAILED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, w, h, &l)) || !l) return nullptr;
  const DWRITE_TEXT_RANGE all{0, (UINT32)s.size()};
  l->SetTextAlignment(t.align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : t.align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                                              : DWRITE_TEXT_ALIGNMENT_LEADING);
  l->SetParagraphAlignment(t.valign == 1 ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : t.valign == 2 ? DWRITE_PARAGRAPH_ALIGNMENT_FAR
                                                                                         : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  l->SetWordWrapping(t.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
  if (t.line > 0.0f) {
    const float h_line = t.size * t.line;  // (the letters' middle on the line's middle)
    l->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, h_line, h_line * 0.5f + t.size * 0.35f);
  }
  if (t.spacing != 0.0f) {
    IDWriteTextLayout1* l1 = nullptr;
    if (SUCCEEDED(l->QueryInterface(__uuidof(IDWriteTextLayout1), reinterpret_cast<void**>(&l1))) && l1) {
      l1->SetCharacterSpacing(0.0f, t.size * t.spacing, 0.0f, all);
      l1->Release();
    }
  }
  if (t.underline) l->SetUnderline(TRUE, all);
  if (t.ellipsis) {
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    IDWriteInlineObject* sign = nullptr;
    if (SUCCEEDED(g_dw->CreateEllipsisTrimmingSign(f, &sign))) {
      l->SetTrimming(&trim, sign);
      sign->Release();
    }
  }
  return l;
}

D2D1_COLOR_F color(uint32_t rgb, float alpha) {
  return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha);
}

}  // namespace

bool init(std::wstring* problem) {
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d))) {
    if (problem) *problem = L"Direct2D";
    return false;
  }
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_dw)))) {
    if (problem) *problem = L"DirectWrite";
    return false;
  }
  load_fonts();
  g_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                       D2D1_LINE_JOIN_ROUND),
                           nullptr, 0, &g_round);
  g_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                       D2D1_LINE_JOIN_MITER),
                           nullptr, 0, &g_miter);
  return true;
}

void shutdown() {
  for (auto& f : g_formats)
    if (f.second) f.second->Release();
  g_formats.clear();
  if (g_round) g_round->Release();
  if (g_miter) g_miter->Release();
  if (g_fonts) g_fonts->Release();
  if (g_dw) {
    g_dw->UnregisterFontCollectionLoader(&g_collection_loader);
    g_dw->UnregisterFontFileLoader(&g_file_loader);
    g_dw->Release();
  }
  if (g_d2d) g_d2d->Release();
  g_round = g_miter = nullptr;
  g_fonts = nullptr;
  g_dw = nullptr;
  g_d2d = nullptr;
}

ID2D1Factory* d2d() { return g_d2d; }

std::wstring fonts_report() {
  std::wstring s = L"font files in the program: " + std::to_wstring(g_blobs.size()) + L"\n";
  for (const Known& k : g_known) s += L"  " + k.family + L" " + std::to_wstring(k.weight) + L"\n";
  for (Face f : {Face::Display, Face::Text, Face::Mono})
    for (int w : {400, 500, 600, 700}) {
      Pick p = pick(f, w);
      s += L"  ask " + std::to_wstring((int)f) + L"/" + std::to_wstring(w) + L" -> " + p.family + L" " +
           std::to_wstring(p.weight) + (p.own ? L"" : L" (Windows)") + L"\n";
    }
  return s;
}

Canvas::Canvas(ID2D1RenderTarget* target) : rt(target) {
  rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &brush_);
  rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
}

Canvas::~Canvas() {
  if (brush_) brush_->Release();
}

ID2D1SolidColorBrush* Canvas::brush(uint32_t rgb, float alpha) {
  brush_->SetColor(color(rgb, alpha));
  return brush_;
}

void Canvas::fill(const D2D1_RECT_F& r, uint32_t rgb, float alpha) { rt->FillRectangle(r, brush(rgb, alpha)); }

void Canvas::frame(const D2D1_RECT_F& r, uint32_t rgb, float width, float alpha) {
  const float h = width * 0.5f;
  rt->DrawRectangle(D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h), brush(rgb, alpha), width, g_miter);
}

void Canvas::dot(float cx, float cy, float radius, uint32_t rgb, float alpha) {
  rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush(rgb, alpha));
}

void Canvas::line(float x0, float y0, float x1, float y1, uint32_t rgb, float width, bool round) {
  rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush(rgb), width, round ? g_round : g_miter);
}

D2D1_SIZE_F Canvas::text(const std::wstring& s, const D2D1_RECT_F& box, const Type& t, uint32_t rgb, float alpha) {
  IDWriteTextLayout* l = layout(s, box.right - box.left, box.bottom - box.top, t);
  if (!l) return D2D1::SizeF(0, 0);
  rt->DrawTextLayout(D2D1::Point2F(box.left, box.top), l, brush(rgb, alpha), D2D1_DRAW_TEXT_OPTIONS_NONE);
  DWRITE_TEXT_METRICS m{};
  l->GetMetrics(&m);
  l->Release();
  return D2D1::SizeF(m.widthIncludingTrailingWhitespace, m.height);
}

D2D1_SIZE_F Canvas::measure(const std::wstring& s, const Type& t, float max_width) {
  Type top = t;
  top.valign = 0;
  IDWriteTextLayout* l = layout(s, max_width, 10000.0f, top);
  if (!l) return D2D1::SizeF(0, 0);
  DWRITE_TEXT_METRICS m{};
  l->GetMetrics(&m);
  l->Release();
  return D2D1::SizeF(m.widthIncludingTrailingWhitespace, m.height);
}

void Canvas::path(const char* d, float x, float y, float scale, bool fill, uint32_t fill_rgb, float stroke_width,
                  uint32_t stroke_rgb, bool round, float alpha) {
  ID2D1PathGeometry* g = nullptr;
  ID2D1GeometrySink* sink = nullptr;
  if (FAILED(g_d2d->CreatePathGeometry(&g)) || FAILED(g->Open(&sink))) {
    if (g) g->Release();
    return;
  }
  sink->SetFillMode(D2D1_FILL_MODE_WINDING);
  const char* p = d;
  float cx = 0, cy = 0, sx = 0, sy = 0;
  bool open = false;
  char cmd = 0;
  auto number = [&](float& v) {
    while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t') ++p;
    char* end = nullptr;
    v = std::strtof(p, &end);
    if (end == p) return false;
    p = end;
    return true;
  };
  auto begin = [&](float px, float py) {
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->BeginFigure(D2D1::Point2F(px, py), D2D1_FIGURE_BEGIN_FILLED);
    open = true;
    sx = cx = px;
    sy = cy = py;
  };
  for (;;) {
    while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t') ++p;
    if (!*p) break;
    if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) cmd = *p++;
    const bool rel = cmd >= 'a';
    float a = 0, b = 0, c = 0, e = 0, f = 0, h = 0;
    switch (cmd) {
      case 'M': case 'm':
        if (!number(a) || !number(b)) goto done;
        begin(rel ? cx + a : a, rel ? cy + b : b);
        cmd = rel ? 'l' : 'L';  // (more pairs are lines)
        break;
      case 'L': case 'l':
        if (!number(a) || !number(b)) goto done;
        cx = rel ? cx + a : a;
        cy = rel ? cy + b : b;
        if (open) sink->AddLine(D2D1::Point2F(cx, cy));
        break;
      case 'H': case 'h':
        if (!number(a)) goto done;
        cx = rel ? cx + a : a;
        if (open) sink->AddLine(D2D1::Point2F(cx, cy));
        break;
      case 'V': case 'v':
        if (!number(a)) goto done;
        cy = rel ? cy + a : a;
        if (open) sink->AddLine(D2D1::Point2F(cx, cy));
        break;
      case 'C': case 'c':
        if (!number(a) || !number(b) || !number(c) || !number(e) || !number(f) || !number(h)) goto done;
        if (open)
          sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(rel ? cx + a : a, rel ? cy + b : b),
                                              D2D1::Point2F(rel ? cx + c : c, rel ? cy + e : e),
                                              D2D1::Point2F(rel ? cx + f : f, rel ? cy + h : h)));
        cx = rel ? cx + f : f;
        cy = rel ? cy + h : h;
        break;
      case 'Z': case 'z':
        if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        open = false;
        cx = sx;
        cy = sy;
        cmd = 0;
        break;
      default:
        goto done;
    }
  }
done:
  if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
  sink->Close();
  sink->Release();
  const D2D1_MATRIX_3X2_F before = push(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(x, y));
  if (fill) rt->FillGeometry(g, brush(fill_rgb, alpha));
  if (stroke_width > 0.0f) rt->DrawGeometry(g, brush(stroke_rgb, alpha), stroke_width, round ? g_round : g_miter);
  pop(before);
  g->Release();
}

D2D1_MATRIX_3X2_F Canvas::push(const D2D1_MATRIX_3X2_F& m) {
  D2D1_MATRIX_3X2_F before;
  rt->GetTransform(&before);
  rt->SetTransform(D2D1::Matrix3x2F(m._11, m._12, m._21, m._22, m._31, m._32) *
                   D2D1::Matrix3x2F(before._11, before._12, before._21, before._22, before._31, before._32));
  return before;
}

void Canvas::pop(const D2D1_MATRIX_3X2_F& before) { rt->SetTransform(before); }

D2D1_MATRIX_3X2_F skew_x(float degrees, float cx, float cy) {
  return D2D1::Matrix3x2F::Skew(degrees, 0.0f, D2D1::Point2F(cx, cy));
}

D2D1_MATRIX_3X2_F rotate(float degrees, float cx, float cy) {
  return D2D1::Matrix3x2F::Rotation(degrees, D2D1::Point2F(cx, cy));
}

bool render_png(const std::wstring& file, float w, float h, float scale, void (*draw)(ID2D1RenderTarget*, void*), void* ctx) {
  IWICImagingFactory* wic = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
  const UINT pw = (UINT)std::lround(w * scale), ph = (UINT)std::lround(h * scale);
  IWICBitmap* bitmap = nullptr;
  ID2D1RenderTarget* rt = nullptr;
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* encoder = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  bool ok = false;
  do {
    if (FAILED(wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) break;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f * scale, 96.0f * scale);
    if (FAILED(g_d2d->CreateWicBitmapRenderTarget(bitmap, props, &rt))) break;
    rt->BeginDraw();
    draw(rt, ctx);
    if (FAILED(rt->EndDraw())) break;
    if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE))) break;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) break;
    if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) break;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) break;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetSize(pw, ph)) || FAILED(frame->SetPixelFormat(&fmt))) break;
    if (FAILED(frame->WriteSource(bitmap, nullptr))) break;
    if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) break;
    ok = true;
  } while (false);
  if (frame) frame->Release();
  if (encoder) encoder->Release();
  if (stream) stream->Release();
  if (rt) rt->Release();
  if (bitmap) bitmap->Release();
  wic->Release();
  return ok;
}

}  // namespace gfx
