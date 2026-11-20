#include "launcher/view.h"

#include <algorithm>
#include <cmath>

#include "launcher/gfx.h"

namespace view {

const uint32_t kAccents[4] = {0x4C9AF0, 0xEF5A5A, 0xA77BF5, 0xF7941D};

namespace {

using gfx::Canvas;
using gfx::Face;
using gfx::rect;
using gfx::Type;

constexpr uint32_t kInk = 0x07100A, kPaper = 0xEEF3E2, kMuted = 0x9DB0A2, kSoft = 0xC9D6C4, kGreen = 0x8BE836,
                   kDeep = 0x1E5A2C, kPanel = 0x0F1B16, kLine = 0x2A4136, kEdge = 0x4A6B59, kBar = 0x050908,
                   kBlack = 0x000000, kBad = 0xFF8A8A, kSheetInk = 0x0B1210, kFaint = 0x6F8578;
constexpr float kMainBottom = 556.0f;  // where the bottom line starts
constexpr float kLean = -7.0f;         // the lettering's and the buttons' slant, degrees
constexpr float kLeft = 40.0f, kColumn = 540.0f;  // the left column

Type type(Face face, float size, int weight = 400, float spacing = 0.0f) {
  Type t;
  t.face = face;
  t.size = size;
  t.weight = weight;
  t.spacing = spacing;
  return t;
}

float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

// Icons on a 24-unit square, drawn with a round pen.
constexpr const char* kIconDownload = "M12 4V15M7 11L12 16L17 11M5 20H19";
constexpr const char* kIconPlay = "M8 5L19 12L8 19Z";
constexpr const char* kIconFolder = "M3 7L5 5H9L11 7H19L21 9V17L19 19H5L3 17Z";
constexpr const char* kIconRetry = "M5 9H18L14.5 5.5M19 15H6L9.5 18.5";
constexpr const char* kIconCheck = "M5 12.5L9.5 17L19 7.5";
constexpr const char* kIconArrow = "M3 12H20M14 6L20 12L14 18";
constexpr const char* kIconEqual = "M5 9H19M5 15H19";
constexpr const char* kIconWarn = "M12 4L21 20H3ZM12 10V15M12 17.6V18";
constexpr const char* kIconBack = "M20 12H5M11 6L5 12L11 18";
constexpr const char* kIconOut = "M13 5H19V11M19 5L10 14M10 7H5V19H17V14";
constexpr const char* kIconClose = "M6 6L18 18M18 6L6 18";
constexpr const char* kIconMinimize = "M6 17H18";
constexpr const char* kIconPrev = "M14 6L8 12L14 18";
constexpr const char* kIconNext = "M10 6L16 12L10 18";
constexpr const char* kIconMinus = "M6 12H18";
constexpr const char* kIconPlus = "M6 12H18M12 6V18";

void icon(Canvas& c, const char* d, float x, float y, float size, uint32_t rgb, float pen = 2.2f, bool solid = false) {
  c.path(d, x, y, size / 24.0f, solid, rgb, pen, rgb);
}

// Dots on a grid in a box; strength(x, y) of 0..1 says how heavy each is.
template <class F>
void halftone(Canvas& c, const D2D1_RECT_F& box, float step, float radius, uint32_t rgb, float alpha, F strength) {
  for (float y = box.top + step * 0.5f; y < box.bottom; y += step)
    for (float x = box.left + step * 0.5f; x < box.right; x += step) {
      const float s = strength(x, y);
      if (s <= 0.03f) continue;
      c.dot(x, y, radius * (0.5f + 0.5f * s), rgb, alpha * s);
    }
}

struct Touch {
  bool hover, down, focus;
};
Touch touch(const View& v, int id) { return {v.hover == id, v.down == id, v.focus == id}; }

void focus_ring(Canvas& c, const D2D1_RECT_F& r) {
  c.frame(D2D1::RectF(r.left - 4, r.top - 4, r.right + 4, r.bottom + 4), kPaper, 2.0f);
}

// --- behind everything: the sky, the moon, the roofs ---------------------------

void backdrop(Canvas& c) {
  ID2D1GradientStopCollection* stops = nullptr;
  const D2D1_GRADIENT_STOP gs[3] = {{0.0f, D2D1::ColorF(0x0A1310)}, {0.55f, D2D1::ColorF(0x0C1813)}, {1.0f, D2D1::ColorF(0x11261B)}};
  if (SUCCEEDED(c.rt->CreateGradientStopCollection(gs, 3, &stops))) {
    ID2D1LinearGradientBrush* sky = nullptr;
    if (SUCCEEDED(c.rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, kHeight)), stops, &sky))) {
      c.rt->FillRectangle(D2D1::RectF(0, 0, kWidth, kHeight), sky);
      sky->Release();
    }
    stops->Release();
  }
  // The moon, half out of the window, its lit side in halftone.
  const float mx = 836.0f, my = 238.0f, mr = 220.0f;
  c.dot(mx, my, mr, 0x12251B);
  halftone(c, D2D1::RectF(mx - mr, my - mr, kWidth, my + mr), 10.0f, 1.9f, 0x23492F, 1.0f, [&](float x, float y) {
    const float dx = x - mx, dy = y - my;
    if (dx * dx + dy * dy > (mr - 3.0f) * (mr - 3.0f)) return 0.0f;
    const float along = ((x - (mx - mr)) + (y - (my - mr))) / (4.0f * mr);  // 0 at its top left, 1 at its bottom right
    return clamp01((along - 0.30f) / 0.65f);
  });
  // Halftone falling from the top left corner.
  halftone(c, D2D1::RectF(0, kCaption, 360, 328), 9.0f, 1.7f, kGreen, 0.20f, [](float x, float y) {
    const float dx = x / 356.0f, dy = (y - kCaption) / 277.0f;
    return clamp01(1.0f - std::sqrt(dx * dx + dy * dy));
  });
  // The roofs along the bottom: a water tower, an aerial, a few lit windows.
  const float sy = kMainBottom - 150.0f;
  const uint32_t roofs = 0x060C0A;
  c.path("M0 150V96h46V78h58v26h44V88h64v24h52V70h6V48h4v22h46v36h58V92h70v20h60V84h44v18h40V58h84v30h52V40h70v44h58V62h64v20"
         "h40V150Z",
         0, sy, 1.0f, true, roofs, 0.0f, 0);
  c.path("M602 12L626 0l24 12v24h4v4h-6l4 18h-4l-4-18h-36l-4 18h-4l4-18h-6v-4h4Z", 0, sy, 1.0f, true, roofs, 0.0f, 0);
  c.path("M760 40V14M752 22H768M755 30H765", 0, sy, 1.0f, false, 0, 2.5f, roofs, false);
  static const float kGreenWindows[][2] = {{604, 74}, {656, 74}, {630, 92}, {616, 110}, {740, 56}, {776, 72},
                                           {752, 90}, {788, 108}, {868, 78}, {892, 96}};
  static const float kWarmWindows[][2] = {{644, 110}, {764, 56}, {880, 114}, {700, 104}};
  for (auto& w : kGreenWindows) c.fill(rect(w[0], sy + w[1], 5, 7), kGreen, 0.6f);
  for (auto& w : kWarmWindows) c.fill(rect(w[0], sy + w[1], 5, 7), 0xF2E08A, 0.55f);
}

