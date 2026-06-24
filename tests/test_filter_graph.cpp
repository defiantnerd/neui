#include "neui_test.h"

// These Tier-1 tests exercise the host-independent filter engine directly
// (neui_detail::evaluate_filter + the fg_* primitive ops in filter_graph.h),
// so they link no host or backend and run everywhere. NEUI_API_FILTER itself
// is an OPTIONAL host extension: a host that can't provide filters (e.g. an
// embedded host with no off-screen surfaces) returns nullptr from
// get_interface(NEUI_API_FILTER), and clients null-check + skip - the engine
// math under test stays valid regardless.

#include "filter_graph.h"  // hosts/shared - SVG fe* model + evaluate_filter

#include <cstdint>
#include <vector>

using namespace neui_test;
using namespace neui_detail;

namespace
{
  // BGRA8 byte offsets within a pixel: [B, G, R, A].
  inline size_t idx(uint32_t x, uint32_t y, uint32_t w) { return (static_cast<size_t>(y) * w + x) * 4u; }

  inline void set_px(std::vector<uint8_t>& buf, uint32_t x, uint32_t y, uint32_t w,
                     uint8_t b, uint8_t g, uint8_t r, uint8_t a)
  {
    uint8_t* p = &buf[idx(x, y, w)];
    p[0] = b; p[1] = g; p[2] = r; p[3] = a;
  }

  inline std::vector<uint8_t> buf_of(uint32_t w, uint32_t h) {
    return std::vector<uint8_t>(static_cast<size_t>(w) * h * 4u, 0u);
  }

  // Fill an inclusive rect [x0,x1]x[y0,y1] with a premultiplied BGRA colour.
  inline void fill_block(std::vector<uint8_t>& b, uint32_t w,
                         uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                         uint8_t bb, uint8_t g, uint8_t r, uint8_t a)
  {
    for (uint32_t y = y0; y <= y1; ++y)
      for (uint32_t x = x0; x <= x1; ++x) set_px(b, x, y, w, bb, g, r, a);
  }
  inline uint8_t R_at(const std::vector<uint8_t>& b, uint32_t x, uint32_t y, uint32_t w) { return b[idx(x,y,w)+2]; }
  inline uint8_t A_at(const std::vector<uint8_t>& b, uint32_t x, uint32_t y, uint32_t w) { return b[idx(x,y,w)+3]; }
}

// ---- Atomic primitive ops (called directly) --------------------------------

TEST_CASE("fe_flood: fills with the premultiplied flood colour")
{
  const uint32_t w = 4, h = 4;
  auto out = buf_of(w, h);
  FilterPrimitive P; P.kind = NEUI_FE_FLOOD;
  P.flood_color = 0xFF0000FFu;  // opaque blue (A=FF,R=00,G=00,B=FF)
  P.flood_opacity = 1.0f;
  fg_flood(out, w, h, P);
  CHECK_EQ(out[0], static_cast<uint8_t>(255));   // B
  CHECK_EQ(out[1], static_cast<uint8_t>(0));     // G
  CHECK_EQ(out[2], static_cast<uint8_t>(0));     // R
  CHECK_EQ(out[3], static_cast<uint8_t>(255));   // A

  // flood-opacity halves the (premultiplied) result.
  P.flood_opacity = 0.5f;
  fg_flood(out, w, h, P);
  CHECK(out[3] > 120 && out[3] < 135);           // A ~127
  CHECK(out[0] > 120 && out[0] < 135);           // premultiplied B ~127
}

TEST_CASE("fe_color_matrix: saturate 0 desaturates to luminance")
{
  const uint32_t w = 1, h = 1;
  auto in = buf_of(w, h);
  set_px(in, 0, 0, w, 0, 0, 255, 255);  // opaque red (premult == straight at A=255)
  auto out = buf_of(w, h);
  FilterPrimitive P; P.kind = NEUI_FE_COLOR_MATRIX; P.cm_type = "saturate";
  P.values = { 0.0f };
  fg_color_matrix(out, in, w, h, P);
  // R contributes 0.213 luma -> all channels equal that, alpha unchanged.
  CHECK_EQ(out[0], out[1]);
  CHECK_EQ(out[1], out[2]);
  CHECK(out[0] > 48 && out[0] < 60);   // ~0.213*255
  CHECK_EQ(out[3], static_cast<uint8_t>(255));
}

