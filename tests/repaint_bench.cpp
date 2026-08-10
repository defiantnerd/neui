// Repaint throughput benchmark for the crossplatform host.
//
// This is the Phase 0a measurement from plans/sst-neuigui-gap-response.md,
// run on the host a plugin UI should actually use. The question it answers is
// the one that motivates wave 5 (partial repaint): with N CUSTOMDRAW widgets
// in a frame, how expensive is ONE full-frame repaint, and does a whole-frame
// invalidate at 60 Hz leave headroom or not?
//
// Why it matters before writing any dirty-rect code: the xpl host paints a
// frame in a single pass, so today `widgets->invalidate(any_widget)` repaints
// every widget in that frame. If a 100-widget grid already repaints in a small
// fraction of a 16.6 ms budget, narrowing the repaint is an optimisation with
// no user-visible payoff - and the honest deliverable is the number, not the
// code. If it does not, the number says exactly how much has to come off.
//
// Method
// ------
// A frame holds a GRID_N x GRID_N mesh of CUSTOMDRAW widgets. Each one paints
// a deliberately representative load rather than a trivial one: a background
// rect, a border, two filled bars and a text label - roughly what a knob, a
// meter, or a labelled value readout costs. The client counts WIDGET_PAINT
// events, so a "full frame" is exactly GRID_N*GRID_N paints.
//
// The loop is invalidate -> pump_once, timed over a fixed wall-clock window.
// pump_once is used rather than run() so the benchmark owns the loop and can
// time it; that is the same pumping path an embedded (DAW) frame uses.
//
// Two cases are measured, and the RATIO between them is the actual finding:
//   1. FULL       - invalidate the frame: every widget repaints (today's
//                   behaviour for any invalidate).
//   2. ONE WIDGET - invalidate a single widget. Identical cost today, since
//                   the host widens it to the frame. After wave 5 this should
//                   drop toward 1/N. Measuring it NOW establishes the
//                   baseline, so the wave-5 claim is a comparison rather than
//                   an assertion.
//
// Reported: mean ms per full-frame repaint, the implied ceiling in frames per
// second, and what fraction of a 60 Hz budget (16.67 ms) one repaint eats.
//
// Needs a GUI session (it realizes a real window), so it is built but NOT
// ctest-registered; run ./tests/<config>/neui_repaint_bench manually.
// Set NEUI_BENCH_GRID to override the mesh size (default 10 -> 100 widgets).

#include <neui/neui.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

int  g_grid_n     = 10;          // GRID_N x GRID_N CUSTOMDRAW widgets
long g_paint_count = 0;          // WIDGET_PAINT events seen
bool g_have_painter = false;     // did we actually get a painter to draw with?
// Text is measured separately because it is the one primitive whose cost is
// dominated by the platform's text engine rather than by neui. Which of the
// two dominates decides whether partial repaint or text caching is the better
// investment - so the benchmark answers that instead of guessing.
bool g_draw_text    = true;

neui_widget_api_t* g_widgets = nullptr;

// One widget's paint load. Deliberately not trivial - a single fill_rect would
// measure the dispatch overhead and nothing else, which would flatter the
// framework and mislead the reader.
void paint_one(const neui_event_paint_t& p)
{
  auto* pa = p.painter_api;
  if (!pa || !p.p) return;
  g_have_painter = true;

  const float w = (float)p.width, h = (float)p.height;
  if (pa->fill_rect) {
    pa->fill_rect(p.p, 0.0f, 0.0f, w, h, 0xFF2B2B2B);
    // Two "value bars" - the sort of thing a meter or a knob fill draws.
    pa->fill_rect(p.p, 2.0f, h - 8.0f, w * 0.6f, 5.0f, 0xFF4FA3FF);
    pa->fill_rect(p.p, 2.0f, h - 15.0f, w * 0.35f, 5.0f, 0xFF7FD07F);
  }
  if (pa->draw_rect)
    pa->draw_rect(p.p, 0.5f, 0.5f, w - 1.0f, h - 1.0f, 1.0f, 0xFF555555);
  if (g_draw_text && pa->draw_text)
    pa->draw_text(p.p, 3.0f, 2.0f, w - 6.0f, 14.0f, "0.75", 11.0f, 0xFFE0E0E0);
}