// --- the window's own bar -------------------------------------------------------

void window_button(Canvas& c, const View& v, std::vector<Hit>& hits, int id, float x, const char* d, uint32_t rgb,
                   bool danger = false) {
  const D2D1_RECT_F r = rect(x, 1, 44, 44);
  const Touch t = touch(v, id);
  if (t.hover || t.down) c.fill(r, danger ? 0xC24A4A : 0xFFFFFF, danger ? 1.0f : (t.down ? 0.14f : 0.08f));
  if (d) icon(c, d, x + 12, 13, 20, danger && (t.hover || t.down) ? 0xFFFFFF : rgb);
  if (t.focus) c.frame(D2D1::RectF(r.left + 2, r.top + 2, r.right - 2, r.bottom - 2), kPaper, 2.0f);
  hits.push_back({id, r});
}

void caption(Canvas& c, const View& v, std::vector<Hit>& hits) {
  c.fill(rect(0, 0, kWidth, kCaption), kBar);
  c.fill(rect(0, kCaption - 2, kWidth, 2), kBlack);
  // The mark: a shell's plates.
  const float s = 26.0f / 28.0f;
  c.path("M14 2L24.4 8V20L14 26L3.6 20V8Z", 16, 10, s, true, kGreen, 2.0f, kBlack);
  c.path("M14 8.5L18.8 11.25V16.75L14 19.5L9.2 16.75V11.25Z", 16, 10, s, true, 0x2F7D3B, 1.6f, kBlack);
  c.path("M14 8.5V2M18.8 11.25L24.4 8M18.8 16.75L24.4 20M14 19.5V26M9.2 16.75L3.6 20M9.2 11.25L3.6 8", 16, 10, s, false, 0,
         1.6f, kBlack, false);
  const D2D1_SIZE_F name = c.text(L"TMNTNET", D2D1::RectF(52, 0, 400, 46), type(Face::Display, 15, 400, 0.04f), kPaper);
  c.text(L"ЛАУНЧЕР", D2D1::RectF(52 + name.width + 10, 1, 520, 47), type(Face::Text, 11, 600, 0.16f), kMuted);

  // Settings: three sliders.
  const uint32_t gear = v.settings ? v.accent : kSoft;
  window_button(c, v, hits, kSettings, 824, nullptr, gear);
  {
    const float x = 836, y = 13, k = 20.0f / 24.0f;
    c.path("M4 6H12M18 6H20M4 12H6M12 12H20M4 18H14", x, y, k, false, 0, 2.2f, gear);
    static const float kKnobs[][2] = {{15, 6}, {9, 12}, {17, 18}};
    for (auto& p : kKnobs)
      c.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + p[0] * k, y + p[1] * k), 2.4f * k, 2.4f * k), c.brush(gear), 2.2f * k);
  }
  window_button(c, v, hits, kMinimize, 868, kIconMinimize, kSoft);
  window_button(c, v, hits, kClose, 912, kIconClose, kSoft, true);
}

// --- buttons --------------------------------------------------------------------

// The big green one. Returns its right edge.
float big_button(Canvas& c, const View& v, std::vector<Hit>& hits, int id, float x, float y, const std::wstring& label,
                 Icon what, bool on) {
  const Type t = type(Face::Display, 17, 400, 0.02f);
  const float tw = c.measure(label, t).width;
  const float iw = what == Icon::None ? 0.0f : 32.0f;
  const float w = 3 + 20 + iw + tw + 24 + 3, h = 62;
  const D2D1_RECT_F r = rect(x, y, w, h);
  const Touch s = touch(v, id);
  const float press = s.down && on ? 3.0f : 0.0f;
  const D2D1_MATRIX_3X2_F before = c.push(gfx::skew_x(kLean, x + w * 0.5f, y + h * 0.5f));
  c.fill(rect(x + 6, y + 6, w, h), kDeep);
  const D2D1_RECT_F b = rect(x + press, y + press, w, h);
  c.fill(b, !on ? 0x4F6B3C : s.hover ? 0xA8F25C : kGreen);
  c.frame(b, kBlack, 3.0f);
  float cx = b.left + 23;
  const float alpha = on ? 1.0f : 0.75f;
  if (what != Icon::None) {
    const char* d = what == Icon::Download ? kIconDownload : what == Icon::Play ? kIconPlay : what == Icon::Folder ? kIconFolder : kIconRetry;
    c.path(d, cx, b.top + 20, 22.0f / 24.0f, what == Icon::Play, kInk, 2.8f, kInk, true, alpha);
    cx += 32;
  }
  c.text(label, D2D1::RectF(cx, b.top, b.right, b.bottom), t, kInk, alpha);
  c.pop(before);
  if (s.focus) focus_ring(c, r);
  if (on) hits.push_back({id, r});
  return x + w;
}

// An outlined one. Returns its width.
float line_button(Canvas& c, const View& v, std::vector<Hit>& hits, int id, float x, float y, float w, float h,
                  const std::wstring& label, bool lean, uint32_t text_rgb = kPaper, uint32_t edge_rgb = kEdge,
                  const char* lead_icon = nullptr, uint32_t fill_rgb = 0, bool filled = false) {
  Type t = type(Face::Text, 13, 700, lean ? 0.06f : 0.0f);
  const float tw = c.measure(label, t).width;
  const float lead = lead_icon ? 26.0f : 0.0f;
  if (w <= 0.0f) w = 2 + 20 + lead + tw + 20 + 2;
  const D2D1_RECT_F r = rect(x, y, w, h);
  const Touch s = touch(v, id);
  D2D1_MATRIX_3X2_F before{};
  if (lean) before = c.push(gfx::skew_x(kLean, x + w * 0.5f, y + h * 0.5f));
  if (filled) c.fill(r, fill_rgb);
  if (s.down) c.fill(r, 0xFFFFFF, 0.10f);
  else if (s.hover) c.fill(r, 0xFFFFFF, 0.05f);
  c.frame(r, s.hover || s.down ? text_rgb : edge_rgb, 2.0f);
  const float content = lead + tw, cx = x + (w - content) * 0.5f;
  if (lead_icon) icon(c, lead_icon, cx, y + (h - 18) * 0.5f, 18, text_rgb, 2.4f);
  c.text(label, D2D1::RectF(cx + lead, y, x + w, y + h), t, text_rgb);
  if (lean) c.pop(before);
  if (s.focus) focus_ring(c, r);
  hits.push_back({id, r});
  return w;
}

