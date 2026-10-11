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
// Animated, on the session's animation clock:
//   * A panel whose compound SHADER layer reads neui_time and sets no "time":
//     it animates by itself.
//   * The same shader drawn with shader_api->draw at half resolution, with
//     animation_time() as its time; it counts its paints for an fps readout.
//   * An fps cap (set_animation_fps) and a pause toggle (set_animation_paused).
// Sampling bitmaps (sampler2D), all three textures baked by shaders at start:
//   * Two knobs lit from a normal map: the SHADER layer's "u.normals" is the
//     baked map, so one small bitmap replaces a filmstrip; the highlight stays
//     put while the knob turns.
//   * A meter that looks its colours up in a 256 x 1 ramp; its level follows
//     the first lit knob.
//   * Water from two layers of a baked seamless noise tile ("repeat"),
//     drifting on the animation clock.
// And a deliberately broken shader, whose compile error is shown in a label
// (how a client reports a skin's shader error).
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

// The plasma on the animation clock: neui_time instead of a `t` uniform.
static const char* const k_plasma_anim = NSL_ROUNDED R"(
vec4 main(vec2 p) {
  float t = neui_time * 1.5;
  vec2 u = p / neui_size.y * 4.0;
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

// ---- Sampling bitmaps: textures baked by shaders, sampled by live ones ----

static const char* const k_normal_bake = R"(
// A machined knob's surface as a normal map (rgb = n * 0.5 + 0.5, y down):
// a shallow dome, fine rings and a rounded edge.
vec4 main(vec2 p) {
  vec2  d = (p - neui_size * 0.5) / (neui_size * 0.5);   // -1..1 across
  float r = length(d);
  float slope = 0.35 * r                                  // the dome
              + 0.05 * sin(r * 80.0)                      // machined rings
              + 1.6 * smoothstep(0.84, 0.98, r);          // the rounded edge
  vec3  n = normalize(vec3(normalize(d) * slope, 1.0));
  return vec4(n * 0.5 + 0.5, 1.0);
}
)";

static const char* const k_lit_knob = R"(
uniform sampler2D normals;           // rgb = surface normal * 0.5 + 0.5, y down, z out
uniform float value  = 0.5;          // 0..1
uniform vec4  accent = vec4(0.25, 0.66, 0.96, 1.0);

vec4 main(vec2 p) {
  vec2  c = neui_size * 0.5;
  float R = min(c.x, c.y);
  vec2  d = p - c;
  // The knob turns with the value; the light does not. Look up the normal
  // map in the knob's own frame, then turn the normal back to the screen.
  float a  = (value - 0.5) * 4.712389;                         // 270 degrees of travel
  float ca = cos(a), sa = sin(a);
  vec2  q  = vec2(ca * d.x + sa * d.y, ca * d.y - sa * d.x);
  vec3  n  = texture2D(normals, q / (2.0 * R) + 0.5).xyz * 2.0 - 1.0;
  n.xy = vec2(ca * n.x - sa * n.y, sa * n.x + ca * n.y);
  vec3  l    = normalize(vec3(-0.5, -0.7, 1.0));               // light from the top left
  float diff = saturate(dot(n, l));
  float spec = pow(saturate(dot(reflect(-l, n), vec3(0.0, 0.0, 1.0))), 32.0);
  vec3  col  = vec3(0.16, 0.17, 0.20) + vec3(0.55) * diff + vec3(0.6) * spec;
  vec2  tip  = c + vec2(sa, -ca) * R * 0.7;                    // the pointer
  col = mix(col, accent.rgb, 1.0 - smoothstep(-neui_px, neui_px, length(p - tip) - R * 0.08));
  return vec4(col, 1.0 - smoothstep(R - neui_px, R, length(d)));
}
)";

static const char* const k_noise_tile = R"(
// Seamless fbm: the lattice wraps every `period` cells, so the edges of the
// tile meet. Bake it once, at the size it is sampled at.
float tile_noise(vec2 u, float period) {
  vec2  i = floor(u), f = u - i, s = f * f * (3.0 - 2.0 * f);
  float a = neui_hash(mod(i, period)),                   b = neui_hash(mod(i + vec2(1.0, 0.0), period));
  float c = neui_hash(mod(i + vec2(0.0, 1.0), period)), d = neui_hash(mod(i + vec2(1.0, 1.0), period));
  return mix(mix(a, b, s.x), mix(c, d, s.x), s.y);
}

