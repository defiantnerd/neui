// Tier-1 coverage for the painter's text metrics + explicit alignment
// (hosts/shared/painter.h). draw_text_aligned does all its work by handing the
// backend a TIGHTENED rect - the backends position a text block by centring it
// in whatever rect they are given - so the whole feature is exactly testable by
// recording the rect that reaches backend->draw_text.
//
// A mock backend supplies deterministic metrics (ascent 8, descent 2,
// line_height 12) and a measure_text of 7 px per char, so every expected
// rect below is arithmetic rather than a golden value.

#include "neui_test.h"

#include "painter.h"

#include <algorithm>

using namespace neui_detail;

namespace {

  constexpr float kAscent  = 8.0f;
  constexpr float kDescent = 2.0f;
  constexpr float kLineH   = 12.0f;
  constexpr float kCharW   = 7.0f;

  struct DrawTextCall {
    float x = 0, y = 0, w = 0, h = 0;
    std::string text;
    float size = 0;
    uint32_t argb = 0;
    int count = 0;
  };

  DrawTextCall g_call;
  int          g_metrics_calls = 0;

  void NEUI_ABI mock_draw_text(neui_render_ctx_t, float x, float y,
                                float w, float h, const char* text,
                                float font_size, uint32_t argb)
  {
    g_call.x = x; g_call.y = y; g_call.w = w; g_call.h = h;
    g_call.text = text ? text : "";
    g_call.size = font_size; g_call.argb = argb;
    ++g_call.count;
  }

  float NEUI_ABI mock_measure_text(neui_render_ctx_t, const char* text,
                                    int text_len, float)
  {
    if (!text) return 0.0f;
    size_t n = (text_len < 0) ? strlen(text) : (size_t)text_len;
    return kCharW * (float)n;
  }

  void NEUI_ABI mock_font_metrics(neui_render_ctx_t, float,
                                   float* a, float* d, float* lh)
  {
    ++g_metrics_calls;
    if (a)  *a  = kAscent;
    if (d)  *d  = kDescent;
    if (lh) *lh = kLineH;
  }

  neui_render_ctx_t const k_ctx = (neui_render_ctx_t)0x1234;

  // Fresh painter + backend per case, with the recorder reset.
  struct Fixture {
    neui_render_backend_t backend{};
    neui_painter          p{};
    Fixture()
    {
      backend.draw_text    = mock_draw_text;
      backend.measure_text  = mock_measure_text;
      backend.font_metrics  = mock_font_metrics;
      p.backend = &backend;
      p.ctx     = k_ctx;
      g_call = DrawTextCall{};
      g_metrics_calls = 0;
    }
  };

} // namespace

// ---------------------------------------------------------------------------
// font_metrics forwarding
// ---------------------------------------------------------------------------

TEST_CASE("painter_font_metrics forwards the backend's numbers")
{
  Fixture f;
  float a = -1, d = -1, lh = -1;
  painter_font_metrics(&f.p, 20.0f, &a, &d, &lh);
  CHECK_APPROX(a, (double)kAscent);
  CHECK_APPROX(d, (double)kDescent);
  CHECK_APPROX(lh, (double)kLineH);
}

TEST_CASE("painter_font_metrics zeroes its outputs when unsupported")
{
  Fixture f;
  f.backend.font_metrics = nullptr;      // backend predating the vtable append
  float a = 99, d = 99, lh = 99;
  painter_font_metrics(&f.p, 20.0f, &a, &d, &lh);
  CHECK_APPROX(a, 0.0);
  CHECK_APPROX(d, 0.0);
  CHECK_APPROX(lh, 0.0);
}

TEST_CASE("painter_font_metrics tolerates NULL out-pointers and a NULL painter")
{
  Fixture f;
  painter_font_metrics(&f.p, 20.0f, nullptr, nullptr, nullptr);
  painter_font_metrics(nullptr, 20.0f, nullptr, nullptr, nullptr);
  CHECK_EQ(g_metrics_calls, 1);          // only the valid painter reached it
}

// ---------------------------------------------------------------------------
// Line counting + block width
// ---------------------------------------------------------------------------