// Underlined words that take a click. Returns their width.
float text_button(Canvas& c, const View& v, std::vector<Hit>& hits, int id, float x, float y, float h,
                  const std::wstring& label, float pad) {
  Type t = type(Face::Text, 13, 600);
  t.underline = true;
  const float tw = c.measure(label, t).width, w = tw + 2 * pad;
  const D2D1_RECT_F r = rect(x, y, w, h);
  const Touch s = touch(v, id);
  c.text(label, D2D1::RectF(x + pad, y, x + w, y + h), t, s.hover || s.down ? kPaper : kSoft);
  if (s.focus) focus_ring(c, r);
  hits.push_back({id, r});
  return w;
}

// --- the left column -------------------------------------------------------------

void ticket(Canvas& c, float x, float y, float w, const wchar_t* label, const std::wstring& value, const std::wstring& note,
            uint32_t edge) {
  const D2D1_RECT_F r = rect(x, y, w, 92);
  c.fill(r, kPanel);
  c.frame(r, edge, 2.0f);
  c.text(label, D2D1::RectF(x + 16, y + 12, x + w - 12, y + 26), type(Face::Text, 11, 600, 0.12f), kMuted);
  Type big = type(Face::Mono, 26, 700);
  big.ellipsis = true;
  c.text(value, D2D1::RectF(x + 16, y + 30, x + w - 12, y + 59), big, kPaper);
  Type tiny = type(Face::Text, 12);
  tiny.ellipsis = true;
  c.text(note, D2D1::RectF(x + 16, y + 63, x + w - 12, y + 78), tiny, kMuted);
}

// The line under the versions; a long one (an error's words) takes two. Returns its height.
float mark_line(Canvas& c, const View& v, float y) {
  float x = kLeft;
  const uint32_t ink = v.mark == Mark::Bad ? kBad : kSoft;
  if (v.mark == Mark::Ok) icon(c, kIconCheck, x, y + 1, 18, kGreen, 2.6f);
  else if (v.mark == Mark::Warn) icon(c, kIconWarn, x, y + 1, 18, v.accent, 2.4f);
  else if (v.mark == Mark::Bad) icon(c, kIconWarn, x, y + 1, 18, kBad, 2.4f);
  else if (v.mark == Mark::Busy)
    for (int i = 0; i < 3; ++i) c.dot(x + 3 + i * 6.0f, y + 10, 1.8f, kMuted);
  if (v.mark != Mark::None) x += 26;
  Type t = type(Face::Text, 13);
  t.wrap = true;
  t.line = 20.0f / 13.0f;
  t.valign = 0;
  t.ellipsis = true;
  const float h = std::min(40.0f, std::max(20.0f, c.measure(v.mark_text, t, kLeft + kColumn - x).height));
  c.text(v.mark_text, D2D1::RectF(x, y, kLeft + kColumn, y + h), t, ink);
  return h;
}

float versions(Canvas& c, const View& v, float y) {
  const float w = (kColumn - 28 - 24) / 2;
  ticket(c, kLeft, y, w, L"УСТАНОВЛЕНО", v.installed, v.installed_note, kLine);
  const float ax = kLeft + w + 12;
  if (v.equal) icon(c, kIconEqual, ax, y + 32, 28, kGreen, 2.6f);
  else if (!v.ahead) icon(c, kIconArrow, ax, y + 32, 28, v.accent, 2.6f);
  const float x2 = ax + 28 + 12;
  ticket(c, x2, y, w, v.latest_label.c_str(), v.latest, v.latest_note, v.latest_bright ? v.accent : kLine);
  if (v.is_new) {
    const Type t = type(Face::Text, 10, 700, 0.12f);
    const float tw = c.measure(L"НОВАЯ", t).width, cw = tw + 16 + 4;
    const D2D1_RECT_F chip = rect(x2 + w - 2 - 10 - cw, y - 9, cw, 20);
    c.fill(chip, v.accent);
    c.frame(chip, kBlack, 2.0f);
    c.text(L"НОВАЯ", D2D1::RectF(chip.left + 10, chip.top, chip.right, chip.bottom), t, kInk);
  }
  return y + 92 + 12 + mark_line(c, v, y + 92 + 12);
}

float no_game(Canvas& c, const View& v, float y) {
  Type body = type(Face::Text, 13);
  body.wrap = true;
  body.line = 1.45f;
  body.valign = 0;
  const std::wstring text = L"Лаунчер не нашёл игру на этом компьютере. Укажите папку, где лежит TMNT.EXE.";
  const float tw = kColumn - 16 - 24 - 12 - 16 - 4;
  const float th = c.measure(text, body, tw).height;
  const float h = 2 + 14 + 20 + 4 + th + 14 + 2;
  const D2D1_RECT_F r = rect(kLeft, y, kColumn, h);
  c.fill(r, kPanel);
  c.frame(r, v.accent, 2.0f);
  icon(c, kIconWarn, kLeft + 18, y + 16, 24, v.accent, 2.2f);
  c.text(L"Игра не найдена", D2D1::RectF(kLeft + 54, y + 16, kLeft + kColumn - 16, y + 36), type(Face::Text, 16, 700), kPaper);
  c.text(text, D2D1::RectF(kLeft + 54, y + 40, kLeft + 54 + tw, y + 40 + th), body, kSoft);
  return y + h;
}