TEST_CASE("fe_color_matrix: identity matrix is a no-op")
{
  const uint32_t w = 1, h = 1;
  auto in = buf_of(w, h);
  set_px(in, 0, 0, w, 10, 80, 200, 255);
  auto out = buf_of(w, h);
  FilterPrimitive P; P.kind = NEUI_FE_COLOR_MATRIX; P.cm_type = "matrix";
  P.values = { 1,0,0,0,0, 0,1,0,0,0, 0,0,1,0,0, 0,0,0,1,0 };
  fg_color_matrix(out, in, w, h, P);
  for (int c = 0; c < 4; ++c) CHECK(std::abs(int(out[c]) - int(in[c])) <= 1);
}

TEST_CASE("fe_offset: shifts the source by (dx, dy)")
{
  const uint32_t w = 8, h = 8;
  auto in = buf_of(w, h);
  set_px(in, 2, 2, w, 0, 0, 255, 255);
  auto out = buf_of(w, h);
  FilterPrimitive P; P.kind = NEUI_FE_OFFSET; P.dx = 3; P.dy = 1;
  fg_offset(out, in, w, h, P, /*scale*/ 1.0f);
  CHECK_EQ(out[idx(2, 2, w) + 3], static_cast<uint8_t>(0));   // vacated
  CHECK_EQ(out[idx(5, 3, w) + 3], static_cast<uint8_t>(255)); // moved here
}

TEST_CASE("fe_composite: over keeps opaque src; in masks by dst alpha")
{
  const uint32_t w = 1, h = 1;
  auto red = buf_of(w, h); set_px(red, 0, 0, w, 0, 0, 255, 255);   // opaque red
  auto half = buf_of(w, h); set_px(half, 0, 0, w, 0, 0, 0, 128);   // 50% black

  auto out = buf_of(w, h);
  FilterPrimitive over; over.kind = NEUI_FE_COMPOSITE; over.composite_op = "over";
  fg_composite(out, red, half, w, h, over);     // src=red over dst=half
  CHECK_EQ(out[2], static_cast<uint8_t>(255));  // opaque red wins
  CHECK_EQ(out[3], static_cast<uint8_t>(255));

  FilterPrimitive in_op; in_op.kind = NEUI_FE_COMPOSITE; in_op.composite_op = "in";
  fg_composite(out, red, half, w, h, in_op);    // src=red IN dst -> red * dstA(0.5)
  CHECK(out[2] > 120 && out[2] < 135);          // R ~127
  CHECK(out[3] > 120 && out[3] < 135);          // A ~127
}

TEST_CASE("fe_blend: normal equals over; multiply darkens")
{
  const uint32_t w = 1, h = 1;
  auto white = buf_of(w, h); set_px(white, 0, 0, w, 255, 255, 255, 255);
  auto gray  = buf_of(w, h); set_px(gray,  0, 0, w, 128, 128, 128, 255);

  auto out = buf_of(w, h);
  FilterPrimitive normal; normal.kind = NEUI_FE_BLEND; normal.blend_mode = "normal";
  fg_blend(out, white, gray, w, h, normal);     // white over gray -> white
  CHECK_EQ(out[0], static_cast<uint8_t>(255));

  FilterPrimitive mult; mult.kind = NEUI_FE_BLEND; mult.blend_mode = "multiply";
  fg_blend(out, white, gray, w, h, mult);       // white*gray -> gray
  CHECK(out[0] > 120 && out[0] < 135);
}

// ---- Graph evaluation ------------------------------------------------------

TEST_CASE("evaluate_filter: empty graph leaves pixels unchanged")
{
  const uint32_t w = 4, h = 4;
  auto buf = buf_of(w, h);
  set_px(buf, 1, 1, w, 10, 20, 30, 200);
  auto before = buf;
  FilterAsset fa;
  evaluate_filter(fa, buf.data(), w, h, 1.0f);
  CHECK(buf == before);
}

TEST_CASE("evaluate_filter: default inputs chain prim-to-prim")
{
  // Two offsets with no explicit `in`: first defaults to SourceGraphic, the
  // second to the first's result, so the pixel shifts twice.
  const uint32_t w = 10, h = 4;
  auto buf = buf_of(w, h);
  set_px(buf, 1, 1, w, 0, 0, 255, 255);
  FilterAsset fa;
  uint32_t a = filter_add_primitive(fa, NEUI_FE_OFFSET);
  filter_get_prim(fa, a)->dx = 2;
  uint32_t b = filter_add_primitive(fa, NEUI_FE_OFFSET);
  filter_get_prim(fa, b)->dx = 3;
  evaluate_filter(fa, buf.data(), w, h, 1.0f);
  CHECK_EQ(buf[idx(1, 1, w) + 3], static_cast<uint8_t>(0));   // original gone
  CHECK_EQ(buf[idx(6, 1, w) + 3], static_cast<uint8_t>(255)); // shifted by 2+3
}