TEST_CASE("painter_text_line_count counts '\\n', matching the backends' split")
{
  CHECK_EQ(painter_text_line_count(""), 1);
  CHECK_EQ(painter_text_line_count("one"), 1);
  CHECK_EQ(painter_text_line_count("one\ntwo"), 2);
  CHECK_EQ(painter_text_line_count("a\nb\nc"), 3);
  CHECK_EQ(painter_text_line_count("trailing\n"), 2);   // opens another line
  CHECK_EQ(painter_text_line_count("crlf\r\nb"), 2);
}

TEST_CASE("painter_text_block_width takes the WIDEST line, not the first")
{
  Fixture f;
  // "ab" = 14, "abcde" = 35 -> the block is 35 wide.
  CHECK_APPROX(painter_text_block_width(&f.p, "ab\nabcde", 20.0f), 35.0);
  CHECK_APPROX(painter_text_block_width(&f.p, "abcde\nab", 20.0f), 35.0);
}

TEST_CASE("painter_text_block_width strips the CR of a CRLF before measuring")
{
  Fixture f;
  // Without the strip the '\r' would count as a 7px glyph -> 21 instead of 14.
  CHECK_APPROX(painter_text_block_width(&f.p, "ab\r\nab", 20.0f), 14.0);
}

// ---------------------------------------------------------------------------
// Horizontal alignment. The rect's RIGHT edge must stay put so overflow keeps
// clipping to the widget rather than to the text width.
// ---------------------------------------------------------------------------

TEST_CASE("halign START is byte-identical to plain draw_text")
{
  Fixture f;
  painter_draw_text_aligned(&f.p, 10, 20, 100, 40, "abc", 20.0f, 0xFF112233,
                             NEUI_TEXT_ALIGN_START, NEUI_TEXT_VALIGN_MIDDLE);
  CHECK_EQ(g_call.count, 1);
  CHECK_APPROX(g_call.x, 10.0);
  CHECK_APPROX(g_call.y, 20.0);
  CHECK_APPROX(g_call.w, 100.0);
  CHECK_APPROX(g_call.h, 40.0);
  CHECK_EQ(g_call.argb, 0xFF112233u);
  // START + MIDDLE must not even ask for metrics - it is the old path.
  CHECK_EQ(g_metrics_calls, 0);
}

TEST_CASE("halign CENTER centres the block and keeps the right clip edge")
{
  Fixture f;
  // "abc" = 21 wide in a 100-wide rect -> x = 10 + (100-21)/2 = 49.5
  painter_draw_text_aligned(&f.p, 10, 20, 100, 40, "abc", 20.0f, 0xFF000000,
                             NEUI_TEXT_ALIGN_CENTER, NEUI_TEXT_VALIGN_MIDDLE);
  CHECK_APPROX(g_call.x, 49.5);
  CHECK_APPROX(g_call.w, 60.5);          // right edge still 110
  CHECK_APPROX(g_call.x + g_call.w, 110.0);
}

TEST_CASE("halign END right-aligns the block")
{
  Fixture f;
  painter_draw_text_aligned(&f.p, 10, 20, 100, 40, "abc", 20.0f, 0xFF000000,
                             NEUI_TEXT_ALIGN_END, NEUI_TEXT_VALIGN_MIDDLE);
  CHECK_APPROX(g_call.x, 89.0);          // 10 + 100 - 21
  CHECK_APPROX(g_call.w, 21.0);
  CHECK_APPROX(g_call.x + g_call.w, 110.0);
}

TEST_CASE("halign centres on the WIDEST line for multi-line text")
{
  Fixture f;
  // widest = "abcde" = 35 -> x = 10 + (100-35)/2 = 42.5
  painter_draw_text_aligned(&f.p, 10, 20, 100, 40, "ab\nabcde", 20.0f, 0xFF0,
                             NEUI_TEXT_ALIGN_CENTER, NEUI_TEXT_VALIGN_MIDDLE);
  CHECK_APPROX(g_call.x, 42.5);
}