float job(Canvas& c, const View& v, std::vector<Hit>& hits, float y) {
  // The three steps.
  float x = kLeft;
  for (const Step& s : v.steps) {
    const Type t = type(Face::Text, 12, 600);
    const float tw = c.measure(s.label, t).width, w = 2 + 5 + 20 + 8 + tw + 12 + 2;
    const D2D1_RECT_F chip = rect(x, y, w, 34);
    c.fill(chip, s.state == 1 ? 0x12261B : kPanel);
    c.frame(chip, s.state == 1 ? kGreen : kLine, 2.0f);
    const D2D1_RECT_F num = rect(x + 7, y + 7, 20, 20);
    c.fill(num, s.state ? kGreen : kLine);
    if (s.state == 2) icon(c, kIconCheck, num.left + 3, num.top + 3, 14, kInk, 3.2f);
    else {
      Type n = type(Face::Mono, 11, 700);
      n.align = 1;
      c.text(std::to_wstring(&s - v.steps + 1), num, n, s.state ? kInk : kSoft);
    }
    c.text(s.label, D2D1::RectF(x + 35, y, x + w, y + 34), t, s.state == 1 ? kPaper : s.state == 2 ? kSoft : kMuted);
    x += w + 8;
  }
  y += 34 + 14;
  // The percent and what is going on.
  {
    const D2D1_MATRIX_3X2_F before = c.push(gfx::skew_x(kLean, kLeft, y + 40));
    c.text(std::to_wstring(v.percent) + L"%", D2D1::RectF(kLeft, y, kLeft + 200, y + 40), type(Face::Display, 40), kPaper);
    c.pop(before);
    Type t = type(Face::Mono, 13, 500);
    t.align = 2;
    t.valign = 2;
    t.ellipsis = true;
    c.text(v.stage, D2D1::RectF(kLeft + 150, y + 10, kLeft + kColumn, y + 38), t, kSoft);
  }
  y += 40 + 14;
  // The pipe filling up.
  {
    const D2D1_RECT_F pipe = D2D1::RectF(kLeft + 14, y, kLeft + kColumn - 22, y + 36);
    c.fill(D2D1::RectF(pipe.left + 6, pipe.top + 6, pipe.right + 6, pipe.bottom + 6), kDeep);
    c.fill(pipe, kPanel);
    const float inner = pipe.right - pipe.left - 6, fw = inner * clamp01(v.percent / 100.0f);
    if (fw > 0.5f) {
      const D2D1_RECT_F full = D2D1::RectF(pipe.left + 3, pipe.top + 3, pipe.left + 3 + fw, pipe.bottom - 3);
      c.fill(full, kGreen);
      c.rt->PushAxisAlignedClip(full, D2D1_ANTIALIAS_MODE_ALIASED);
      const float period = 22.6f, travel = std::fmod(v.flow, period);
      for (float sx = full.left - 60 + travel; sx < full.right + 40; sx += period)  // (slanted bands, going with the flow)
        c.rt->DrawLine(D2D1::Point2F(sx + 38, full.top - 4), D2D1::Point2F(sx, full.bottom + 4), c.brush(kBlack, 0.14f), 8.0f);
      c.rt->PopAxisAlignedClip();
      c.fill(D2D1::RectF(full.left, full.top + 4, full.right, full.top + 9), 0xFFFFFF, 0.5f);
    }
    c.frame(pipe, kBlack, 3.0f);
    for (float fx : {pipe.left - 17, pipe.right + 3}) {
      const D2D1_RECT_F flange = rect(fx, y - 7, 14, 50);
      c.fill(flange, kLine);
      c.frame(flange, kBlack, 3.0f);
    }
  }
  y += 36 + 6 + 14;
  // Where it comes from; the way out.
  {
    float right = kLeft + kColumn;
    if (v.can_cancel) {
      Type t = type(Face::Text, 13, 700, 0.06f);
      const float w = 2 + 18 + c.measure(L"ОТМЕНА", t).width + 18 + 2;
      line_button(c, v, hits, kCancel, right - w, y, w, 44, L"ОТМЕНА", false);
      right -= w + 16;
    }
    Type t = type(Face::Mono, 12, 500);
    t.ellipsis = true;
    c.text(v.source, D2D1::RectF(kLeft, y, right, y + 44), t, kMuted);
  }
  y += 44 + 14;
  Type note = type(Face::Text, 12);
  note.wrap = true;
  note.line = 1.45f;
  note.valign = 0;
  c.text(v.job_note, D2D1::RectF(kLeft, y, kLeft + kColumn, y + 40), note, kMuted);
  return y + 36;
}

void game_folder(Canvas& c, const View& v, std::vector<Hit>& hits, float y, float left, float right, int id, float pad) {
  icon(c, kIconFolder, left, y + 13, 18, kMuted, 2.2f);
  float x = left + 28;
  const Type label = type(Face::Text, 11, 600, 0.12f);
  x += c.text(L"ПАПКА ИГРЫ", D2D1::RectF(x, y, x + 200, y + 44), label, kMuted).width + 10;
  Type under = type(Face::Text, 13, 600);
  const float bw = c.measure(L"Изменить", under).width + 2 * pad;
  Type path = type(Face::Mono, 12, 500);
  path.ellipsis = true;
  const float room = right - bw - 6 - x;
  const float pw = std::min(room, c.measure(v.game_path, path).width + 2);
  c.text(v.game_path, D2D1::RectF(x, y, x + pw, y + 44), path, kPaper);
  text_button(c, v, hits, id, x + pw + 6, y, 44, L"Изменить", pad);
}

// --- the sheet pinned on the right ------------------------------------------------

void sheet(Canvas& c, const View& v, std::vector<Hit>& hits) {
  const float px = 620, py = 104, pw = 300, inner = 254;
  Type meta = type(Face::Mono, 12, 700);
  meta.ellipsis = true;
  Type title = type(Face::Display, 17);
  title.wrap = true;
  title.line = 1.3f;
  title.valign = 0;
  Type item = type(Face::Text, 13);
  item.wrap = true;
  item.line = 1.45f;
  item.valign = 0;
  Type link = type(Face::Text, 13, 700);
  link.underline = true;

  const float title_h = v.paper_title.empty() ? 0.0f : c.measure(v.paper_title, title, inner).height;
  std::vector<float> item_h;
  float items_h = 0.0f;
  for (const std::wstring& s : v.paper_items) {
    item_h.push_back(c.measure(s, item, inner - 18).height);
    items_h += item_h.back() + (item_h.size() > 1 ? 10.0f : 0.0f);
  }
  float content = 16;
  if (title_h > 0) content += 12 + title_h;
  if (items_h > 0) content += 12 + items_h;
  if (!v.paper_link.empty()) content += 12 + 2 + 12 + 28;
  const float ph = 3 + 28 + content + 16 + 3;

  const D2D1_MATRIX_3X2_F before = c.push(gfx::rotate(1.2f, px + pw * 0.5f, py + ph * 0.5f));
  c.fill(rect(px + 8, py + 8, pw, ph), v.accent);
  c.fill(rect(px, py, pw, ph), kPaper);
  const float cx = px + pw - 3, cy = py + ph - 3;
  halftone(c, D2D1::RectF(cx - 144, std::max(py + 3, cy - 144), cx, cy), 8.0f, 1.6f, kSheetInk, 0.24f, [&](float x, float y) {
    const float dx = cx - x, dy = cy - y;
    return clamp01(1.0f - std::sqrt(dx * dx + dy * dy) / 142.0f);
  });
  c.frame(rect(px, py, pw, ph), kBlack, 3.0f);

  // The tab: a headband's knot with its two tails.
  const Type tab = type(Face::Display, 12, 400, 0.06f);
  const float tw = c.measure(v.paper_tab, tab).width, tabw = 3 + 12 + tw + 12 + 3, tx = px + 17, ty = py - 15;
  c.path("M2 10L44 20L37 23L43 30L2 17Z", tx + tabw - 4, ty + 4, 1.0f, true, v.accent, 2.5f, kBlack);
  c.path("M2 6L50 2L44 9L53 15L2 14Z", tx + tabw - 4, ty + 4, 1.0f, true, v.accent, 2.5f, kBlack);
  c.fill(rect(tx, ty, tabw, 30), v.accent);
  c.frame(rect(tx, ty, tabw, 30), kBlack, 3.0f);
  c.text(v.paper_tab, D2D1::RectF(tx + 15, ty, tx + tabw + 20, ty + 30), tab, kSheetInk);

  const float x = px + 23;
  float y = py + 31;
  c.text(v.paper_meta, D2D1::RectF(x, y, x + inner, y + 16), meta, 0x3D4D44);
  y += 16;
  if (title_h > 0) {
    y += 12;
    c.text(v.paper_title, D2D1::RectF(x, y, x + inner, y + title_h), title, kSheetInk);
    y += title_h;
  }
  if (items_h > 0) {
    y += 12;
    for (size_t i = 0; i < v.paper_items.size(); ++i) {
      if (i) y += 10;
      const D2D1_MATRIX_3X2_F lean = c.push(gfx::skew_x(-12.0f, x + 4, y + 9));
      c.fill(rect(x, y + 5, 8, 8), 0x2F7D3B);
      c.frame(rect(x, y + 5, 8, 8), kBlack, 1.5f);
      c.pop(lean);
      c.text(v.paper_items[i], D2D1::RectF(x + 18, y, x + inner, y + item_h[i]), item, kSheetInk);
      y += item_h[i];
    }
  }
  if (!v.paper_link.empty()) {
    y += 12;
    for (float dx = 0; dx < inner; dx += 10) c.fill(rect(x + dx, y, 6, 2), kSheetInk, 0.4f);
    y += 2 + 12;
    const Touch s = touch(v, kPaperLink);
    const float lw = c.text(v.paper_link, D2D1::RectF(x, y, x + inner - 20, y + 28), link, s.hover ? 0x2F7D3B : kSheetInk).width;
    icon(c, kIconOut, x + lw + 6, y + 7, 14, s.hover ? 0x2F7D3B : kSheetInk, 2.6f);
    const D2D1_RECT_F r = rect(x, y, lw + 24, 28);
    if (s.focus) c.frame(D2D1::RectF(r.left - 4, r.top - 2, r.right + 4, r.bottom + 2), kSheetInk, 2.0f);
    hits.push_back({kPaperLink, r});
  }
  c.pop(before);
}

