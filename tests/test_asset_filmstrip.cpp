#include "neui_test.h"

// Tier-1 coverage for filmstrip / stitchmap frame-strip assets in
// AssetStore<Loader> (hosts/shared/asset_store.h) + the frame-aware draw
// helper (hosts/shared/painter.h). A filmstrip is a BITMAP/SURFACE asset
// tagged with a cols x rows grid; the value -> frame mapping samples a cell
// sub-rect from the single cached GPU upload of the whole strip.
//
// Uses a fake image loader + a fake backend recording create/draw_bitmap
// traffic, so the portable tag -> frame-rect -> draw path is exercised with
// no real backend / host linked.

#include "asset_store.h"
#include "painter.h"

#include <cstdlib>
#include <cstring>

using namespace neui_detail;

namespace {

// --- Fake image loader: filmstrips are tagged onto already-loaded bitmaps,
// so allocate_bitmap is used directly and the loader is never invoked. ------
struct FakeLoader {
  static uint8_t* load(const char*, uint32_t*, uint32_t*) { return nullptr; }
  static void     free_pixels(uint8_t*) {}
};

// --- Sized loader: returns a fixed-dimension heap buffer for any path, so
// allocate_filmstrip_from_file's load + tag + cleanup path can be exercised
// without real files (resolve_path probes only via Loader::load). -----------
struct SizedLoader {
  static uint32_t s_w, s_h;
  static uint8_t* load(const char*, uint32_t* w, uint32_t* h) {
    if (w) *w = s_w;
    if (h) *h = s_h;
    return static_cast<uint8_t*>(std::calloc((size_t)s_w * s_h * 4u, 1));
  }
  static void free_pixels(uint8_t* p) { std::free(p); }
};
uint32_t SizedLoader::s_w = 0;
uint32_t SizedLoader::s_h = 0;

// --- Fake backend recording the bitmap upload + draw sub-rect. -------------
int   g_create_calls = 0;
int   g_draw_calls   = 0;
float g_last_src[4]  = { -1, -1, -1, -1 };   // sx, sy, sw, sh
float g_last_dst[4]  = { -1, -1, -1, -1 };   // dx, dy, dw, dh

void* fake_create_bitmap(neui_render_ctx_t, uint32_t, uint32_t,
                         const uint8_t*, float) {
  ++g_create_calls;
  return reinterpret_cast<void*>(0xB14u);    // any non-null handle
}
void fake_destroy_bitmap(neui_render_ctx_t, void*) {}
void fake_draw_bitmap(neui_render_ctx_t, void*,
                      float sx, float sy, float sw, float sh,
                      float dx, float dy, float dw, float dh, uint32_t) {
  ++g_draw_calls;
  g_last_src[0] = sx; g_last_src[1] = sy; g_last_src[2] = sw; g_last_src[3] = sh;
  g_last_dst[0] = dx; g_last_dst[1] = dy; g_last_dst[2] = dw; g_last_dst[3] = dh;
}

neui_render_backend_t make_backend() {
  neui_render_backend_t b{};
  b.create_bitmap  = fake_create_bitmap;
  b.destroy_bitmap = fake_destroy_bitmap;
  b.draw_bitmap    = fake_draw_bitmap;
  return b;
}

void reset_counters() {
  g_create_calls = g_draw_calls = 0;
  for (int i = 0; i < 4; ++i) { g_last_src[i] = g_last_dst[i] = -1; }
}

// Allocate a w x h dummy BITMAP (pixels are never inspected by the fakes).
uint32_t make_bitmap(AssetStore<FakeLoader>& store, uint32_t w, uint32_t h) {
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4u, 0xAB);
  return store.allocate_bitmap(w, h, px.data(), 1.0f);
}

neui_render_ctx_t k_ctx = reinterpret_cast<neui_render_ctx_t>(0x100u);

} // namespace

TEST_CASE("filmstrip: vertical strip tags frame size + count")
{
  AssetStore<FakeLoader> store;
  uint32_t slot = make_bitmap(store, 64, 6400);     // 1 x 100 vertical strip
  REQUIRE(slot != 0);

  CHECK(store.set_frame_layout(slot, /*cols=*/1, /*rows=*/100, /*gutter=*/0));
  const FilmstripInfo* fs = store.frame_info(slot);
  REQUIRE(fs != nullptr);
  CHECK_EQ((int)fs->frame_count, 100);
  CHECK_EQ((int)fs->cols, 1);
  CHECK_EQ((int)fs->rows, 100);
  CHECK_EQ((int)fs->frame_w_px, 64);
  CHECK_EQ((int)fs->frame_h_px, 64);
  CHECK_EQ((int)store.frame_count(slot), 100);
}

