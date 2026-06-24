// filter_knob_example - SVG-style filter operations (feGaussianBlur +
// feDropShadow) on a baked SURFACE, the "bake once, blit many" architecture
// from plans/svg-adaption.md.
//
// A designer's SVG knob defines its drop shadow as a filter graph that
// resolves to feDropShadow, plus a vertical linear-gradient face:
//
//   feOffset dy=9 + feGaussianBlur stdDeviation=9, black drop shadow
//   linearGradient down the face, #4E4E4E -> #363636
//
// We reproduce that shadow primitive-for-primitive: the static art (bezel +
// gradient face) is painted once into an off-screen SURFACE via paint_surface,
// then build_drop_shadow_filter() hand-wires the designer's fe* chain into a
// NEUI_API_FILTER graph and assets->apply_filter bakes it into the surface
// pixels (sigma = the SVG stdDeviation, colour = black at 0.35 alpha). The
// canvas carries transparent margin so the offset + blurred halo fits (see the
// geometry constants below). A second small SURFACE holds the rotating
// indicator. A two-layer COMPOUND draws the baked face (below) and the
// indicator (above, rotation bound to neui.param.value); a DRAG_ROTATIONAL
// BEHAVIOR turns the knob. The compound + behavior + both surfaces are shared
// across all three knob instances (shape baked once, AttrBag per widget) - the
// whole point of the bake-once approach.

#include "neui/neui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#ifdef WIN32
#define ACTIVE_HOST "neui.host.win32"
#elif defined(__APPLE__)
#define ACTIVE_HOST "neui.host.macos"
#else
#define ACTIVE_HOST "neui.host.crossplatform"
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static const float kTwoPi = 6.28318530718f;

// Knob geometry, in the surface's logical pixels. The canvas carries generous
// transparent margin around the bezel so the drop shadow (offset dy + ~3*sigma
// of blur halo) stays inside the surface instead of clipping at the edge: the
// bezel bottom (kCy+kRBezel=113) plus dy 9 + 3*sigma 27 = 149 < kCanvas 160.
static const float kCanvas  = 160.0f;
static const float kCx      = 80.0f;
static const float kCy      = 80.0f;
static const float kRBezel  = 33.0f;
static const float kRFace   = 29.0f;

struct AppState
{
  neui_api_t*          neui     = nullptr;
  neui_widget_api_t*   widgets  = nullptr;
  neui_asset_api_t*    assets   = nullptr;
  neui_attr_api_t*     attrs    = nullptr;
  neui_compound_api_t* compound = nullptr;
  neui_behavior_api_t* behavior = nullptr;
  neui_filter_api_t*   filters  = nullptr;
  neui_session_t       session  = { 0 };

  neui_asset_t face   = asset_none;   // baked static art (bezel + gradient + shadows)
  neui_asset_t needle = asset_none;   // rotating indicator
  neui_asset_t comp   = asset_none;   // 2-layer compound (shared)
  neui_asset_t behav  = asset_none;   // drag behavior (shared)
};

static void dbglog(const char* fmt, ...)
{
  char buf[1024];
  va_list args; va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
#ifdef _WIN32
  OutputDebugStringA(buf);
#endif
  fputs(buf, stderr);
}

// Paint the static knob art into the face surface: a dark bezel disc, a
// vertical-linear-gradient face on top, and a faint top highlight. Opaque
// where the knob is, transparent elsewhere so the baked shadow + the widget
// background show through.
static void NEUI_ABI paint_face(neui_painter* p, neui_painter_api* px,
                                float /*w*/, float /*h*/, void* /*user*/)
{
  // Bezel (the silhouette that casts the shadow).
  px->begin_path(p);
  px->arc(p, kCx, kCy, kRBezel, 0.0f, kTwoPi);
  px->fill_path(p, 0xFF2A2A2A);

  // Gradient face - the designer's #4E4E4E -> #363636 vertical linear.
  neui_gradient_stop_t stops[2] = {
    { 0.0f, 0xFF4E4E4E },
    { 1.0f, 0xFF363636 },
  };
  neui_gradient_t g;
  memset(&g, 0, sizeof(g));
  g.kind       = NEUI_GRADIENT_LINEAR;
  g.stops      = stops;
  g.stop_count = 2;
  g.extend     = NEUI_GRADIENT_EXTEND_CLAMP;
  g.start_x = kCx; g.start_y = kCy - kRFace;
  g.end_x   = kCx; g.end_y   = kCy + kRFace;

  px->begin_path(p);
  px->arc(p, kCx, kCy, kRFace, 0.0f, kTwoPi);
  px->fill_path_gradient(p, &g);

  // Faint top highlight ring.
  px->begin_path(p);
  px->arc(p, kCx, kCy, kRFace - 1.5f, 3.5f, 6.0f);
  px->stroke_path(p, 1.5f, 0x33FFFFFF);
}