void hero(Canvas& c, const View& v, std::vector<Hit>& hits) {
  // Four headbands' colours and what this is.
  {
    float x = kLeft;
    for (uint32_t rgb : kAccents) {
      const D2D1_MATRIX_3X2_F lean = c.push(gfx::skew_x(-24.0f, x + 7, 82));
      c.fill(rect(x, 79, 14, 6), rgb);
      c.pop(lean);
      x += 18;
    }
    c.text(L"СЕТЕВОЙ КООП · ДО 4 ИГРОКОВ", D2D1::RectF(kLeft + 78, 74, kLeft + kColumn, 90), type(Face::Text, 12, 600, 0.14f),
           kMuted);
  }
  // The name, leaning, with a hard shadow.
  {
    const D2D1_MATRIX_3X2_F lean = c.push(gfx::skew_x(kLean, kLeft, 204));
    const Type big = type(Face::Display, 56), second = type(Face::Display, 38);
    c.text(L"TMNT 2003", D2D1::RectF(kLeft + 4, 108, 700, 164), big, kDeep);
    c.text(L"TMNT 2003", D2D1::RectF(kLeft, 104, 700, 160), big, kPaper);
    c.text(L"КООП ПО СЕТИ", D2D1::RectF(kLeft + 3, 169, 700, 207), second, 0x123A1E);
    c.text(L"КООП ПО СЕТИ", D2D1::RectF(kLeft, 166, 700, 204), second, kGreen);
    c.pop(lean);
  }
  float y = 226;
  if (v.job) y = job(c, v, hits, y);
  else {
    if (v.tickets) y = versions(c, v, y);
    if (v.no_game) y = no_game(c, v, y);
    if (!v.primary.empty()) {
      y += 20;
      float x = big_button(c, v, hits, kPrimary, kLeft + 6, y, v.primary, v.icon, v.primary_on);
      if (!v.secondary.empty()) line_button(c, v, hits, kSecondary, x + 18, y, 0, 62, v.secondary, true);
      y += 62 + 16;
      Type hint = type(Face::Text, 12);
      hint.wrap = true;
      hint.line = 1.45f;
      hint.valign = 0;
      c.text(v.hint, D2D1::RectF(kLeft, y, kLeft + kColumn, y + 40), hint, kMuted);
    }
  }
  game_folder(c, v, hits, kMainBottom - 50, kLeft, kLeft + kColumn, kPick, 8);
  sheet(c, v, hits);
}

// --- settings ---------------------------------------------------------------------

D2D1_RECT_F group(Canvas& c, const View& v, float x, float y, float w, float h, const std::wstring& name) {
  const D2D1_RECT_F r = rect(x, y, w, h);
  c.fill(r, kPanel);
  c.frame(r, kLine, 2.0f);
  c.text(name, D2D1::RectF(x + 18, y + 16, x + w - 18, y + 32), type(Face::Display, 13, 400, 0.06f), v.accent);
  return D2D1::RectF(x + 18, y + 16 + 16, x + w - 18, y + h - 18);
}

void toggle(Canvas& c, const View& v, std::vector<Hit>& hits, int id, const D2D1_RECT_F& row, const std::wstring& label, bool on,
            bool usable = true) {
  c.text(label, D2D1::RectF(row.left, row.top, row.right - 64, row.bottom), type(Face::Text, 14), usable ? kPaper : kFaint);
  const D2D1_RECT_F hit = D2D1::RectF(row.right - 56, row.top, row.right, row.bottom);
  const float ty = (row.top + row.bottom) * 0.5f - 13, alpha = usable ? 1.0f : 0.45f;
  const D2D1_RECT_F track = rect(hit.left + 2, ty, 52, 26);
  const Touch s = touch(v, id);
  c.fill(track, on ? kGreen : 0x1A2B23, alpha);
  c.frame(track, on ? kBlack : (s.hover && usable ? kPaper : kEdge), 2.0f, alpha);
  const D2D1_RECT_F knob = rect(track.left + (on ? 30 : 4), ty + 4, 18, 18);
  c.fill(knob, kPaper, alpha);
  c.frame(knob, kBlack, 2.0f, alpha);
  if (!usable) return;
  if (s.focus) focus_ring(c, track);
  hits.push_back({id, hit});
}

// The settings' pages: leaning tabs at the right of the page's name.
void tabs(Canvas& c, const View& v, std::vector<Hit>& hits, float y) {
  constexpr int kTabs = 4;
  static const wchar_t* kNames[kTabs] = {L"ЛАУНЧЕР", L"ГРАФИКА", L"ЗВУК", L"УПРАВЛЕНИЕ"};
  Type t = type(Face::Display, 12, 400, 0.06f);
  float widths[kTabs], total = 0.0f;
  for (int i = 0; i < kTabs; ++i) {
    widths[i] = 14 + c.measure(kNames[i], t).width + 14;
    total += widths[i] + (i ? 10.0f : 0.0f);
  }
  t.align = 1;
  float x = kWidth - kLeft - 6 - total;
  for (int i = 0; i < kTabs; ++i) {
    const bool on = v.tab == i;
    const D2D1_RECT_F r = rect(x, y, widths[i], 44);
    const Touch s = touch(v, kTab0 + i);
    const D2D1_MATRIX_3X2_F lean = c.push(gfx::skew_x(kLean, x + widths[i] * 0.5f, y + 22));
    if (on) {
      c.fill(rect(x + 4, y + 4, widths[i], 44), kDeep);
      c.fill(r, v.accent);
      c.frame(r, kBlack, 3.0f);
    } else {
      if (s.down) c.fill(r, 0xFFFFFF, 0.10f);
      else if (s.hover) c.fill(r, 0xFFFFFF, 0.05f);
      c.frame(r, s.hover || s.down ? kPaper : kEdge, 2.0f);
    }
    c.text(kNames[i], r, t, on ? kSheetInk : kPaper);
    c.pop(lean);
    if (s.focus) focus_ring(c, r);
    hits.push_back({kTab0 + i, r});
    x += widths[i] + 10;
  }
}