TEST_CASE("filmstrip: frame_src_rect walks cells row-major")
{
  AssetStore<FakeLoader> store;
  uint32_t slot = make_bitmap(store, 64, 6400);
  REQUIRE(store.set_frame_layout(slot, 1, 100, 0));

  float sx, sy, sw, sh;
  REQUIRE(store.frame_src_rect(slot, 0, &sx, &sy, &sw, &sh));
  CHECK_EQ((int)sx, 0);  CHECK_EQ((int)sy, 0);
  CHECK_EQ((int)sw, 64); CHECK_EQ((int)sh, 64);

  REQUIRE(store.frame_src_rect(slot, 1, &sx, &sy, &sw, &sh));
  CHECK_EQ((int)sy, 64);                          // second cell down

  REQUIRE(store.frame_src_rect(slot, 99, &sx, &sy, &sw, &sh));
  CHECK_EQ((int)sy, 99 * 64);                      // last cell
}

TEST_CASE("filmstrip: 2D grid is row-major (cols vary fastest)")
{
  AssetStore<FakeLoader> store;
  uint32_t slot = make_bitmap(store, 120, 80);     // 3 cols x 2 rows, 40x40
  REQUIRE(store.set_frame_layout(slot, 3, 2, 0));
  const FilmstripInfo* fs = store.frame_info(slot);
  REQUIRE(fs != nullptr);
  CHECK_EQ((int)fs->frame_count, 6);
  CHECK_EQ((int)fs->frame_w_px, 40);
  CHECK_EQ((int)fs->frame_h_px, 40);

  float sx, sy, sw, sh;
  store.frame_src_rect(slot, 0, &sx, &sy, &sw, &sh);   // (col0, row0)
  CHECK_EQ((int)sx, 0);   CHECK_EQ((int)sy, 0);
  store.frame_src_rect(slot, 2, &sx, &sy, &sw, &sh);   // (col2, row0)
  CHECK_EQ((int)sx, 80);  CHECK_EQ((int)sy, 0);
  store.frame_src_rect(slot, 3, &sx, &sy, &sw, &sh);   // wraps to (col0, row1)
  CHECK_EQ((int)sx, 0);   CHECK_EQ((int)sy, 40);
  store.frame_src_rect(slot, 5, &sx, &sy, &sw, &sh);   // (col2, row1)
  CHECK_EQ((int)sx, 80);  CHECK_EQ((int)sy, 40);
}

TEST_CASE("filmstrip: gutter spacing between cells")
{
  AssetStore<FakeLoader> store;
  // 4 cells across, 2px gutters between => width = 4*fw + 3*2.
  // width 86, gutter 2 => fw = (86 - 6) / 4 = 20.
  uint32_t slot = make_bitmap(store, 86, 20);
  REQUIRE(store.set_frame_layout(slot, 4, 1, 2));
  const FilmstripInfo* fs = store.frame_info(slot);
  REQUIRE(fs != nullptr);
  CHECK_EQ((int)fs->frame_w_px, 20);

  float sx, sy, sw, sh;
  store.frame_src_rect(slot, 0, &sx, &sy, &sw, &sh);
  CHECK_EQ((int)sx, 0);
  store.frame_src_rect(slot, 1, &sx, &sy, &sw, &sh);
  CHECK_EQ((int)sx, 22);                          // 20 cell + 2 gutter
  store.frame_src_rect(slot, 3, &sx, &sy, &sw, &sh);
  CHECK_EQ((int)sx, 66);                          // 3 * (20 + 2)
}

TEST_CASE("filmstrip: out-of-range frame clamps to last")
{
  AssetStore<FakeLoader> store;
  uint32_t slot = make_bitmap(store, 64, 640);    // 10 frames
  REQUIRE(store.set_frame_layout(slot, 1, 10, 0));

  float sx, sy, sw, sh;
  store.frame_src_rect(slot, 999, &sx, &sy, &sw, &sh);
  CHECK_EQ((int)sy, 9 * 64);                       // pinned to frame 9
}