// Paint the rotating indicator into its own surface: a tapered pointer from
// the centre toward 12 o'clock, plus a small hub. Pivots around the surface
// centre (== the knob centre), which the compound rotates by the value.
static void NEUI_ABI paint_needle(neui_painter* p, neui_painter_api* px,
                                  float /*w*/, float /*h*/, void* /*user*/)
{
  px->begin_path(p);
  px->move_to(p, kCx,         kCy - (kRFace - 4.0f));
  px->line_to(p, kCx - 3.5f,  kCy);
  px->line_to(p, kCx + 3.5f,  kCy);
  px->close_path(p);
  px->fill_path(p, 0xFFEAF2FF);

  px->begin_path(p);
  px->arc(p, kCx, kCy, 5.0f, 0.0f, kTwoPi);
  px->fill_path(p, 0xFF9FB8D6);
}

// Build the designer's SVG drop-shadow <filter> as a named neui filter graph,
// primitive-for-primitive (the same chain the exported SVG uses):
//   feFlood(opacity 0) -> feColorMatrix(SourceAlpha, hardAlpha)
//     -> feOffset(dy) -> feGaussianBlur(stdDeviation) -> feComposite(out,
//        in2=hardAlpha) -> feColorMatrix(black @ opacity) -> feBlend(over bg)
//     -> feBlend(SourceGraphic over the shadow).
// dy / std_dev are the SVG values; alpha is the shadow opacity (0..1).
static neui_asset_t build_drop_shadow_filter(AppState* a, float dy, float std_dev, float alpha)
{
  neui_asset_t f = a->assets->create_filter(a->session);
  if (f.id == asset_none.id) return f;
  neui_filter_api_t* fi = a->filters;

  // hardAlpha: alpha *= 127 then clamps to 1 -> a hard-edged mask of the source.
  const float hard[20] = { 0,0,0,0,0,  0,0,0,0,0,  0,0,0,0,0,  0,0,0,127,0 };
  // tint: zero RGB, alpha *= `alpha` -> black shadow at the given opacity.
  const float tint[20] = { 0,0,0,0,0,  0,0,0,0,0,  0,0,0,0,0,  0,0,0,alpha,0 };

  neui_filter_prim_t p;
  p = fi->add_primitive(a->session, f, NEUI_FE_FLOOD);
  fi->set_float(a->session, f, p, "flood_opacity", 0.0f);
  fi->set_result(a->session, f, p, "bg");

  p = fi->add_primitive(a->session, f, NEUI_FE_COLOR_MATRIX);
  fi->set_input(a->session, f, p, 0, NEUI_FILTER_SRC_ALPHA);
  fi->set_string(a->session, f, p, "type", "matrix");
  fi->set_floats(a->session, f, p, "values", hard, 20);
  fi->set_result(a->session, f, p, "hardAlpha");

  p = fi->add_primitive(a->session, f, NEUI_FE_OFFSET);
  fi->set_float(a->session, f, p, "dy", dy);

  p = fi->add_primitive(a->session, f, NEUI_FE_GAUSSIAN_BLUR);
  fi->set_float(a->session, f, p, "std_dev", std_dev);

  p = fi->add_primitive(a->session, f, NEUI_FE_COMPOSITE);
  fi->set_string(a->session, f, p, "operator", "out");
  fi->set_input(a->session, f, p, 1, "hardAlpha");

  p = fi->add_primitive(a->session, f, NEUI_FE_COLOR_MATRIX);
  fi->set_string(a->session, f, p, "type", "matrix");
  fi->set_floats(a->session, f, p, "values", tint, 20);

  p = fi->add_primitive(a->session, f, NEUI_FE_BLEND);
  fi->set_string(a->session, f, p, "mode", "normal");
  fi->set_input(a->session, f, p, 1, "bg");
  fi->set_result(a->session, f, p, "shadow");

  p = fi->add_primitive(a->session, f, NEUI_FE_BLEND);
  fi->set_string(a->session, f, p, "mode", "normal");
  fi->set_input(a->session, f, p, 0, NEUI_FILTER_SRC_GRAPHIC);
  fi->set_input(a->session, f, p, 1, "shadow");
  return f;
}