bool NEUI_ABI onevent(void*, neui_event_t* ev)
{
  if (!ev) return false;
  if (ev->type == NEUI_EVENT_WIDGET_PAINT) {
    ++g_paint_count;
    paint_one(ev->data.paint);
    return true;
  }
  return false;
}

neui_widget_client_t g_widget_client = { NEUI_VERSION, nullptr, onevent };

void* NEUI_ABI get_interface(void*, const char* iface)
{
  if (iface && std::strcmp(iface, NEUI_API_WIDGETS) == 0) return &g_widget_client;
  return nullptr;
}

using clock_t_ = std::chrono::steady_clock;

struct Result
{
  double frames        = 0;   // full-frame repaints completed
  double seconds       = 0;
  double ms_per_frame  = 0;
  double fps_ceiling   = 0;
  double pct_of_60hz   = 0;
  long   paints        = 0;
};

// Drive invalidate -> pump_once for `budget_s` seconds and report throughput.
// `target` is the widget to invalidate: the frame (whole-frame repaint) or one
// leaf (which today widens to the same thing).
Result measure(neui_api_t* neui, neui_session_t sess, neui_widget_t target,
               double budget_s, int widgets_per_frame)
{
  const long start_paints = g_paint_count;
  auto t0 = clock_t_::now();
  long iterations = 0;
  for (;;) {
    g_widgets->invalidate(sess, target);
    if (!neui->pump_once(sess)) break;      // window closed / quit
    ++iterations;
    double el = std::chrono::duration<double>(clock_t_::now() - t0).count();
    if (el >= budget_s) break;
  }
  auto t1 = clock_t_::now();

  Result r;
  r.paints  = g_paint_count - start_paints;
  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  // A "frame" is one full pass over the mesh. Derived from the paint count
  // rather than from the iteration count on purpose: a pump_once that
  // coalesced two invalidates into one repaint must not be counted twice, and
  // this is also what will show wave 5 painting FEWER widgets per invalidate.
  r.frames = (widgets_per_frame > 0)
             ? (double)r.paints / (double)widgets_per_frame : 0.0;
  if (r.frames > 0.0 && r.seconds > 0.0) {
    r.ms_per_frame = (r.seconds * 1000.0) / r.frames;
    r.fps_ceiling  = r.frames / r.seconds;
    r.pct_of_60hz  = (r.ms_per_frame / (1000.0 / 60.0)) * 100.0;
  }
  (void)iterations;
  return r;
}

void report(const char* label, const Result& r, int n_widgets)
{
  std::printf("  %-12s  %7.3f ms/frame   %8.0f fps ceiling   %6.1f%% of 60Hz budget"
              "   (%ld paints / %.2fs)\n",
              label, r.ms_per_frame, r.fps_ceiling, r.pct_of_60hz,
              r.paints, r.seconds);
  if (r.frames > 0.0)
    std::printf("  %-12s  %7.4f ms per widget paint\n", "",
                r.ms_per_frame / (double)n_widgets);
}

} // namespace

