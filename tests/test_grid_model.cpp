#include "neui_test.h"

#include "grid_model.h"

using namespace neui_detail;

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

// Build a model with `nrows` rows and columns of the given widths.
static GridModel make_grid(int nrows, std::vector<int> col_widths)
{
  GridModel m;
  for (int w : col_widths) {
    GridColumn c;
    c.width = w;
    m.columns.push_back(c);
  }
  for (int r = 0; r < nrows; ++r) {
    GridRow row;
    row.cells.resize(col_widths.size());
    for (size_t c = 0; c < col_widths.size(); ++c)
      row.cells[c] = "r" + std::to_string(r) + "c" + std::to_string(c);
    m.rows.push_back(std::move(row));
  }
  return m;
}

// ---------------------------------------------------------------------------
// Layout queries
// ---------------------------------------------------------------------------

TEST_CASE("grid_total_content_width sums column widths")
{
  GridModel m = make_grid(0, { 50, 100, 30 });
  CHECK_EQ(grid_total_content_width(m), 180);
}

TEST_CASE("grid_column_left is the cumulative left edge")
{
  GridModel m = make_grid(0, { 50, 100, 30 });
  CHECK_EQ(grid_column_left(m, 0), 0);
  CHECK_EQ(grid_column_left(m, 1), 50);
  CHECK_EQ(grid_column_left(m, 2), 150);
  CHECK_EQ(grid_column_left(m, 3), 180);   // past-end clamps to total
  CHECK_EQ(grid_column_left(m, 99), 180);  // clamp guards overflow
}

TEST_CASE("grid_column_min_width: per-column override beats default")
{
  GridModel m = make_grid(0, { 50, 100 });
  m.columns[1].min_width = 40;
  CHECK_EQ(grid_column_min_width(m, 0, 24), 24);   // no override -> default
  CHECK_EQ(grid_column_min_width(m, 1, 24), 40);   // override wins
  CHECK_EQ(grid_column_min_width(m, 9, 24), 24);   // out of range -> default
}

TEST_CASE("grid_horizontal_step_px: average width, fallback when empty")
{
  GridModel empty = make_grid(0, {});
  CHECK_EQ(grid_horizontal_step_px(empty), GRID_LEFT_RIGHT_STEP_PX_FALLBACK);
  GridModel m = make_grid(0, { 40, 80, 120 });
  CHECK_EQ(grid_horizontal_step_px(m), 80);  // (40+80+120)/3
}

TEST_CASE("grid_parse_align maps strings to enum")
{
  CHECK(grid_parse_align("left")   == GridColAlign::Left);
  CHECK(grid_parse_align("center") == GridColAlign::Center);
  CHECK(grid_parse_align("right")  == GridColAlign::Right);
  CHECK(grid_parse_align(nullptr)  == GridColAlign::Left);
  CHECK(grid_parse_align("bogus")  == GridColAlign::Left);
}

TEST_CASE("grid_resize_rows_to_columns pads and truncates to N cells")
{
  GridModel m = make_grid(3, { 10, 10 });   // 2 cells per row
  grid_resize_rows_to_columns(m, 4);
  for (auto& r : m.rows) CHECK_EQ((int)r.cells.size(), 4);
  grid_resize_rows_to_columns(m, 1);
  for (auto& r : m.rows) CHECK_EQ((int)r.cells.size(), 1);
}

// ---------------------------------------------------------------------------
// Viewport / scrollbar visibility
// ---------------------------------------------------------------------------

TEST_CASE("grid_compute_viewport: content fits -> no scrollbars")
{
  GridModel m = make_grid(3, { 50, 50 });   // content 100w, 3*20=60h
  GridViewport vp = grid_compute_viewport(m, 200, 200, 20, 24);
  CHECK_FALSE(vp.vert_sb_shown);
  CHECK_FALSE(vp.horz_sb_shown);
  CHECK_EQ(vp.body_w, 200);
  CHECK_EQ(vp.body_h, 200 - 24);
}