TEST_CASE("filmstrip: bad layouts are rejected, asset stays a plain bitmap")
{
  AssetStore<FakeLoader> store;
  uint32_t slot = make_bitmap(store, 64, 640);
  REQUIRE(slot != 0);

  CHECK_FALSE(store.set_frame_layout(slot, 0, 10, 0));   // cols < 1
  CHECK_FALSE(store.set_frame_layout(slot, 10, 0, 0));   // rows < 1
  // Gutters consume the whole dimension: 64px wide, 2 cols, 64px gutter.
  CHECK_FALSE(store.set_frame_layout(slot, 2, 1, 64));
  CHECK(store.frame_info(slot) == nullptr);              // never tagged
  CHECK_EQ((int)store.frame_count(slot), 0);

  // frame_src_rect on an untagged slot returns false, leaves out untouched.
  float sx = 7, sy = 7, sw = 7, sh = 7;
  CHECK_FALSE(store.frame_src_rect(slot, 0, &sx, &sy, &sw, &sh));
  CHECK_EQ((int)sx, 7);
}

TEST_CASE("filmstrip: re-tag overwrites a prior layout")
{
  AssetStore<FakeLoader> store;
  uint32_t slot = make_bitmap(store, 100, 100);
  REQUIRE(store.set_frame_layout(slot, 1, 10, 0));
  CHECK_EQ((int)store.frame_count(slot), 10);
  REQUIRE(store.set_frame_layout(slot, 2, 5, 0));
  CHECK_EQ((int)store.frame_count(slot), 10);
  const FilmstripInfo* fs = store.frame_info(slot);
  REQUIRE(fs != nullptr);
  CHECK_EQ((int)fs->cols, 2);
  CHECK_EQ((int)fs->frame_w_px, 50);
}

TEST_CASE("filmstrip: non-bitmap kinds rejected")
{
  AssetStore<FakeLoader> store;
  uint32_t comp = store.allocate_compound();
  REQUIRE(comp != 0);
  CHECK_FALSE(store.set_frame_layout(comp, 1, 4, 0));
  CHECK_EQ((int)store.frame_count(comp), 0);
  // Invalid slot.
  CHECK_FALSE(store.set_frame_layout(9999, 1, 4, 0));
}

TEST_CASE("filmstrip: frame-aware draw samples the cell sub-rect")
{
  reset_counters();
  neui_render_backend_t backend = make_backend();
  AssetStore<FakeLoader> store;
  uint32_t slot = make_bitmap(store, 64, 6400);
  REQUIRE(store.set_frame_layout(slot, 1, 100, 0));
  AssetEntry* e = store.get_slot(slot);
  REQUIRE(e != nullptr);

  // Draw frame 5 into a 32x32 dest at (10, 20).
  painter_draw_entry_frame_cached(&backend, k_ctx, e, /*frame=*/5,
                                  10.0f, 20.0f, 32.0f, 32.0f, 0xFFFFFFFFu);
  CHECK_EQ(g_create_calls, 1);                     // one upload of the whole strip
  CHECK_EQ(g_draw_calls, 1);
  CHECK_EQ((int)g_last_src[0], 0);                 // src x
  CHECK_EQ((int)g_last_src[1], 5 * 64);            // src y = frame 5
  CHECK_EQ((int)g_last_src[2], 64);                // src w
  CHECK_EQ((int)g_last_src[3], 64);                // src h
  CHECK_EQ((int)g_last_dst[0], 10);
  CHECK_EQ((int)g_last_dst[2], 32);

  // A second frame reuses the cached upload (no re-create).
  painter_draw_entry_frame_cached(&backend, k_ctx, e, 6,
                                  0.0f, 0.0f, 32.0f, 32.0f, 0xFFFFFFFFu);
  CHECK_EQ(g_create_calls, 1);                     // still one upload
  CHECK_EQ((int)g_last_src[1], 6 * 64);
}