int part_id(const Row& row, int part) { return kRowFirst + row.number * kRowParts + part; }

void page_foot(Canvas& c, const View& v, std::vector<Hit>& hits, float y);

// A value between two arrows, like a row of the game's own options: 220 wide, its right edge at `right`.
void arrows_box(Canvas& c, const View& v, std::vector<Hit>& hits, float right, float top, const std::wstring& value,
                bool usable, int id_prev, int id_next) {
  const D2D1_RECT_F box = rect(right - 220, top, 220, 44);
  const uint32_t ink = usable ? kPaper : kFaint;
  c.fill(box, 0x0A1310);
  c.frame(box, usable ? kEdge : kLine, 2.0f);
  for (int i = 0; i < 2; ++i) {
    const int id = i ? id_next : id_prev;
    const D2D1_RECT_F a = rect(i ? box.right - 44 : box.left, box.top, 44, 44);
    const Touch s = touch(v, id);
    if (usable && (s.hover || s.down))
      c.fill(D2D1::RectF(a.left + 2, a.top + 2, a.right - 2, a.bottom - 2), 0xFFFFFF, s.down ? 0.14f : 0.07f);
    c.fill(rect(i ? a.left : a.right - 2, box.top + 2, 2, 40), kLine);
    icon(c, i ? kIconNext : kIconPrev, a.left + 13, a.top + 13, 18, usable && (s.hover || s.down) ? kGreen : ink, 2.8f);
    if (!usable) continue;
    if (s.focus) focus_ring(c, a);
    hits.push_back({id, a});
  }
  Type t = type(Face::Text, 13, 700);
  t.align = 1;
  t.ellipsis = true;
  c.text(value, D2D1::RectF(box.left + 46, box.top, box.right - 46, box.bottom), t, ink);
}

void choice(Canvas& c, const View& v, std::vector<Hit>& hits, const Row& row, const D2D1_RECT_F& r) {
  arrows_box(c, v, hits, r.right, r.top, row.value, row.usable, part_id(row, kPartPrev), part_id(row, kPartNext));
}

// How loud: five blocks between "quieter" and "louder".
void level(Canvas& c, const View& v, std::vector<Hit>& hits, const Row& row, const D2D1_RECT_F& r) {
  const float w = 44 + 12 + 5 * 28 + 4 * 6 + 12 + 44;
  const uint32_t ink = row.usable ? kPaper : kFaint;
  auto button = [&](int part, float x, const char* d) {
    const D2D1_RECT_F b = rect(x, r.top, 44, 44);
    const Touch s = touch(v, part_id(row, part));
    if (row.usable && (s.hover || s.down)) c.fill(b, 0xFFFFFF, s.down ? 0.12f : 0.06f);
    c.frame(b, !row.usable ? kLine : s.hover || s.down ? kPaper : kEdge, 2.0f);
    icon(c, d, x + 13, r.top + 13, 18, row.usable && (s.hover || s.down) ? kGreen : ink, 2.8f);
    if (!row.usable) return;
    if (s.focus) focus_ring(c, b);
    hits.push_back({part_id(row, part), b});
  };
  button(kPartPrev, r.right - w, kIconMinus);
  float x = r.right - w + 44 + 12;
  for (int i = 0; i < 5; ++i) {
    const D2D1_RECT_F block = rect(x, r.top + 11, 28, 22);
    const bool full = i <= row.level;
    const Touch s = touch(v, part_id(row, kPartPiece + i));
    const D2D1_MATRIX_3X2_F lean = c.push(gfx::skew_x(-12.0f, x + 14, r.top + 22));
    c.fill(block, full ? (row.usable ? kGreen : 0x4F6B3C) : 0x1A2B23);
    if (row.usable && (s.hover || s.down)) c.fill(block, 0xFFFFFF, 0.20f);
    c.frame(block, full ? kBlack : row.usable ? kEdge : kLine, 2.0f);
    c.pop(lean);
    if (row.usable) {
      if (s.focus) focus_ring(c, block);
      hits.push_back({part_id(row, kPartPiece + i), rect(x - 3, r.top, 34, 44)});
    }
    x += 28 + 6;
  }
  button(kPartNext, r.right - 44, kIconPlus);
}

// One of two.
void pair(Canvas& c, const View& v, std::vector<Hit>& hits, const Row& row, const D2D1_RECT_F& r) {
  const float w = 240, half = (w - 8) / 2;
  for (int i = 0; i < 2; ++i) {
    const bool on = row.chosen == i;
    const D2D1_RECT_F b = rect(r.right - w + i * (half + 8), r.top, half, 44);
    if (row.usable) {
      line_button(c, v, hits, part_id(row, kPartPiece + i), b.left, b.top, half, 44, row.options[i], false, on ? kInk : kPaper,
                  on ? kBlack : kEdge, nullptr, kGreen, on);
    } else {
      c.frame(b, kLine, 2.0f);
      Type t = type(Face::Text, 13, 700);
      t.align = 1;
      c.text(row.options[i], b, t, kFaint);
    }
  }
}

// ГРАФИКА, ЗВУК: the game's settings in two groups; under them what they are
// (or why they cannot be changed now) and the way back to the defaults.
void game_page(Canvas& c, const View& v, std::vector<Hit>& hits, float y) {
  const float w = (kWidth - 80 - 28) / 2;
  size_t most = 1;
  for (const Group& g : v.groups) most = std::max(most, g.rows.size());
  const float h = 32 + 8 + most * 44.0f + (most - 1) * 6.0f + 18;
  for (int i = 0; i < 2; ++i) {
    const Group& g = v.groups[i];
    const D2D1_RECT_F in = group(c, v, kLeft + i * (w + 28), y, w, h, g.name);
    float gy = in.top + 8;
    for (const Row& row : g.rows) {
      const D2D1_RECT_F r = D2D1::RectF(in.left, gy, in.right, gy + 44);
      if (row.kind == RowKind::Switch) {
        toggle(c, v, hits, part_id(row, kPartSwitch), r, row.label, row.on, row.usable);
      } else {
        Type label = type(Face::Text, 14);
        label.ellipsis = true;
        c.text(row.label, D2D1::RectF(r.left, r.top, r.right - 226, r.bottom), label, row.usable ? kPaper : kFaint);
        if (row.kind == RowKind::Choice) choice(c, v, hits, row, r);
        else if (row.kind == RowKind::Level) level(c, v, hits, row, r);
        else pair(c, v, hits, row, r);
      }
      gy += 44 + 6;
    }
    if (!g.note.empty()) {
      Type note = type(Face::Text, 12);
      note.wrap = true;
      note.line = 1.45f;
      note.valign = 0;
      c.text(g.note, D2D1::RectF(in.left, gy + 4, in.right, in.bottom), note, kMuted);
    }
  }
  page_foot(c, v, hits, y + h + 14);
}

