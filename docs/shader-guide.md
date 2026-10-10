<!-- neui guide. A practical introduction to .nsl shaders; the normative spec is docs/shader-language.md. -->

# Writing shaders for neui

This guide teaches you to write `.nsl` shaders and put them on screen. It
covers the language in practice, the coordinate system, uniforms, a set of
recipes, animation, the client API and performance.

The normative definition of every rule is in `docs/shader-language.md` (the
spec). A bare § refers to a section of this guide, and "spec §N" to the
spec. The client API is `include/neui/d/shader.h`. Every shader in this
guide compiles as written, and `neui_shader_example` runs the techniques it
describes.

## 1. What a shader is in neui

A `.nsl` shader is an **image generator**: a function from a position to a
colour.
- neui runs it once per device pixel of a rectangle and draws the result like
  a bitmap, inside the normal paint order. Clip, transform, alpha and the
  z-order of compound layers all apply as for any other drawing.
- The only inputs are the position and the uniforms. There are no textures,
  no state kept between pixels or frames, and no side effects.
- On the CPU, the pixels are bit-identical on every platform and compiler.
- On Windows (Direct2D), live shaders run on the GPU by default, within a
  small tolerance of the CPU result (§7.7).

Shaders are an **optional** host interface. `get_interface(sess,
NEUI_API_SHADER)` returns NULL on the null platform, the LVGL host and
builds configured with `-DNEUI_NO_SHADERS=ON`. Always check it, and keep a
plain look to fall back to (§7.8).

**When to use one:** for procedural art that would otherwise be a stack of
bitmaps. Examples are a knob whose glow follows its value, brushed metal at
any size, a gradient that doesn't band, or a value-noise texture. Ordinary
shapes, text and icons are cheaper with the painter or a compound.

## 2. A first shader

```glsl
vec4 main(vec2 p) {
  vec2  c = neui_size * 0.5;              // the centre of the layer
  float r = min(c.x, c.y) - 2.0;          // a radius 2 px inside the edge
  float inside = step(length(p - c), r);  // 1 inside the circle, 0 outside
  return vec4(1.0, 0.6, 0.2, inside);     // orange, alpha = inside
}
```

- `main` is the entry point. It takes the position `p` and returns the
  colour.
- `neui_size` is the layer's size. It is one of four reserved uniforms that
  neui fills in (§3).
- The colour is **straight** (not premultiplied) RGBA in 0..1. neui
  premultiplies it and clamps it, and a NaN becomes 0.

The edge of that disc is jagged, because every pixel is fully in or fully
out. Give it an anti-aliased edge one device pixel wide:

```glsl
vec4 main(vec2 p) {
  vec2  c = neui_size * 0.5;
  float r = min(c.x, c.y) - 2.0;
  float d = length(p - c) - r;            // signed distance: < 0 inside
  float a = saturate(0.5 - d / neui_px);  // a one-device-pixel ramp across the edge
  return vec4(1.0, 0.6, 0.2, a);
}
```

`neui_px` is one device pixel in logical px. Edges written with it stay
equally crisp at 100%, 150% and 200%. Almost every shape in §5 is built on
this "signed distance, then coverage" pattern.

**Try it** by baking it into an IMAGE widget:

```cpp
neui_shader_api_t* sh = (neui_shader_api_t*)neui->get_interface(sess, NEUI_API_SHADER);
if (sh) {
  char err[256];
  neui_asset_t disc = sh->create(sess, src, (uint32_t)strlen(src), "disc.nsl", err, sizeof err);
  if (disc.id == asset_none.id) { fprintf(stderr, "%s\n", err); /* show it, fall back */ }
  neui_shader_bake_t b{};                    // zero-initialise: fields may be appended
  b.width = b.height = 64;                   // logical px
  b.scale = 2;                               // device px per logical px
  neui_asset_t bmp = sh->bake_bitmap(sess, disc, &b);
  widgets->set_asset(sess, image, bmp);      // an NEUI_W_IMAGE
}
```

## 3. Coordinates