TEST_CASE("halign: over-long text pins to the left edge, never outside it")
{
  Fixture f;
  // 10 chars = 70 px in a 30-wide rect. Centring would give x = 10-20 = -10,
  // drawing (and clipping) outside the widget. Must clamp to x.
  painter_draw_text_aligned(&f.p, 10, 20, 30, 40, "abcdefghij", 20.0f, 0xFF0,
                             NEUI_TEXT_ALIGN_CENTER, NEUI_TEXT_VALIGN_MIDDLE);
  CHECK_APPROX(g_call.x, 10.0);
  CHECK_APPROX(g_call.w, 30.0);          // full rect, so it clips at the widget

  Fixture f2;
  painter_draw_text_aligned(&f2.p, 10, 20, 30, 40, "abcdefghij", 20.0f, 0xFF0,
                             NEUI_TEXT_ALIGN_END, NEUI_TEXT_VALIGN_MIDDLE);
  CHECK_APPROX(g_call.x, 10.0);
  CHECK_APPROX(g_call.w, 30.0);
}

TEST_CASE("halign falls back to START when the backend cannot measure")
{
  Fixture f;
  f.backend.measure_text = nullptr;
  painter_draw_text_aligned(&f.p, 10, 20, 100, 40, "abc", 20.0f, 0xFF0,
                             NEUI_TEXT_ALIGN_CENTER, NEUI_TEXT_VALIGN_MIDDLE);
  CHECK_APPROX(g_call.x, 10.0);          // unshifted
  CHECK_APPROX(g_call.w, 100.0);
}

// ---------------------------------------------------------------------------
// Vertical alignment. Tightening the rect to the block height is what makes
// the backends' own centring land the block exactly where we asked.
// ---------------------------------------------------------------------------

TEST_CASE("valign TOP tightens the rect to the block height at the top edge")
{
  Fixture f;
  painter_draw_text_aligned(&f.p, 10, 20, 100, 40, "abc", 20.0f, 0xFF0,
                             NEUI_TEXT_ALIGN_START, NEUI_TEXT_VALIGN_TOP);
  // One line -> block 12. Centred in (20, 12) puts the block top at 20.
  CHECK_APPROX(g_call.y, 20.0);
  CHECK_APPROX(g_call.h, 12.0);
}

TEST_CASE("valign BOTTOM puts the block against the bottom edge")
{
  Fixture f;
  painter_draw_text_aligned(&f.p, 10, 20, 100, 40, "abc", 20.0f, 0xFF0,
                             NEUI_TEXT_ALIGN_START, NEUI_TEXT_VALIGN_BOTTOM);
  // y + h - block = 20 + 40 - 12 = 48, and the block bottom lands on 60.
  CHECK_APPROX(g_call.y, 48.0);
  CHECK_APPROX(g_call.h, 12.0);
  CHECK_APPROX(g_call.y + g_call.h, 60.0);
}

TEST_CASE("valign accounts for every line of a multi-line block")
{
  Fixture f;
  painter_draw_text_aligned(&f.p, 10, 20, 100, 60, "a\nb\nc", 20.0f, 0xFF0,
                             NEUI_TEXT_ALIGN_START, NEUI_TEXT_VALIGN_BOTTOM);
  // 3 lines * 12 = 36 -> y = 20 + 60 - 36 = 44.
  CHECK_APPROX(g_call.y, 44.0);
  CHECK_APPROX(g_call.h, 36.0);
}

TEST_CASE("valign: a block taller than the rect pins to the top and clips")
{
  Fixture f;
  // 4 lines * 12 = 48 > 30. Must not push y negative or grow h past the rect.
  painter_draw_text_aligned(&f.p, 10, 20, 100, 30, "a\nb\nc\nd", 20.0f, 0xFF0,
                             NEUI_TEXT_ALIGN_START, NEUI_TEXT_VALIGN_BOTTOM);
  CHECK_APPROX(g_call.y, 20.0);
  CHECK_APPROX(g_call.h, 30.0);
}

