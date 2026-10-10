// shader_example - shaders through NEUI_API_SHADER (plans/shader-effects.md).
//
// Baked once into ordinary bitmaps:
//   * A brushed-metal banner: bake_bitmap at the banner's size, drawn by a
//     compound ASSET layer with a TEXT layer on top.
//   * Six glow knobs: bake_filmstrip sweeps the shader's `value` uniform over
//     128 frames (one strip per accent colour). A compound ASSET layer binds
//     its "frame" prop to NEUI_PARAM_VALUE and a behavior drags the value, so
//     a knob costs nothing per frame after the bake.
//   * Three tiles: two plasma bakes at different `t`, one fbm marble.
// Live:
//   * Three knobs whose compound has a SHADER layer: "u.value" is bound to
//     NEUI_PARAM_VALUE (stepped to 1/256) and "u.accent" to an int attr that
//     holds each knob's colour. Re-shaded only when the value changes, and
//     cached by content, so knobs at one value share an image.
//   * A knob from a JSON component with an inline shader and a fallback disc.
//   * A CUSTOMDRAW panel drawn with shader_api->draw; the first live knob
//     drives its `t`.
//   * A checkbox for set_gpu: where the renderer has a GPU path (Direct2D),
//     the live row runs on the GPU by default; unticked, on the CPU.
// And a deliberately broken shader, whose compile error is shown in a label.
// Without NEUI_API_SHADER (null platform, LVGL), plain KNOB widgets.
//
// The pixels are bit-identical on every platform: the CPU executor runs the
// shaders with neui's own deterministic math.

#include "neui/neui.h"

#include <chrono>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef WIN32
#define ACTIVE_HOST "neui.host.win32"
#elif defined(__APPLE__)
#define ACTIVE_HOST "neui.host.macos"
#else
#define ACTIVE_HOST "neui.host.crossplatform"
#endif

static void dbglog(const char* fmt, ...)
{
  char buf[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  fputs(buf, stderr);
}

// A knob: dark body, a value arc from 7 to 5 o'clock in the accent colour
// with a soft glow, and a pointer dot. `value` is swept by the filmstrip bake.
static const char* const k_glow_knob = R"(
uniform float value = 0.5;
uniform vec4  accent = vec4(0.25, 0.66, 0.96, 1.0);

// Straight-alpha "over".
vec4 over(vec4 top, vec4 bottom) {
  float a = top.a + bottom.a * (1.0 - top.a);
  vec3 c = (top.rgb * top.a + bottom.rgb * (bottom.a * (1.0 - top.a))) / max(a, 1e-6);
  return vec4(c, a);
}

vec4 main(vec2 p) {
  vec2  c  = neui_size * 0.5;
  float R  = min(c.x, c.y);
  vec2  d  = p - c;
  float r  = length(d);
  float aa = neui_px;

  float ang   = atan(d.x, -d.y);                 // clockwise from 12 o'clock
  float a0    = -2.3561945;                      // -135 degrees
  float sweep = a0 + value * 4.712389;
  float range = 1.0 - step(2.3561945, abs(ang)); // the 270-degree track
  float lit   = range * (1.0 - step(sweep, ang));

  float ring_r = R * 0.80;
  float ring_w = R * 0.065;
  float ring_d = abs(r - ring_r) - ring_w;
  float ring   = 1.0 - smoothstep(-aa, aa, ring_d);
  float glow   = exp(-max(ring_d, 0.0) / (R * 0.07)) * lit * 0.55;

  vec4 col = vec4(accent.rgb, glow * accent.a);
  col = over(vec4(mix(vec3(0.17, 0.19, 0.23), accent.rgb, lit), ring * range), col);

  float body_r = R * 0.62;
  float shade  = d.y / body_r * 0.5 + 0.5;
  vec3  body   = mix(vec3(0.22, 0.24, 0.29), vec3(0.08, 0.09, 0.11), shade);
  col = over(vec4(body, 1.0 - smoothstep(-aa, aa, r - body_r)), col);

  vec2  tip = c + vec2(sin(sweep), -cos(sweep)) * (body_r * 0.70);
  float dot_a = 1.0 - smoothstep(-aa, aa, length(p - tip) - R * 0.07);
  return over(vec4(accent.rgb, dot_a), col);
})";