TEST_CASE("grid_compute_viewport: tall content -> vertical bar steals width")
{
  GridModel m = make_grid(100, { 50, 50 });   // 100*20 = 2000h >> viewport
  GridViewport vp = grid_compute_viewport(m, 200, 224, 20, 24);
  CHECK(vp.vert_sb_shown);
  CHECK_FALSE(vp.horz_sb_shown);                // 100w content < 190 body
  CHECK_EQ(vp.body_w, 200 - SCROLLBAR_W);
  CHECK_EQ(vp.body_h, 224 - 24);
}

TEST_CASE("grid_compute_viewport: wide content -> horizontal bar steals height")
{
  GridModel m = make_grid(2, { 300, 300 });   // 600w content, short
  GridViewport vp = grid_compute_viewport(m, 200, 224, 20, 24);
  CHECK(vp.horz_sb_shown);
  CHECK_FALSE(vp.vert_sb_shown);
  CHECK_EQ(vp.body_h, (224 - 24) - SCROLLBAR_W);
}

TEST_CASE("grid_compute_viewport: both bars when content exceeds both axes")
{
  GridModel m = make_grid(100, { 300, 300 });
  GridViewport vp = grid_compute_viewport(m, 200, 224, 20, 24);
  CHECK(vp.vert_sb_shown);
  CHECK(vp.horz_sb_shown);
  CHECK_EQ(vp.body_w, 200 - SCROLLBAR_W);
  CHECK_EQ(vp.body_h, (224 - 24) - SCROLLBAR_W);
}

// ---------------------------------------------------------------------------
// Clamp / ensure-visible
// ---------------------------------------------------------------------------

TEST_CASE("grid_clamp_scroll keeps offsets within [0, max] and is idempotent")
{
  GridModel m = make_grid(100, { 300 });
  GridViewport vp = grid_compute_viewport(m, 200, 224, 20, 24);

  m.scroll_offset_y = 9999;
  m.scroll_offset_x = 9999;
  grid_clamp_scroll(m, vp, 20);
  int vis = grid_visible_rows(vp, 20);
  CHECK_EQ(m.scroll_offset_y, 100 - vis);
  CHECK_EQ(m.scroll_offset_x, grid_total_content_width(m) - vp.body_w);

  // Idempotent: clamping again changes nothing.
  int y = m.scroll_offset_y, x = m.scroll_offset_x;
  grid_clamp_scroll(m, vp, 20);
  CHECK_EQ(m.scroll_offset_y, y);
  CHECK_EQ(m.scroll_offset_x, x);

  m.scroll_offset_y = -50;
  m.scroll_offset_x = -50;
  grid_clamp_scroll(m, vp, 20);
  CHECK_EQ(m.scroll_offset_y, 0);
  CHECK_EQ(m.scroll_offset_x, 0);
}

TEST_CASE("grid_ensure_row_visible scrolls minimally and is idempotent")
{
  GridModel m = make_grid(100, { 50 });
  GridViewport vp = grid_compute_viewport(m, 200, 224, 20, 24);
  int vis = grid_visible_rows(vp, 20);

  // Row below the viewport -> becomes the last visible row.
  grid_ensure_row_visible(m, vp, 20, 50);
  CHECK(50 >= m.scroll_offset_y);
  CHECK(50 < m.scroll_offset_y + vis);
  CHECK_EQ(m.scroll_offset_y, 50 - vis + 1);

  // Idempotent for an already-visible row.
  int y = m.scroll_offset_y;
  grid_ensure_row_visible(m, vp, 20, 50);
  CHECK_EQ(m.scroll_offset_y, y);

  // Row above the viewport -> becomes the top row.
  grid_ensure_row_visible(m, vp, 20, 3);
  CHECK_EQ(m.scroll_offset_y, 3);
}

TEST_CASE("grid_ensure_cell_visible scrolls horizontally to reveal the column")
{
  GridModel m = make_grid(5, { 100, 100, 100, 100 });   // 400w content
  GridViewport vp = grid_compute_viewport(m, 200, 224, 20, 24);

  // Column 3 is off the right edge: left edge 300, width 100.
  grid_ensure_cell_visible(m, vp, 20, 0, 3);
  CHECK_EQ(m.scroll_offset_x, 300 + 100 - vp.body_w);  // right-aligns col 3

  // Column 0 is off the left edge now: scroll back to its left.
  grid_ensure_cell_visible(m, vp, 20, 0, 0);
  CHECK_EQ(m.scroll_offset_x, 0);
}