| Name | Type | Meaning |
|---|---|---|
| `p` | `vec2` | the centre of the device pixel being shaded, in **layer-local logical px**, **y down**; (0, 0) is the layer's top-left corner |
| `neui_size` | `vec2` | the layer's size in logical px |
| `neui_scale` | `float` | device px per logical px (display scale × the layer's `resolution`) |
| `neui_px` | `float` | `1.0 / neui_scale`: one device pixel in logical px |
| `neui_time` | `float` | seconds on the animation clock, wrapped to [0, 3600) (§6) |

Write shaders in logical px and they come out sharp at any display scale:
neui evaluates every device pixel and never resamples the result. On a 2×
display, the first pixel of a layer is at `p = (0.25, 0.25)`.

Common normalisations:

```glsl
vec2 uv = p / neui_size;                          // 0..1 on both axes (stretches)
vec2 q  = p / neui_size.y;                        // 0..aspect across, 0..1 down (keeps aspect)
vec2 d  = (p - neui_size * 0.5) / min(neui_size.x, neui_size.y);  // centred, -0.5..0.5 on the short side
```

**y is down**, as everywhere in neui. Angles measured with `atan(y, x)` turn
clockwise on screen. For a knob, `atan(d.x, -d.y)` with `d = p - centre` is
0 at 12 o'clock and grows clockwise, from -π to π (§5.2).

## 4. The language in brief

`.nsl` is a small subset of GLSL ES 1.00. If you know GLSL, the differences
that matter are:

- **Types:** `float`, `vec2`, `vec3`, `vec4` and `bool`. `int` exists only at
  compile time: literals, `const int`, and loop counters. A plain `int k = 3;`
  is an error ("int variables must be const"), and so is int division
  (`1 / 2`): write `1.0 / 2.0`. An int converts to float by itself wherever a
  float is expected, so `float(i) / 10.0` and `i * 0.25` both work.
- **Vectors:** component-wise arithmetic, with a scalar broadcast against a
  vector (`0.5 + 0.5 * v`). Constructors take components in order (`vec4(c.rgb,
  1.0)`, `vec3(0.2)`). Swizzles use `.xyzw`, `.rgba` or `.stpq`, one set at a
  time. `<` and `>` compare scalars only.
- **Functions** must be defined above their first call (there are no
  prototypes). They are inlined, cannot recurse, and take `in` (the default),
  `out` or `inout` parameters. User functions cannot be overloaded.
- **Globals** are `uniform` or `const`. There are no mutable globals.
- **Control flow:** `if` / `else`, `?:`, and `for` loops with constant
  bounds. A loop runs at most 64 times, nested loops at most 1024 times in
  total, and `break` / `continue` work. Loops are unrolled, so a loop costs
  its full trip count even when every pixel breaks out early.
- **Not in v1:** `while`, `do`, `switch`, arrays, structs, matrices,
  textures, `discard`, `%` (use `mod`), bitwise operators, and the
  preprocessor (the only directive is `#pragma neui <name>`).
- Uninitialised variables are zero, and every edge case is defined
  (`normalize(vec2(0))` is zero, `mod` floors, `x / 0.0` is ±inf).

```glsl
uniform float value = 0.5;

float cover(float d) { return saturate(0.5 - d / neui_px); }

// Polar coordinates around c: the distance, and the angle clockwise from
// 12 o'clock.
void polar(vec2 p, vec2 c, out float radius, out float angle) {
  vec2 d = p - c;
  radius = length(d);
  angle  = atan(d.x, -d.y);
}

vec4 main(vec2 p) {
  float r, a;
  polar(p, neui_size * 0.5, r, a);

  // Eleven ticks from 7:30 to 4:30, lit up to `value`.
  float lit = 0.0;
  for (int i = 0; i <= 10; i++) {
    float t = float(i) / 10.0;
    if (t > value) break;                                    // the rest stay dark
    float across = abs(a - mix(-2.3561945, 2.3561945, t)) * r;  // arc distance to the tick
    lit = max(lit, cover(across - 1.0) * cover(abs(r - 35.0) - 5.0));
  }
  return vec4(1.0, 0.8, 0.3, lit);
}
```

The full list of built-ins is in §11. Their definitions are in spec §9.

### 4.1 Uniforms

Uniforms are the shader's parameters. Declare them with a default:

```glsl
uniform float value  = 0.5;                          // 0..1
uniform vec4  accent = vec4(0.25, 0.66, 0.96, 1.0);  // a colour, straight RGBA

vec4 main(vec2 p) {
  vec2  c = neui_size * 0.5;
  float R = min(c.x, c.y) - 2.0;
  float d = length(p - c) - R * mix(0.3, 1.0, value);
  float a = saturate(0.5 - d / neui_px);
  return vec4(accent.rgb, accent.a * a);
}
```

- **Types:** `float`, `vec2`, `vec3` and `vec4`. A uniform without a default
  starts at zero.
- **Values come from the client**, by name: the `u.<name>` props of a
  compound layer (set or bound to a widget attribute), `uniforms` in `draw`
  and in the bakes, or `"uniforms"` in component JSON (§7). A name the shader
  does not declare is **ignored without an error**. When a uniform seems to
  have no effect, check its spelling first.
- **Colours** go in a `vec4` as straight RGBA. From the client, an ARGB int
  arrives exactly as `byte / 255` per channel: use `neui_shader_color(name,
  argb)`, `set_int` on a `u.` prop, or `"#AARRGGBB"` in JSON.
- **Work that depends only on uniforms runs once per draw, not once per
  pixel.** The compiler hoists it, and the reserved uniforms count as
  uniforms too. In the shader above, `c`, `R` and `R * mix(0.3, 1.0, value)`
  are computed once. Derive what you need from uniforms freely.

## 5. Recipes

### 5.1 Shapes from signed distances

A signed distance is negative inside a shape, zero on its edge and positive
outside, in logical px. `cover(d)` turns it into an anti-aliased alpha, and
shapes combine with `min` (union), `max` (intersection) and `max(a, -b)`
(a minus b). `over` stacks colours from bottom to top.

```glsl
// Coverage of a signed distance d (logical px, < 0 inside): an anti-aliased
// edge one device pixel wide at any display scale.
float cover(float d) { return saturate(0.5 - d / neui_px); }

float circle(vec2 p, vec2 c, float r) { return length(p - c) - r; }

// A ring of radius r and width w.
float ring(vec2 p, vec2 c, float r, float w) { return abs(length(p - c) - r) - w * 0.5; }

// A box centred on c, half-size h, corner radius r.
float rounded_box(vec2 p, vec2 c, vec2 h, float r) {
  vec2 q = abs(p - c) - (h - vec2(r));
  return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
}

// A line from a to b; give it a width with `- w * 0.5`.
float segment(vec2 p, vec2 a, vec2 b) {
  vec2  pa = p - a, ba = b - a;
  float h  = saturate(dot(pa, ba) / dot(ba, ba));
  return length(pa - ba * h);
}

// Straight-alpha "over": top drawn on bottom.
vec4 over(vec4 top, vec4 bottom) {
  float a = top.a + bottom.a * (1.0 - top.a);
  vec3  c = (top.rgb * top.a + bottom.rgb * (bottom.a * (1.0 - top.a))) / max(a, 1e-6);
  return vec4(c, a);
}

vec4 main(vec2 p) {
  vec2 c = neui_size * 0.5;
  vec4 col = vec4(0.16, 0.17, 0.20, cover(rounded_box(p, c, c - vec2(4.0), 12.0)));
  col = over(vec4(0.9, 0.5, 0.2, cover(ring(p, c, 40.0, 6.0))), col);
  col = over(vec4(1.0, 1.0, 1.0, cover(segment(p, c, c + vec2(28.0, -28.0)) - 2.0)), col);
  col = over(vec4(0.9, 0.9, 0.9, cover(max(circle(p, c, 10.0), -circle(p, c, 6.0)))), col);
  return col;
}
```

There is no `#include`, so copy the helpers you need into each shader. In
C++, keeping them in a string and concatenating it in front of each source
works well (`NSL_ROUNDED` in `examples/shader_example.cpp`).

### 5.2 A value arc for a knob

The usual 270° knob track runs from 7:30 to 4:30. Take the angle from 12
o'clock, clockwise (§3), and light the track up to the value:

```glsl
uniform float value  = 0.5;
uniform vec4  accent = vec4(0.25, 0.66, 0.96, 1.0);

float cover(float d) { return saturate(0.5 - d / neui_px); }

vec4 main(vec2 p) {
  vec2  c   = neui_size * 0.5;
  float R   = min(c.x, c.y);
  vec2  d   = p - c;
  float ang = atan(d.x, -d.y);          // 0 at 12 o'clock, clockwise, -pi..pi

  const float k_start = -2.3561945;     // -135 degrees: 7:30
  const float k_range =  4.712389;      // 270 degrees, to 4:30
  float track = 1.0 - step(-k_start, abs(ang));             // inside the 270 degrees
  float lit   = track * (1.0 - step(k_start + value * k_range, ang));

  float ring = cover(abs(length(d) - R * 0.8) - R * 0.07);
  vec3  col  = mix(vec3(0.20, 0.22, 0.27), accent.rgb, lit);
  return vec4(col, ring * track * accent.a);
}
```

`k_glow_knob` in `examples/shader_example.cpp` builds on this with a glow,
a shaded body and a pointer dot.

### 5.3 Gradients without banding

```glsl
#pragma neui dither                     // hides banding in slow gradients

uniform vec4 top    = vec4(0.18, 0.20, 0.26, 1.0);
uniform vec4 bottom = vec4(0.08, 0.09, 0.12, 1.0);

vec4 main(vec2 p) {
  vec4  col = mix(top, bottom, p.y / neui_size.y);           // vertical
  vec2  c   = vec2(neui_size.x * 0.3, 0.0);                  // a light at the top
  float l   = 1.0 - saturate(length(p - c) / neui_size.x);   // radial falloff
  col.rgb  += vec3(0.10, 0.12, 0.16) * l * l;
  return col;
}
```

A dark gradient over a few hundred pixels has more pixels than 8-bit steps,
so it bands. `#pragma neui dither` rounds each pixel with an ordered 4×4
pattern that is anchored to the layer, so it does not crawl when the layer
moves. It costs nothing measurable.

### 5.4 Noise, texture and grain

`neui_noise(v)` is smooth value noise in [0, 1) with features about one unit
apart. `neui_hash(v)` is one random value in [0, 1) per integer cell of `v`.
Both are integer-exact, so they give the same result on the GPU as on the
CPU.

```glsl
uniform vec4 dark  = vec4(0.10, 0.12, 0.20, 1.0);
uniform vec4 light = vec4(0.85, 0.80, 0.70, 1.0);

// Fractal noise: five octaves of value noise, each at twice the frequency
// and half the weight of the last.
float fbm(vec2 u) {
  float n = 0.0, amp = 0.5;
  for (int i = 0; i < 5; i++) {
    n  += neui_noise(u) * amp;
    u   = u * 2.03 + vec2(1.7, 9.2);    // shift each octave so they don't line up
    amp *= 0.5;
  }
  return n;
}

vec4 main(vec2 p) {
  float n     = fbm(p / neui_size.y * 3.0);
  float vein  = abs(sin(p.x / neui_size.y * 6.0 + n * 9.0));
  float grain = neui_hash(p * neui_scale) - 0.5;             // one value per device pixel
  vec3  col   = mix(light.rgb, dark.rgb, smoothstep(0.0, 0.35, vein) * 0.85);
  return vec4(col + grain * 0.04, 1.0);
}
```

- **Scale noise in logical px** (`p / 24.0`: features about 24 px apart), so
  the texture looks the same at every display scale.
- **Grain** should be per device pixel: `p * neui_scale` puts one hash cell
  on each device pixel.
- **The classic `fract(sin(x) * 43758.5)` hash works,** and is identical on
  every CPU. On the GPU, `sin` of a large argument differs in the last bits,
  and that hash amplifies them. Prefer `neui_hash` for anything that has to
  match the CPU.

## 6. Animation

A shader that reads `neui_time` animates:

```glsl
uniform vec4 accent = vec4(1.0, 0.35, 0.25, 1.0);

vec4 main(vec2 p) {
  // One beat every 1.5 s. 1.5 divides 3600, so the hourly wrap of
  // neui_time is invisible.
  float beat = fract(neui_time / 1.5);
  float glow = exp(-beat * 4.0);                       // a sharp rise, a slow decay
  vec2  c    = neui_size * 0.5;
  float d    = length(p - c) - min(c.x, c.y) * 0.45;
  float core = saturate(0.5 - d / neui_px);
  float halo = exp(-max(d, 0.0) / 6.0) * glow;
  return vec4(accent.rgb, max(core, halo) * accent.a);
}
```

- **`neui_time` wraps to [0, 3600)**, because DAW sessions run for hours and
  a float loses precision as it grows. Give every cycle a period that divides
  3600 s (1, 1.5, 2, 4, 5, 6, 8, 10, 12, 15, ...) and the jump never shows:
  `sin(neui_time * 6.2831853 / 4.0)` repeats exactly every 4 s.
- **On a compound SHADER layer, it animates by itself.** When the layer
  neither sets nor binds `"time"`, the shader runs on the session's animation
  clock, and the widget repaints on every clock frame (30 fps by default). On
  the xpl host, each frame repaints only the animating widget.
- **With `draw`,** pass `shader_api->animation_time(sess)` as `time`. Reading
  the clock in a paint is what keeps that widget animating.
- **The animation stops when it is not seen.** A widget that stops painting
  (hidden, scrolled away, a minimised window) drops off the clock, and its
  timer stops. The next paint that reads the clock starts it again.
- **The client controls the clock:** `set_animation_fps(sess, fps)` (1..120;
  never more than the display shows) and `set_animation_paused(sess, true)`.
  The clock also stands still while the OS asks for reduced motion
  (Windows "Show animations" off, Reduce motion on macOS and iOS; not read
  on Linux yet).
- **Driving time yourself:** set or bind the layer's `"time"` prop, for
  example to a transport position attribute. The layer then follows that
  value and ignores the clock.
- **Baking a loop costs almost nothing per frame:** `bake_filmstrip(...,
  sweep = "neui_time", from = 0, to = T * (n - 1) / n, n)` gives n frames of
  a loop of period T. Draw it in a CUSTOMDRAW paint with
  `painter_api->draw_asset_frame` at frame `(uint32_t)(t / T * n) % n`, with
  `t = animation_time(sess)`. Reading the clock keeps the widget animating,
  and each frame is a blit.

A shader that does not read `neui_time` is re-run only when its inputs
change (§8).

## 7. Putting a shader on screen

### 7.1 Compile

```cpp
char err[256];
neui_asset_t s = sh->create(sess, src, (uint32_t)strlen(src), "knob.nsl", err, sizeof err);
// or from a file / the client resource provider (NEUI_RESOURCE_KIND_SHADER):
neui_asset_t t = sh->create_from_file(sess, "skin/knob.nsl", err, sizeof err);
if (s.id == asset_none.id) { /* err: "knob.nsl:12:17: error: 'glow' is not declared" */ }
```

- A failed compile returns `asset_none`, with the first error in `err`. Show
  it somewhere a skin author will see it: `neui_shader_example` puts it in a
  LABEL.
- `uniform_count` / `uniform_at` list the declared uniforms (name, component
  count, default), for tools and inspectors.
- A SHADER asset is destroyed like any asset, with `assets->destroy`. Widgets
  and compounds hold assets **without a refcount**, so destroy them first.
  A baked bitmap does not depend on its shader: the shader can go as soon as
  the bake is done.

### 7.2 Choosing a path

| You want | Use | Cost per frame |
|---|---|---|
| fixed art: a background, a panel, a tile | `bake_bitmap` into a BITMAP | none |
| a control whose look follows **one** value | `bake_filmstrip` over that uniform, drawn by index through a compound ASSET layer's `"frame"` binding | none; memory = frames × size |
| a look that follows several values, colours or the theme, or is too large for a strip | a compound **SHADER layer** with `u.<name>` bindings | a re-shade when an input changes; cached |
| shader output mixed with your own painting | `draw` in a CUSTOMDRAW paint | as the layer |
| motion | a SHADER layer reading `neui_time`, or `draw` + `animation_time` | a re-shade every clock frame |

Start with a bake when you can. Bakes always run on the CPU, so their pixels
are identical everywhere.

### 7.3 Bake

```cpp
// A 128-frame strip of `value` from 0 to 1, in one accent colour.
const neui_shader_uniform_t accent = neui_shader_color("accent", 0xFF3FA9F5u);
neui_shader_bake_t b{};
b.width = b.height = 88;              // logical px
b.scale = 2;                          // sharp on a 2x display
b.uniforms = &accent;
b.uniform_count = 1;
neui_asset_t strip = sh->bake_filmstrip(sess, knob_shader, &b, "value", 0.0f, 1.0f, 128);

// Draw frame = value * 127 over the whole widget.
neui_asset_t comp = assets->create_compound(sess);
neui_compound_layer_t L = compound->add_layer(sess, comp, NEUI_COMPOUND_LAYER_ASSET, 0);
compound->set_anchor(sess, comp, L, NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER);
compound->set_int(sess, comp, L, "width", NEUI_COMPOUND_FILL);
compound->set_int(sess, comp, L, "height", NEUI_COMPOUND_FILL);
compound->set_asset(sess, comp, L, "asset", strip);
compound->bind(sess, comp, L, "frame", NEUI_PARAM_VALUE, 127.0f, 0.0f);
widgets->set_asset(sess, knob, comp);     // a CUSTOMDRAW; add a behavior asset for input
```

- `bake_bitmap` makes one image of `ceil(width * scale)` × `ceil(height *
  scale)` px, tagged with `scale` so that it draws at `width` × `height`
  logical px.
- Bake at the highest scale you expect. A 2× bake is downsampled on a 1×
  display.
- A side may be at most 16384 px. neui chooses a strip's grid of cells
  itself.

### 7.4 A live SHADER layer

```cpp
neui_asset_t comp = assets->create_compound(sess);
// The look where the shader cannot paint: a plain disc.
neui_compound_layer_t disc = compound->add_layer(sess, comp, NEUI_COMPOUND_LAYER_RECT, 0);
/* anchor, "width" / "height", "fill_color", "corner_radius" */

neui_compound_layer_t L = compound->add_layer(sess, comp, NEUI_COMPOUND_LAYER_SHADER, 1);
compound->set_anchor(sess, comp, L, NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER);
compound->set_int(sess, comp, L, "width", NEUI_COMPOUND_FILL);
compound->set_int(sess, comp, L, "height", NEUI_COMPOUND_FILL);
compound->set_asset(sess, comp, L, "shader", knob_shader);
compound->bind(sess, comp, L, "u.value", NEUI_PARAM_VALUE, 1.0f, 0.0f);  // value = attr * 1 + 0
compound->set_float(sess, comp, L, "u.value.step", 1.0f / 256.0f);       // quantise (see below)
compound->bind(sess, comp, L, "u.accent", "app.accent", 1.0f, 0.0f);     // an int attr: a colour
compound->set_int(sess, comp, L, "fallback", (int)disc.id);
// per widget:
attrs->set_int(sess, knob, "app.accent", (int)0xFF3FA9F5);
```

- **Setting uniforms:**
  - `set_float(L, "u.radius", 0.4f)` sets every component.
  - `set_int` on a vec3 / vec4 is an ARGB colour. On a float or vec2 it is
    the number.
  - `set_string(L, "u.center", "0.5, 0.25")` sets the components in order.
  - `"u.center.y"` (also `.x .z .w .r .g .b .a`) sets one component.
- **Binding uniforms:** `bind` works on both the whole and the one-component
  forms. A vec3 / vec4 bound as a whole to an **int** attribute reads it as a
  colour, all 32 bits.
- **`"u.<name>.step"` quantises a binding.** It is the usual binding
  quantisation, and it matters here. Images are cached by content (§8), so a
  1/256 step bounds what DAW automation can produce to 257 images per look.
  Knobs at the same value share one image.
- **Other props:**
  - `"resolution"` (0, 4], default 1, is the shading density. 0.5 shades a
    quarter of the pixels and scales the result up, for soft backgrounds.
  - `"time"` (§6).
  - `"fallback"` names a layer that paints instead when this one cannot: no
    `NEUI_API_SHADER` on the host, or no valid shader.
- **The shader can be attached or swapped after its uniforms are set.**
  Values are matched by name at paint time.

Details: `docs/compound-behavior-component.md`, "Shader layers".

### 7.5 In a component (JSON)

```json
"shaders": {
  "ring": "shaders/ring.nsl",
  "glow": { "source": "uniform float value;\nvec4 main(vec2 p) { ... }\n" }
},
"layers": [
  { "kind": "rect", "name": "disc", "z": 0, "size": [60, 60],
    "fill_color": "#FF2A2E38", "corner_radius": 30 },
  { "kind": "shader", "z": 1, "size": "fill", "shader": "ring",
    "uniforms": { "accent": "#FFE05AD0", "radius": 0.4, "center": [0.5, 0.5] },
    "bind": { "u.value": { "attr": "neui.param.value", "step": 0.00390625 } },
    "resolution": 1, "fallback": "disc" }
]
```

- **Loading:** a path is read through the client resource provider first,
  then from the component's directory.
- **`"uniforms"`:** a `"#AARRGGBB"` string is a colour, a number sets every
  component, and an array sets the components in order.
- **Errors are silent.** A shader that fails to load or compile leaves its
  layers painting their fallback, and the error is not reported anywhere.
  While authoring, compile the source with `create` to see the error.

The complete knob is `k_ring_component` in `examples/shader_example.cpp`.

### 7.6 Drawing from a paint callback

```cpp
case NEUI_EVENT_WIDGET_PAINT:
  if (event->data.paint.widget.id == app->panel_id) {
    const neui_shader_uniform_t u[] = {
      { "t", { app->t, 0, 0, 0 } },
      neui_shader_color("accent", app->accent),
    };
    neui_shader_draw_t d{};                     // zero-initialise: fields may be appended
    d.shader = app->plasma;
    d.width  = event->data.paint.width;         // x = y = 0: the widget's top-left
    d.height = event->data.paint.height;
    d.resolution = 0.5f;                        // optional
    d.time = sh->animation_time(app->sess);     // only to animate
    d.uniforms = u;
    d.uniform_count = 2;
    sh->draw(event->data.paint.p, &d);
    return true;
  }
  break;
```

- `draw` uses the painter's current clip, transform and alpha. Draw before
  or after your own painter calls to layer them.
- It returns false (and draws nothing) for a bad handle, an empty rect or a
  side over 16384 device px.

### 7.7 GPU, and exact pixels

- **Where it runs:** on Windows, the live paths (the SHADER layer and `draw`)
  run on the GPU when the layer sits on the device grid (no rotation, and
  `resolution` 1).
- **Fidelity:** within 2/255 per channel of the CPU on 99.9% of pixels. A
  pixel exactly on a hard edge (`step`, a comparison, `floor`) can land on the
  other side.
- **Compile delay:** the GPU form compiles in the background. Until it is
  ready, the CPU draws.
- **Forcing the CPU:** when exact pixels matter more than speed, use
  `#pragma neui cpu_only` in the shader, or `set_gpu(sess, false)` for a whole
  session.

### 7.8 Without shader support

When `NEUI_API_SHADER` is NULL:
- a SHADER layer stays in its compound and paints its `"fallback"` layer,
  or nothing if it has none;
- component JSON with shader layers still loads.

So the cheapest robust skin gives every shader layer a fallback layer.
In C++ code, test the pointer and build the plain look instead (as
`neui_shader_example` does with plain KNOB widgets).

## 8. Performance

A shader costs about **pixels × operations**, and the cost is paid on every
re-shade.
- A SHADER layer or `draw` re-shades only when its inputs change: size,
  scale, sub-pixel position, uniform values, and the time when the shader
  reads it.
- The result is cached per session by content.
- An idle UI with shaders costs nothing.

Measured on the CPU executor (single thread, ns per device pixel; a knob is
120×120 logical px at 2×, 57,600 px):

| Shader | Apple M1 Ultra | Intel i7-6800K (2016) | one knob re-shade |
|---|---|---|---|
| flat colour | 0.9 | 4 | 0.05-0.23 ms |
| ring glow (29 ops) | 6 | 13 | 0.4-0.8 ms |
| plasma (35 ops, 6 `sin`) | 14 | 35-36 | 0.8-2.1 ms |
| realistic metal knob (128 ops) | 25 | 62-66 | 1.4-3.8 ms |
| fbm, 5 octaves (191 ops) | 44-49 | 134 | 2.5-7.7 ms |

An 800×600 background at 2× (1.9 Mpx) with a heavy shader (176 ops) costs
70-170 ms. That is fine for a bake, but not for animation.

How to keep it cheap:

- **Bake what doesn't change.** Bakes and filmstrips cost nothing per frame.
- **Quantise bindings** with `u.<name>.step`. Automation then produces a
  bounded set of images, and widgets at the same value share one.
- **Read `neui_time` only in shaders that should move.** Reading it makes
  the shader re-run on every frame of the clock.
- **Lower `resolution`** for soft content (glows, backgrounds, blurs): 0.5
  shades a quarter of the pixels.
- **Count the transcendentals.** `sin`, `cos`, `exp`, `log`, `pow` and `atan`
  each cost several ordinary operations, `neui_noise` costs four hashes, and
  a loop multiplies its body.
- **Move work to uniforms.** Anything that does not depend on `p` runs once
  per draw (§4.1).
- **Size effects for the CPU.** macOS, iOS and Linux always run shaders on
  the CPU. The Windows GPU path buys headroom, not a different look.

## 9. Errors and troubleshooting

A diagnostic reads `<name>:<line>:<col>: error: <message>`, with 1-based
lines and byte columns. Only the first error is reported. Common ones:

| Message (abridged) | Fix |
|---|---|
| `int division; write 1.0 / 2.0` | use float literals: `1.0 / 2.0` |
| `int variables must be const (ints are compile-time values)` | use a `float`, or `const int` for a constant |
| `'g' is not declared (functions must be defined before they are called)` | move the function above its first use; also what HLSL's `lerp` / `fmod` / `frac` give (use `mix` / `mod` / `fract`) |
| `loop runs 128 times; the limit is 64` | fewer iterations, or a larger step per iteration |
| `the loop bound must be an int` | loop bounds must be constants, not uniforms: loop to a constant maximum and `break` |
| `nested loops run 2048 times in total; the limit is 1024` | reduce the inner or outer count |
| `shader needs 32894 operations after inlining and unrolling; the limit is 8192` | the shader is too heavy: fewer octaves or iterations, less work in loops |
| `global variables must be 'const' or 'uniform'` | make it `const`, or compute it inside `main` |
| `uniforms are float, vec2, vec3 or vec4` | no `int` / `bool` uniforms: use a float (`value > 0.5`) |
| `uniform 'v' cannot be assigned` | copy it to a local first |
| `the condition is float, expected bool` | compare explicitly: `if (x > 0.0)` |
| `'<' compares scalars only, not vec2 and vec2` | compare components, or use `step` |
| `cannot construct vec3 from vec2` | supply every component: `vec3(p, 0.0)` |
| `swizzle 'xg' mixes component sets` | `.xy` or `.rg`, not both |
| `'%' is not supported in .nsl v1; use mod()` | `mod(x, y)` (floored, unlike HLSL's `fmod`) |
| `'neui_glow' is reserved (names starting with neui_)` | rename it; `gl_` and `__` are reserved as well |
| `'gl_FragCoord' is reserved; main returns the colour ...` | use `p` and return the colour |
| `the preprocessor is not supported ...` | use `const` instead of `#define`, and paste helper functions in |
| `no 'vec4 main(vec2 p)' is defined` | the entry point must be exactly `vec4 main(vec2 ...)` |

**Nothing shows?** Check, in order:
1. Was `NEUI_API_SHADER` NULL, so the fallback (or nothing) painted?
2. Did the compile fail? In a component the failure is silent (§7.5).
3. Is the alpha zero? `vec4(col, 0.0)` is invisible, and the colour is
   straight alpha, so don't premultiply yourself.
4. Is the layer size zero?
5. Is a uniform name misspelt? Unknown names are ignored.
6. Is the bound attribute set on this widget?

## 10. Porting

**From Shadertoy:**

```glsl
// Shadertoy
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
  vec2 uv = fragCoord / iResolution.xy;
  vec3 col = 0.5 + 0.5 * cos(iTime + uv.xyx + vec3(0, 2, 4));
  fragColor = vec4(col, 1.0);
}
```

```glsl
// .nsl
vec4 main(vec2 p) {
  vec2 uv  = vec2(p.x, neui_size.y - p.y) / neui_size;   // y-up, 0..1, like fragCoord / iResolution
  vec3 col = 0.5 + 0.5 * cos(neui_time + uv.xyx + vec3(0.0, 2.0, 4.0));
  return vec4(col, 1.0);
}
```

- **Coordinates:** `fragCoord` is in device px with **y up**, and `p` is in
  logical px with **y down**. Flip with `neui_size.y - p.y` when it matters.
- **Equivalents:** `iResolution.xy` is `neui_size`, and `iTime` is
  `neui_time` (wrapped to an hour, §6).
- **Not available:** textures (`iChannel0`, `texture`) are not in v1.
- **Loops:** data-dependent and `while` loops become a `for` to a constant
  maximum plus `break`.
- **Integers:** int variables become `const` or `float`.

**From HLSL:** `float4` → `vec4`, `lerp` → `mix`, `frac` → `fract`,
`atan2(y, x)` → `atan(y, x)`, `rsqrt` → `inversesqrt`. **`fmod` is not
`mod`:** HLSL truncates, `.nsl` floors, and the two differ for negative
operands. There is no `mul`, because there are no matrices.

More in spec §16.

## 11. Quick reference

**Built-ins** (component-wise on `float` / `vecN` unless noted; definitions
and edge cases in spec §9):

| Group | Functions |
|---|---|
| exact | `abs` `sign` `floor` `ceil` `fract` `mod` `min` `max` `clamp` `saturate` `mix` `step` `smoothstep` `sqrt` `inversesqrt` `radians` `degrees` |
| geometry | `dot` `length` `distance` `normalize` `cross` (vec3) `reflect` |
| transcendental | `sin` `cos` `tan` `asin` `acos` `atan(y, x)` `atan(t)` `exp` `exp2` `log` `log2` `pow` |
| noise | `neui_hash(vec2)` → [0, 1) per integer cell, `neui_noise(vec2)` → [0, 1) smooth |

**Reserved uniforms:** `neui_size`, `neui_scale`, `neui_px` and `neui_time`
(§3).

**Pragmas:**
- `#pragma neui dither` (§5.3);
- `#pragma neui cpu_only` (§7.7).

**Limits** (spec §13):

| Limit | Value |
|---|---|
| source size | 64 KiB |
| uniforms | 32 declarations, 64 floats |
| user functions | 64 |
| loop trip count | 64 |
| loop nesting depth | 4 |
| product of nested loop trip counts | 1024 |
| operations per pixel after inlining and unrolling | 8192 |

**Keywords:** `uniform const float int bool vec2 vec3 vec4 void true false if
else for break continue return in out inout`, plus `precision lowp mediump
highp`, which are accepted and ignored.

**See also:**
- `docs/shader-language.md`, the spec;
- `include/neui/d/shader.h`, the API;
- `docs/rendering-and-assets.md` ("Shaders", "Animation"), how it works
  inside;
- `examples/shader_example.cpp`, everything above, running.
