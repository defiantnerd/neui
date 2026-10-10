# Plan: shader effects - a neui shading language

## Status

**Phases 0 and 1 done (2026-10-10); phase 2 next.** The prerequisite track,
`plans/xpl-partial-repaint.md`, is implemented (P1-P4).

Phase 1 produced the compiler and the CPU executor: the `neui-shader` static
library in `src/shader/` (API `src/shader/nsl.h`, linked by `neui`), with
`tests/test_shader_lang.cpp` and `tests/test_shader_exec.cpp`. No host exposes
it yet. See "Phase 1 results" at the end of this document.

Phase 0 produced:
- **the language specification**, `docs/shader-language.md` (types,
  semantics, every built-in and edge case, pixel grid, output rule, limits,
  grammar);
- **a CPU executor prototype + benchmark**, `tests/shader_bench/` (since
  phase 1 a timing tool over the real library; the goldens moved into
  `neui_tests`);
- **a D2D custom-effect spike**, branch `spike/d2d-shader-effect` (f1774d2),
  run on the Windows machine.

Results and the decisions they forced are in "Phase 0 results" at the end of
this document. The sections above it are updated to match.

## Context

A client wants to use shaders in neui for a new level of visual polish. The
concrete use cases are not known yet. Three are in play, and the design covers
all of them, phased by cost:

1. **Param-driven widget looks:** glowing knobs, meters and sliders whose
   shader inputs follow a value or attribute.
2. **Self-animating effects:** plasma, shimmer or noise that moves by itself.
3. **Post-processing of existing UI:** bloom, glass, CRT.

Constraints:

- **No new dependencies.** OS components are fine (`d3dcompiler_47.dll`,
  `Metal.framework`). Third-party shader tooling is not: glslang,
  SPIRV-Cross, Skia, bgfx, and GL/EGL on Linux are all out.
- **Optional, as usual.** A host or backend that cannot support shaders
  (LVGL, null) returns `nullptr` for the interface.
- **Same look everywhere.** A UI framework wants identical chrome on every
  host. `docs/design-notes.md` makes the CPU filter graph pixel-identical
  across backends for exactly this reason.

What the code gives us today:

- **D2D** already runs on a process-wide D3D11 device with a per-ctx
  `ID2D1DeviceContext` and DXGI flip swap chains, and it already uses one
  D2D effect (`CLSID_D2D1Tint`, `backends/d2d/d2d_backend.cpp` ~1141). The
  design notes call the D2D 1.1 migration the door to "future per-frame"
  effects.
- **CG** (macOS / iOS) is pure CoreGraphics into the `drawRect:` context.
  **Cairo** (Linux) is a CPU image surface blitted via XShm. GL on Linux was
  rejected deliberately, for robustness inside DAWs and over remote X.
- **No public animation clock** exists. There are only internal 16 ms
  heartbeats (toast, scroll bounce).
- **xpl repaints the whole frame** on every invalidate (see the partial
  repaint plan).
- **All four hosts** (xpl, win32, macOS, iOS) paint compound assets through
  the same `hosts/shared/widget_paint_compound.h`.

## TL;DR

The design has four parts:

- **One neui-owned shading language**, a small GLSL subset (`.nsl`), parsed
  and compiled by neui itself.
- **One CPU reference executor** that runs on every backend that can draw a
  bitmap. Its output is bit-identical on every platform.
- **Optional GPU acceleration** only where the backend is already GPU-resident
  (D2D), emitted from the same IR, with the CPU executor as the fallback.
- **Integration through existing concepts:** a new asset kind, a new compound
  layer whose uniforms bind to the `AttrBag` through the *existing* `bind`
  machinery, a component-JSON `kind`, and an optional `NEUI_API_SHADER`
  interface.

The shader result is an image drawn inside the normal paint order. Clip,
transform, alpha, z-order and `show_when` therefore compose without any new
rules.

**The earliest visible win needs no live layer at all:** bake a shader into a
filmstrip once, then drive it with the existing ASSET layer and `frame`
binding.

## 1. How shaders fit neui's concepts

| neui concept | Shader counterpart |
|---|---|
| Optional host interfaces (`NEUI_API_FILTER`, `NEUI_API_EMBED`) | `NEUI_API_SHADER`; `nullptr` where unsupported (LVGL, null backend, `NEUI_NO_SHADERS` builds) |
| Asset kinds (`NEUI_ASSET_KIND_*`) | `NEUI_ASSET_KIND_SHADER = 10` (next free integer) |
| Compound layers (`NEUI_COMPOUND_LAYER_*`) | `NEUI_COMPOUND_LAYER_SHADER = 8` |
| `AttrBag` → bindings | Uniforms are layer props `u.<name>`, set and bound with the existing `set_float` / `set_int` / `bind` |
| JSON components | `"kind": "shader"` layer, plus a `"shaders"` block |
| Resource client | `NEUI_RESOURCE_KIND_SHADER = 7` (5 and 6 are reserved) |
| CPU filter graph | `NEUI_FE_SHADER` primitive, later (bake-time post-process over a SURFACE) |
| CUSTOMDRAW painter | `shader_api->draw(p, ...)`; the public painter vtable does **not** change |
| Filmstrips | `shader_api->bake_filmstrip`, which feeds the existing `frame` binding |

**Why output-as-image wins.** Every executor ends in either "here is a
premultiplied BGRA bitmap" (CPU) or "here is an effect drawn inside the
current device context" (D2D). Both happen at the layer's position in the
normal paint walk. Overlapping widgets, compound z-interleaving, group clips,
rotations, `push_alpha` and disabled-dim all work as they already do.

**Why not native GPU layers** (a `CAMetalLayer` sublayer, a separate swap
chain per effect):

- xpl draws a whole frame into one surface, and a compound interleaves shader
  layers with vector and text layers. A native sublayer would sit above or
  below everything and break both.
- New GPU contexts inside DAW-hosted plugins are a known source of trouble
  (multiple instances, device loss, threading). The D2D path reuses the
  device neui already owns, and no other platform creates a GPU context at
  all.

## 2. The language (`.nsl`)

**The normative specification is now `docs/shader-language.md`.** This
section is the original design summary, and where the two differ, the spec
wins. Phase 0 added:
- `break` / `continue`, `out` / `inout` parameters and `const int`;
- int division as an error; zero-initialised locals;
- the exact pixel grid with edge coverage, the dithering formula and concrete
  limits.

GLSL-ES-flavoured, shaped like SkSL "runtime effects": a single entry point,
uniforms, and nothing else that touches the outside world. Example:

```glsl
// ring_glow.nsl - a value-driven glow ring for a knob
uniform float value;   // 0..1, bound to neui.param.value
uniform vec4  accent;  // set_int(ARGB) arrives as straight RGBA

vec4 main(vec2 p) {
  vec2  c    = neui_size * 0.5;
  float r    = min(c.x, c.y) * 0.8;
  float d    = abs(length(p - c) - r);            // distance to the ring centreline
  float ring = 1.0 - smoothstep(0.0, 1.5 * neui_px, d - 2.0);
  float glow = exp(-d * mix(0.6, 0.15, value));   // wider + brighter as value rises
  float a    = clamp(ring + glow * value, 0.0, 1.0);
  return vec4(accent.rgb, a * accent.a);
}
```

**Entry point and coordinates.** `vec4 main(vec2 p)`:
- `p` is in layer-local **logical** px, **y-down**, at the pixel centre, so it
  matches every other neui coordinate;
- it returns **straight** (non-premultiplied) RGBA in 0..1, and the runtime
  premultiplies;
- **porting note:** Shadertoy is y-up and uses device px. Flip y and use
  `neui_size`.

**Types:** `float`, `vec2`, `vec3`, `vec4`, `bool`; `int` only as a loop
counter.

**Statements:**
- local variables, assignment (including swizzle write masks), `if` / `else`;
- `for (int i = 0; i < N; i++)` with a **literal** `N` ≤ 64;
- non-recursive user functions, inlined by the compiler;
- `return`, including early return.

**Not in v1:** `while`, recursion, arrays and dynamic indexing, matrices
(possibly `mat2` later), textures and sampling (see the filter phase), and
derivatives (`fwidth` / `dFdx`, which need 2×2 quad lanes; deferred).

**Built-ins:**
- **Math:** `abs sign floor ceil fract mod min max clamp saturate mix step
  smoothstep sqrt inversesqrt pow exp log exp2 log2 sin cos tan asin acos
  atan(y,x) length distance dot cross normalize reflect`, and swizzles
  `.xyzw` / `.rgba`. `saturate(x)` is `clamp(x, 0.0, 1.0)`, under the name
  HLSL and bgfx use.
- **Noise:** `neui_hash(vec2)` and `neui_noise(vec2)` are integer-exact, so
  they produce the same value on every executor. The popular
  `fract(sin(x) * 43758.5)` idiom differs per GPU and is documented as "works,
  but not identical across hosts".

**Reserved uniforms** (the `neui_` prefix is reserved):

| Uniform | Meaning |
|---|---|
| `neui_size` | layer size, logical px |
| `neui_scale` | device px per logical px |
| `neui_px` | `1 / neui_scale`, for anti-aliasing edges |
| `neui_time` | seconds; wrapped to a fixed period, because DAW sessions run for hours and float precision decays |

**Limits** (protecting against untrusted skins and against a GPU timeout,
i.e. a TDR on Windows):
- maximum source size;
- AST depth cap (the mujson precedent);
- about 8k ops after unrolling;
- a cap on the product of nested loop counts;
- a register cap.

A shader that exceeds any limit fails to compile with a diagnostic. Its cost
is therefore known statically, and every shader terminates.

**Defined edge cases** (a table in the language spec; every executor must
match it):

| Case | Defined result |
|---|---|
| `mod` | floored, `x - y*floor(x/y)`; emitters never use HLSL `fmod` |
| `pow(x<0, y)` | defined |
| `x/0` | defined |
| NaN at output | 0 |
| `normalize(0)` | 0 |
| `smoothstep(a, a, x)` | defined |
| `clamp(lo > hi)` | defined |

Output rounding is a fixed rule. Optional ordered (Bayer) dithering on
output, identical on every executor, fights banding in large gradients.

**Uniform defaults** (an extension over GLSL): `uniform float r = 0.4;`.
Designer-written shaders then render sensibly with no client setup.

### bgfx source compatibility: considered, not adopted

The client pointed out that many people use bgfx, so we checked whether
`.nsl` should accept bgfx shader source (`.sc`). There are no client `.sc`
shaders, so nothing has to be imported today.

**What bgfx's shader language is.** It is GLSL 1.x with three layers on top:
- a C preprocessor (shaderc ships its own);
- the ~970-line macro header `bgfx_shader.sh`, which maps the GLSL onto
  HLSL, Metal and SPIR-V;
- `$input` / `$output` lines plus a `varying.def.sc` file that wire a
  fragment shader to its vertex shader.

shaderc compiles this offline (glslang, SPIRV-Cross, SPIRV-Tools, DXC).
The bgfx runtime loads only the compiled binaries.

**Why not:**
- **No vertex stage in neui.** A bgfx fragment shader reads values handed
  over by the app's vertex shader (`v_texcoord0`, `v_position`), and the
  vertex shader decides what they mean. An `.nsl` shader gets only `p`,
  `neui_size` and its uniforms.
- **Uniforms.** bgfx has only `vec4`, `mat3`, `mat4` and samplers, so values
  get packed by convention (its NanoVG shader does
  `#define u_feather (u_params.x)`). `.nsl` uniforms are named, typed and
  defaulted, and map 1:1 onto `u.<name>` bindings.
- **Real bgfx shaders need what v1 leaves out:** textures (`SAMPLER2D`,
  `texture2D`), matrices (`mul`, `mtxFromCols`) and preprocessor macros.
  Even a compatibility layer would run almost no existing bgfx shader
  unchanged.
- **Semantics.** bgfx results are whatever the GPU gives. The y-axis
  direction depends on the graphics API, and blending is set from C++, not in
  the shader. `.nsl` promises the same pixels on every host.
- **Cost.** Compatibility would need a full preprocessor, matrix types and
  bgfx's helper functions, and would pull the language toward a 3D pipeline.
  Using shaderc itself breaks the no-new-dependencies rule.

**Porting from bgfx** is mechanical, because both languages are plain GLSL
inside the functions:

| bgfx | `.nsl` |
|---|---|
| `$input v_texcoord0` / `gl_FragCoord` | `p` (layer-local, y-down) and `neui_size` |
| packed `uniform vec4 u_params;` + `#define` | one named uniform per value, with a default |
| `void main() { gl_FragColor = c; }` | `vec4 main(vec2 p) { return c; }` (straight alpha) |
| `vec4_splat(a)` | `vec4(a)` |
| `saturate(x)` | `saturate(x)` (built-in) |
| `texture2D`, `mul`, `mtxFromCols` | not in v1 |