TEST_CASE("filmstrip: allocate_filmstrip_from_file tags on success")
{
  SizedLoader::s_w = 64; SizedLoader::s_h = 640;     // 10 frames of 64x64
  neui_render_backend_t backend = make_backend();
  AssetStore<SizedLoader> store;

  uint32_t slot = store.allocate_filmstrip_from_file(
      "knob.png", 1.0f, /*frame_count=*/10, /*horizontal=*/false, &backend);
  REQUIRE(slot != 0);
  CHECK_EQ((int)store.frame_count(slot), 10);
  const FilmstripInfo* fs = store.frame_info(slot);
  REQUIRE(fs != nullptr);
  CHECK_EQ((int)fs->cols, 1);
  CHECK_EQ((int)fs->rows, 10);
  CHECK_EQ((int)fs->frame_h_px, 64);

  // Horizontal orientation tags rows 1.
  SizedLoader::s_w = 640; SizedLoader::s_h = 64;
  uint32_t hslot = store.allocate_filmstrip_from_file(
      "fader.png", 1.0f, 10, /*horizontal=*/true, &backend);
  REQUIRE(hslot != 0);
  const FilmstripInfo* hfs = store.frame_info(hslot);
  REQUIRE(hfs != nullptr);
  CHECK_EQ((int)hfs->cols, 10);
  CHECK_EQ((int)hfs->rows, 1);
}

TEST_CASE("filmstrip: allocate_filmstrip_from_file releases the slot on tag failure")
{
  SizedLoader::s_w = 64; SizedLoader::s_h = 640;
  neui_render_backend_t backend = make_backend();
  AssetStore<SizedLoader> store;

  // 700 frames can't fit a 640px-tall strip (cell height floors to 0), so the
  // tag fails after a successful load. The transiently-allocated slot must be
  // released, not leaked.
  uint32_t bad = store.allocate_filmstrip_from_file(
      "knob.png", 1.0f, /*frame_count=*/700, false, &backend);
  CHECK_EQ((int)bad, 0);

  // Proof of cleanup: the next allocation reuses the lowest slot (1). Had the
  // failed call leaked its slot, this bitmap would land on slot 2.
  std::vector<uint8_t> px(64u * 64u * 4u, 0);
  uint32_t first = store.allocate_bitmap(64, 64, px.data(), 1.0f);
  CHECK_EQ((int)first, 1);

  // frame_count == 0 / 1 are also rejected up front (no load attempted).
  CHECK_EQ((int)store.allocate_filmstrip_from_file("x.png", 1.0f, 0, false, &backend), 0);
}

TEST_CASE("filmstrip: frame-aware draw emits LOGICAL src coords at @2x scale")
{
  reset_counters();
  neui_render_backend_t backend = make_backend();
  AssetStore<FakeLoader> store;
  // @2x strip: 128x12800 physical px, scale 2.0 -> 64x6400 logical, 100 frames
  // of 128x128 physical (64x64 logical).
  std::vector<uint8_t> px(128u * 12800u * 4u, 0);
  uint32_t slot = store.allocate_bitmap(128, 12800, px.data(), 2.0f);
  REQUIRE(store.set_frame_layout(slot, 1, 100, 0));
  AssetEntry* e = store.get_slot(slot);
  REQUIRE(e != nullptr);

  // frame_src_rect (store query) stays PHYSICAL: frame 5 -> sy = 5*128.
  float psx, psy, psw, psh;
  REQUIRE(store.frame_src_rect(slot, 5, &psx, &psy, &psw, &psh));
  CHECK_EQ((int)psy, 5 * 128);
  CHECK_EQ((int)psh, 128);

  // The draw path divides by scale so the backend (which re-applies scale)
  // samples the right cell: logical sy = 5*128/2 = 320, sh = 64.
  painter_draw_entry_frame_cached(&backend, k_ctx, e, /*frame=*/5,
                                  0.0f, 0.0f, 64.0f, 64.0f, 0xFFFFFFFFu);
  CHECK_EQ(g_draw_calls, 1);
  CHECK_EQ((int)g_last_src[1], 320);   // logical, not physical 640
  CHECK_EQ((int)g_last_src[2], 64);    // logical cell width
  CHECK_EQ((int)g_last_src[3], 64);
}

// ---------------------------------------------------------------------------
// Recognition helpers (filmstrip_recognize.h)
// ---------------------------------------------------------------------------