// ---------------------------------------------------------------------------
// Hit-test regions
// ---------------------------------------------------------------------------

TEST_CASE("grid_hit_test: out-of-bounds point -> None")
{
  GridModel m = make_grid(5, { 50 });
  GridViewport vp = grid_compute_viewport(m, 200, 200, 20, 24);
  CHECK(grid_hit_test(m, vp, 20, 200, 200, -1, 10).region == GridHitRegion::None);
  CHECK(grid_hit_test(m, vp, 20, 200, 200, 10, 999).region == GridHitRegion::None);
}

TEST_CASE("grid_hit_test: header band and divider")
{
  GridModel m = make_grid(5, { 50, 80 });
  GridViewport vp = grid_compute_viewport(m, 200, 200, 20, 24);

  GridHit h = grid_hit_test(m, vp, 20, 200, 200, 20, 5);   // y in header
  CHECK(h.region == GridHitRegion::Header);
  CHECK_EQ(h.col, 0);

  // Divider sits at the right edge of column 0 (x == 50), +-3px grab zone.
  GridHit d = grid_hit_test(m, vp, 20, 200, 200, 50, 5);
  CHECK(d.region == GridHitRegion::HeaderDivider);
  CHECK_EQ(d.col, 0);   // column to the LEFT of the divider
}

TEST_CASE("grid_hit_test: body cell, empty-below, and past-last-column")
{
  GridModel m = make_grid(3, { 50, 50 });   // 3 rows, body starts at y=24
  GridViewport vp = grid_compute_viewport(m, 200, 200, 20, 24);

  // Row 0, col 1: y = 24 + 0*20 + 10 = 34; x = 60 -> col 1.
  GridHit cell = grid_hit_test(m, vp, 20, 200, 200, 60, 34);
  CHECK(cell.region == GridHitRegion::Cell);
  CHECK_EQ(cell.row, 0);
  CHECK_EQ(cell.col, 1);

  // Below the last row (only 3 rows) -> BodyEmpty.
  GridHit below = grid_hit_test(m, vp, 20, 200, 200, 10, 24 + 3 * 20 + 5);
  CHECK(below.region == GridHitRegion::BodyEmpty);

  // Inside a valid row but past the last column (x=150 > 100 content).
  GridHit past = grid_hit_test(m, vp, 20, 200, 200, 150, 34);
  CHECK(past.region == GridHitRegion::BodyEmpty);
  CHECK_EQ(past.row, 0);
}

TEST_CASE("grid_hit_test: scrollbar gutters and corner")
{
  GridModel m = make_grid(100, { 300, 300 });   // forces both bars
  GridViewport vp = grid_compute_viewport(m, 200, 224, 20, 24);
  REQUIRE(vp.vert_sb_shown);
  REQUIRE(vp.horz_sb_shown);

  CHECK(grid_hit_test(m, vp, 20, 200, 224, 195, 60).region == GridHitRegion::VertScrollTrack);
  CHECK(grid_hit_test(m, vp, 20, 200, 224, 60, 219).region == GridHitRegion::HorzScrollTrack);
  CHECK(grid_hit_test(m, vp, 20, 200, 224, 195, 219).region == GridHitRegion::Corner);
}

// ---------------------------------------------------------------------------
// THE key invariant: paint y-position <-> hit-test row are exact inverses.
//
// The paint code (widget_paint_grid.h) draws row R at widget-local y:
//     ry = body_y - scroll_px_offset + (R - scroll_offset_y) * row_h
// Feeding the vertical centre of that band back into grid_hit_test must
// return row R. This crosses the two independent implementations of the
// offset arithmetic, so a sign error in either side is caught.
// ---------------------------------------------------------------------------

static int paint_row_y(const GridViewport& vp, const GridModel& m, int row_h, int row)
{
  return vp.body_y - m.scroll_px_offset + (row - m.scroll_offset_y) * row_h;
}