TEST_CASE("valign falls back to MIDDLE when the backend has no font_metrics")
{
  Fixture f;
  f.backend.font_metrics = nullptr;      // backend predating the vtable append
  painter_draw_text_aligned(&f.p, 10, 20, 100, 40, "abc", 20.0f, 0xFF0,
                             NEUI_TEXT_ALIGN_START, NEUI_TEXT_VALIGN_TOP);
  CHECK_APPROX(g_call.y, 20.0);          // untightened -> old centring
  CHECK_APPROX(g_call.h, 40.0);
}

TEST_CASE("baseline placement: VALIGN_TOP puts the baseline `ascent` down")
{
  // The documented recipe for baseline control, since there is no BASELINE
  // mode: align TOP, then the first line's baseline is at rect_top + ascent.
  Fixture f;
  float asc = 0;
  painter_font_metrics(&f.p, 20.0f, &asc, nullptr, nullptr);
  const float want_baseline = 100.0f;
  painter_draw_text_aligned(&f.p, 0, want_baseline - asc, 100, 40, "abc",
                             20.0f, 0xFF0,
                             NEUI_TEXT_ALIGN_START, NEUI_TEXT_VALIGN_TOP);
  CHECK_APPROX(g_call.y + asc, (double)want_baseline);
}

// ---------------------------------------------------------------------------
// Combined + degenerate inputs
// ---------------------------------------------------------------------------

TEST_CASE("halign and valign compose independently")
{
  Fixture f;
  painter_draw_text_aligned(&f.p, 10, 20, 100, 40, "abc", 20.0f, 0xFF0,
                             NEUI_TEXT_ALIGN_END, NEUI_TEXT_VALIGN_TOP);
  CHECK_APPROX(g_call.x, 89.0);
  CHECK_APPROX(g_call.y, 20.0);
  CHECK_APPROX(g_call.h, 12.0);
}

TEST_CASE("draw_text_aligned: empty / NULL text and NULL painter draw nothing")
{
  Fixture f;
  painter_draw_text_aligned(&f.p, 10, 20, 100, 40, "", 20.0f, 0xFF0,
                             NEUI_TEXT_ALIGN_CENTER, NEUI_TEXT_VALIGN_TOP);
  painter_draw_text_aligned(&f.p, 10, 20, 100, 40, nullptr, 20.0f, 0xFF0,
                             NEUI_TEXT_ALIGN_CENTER, NEUI_TEXT_VALIGN_TOP);
  painter_draw_text_aligned(nullptr, 10, 20, 100, 40, "abc", 20.0f, 0xFF0,
                             NEUI_TEXT_ALIGN_CENTER, NEUI_TEXT_VALIGN_TOP);
  CHECK_EQ(g_call.count, 0);
}

TEST_CASE("draw_text_aligned: a backend with no draw_text is a safe no-op")
{
  Fixture f;
  f.backend.draw_text = nullptr;
  painter_draw_text_aligned(&f.p, 10, 20, 100, 40, "abc", 20.0f, 0xFF0,
                             NEUI_TEXT_ALIGN_CENTER, NEUI_TEXT_VALIGN_TOP);
  CHECK_EQ(g_call.count, 0);
}

TEST_CASE("the appended painter vtable slots are populated")
{
  // Guards the positional vtable: a missed entry would land these on the
  // wrong function rather than failing to compile.
  CHECK(k_painter_api.font_metrics == painter_font_metrics);
  CHECK(k_painter_api.draw_text_aligned == painter_draw_text_aligned);
}

// ---------------------------------------------------------------------------
// Convenience shapes (1.2). Recorded through the path API, since none of them
// touches a backend primitive - the point is that the emitted geometry is right.
// ---------------------------------------------------------------------------

namespace {

  struct PathLog {
    std::string ops;                 // 'B'egin 'M'ove 'L'ine 'A'rc 'C'ubic 'X' close
    std::vector<float> xs, ys;
    int fills = 0, strokes = 0;
    float last_stroke_w = 0;
    uint32_t last_argb = 0;
  };
  PathLog g_path;