TEST_CASE("evaluate_filter: SourceAlpha seeds black with the source alpha")
{
  // A single color_matrix(in=SourceAlpha, identity) yields RGB=0, A=src alpha.
  const uint32_t w = 2, h = 2;
  auto buf = buf_of(w, h);
  set_px(buf, 0, 0, w, 0, 0, 255, 200);   // red, alpha 200
  FilterAsset fa;
  uint32_t m = filter_add_primitive(fa, NEUI_FE_COLOR_MATRIX);
  { auto* P = filter_get_prim(fa, m); P->in = "SourceAlpha"; P->cm_type = "matrix";
    P->values = { 1,0,0,0,0, 0,1,0,0,0, 0,0,1,0,0, 0,0,0,1,0 }; }
  evaluate_filter(fa, buf.data(), w, h, 1.0f);
  CHECK_EQ(buf[idx(0, 0, w) + 2], static_cast<uint8_t>(0));    // R zeroed
  CHECK_EQ(buf[idx(0, 0, w) + 3], static_cast<uint8_t>(200));  // alpha preserved
}

TEST_CASE("evaluate_filter: drop-shadow graph casts an offset halo under the source")
{
  // feColorMatrix(SourceAlpha -> black tint) -> feOffset -> feGaussianBlur
  //   -> feComposite(over, in2 = SourceGraphic). Mirrors drop_shadow_surface.
  const uint32_t w = 16, h = 16;
  auto buf = buf_of(w, h);
  for (uint32_t y = 2; y <= 5; ++y)
    for (uint32_t x = 2; x <= 5; ++x)
      set_px(buf, x, y, w, 0, 0, 255, 255);   // opaque red block

  FilterAsset fa;
  const float cm[20] = { 0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0, 0,0,0,1,0 };  // black, A = srcA
  uint32_t pm = filter_add_primitive(fa, NEUI_FE_COLOR_MATRIX);
  { auto* P = filter_get_prim(fa, pm); P->in = "SourceAlpha"; P->values.assign(cm, cm + 20); }
  uint32_t po = filter_add_primitive(fa, NEUI_FE_OFFSET);
  { auto* P = filter_get_prim(fa, po); P->dx = 3; P->dy = 3; }
  uint32_t pb = filter_add_primitive(fa, NEUI_FE_GAUSSIAN_BLUR);
  { auto* P = filter_get_prim(fa, pb); P->sigma_x = 1.0f; P->sigma_y = 1.0f; }
  uint32_t pc = filter_add_primitive(fa, NEUI_FE_COMPOSITE);
  { auto* P = filter_get_prim(fa, pc); P->composite_op = "over"; P->in = "SourceGraphic"; }

  evaluate_filter(fa, buf.data(), w, h, 1.0f);

  // Source still crisp red on top.
  CHECK_EQ(buf[idx(3, 3, w) + 2], static_cast<uint8_t>(255));  // R
  CHECK_EQ(buf[idx(3, 3, w) + 3], static_cast<uint8_t>(255));  // A
  // Offset region (was transparent) now carries a substantial, neutral-dark
  // shadow: real coverage (alpha well above 0, not a faint sliver), genuinely
  // black (premultiplied R far below its own alpha), and untinted (B == R).
  const uint8_t sa = buf[idx(7, 7, w) + 3];
  CHECK(sa > 64);                                          // not a sliver
  CHECK(buf[idx(7, 7, w) + 2] <= sa / 4);                  // near-black vs its own alpha
  CHECK_EQ(buf[idx(7, 7, w) + 0], buf[idx(7, 7, w) + 2]);  // neutral grey (B == R)
}

// ---- Convenience recipe builders -------------------------------------------

TEST_CASE("filter_build_tint: colourises while keeping coverage")
{
  const uint32_t w = 8, h = 8;
  auto buf = buf_of(w, h);
  fill_block(buf, w, 2, 2, 5, 5, 0, 0, 255, 255);   // opaque red block
  FilterAsset fa; filter_build_tint(fa, 0xFF00FF00u);  // -> green
  evaluate_filter(fa, buf.data(), w, h, 1.0f);
  CHECK_EQ(R_at(buf, 3, 3, w), static_cast<uint8_t>(0));     // R gone
  CHECK_EQ(buf[idx(3, 3, w) + 1], static_cast<uint8_t>(255)); // G full
  CHECK_EQ(A_at(buf, 3, 3, w), static_cast<uint8_t>(255));   // coverage kept
}