int main()
{
  if (const char* g = std::getenv("NEUI_BENCH_GRID")) {
    int v = std::atoi(g);
    if (v >= 1 && v <= 60) g_grid_n = v;
  }
  const int n_widgets = g_grid_n * g_grid_n;

  neui_init();
  // The xpl host explicitly: this benchmark is about ITS single-pass frame
  // paint. On win32/macOS neui_get_api(NULL) hands back the native host first,
  // which paints per-widget native views and would measure something else.
  neui_api_t* neui = neui_get_api("neui.host.crossplatform");
  if (!neui) { std::printf("[FAIL] xpl host not registered\n"); return 1; }

  neui_client_t client = { NEUI_VERSION, get_interface };
  neui_session_t sess = neui->create_session(&client, nullptr);

  g_widgets = (neui_widget_api_t*)neui->get_interface(sess, NEUI_API_WIDGETS);
  if (!g_widgets) { std::printf("[FAIL] no widget api\n"); return 1; }

  // Lay the mesh out with a small margin, then size the frame from the
  // content (the recurring client-code rule - never undersize a frame).
  const int cell = 44, gap = 4, margin = 10;
  const int span = g_grid_n * cell + (g_grid_n - 1) * gap;
  const int win_w = span + margin * 2;
  const int win_h = span + margin * 2;

  neui_widget_t win = g_widgets->create(sess, widget_none, NEUI_W_APPWINDOW,
                                        60, 60, win_w, win_h, nullptr);
  std::vector<neui_widget_t> cells;
  cells.reserve((size_t)n_widgets);
  for (int row = 0; row < g_grid_n; ++row) {
    for (int col = 0; col < g_grid_n; ++col) {
      int x = margin + col * (cell + gap);
      int y = margin + row * (cell + gap);
      cells.push_back(g_widgets->create(sess, win, NEUI_W_CUSTOMDRAW,
                                        x, y, cell, cell, nullptr));
    }
  }
  g_widgets->show(sess, win);

  std::printf("\nneui repaint benchmark - host neui.host.crossplatform\n");
  std::printf("  %d CUSTOMDRAW widgets (%dx%d mesh), frame %dx%d logical px\n\n",
              n_widgets, g_grid_n, g_grid_n, win_w, win_h);

  // Warm up: first paints build font caches and realize the native surface,
  // which would otherwise land in the first measurement.
  measure(neui, sess, win, 0.4, n_widgets);
  if (!g_have_painter) {
    std::printf("[FAIL] no WIDGET_PAINT reached the client - nothing measured\n");
    neui->destroy(sess);
    return 1;
  }
  g_paint_count = 0;

  Result full = measure(neui, sess, win, 2.0, n_widgets);
  std::printf("FULL frame invalidate (widgets->invalidate on the frame):\n");
  report("full", full, n_widgets);

  // One leaf widget. TODAY this is identical to the full case, because the xpl
  // host widens any invalidate to its frame - that equality IS the baseline
  // wave 5 has to break.
  Result one = measure(neui, sess, cells.back(), 2.0, n_widgets);
  std::printf("\nSINGLE widget invalidate (widgets->invalidate on one cell):\n");
  report("one-widget", one, n_widgets);

  double widgets_painted_per_invalidate =
    (one.paints > 0 && one.seconds > 0.0)
      ? (double)one.paints / (one.fps_ceiling * one.seconds) : 0.0;
  std::printf("\n  widgets repainted per single-widget invalidate: %.1f of %d\n",
              widgets_painted_per_invalidate, n_widgets);
  std::printf("  ratio one-widget / full cost: %.2fx  "
              "(1.00x = no narrowing, %.2fx = perfect)\n",
              (full.ms_per_frame > 0.0) ? one.ms_per_frame / full.ms_per_frame : 0.0,
              1.0 / (double)n_widgets);

  // Same load minus the text draw. The delta is the platform text engine's
  // share of a repaint.
  g_draw_text = false;
  measure(neui, sess, win, 0.3, n_widgets);       // re-warm without text
  Result notext = measure(neui, sess, win, 2.0, n_widgets);
  g_draw_text = true;
  std::printf("\nFULL frame invalidate, NO text (shapes only):\n");
  report("no-text", notext, n_widgets);
  if (full.ms_per_frame > 0.0 && notext.ms_per_frame > 0.0) {
    double text_share = 1.0 - (notext.ms_per_frame / full.ms_per_frame);
    std::printf("\n  text is %.0f%% of the repaint cost "
                "(%.3f ms of %.3f ms); shapes are the other %.0f%%\n",
                text_share * 100.0,
                full.ms_per_frame - notext.ms_per_frame, full.ms_per_frame,
                (1.0 - text_share) * 100.0);
  }

  std::printf("\n  VERDICT: one full repaint of %d CUSTOMDRAW widgets costs "
              "%.2f ms (%.1f%% of a 60Hz frame).\n",
              n_widgets, full.ms_per_frame, full.pct_of_60hz);
  if (full.pct_of_60hz < 25.0)
    std::printf("           Comfortable headroom at 60Hz with NO partial repaint.\n");
  else if (full.pct_of_60hz < 60.0)
    std::printf("           Workable at 60Hz but partial repaint would buy real margin.\n");
  else
    std::printf("           Partial repaint is REQUIRED for 60Hz at this widget count.\n");
  std::printf("\n");

  neui->destroy(sess);
  return 0;
}