// Brushed metal: streaks that vary fast across rows and slowly along them, a
// soft sheen, and a bevelled edge.
static const char* const k_brushed_metal = R"(
uniform vec4 tint = vec4(0.60, 0.62, 0.66, 1.0);

vec4 main(vec2 p) {
  float s = neui_noise(vec2(p.x * 0.015, p.y * 2.0)) * 0.6
          + neui_noise(vec2(p.x * 0.05, p.y * 5.0)) * 0.4;
  float x = p.x / neui_size.x - 0.35;
  float sheen = 0.85 + 0.25 * exp(-x * x * 9.0);
  vec3 col = tint.rgb * (0.82 + 0.28 * s) * sheen;
  float top = 1.0 - smoothstep(0.0, 1.5, min(p.x, p.y));
  float bot = 1.0 - smoothstep(0.0, 1.5, min(neui_size.x - p.x, neui_size.y - p.y));
  col = mix(col, vec3(0.92), top * 0.6);
  col = mix(col, vec3(0.25), bot * 0.6);
  return vec4(col, 1.0);
})";

// Rounded-rect coverage, shared by the tiles.
#define NSL_ROUNDED R"(
float rounded(vec2 p, float r) {
  vec2 q = abs(p - neui_size * 0.5) - (neui_size * 0.5 - vec2(r));
  float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
  return 1.0 - smoothstep(-neui_px * 0.5, neui_px * 0.5, d);
}
)"

static const char* const k_plasma = NSL_ROUNDED R"(
uniform float t = 1.7;

vec4 main(vec2 p) {
  vec2 u = p / neui_size * 10.0;
  float v = sin(u.x + t) + sin((u.y + t) * 0.5) + sin((u.x + u.y + t) * 0.5);
  vec2 c = u + vec2(sin(t * 0.33), cos(t * 0.5)) * 5.0;
  v += sin(sqrt(dot(c, c) + 1.0) + t);
  v *= 3.14159;
  vec3 col = vec3(sin(v), sin(v + 2.094), sin(v + 4.189)) * 0.5 + 0.5;
  return vec4(col, rounded(p, 10.0));
})";

static const char* const k_marble = NSL_ROUNDED R"(
uniform vec4 dark  = vec4(0.10, 0.12, 0.20, 1.0);
uniform vec4 light = vec4(0.85, 0.80, 0.70, 1.0);

vec4 main(vec2 p) {
  vec2  u = p / neui_size.y * 3.0;
  float n = 0.0;
  float amp = 0.5;
  for (int i = 0; i < 5; i++) {
    n += neui_noise(u) * amp;
    u = u * 2.03 + vec2(1.7, 9.2);
    amp *= 0.5;
  }
  float vein = abs(sin(p.x / neui_size.y * 6.0 + n * 9.0));
  vec3 col = mix(light.rgb, dark.rgb, smoothstep(0.0, 0.35, vein) * 0.85);
  return vec4(col, rounded(p, 10.0));
})";