  void NEUI_ABI lg_begin(neui_render_ctx_t) { g_path.ops += 'B'; }
  void NEUI_ABI lg_move(neui_render_ctx_t, float x, float y)
  { g_path.ops += 'M'; g_path.xs.push_back(x); g_path.ys.push_back(y); }
  void NEUI_ABI lg_line(neui_render_ctx_t, float x, float y)
  { g_path.ops += 'L'; g_path.xs.push_back(x); g_path.ys.push_back(y); }
  void NEUI_ABI lg_arc(neui_render_ctx_t, float, float, float, float, float)
  { g_path.ops += 'A'; }
  void NEUI_ABI lg_cubic(neui_render_ctx_t, float, float, float, float,
                          float x, float y)
  { g_path.ops += 'C'; g_path.xs.push_back(x); g_path.ys.push_back(y); }
  void NEUI_ABI lg_close(neui_render_ctx_t) { g_path.ops += 'X'; }
  void NEUI_ABI lg_fill(neui_render_ctx_t, uint32_t argb)
  { ++g_path.fills; g_path.last_argb = argb; }
  void NEUI_ABI lg_stroke(neui_render_ctx_t, float w, uint32_t argb)
  { ++g_path.strokes; g_path.last_stroke_w = w; g_path.last_argb = argb; }

  struct ShapeFixture {
    neui_render_backend_t backend{};
    neui_painter          p{};
    ShapeFixture()
    {
      backend.begin_path  = lg_begin;
      backend.move_to     = lg_move;
      backend.line_to     = lg_line;
      backend.arc         = lg_arc;
      backend.cubic_to    = lg_cubic;
      backend.close_path  = lg_close;
      backend.fill_path   = lg_fill;
      backend.stroke_path = lg_stroke;
      p.backend = &backend;
      p.ctx     = k_ctx;
      g_path = PathLog{};
    }
  };

  float minv(const std::vector<float>& v)
  { float m = v.empty() ? 0.f : v[0]; for (float f : v) if (f < m) m = f; return m; }
  float maxv(const std::vector<float>& v)
  { float m = v.empty() ? 0.f : v[0]; for (float f : v) if (f > m) m = f; return m; }

} // namespace

TEST_CASE("fill_round_rect with radius 0 emits a plain 4-line rectangle")
{
  ShapeFixture f;
  painter_fill_round_rect(&f.p, 10, 20, 100, 50, 0.0f, 0xFF334455);
  CHECK_EQ(g_path.ops, std::string("BMLLLX"));
  CHECK_EQ(g_path.fills, 1);
  CHECK_EQ(g_path.last_argb, 0xFF334455u);
  CHECK_APPROX(minv(g_path.xs), 10.0);
  CHECK_APPROX(maxv(g_path.xs), 110.0);
  CHECK_APPROX(minv(g_path.ys), 20.0);
  CHECK_APPROX(maxv(g_path.ys), 70.0);
}

TEST_CASE("fill_round_rect with a radius emits four arcs and stays in bounds")
{
  ShapeFixture f;
  painter_fill_round_rect(&f.p, 10, 20, 100, 50, 8.0f, 0xFF000000);
  // move, then (arc,line) x4 minus the final line, closed.
  CHECK_EQ(g_path.ops, std::string("BMALALALAX"));
  CHECK_EQ(g_path.fills, 1);
  CHECK(minv(g_path.xs) >= 10.0f - 0.01f);
  CHECK(maxv(g_path.xs) <= 110.0f + 0.01f);
  CHECK(minv(g_path.ys) >= 20.0f - 0.01f);
  CHECK(maxv(g_path.ys) <= 70.0f + 0.01f);
}

TEST_CASE("round rect radius clamps to half the shorter side")
{
  // A wildly over-large radius must degrade to a stadium, not corrupt the
  // outline by pushing control points past the rect.
  ShapeFixture f;
  painter_fill_round_rect(&f.p, 0, 0, 100, 40, 999.0f, 0xFF0);
  CHECK(minv(g_path.xs) >= -0.01f);
  CHECK(maxv(g_path.xs) <= 100.01f);
  CHECK(minv(g_path.ys) >= -0.01f);
  CHECK(maxv(g_path.ys) <= 40.01f);
}