vec4 main(vec2 p) {
  vec2  u = p / neui_size * 8.0;                 // 8 lattice cells across the tile
  float n = 0.0, amp = 0.5, period = 8.0;
  for (int o = 0; o < 4; o++) {
    n += amp * tile_noise(u, period);
    u *= 2.0;
    period *= 2.0;
    amp *= 0.5;
  }
  return vec4(vec3(n), 1.0);
}
)";

static const char* const k_water = R"(
#pragma neui sampler noise repeat

uniform sampler2D noise;             // the seamless tile above, 128 px
uniform vec4 deep    = vec4(0.05, 0.08, 0.20, 1.0);
uniform vec4 shallow = vec4(0.10, 0.55, 0.65, 1.0);

vec4 main(vec2 p) {
  vec2  drift = vec2(6.0, 2.5) * neui_time;                    // px per second
  float a = texture2D(noise, (p + drift) / 128.0).r;           // one tile per 128 px
  float b = texture2D(noise, (p * 0.5 - drift.yx) / 128.0).r;  // a larger, slower layer
  float n = a * 0.6 + b * 0.4;
  return vec4(mix(deep.rgb, shallow.rgb, smoothstep(0.3, 0.7, n)), 1.0);
}
)";

static const char* const k_ramp = R"(
// A meter ramp, baked to 256 x 1: green, then yellow, then red.
vec4 main(vec2 p) {
  float t = p.x / neui_size.x;
  vec3  c = mix(vec3(0.20, 0.80, 0.30), vec3(0.95, 0.85, 0.20), smoothstep(0.45, 0.70, t));
  return vec4(mix(c, vec3(0.95, 0.25, 0.20), smoothstep(0.80, 0.90, t)), 1.0);
}
)";

static const char* const k_meter = R"(
#pragma neui sampler palette nearest

uniform sampler2D palette;           // a 256 x 1 colour ramp, bottom to top
uniform float level = 0.7;           // 0..1, e.g. a meter value

vec4 main(vec2 p) {
  float t   = 1.0 - p.y / neui_size.y;                  // 0 at the bottom
  vec3  col = texture2D(palette, vec2(t, 0.5)).rgb;     // the ramp's colour at t
  float lit = step(t, level);                           // 1 below the level
  float seg = step(0.25, fract(p.y / 6.0));             // 6 px segments with a gap
  return vec4(col * mix(0.2, 1.0, lit), seg);
}
)";

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
  neui_items_api_t*    items    = nullptr;
  neui_session_t       session  = { 0 };

  uint32_t knob_ids[k_knobs] = {};
  uint32_t live_ids[k_live] = {};
  uint32_t readout_id = 0;

  // The CUSTOMDRAW panel drawn with shader_api->draw; live knob 1 drives `t`.
  uint32_t     panel_id = 0;
  neui_asset_t plasma = asset_none;
  float        plasma_t = 1.7f;
  uint32_t     gpu_box_id = 0;

  // The animated row: the draw() panel, its fps readout and the controls.
  uint32_t     anim_panel_id = 0;
  neui_asset_t plasma_anim = asset_none;
  uint32_t     fps_combo_id = 0;
  uint32_t     pause_box_id = 0;
  uint32_t     fps_label_id = 0;
  int          anim_paints = 0;
  int          compiled = 0;

  // The sampling row: two lit knobs, the meter, the water.
  uint32_t     lit_ids[2] = {};
  uint32_t     meter_id = 0;
  uint32_t     water_id = 0;
  std::chrono::steady_clock::time_point anim_t0 = std::chrono::steady_clock::now();
};

static const uint32_t k_fps_choices[] = { 10, 30, 60 };