TEST_CASE("filter_build_desaturate: amount 1 greys the image")
{
  const uint32_t w = 8, h = 8;
  auto buf = buf_of(w, h);
  fill_block(buf, w, 2, 2, 5, 5, 0, 0, 255, 255);   // opaque red
  FilterAsset fa; filter_build_desaturate(fa, 1.0f);
  evaluate_filter(fa, buf.data(), w, h, 1.0f);
  const uint8_t* p = &buf[idx(3, 3, w)];
  CHECK_EQ(p[0], p[1]);   // B == G
  CHECK_EQ(p[1], p[2]);   // G == R  -> grey
}

TEST_CASE("filter_build_glow: symmetric coloured halo outside the shape")
{
  const uint32_t w = 16, h = 16;
  auto buf = buf_of(w, h);
  fill_block(buf, w, 6, 6, 9, 9, 255, 255, 255, 255);   // opaque white block
  FilterAsset fa; filter_build_glow(fa, 2.0f, 0xFFFF0000u);  // red glow
  evaluate_filter(fa, buf.data(), w, h, 1.0f);
  // Just outside the block was transparent; now a red halo (symmetric, no offset).
  CHECK(A_at(buf, 4, 8, w) > 0);
  CHECK(R_at(buf, 4, 8, w) > 0);
  CHECK_EQ(A_at(buf, 4, 8, w), A_at(buf, 11, 8, w));   // left == right (no offset)
  CHECK(R_at(buf, 8, 8, w) > 200);                     // source still bright
}

TEST_CASE("filter_build_inner_shadow: darkens the interior edge, keeps coverage")
{
  const uint32_t w = 16, h = 16;
  auto buf = buf_of(w, h);
  fill_block(buf, w, 3, 3, 12, 12, 255, 255, 255, 255);  // opaque white block
  FilterAsset fa; filter_build_inner_shadow(fa, 3.0f, 3.0f, 2.0f, 0xFF000000u);
  evaluate_filter(fa, buf.data(), w, h, 1.0f);
  CHECK_EQ(A_at(buf, 8, 8, w), static_cast<uint8_t>(255));   // still inside the shape
  CHECK(R_at(buf, 4, 4, w) < R_at(buf, 8, 8, w));            // edge darker than centre
  CHECK_EQ(A_at(buf, 0, 0, w), static_cast<uint8_t>(0));     // nothing leaks outside
}

TEST_CASE("filter_build_elevation: casts a shadow below the shape")
{
  const uint32_t w = 24, h = 24;
  auto buf = buf_of(w, h);
  fill_block(buf, w, 6, 4, 17, 11, 255, 255, 255, 255);  // opaque white, upper area
  FilterAsset fa; filter_build_elevation(fa, 8.0f);
  evaluate_filter(fa, buf.data(), w, h, 1.0f);
  CHECK(A_at(buf, 11, 16, w) > 0);          // shadow below the block
  CHECK(R_at(buf, 11, 16, w) < 128);        // dark
  CHECK(R_at(buf, 11, 8, w) > 200);         // source preserved on top
}

TEST_CASE("filter_build_bevel: opposite interior edges differ (light vs dark)")
{
  const uint32_t w = 16, h = 16;
  auto buf = buf_of(w, h);
  fill_block(buf, w, 4, 4, 11, 11, 128, 128, 128, 255);  // opaque mid-grey block
  FilterAsset fa; filter_build_bevel(fa, 3.0f, 3.0f, 2.0f, 0xFFFFFFFFu, 0xFF000000u);
  evaluate_filter(fa, buf.data(), w, h, 1.0f);
  CHECK_EQ(A_at(buf, 8, 8, w), static_cast<uint8_t>(255));     // coverage kept
  // A genuine bevel SPLITS the two opposite interior edges relative to the
  // mid-grey (128) fill: one lightens (R above 128), the other darkens (R
  // below). Asserting the split DIRECTION - not just "differ" - catches a
  // light/dark swap (or both bands the same colour). dark=black at +dx/+dy
  // lands top-left; light=white at -dx/-dy lands bottom-right.
  CHECK(R_at(buf, 5, 5, w)   < 128);   // top-left interior: dark band
  CHECK(R_at(buf, 10, 10, w) > 128);   // bottom-right interior: light band
}