// A component: a ring knob with its shader inline and a plain disc as the
// fallback where shaders are not supported.
static const char* const k_ring_component = R"json({
  "component": "ring_knob",
  "size": [88, 88],
  "params": [ { "key": "neui.param.value", "default": 0.5, "min": 0, "max": 1 } ],
  "shaders": {
    "ring": { "source": "uniform float value = 0.5;\nuniform vec4 accent = vec4(1.0);\nvec4 main(vec2 p) {\n  vec2 c = neui_size * 0.5;\n  vec2 d = p - c;\n  float R = min(c.x, c.y) * 0.78;\n  float ang = atan(d.x, -d.y);\n  float lit = step(abs(ang), 2.3561945) * step(ang, -2.3561945 + value * 4.712389);\n  float ring = 1.0 - smoothstep(-neui_px, neui_px, abs(length(d) - R) - R * 0.10);\n  float disc = 1.0 - smoothstep(-neui_px, neui_px, length(d) - R * 0.62);\n  vec3 col = mix(vec3(0.20, 0.22, 0.27), accent.rgb, lit);\n  return vec4(mix(col, vec3(0.13, 0.14, 0.17), disc), max(ring, disc));\n}\n" }
  },
  "layers": [
    { "kind": "rect", "name": "disc", "z": 0, "size": [60, 60],
      "fill_color": "#FF2A2E38", "corner_radius": 30 },
    { "kind": "shader", "z": 1, "size": "fill", "shader": "ring",
      "uniforms": { "accent": "#FFE05AD0" },
      "bind": { "u.value": { "attr": "neui.param.value", "step": 0.00390625 } },
      "fallback": "disc" }
  ],
  "behavior": [
    { "kind": "drag_vertical", "target": "neui.param.value", "sweep": 160 },
    { "kind": "wheel" },
    { "kind": "context_reset" }
  ]
})json";

// An unknown identifier: the compile error goes into a label.
static const char* const k_broken = "vec4 main(vec2 p) {\n  return vec4(glow, 1.0);\n}\n";

static const int k_knobs = 6;
static const int k_live = 4;  // three compound knobs + one component
static const int k_frames = 128;
static const float k_bake_scale = 2.0f;  // sharp on a 2x display, downsampled on 1x

struct AppState
{
  neui_api_t*          neui     = nullptr;
  neui_widget_api_t*   widgets  = nullptr;
  neui_asset_api_t*    assets   = nullptr;
  neui_attr_api_t*     attrs    = nullptr;
  neui_compound_api_t* compound = nullptr;
  neui_behavior_api_t* behavior = nullptr;
  neui_shader_api_t*   shaders  = nullptr;  // null where shaders are unsupported
  neui_session_t       session  = { 0 };

  uint32_t knob_ids[k_knobs] = {};
  uint32_t live_ids[k_live] = {};
  uint32_t readout_id = 0;

  // The CUSTOMDRAW panel drawn with shader_api->draw; live knob 1 drives `t`.
  uint32_t     panel_id = 0;
  neui_asset_t plasma = asset_none;
  float        plasma_t = 1.7f;
  uint32_t     gpu_box_id = 0;
};

static neui_asset_t compile(AppState* a, const char* src, const char* name)
{
  char err[256];
  neui_asset_t s = a->shaders->create(a->session, src, (uint32_t)strlen(src), name, err, sizeof err);
  if (s.id == asset_none.id) dbglog("[shader] %s\n", err);
  return s;
}

// A compound that draws `bitmap` over the whole widget; `frame_scale` > 0
// binds the frame to NEUI_PARAM_VALUE (value 0..1 -> frame 0..N-1).
static neui_asset_t bitmap_compound(AppState* a, neui_asset_t bitmap, float frame_scale)
{
  neui_asset_t comp = a->assets->create_compound(a->session);
  neui_compound_layer_t L = a->compound->add_layer(a->session, comp, NEUI_COMPOUND_LAYER_ASSET, 0);
  a->compound->set_anchor(a->session, comp, L, NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER);
  a->compound->set_int(a->session, comp, L, "width", NEUI_COMPOUND_FILL);
  a->compound->set_int(a->session, comp, L, "height", NEUI_COMPOUND_FILL);
  a->compound->set_asset(a->session, comp, L, "asset", bitmap);
  if (frame_scale > 0.0f)
    a->compound->bind(a->session, comp, L, "frame", NEUI_PARAM_VALUE, frame_scale, 0.0f);
  return comp;
}