static bool NEUI_ABI on_event(void* /*token*/, neui_event_t* event)
{
  if (event->type == NEUI_EVENT_APP_QUIT) return true;
  return false;  // compound paints, behavior handles input
}

static void* NEUI_ABI get_interface(void* /*token*/, const char* iface)
{
  static neui_widget_client_t widget_client;
  if (!strcmp(iface, NEUI_API_WIDGETS)) {
    widget_client.neui_version = NEUI_VERSION;
    widget_client.ondestroy    = nullptr;
    widget_client.onevent      = on_event;
    return &widget_client;
  }
  return nullptr;
}

int main(int /*argc*/, char* /*argv*/[])
{
  neui_init();
  neui_api_t* host = neui_get_api(ACTIVE_HOST);
  if (!host) host = neui_get_api(nullptr);
  if (!host) { dbglog("[filter] no host\n"); return 1; }

  AppState app;
  app.neui = host;

  neui_client_t client;
  client.neui_version  = NEUI_VERSION;
  client.get_interface = get_interface;

  app.session = host->create_session(&client, &app);
  if (!app.session.session) { dbglog("[filter] no session\n"); return 1; }

  app.widgets  = (neui_widget_api_t*)  host->get_interface(app.session, NEUI_API_WIDGETS);
  app.assets   = (neui_asset_api_t*)   host->get_interface(app.session, NEUI_API_ASSETS);
  app.attrs    = (neui_attr_api_t*)    host->get_interface(app.session, NEUI_API_ATTRS);
  app.compound = (neui_compound_api_t*)host->get_interface(app.session, NEUI_API_COMPOUND);
  app.behavior = (neui_behavior_api_t*)host->get_interface(app.session, NEUI_API_BEHAVIOR);
  // NEUI_API_FILTER is an OPTIONAL extension - a host that can't provide it
  // (e.g. an embedded host with no off-screen surfaces) returns nullptr here.
  // The knob still renders without its shadow, so filters is not required.
  app.filters  = (neui_filter_api_t*)  host->get_interface(app.session, NEUI_API_FILTER);
  if (!app.widgets || !app.assets || !app.attrs || !app.compound || !app.behavior) {
    dbglog("[filter] missing API\n"); return 1;
  }
  if (!app.filters) dbglog("[filter] NEUI_API_FILTER unavailable - drawing knobs without shadow\n");

  // --- Bake the static face: paint the art, then apply the designer's two
  // SVG drop-shadow filters (built primitive-for-primitive via NEUI_API_FILTER).
  // (Bake at the display scale so the symbol stays crisp; 2.0 here for HiDPI.)
  const float bake_scale = 2.0f;
  app.face = app.assets->create_surface(app.session, kCanvas, kCanvas, bake_scale);
  if (app.face.id != asset_none.id) {
    app.assets->paint_surface(app.session, app.face, 0x00000000, paint_face, &app);
    // The designer's drop shadow is one fe* chain (their two filters are the
    // same graph at dy/stdDeviation 9 vs 18). Build it once and apply it -
    // skipped when the optional filter extension is unavailable.
    if (app.filters) {
      neui_asset_t shadow = build_drop_shadow_filter(&app, 9.0f, 9.0f, 0.35f);
      app.assets->apply_filter(app.session, app.face, shadow);
      if (shadow.id != asset_none.id) app.assets->destroy(app.session, shadow);
    }
  }

  app.needle = app.assets->create_surface(app.session, kCanvas, kCanvas, bake_scale);
  if (app.needle.id != asset_none.id)
    app.assets->paint_surface(app.session, app.needle, 0x00000000, paint_needle, &app);

  // --- Build the shared compound (face below + needle above) ----------------
  app.comp = app.assets->create_compound(app.session);
  if (app.comp.id != asset_none.id) {
    neui_compound_layer_t lf = app.compound->add_layer(app.session, app.comp,
                                                       NEUI_COMPOUND_LAYER_ASSET, -1);
    app.compound->set_asset(app.session, app.comp, lf, "asset", app.face);

    neui_compound_layer_t ln = app.compound->add_layer(app.session, app.comp,
                                                       NEUI_COMPOUND_LAYER_ASSET, 1);
    app.compound->set_asset(app.session, app.comp, ln, "asset", app.needle);
    // Rotate the needle 0..1 -> -0.75pi..+0.75pi (1.5pi sweep), the audio-knob
    // convention shared with components/knob.json.
    app.compound->bind(app.session, app.comp, ln, "rotation", NEUI_PARAM_VALUE,
                       4.712389f, -2.356194f);
  }

  // --- Build the shared drag behavior ---------------------------------------
  app.behav = app.assets->create_behavior(app.session);
  if (app.behav.id != asset_none.id) {
    neui_behavior_handler_t drag = app.behavior->add_handler(app.session, app.behav,
                                                             NEUI_BEHAVIOR_KIND_DRAG_ROTATIONAL);
    app.behavior->set_string(app.session, app.behav, drag, "target", NEUI_PARAM_VALUE);
    app.behavior->set_float (app.session, app.behav, drag, "min", 0.0f);
    app.behavior->set_float (app.session, app.behav, drag, "max", 1.0f);
    neui_behavior_handler_t rst = app.behavior->add_handler(app.session, app.behav,
                                                            NEUI_BEHAVIOR_KIND_CONTEXT_RESET);
    app.behavior->set_string(app.session, app.behav, rst, "target", NEUI_PARAM_VALUE);
  }

  neui_widget_t win = app.widgets->create(app.session, neui_widget_t{ UINT32_MAX },
                                          NEUI_W_APPWINDOW, 120, 120, 720, 300, nullptr);
  app.widgets->set_text(app.session, win, "neui filter (drop-shadow knob) example");

  neui_widget_t lbl = app.widgets->create(app.session, win, NEUI_W_LABEL, 16, 12, 460, 20, nullptr);
  app.widgets->set_text(app.session, lbl,
    "Drag to turn, right-click to reset. Shadow baked via an SVG fe* filter graph.");

  const char* names[3] = { "Gain", "Tone", "Mix" };
  float       vals [3] = { 0.25f, 0.55f, 0.80f };
  // The baked surface now includes shadow margin around the disc, so upsize the
  // widget (and spacing) to keep the disc's apparent size ~constant.
  const int   knob_sz  = 144;
  for (int i = 0; i < 3 && app.comp.id != asset_none.id; ++i) {
    int x = 20 + i * 165;
    neui_widget_t k = app.widgets->create(app.session, win, NEUI_W_CUSTOMDRAW,
                                          x, 40, knob_sz, knob_sz, nullptr);
    app.widgets->set_asset(app.session, k, app.comp);    // visual
    app.widgets->set_asset(app.session, k, app.behav);   // input
    app.attrs->set_float(app.session, k, NEUI_PARAM_VALUE,   vals[i]);
    app.attrs->set_float(app.session, k, NEUI_PARAM_DEFAULT, 0.5f);

    neui_widget_t cap = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                            x, 40 + knob_sz + 4, 120, 18, nullptr);
    app.widgets->set_text(app.session, cap, names[i]);
  }

  app.widgets->show(app.session, win);
  host->run(app.session);

  if (app.comp.id   != asset_none.id) app.assets->destroy(app.session, app.comp);
  if (app.behav.id  != asset_none.id) app.assets->destroy(app.session, app.behav);
  if (app.needle.id != asset_none.id) app.assets->destroy(app.session, app.needle);
  if (app.face.id   != asset_none.id) app.assets->destroy(app.session, app.face);
  host->destroy(app.session);
  return 0;
}