TEST_CASE("filmstrip: parse_filename accepts marked trailing counts")
{
  uint32_t n = 0;
  CHECK(filmstrip_parse_filename("knob_100frames", n)); CHECK_EQ((int)n, 100);
  CHECK(filmstrip_parse_filename("KNOB_64FRAME",  n)); CHECK_EQ((int)n, 64);   // case-insensitive, singular
  CHECK(filmstrip_parse_filename("fader-128",     n)); CHECK_EQ((int)n, 128);
  CHECK(filmstrip_parse_filename("knob_100",      n)); CHECK_EQ((int)n, 100);
  CHECK(filmstrip_parse_filename("knob_f64",      n)); CHECK_EQ((int)n, 64);
  CHECK(filmstrip_parse_filename("knob_strip128", n)); CHECK_EQ((int)n, 128);
  CHECK(filmstrip_parse_filename("filmstrip256",  n)); CHECK_EQ((int)n, 256);
}

TEST_CASE("filmstrip: parse_filename rejects unmarked / ambiguous names")
{
  uint32_t n = 99;
  CHECK_FALSE(filmstrip_parse_filename("image2", n));   // bare trailing digit
  CHECK_FALSE(filmstrip_parse_filename("shelf3", n));   // 'f' not separator-marked
  CHECK_FALSE(filmstrip_parse_filename("panda",  n));   // no digits
  CHECK_FALSE(filmstrip_parse_filename("lemur",  n));
  CHECK_FALSE(filmstrip_parse_filename("knob_1", n));   // count < 2
  CHECK_EQ((int)n, 99);                                  // out untouched on reject
}

TEST_CASE("filmstrip: parse_sidecar reads frames/orientation and cols/rows/gutter")
{
  FilmstripLayout l;
  REQUIRE(filmstrip_parse_sidecar(R"({ "frames": 100 })", l));
  CHECK_EQ((int)l.cols, 1); CHECK_EQ((int)l.rows, 100);

  REQUIRE(filmstrip_parse_sidecar(R"({ "frames": 8, "orientation": "horizontal" })", l));
  CHECK_EQ((int)l.cols, 8); CHECK_EQ((int)l.rows, 1);

  REQUIRE(filmstrip_parse_sidecar(R"({ "cols": 4, "rows": 2, "gutter": 3 })", l));
  CHECK_EQ((int)l.cols, 4); CHECK_EQ((int)l.rows, 2); CHECK_EQ((int)l.gutter, 3);

  // Junk / missing keys -> not a layout.
  CHECK_FALSE(filmstrip_parse_sidecar(R"({ "hello": 1 })", l));
  CHECK_FALSE(filmstrip_parse_sidecar("not json at all", l));
}

TEST_CASE("filmstrip: create-with-discovery tags from a filename token")
{
  SizedLoader::s_w = 64; SizedLoader::s_h = 6400;        // 100 cells of 64x64
  neui_render_backend_t backend = make_backend();
  AssetStore<SizedLoader> store;

  // frame_count == 0 => discover. No sidecar file exists, so it falls to the
  // "_100frames" filename token.
  uint32_t slot = store.allocate_filmstrip_from_file(
      "knob_100frames.png", 1.0f, /*frame_count=*/0, /*horizontal=*/false, &backend);
  REQUIRE(slot != 0);
  CHECK_EQ((int)store.frame_count(slot), 100);

  // A plain name with no sidecar / token => discovery fails => 0.
  uint32_t none = store.allocate_filmstrip_from_file(
      "plain.png", 1.0f, 0, false, &backend);
  CHECK_EQ((int)none, 0);
}

TEST_CASE("filmstrip: frame draw on an untagged bitmap draws the whole image")
{
  reset_counters();
  neui_render_backend_t backend = make_backend();
  AssetStore<FakeLoader> store;
  uint32_t slot = make_bitmap(store, 48, 48);      // plain bitmap, no layout
  AssetEntry* e = store.get_slot(slot);
  REQUIRE(e != nullptr);

  painter_draw_entry_frame_cached(&backend, k_ctx, e, /*frame=*/3,
                                  0.0f, 0.0f, 48.0f, 48.0f, 0xFFFFFFFFu);
  CHECK_EQ(g_draw_calls, 1);
  // Untagged => 0,0,0,0 src rect (backend interprets as full bitmap).
  CHECK_EQ((int)g_last_src[0], 0);
  CHECK_EQ((int)g_last_src[1], 0);
  CHECK_EQ((int)g_last_src[2], 0);
  CHECK_EQ((int)g_last_src[3], 0);
}