**Left open:** an exporter that writes `.nsl` as bgfx `.sc`, from the same IR
as the HLSL emitter, which is cheap. Build it only if a client ships bgfx and
asks for it. Importing `.sc` stays out.

## 3. Architecture

```
 .nsl source ──► front end (src/shader/nsl_compile.cpp)
                 lex → parse → type-check → line:col diagnostics
                 → scalarised IR (const-fold, DCE, CSE, hoist uniform-only
                   work, unroll, register alloc)
                       │
       ┌───────────────┼──────────────────────────┐
       ▼               ▼                          ▼
  CPU executor     HLSL emitter               MSL emitter (later, if needed)
  src/shader/      hosts/shared/              hosts/shared/
  nsl_exec.cpp
  (reference,      shader_emit_hlsl.h         shader_emit_msl.h
   every host)     → D2D custom effect        → Metal → CGImage
       │
       ▼
  premultiplied BGRA8 → bitmap cache on the shader asset
  → per-ctx upload (create_bitmap) → draw_bitmap inside the normal paint walk
```

### Front end

- `src/shader/nsl_compile.cpp` in the `neui-shader` library, next to the
  executor. (The original design had a header-only front end in
  `hosts/shared/`. But constant folding is shader arithmetic, and a header is
  compiled with whatever flags its includer uses, so it lives in the library
  with the executor's flags.)
- Hand-written lexer and recursive-descent parser in the mujson style, with
  exact `line:col` diagnostics.
- Vector ops are **scalarised** at compile time (a `vec3` add becomes three
  scalar adds), which makes both the CPU lanes and the emitters simple.
- **Uniform-only subexpressions are hoisted** out of the per-pixel program
  and evaluated once per draw.

### CPU executor (the reference)

- **Where:** `src/shader/nsl_exec.cpp`, with the math in
  `src/shader/nsl_math.h` and the IR in `src/shader/nsl_ir.h`. The
  `neui-shader` library is compiled at `-O2` / `/O2` even in Debug builds
  (Debug builds are 10-50× slower). On MSVC, Debug's `/Od`, `/RTC1` **and
  `/Ob0`** are stripped for its directory. `/Ob0` disables all inlining,
  `__forceinline` included, and phase 0 measured the SIMD kernels 6-11×
  slower with it.
- **How it runs:** a batched structure-of-arrays interpreter. Each IR op
  processes a lane of 64 pixels held in float arrays, so dispatch is
  amortised over the lane.
- **Explicit SIMD kernels**, not auto-vectorisation: a small 4-wide wrapper
  over SSE2 (x64 baseline; AVX2 is not guaranteed inside plugins), NEON
  (arm64), and a portable fallback, with the helpers force-inlined. Phase 0
  showed that auto-vectorisation depends on the compiler: clang vectorises
  every kernel, GCC part of them, and MSVC only the trivial ones, which left
  MSVC 3-4× behind.
- **Control flow:** `if` / `else` becomes predication + select. Early
  `return` uses a "returned" mask. Loops have constant trip counts.
- **Bit-identity:**
  - neui ships its own polynomial `sin` / `exp2` / `log2` / `atan2` instead
    of libm;
  - **all** shader arithmetic lives in the executor TU, which is built with
    `-ffp-contract=off` (no FMA contraction). That includes the front end's
    constant folding: the header-only front end calls into the executor's
    math rather than folding inline, because a header is compiled with
    whatever flags its includer uses, and GCC on arm64 contracts by default;
  - the FP environment (round-to-nearest, no flush-to-zero) is pinned per
    draw and restored after it, because a DAW may set flush-to-zero on its
    UI thread;
  - operation order is fixed.

  CRC goldens in CI then guarantee the same pixels on every compiler and
  CPU. Phase 0 confirmed this on AppleClang arm64, MSVC x64, GCC 13 x64 and
  Clang 18 x64.
- **Threads:** single-threaded in v1 and UI-thread-only like all paint. An
  opt-in fork-join pool (pure data, no backend calls, joins before returning)
  is a later item.

### Bitmap cache

- **Where it lives:** on the **shader asset** (`AssetEntry::shader`, a
  `unique_ptr<ShaderAsset>` holding the program, a monotonic `uid` and the
  cache), **not** on the layer.
- **Why not copy the QR layer's cache:**
  - its FIFO of 16 would re-shade most of 50 knobs on every repaint;
  - it leaks GPU uploads (see §8);
  - the CUSTOMDRAW `draw`, the compound layer and a future filter primitive
    can all share one cache on the asset.
- **Key:** program `uid` (not the slot, which is reused), `w_px`, `h_px`,
  `scale × resolution`, the **sub-pixel remainder of the layer's device
  origin** (`X0 - floor(X0)`, `Y0 - floor(Y0)`: `p` depends on it, see the
  spec's pixel grid), the values of the uniforms the program actually reads,
  and the wrapped time, only if the program reads `neui_time`. Widgets on
  whole logical px have at most a few remainders (0 or 0.5 at 150%).
- **Content keying + quantisation:** 50 knobs at the same value share one
  bitmap. With the existing `"<prop>.step"` binding quantisation (e.g.
  `u.value.step = 1/128`), the cache stays bounded even under DAW automation.
- **Eviction:** LRU by paint stamp, within a byte budget (default about
  48-64 MB, configurable). Eviction destroys every per-ctx upload through
  `backend->destroy_bitmap`, which is safe across ctxs, as
  `drop_surface_gpu_cache` already shows.
- **Release:** `release_context`, `clear` and `release_slot` in
  `hosts/shared/asset_store.h` free the cache and its uploads.
- **Upload:** reuses the `qr_upload` pattern (`widget_paint_compound.h` ~401):
  lazy per ctx, and re-upload on a `get_context_generation` bump (device
  loss). The bitmap is drawn 1:1 at the floored device origin and is never
  resampled.
- **Live resize:** while the layer size is changing, draw the last bitmap
  stretched, and re-shade once the size has been stable for a frame.

### GPU executors

**D2D (Windows): the only planned GPU path.**

- **Program creation:**
  - HLSL is emitted from the IR in `hosts/shared/shader_emit_hlsl.h`, so its
    output is Tier-1 golden-testable on every platform.
  - The backend compiles it with `D3DCompile`, loaded at runtime with
    `LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)`.
    That DLL is an OS component on Windows 10+, so there is no link
    dependency, and the system-directory search prevents DLL planting inside a
    DAW.
  - If the DLL is absent, or compilation fails, the program is CPU-only.
- **Effect:**
  - one custom effect class registered once per process on the D2D factory
    (`RegisterEffectFromString`; the factory survives device loss);
  - an `ID2D1EffectImpl` + `ID2D1DrawTransform` with **zero inputs**, whose
    output rect is the layer rect in pixels;
  - uniforms travel in a constant buffer (16-byte padded) set in
    `PrepareForRender`;
  - one effect instance per ctx, cached like `tint_effect` and released with
    the target objects.
- **Shader identity:** the shader GUID is a hash of HLSL text + profile. A
  backend-wide GUID → bytecode map plus `IsShaderLoaded` makes shaders reload
  transparently after device loss.
- **Draw:** `DrawImage` inside the current frame, so clip and transform
  compose. The current alpha (neui's alpha stack) goes into the constant
  buffer, the same trick the tint path uses.
- **Coordinates (settled by the phase-0 spike):**
  - `SCENE_POSITION` is in **device pixels**, with its origin where the
    effect output lands on the target and pixel centres at +0.5. The DIP
    transform does not affect it.
  - The output rect from `MapInputRectsToOutputRect` is in device pixels too.
  - Drawing in DIP mode at a DIP offset is crisp only when the layer's device
    origin is an integer. Otherwise D2D resamples the output (98-100% of
    pixels off by more than 2/255 at 125% and 150%).
  - **Draw it like this:** inside the frame, temporarily `SetDpi(96, 96)` and
    an identity transform; `DrawImage` at `floor(X0, Y0)` with an output rect
    of `ceil(X1) - floor(X0)` × `ceil(Y1) - floor(Y0)` device px; restore.
    The constant buffer carries the sub-pixel remainder and the scale, and
    the shader computes the spec's `p`.
  - Measured: within 0-1/255 of the CPU at 96, 120, 144 and 192 DPI, with
    integer and fractional origins, a translation and a pushed clip. 0% of
    pixels were over 2/255, and the clip pushed in DIP mode is still honoured.
  - **Rotation and skew** cannot use this path: fall back to the CPU bitmap.
    Edge coverage (spec §11) is computed in the shader.
- **Feature level:** the device is created with default feature levels
  (`d2d_backend.cpp` ~229), so it can land on 9_x. Store the level: use
  `ps_4_0` at ≥ 10_0, CPU below that. Compile with
  `D3DCOMPILE_IEEE_STRICTNESS`. (With a null level array, D3D11 stops at
  11_0; ps_4_0 does not need more.)
- **Lazy compile:** `D3DCompile` measured 2.5 ms for a small shader and
  14.5 ms for a 6-octave plasma (GTX 1080 machine). Compile lazily, draw with
  the CPU path until the program is ready, and never block a paint on the
  compiler.
- **No SDK headers at runtime:** do not use `d2d1effecthelpers.hlsli`; it ships
  only in the SDK, so the emitter writes the plain
  `float4 main(float4 pos : SV_POSITION, float4 scene : SCENE_POSITION) : SV_Target`
  form.

**Metal (macOS / iOS): only if the CPU proves insufficient on Apple.**

- CG draws into the CPU-side `drawRect:` context, so a GPU result must come
  back as a bitmap: MSL compiled at runtime with `newLibraryWithSource:`,
  rendered into a shared-storage texture (on unified memory that is
  effectively zero-copy), wrapped as a `CGImage`, and drawn.
- The cost is a GPU sync per re-shade. Apple CPUs plus NEON auto-vectorisation
  may well make this unnecessary at widget sizes; measure first.

**Linux: CPU only, permanently.** This is consistent with the Cairo design
decision.

### Backend ABI (vtable-append, every slot nullable)

```c
uint32_t (*shader_caps)(neui_render_ctx_t);        // bitmask, 0 = CPU only
void*    (*create_program)(const char* src, size_t len, uint64_t uid, int lang);
void     (*destroy_program)(void* prog);
bool     (*draw_program)(neui_render_ctx_t, void* prog,
                         float x, float y, float w, float h,
                         const float* constants, int count);  // false → CPU this frame
```

The backend only compiles emitted text and draws. It never sees `.nsl`.

### Fidelity contract

- **CPU:** bit-identical on every platform, enforced by CRC goldens.
- **GPU vs CPU:** within 2/255 per channel on ≥ 99.9% of pixels and within
  8/255 on all pixels, checked over the golden corpus at 1×, 1.5× and 2×.
- **Escape hatches:** a `#pragma neui cpu_only` per shader, and a session
  switch that forces the CPU everywhere. Use them for golden screenshots or
  when exact identity matters more than speed.

## 4. Public API

### `NEUI_API_SHADER` (optional interface, new `include/neui/d/shader.h`)

- **Shape:** vtable-shaped like `neui_filter_api_t`; future methods are
  appended.
- **When it is null:** `get_interface(sess, NEUI_API_SHADER)` returns
  `nullptr` on `NEUI_NO_SHADERS` builds (forced on for LVGL) and when the
  backend cannot draw bitmaps (null).
- **Never handed out inert:** this is the design-notes lesson from `EMBED` on
  iOS. A non-null interface must work.

```c
#define NEUI_API_SHADER "com.defiantnerd.neui.extension.shader/0"

typedef struct neui_shader_uniform {
  const char* name;
  float       v[4];            // scalar uses v[0]; vec2/3/4 use the leading slots
} neui_shader_uniform_t;

typedef struct neui_shader_draw {   // passed by pointer; append-only (like stroke_style)
  neui_asset_t                 shader;
  float                        x, y, w, h;      // logical px, painter space
  const neui_shader_uniform_t* uniforms;
  int                          uniform_count;
  float                        resolution;      // 0 or 1 = full; 0.5 = half-res + upscale
  uint32_t                     cache_tag;       // optional extra cache-key discriminator
} neui_shader_draw_t;

typedef struct neui_shader_api {
  uint32_t neui_version;
  // Compile. Returns asset_none on failure and writes "line:col: message"
  // into err (a separate error query could not work - there is no handle).
  neui_asset_t (NEUI_ABI *create)(neui_session_t, const char* src, size_t len,
                                  char* err, size_t err_cap);
  neui_asset_t (NEUI_ABI *create_from_file)(neui_session_t, const char* name,
                                            char* err, size_t err_cap);
  // Introspection - lets tools / skins list what a shader expects.
  int  (NEUI_ABI *uniform_count)(neui_session_t, neui_asset_t shader);
  bool (NEUI_ABI *uniform_info)(neui_session_t, neui_asset_t shader, int i,
                                char* name, size_t name_cap, int* type);
  // Bake to ordinary BITMAP assets (zero per-frame cost afterwards).
  neui_asset_t (NEUI_ABI *bake_bitmap)(neui_session_t, neui_asset_t shader,
                                       int w, int h, float scale,
                                       const neui_shader_uniform_t*, int count);
  neui_asset_t (NEUI_ABI *bake_filmstrip)(neui_session_t, neui_asset_t shader,
                                          int w, int h, float scale,
                                          const char* sweep_uniform, uint32_t frames,
                                          const neui_shader_uniform_t*, int count);
  // CUSTOMDRAW: draw inside a WIDGET_PAINT callback with the opaque painter.
  void (NEUI_ABI *draw)(neui_painter_t* p, const neui_shader_draw_t* d);
} neui_shader_api_t;
```

- Shader handles are ordinary `neui_asset_t` of kind SHADER and are destroyed
  with `assets->destroy`. **The asset vtable does not change.**
- `bake_filmstrip` sweeps `sweep_uniform` over 0..1 across `frames` cells and
  tags the result with the existing filmstrip frame layout.
- `draw` takes the opaque `neui_painter_t*`, so **the public painter vtable
  does not change**. The host resolves the asset through the painter's
  `host_token`. A painter exists only inside a paint callback (`WIDGET_PAINT`
  or `paint_surface`), so `draw` works in both and nowhere else.

### Compound layer `NEUI_COMPOUND_LAYER_SHADER = 8`

- **Geometry and common props:** anchors, offset, size, `z`, `alpha` and
  `show_when` come from the shared layer prologue; nothing new is needed.
- **New props:**
  - `shader` (asset), via `set_asset`. `apply_set_asset`
    (`hosts/shared/compound.h` ~753) learns the SHADER `"shader"` prop, and
    `bind_asset("shader", ...)` then works for free.
  - `resolution` (float).
  - `u.<name>` for each uniform.
- **How uniform props are routed:** the xpl thunks already pass any prop name
  through to `apply_set_*`, and `apply_bind` stores any prop name. So
  `bind(..., "u.value", NEUI_PARAM_VALUE, 1, 0)` needs **no new compound
  vtable slot**. `"u.value.step"` reuses the existing `.step` quantisation
  rule (`apply_set_float` ~665).
- **Vectors:**
  - `set_int` on a `vec3` / `vec4` unpacks ARGB;
  - `set_float` splats the value to every component;
  - `u.center.x` / `.y` address single components;
  - `set_string("u.v", "0.1,0.2")` follows the `stroke_dasharray` precedent.
- **Colours:** a `vec4` bound as a whole reads the attr as a **raw int** and
  unpacks it, never through float (see the colour-precision bug in §8).
- **Storage and lookup:**
  - Uniform statics live in a per-layer name → (int or float) map,
    interpreted against the program at paint time, because the shader may be
    attached after its uniforms are set, or swapped.
  - A layer `rev` counter (bumped in every `apply_*`) keys a cached
    uniform → binding/static resolution. Per-paint `std::string` lookups per
    uniform per widget are avoided.
- **Without the interface:** `paint_compound_layer` already ignores unknown
  kinds (`default: break;`), so a SHADER layer on a host without shader
  support is stored and invisible. An optional `fallback` prop names another
  layer (typically ASSET) that paints instead, so skins can ship a static
  image for LVGL / null. That layer is hidden whenever the shader layer
  renders.
  - **Imperatively:** `set_int(L, "fallback", other.id)`.
  - **In JSON:** a **new** optional layer `name` key (component layers have
    no name today).
- **Where the code lives:** the layer logic goes in a new
  `hosts/shared/shader_layer.h`, not in `widget_paint_compound.h`, which
  includes `qrcodegen.hpp` and has no Tier-1 coverage.

Client sketch:

```c
neui_shader_api_t* shaders =
  (neui_shader_api_t*) neui->get_interface(sess, NEUI_API_SHADER);
if (shaders) {
  char err[256];
  neui_asset_t glow = shaders->create(sess, ring_glow_src, strlen(ring_glow_src),
                                      err, sizeof err);
  if (glow.id != asset_none.id) {
    neui_compound_layer_t L = compound->add_layer(sess, knob, NEUI_COMPOUND_LAYER_SHADER, -1);
    compound->set_asset(sess, knob, L, "shader", glow);
    compound->set_int  (sess, knob, L, "u.accent", (int)0xFF3FA9F5);
    compound->bind     (sess, knob, L, "u.value", NEUI_PARAM_VALUE, 1.0f, 0.0f);
    compound->set_float(sess, knob, L, "u.value.step", 1.0f / 128.0f);
  } else {
    dbglog("shader: %s\n", err);   // e.g. "12:9: unknown identifier 'radius'"
  }
}
```

### Component JSON

```jsonc
{
  "component": "glow_knob",
  "size": [110, 110],
  "shaders": { "ring_glow": "shaders/ring_glow.nsl" },   // resolved via the resource client
  "params": [ { "key": "neui.param.value", "default": 0.5, "min": 0, "max": 1 } ],
  "layers": [
    { "kind": "shader", "z": -2, "anchor": ["center", "center"], "size": [104, 104],
      "shader": "ring_glow",
      "uniforms": { "accent": "#FF3FA9F5" },
      "bind": { "u.value": { "attr": "neui.param.value" } },
      "fallback": "disc" },                       // new: refers to a layer "name"
    { "kind": "rect", "name": "disc", "z": -1,      // new optional key "anchor": ["center", "center"],
      "size": [84, 84], "fill_color": "#FF2A2E38", "corner_radius": 42 }
  ]
}
```

- **Loader** (`hosts/shared/component_loader.h`):
  - the `shader` layer kind in `parse_layer_kind` / `layer_kind_token`;
  - a `shaders` map whose names resolve through
    `ResourceProvider::read_bytes(NEUI_RESOURCE_KIND_SHADER, ...)`, as
    COMPONENT bytes do;
  - `uniforms` → `u.*` statics;
  - a serialize round trip;
  - a shader asset owned by the component (released with it, like
    `comp_owned_assets`).
- **Without the interface:** the component still loads. The SHADER layer is
  inert, and `fallback` decides what shows instead.

### Resource kind

- `NEUI_RESOURCE_KIND_SHADER = 7` plus `NEUI_RESOURCE_MASK_SHADER` in
  `include/neui/d/resource.h`.
- Shader bytes are byte-native like COMPONENT: re-asked on every load, not
  image-cached.

### Internal painter seam

- The compound paint path needs to resolve a SHADER asset from the painter,
  so the internal `struct neui_painter` (`hosts/shared/painter.h`) gains a
  `resolve_shader(host_token, asset)` thunk.
- It has six construction sites: xpl ×2, win32 `widgets.cpp`, macOS
  `window.mm`, iOS `window.mm`, and `asset_store.h` `paint_surface`. That is
  about 10 lines per host.
- When the thunk is null, the layer paints nothing.
- The interface implementation itself (`create`, `bake_*`, `draw`) is about
  one table and a handful of thunks per host, following the `create_filter`
  precedent in each host's `widgets.cpp` / `.mm`.

### Support matrix

| Host / backend | `NEUI_API_SHADER` | Executor |
|---|---|---|
| xpl + D2D (win32) | yes | D2D GPU when compiled + FL ≥ 10_0, else CPU |
| native win32 + D2D | yes | same |
| xpl / native macOS + CG | yes | CPU (Metal later only if needed) |
| xpl / native iOS + CG | yes | CPU |
| xpl + Cairo (Linux) | yes | CPU |
| null backend | `nullptr` | - |
| LVGL (experimental) | `nullptr` (`NEUI_NO_SHADERS`) | - |
| wasm (planned, Canvas 2D) | yes, possible | CPU (`putImageData` path) |

## 5. Time and animation

- **Clock and registry:** a shared clock + animating-widget registry
  (`hosts/shared/anim_clock.h`), Tier-1 testable.
- **When a widget animates:** while it shows a compound whose SHADER layer
  reads `neui_time`, it registers. The host ticks the registry and
  invalidates registered widgets per tick, through the partial repaint path.
- **Heartbeat:**
  - **xpl:** generalise the toast heartbeat (`platform_start_toast_animation`
    / `platform_stop_toast_animation`, `hosts/crossplatform/platform.h`
    ~124) into a ref-counted `platform_start_animation` /
    `platform_stop_animation`. Every platform already has the timer (Win32
    `SetTimer` 16 ms, macOS `NSTimer`, iOS `CADisplayLink`, Linux `timerfd`
    armed only while animating).
  - **Native hosts:** a per-painted-widget timer, as GRID / SECTION bounce
    already do.
- **Rate:** default 30 fps, capped per session (`NEUI_ATTR_ANIMATION_FPS`, a
  new key with a `k_well_known_attrs` row).
- **Pausing:** stop while the window is hidden or minimised. Freeze time when
  the OS asks for reduced motion (iOS / macOS accessibility, Windows
  `SPI_GETCLIENTAREAANIMATION`).
- **Per-widget opt-out:** `NEUI_ATTR_ANIMATE = 0` (a new key) freezes one widget.
- **Embedded mode:** Linux runs on the existing 16 ms gate in
  `pump_and_tick`. Win32 and macOS ride the DAW's pump (`WM_TIMER` /
  `NSTimer`) as today.
- **Clock source:** a monotonic high-resolution clock. On Windows use
  `QueryPerformanceCounter`; the current `platform_now_ms` uses
  `GetTickCount64` (`platform_win32.cpp` ~2114), which ticks only about every
  15.6 ms. `neui_time` is wrapped to a period, so long sessions keep float
  precision.
- **Possible spin-off:** a public per-frame tick for client animations. It is
  not needed for shaders, and stays out of this plan.

## 6. Prerequisite: partial repaint on xpl

See `plans/xpl-partial-repaint.md`.

- **Why:** without it, every animation tick or automated parameter change
  re-rasterises and re-presents the whole window. That is already costly on
  Cairo and CG without shaders.
- **What it gates:** phase 6 (time-animated shaders). It is also recommended
  before param-driven shader layers see heavy DAW automation.
- **What it does not block:** phases 0-3 may start in parallel if the client
  is in a hurry.

## 7. Performance budget (CPU executor, single thread, measured in phase 0)

ns per pixel with the explicit SIMD kernels, 64-pixel lanes, from
`tests/shader_bench/`:

| CPU (compiler) | ring_glow, 27 ops | plasma, 35 ops / 6 sin | fbm5, 200 ops / 20 hash | synthetic, 176 ops / 15 transc. |
|---|---|---|---|---|
| Apple M1 Ultra (AppleClang) | 7 | 15 | 46 | 40 |
| Apple M1, CI VM (AppleClang) | 8 | 17 | 53 | 43 |
| AMD EPYC 9V45, CI VM (Clang 18 / GCC 13) | 5 / 7 | 17 / 20 | 67 / 72 | 41 / 49 |
| AMD EPYC 7763, CI VM (Clang 18 / MSVC / GCC 13) | 10 / 14 / 13 | 25 / 33 / 31 | 98 / 119 / 124 | 70 / 79 / 92 |
| Intel i7-6800K, 2016 (MSVC 19.42) | 16 | 36 | 131 | 90 |

What a re-shade costs (synthetic shader):

| Case | Cost |
|---|---|
| knob 120×120 logical @2× (57.6k px) | **2.2-5.4 ms** |
| background 800×600 @2× (1.92 Mpx) | **70-170 ms**; 18-42 ms at `resolution` 0.5 |
| one pixel at a time instead of lanes (rejected) | 10-20× slower |

The pre-phase-0 estimate of 40-80 ns/px for a 150-op shader holds. Older
x64 CPUs reach about 90.

What this means:

- **Fine:** dragging a single knob at 60 Hz. A preset load that re-shades 50
  knobs costs a one-off ~0.1-0.3 s, much less when values share cache
  entries.
- **Needs quantisation + content sharing:** DAW automation of many knobs.
- **Not viable on the CPU at 60 Hz:** animated full-window backgrounds. Use
  `resolution` < 1, bake a loop into a filmstrip, or rely on the D2D path
  (Windows only).
- **Guidance for skin authors:** size effects for the CPU worst case (Cairo,
  CG). The GPU path buys headroom on Windows, not a different look.

## 8. Pre-existing issues surfaced (fix independently, before phase 3)

- **QR layer leaks GPU uploads.** `QrSymbol::bitmaps` per-ctx uploads are
  destroyed only by `release_context` / `clear`
  (`compound_release_ctx_bitmaps`, `hosts/shared/compound.h` ~372). These
  paths free a symbol without destroying them:
  - FIFO eviction (`widget_paint_compound.h` ~393);
  - `compound_remove_layer`;
  - `compound_clear`;
  - `AssetStore::release_slot` (`asset_store.h` ~784).
- **Colour bindings lose precision.** A bound `color` goes
  `eval_binding_float` → float → `round_to_int` (`widget_paint_compound.h`
  ~461; `attr_as_float` in `hosts/shared/attrs.h`). A partly transparent ARGB
  value (alpha `0x01`-`0xFE`) has a magnitude above 2^24 as an int32, so its
  low blue / green bits are rounded away. Opaque `0xFF…` values survive
  because they wrap to a small negative int.
- **Stale D2D documentation.** `docs/rendering-and-assets.md`, the comment in
  `backends/d2d/d2d_backend.h` and `plans/winui3-host.md` (~185-210) still
  describe a WIC / `ID2D1HwndRenderTarget` backend. The code has been D2D 1.1
  on D3D11 + DXGI flip swap chains for a while.

## 9. Phases

| # | Scope | Main files |
|---|---|---|
| 0 | **Done.** Language spec + edge-case table; executor micro-benchmark (MSVC, AppleClang arm64, GCC, Clang); D2D zero-input effect spike (`SCENE_POSITION` at 100/120/150/200% vs CPU) | `docs/shader-language.md`, `tests/shader_bench/`, branch `spike/d2d-shader-effect` |
| 1 | **Done.** Front end + IR + optimiser; deterministic built-ins; SoA CPU executor; output packer (premultiply, optional dither) | `src/shader/{nsl.h,nsl_ir.h,nsl_math.h,nsl_compile.cpp,nsl_exec.cpp,CMakeLists.txt}`, `tests/test_shader_{lang,exec}.cpp` |
| 2 | `NEUI_API_SHADER` (`create`, `create_from_file`, introspection, **`bake_bitmap` / `bake_filmstrip`**); asset kind + resource kind; shader cache on `AssetEntry`; QR leak fix. **First visible win:** baked shader filmstrips via the existing ASSET layer + `frame` binding, on every supporting host, zero per-frame cost | `include/neui/d/{shader.h,assets.h,resource.h}`, `hosts/shared/{asset_store.h,resource_provider.h}`, the 4 hosts' `host.*` (`get_interface`) + `widgets.*` (thunk tables) |
| 3 | Live SHADER compound layer (`u.*`, `resolution`, `fallback`); `shader_api->draw`; painter `resolve_shader` seam; component JSON + serialize | `include/neui/d/compound.h`, `hosts/shared/{compound.h,shader_layer.h,widget_paint_compound.h,painter.h,component_loader.h}`, 6 painter construction sites |
| 4 | D2D GPU path: HLSL emitter, runtime `D3DCompile`, custom effect, tolerance tests | `hosts/shared/shader_emit_hlsl.h`, `include/neui/d/renderer.h`, `backends/d2d/{d2d_backend.cpp,d2d_shader_effect.cpp}` |
| 5 | **Partial repaint** - separate track, scheduled **first** (`plans/xpl-partial-repaint.md`) | see that plan |
| 6 | Time + animation: clock, registry, generalised heartbeat, fps cap, pause / reduce-motion (**requires 5**) | `hosts/shared/anim_clock.h`, `hosts/crossplatform/{platform.h,platform_*}`, native hosts' timers |
| 7 | Only if measured necessary: Metal executor on Apple; opt-in CPU fork-join pool | `hosts/shared/shader_emit_msl.h`, `backends/cg/cg_backend.mm`, `src/shader/nsl_exec.cpp` |
| 8 | Research: post-processing of existing UI. Bake-time `NEUI_FE_SHADER` with `sample(in, uv)` over a SURFACE fits the CPU filter graph directly. Live scene post-processing needs the rendered region as input: cheap on Cairo (the window surface is CPU memory) and feasible on D2D (copy the target region into an effect input), but CG's `drawRect:` context is not readable, so Apple would need an offscreen-then-blit frame | `hosts/shared/filter_graph.h`, `include/neui/d/filter.h` |

## 10. Tier-1 test strategy

All of the following run host-less, everywhere the suite runs, including the
null platform:

- **`tests/test_shader_lang.cpp`:**
  - a positive / negative corpus with exact `line:col` diagnostics;
  - every limit (depth, op count, loop product, source size);
  - deterministic mutation fuzzing of the corpus, which must never crash or
    hang.
- **`tests/test_shader_exec.cpp`:**
  - each built-in against a double-precision reference;
  - the edge-case table;
  - **CRC32 goldens** of the built-in library at 64×64 @1× and @2×, checked
    on Windows, macOS, Linux and the iOS simulator. This is how "same look"
    is enforced;
  - control-flow programs against hand-written C++ equivalents (with a
    tolerance, since the C++ side uses libm). This replaced the planned
    tree-walking reference interpreter: it catches the same lowering bugs
    for much less code.
- **`tests/test_shader_cache.cpp`:** uses a fake backend counting
  create/destroy (in the style of `test_asset_font.cpp`). Covers:
  - content sharing across AttrBags;
  - LRU and the byte budget;
  - eviction destroying every per-ctx upload;
  - re-upload on a generation bump;
  - the `release_context` / `release_slot` / `clear` paths;
  - a QR-leak regression.
- **`tests/test_shader_emit.cpp`:** HLSL (and later MSL) golden text.
- **Extensions:** `tests/test_compound.cpp` (`u.*` routing, `.step`
  interplay, raw-int colour unpack) and `tests/test_component_loader.cpp`
  (shader layer JSON round trip, with the source served by a fake resource
  provider).
- **Windows-only smoke target:** an offscreen D2D ctx + `read_pixels_bgra`,
  comparing GPU against the CPU reference within the tolerance contract.

## 11. Example and built-in shader library

`examples/shader_example.cpp` (target `neui_shader_example`), with
`examples/components/glow_knob.json` and
`examples/shaders/{ring_glow,brushed_metal,plasma,vu,grain}.nsl`. The library
doubles as the golden-test corpus and gives skin authors working starting
points. The example shows:

- a brushed-metal background **baked once** (`bake_bitmap`);
- 8 `glow_knob` components with `u.value` bound to `neui.param.value` and
  `u.value.step`;
- a theme accent colour passed as an int ARGB `vec4`;
- a VU meter driven by a client-written attr (simulated automation);
- a CUSTOMDRAW panel drawn with `shader_api->draw`;
- a `NEUI_API_SHADER == nullptr` path that falls back to plain knobs, like
  `filter_knob_example`;
- a deliberately broken shader whose compile error appears in a LABEL;
- from phase 6: an animated plasma with an fps cap and a pause toggle.

## 12. Risks and open questions

- **Which use case does the client have?** That decides the order of
  phases 2-3 (bake + param-driven) against 5-6 (partial repaint + animation).
  Ask before phase 1 ends.
- **`SCENE_POSITION` under DPI scaling:** settled by the phase-0 spike
  (device px; draw in device space at the floored origin, see "GPU
  executors").
- **Effects that look good on the GPU but cost too much on the CPU.** "Same
  look" is guaranteed; "same frame rate" is not. Tier the guidance, and
  provide `resolution` and bake-to-filmstrip as the levers.
- **Re-shade storms under DAW automation.** Mitigated by quantisation +
  content-keyed sharing + partial repaint, and measured with the example.
- **GPU in DAWs.** Mitigated: D2D reuses the device neui already owns, Linux
  has no GPU path, and Apple's Metal path is optional and late.
- **Language scope creep.** Every addition (matrices, arrays, textures,
  derivatives) needs a CPU-executor answer first. The CPU reference is the
  spec.
- **`neui_time` precision** over long sessions. It is wrapped; document the
  period so loop-based effects can choose divisors of it.

## 13. Phase 0 results (2026-10-10)

**Deliverables:**
- **Spec:** `docs/shader-language.md`.
- **CPU executor prototype + benchmark:** `tests/shader_bench/`, ctest
  `neui_shader_bench_check`. It has a hand-built IR (no front end), 4 test
  programs, scalar / auto-vectorised / explicit-SIMD kernels, and the spec's
  pixel grid, coverage and dithering.
- **D2D spike:** branch `spike/d2d-shader-effect` (f1774d2), standalone
  under `spikes/d2d_shader_effect/`.

**Determinism: confirmed.**
- The bits were identical on AppleClang arm64, MSVC x64 (two machines),
  GCC 13 x64 and Clang 18 x64. The test covered:
  - sweeps of every approximated built-in, including denormals, ±0, ±inf
    and NaN;
  - 4 programs at 1×, 1.5×, 2× and a fractional, dithered origin;
  - scalar vs auto-vectorised vs SIMD runs.
- **FMA contraction changes bits:** the built-in sweeps differ under clang
  `-ffp-contract=fast`. Hence the one-TU rule in §3.
- **A host's flush-to-zero changes bits** unless the executor pins the FP
  environment (`log2` of a denormal). Hence the per-draw guard in §3.

**Performance:** §7. The estimate holds.
- Auto-vectorisation was the weak point: GCC 2.2× and MSVC 1.8× slower than
  their explicit-SIMD runs on the same CPU. Clang is fast either way.
- With explicit SIMD, the three x64 compilers are within 1.3× of each other
  on one CPU.
- Force-inlining made no measurable difference on MSVC.

**D2D:**
- Approach B (device-space draw at the floored origin) matches the CPU
  within 1/255. See "Coordinates" in §3.
- `D3DCompile` takes 2.5-14.5 ms per program.
- A 240×240 effect draw costs about 83 µs per frame, which is D2D's
  per-frame overhead. Shader cost does not show at that size.

**Carried into phase 1:**
- Explicit SIMD kernels with 64-pixel lanes. Lanes of 256 were 10-20% faster
  on x64 and no faster on Apple, so measure again with the real IR.
- Vectorise the output packer: about 2 ns/px of fixed cost, which is visible
  on tiny shaders.
- The checksums move into `neui_tests` as goldens, and `tests/shader_bench/`
  goes away.
- Optional: inspect MSVC kernel codegen (`/FAs`, SSE2-only floor and convert
  sequences).
- The throwaway branch `spike/shader-bench-ci` (benchmark report on the CI
  compilers) can be deleted.

## 14. Phase 1 results (2026-10-10)

**What exists:** the `neui-shader` static library in `src/shader/`, linked
PUBLIC by `neui`, and its internal API `src/shader/nsl.h`:
- `compile()` gives a program or a `<name>:<line>:<col>: error: <message>`
  diagnostic (first error only);
- uniform introspection and defaults, flags (`READS_TIME`, `DITHER`,
  `CPU_ONLY`) and stats;
- `placement()` and `execute()`, which give premultiplied BGRA8 at the
  spec's pixel grid;
- test hooks `execute_scalar()` and `evaluate()`.

**How it compiles** (`nsl_compile.cpp`): lexer, one-pass parser + type
checker, then lowering:
- every call is inlined and every loop unrolled;
- control flow is predicated: `if` merges changed variables with selects,
  while `return` / `break` / `continue` clear alive masks;
- constants fold through the executor's own math; CSE and exact
  simplifications apply;
- dead code is removed, the program is split into a draw part (uniform-only
  work, scalar, once per draw) and a pixel part, and registers are allocated
  by linear scan.

Constants and draw values reach the pixel program through a pool and an
`OP_LOAD` right before their first use, so an unrolled loop full of distinct
constants does not exhaust the 256-register limit.

**Checks:** 26 test cases.
- The four phase-0 programs compiled from `.nsl` reproduce the phase-0
  checksums bit for bit.
- So do the built-in sweeps, with one deliberate change: `atan` now gives NaN
  for a NaN argument (the spec says so), where phase 0 returned 0.
- SIMD equals scalar to the bit, and a host's flush-to-zero changes nothing.
- The tests also cover:
  - the edge-case table;
  - the pixel grid, coverage and stride;
  - uniforms, `neui_time` and stats;
  - control flow against C++;
  - diagnostics with exact positions, every limit, literal rounding under a
    decimal-comma locale;
  - mutation fuzzing (about 3000 mutants).

**Speed** (`neui_shader_bench`, M1 Ultra, AppleClang):

| Shader | ns/px | knob 120×120 @2× |
|---|---|---|
| ring_glow (29 ops) | 6.2 | 0.36 ms |
| plasma (35 ops, 6 sin) | 14.3 | 0.83 ms |
| fbm5 (191 ops) | 44 | 2.5 ms |
| metal_knob (128 ops) | 24 | 1.4 ms |

That is the same as the phase-0 prototype.

**Next (phase 2):** `NEUI_API_SHADER` (`create`, `create_from_file`,
introspection, `bake_bitmap` / `bake_filmstrip`), the SHADER asset kind and
resource kind, the shader cache on `AssetEntry`, and the QR-leak fix.