TEST_CASE("paint/hit-test inverse: aligned scroll (px_offset == 0)")
{
  const int row_h = 20;
  GridModel m = make_grid(100, { 50, 50, 50 });   // body_w 190 (vbar) fits 150
  GridViewport vp = grid_compute_viewport(m, 200, 224, row_h, 24);
  m.scroll_offset_y = 12;
  m.scroll_px_offset = 0;

  int vis = grid_visible_rows(vp, row_h);
  for (int row = m.scroll_offset_y; row < m.scroll_offset_y + vis; ++row) {
    int ry = paint_row_y(vp, m, row_h, row);
    GridHit h = grid_hit_test(m, vp, row_h, 200, 224, 60 /*col 1*/, ry + row_h / 2);
    CHECK(h.region == GridHitRegion::Cell);
    CHECK_EQ(h.row, row);
    CHECK_EQ(h.col, 1);
  }
}

TEST_CASE("paint/hit-test inverse: sub-row scroll (positive px_offset)")
{
  const int row_h = 20;
  GridModel m = make_grid(100, { 50, 50, 50 });
  GridViewport vp = grid_compute_viewport(m, 200, 224, row_h, 24);
  m.scroll_offset_y = 5;
  m.scroll_px_offset = 7;   // content shifted up 7px -> first row partly clipped

  // First fully-addressable row centre maps back to itself.
  for (int row = 5; row < 12; ++row) {
    int ry = paint_row_y(vp, m, row_h, row);
    int probe_y = ry + row_h / 2;
    if (probe_y < vp.body_y) continue;           // clipped above body top
    if (probe_y >= vp.body_y + vp.body_h) break;  // below body
    GridHit h = grid_hit_test(m, vp, row_h, 200, 224, 10, probe_y);
    CHECK(h.region == GridHitRegion::Cell);
    CHECK_EQ(h.row, row);
  }
}

TEST_CASE("paint/hit-test inverse: top rubber-band (negative px_offset)")
{
  const int row_h = 20;
  GridModel m = make_grid(100, { 50, 50, 50 });
  GridViewport vp = grid_compute_viewport(m, 200, 224, row_h, 24);
  m.scroll_offset_y = 0;
  m.scroll_px_offset = -8;   // 8px blank band above row 0

  // A point inside the blank band (above row 0's painted top) -> BodyEmpty.
  GridHit band = grid_hit_test(m, vp, row_h, 200, 224, 10, vp.body_y + 2);
  CHECK(band.region == GridHitRegion::BodyEmpty);

  // Row 0's painted centre still maps to row 0.
  int ry0 = paint_row_y(vp, m, row_h, 0);
  GridHit h = grid_hit_test(m, vp, row_h, 200, 224, 10, ry0 + row_h / 2);
  CHECK(h.region == GridHitRegion::Cell);
  CHECK_EQ(h.row, 0);
}

// ---------------------------------------------------------------------------
// Smooth-scroll math
// ---------------------------------------------------------------------------

TEST_CASE("grid_scroll_overshoot: zero at origin, monotonic, bounded by limit")
{
  const double dim = 400.0;   // limit = min(200, 60) = 60
  CHECK_APPROX(grid_scroll_overshoot(0.0, dim), 0.0);

  double prev = -1.0;
  for (double x = 0.0; x <= 2000.0; x += 25.0) {
    double o = grid_scroll_overshoot(x, dim);
    CHECK(o >= prev);                          // monotonic non-decreasing
    CHECK(o < GRID_SCROLL_OVERSCROLL_MAX);     // asymptote never reached
    prev = o;
  }
}

TEST_CASE("grid_scroll_overshoot: limit caps at OVERSCROLL_MAX for big viewports")
{
  // Pull very hard so the term approaches the limit.
  double big = grid_scroll_overshoot(1e6, 4000.0);   // dim/2 would be 2000
  CHECK(big < GRID_SCROLL_OVERSCROLL_MAX);
  CHECK(big > GRID_SCROLL_OVERSCROLL_MAX - 1.0);      // but close to 60
}

TEST_CASE("grid_scroll_rubber: in-range pass-through, damped + continuous at edges")
{
  const double max_px = 500.0, dim = 400.0;
  CHECK_APPROX(grid_scroll_rubber(0.0, max_px, dim), 0.0);
  CHECK_APPROX(grid_scroll_rubber(250.0, max_px, dim), 250.0);
  CHECK_APPROX(grid_scroll_rubber(max_px, max_px, dim), max_px);

  // Just past the bottom edge: slightly beyond max, but damped (< raw).
  double over = grid_scroll_rubber(max_px + 100.0, max_px, dim);
  CHECK(over > max_px);
  CHECK(over < max_px + 100.0);

  // Just past the top edge: negative, damped.
  double under = grid_scroll_rubber(-100.0, max_px, dim);
  CHECK(under < 0.0);
  CHECK(under > -100.0);
}