// УПРАВЛЕНИЕ: whose keys, the ready layouts, and the twelve actions with their
// keys in three columns (moving, fighting, the rest). A key's cell, pressed,
// waits for the next key.
void controls_page(Canvas& c, const View& v, std::vector<Hit>& hits, float y) {
  const float h = 32 + 8 + 44 + 16 + 4 * 44 + 3 * 6 + 18;
  const D2D1_RECT_F in = group(c, v, kLeft, y, kWidth - 80, h, L"КЛАВИАТУРА И МЫШЬ");
  const bool usable = v.keys_usable;
  float gy = in.top + 8;
  for (int i = 0; i < 2; ++i) {
    const bool on = v.keys_player == i;
    const std::wstring name = i ? L"Игрок 2" : L"Игрок 1";
    const D2D1_RECT_F b = rect(in.left + i * (116 + 8), gy, 116, 44);
    if (usable) {
      line_button(c, v, hits, kPlayer1 + i, b.left, b.top, 116, 44, name, false, on ? kInk : kPaper, on ? kBlack : kEdge, nullptr,
                  kGreen, on);
    } else {
      c.frame(b, kLine, 2.0f);
      Type t = type(Face::Text, 13, 700);
      t.align = 1;
      c.text(name, b, t, kFaint);
    }
  }
  {
    Type label = type(Face::Text, 14);
    label.align = 2;
    c.text(L"Раскладка", D2D1::RectF(in.right - 220 - 14 - 160, gy, in.right - 220 - 14, gy + 44), label, usable ? kPaper : kFaint);
    arrows_box(c, v, hits, in.right, gy, v.layout, usable, kLayoutPrev, kLayoutNext);
  }
  gy += 44 + 7;
  c.fill(rect(in.left, gy, in.right - in.left, 2), kLine);
  gy += 2 + 7;
  const float gap = 28, column = (in.right - in.left - 2 * gap) / 3;
  for (int a = 0; a < 12; ++a) {
    const KeyCell& cell = v.keys[a];
    const float x = in.left + (a / 4) * (column + gap), cy = gy + (a % 4) * (44 + 6);
    Type label = type(Face::Text, 14);
    label.ellipsis = true;
    c.text(cell.label, D2D1::RectF(x, cy, x + column - 128 - 10, cy + 44), label, usable ? kPaper : kFaint);
    const D2D1_RECT_F chip = rect(x + column - 128, cy, 128, 44);
    const Touch s = touch(v, kKey0 + a);
    c.fill(chip, cell.waiting ? 0x12261B : 0x0A1310);
    if (usable && !cell.waiting && (s.hover || s.down)) c.fill(chip, 0xFFFFFF, s.down ? 0.12f : 0.06f);
    c.frame(chip, cell.waiting ? v.accent : !usable ? kLine : s.hover || s.down ? kPaper : kEdge, 2.0f);
    Type key = type(Face::Mono, 12, 700);
    key.align = 1;
    key.ellipsis = true;
    c.text(cell.waiting ? L"нажмите…" : cell.key, D2D1::RectF(chip.left + 6, chip.top, chip.right - 6, chip.bottom), key,
           cell.waiting ? v.accent : usable ? kPaper : kFaint);
    if (!usable) continue;
    if (s.focus) focus_ring(c, chip);
    hits.push_back({kKey0 + a, chip});
  }
  page_foot(c, v, hits, y + h + 14);
}

// Under a page's groups: what these settings are, why they cannot be changed
// now or what is waited for; the way back to the defaults.
void page_foot(Canvas& c, const View& v, std::vector<Hit>& hits, float y) {
  const std::wstring reset = L"По умолчанию";
  const float right = kWidth - kLeft, bw = 44 + c.measure(reset, type(Face::Text, 13, 700)).width;
  if (v.can_reset) {
    line_button(c, v, hits, kReset, right - bw, y, bw, 44, reset, false, kPaper, kEdge, nullptr, kPanel, true);
  } else {
    const D2D1_RECT_F r = rect(right - bw, y, bw, 44);
    c.fill(r, kPanel);
    c.frame(r, kLine, 2.0f);
    Type t = type(Face::Text, 13, 700);
    t.align = 1;
    c.text(reset, r, t, kFaint);
  }
  if (v.page_warn) {
    icon(c, kIconWarn, kLeft, y + 13, 18, v.accent, 2.4f);
  } else {  // an "i" in a ring
    c.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(kLeft + 9, y + 22), 6.75f, 6.75f), c.brush(kMuted), 1.65f);
    c.path("M12 11V16.5M12 7.4V7.8", kLeft, y + 13, 0.75f, false, 0, 2.2f, kMuted);
  }
  Type note = type(Face::Text, 12);
  note.wrap = true;
  note.line = 1.45f;
  c.text(v.page_note, D2D1::RectF(kLeft + 28, y, right - bw - 24, y + 44), note, v.page_warn || v.page_bright ? kSoft : kMuted);
}