TEST_CASE("draw_round_rect strokes rather than fills, forwarding the width")
{
  ShapeFixture f;
  painter_draw_round_rect(&f.p, 10, 20, 100, 50, 6.0f, 2.5f, 0xFFAABBCC);
  CHECK_EQ(g_path.fills, 0);
  CHECK_EQ(g_path.strokes, 1);
  CHECK_APPROX(g_path.last_stroke_w, 2.5);
  CHECK_EQ(g_path.last_argb, 0xFFAABBCCu);
}

TEST_CASE("fill_ellipse inscribes the rect and closes the path")
{
  ShapeFixture f;
  painter_fill_ellipse(&f.p, 10, 20, 100, 40, 0xFF0);
  CHECK_EQ(g_path.ops.front(), 'B');
  CHECK_EQ(g_path.ops[1], 'M');
  CHECK_EQ(g_path.ops.back(), 'X');
  CHECK_EQ(g_path.fills, 1);
  // Starts at angle 0 == the right-hand vertex.
  CHECK_APPROX(g_path.xs[0], 110.0);
  CHECK_APPROX(g_path.ys[0], 40.0);
  // Full sweep -> 4 cubic segments, all inside the rect.
  CHECK_EQ((int)std::count(g_path.ops.begin(), g_path.ops.end(), 'C'), 4);
  CHECK(minv(g_path.xs) >= 10.0f - 0.01f);
  CHECK(maxv(g_path.xs) <= 110.0f + 0.01f);
  CHECK(minv(g_path.ys) >= 20.0f - 0.01f);
  CHECK(maxv(g_path.ys) <= 60.0f + 0.01f);
}

TEST_CASE("fill_ellipse on a square rect is a circle through all four vertices")
{
  ShapeFixture f;
  painter_fill_ellipse(&f.p, 0, 0, 100, 100, 0xFF0);
  // The 4 cubic endpoints are the cardinal points, in clockwise screen order
  // from 3 o'clock: (100,50) start, then (50,100), (0,50), (50,0), (100,50).
  CHECK_APPROX(g_path.xs[0], 100.0); CHECK_APPROX(g_path.ys[0], 50.0);
  CHECK_APPROX(g_path.xs[1], 50.0);  CHECK_APPROX(g_path.ys[1], 100.0);
  CHECK_APPROX(g_path.xs[2], 0.0);   CHECK_APPROX(g_path.ys[2], 50.0);
  CHECK_APPROX(g_path.xs[3], 50.0);  CHECK_APPROX(g_path.ys[3], 0.0);
  CHECK_APPROX(g_path.xs[4], 100.0); CHECK_APPROX(g_path.ys[4], 50.0);
}

TEST_CASE("draw_ellipse strokes with the given width")
{
  ShapeFixture f;
  painter_draw_ellipse(&f.p, 0, 0, 50, 50, 3.0f, 0xFF112233);
  CHECK_EQ(g_path.fills, 0);
  CHECK_EQ(g_path.strokes, 1);
  CHECK_APPROX(g_path.last_stroke_w, 3.0);
}

TEST_CASE("draw_line emits exactly move+line and does NOT close the path")
{
  // Closing a 2-point path can make a backend draw a join where the line cap
  // belongs, so the open path is load-bearing, not incidental.
  ShapeFixture f;
  painter_draw_line(&f.p, 5, 6, 25, 36, 1.5f, 0xFF999999);
  CHECK_EQ(g_path.ops, std::string("BML"));
  CHECK_EQ(g_path.strokes, 1);
  CHECK_APPROX(g_path.xs[0], 5.0);  CHECK_APPROX(g_path.ys[0], 6.0);
  CHECK_APPROX(g_path.xs[1], 25.0); CHECK_APPROX(g_path.ys[1], 36.0);
  CHECK_APPROX(g_path.last_stroke_w, 1.5);
}