TEST_CASE("grid_scroll_max_px: 0 when content fits, else content - body_h")
{
  GridModel fits = make_grid(3, { 50 });
  GridViewport vpf = grid_compute_viewport(fits, 200, 200, 20, 24);
  CHECK_APPROX(grid_scroll_max_px(fits, vpf, 20), 0.0);

  GridModel tall = make_grid(100, { 50 });
  GridViewport vpt = grid_compute_viewport(tall, 200, 224, 20, 24);
  CHECK_APPROX(grid_scroll_max_px(tall, vpt, 20), (double)(100 * 20 - vpt.body_h));
}

TEST_CASE("grid_scroll_commit: round-trips raw into offset_y + px_offset")
{
  const int row_h = 20;
  GridModel m = make_grid(100, { 50 });
  GridViewport vp = grid_compute_viewport(m, 200, 224, row_h, 24);
  double max_px = grid_scroll_max_px(m, vp, row_h);

  // Mid-range raw -> exact decomposition.
  m.scroll_kin.raw_px = 137.0;
  grid_scroll_commit(m, vp, row_h);
  CHECK_EQ(m.scroll_offset_y, 137 / row_h);          // 6
  CHECK_EQ(m.scroll_px_offset, 137 % row_h);         // 17
  CHECK_EQ(m.scroll_offset_y * row_h + m.scroll_px_offset, 137);
  CHECK_EQ(m.scroll_kin.last_commit_px, 137);

  // Top overscroll -> offset_y pinned at 0, px_offset <= 0.
  m.scroll_kin.raw_px = -30.0;
  grid_scroll_commit(m, vp, row_h);
  CHECK_EQ(m.scroll_offset_y, 0);
  CHECK(m.scroll_px_offset <= 0);

  (void)max_px;
}

// ---- grid_scroll_wheel ----------------------------------------------------

static GridWheelInput precise_delta(double dpx)
{
  GridWheelInput in;
  in.precise = true;
  in.phase_changed = true;   // finger-down move
  in.delta_px = dpx;
  return in;
}

TEST_CASE("grid_scroll_wheel: legacy wheel hard-clamps, never springs back")
{
  const int row_h = 20;
  GridModel m = make_grid(100, { 50 });
  GridViewport vp = grid_compute_viewport(m, 200, 224, row_h, 24);

  // Negative delta scrolls down (raw increases); positive scrolls up.
  GridWheelInput down;   // classic wheel: no precise, no phase, no momentum
  down.delta_px = -60.0;
  GridWheelAction a = grid_scroll_wheel(m, vp, row_h, down);
  CHECK(a.changed);
  CHECK_FALSE(a.start_bounce);
  CHECK_APPROX(m.scroll_kin.raw_px, 60.0);

  // Over-scroll attempt past the top is hard-clamped to 0, no bounce.
  GridWheelInput up;
  up.delta_px = 10000.0;
  GridWheelAction b = grid_scroll_wheel(m, vp, row_h, up);
  CHECK_APPROX(m.scroll_kin.raw_px, 0.0);
  CHECK_FALSE(b.start_bounce);

  // Over-scroll attempt past the bottom is hard-clamped to max, no bounce.
  double max_px = grid_scroll_max_px(m, vp, row_h);
  GridWheelInput down2;
  down2.delta_px = -1e6;
  GridWheelAction c = grid_scroll_wheel(m, vp, row_h, down2);
  CHECK_APPROX(m.scroll_kin.raw_px, max_px);
  CHECK_FALSE(c.start_bounce);
}

TEST_CASE("grid_scroll_wheel: precise in-range scroll moves position")
{
  const int row_h = 20;
  GridModel m = make_grid(100, { 50 });
  GridViewport vp = grid_compute_viewport(m, 200, 224, row_h, 24);

  GridWheelAction a = grid_scroll_wheel(m, vp, row_h, precise_delta(-50.0)); // down
  CHECK(a.changed);
  CHECK_APPROX(m.scroll_kin.raw_px, 50.0);
  CHECK_FALSE(a.start_bounce);   // still in range
}