static neui_asset_t compile(AppState* a, const char* src, const char* name)
{
  char err[256];
  neui_asset_t s = a->shaders->create(a->session, src, (uint32_t)strlen(src), name, err, sizeof err);
  if (s.id == asset_none.id) dbglog("[shader] %s\n", err);
  else ++a->compiled;
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

// A compound with one SHADER layer over the whole widget.
static neui_asset_t shader_compound(AppState* a, neui_asset_t shader, neui_compound_layer_t* out)
{
  neui_asset_t comp = a->assets->create_compound(a->session);
  neui_compound_layer_t L = a->compound->add_layer(a->session, comp, NEUI_COMPOUND_LAYER_SHADER, 0);
  a->compound->set_anchor(a->session, comp, L, NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER);
  a->compound->set_int(a->session, comp, L, "width", NEUI_COMPOUND_FILL);
  a->compound->set_int(a->session, comp, L, "height", NEUI_COMPOUND_FILL);
  a->compound->set_asset(a->session, comp, L, "shader", shader);
  *out = L;
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
  if (event->type == NEUI_EVENT_APP_QUIT) return true;  // let the window close
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
    for (int i = 0; i < 2; ++i)
      if (a->lit_ids[i] && event->data.attr.widget.id == a->lit_ids[i]) {
        set_readout(a, k_knobs + k_live + i, event->data.attr.value);
        if (i == 0 && a->meter_id) {  // the meter shows the first lit knob
          a->attrs->set_float(a->session, { a->meter_id }, "app.level", event->data.attr.value);
          a->widgets->invalidate(a->session, { a->meter_id });
        }
        return false;
      }
  }
  if (event->type == NEUI_EVENT_CHECKBOX_CHANGED && a->gpu_box_id &&
      event->data.checkbox.widget.id == a->gpu_box_id) {
    a->shaders->set_gpu(a->session, event->data.checkbox.state == NEUI_CHECK_CHECKED);
    for (uint32_t id : a->live_ids)
      if (id) a->widgets->invalidate(a->session, { id });
    const uint32_t sampling[] = { a->lit_ids[0], a->lit_ids[1], a->meter_id, a->water_id };
    for (uint32_t id : sampling)
      if (id) a->widgets->invalidate(a->session, { id });
    a->widgets->invalidate(a->session, { a->panel_id });
    return false;
  }
  if (event->type == NEUI_EVENT_CHECKBOX_CHANGED && a->pause_box_id &&
      event->data.checkbox.widget.id == a->pause_box_id) {
    const bool paused = event->data.checkbox.state == NEUI_CHECK_CHECKED;
    a->shaders->set_animation_paused(a->session, paused);
    if (paused) a->widgets->set_text(a->session, { a->fps_label_id }, "paused");
    a->anim_paints = 0;
    a->anim_t0 = std::chrono::steady_clock::now();
    return false;
  }
  if (event->type == NEUI_EVENT_ITEM_SELECTED && a->fps_combo_id &&
      event->data.item.widget.id == a->fps_combo_id && event->data.item.index < 3) {
    a->shaders->set_animation_fps(a->session, k_fps_choices[event->data.item.index]);
    return false;
  }
  if (event->type == NEUI_EVENT_WIDGET_PAINT && a->anim_panel_id &&
      event->data.paint.widget.id == a->anim_panel_id) {
    // Reading the clock in the paint is what keeps this widget animating.
    neui_shader_draw_t d{};
    d.shader = a->plasma_anim;
    d.width = event->data.paint.width;
    d.height = event->data.paint.height;
    d.resolution = 0.5f;
    d.time = a->shaders->animation_time(a->session);
    a->shaders->draw(event->data.paint.p, &d);
    ++a->anim_paints;
    const auto now = std::chrono::steady_clock::now();
    const double s = std::chrono::duration<double>(now - a->anim_t0).count();
    if (s >= 1.0) {
      char buf[48];
      snprintf(buf, sizeof buf, "%.1f paints/s", a->anim_paints / s);
      a->widgets->set_text(a->session, { a->fps_label_id }, buf);
      a->anim_paints = 0;
      a->anim_t0 = now;
    }
    return true;
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

int main(int argc, char* argv[])
{
  neui_init();
  // An optional host id, e.g. neui.host.crossplatform, to compare the hosts.
  neui_api_t* host = neui_get_api(argc > 1 ? argv[1] : ACTIVE_HOST);
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
  app.items    = (neui_items_api_t*)host->get_interface(app.session, NEUI_API_ITEMS);
  if (!app.widgets || !app.assets || !app.attrs || !app.compound || !app.behavior || !app.items) {
    dbglog("[shader] missing API\n");
    return 1;
  }

  const int pad = 12, knob = 88, knob_gap = 16, tile_w = 196, tile_h = 120;
  const int win_w = 2 * pad + k_knobs * knob + (k_knobs - 1) * knob_gap;  // 632
  const int banner_h = 64, knob_y = pad + banner_h + 16, readout_y = knob_y + knob + 10;
  const int tile_y = readout_y + 32, live_label_y = tile_y + tile_h + 16, live_y = live_label_y + 26;
  const int anim_label_y = live_y + knob + 16, anim_y = anim_label_y + 30;
  const int tex_label_y = anim_y + knob + 16, tex_y = tex_label_y + 26;
  const int error_y = tex_y + knob + 16, timing_y = error_y + 26;
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

  // Textures for the sampling row, baked by shaders: a knob's normal map, a
  // seamless noise tile and a 256 x 1 colour ramp.
  neui_asset_t normal_bake = compile(&app, k_normal_bake, "knob_normals.nsl");
  neui_asset_t tile_bake = compile(&app, k_noise_tile, "noise_tile.nsl");
  neui_asset_t ramp_bake = compile(&app, k_ramp, "meter_ramp.nsl");
  bake = neui_shader_bake_t{};
  bake.width = bake.height = 128.0f;
  bake.scale = 1.0f;
  neui_asset_t normals_bmp = app.shaders->bake_bitmap(app.session, normal_bake, &bake);
  neui_asset_t noise_bmp = app.shaders->bake_bitmap(app.session, tile_bake, &bake);
  bake.width = 256.0f;
  bake.height = 1.0f;
  neui_asset_t ramp_bmp = app.shaders->bake_bitmap(app.session, ramp_bake, &bake);
  bakes += 3;

  const double ms =
    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const int compiled = app.compiled;

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

  // Animated: the plasma on neui_time as a self-animating SHADER layer, and
  // drawn with draw() + animation_time(); an fps cap and a pause toggle.
  neui_asset_t plasma_anim = compile(&app, k_plasma_anim, "plasma_anim.nsl");
  app.plasma_anim = plasma_anim;
  neui_widget_t anim_label = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                                 pad, anim_label_y + 2, 220, 22, nullptr);
  app.widgets->set_text(app.session, anim_label, "Animated on the session clock");
  neui_widget_t fps_combo = app.widgets->create(app.session, win, NEUI_W_COMBOBOX,
                                                pad + 228, anim_label_y, 96, 26, nullptr);
  for (uint32_t fps : k_fps_choices) {
    char item[16];
    snprintf(item, sizeof item, "%u fps", fps);
    app.items->add(app.session, fps_combo, item, nullptr);
  }
  app.items->set_selected(app.session, fps_combo, 1);  // the default, 30
  app.fps_combo_id = fps_combo.id;
  neui_widget_t pause_box = app.widgets->create(app.session, win, NEUI_W_CHECKBOX,
                                                pad + 336, anim_label_y + 2, 80, 22, nullptr);
  app.widgets->set_text(app.session, pause_box, "Pause");
  app.pause_box_id = pause_box.id;
  neui_widget_t fps_label = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                                pad + 424, anim_label_y + 2, win_w - 2 * pad - 424, 22,
                                                nullptr);
  app.fps_label_id = fps_label.id;
  const int anim_w = (win_w - 2 * pad - 16) / 2;
  neui_asset_t anim_comp = app.assets->create_compound(app.session);
  {
    neui_compound_layer_t L = app.compound->add_layer(app.session, anim_comp,
                                                      NEUI_COMPOUND_LAYER_SHADER, 0);
    app.compound->set_anchor(app.session, anim_comp, L, NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER);
    app.compound->set_int(app.session, anim_comp, L, "width", NEUI_COMPOUND_FILL);
    app.compound->set_int(app.session, anim_comp, L, "height", NEUI_COMPOUND_FILL);
    app.compound->set_asset(app.session, anim_comp, L, "shader", plasma_anim);  // no "time": the clock
  }
  neui_widget_t anim_layer = app.widgets->create(app.session, win, NEUI_W_CUSTOMDRAW,
                                                 pad, anim_y, anim_w, knob, nullptr);
  app.widgets->set_asset(app.session, anim_layer, anim_comp);
  neui_widget_t anim_panel = app.widgets->create(app.session, win, NEUI_W_CUSTOMDRAW,
                                                 win_w - pad - anim_w, anim_y, anim_w, knob, nullptr);
  app.anim_panel_id = anim_panel.id;

  // Sampling bitmaps: knobs lit from the normal map, the meter on the ramp,
  // water from the noise tile.
  neui_widget_t tex_label = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                                pad, tex_label_y, win_w - 2 * pad, 22, nullptr);
  app.widgets->set_text(app.session, tex_label,
                        "Sampling baked bitmaps: a normal map, a colour ramp, a noise tile");
  neui_asset_t lit = compile(&app, k_lit_knob, "lit_knob.nsl");
  neui_asset_t meter = compile(&app, k_meter, "meter.nsl");
  neui_asset_t water = compile(&app, k_water, "water.nsl");
  neui_compound_layer_t L;
  neui_asset_t lit_comp = shader_compound(&app, lit, &L);
  app.compound->bind(app.session, lit_comp, L, "u.value", NEUI_PARAM_VALUE, 1.0f, 0.0f);
  app.compound->set_float(app.session, lit_comp, L, "u.value.step", 1.0f / 256.0f);
  app.compound->bind(app.session, lit_comp, L, "u.accent", "app.accent", 1.0f, 0.0f);
  app.compound->set_asset(app.session, lit_comp, L, "u.normals", normals_bmp);
  for (int i = 0; i < 2; ++i) {
    neui_widget_t k = app.widgets->create(app.session, win, NEUI_W_CUSTOMDRAW,
                                          pad + i * (knob + knob_gap), tex_y, knob, knob, nullptr);
    app.lit_ids[i] = k.id;
    app.widgets->set_asset(app.session, k, lit_comp);
    app.widgets->set_asset(app.session, k, behav);
    app.attrs->set_int(app.session, k, "app.accent", (int)accents[i * 2]);
    const float v = i == 0 ? 0.3f : 0.75f;
    app.attrs->set_float(app.session, k, NEUI_PARAM_DEFAULT, v);
    app.attrs->set_float(app.session, k, NEUI_PARAM_VALUE, v);
  }
  neui_asset_t meter_comp = shader_compound(&app, meter, &L);
  app.compound->set_asset(app.session, meter_comp, L, "u.palette", ramp_bmp);
  app.compound->bind(app.session, meter_comp, L, "u.level", "app.level", 1.0f, 0.0f);
  const int meter_x = pad + 2 * (knob + knob_gap);
  neui_widget_t meter_w = app.widgets->create(app.session, win, NEUI_W_CUSTOMDRAW,
                                              meter_x, tex_y, 24, knob, nullptr);
  app.meter_id = meter_w.id;
  app.widgets->set_asset(app.session, meter_w, meter_comp);
  app.attrs->set_float(app.session, meter_w, "app.level", 0.3f);
  neui_asset_t water_comp = shader_compound(&app, water, &L);  // no "time": the clock
  app.compound->set_asset(app.session, water_comp, L, "u.noise", noise_bmp);
  const int water_x = meter_x + 24 + knob_gap;
  neui_widget_t water_w = app.widgets->create(app.session, win, NEUI_W_CUSTOMDRAW,
                                              water_x, tex_y, win_w - pad - water_x, knob, nullptr);
  app.water_id = water_w.id;
  app.widgets->set_asset(app.session, water_w, water_comp);

  // The broken shader's diagnostic.
  char err[256];
  neui_asset_t broken = app.shaders->create(app.session, k_broken, (uint32_t)strlen(k_broken),
                                            "broken.nsl", err, sizeof err);
  char line[320];
  snprintf(line, sizeof line, "Compile-error demo (broken on purpose): %s",
           broken.id == asset_none.id ? err : "it compiled?");
  neui_widget_t error = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                            pad, error_y, win_w - 2 * pad, 22, nullptr);
  app.widgets->set_text(app.session, error, line);

  snprintf(line, sizeof line, "Compiled %d shaders and baked %d images in %.0f ms (%.0fx scale).",
           compiled, bakes, ms, (double)k_bake_scale);
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
  app.assets->destroy(app.session, anim_comp);
  const neui_asset_t sampling[] = { lit_comp, meter_comp, water_comp, normals_bmp, noise_bmp, ramp_bmp };
  for (neui_asset_t a : sampling) app.assets->destroy(app.session, a);
  if (ring.id != asset_none.id) app.assets->destroy(app.session, ring);
  app.assets->destroy(app.session, banner_comp);
  app.assets->destroy(app.session, banner_bmp);
  for (int c = 0; c < 3; ++c) {
    app.assets->destroy(app.session, knob_comps[c]);
    app.assets->destroy(app.session, strips[c]);
  }
  for (neui_asset_t t : tiles) app.assets->destroy(app.session, t);
  const neui_asset_t programs[] = { metal, glow, plasma, marble, plasma_anim, normal_bake, tile_bake,
                                    ramp_bake, lit, meter, water };
  for (neui_asset_t s : programs) app.assets->destroy(app.session, s);
  host->destroy(app.session);
  return 0;
}