// A compound for a live knob: a SHADER layer over the whole widget, its
// value and accent bound to attributes, and a plain disc that paints instead
// where the shader cannot.
static neui_asset_t live_knob_compound(AppState* a, neui_asset_t shader)
{
  neui_asset_t comp = a->assets->create_compound(a->session);
  neui_compound_layer_t disc = a->compound->add_layer(a->session, comp, NEUI_COMPOUND_LAYER_RECT, 0);
  a->compound->set_anchor(a->session, comp, disc, NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER);
  a->compound->set_int(a->session, comp, disc, "width", 60);
  a->compound->set_int(a->session, comp, disc, "height", 60);
  a->compound->set_int(a->session, comp, disc, "fill_color", (int)0xFF2A2E38);
  a->compound->set_float(a->session, comp, disc, "corner_radius", 30.0f);

  neui_compound_layer_t L = a->compound->add_layer(a->session, comp, NEUI_COMPOUND_LAYER_SHADER, 1);
  a->compound->set_anchor(a->session, comp, L, NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER);
  a->compound->set_int(a->session, comp, L, "width", NEUI_COMPOUND_FILL);
  a->compound->set_int(a->session, comp, L, "height", NEUI_COMPOUND_FILL);
  a->compound->set_asset(a->session, comp, L, "shader", shader);
  a->compound->bind(a->session, comp, L, "u.value", NEUI_PARAM_VALUE, 1.0f, 0.0f);
  a->compound->set_float(a->session, comp, L, "u.value.step", 1.0f / 256.0f);
  a->compound->bind(a->session, comp, L, "u.accent", "app.accent", 1.0f, 0.0f);  // an int: a colour
  a->compound->set_int(a->session, comp, L, "fallback", (int)disc.id);
  return comp;
}

static void set_readout(AppState* a, int knob, float value)
{
  char buf[96];
  snprintf(buf, sizeof buf, "knob %d: value %.3f", knob + 1, (double)value);
  a->widgets->set_text(a->session, { a->readout_id }, buf);
}

static bool NEUI_ABI on_event(void* token, neui_event_t* event)
{
  auto* a = static_cast<AppState*>(token);
  if (event->type == NEUI_EVENT_ATTR_CHANGED) {
    for (int i = 0; i < k_knobs; ++i)
      if (a->knob_ids[i] && event->data.attr.widget.id == a->knob_ids[i]) {
        set_readout(a, i, event->data.attr.value);
        return false;  // the compound binding repaints the knob
      }
    for (int i = 0; i < k_live; ++i)
      if (a->live_ids[i] && event->data.attr.widget.id == a->live_ids[i]) {
        set_readout(a, k_knobs + i, event->data.attr.value);
        if (i == 0 && a->panel_id) {
          a->plasma_t = 1.7f + event->data.attr.value * 6.2831853f;
          a->widgets->invalidate(a->session, { a->panel_id });
        }
        return false;
      }
  }
  if (event->type == NEUI_EVENT_CHECKBOX_CHANGED && a->gpu_box_id &&
      event->data.checkbox.widget.id == a->gpu_box_id) {
    a->shaders->set_gpu(a->session, event->data.checkbox.state == NEUI_CHECK_CHECKED);
    for (uint32_t id : a->live_ids)
      if (id) a->widgets->invalidate(a->session, { id });
    a->widgets->invalidate(a->session, { a->panel_id });
    return false;
  }
  if (event->type == NEUI_EVENT_WIDGET_PAINT && a->panel_id &&
      event->data.paint.widget.id == a->panel_id) {
    const neui_shader_uniform_t t = { "t", { a->plasma_t, 0, 0, 0 } };
    neui_shader_draw_t d{};
    d.shader = a->plasma;
    d.width = event->data.paint.width;
    d.height = event->data.paint.height;
    d.resolution = 0.5f;  // a soft background: shade a quarter of the pixels
    d.uniforms = &t;
    d.uniform_count = 1;
    a->shaders->draw(event->data.paint.p, &d);
    return true;
  }
  if (event->type == NEUI_EVENT_VALUE_CHANGED) {  // the plain-KNOB fallback
    for (int i = 0; i < k_knobs; ++i)
      if (a->knob_ids[i] && event->data.value.widget.id == a->knob_ids[i]) {
        set_readout(a, i, event->data.value.value);
        return false;
      }
  }
  return false;
}

static void* NEUI_ABI get_interface(void* /*token*/, const char* iface)
{
  static neui_widget_client_t widget_client;
  if (!strcmp(iface, NEUI_API_WIDGETS)) {
    widget_client.neui_version = NEUI_VERSION;
    widget_client.ondestroy = nullptr;
    widget_client.onevent = on_event;
    return &widget_client;
  }
  return nullptr;
}