TEST_CASE("grid_scroll_wheel: phase_began cancels bounce + momentum suppression")
{
  const int row_h = 20;
  GridModel m = make_grid(100, { 50 });
  GridViewport vp = grid_compute_viewport(m, 200, 224, row_h, 24);
  m.scroll_kin.suppress_momentum = true;

  GridWheelInput in;
  in.precise = true;
  in.phase_began = true;
  in.delta_px = -10.0;
  GridWheelAction a = grid_scroll_wheel(m, vp, row_h, in);
  CHECK(a.stop_bounce);
  CHECK_FALSE(m.scroll_kin.suppress_momentum);
}

TEST_CASE("grid_scroll_wheel: inertial overscroll triggers spring-back + suppression")
{
  const int row_h = 20;
  GridModel m = make_grid(100, { 50 });
  GridViewport vp = grid_compute_viewport(m, 200, 224, row_h, 24);

  // A momentum (post-lift) event that drives past the top edge.
  GridWheelInput mom;
  mom.precise = true;
  mom.momentum = true;
  mom.delta_px = 10000.0;   // natural-scroll: positive delta subtracts -> raw < 0
  GridWheelAction a = grid_scroll_wheel(m, vp, row_h, mom);
  CHECK(a.start_bounce);
  CHECK(m.scroll_kin.suppress_momentum);
  CHECK(m.scroll_kin.raw_px < 0.0);

  // While suppressed, further momentum events are swallowed (no change).
  GridWheelInput mom2 = mom;
  GridWheelAction b = grid_scroll_wheel(m, vp, row_h, mom2);
  CHECK_FALSE(b.changed);

  // momentum_ended clears suppression.
  GridWheelInput end;
  end.precise = true;
  end.momentum = true;
  end.momentum_ended = true;
  grid_scroll_wheel(m, vp, row_h, end);
  CHECK_FALSE(m.scroll_kin.suppress_momentum);
}

TEST_CASE("grid_scroll_wheel: resyncs raw when the model moved externally")
{
  const int row_h = 20;
  GridModel m = make_grid(100, { 50 });
  GridViewport vp = grid_compute_viewport(m, 200, 224, row_h, 24);

  // Simulate a keyboard nav / API move that bypassed the kinetics.
  m.scroll_offset_y = 10;
  m.scroll_px_offset = 0;          // committed == 200, last_commit_px still 0
  GridWheelAction a = grid_scroll_wheel(m, vp, row_h, precise_delta(-20.0));
  CHECK(a.changed);
  // raw should have resynced to 200 then applied -(-20) = +20 -> 220.
  CHECK_APPROX(m.scroll_kin.raw_px, 220.0);
}

// ---- grid_scroll_bounce_step ----------------------------------------------

TEST_CASE("grid_scroll_bounce_step: converges to the clamped target and stops")
{
  const int row_h = 20;
  GridModel m = make_grid(100, { 50 });
  GridViewport vp = grid_compute_viewport(m, 200, 224, row_h, 24);

  // Start overscrolled past the top.
  m.scroll_kin.raw_px = -55.0;
  grid_scroll_commit(m, vp, row_h);   // sets last_commit_px to match

  double prev_dist = 1e9;
  int steps = 0;
  bool animating = true;
  while (animating) {
    double dist_before = std::fabs(m.scroll_kin.raw_px - 0.0);
    animating = grid_scroll_bounce_step(m, vp, row_h);
    double dist_after = std::fabs(m.scroll_kin.raw_px - 0.0);
    CHECK(dist_after <= dist_before + 1e-9);   // never moves away from target
    CHECK(m.scroll_kin.raw_px <= 1e-9);        // never overshoots past 0
    prev_dist = dist_after;
    if (++steps > 1000) { CHECK(false); break; }   // must terminate
  }
  CHECK_APPROX(m.scroll_kin.raw_px, 0.0);   // settled exactly at the edge
  (void)prev_dist;
}