// ЛАУНЧЕР: its own settings.
void launcher_page(Canvas& c, const View& v, std::vector<Hit>& hits, float y) {
  const float w = (kWidth - 80 - 28) / 2, x2 = kLeft + w + 28, h1 = 196, h2 = 154;
  const Type tiny = type(Face::Text, 12);

  // Updates.
  {
    const D2D1_RECT_F in = group(c, v, kLeft, y, w, h1, L"ОБНОВЛЕНИЯ");
    float gy = in.top + 6;
    toggle(c, v, hits, kSwitchCheck, D2D1::RectF(in.left, gy, in.right, gy + 44), L"Проверять при запуске", v.check_at_start);
    gy += 44 + 4;
    toggle(c, v, hits, kSwitchSelf, D2D1::RectF(in.left, gy, in.right, gy + 44), L"Обновлять лаунчер при запуске", v.auto_self);
    gy += 44 + 4;
    toggle(c, v, hits, kSwitchAuto, D2D1::RectF(in.left, gy, in.right, gy + 44), L"Ставить обновления мода без вопроса",
           v.auto_install);
  }
  // The network.
  {
    const D2D1_RECT_F in = group(c, v, x2, y, w, h1, L"СЕТЬ");
    float gy = in.top + 8;
    c.text(L"Прокси для GitHub — если он не открывается напрямую", D2D1::RectF(in.left, gy, in.right, gy + 16), tiny, kMuted);
    gy += 16 + 8;
    static const wchar_t* kModes[3] = {L"Нет", L"Системный", L"Свой"};
    const float sw = (in.right - in.left - 16) / 3;
    for (int i = 0; i < 3; ++i) {
      const bool on = v.proxy == i;
      line_button(c, v, hits, kProxyNone + i, in.left + i * (sw + 8), gy, sw, 44, kModes[i], false, on ? kInk : kPaper,
                  on ? kBlack : kEdge, nullptr, kGreen, on);
    }
    gy += 44 + 8;
    c.text(L"Адрес своего прокси", D2D1::RectF(in.left, gy, in.right, gy + 16), tiny, kMuted);
    gy += 16 + 8;
    const D2D1_RECT_F field = D2D1::RectF(in.left, gy, in.right, gy + 40);
    const bool own = v.proxy == 2;
    c.fill(field, 0x0A1310);
    c.frame(field, own ? (v.focus == kProxyEdit ? kPaper : kEdge) : kLine, 2.0f);
    if (v.draw_address || !own) {
      Type t = type(Face::Mono, 13, 500);
      t.ellipsis = true;
      const bool empty = v.proxy_address.empty();
      c.text(empty ? L"адрес:порт" : v.proxy_address, D2D1::RectF(field.left + 12, field.top, field.right - 12, field.bottom), t,
             empty || !own ? kFaint : kPaper);
    }
    if (own) hits.push_back({kProxyEdit, field});
  }
  y += h1 + 18;
  // The game.
  {
    const D2D1_RECT_F in = group(c, v, kLeft, y, w, h2, L"ИГРА");
    float gy = in.top + 8;
    game_folder(c, v, hits, gy, in.left, in.right, kPickSettings, 4);
    gy += 44 + 8;
    const float half = (in.right - in.left - 8) / 2;
    line_button(c, v, hits, kOpenLog, in.left, gy, half, 44, L"Открыть журнал", false);
    if (v.can_uninstall)
      line_button(c, v, hits, kUninstall, in.left + half + 8, gy, half, 44, v.uninstall_label, false, kBad, 0xC24A4A);
    else {
      const D2D1_RECT_F r = rect(in.left + half + 8, gy, half, 44);
      c.frame(r, kLine, 2.0f);
      Type t = type(Face::Text, 13, 700);
      t.align = 1;
      c.text(v.uninstall_label, r, t, kFaint);
    }
  }
  // The look.
  {
    const D2D1_RECT_F in = group(c, v, x2, y, w, h2, L"ВИД");
    float gy = in.top + 8;
    c.text(L"Цвет повязки — цвет акцентов лаунчера", D2D1::RectF(in.left, gy, in.right, gy + 16), tiny, kMuted);
    gy += 16 + 14;
    float x = in.left + 8;
    for (int i = 0; i < 4; ++i) {
      const D2D1_RECT_F r = rect(x, gy, 44, 44);
      const Touch s = touch(v, kSwatch0 + i);
      const D2D1_MATRIX_3X2_F lean = c.push(gfx::skew_x(kLean, x + 22, gy + 22));
      if (kAccents[i] == v.accent || s.focus) c.frame(D2D1::RectF(r.left - 6, r.top - 6, r.right + 6, r.bottom + 6), kPaper, 3.0f);
      c.fill(r, kAccents[i]);
      if (s.hover) c.fill(r, 0xFFFFFF, 0.15f);
      c.frame(r, kBlack, 3.0f);
      c.pop(lean);
      hits.push_back({kSwatch0 + i, r});
      x += 44 + 18;
    }
  }
}

void settings(Canvas& c, const View& v, std::vector<Hit>& hits) {
  // Back, the page's name, its tabs.
  float y = 68;
  const float bw = line_button(c, v, hits, kBack, kLeft, y, 0, 44, L"Назад", false, kPaper, kEdge, kIconBack, kPanel, true);
  {
    const float x = kLeft + bw + 18;
    const D2D1_MATRIX_3X2_F lean = c.push(gfx::skew_x(kLean, x, y + 44));
    const Type t = type(Face::Display, 30);
    c.text(L"НАСТРОЙКИ", D2D1::RectF(x + 3, y + 3, x + 500, y + 47), t, kDeep);
    c.text(L"НАСТРОЙКИ", D2D1::RectF(x, y, x + 500, y + 44), t, kPaper);
    c.pop(lean);
  }
  tabs(c, v, hits, y);
  y += 44 + 16;
  if (v.tab == 0) launcher_page(c, v, hits, y);
  else if (v.tab == 3) controls_page(c, v, hits, y);
  else game_page(c, v, hits, y);
}

// --- the bottom line ----------------------------------------------------------------

void footer(Canvas& c, const View& v, std::vector<Hit>& hits) {
  c.fill(rect(0, kMainBottom, kWidth, kHeight - kMainBottom), kBar);
  c.fill(rect(0, kMainBottom, kWidth, 2), kBlack);
  const float top = kMainBottom + 2, bottom = kHeight;
  c.dot(20, (top + bottom) * 0.5f, 4.0f, v.net_color);
  float x = 32;
  x += c.text(v.net, D2D1::RectF(x, top, 520, bottom), type(Face::Text, 12), kSoft).width + 14;
  c.fill(rect(x, (top + bottom) * 0.5f - 7, 1, 14), kLine);
  x += 15;
  {
    const Touch s = touch(v, kRepoLink);
    const float w = c.text(v.repo, D2D1::RectF(x, top, 700, bottom), type(Face::Mono, 12, 500), s.hover ? kPaper : kMuted).width;
    const D2D1_RECT_F r = D2D1::RectF(x - 4, top + 4, x + w + 4, bottom - 4);
    if (s.focus) c.frame(r, kPaper, 2.0f);
    hits.push_back({kRepoLink, r});
  }
  float bx = kWidth - 16 - (4 * 16 + 3 * 3);
  Type right = type(Face::Text, 12);
  right.align = 2;
  c.text(L"Неофициальный фанатский мод · MIT", D2D1::RectF(500, top, bx - 14, bottom), right, kMuted);
  for (uint32_t rgb : kAccents) {
    const D2D1_MATRIX_3X2_F lean = c.push(gfx::skew_x(-24.0f, bx + 8, (top + bottom) * 0.5f));
    c.fill(rect(bx, (top + bottom) * 0.5f - 3, 16, 6), rgb);
    c.pop(lean);
    bx += 19;
  }
}

}  // namespace

void paint(ID2D1RenderTarget* rt, const View& v, std::vector<Hit>& hits) {
  hits.clear();
  Canvas c(rt);
  backdrop(c);
  caption(c, v, hits);
  if (v.settings) settings(c, v, hits);
  else hero(c, v, hits);
  footer(c, v, hits);
}

}  // namespace view