int main(int /*argc*/, char* /*argv*/[])
{
  neui_init();
  neui_api_t* host = neui_get_api(ACTIVE_HOST);
  if (!host) host = neui_get_api(nullptr);
  if (!host) { dbglog("[shader] no host\n"); return 1; }

  AppState app;
  app.neui = host;
  neui_client_t client;
  client.neui_version = NEUI_VERSION;
  client.get_interface = get_interface;
  app.session = host->create_session(&client, &app);
  if (!app.session.session) { dbglog("[shader] no session\n"); return 1; }

  app.widgets  = (neui_widget_api_t*)host->get_interface(app.session, NEUI_API_WIDGETS);
  app.assets   = (neui_asset_api_t*)host->get_interface(app.session, NEUI_API_ASSETS);
  app.attrs    = (neui_attr_api_t*)host->get_interface(app.session, NEUI_API_ATTRS);
  app.compound = (neui_compound_api_t*)host->get_interface(app.session, NEUI_API_COMPOUND);
  app.behavior = (neui_behavior_api_t*)host->get_interface(app.session, NEUI_API_BEHAVIOR);
  app.shaders  = (neui_shader_api_t*)host->get_interface(app.session, NEUI_API_SHADER);
  if (!app.widgets || !app.assets || !app.attrs || !app.compound || !app.behavior) {
    dbglog("[shader] missing API\n");
    return 1;
  }

  const int pad = 12, knob = 88, knob_gap = 16, tile_w = 196, tile_h = 120;
  const int win_w = 2 * pad + k_knobs * knob + (k_knobs - 1) * knob_gap;  // 632
  const int banner_h = 64, knob_y = pad + banner_h + 16, readout_y = knob_y + knob + 10;
  const int tile_y = readout_y + 32, live_label_y = tile_y + tile_h + 16, live_y = live_label_y + 26;
  const int error_y = live_y + knob + 16, timing_y = error_y + 26;
  const int win_h = timing_y + 22 + pad;

  neui_widget_t win = app.widgets->create(app.session, widget_none, NEUI_W_APPWINDOW,
                                          120, 120, win_w, win_h, nullptr);
  app.widgets->set_text(app.session, win, "neui shaders");

  neui_widget_t readout = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                              pad, readout_y, 300, 22, nullptr);
  app.readout_id = readout.id;

  if (!app.shaders) {
    neui_widget_t note = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                             pad, pad, win_w - 2 * pad, 22, nullptr);
    app.widgets->set_text(app.session, note,
                          "NEUI_API_SHADER is not available on this host: plain knobs instead.");
    for (int i = 0; i < k_knobs; ++i) {
      neui_widget_t k = app.widgets->create(app.session, win, NEUI_W_KNOB,
                                            pad + i * (knob + knob_gap), knob_y, knob, knob, nullptr);
      app.knob_ids[i] = k.id;
      app.attrs->set_float(app.session, k, NEUI_PARAM_VALUE, 0.5f);
    }
    set_readout(&app, 0, 0.5f);
    app.widgets->show(app.session, win);
    host->run(app.session);
    host->destroy(app.session);
    return 0;
  }

  const auto t0 = std::chrono::steady_clock::now();
  int bakes = 0;

  // Banner: brushed metal + a title.
  neui_asset_t metal = compile(&app, k_brushed_metal, "brushed_metal.nsl");
  neui_shader_bake_t bake{};
  bake.width = (float)(win_w - 2 * pad);
  bake.height = (float)banner_h;
  bake.scale = k_bake_scale;
  neui_asset_t banner_bmp = app.shaders->bake_bitmap(app.session, metal, &bake);
  ++bakes;
  neui_asset_t banner_comp = bitmap_compound(&app, banner_bmp, 0.0f);
  neui_compound_layer_t title =
    app.compound->add_layer(app.session, banner_comp, NEUI_COMPOUND_LAYER_TEXT, 1);
  app.compound->set_anchor(app.session, banner_comp, title, NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER);
  app.compound->set_string(app.session, banner_comp, title, "text", "Baked with NEUI_API_SHADER");
  app.compound->set_float(app.session, banner_comp, title, "size", 22.0f);
  app.compound->set_int(app.session, banner_comp, title, "color", (int)0xFF20242C);
  neui_widget_t banner = app.widgets->create(app.session, win, NEUI_W_CUSTOMDRAW,
                                             pad, pad, win_w - 2 * pad, banner_h, nullptr);
  app.widgets->set_asset(app.session, banner, banner_comp);

  // Knobs: one 128-frame strip per accent colour, two knobs each.
  neui_asset_t glow = compile(&app, k_glow_knob, "glow_knob.nsl");
  const uint32_t accents[3] = { 0xFF3FA9F5u, 0xFFF5A23Fu, 0xFF6BE07Au };
  neui_asset_t strips[3], knob_comps[3];
  for (int c = 0; c < 3; ++c) {
    const neui_shader_uniform_t accent = neui_shader_color("accent", accents[c]);
    bake = neui_shader_bake_t{};
    bake.width = bake.height = (float)knob;
    bake.scale = k_bake_scale;
    bake.uniforms = &accent;
    bake.uniform_count = 1;
    strips[c] = app.shaders->bake_filmstrip(app.session, glow, &bake, "value", 0.0f, 1.0f, k_frames);
    bakes += k_frames;
    knob_comps[c] = bitmap_compound(&app, strips[c], (float)(k_frames - 1));
  }
  neui_asset_t behav = app.assets->create_behavior(app.session);
  neui_behavior_handler_t drag =
    app.behavior->add_handler(app.session, behav, NEUI_BEHAVIOR_KIND_DRAG_VERTICAL);
  app.behavior->set_float(app.session, behav, drag, "sweep", 160.0f);
  app.behavior->add_handler(app.session, behav, NEUI_BEHAVIOR_KIND_WHEEL);
  app.behavior->add_handler(app.session, behav, NEUI_BEHAVIOR_KIND_CONTEXT_RESET);
  for (int i = 0; i < k_knobs; ++i) {
    neui_widget_t k = app.widgets->create(app.session, win, NEUI_W_CUSTOMDRAW,
                                          pad + i * (knob + knob_gap), knob_y, knob, knob, nullptr);
    app.knob_ids[i] = k.id;
    app.widgets->set_asset(app.session, k, knob_comps[i / 2]);
    app.widgets->set_asset(app.session, k, behav);
    const float v = 0.15f + 0.14f * (float)i;
    app.attrs->set_float(app.session, k, NEUI_PARAM_DEFAULT, v);
    app.attrs->set_float(app.session, k, NEUI_PARAM_VALUE, v);
  }
  set_readout(&app, 0, 0.15f);

  // Tiles: two plasma bakes at different `t`, one marble.
  neui_asset_t plasma = compile(&app, k_plasma, "plasma.nsl");
  neui_asset_t marble = compile(&app, k_marble, "marble.nsl");
  neui_asset_t tiles[3];
  for (int i = 0; i < 3; ++i) {
    const neui_shader_uniform_t t = { "t", { i == 0 ? 1.7f : 5.3f, 0, 0, 0 } };
    bake = neui_shader_bake_t{};
    bake.width = (float)tile_w;
    bake.height = (float)tile_h;
    bake.scale = k_bake_scale;
    bake.uniforms = &t;
    bake.uniform_count = 1;
    tiles[i] = app.shaders->bake_bitmap(app.session, i < 2 ? plasma : marble, &bake);
    ++bakes;
    neui_widget_t img = app.widgets->create(app.session, win, NEUI_W_IMAGE,
                                            pad + i * (tile_w + (win_w - 2 * pad - 3 * tile_w) / 2),
                                            tile_y, tile_w, tile_h, nullptr);
    app.widgets->set_asset(app.session, img, tiles[i]);
  }

  const double ms =
    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

  // Live: three compound knobs on the glow shader, one component knob, and a
  // panel drawn with shader_api->draw.
  neui_widget_t live_label = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                                 pad, live_label_y, 380, 22, nullptr);
  app.widgets->set_text(app.session, live_label,
                        "Live: re-shaded when the value changes (drag the knobs)");
  neui_widget_t gpu_box = app.widgets->create(app.session, win, NEUI_W_CHECKBOX,
                                              win_w - pad - 200, live_label_y, 200, 22, nullptr);
  app.widgets->set_text(app.session, gpu_box, "GPU where available");
  app.widgets->set_check(app.session, gpu_box, NEUI_CHECK_CHECKED);
  app.gpu_box_id = gpu_box.id;
  neui_asset_t live_comp = live_knob_compound(&app, glow);
  for (int i = 0; i < 3; ++i) {
    neui_widget_t k = app.widgets->create(app.session, win, NEUI_W_CUSTOMDRAW,
                                          pad + i * (knob + knob_gap), live_y, knob, knob, nullptr);
    app.live_ids[i] = k.id;
    app.widgets->set_asset(app.session, k, live_comp);
    app.widgets->set_asset(app.session, k, behav);
    app.attrs->set_int(app.session, k, "app.accent", (int)accents[i]);
    const float v = 0.25f + 0.25f * (float)i;
    app.attrs->set_float(app.session, k, NEUI_PARAM_DEFAULT, v);
    app.attrs->set_float(app.session, k, NEUI_PARAM_VALUE, v);
  }
  neui_asset_t ring = app.assets->create_component_from_string(
    app.session, k_ring_component, (uint32_t)strlen(k_ring_component), nullptr);
  if (ring.id != asset_none.id) {
    neui_widget_t k = app.widgets->create_from_component(app.session, win, ring,
                                                         pad + 3 * (knob + knob_gap), live_y, knob, knob);
    app.live_ids[3] = k.id;
  } else {
    dbglog("[shader] the ring component did not load\n");
  }
  app.plasma = plasma;
  app.plasma_t = 1.7f + 0.25f * 6.2831853f;
  const int panel_x = pad + 4 * (knob + knob_gap);
  neui_widget_t panel = app.widgets->create(app.session, win, NEUI_W_CUSTOMDRAW,
                                            panel_x, live_y, win_w - pad - panel_x, knob, nullptr);
  app.panel_id = panel.id;

  // The broken shader's diagnostic.
  char err[256];
  neui_asset_t broken = app.shaders->create(app.session, k_broken, (uint32_t)strlen(k_broken),
                                            "broken.nsl", err, sizeof err);
  char line[320];
  snprintf(line, sizeof line, "Broken shader: %s", broken.id == asset_none.id ? err : "compiled?");
  neui_widget_t error = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                            pad, error_y, win_w - 2 * pad, 22, nullptr);
  app.widgets->set_text(app.session, error, line);

  snprintf(line, sizeof line, "Compiled 4 shaders and baked %d images in %.0f ms (%.0fx scale).",
           bakes, ms, (double)k_bake_scale);
  neui_widget_t timing = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                             pad, timing_y, win_w - 2 * pad, 22, nullptr);
  app.widgets->set_text(app.session, timing, line);
  dbglog("[shader] %s\n", line);

  app.widgets->show(app.session, win);
  host->run(app.session);

  // Widgets reference these assets without a refcount: tear the window down first.
  app.widgets->destroy(app.session, win);
  app.assets->destroy(app.session, behav);
  app.assets->destroy(app.session, live_comp);
  if (ring.id != asset_none.id) app.assets->destroy(app.session, ring);
  app.assets->destroy(app.session, banner_comp);
  app.assets->destroy(app.session, banner_bmp);
  for (int c = 0; c < 3; ++c) {
    app.assets->destroy(app.session, knob_comps[c]);
    app.assets->destroy(app.session, strips[c]);
  }
  for (neui_asset_t t : tiles) app.assets->destroy(app.session, t);
  const neui_asset_t programs[] = { metal, glow, plasma, marble };
  for (neui_asset_t s : programs) app.assets->destroy(app.session, s);
  host->destroy(app.session);
  return 0;
}