TEST_CASE("shapes: degenerate sizes and a NULL painter draw nothing")
{
  ShapeFixture f;
  painter_fill_round_rect(&f.p, 0, 0, 0, 10, 2.0f, 0xFF0);
  painter_fill_round_rect(&f.p, 0, 0, 10, -1, 2.0f, 0xFF0);
  painter_fill_ellipse(&f.p, 0, 0, 0, 0, 0xFF0);
  painter_draw_ellipse(&f.p, 0, 0, -5, 10, 1.0f, 0xFF0);
  painter_fill_round_rect(nullptr, 0, 0, 10, 10, 2.0f, 0xFF0);
  painter_draw_line(nullptr, 0, 0, 1, 1, 1.0f, 0xFF0);
  CHECK_EQ(g_path.fills, 0);
  CHECK_EQ(g_path.strokes, 0);
  CHECK(g_path.ops.empty());
}

TEST_CASE("the appended shape vtable slots are populated")
{
  CHECK(k_painter_api.fill_round_rect == painter_fill_round_rect);
  CHECK(k_painter_api.draw_round_rect == painter_draw_round_rect);
  CHECK(k_painter_api.fill_ellipse    == painter_fill_ellipse);
  CHECK(k_painter_api.draw_ellipse    == painter_draw_ellipse);
  CHECK(k_painter_api.draw_line       == painter_draw_line);
}

// ---------------------------------------------------------------------------
// Font style axis (1.3). The interesting part is the fallback: pop_font is
// unconditional, so a backend predating push_font_styled must still get SOME
// push or the client's balanced push/pop pair would underflow the stack.
// ---------------------------------------------------------------------------

namespace {

  struct FontPush {
    std::string family;
    int  weight = -1;
    bool italic = false;
    bool via_styled = false;
    int  count = 0;
  };
  FontPush g_font;

  void NEUI_ABI lg_push_font(neui_render_ctx_t, const char* fam, int weight)
  {
    g_font.family = fam ? fam : "";
    g_font.weight = weight;
    g_font.italic = false;
    g_font.via_styled = false;
    ++g_font.count;
  }
  void NEUI_ABI lg_push_font_styled(neui_render_ctx_t, const char* fam,
                                     int weight, bool italic)
  {
    g_font.family = fam ? fam : "";
    g_font.weight = weight;
    g_font.italic = italic;
    g_font.via_styled = true;
    ++g_font.count;
  }

  struct FontFixture {
    neui_render_backend_t backend{};
    neui_painter          p{};
    FontFixture()
    {
      backend.push_font        = lg_push_font;
      backend.push_font_styled = lg_push_font_styled;
      p.backend = &backend;
      p.ctx     = k_ctx;
      g_font = FontPush{};
    }
  };

} // namespace

TEST_CASE("push_font_styled forwards the italic flag")
{
  FontFixture f;
  painter_push_font_styled(&f.p, "Inter", 600, true);
  CHECK_EQ(g_font.count, 1);
  CHECK(g_font.via_styled);
  CHECK_EQ(g_font.family, std::string("Inter"));
  CHECK_EQ(g_font.weight, 600);
  CHECK(g_font.italic);
}

TEST_CASE("push_font_styled(..., false) is the upright request")
{
  FontFixture f;
  painter_push_font_styled(&f.p, "Inter", 400, false);
  CHECK(g_font.via_styled);
  CHECK_FALSE(g_font.italic);
}

TEST_CASE("push_font_styled degrades to push_font on an older backend")
{
  // Critical for stack balance: the client will call pop_font regardless, so
  // dropping the push entirely would underflow the backend's font stack.
  FontFixture f;
  f.backend.push_font_styled = nullptr;
  painter_push_font_styled(&f.p, "Inter", 600, true);
  CHECK_EQ(g_font.count, 1);
  CHECK_FALSE(g_font.via_styled);        // took the plain path
  CHECK_EQ(g_font.family, std::string("Inter"));
  CHECK_EQ(g_font.weight, 600);          // family + weight still honoured
}

TEST_CASE("push_font_styled: NULL painter / backend without either entry is inert")
{
  FontFixture f;
  f.backend.push_font_styled = nullptr;
  f.backend.push_font        = nullptr;
  painter_push_font_styled(&f.p, "Inter", 400, true);
  painter_push_font_styled(nullptr, "Inter", 400, true);
  CHECK_EQ(g_font.count, 0);
}

TEST_CASE("the appended font-style vtable slot is populated")
{
  CHECK(k_painter_api.push_font_styled == painter_push_font_styled);
}