TEST_CASE("grid_scroll_bounce_step: aborts when the model changed externally")
{
  const int row_h = 20;
  GridModel m = make_grid(100, { 50 });
  GridViewport vp = grid_compute_viewport(m, 200, 224, row_h, 24);

  m.scroll_kin.raw_px = -40.0;
  grid_scroll_commit(m, vp, row_h);

  // Something else moves the committed position out from under the bounce.
  m.scroll_offset_y = 7;
  double raw_before = m.scroll_kin.raw_px;
  bool animating = grid_scroll_bounce_step(m, vp, row_h);
  CHECK_FALSE(animating);                       // yields immediately
  CHECK_APPROX(m.scroll_kin.raw_px, raw_before);// did not clobber position
}

// ---------------------------------------------------------------------------
// Config read
// ---------------------------------------------------------------------------

TEST_CASE("grid_read_config: defaults when bag is null")
{
  GridPaintConfig c = grid_read_config(nullptr);
  CHECK_EQ(c.row_h, GRID_DEFAULT_ROW_H);
  CHECK_EQ(c.header_h, GRID_DEFAULT_HEADER_H);
  CHECK_EQ(c.col_min_w_def, GRID_DEFAULT_COLUMN_MIN_W);
  CHECK(c.show_focus_row);
  CHECK_FALSE(c.cell_focus);
  CHECK_FALSE(c.bg_explicit);
}

TEST_CASE("grid_read_config: attrs override defaults")
{
  AttrBag bag;
  bag.set_int(NEUI_ATTR_GRID_ROW_HEIGHT, 30);
  bag.set_int(NEUI_ATTR_GRID_CELL_FOCUS, 1);
  bag.set_int(NEUI_ATTR_BACKGROUND, (int)0xFF202020);
  GridPaintConfig c = grid_read_config(&bag);
  CHECK_EQ(c.row_h, 30);
  CHECK(c.cell_focus);
  CHECK(c.bg_explicit);
  CHECK_EQ((unsigned)c.bg_argb, 0xFF202020u);
}

// ---------------------------------------------------------------------------
// Cell editor lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("grid_cell_edit_allowed: requires editable column, cell-focus, enabled cell")
{
  GridModel m = make_grid(2, { 50, 50 });
  // Defaults: editable=false.
  CHECK_FALSE(grid_cell_edit_allowed(m, 0, 0, /*cell_focus=*/true));
  m.columns[1].editable = true;
  CHECK(grid_cell_edit_allowed(m, 0, 1, true));
  // Out-of-range.
  CHECK_FALSE(grid_cell_edit_allowed(m, -1, 1, true));
  CHECK_FALSE(grid_cell_edit_allowed(m, 0,  2, true));
  // Cell-focus off.
  CHECK_FALSE(grid_cell_edit_allowed(m, 0, 1, false));
  // Disabled cell override blocks edit.
  auto& ov = grid_ensure_override(m, 0, 1);
  ov.enabled = false; ov.has_enabled = true;
  CHECK_FALSE(grid_cell_edit_allowed(m, 0, 1, true));
}

TEST_CASE("grid_begin_edit seeds buffer from the cell and selects all")
{
  GridModel m = make_grid(2, { 50, 50 });
  m.rows[0].cells[1] = "abc";
  m.columns[1].editable = true;
  grid_begin_edit(m, 0, 1);
  CHECK(m.edit.active);
  CHECK_EQ(m.edit.row, 0);
  CHECK_EQ(m.edit.col, 1);
  CHECK_EQ(m.edit.te.text, std::string("abc"));
  // Whole content selected: anchor=0, cursor=end.
  CHECK_EQ(m.edit.te.sel_anchor, 0);
  CHECK_EQ(m.edit.te.cursor, 3);
  CHECK_EQ(m.edit.orig_text, std::string("abc"));
}

TEST_CASE("grid_end_edit clears the state and returns the working text")
{
  GridModel m = make_grid(1, { 50 });
  m.columns[0].editable = true;
  grid_begin_edit(m, 0, 0);
  m.edit.te.text   = "hello";
  m.edit.te.cursor = 5;
  m.edit.te.sel_anchor = 5;
  std::string out = grid_end_edit(m);
  CHECK_EQ(out, std::string("hello"));
  CHECK_FALSE(m.edit.active);
  CHECK_EQ(m.edit.row, -1);
  CHECK_EQ(m.edit.col, -1);
}
