#pragma once
// Direct2D / DirectWrite for the launcher's window: the three faces from the
// program's resources, text, shapes, paths in SVG's short notation.
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>

#include <cstdint>
#include <string>

namespace gfx {

enum class Face { Display, Text, Mono };  // Dela Gothic One, Onest, JetBrains Mono

struct Type {
  Face face = Face::Text;
  float size = 14.0f;
  int weight = 400;
  float spacing = 0.0f;    // between letters, em
  float line = 0.0f;       // line height, times the size (0: the font's own)
  bool wrap = false;
  bool ellipsis = false;   // a line too long ends with "..."
  bool underline = false;
  int align = 0;           // 0 left, 1 centre, 2 right
  int valign = 1;          // 0 top, 1 centre, 2 bottom
};

bool init(std::wstring* problem = nullptr);  // factories and fonts; false: no Direct2D
void shutdown();
ID2D1Factory* d2d();
std::wstring fonts_report();                 // the families found (lab)

// Draws on one render target, in the window's own units (960 x 600).
class Canvas {
 public:
  explicit Canvas(ID2D1RenderTarget* target);
  ~Canvas();
  Canvas(const Canvas&) = delete;
  Canvas& operator=(const Canvas&) = delete;

  ID2D1RenderTarget* rt;

  ID2D1SolidColorBrush* brush(uint32_t rgb, float alpha = 1.0f);
  void fill(const D2D1_RECT_F& r, uint32_t rgb, float alpha = 1.0f);
  void frame(const D2D1_RECT_F& r, uint32_t rgb, float width, float alpha = 1.0f);  // inside r, like a CSS border
  void dot(float cx, float cy, float radius, uint32_t rgb, float alpha = 1.0f);
  void line(float x0, float y0, float x1, float y1, uint32_t rgb, float width, bool round = true);
  // Text in a box; returns what it took (width, height).
  D2D1_SIZE_F text(const std::wstring& s, const D2D1_RECT_F& box, const Type& t, uint32_t rgb, float alpha = 1.0f);
  D2D1_SIZE_F measure(const std::wstring& s, const Type& t, float max_width = 10000.0f);
  // A path in SVG's "d" notation (M L H V C Z and their relative forms), put
  // at (x, y) and scaled. stroke_width is in the path's own units.
  void path(const char* d, float x, float y, float scale, bool fill, uint32_t fill_rgb, float stroke_width,
            uint32_t stroke_rgb, bool round = true, float alpha = 1.0f);

  // Everything drawn until pop() goes through m as well.
  D2D1_MATRIX_3X2_F push(const D2D1_MATRIX_3X2_F& m);
  void pop(const D2D1_MATRIX_3X2_F& before);

 private:
  ID2D1SolidColorBrush* brush_ = nullptr;
};

inline D2D1_RECT_F rect(float x, float y, float w, float h) { return D2D1::RectF(x, y, x + w, y + h); }
D2D1_MATRIX_3X2_F skew_x(float degrees, float cx, float cy);  // CSS skewX about a point
D2D1_MATRIX_3X2_F rotate(float degrees, float cx, float cy);

// The target's picture as a PNG file (a WIC bitmap target of w x h units at the given scale).
bool render_png(const std::wstring& file, float w, float h, float scale, void (*draw)(ID2D1RenderTarget*, void*), void* ctx);

}  // namespace gfx
