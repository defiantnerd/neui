# Plan: shader effects - a neui shading language

## Status

**Phases 0-4, 6 (2026-10-10) and 9 (2026-10-11) done.** Phase 7 (Metal, a
CPU thread pool) only if measured necessary; phase 8 is research, and now
has the sampling it needs. The prerequisite track,
`plans/xpl-partial-repaint.md`, is implemented (P1-P4).

Phase 9, texture sampling (2026-10-11), is done: a `sampler2D`
uniform bound to a BITMAP or SURFACE asset, a bit-exact sample built-in on
the CPU, and D2D resource textures on the GPU. Both spikes are done and
carried most of steps 9a and 9c: the language, the CPU executor and the
D2D draw path work on every platform, within 1/255 of the CPU on the GPU.
Step 9b binds bitmaps in every host (draw / bake `textures`, a SHADER
layer's `u.<name>`, component JSON `"textures"`, content-id cache keys);
9c and 9d finished the D2D texture cache, the spec (§6.1, §9.4), the
guide (§5.5, §7.9) and a sampling row in `neui_shader_example`. It is also what phase 8 needs. See "Phase 9 design" and "Phase 9.0
results".

Phase 6 produced animation: a session clock (`hosts/shared/anim_clock.h`)
that a SHADER layer reading `neui_time` runs on by itself, and that
`shader_api->animation_time` exposes to `draw` and any CUSTOMDRAW; an fps
cap, a pause, the OS reduced-motion setting, and a timer per frame (xpl,
win32) or per session (macOS, iOS) that repaints only the widgets that
animate. See "Phase 6 results".

Phase 4 produced the D2D GPU path: the library emits each program's pixel
part as HLSL (`src/shader/nsl_gpu.cpp`), the D2D backend compiles it in the
background and draws it as a custom effect (`backends/d2d/d2d_shader.cpp`),
and live layers and `draw` use it on the device grid; within 1/255 of the CPU
on a GTX 1080 and on WARP. See "Phase 4 results".

Phase 3 produced the live paths: the compound SHADER layer
(`NEUI_COMPOUND_LAYER_SHADER`, `u.*` uniform props, `resolution`, `time`,
`fallback`), `shader_api->draw` for CUSTOMDRAW, the session image cache, the
painter `resolve_shader` seam, a `get_device_transform` backend slot, and
shader layers in component JSON (`hosts/shared/shader_layer.h`, plus
additions to `shader_asset.h`, `compound.h`, `component_loader.h`). See
"Phase 3 results".

Phase 2 produced the client interface: `NEUI_API_SHADER`
(`include/neui/d/shader.h`) with the SHADER asset kind, compile + uniform
introspection and the bake API, implemented once for all four hosts
(`hosts/shared/shader_api.h`, `shader_asset.h`), plus `neui_shader_example`.
See "Phase 2 results".

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
(possibly `mat2` later), textures and sampling (designed as phase 9, §19), and
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

Implemented in phase 3 (`ShaderCache`, `hosts/shared/shader_asset.h`). A bake
creates its own BITMAP asset and needs no cache; the cache serves the live
layer and `draw`.

- **Where it lives:** one per session, on the asset store
  (`AssetStore::shader_cache()`), keyed by the program's `uid`, **not** on the
  layer. (The design had one cache per shader asset; a store-wide one gives a
  single LRU and budget across all shaders for the same lookup cost, and a
  SHADER asset's release drops its entries by `uid`.)
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
  **Not done in phase 3**: a resize re-shades every frame. Revisit if live
  resizing of large shader layers proves slow.

### GPU executors

**D2D (Windows): the only planned GPU path.**

- **Program creation:**
  - HLSL is emitted from the IR in the library (`src/shader/nsl_gpu.cpp`,
    which can see the IR), so its output is Tier-1 golden-testable on every
    platform.
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
    `PrepareForRender`, delivered through one blob property;
  - one effect instance per program per device context (its pixel shader is
    fixed at creation), reused like `tint_effect` and released with the
    target objects.
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

As implemented in phase 4 (`include/neui/d/renderer.h`):

```c
void* (*create_shader_program)(int lang, const char* src, size_t len);  // NULL = CPU
void  (*destroy_shader_program)(void* program);
bool  (*draw_shader_program)(neui_render_ctx_t, void* program,
                             int32_t x, int32_t y, uint32_t w, uint32_t h,  // device px
                             const float* constants, uint32_t count);       // false -> CPU
#define NEUI_SHADER_LANG_HLSL_D2D 1
#define NEUI_SHADER_OPACITY_INDEX 6   // the backend multiplies its alpha in
```

The backend only compiles emitted text and draws. It never sees `.nsl`. No
`shader_caps`: a null `create_shader_program` result says the same, per
shader. Programs are process-wide; per-context state (D2D's effect
instances) is the backend's business. The draw position is in device pixels,
because the GPU path draws only on the device grid (phase 0, approach B).

### Fidelity contract

- **CPU:** bit-identical on every platform, enforced by CRC goldens.
- **GPU vs CPU:** within 2/255 per channel on ≥ 99.9% of pixels and within
  8/255 on all pixels, checked over the golden corpus at 1×, 1.5× and 2×.
  Exception (found in phase 4): pixels on a discontinuity of the shader
  (`step`, a comparison) can flip to the other side's colour, since a
  last-bit difference moves the edge.
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

Implemented in phase 2 (`include/neui/d/shader.h`; the header is the
reference, this is its shape):

```c
#define NEUI_API_SHADER "com.defiantnerd.neui.extension.shader/0"

typedef struct neui_shader_uniform { const char* name; float v[4]; } neui_shader_uniform_t;
neui_shader_uniform_t neui_shader_color(const char* name, uint32_t argb);  // static inline

typedef struct neui_shader_uniform_info {
  const char* name; uint32_t components; float def[4];
} neui_shader_uniform_info_t;

typedef struct neui_shader_bake {    // passed by pointer; fields may be appended
  float width, height;               // logical px (one image, or one frame)
  float scale;                       // device px per logical px; <= 0 means 1
  float time;                        // neui_time; wrapped to [0, 3600)
  const neui_shader_uniform_t* uniforms;  // by name; others keep their defaults
  uint32_t uniform_count;
} neui_shader_bake_t;

typedef struct neui_shader_api {
  uint32_t neui_version;
  neui_asset_t (*create)(neui_session_t, const char* src, uint32_t len, const char* name,
                         char* err, uint32_t err_cap);
  neui_asset_t (*create_from_file)(neui_session_t, const char* path, char* err, uint32_t err_cap);
  uint32_t     (*uniform_count)(neui_session_t, neui_asset_t shader);
  bool         (*uniform_at)(neui_session_t, neui_asset_t shader, uint32_t index,
                             neui_shader_uniform_info_t* out);
  neui_asset_t (*bake_bitmap)(neui_session_t, neui_asset_t shader, const neui_shader_bake_t*);
  neui_asset_t (*bake_filmstrip)(neui_session_t, neui_asset_t shader, const neui_shader_bake_t*,
                                 const char* sweep, float from, float to, uint32_t frames);
  // phase 3: draw a shader from a paint callback
  bool (*draw)(neui_painter_t* p, const neui_shader_draw_t* d);
} neui_shader_api_t;
```

- Shader handles are ordinary `neui_asset_t` of kind SHADER and are destroyed
  with `assets->destroy`. **The asset vtable does not change.**
- `create` reports the first error as `"<name>:<line>:<col>: error: ..."`
  into a caller buffer: a failed create has no handle to query.
- `bake_filmstrip` sets frame i's `sweep` (a float uniform, or `"neui_time"`
  to bake an animation) to `from + (to - from) * i / (frames - 1)`, matching
  a `"frame"` binding with scale `frames - 1`. **neui picks the cell grid**:
  the column count that keeps the longer bitmap side shortest. A
  single-column strip of 128 frames at 240 px would be 30720 px tall, beyond
  every GPU's texture limit; frames are drawn by index, so the layout is
  invisible to clients. `get_frame_count` returns `frames` even when the grid
  has spare cells.
- Bakes use the scale the client passes. Clients have no display-scale query,
  and the xpl host only knows a scale once a window exists, so `scale <= 0`
  means 1, not "the display's". The example bakes at 2.
- **Phase 3 appended** `draw(neui_painter_t*, const neui_shader_draw_t*)` for
  CUSTOMDRAW, with `neui_shader_draw_t` carrying the asset, rect, uniforms,
  `resolution` and `time` (no cache tag: content keying made it unneeded). It
  takes the opaque `neui_painter_t*`, so **the public painter vtable does not
  change**. The host resolves the asset through the painter's
  `resolve_shader` thunk. A painter exists only inside a paint callback
  (`WIDGET_PAINT` or `paint_surface`), so `draw` works in both and nowhere
  else.

### Compound layer `NEUI_COMPOUND_LAYER_SHADER = 8`

Implemented in phase 3 as designed below, with these differences:

- **No `rev` counter.** Paint resolves the layer's statics and bindings
  against the program directly (four passes over the layer's small maps, no
  per-uniform string building). Cheap next to a cache lookup.
- **`"time"` prop** (bindable float seconds) so a client can animate before
  phase 6 adds a clock.
- **`fallback` also covers a missing shader**: the fallback layer paints when
  the host has no shader support *or* the layer has no valid shader asset
  (e.g. a component shader that failed to compile).
- **The `"name"` prop** is generic: any layer can carry one.

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

Implemented in phase 3: `resolve_shader_thunk_t` returns the `ShaderAsset`
and the session's `ShaderCache`; every host sets it to
`ShaderApi<Host>::painter_resolve` (null under `NEUI_NO_SHADERS`), and
`AssetStore::paint_surface` takes it as a trailing parameter. Placement also
needed a backend slot the design missed: **`get_device_transform`**
(appended to `neui_render_backend_t`, nullable), the current user -> device
mapping, because the painter cannot otherwise know the layer's device origin
under the host's and client's transforms (see the spec's pixel grid).

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

Implemented in phase 6; "Phase 6 results" lists where it differs from this
design (no new attrs, a timer of its own rather than a generalised toast
heartbeat, no visibility tracking).

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

- **QR layer leaks GPU uploads. Fixed in phase 2:** FIFO eviction,
  `compound_remove_layer` (which now takes the backend), `compound_clear` and
  `AssetStore::release_slot` destroy a symbol's uploads in every ctx;
  regression tests in `tests/test_compound.cpp` and
  `tests/test_asset_filmstrip.cpp`. The finding was: `QrSymbol::bitmaps`
  per-ctx uploads were destroyed only by `release_context` / `clear`
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
- **Stale D2D documentation. Fixed in phase 4** for
  `docs/rendering-and-assets.md` and the comment in
  `backends/d2d/d2d_backend.h`. `plans/winui3-host.md` (~185-210) still
  describes a WIC / `ID2D1HwndRenderTarget` backend; it is a deferred
  analysis and is left as written. The code has been D2D 1.1 on D3D11 + DXGI
  flip swap chains for a while.

## 9. Phases

| # | Scope | Main files |
|---|---|---|
| 0 | **Done.** Language spec + edge-case table; executor micro-benchmark (MSVC, AppleClang arm64, GCC, Clang); D2D zero-input effect spike (`SCENE_POSITION` at 100/120/150/200% vs CPU) | `docs/shader-language.md`, `tests/shader_bench/`, branch `spike/d2d-shader-effect` |
| 1 | **Done.** Front end + IR + optimiser; deterministic built-ins; SoA CPU executor; output packer (premultiply, optional dither) | `src/shader/{nsl.h,nsl_ir.h,nsl_math.h,nsl_compile.cpp,nsl_exec.cpp,CMakeLists.txt}`, `tests/test_shader_{lang,exec}.cpp` |
| 2 | **Done.** `NEUI_API_SHADER` (`create`, `create_from_file`, introspection, **`bake_bitmap` / `bake_filmstrip`**); asset kind + resource kind; `ShaderAsset` on `AssetEntry`; QR leak fix. **First visible win:** baked shader filmstrips via the existing ASSET layer + `frame` binding, on every supporting host, zero per-frame cost | `include/neui/d/{shader.h,assets.h,resource.h}`, `hosts/shared/{shader_api.h,shader_asset.h,asset_store.h}`, the 4 hosts' `host.*` (`get_interface`) + `widgets.*` (a 10-line host policy each), `examples/shader_example.cpp` |
| 3 | **Done.** Live SHADER compound layer (`u.*`, `resolution`, `time`, `fallback`); `shader_api->draw`; the session image cache; painter `resolve_shader` seam; `get_device_transform` backend slot; component JSON + serialize | `include/neui/d/{compound.h,shader.h,renderer.h,component.h}`, `hosts/shared/{compound.h,shader_layer.h,shader_asset.h,shader_api.h,widget_paint_compound.h,painter.h,component_loader.h,asset_store.h}`, the backends' device transform, 9 painter sites in the 4 hosts |
| 4 | **Done.** D2D GPU path: HLSL emitter, runtime `D3DCompile` on a worker thread, custom effect, `set_gpu`, tolerance smoke test; canonical IR numbering | `src/shader/{nsl_gpu.cpp,nsl_compile.cpp,nsl.h}`, `include/neui/d/{renderer.h,shader.h}`, `hosts/shared/{shader_asset.h,shader_layer.h,shader_api.h}`, `backends/d2d/{d2d_backend.cpp,d2d_shader.cpp}`, `tests/{test_shader_gpu.cpp,shader_gpu_smoke.cpp}` |
| 5 | **Partial repaint** - separate track, scheduled **first** (`plans/xpl-partial-repaint.md`) | see that plan |
| 6 | **Done.** Time + animation: session clock + animating-widget registry, per-frame / per-session timers, fps cap, pause, OS reduced motion, clock images dropped per frame | `hosts/shared/{anim_clock.h,shader_asset.h,shader_layer.h,shader_api.h}`, `include/neui/d/shader.h`, `hosts/crossplatform/{platform.h,platform_*,host.cpp}`, the native hosts' CUSTOMDRAW paint + timers, `tests/test_anim_clock.cpp` |
| 7 | Only if measured necessary: Metal executor on Apple; opt-in CPU fork-join pool | `hosts/shared/shader_emit_msl.h`, `backends/cg/cg_backend.mm`, `src/shader/nsl_exec.cpp` |
| 8 | Research: post-processing of existing UI. Bake-time `NEUI_FE_SHADER` with `sample(in, uv)` over a SURFACE fits the CPU filter graph directly. Live scene post-processing needs the rendered region as input: cheap on Cairo (the window surface is CPU memory) and feasible on D2D (copy the target region into an effect input), but CG's `drawRect:` context is not readable, so Apple would need an offscreen-then-blit frame. Needs phase 9's sampling first | `hosts/shared/filter_graph.h`, `include/neui/d/filter.h` |
| 9 | **Done (§19, §20).** Texture sampling: `uniform sampler2D` + `texture2D` bound to BITMAP / SURFACE assets; bit-exact fixed-point bilinear on the CPU; asset content ids in the image cache key; `u.<sampler>` layer props + JSON `"textures"`; D2D resource textures | `src/shader/{nsl.h,nsl_ir.h,nsl_compile.cpp,nsl_exec.cpp,nsl_gpu.cpp}`, `include/neui/d/{shader.h,renderer.h}`, `hosts/shared/{asset_store.h,shader_asset.h,shader_layer.h,shader_api.h,compound.h,component_loader.h}`, `backends/d2d/d2d_shader.cpp`, `tests/test_shader_*.cpp` |

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
- **`tests/test_shader_asset.cpp`** (phase 2): the interface over a real
  `AssetStore` and a fake host policy: compile + diagnostics, introspection,
  bake == executor bytes, time wrap, bad input, the strip grid, the resource
  path, and a filmstrip golden.
- **`tests/test_shader_layer.cpp`** (phase 3): a fake backend recording
  uploads, draws and the device transform. Covers:
  - `u.*` prop routing and uniform resolution (statics by kind, colours,
    bindings, components, steps, order);
  - placement against the executor: 1:1 at scale 1, whole device px under
    150% + translation, a fractional origin, `resolution`, rotation;
  - content sharing across contexts and positions, time keying;
  - LRU and the byte budget, eviction destroying uploads;
  - re-upload on a generation bump;
  - the `release_context` / `release_slot` / `clear` paths;
  - fallback layers through the real compound paint walk, `draw`, the
    resolve thunk's session checks, and the component shader hook.
- **`tests/test_shader_gpu.cpp`** (phase 4): HLSL golden text, the
  constant buffer, and the host's GPU / CPU choice with a fake backend.
- **Extensions:** `tests/test_component_loader.cpp` (shader layer JSON load,
  without the hook, and the serialize round trip). The `u.*` routing tests
  went into `test_shader_layer.cpp` rather than `test_compound.cpp`.
- **Windows-only smoke target** (`neui_shader_gpu_smoke`, phase 4,
  ctest-registered): an offscreen D2D ctx + `read_pixels_bgra`, comparing GPU
  against the CPU reference within the tolerance contract, plus two draws per
  frame, a pushed clip and the alpha stack. Runs on WARP in CI.

## 11. Example and built-in shader library

`examples/shader_example.cpp` (target `neui_shader_example`). As built
(phase 3), the shaders and the component are inline in the source rather than
separate `.nsl` / `.json` files; the list below is the original target. It
shows baked banner, knobs and tiles; three live knobs (SHADER layer, `u.value`
bound with a step, `u.accent` bound to an int attr per knob, a fallback disc);
a knob from a JSON component with an inline shader; a CUSTOMDRAW panel drawn
with `draw` at `resolution` 0.5 whose `t` follows the first live knob; the
broken-shader diagnostic; and the plain-KNOB fallback. The original plan:

- a brushed-metal background **baked once** (`bake_bitmap`);
- 8 `glow_knob` components with `u.value` bound to `neui.param.value` and
  `u.value.step`;
- a theme accent colour passed as an int ARGB `vec4`;
- a VU meter driven by a client-written attr (simulated automation);
- a CUSTOMDRAW panel drawn with `shader_api->draw`;
- a `NEUI_API_SHADER == nullptr` path that falls back to plain knobs, like
  `filter_knob_example`;
- a deliberately broken shader whose compile error appears in a LABEL;
- from phase 6: an animated plasma with an fps cap and a pause toggle (done:
  the "Animated on the session clock" row, a self-animating SHADER layer
  next to the same shader drawn with `draw` + `animation_time` at half
  resolution, with a paints/s readout).

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

Constants and draw values reach the pixel program through a pool and
`OP_LOAD`:
- by default, a prologue that runs once per draw loads all of them into
  pinned registers;
- when pinning would exceed the 256 live-value limit, as an unrolled loop
  full of distinct constants can, each value is loaded right before its first
  use and freed after its last;
- the load-on-demand allocation also decides the limit;
- `NEUI_DEBUG_SHADER_PIN=0/1` forces either policy for measurement. Pinning
  was 5-12% faster on MSVC and within noise on the M1.

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

**Speed** (`neui_shader_bench`, SIMD ns/px; `flat` has no per-pixel work and
measures the fixed cost of the block loop and output packing):

| Shader | M1 Ultra, AppleClang | i7-6800K (2016), MSVC | knob 120×120 @2× |
|---|---|---|---|
| flat | 0.9 | 4.0 | 0.05-0.23 ms |
| ring_glow (29 ops) | 6.2 | 13.3 | 0.36-0.77 ms |
| plasma (35 ops, 6 sin) | 14.4 | 35-36 | 0.83-2.1 ms |
| fbm5 (191 ops) | 44-49 | 134 | 2.5-7.7 ms |
| metal_knob (128 ops, realistic) | 25 | 62-66 | 1.4-3.8 ms |

These are at or better than the phase-0 prototype.

**An MSVC pitfall found on the way:**
- **Symptom:** the first version of the output packer cost MSVC a fixed
  16 ns per pixel, making small shaders nearly 2× slower than the prototype.
  The Windows machine found it with `/FAs`.
- **Cause:** when the `F4` / `I4` wrapper structs are nested several levels
  deep, MSVC keeps every temporary on the stack. That came to 83 instructions
  per pixel, mostly stack stores.
- **Fix:** the x86 packer now uses raw `__m128` and does the same operations
  in the same order, so the bits are unchanged. That gives 23 instructions
  per pixel with zero stack traffic.
- **Rule for future hot code on x86:** keep wrapper expressions shallow (as
  the op kernels are), or use raw intrinsics.
- **What didn't help:** force-inlining the kernel switch into the block loop
  made MSVC 2-3× slower. 256-pixel lanes gave 5-12%, which phase 2+ can
  revisit.

## 15. Phase 2 results (2026-10-10)

**What exists:**
- **The interface:** `NEUI_API_SHADER` in `include/neui/d/shader.h` (shape
  in §4), with `NEUI_ASSET_KIND_SHADER = 10` and
  `NEUI_RESOURCE_KIND_SHADER = 7`.
- **One implementation for all four hosts:**
  - `hosts/shared/shader_api.h` is a template over a host policy that only
    finds a session's asset store. Each host adds about 10 lines and a
    `get_interface` entry.
  - `hosts/shared/shader_asset.h` holds `ShaderAsset` (on `AssetEntry`) and
    the bake.
- **Where it is off:**
  - `-DNEUI_NO_SHADERS=ON` (a CMake option) drops the interface;
  - the LVGL host and the null platform never hand it out.
- **The example:** `neui_shader_example`. It bakes:
  - a brushed-metal banner;
  - three 128-frame glow-knob strips, driven by a compound ASSET layer's
    `frame` binding and a drag behavior;
  - three tiles;
  - and it shows a broken shader's diagnostic in a label.

  It also has a plain-KNOB fallback for hosts without the interface.
  - Verified on macOS on both the native and xpl hosts, which give identical
    screenshots, with a synthesized drag. The `NEUI_NO_SHADERS` fallback was
    checked too.
  - It bakes 388 images (12.3 Mpx at 2×) in about 0.5 s on the M1 Ultra.
    The knob shader costs about 40 ns/px: it is heavier than ring_glow.
- **The QR upload leak (§8) is fixed.**
- **New library helpers:** `nsl::wrap_time` (the spec's time wrap) and
  `nsl::sweep_value` (exact sweep ends). They are in the library so no
  compiler contracts them.

**Decisions taken while building it:**
- **The bake parameters are a struct** passed by pointer (size, scale, time,
  uniforms), so more can be appended.
- **neui chooses the filmstrip grid,** to stay within GPU texture limits (§4).
- **The bitmap cache moved to phase 3,** where the live layer and `draw` use
  it. A bake makes its own BITMAP.
- **Bakes take an explicit scale.** A client cannot query the display scale,
  and the xpl host knows one only after a window exists.
  - **Open:** add a display-scale query (`NEUI_API_METRICS`) if clients need
    one before showing a window.
- **Unknown uniform names are ignored** by the bake, as the compound layer
  will ignore `u.*` props a swapped-in shader does not declare. A sweep
  that names an unknown or non-float uniform fails instead, because it
  would bake N identical frames.

**Compiler review (an independent review of `nsl_compile.cpp` at 03ae00b),
findings fixed:**
- **A use-after-free in `return` with a call in its value.** The statement
  held a reference into the call-frame vector, which inlining the call
  reallocated. It crashed the example, and the `polar` test had been passing
  by luck.
  - **Fix:** frames live in a `std::deque`.
  - **Regression:** a flow case.
  - **Check:** the whole suite now runs clean under ASan + UBSan, apart from
    a pre-existing out-of-range enum in `test_metrics.cpp`.
- **Exponential compile time.** A chain of functions that each call the
  previous one twice did 2^N inlinings, while CSE kept the IR tiny:
  measured at 5 s for N = 24, and days for N = 40.
  - **Fix:** a compile-work budget of 2^20 steps (statements and expressions
    lowered, plus the variable snapshots each `if` takes).
- **Unbounded recursion and memory on braceless `if` / `else if` / `for`
  chains:** 3000 nested `if`s overflowed a 1 MiB stack.
  - **Fix:** braceless bodies count toward the 128 nesting limit.
  - **New cap:** nesting after inlining is capped at 512. The deepest
    allowed program needs under 192 KiB of stack on arm64.
  - **Out of memory:** `bad_alloc` now becomes a diagnostic.
- **Smaller fixes:**
  - a self-call was accepted and could even run under a constant guard; it
    is now an error;
  - `f(x) + 1.0;` was accepted as a statement; it is now an error;
  - a local could shadow a function name; it is now an error;
  - a long loop that does end was reported as "does not terminate" after a
    10^6-step simulation; the message now gives the real trip count;
  - the op and live-value messages now say "after inlining and unrolling";
  - `floor(-0)` now gives `-0` (the spec says floor is exact, and so does
    every GPU). No golden changed.
- **The spec's limits table gained:**
  - statement nesting;
  - nesting after inlining (512);
  - compile work (1,048,576).

**Next (phase 3):** done, see "Phase 3 results".

## 16. Phase 3 results (2026-10-10)

**What exists:**
- **The SHADER compound layer** (`NEUI_COMPOUND_LAYER_SHADER = 8`): props
  `shader`, `u.<name>` / `u.<name>.<c>`, `resolution`, `time`, `fallback`,
  and a generic `name` on every layer. All routed through the existing
  `set_*` / `bind` / `bind_asset`: no compound vtable change. Storage on
  `CompoundLayer` (`compound.h`); uniform resolution, placement and fallback
  logic in `hosts/shared/shader_layer.h`, called from
  `widget_paint_compound.h`.
- **`shader_api->draw`** (appended), with `neui_shader_draw_t`.
- **The image cache** (`ShaderCache`, one per session on the asset store):
  content-keyed, LRU within 48 MB of CPU pixels, per-ctx uploads with
  device-loss re-upload, released with the shader, the context and the
  store.
- **Seams:** the painter's `resolve_shader` thunk (9 construction sites:
  5 window paints, 4 surface paints) and the component loader's
  `shader_from` hook, both from `ShaderApi<Host>`; each host's policy gained
  `store_from_token`.
- **A backend slot, `get_device_transform`** (appended, nullable),
  implemented by D2D, CG and Cairo. The design assumed the painter could
  place the image without it; it cannot, because hosts and clients translate
  (and may rotate or scale) before a layer paints.
- **Component JSON:** a `"shaders"` map (file or inline `source`), `"kind":
  "shader"` layers with `"uniforms"`, `"resolution"`, `"time"`,
  `"fallback"` by layer `"name"`, binds on `u.*` and `shader`; full serialize
  round trip.
- **Example:** live knobs, a component knob and a `draw` panel added to
  `neui_shader_example` (see §11).
- **Tests:** 22 new cases (`test_shader_layer.cpp`, three in
  `test_component_loader.cpp`): 420 pass on macOS, also under ASan + UBSan.
  The suite now links `neui-qrcodegen`, so the compound paint walk itself is
  under Tier-1 test for the first time.

**Verified:**
- **macOS**, native and xpl hosts: identical rendering; synthesized drags on
  a live knob and the component knob re-shade them, and the `draw` panel
  follows the first knob.
- **Builds:** macOS (Ninja, Debug), the `NEUI_NO_SHADERS` configuration and
  the iOS simulator (Xcode) build clean.
- **Windows** (D2D, MSVC Debug `/W4 /WX`): a clean build, 422/422 tests, and
  at 150% the live row is as sharp as the baked knobs on both the native
  win32 and the xpl host, so D2D's `get_device_transform` places the images
  on whole device pixels. Real-mouse drags on both hosts re-shade only the
  dragged knob (screenshot diffs: the other knobs' pixels unchanged), and the
  `draw` panel follows the first knob only.
- **Bench on Windows** (i7-6800K, MSVC 19.42, unchanged executor since phase
  2): flat 2.8, ring_glow 13.2-14.3, plasma 34.5-34.7, metal_knob 64.8-65.1
  ns/px; fbm5 145, up from 134 in phase 1 and 140-143 in phase 2. The rise
  arrived with phase 2's sign-preserving `floor` (the noise built-ins floor
  every octave); within noise of the phase 2 figure.
- **Seen in passing:** the glow knob's glow is cut off at the widget rect
  and at the arc's end angle, in the baked strips and the live layer alike.
  That is the shader, not the layer; soften it in the shader if wanted.

**Decisions taken while building it:**
- **Placement snaps the sub-pixel remainder to 1/256 device px.** Float
  noise in a backend's transform (CG composes doubles, D2D floats) would
  otherwise split the cache and could change pixels; whole logical px give
  exact remainders at the usual scales anyway. Documented in the spec's
  pixel grid.
- **Under rotation, skew, mirror or uneven scale** the layer is shaded in
  its own space at `sqrt(|det|)` and drawn through the transform (resampled);
  there is no device grid to match.
- **`resolution` is clamped to [1/16, 4].**
- **A vec3 / vec4 bound as a whole to an int attr is a raw colour**;
  scale, offset and step do not apply. A float attr splats.
- **Uniform statics and bindings that the current program does not declare
  are ignored**, so a shader can be swapped under existing props.
- **`time` is a layer prop** for now (bind it to animate); phase 6 adds the
  clock.

**Not done (deliberately):**
- **Live-resize stretch:** a resizing shader layer re-shades every frame.
- **A per-layer `rev` cache** of the uniform resolution: not needed at the
  measured cost.
- **Linux on-screen check** of the live layer: CI builds and tests it; the
  Cairo `get_device_transform` has no on-screen verification yet.

**Next:** phase 4 (D2D GPU path) or phase 6 (time and animation; partial
repaint, its prerequisite, is done). Which one first depends on the client's
use case.

## 17. Phase 4 results (2026-10-10)

**What exists:**
- **The GPU form of a program** (`src/shader/nsl_gpu.cpp`, in the library):
  - `emit_hlsl` writes the pixel program as a `ps_4_0` pixel shader for a
    zero-input D2D effect. It uses neui's own polynomials and hash (no GPU
    intrinsics), the spec's pixel grid, coverage, rounding and dithering,
    and marks every variable `precise`, so no multiply-add is fused. The
    shader outputs the 8-bit values as exact multiples of 1/255, times an
    opacity.
  - `gpu_constants` fills the constant buffer: two float4 of pixel grid,
    then the values the pixel program loads. The draw program runs on the
    CPU, so everything computed once per draw is bit-exact.
- **The backend ABI** (§3, "Backend ABI"): `create_shader_program`,
  `destroy_shader_program`, `draw_shader_program` (device pixels), appended
  and nullable.
- **D2D** (`backends/d2d/d2d_shader.cpp`):
  - `D3DCompile` from System32's `d3dcompiler_47.dll`, on a worker thread;
    draws are declined until the compile is done, so no paint waits for it;
  - one custom effect class (zero inputs, one blob property: output size +
    constants), one instance per program per device context, shaders keyed
    by a GUID hashed from the text;
  - drawn at the floored device origin with `SetDpi(96)` + identity, the
    clip and the alpha stack honoured;
  - feature level 10_0 or the CPU.
- **Host:** `shader_paint` uses the GPU on the device grid (translation +
  uniform scale, resolution 1) when the session's `ShaderCache` has a
  program for the shader; everything else, and any declined draw, goes to
  the CPU image. `shader_api->set_gpu(session, false)` and `#pragma neui
  cpu_only` force the CPU. Bakes stay on the CPU.
- **Tests:** `tests/test_shader_gpu.cpp` (8 cases) and the Windows smoke
  target `neui_shader_gpu_smoke` (ctest, WARP in CI).
- **Example:** a "GPU where available" checkbox over the live row.

**Measured** (neui-win, GTX 1080, i7-6800K, MSVC; and WARP in CI):
- GPU vs CPU over 6 programs at 1×, 1.25×, 1.5× and 2× with fractional
  origins: **at most 1/255**, no pixel over 2/255, nothing outside the rect.
  Two draws of one program in one frame, a pushed clip and alpha 0.5 are
  right too.
- In the example at 150%: GPU vs CPU differ in 34 of 121 030 pixels of the
  live row, 31 of them by 1-2/255. The other 3 sit on the ring knob's hard
  `step()` edge and flip colour (see "Fidelity contract": the exception for
  discontinuities was added because of them).
- Plasma, 240×240 px per frame: **GPU 0.10 ms, CPU 2.19 ms**.
- Compile time per shader: 15-136 ms on the worker thread; creating the
  program returns in about 0.1 ms. (A first "compile" of 183 ms in an
  early run was the D3D device being created, which a window has already
  done.)
- With the background compile, the first frame is the CPU image and the
  GPU takes over at the next repaint; an idle UI keeps the CPU image until
  something invalidates. Indistinguishable on screen. Phase 6's clock
  repaints animated shaders anyway; a "program ready" invalidate is
  possible if profiling ever needs it.

- **Bench after the canonical IR order** (MSVC, SIMD ns/px): flat 2.75,
  ring_glow 14.1, plasma 34-36, fbm5 136-144 (was 145), metal_knob 64: no
  regression.

**Found and fixed on the way:**
- **The compiler's IR depended on the C++ compiler.** Lowering builds IR
  nodes inside function argument lists, which MSVC and GCC evaluate right to
  left and Clang left to right, and it merged `if` branches in hash-map
  order. Values were never affected (every CPU golden matched everywhere),
  but register numbers, the schedule and so the HLSL text differed between
  compilers, and with an `if` even between runs. `finalize` now renumbers
  the live IR canonically (the inputs, then a post-order walk from the
  outputs), found by neui-win.
  - Side effect: the post-order schedules each value just before its use.
    Register pressure only drops (one limit test had to be rewritten to
    still exceed 256 live values). The SIMD kernels are as fast as before
    on the M1 and on MSVC; the scalar test hook, which runs one pixel at a
    time, got slower on the M1 on long dependency chains (fbm5 591 to 1180
    ns/px; not on MSVC). Production never uses it.
- **Stale D2D descriptions** (§8): `d2d_backend.h` and
  `docs/rendering-and-assets.md` no longer describe WIC /
  `ID2D1HwndRenderTarget`. (`plans/winui3-host.md` still does; it is a
  deferred analysis of the old design.)

**Decisions:**
- **GPU always when eligible, no image cache:** a GPU draw costs about as
  much as a cached bitmap draw plus a small effect overhead, and it keeps
  automation and large layers cheap.
- **Resolution below 1 stays on the CPU:** it is the cheap path anyway.
- **No `shader_caps` slot:** a null program says the same, per shader.

**Next:** phase 6 (time and animation). Phase 7 (Metal, CPU thread pool)
only if measured necessary.

## 18. Phase 6 results (2026-10-10)

**What exists:**
- **The clock** (`hosts/shared/anim_clock.h`, one per session, owned by the
  `ShaderCache`): seconds of running time on `std::chrono::steady_clock`;
  stands while paused or while the OS asks for reduced motion and continues
  from where it stopped. Every widget painted in one frame reads the same
  frame time.
- **Which widgets animate** is learned from paints, not declared: each host
  brackets a CUSTOMDRAW paint with `begin_widget` / `end_widget` and marks a
  widget whose paint read the clock, with an owner (its frame on xpl and
  win32, the session on macOS and iOS). A timer per owner calls `tick`;
  when a frame is due (fps cap) it invalidates the widgets painted since the
  last frame and drops the ones that missed two frames running (one miss is
  a late paint and is invalidated again). A dropped widget
  that paints again (shown, scrolled back, window restored) is marked again
  and restarts the timer, so nothing tracks visibility, minimised windows
  stop on their own, and a widget whose layer gets a set time stops too.
- **Readers:** a SHADER layer whose shader reads `neui_time` and whose
  `"time"` is neither set nor bound (`CompoundLayer::shader_time_set`, and
  component serialize now emits `"time"` whenever it was set, also 0); and
  `shader_api->animation_time(sess)`, for `draw` or anything else a
  CUSTOMDRAW animates.
- **API** (appended to `neui_shader_api_t`): `animation_time`,
  `set_animation_fps` (default 30, clamped to [1, 120]),
  `set_animation_paused` (resuming invalidates the marked widgets through a
  new host-policy hook, `invalidate_widget`).
- **Timers:** xpl: `platform_set_animation_timer(native, ms)` +
  `Session::animation_tick(frame)` (win32 `SetTimer` on the frame HWND,
  macOS / iOS an `NSTimer` on the frame's view, Linux the shared 16 ms
  timerfd heartbeat, stubs on null and LVGL); win32 native: a `WM_TIMER` on
  the frame HWND; macOS and iOS native: one block `NSTimer` per session
  (looked up by session id, stopped on session destroy). The interval is
  half a frame (at least 8 ms) so a frame is never more than half a frame
  late; a stall resyncs instead of bursting.
- **Reduced motion:** Windows `SPI_GETCLIENTAREAANIMATION` ("Show
  animations in Windows" off), macOS `accessibilityDisplayShouldReduceMotion`,
  iOS `UIAccessibilityIsReduceMotionEnabled`, read at each tick and after a
  paint that marks; while it holds the clock the timer polls every 500 ms,
  so animation comes back when it is turned off. Not read on Linux.
- **Cache:** an image made at the clock's time drops the clock images of
  other times, so an animation keeps one image per widget instead of filling
  the 48 MB budget and pushing out the knobs' images. A `draw` whose time
  equals the clock's frame counts as clock-driven.
- **Tests:** `tests/test_anim_clock.cpp` (8 cases: frame gating, agreement
  within a frame, dropping, stalls, pause, reduced motion, owners) and 3 more
  in `test_shader_layer.cpp` (clock vs set / bound time, clock images
  dropped, `animation_time` + pause / resume through the API). 439 cases on
  macOS, 441 on Windows.
- **Example:** an "Animated on the session clock" row: the plasma on
  `neui_time` as a SHADER layer next to the same shader drawn with `draw` +
  `animation_time` at half resolution, an fps combobox, a Pause checkbox and
  a paints/s readout. `neui_shader_example <host id>` picks the host.

**Measured** (M1, macOS, 2x):
- Both hosts animate at 30.2-30.4 paints/s at the default 30 fps.
- xpl with `NEUI_DEBUG_PAINT_STATS`: every animation frame is partial
  (30-31 frames/s, 3.3-3.7 ms average).
- 10-13% of one core for the two panels (302 x 88 logical px each, the left
  at full resolution on the CPU).

**Measured** (neui-win: GTX 1080, i7-6800K, 4K 60 Hz at 150%, MSVC Debug
`/W4` warning-free; 441 tests, `neui_shader_gpu_smoke` PASS; CPU from
`QueryProcessCycleTime`, as `GetProcessTimes` under-counts short bursts):
- **Rate:** native 9.8 / 29.6-30.7 / 60.0 paints/s for 10 / 30 / 60 fps;
  xpl 11.0 / 30.5-31.6 / 58.3-58.7 (`SetTimer`'s ~15.6 ms grid). Both panels
  in phase in every shot.
- **Pause:** frames bit-identical 1.5 s apart, 0.1% CPU, resumes at ~30.
  **Minimise** (and `SW_HIDE`, as DAWs hide editors): 0.1-0.3% CPU, no
  frames; restore animates again. **Reduced motion** toggled at runtime:
  froze within 0.7 s, 0.1% CPU, resumed within 1.2 s.
- **CPU, both panels** (% of one 3.4 GHz core, native / xpl): GPU path
  2.5 / 2.6 at 10 fps, 6.8 / 7.1-7.3 at 30, 13.6 / 12.8 at 60; CPU path
  15.5 / 15.6 at 30, 31.1 / 30.4 at 60. About 0.23 ms of CPU per frame on the
  GPU path and 0.53 ms on the CPU path.
- **xpl partial repaint:** every animation frame partial; at 30 fps
  1.6-1.7 ms a frame on the GPU path, 4.2-4.4 ms on the CPU path.
- **At 60 fps on the 60 Hz display `paint_frame` averages ~16.4 ms on both
  paths:** frames arrive slightly faster than vblank and `Present1(1, ...)`
  on the 2-buffer flip chain waits for it on the UI thread. Not CPU (13%),
  but the thread waits for most of every frame; in a DAW that is the host's
  UI thread. With several sessions on one thread all of them share about
  170-180 presents/s (8 editors: 21 fps each). Presenting with sync interval
  0 measured 1.6-1.8 ms a frame and full rate for 8 sessions, without
  tearing (DWM still composites at vblank); but it also stops pacing
  input-driven paints (a 1000 Hz drag then paints ~3x the refresh rate at
  2.5-3x the CPU), so it needs a paint-rate cap too. Recorded with that design
  in `docs/deferred-issues.md` first, then done as a follow-up (below).
- **DAW embedding** (review): `SetTimer` with no `TIMERPROC` on the frame
  HWND is the right shape (no code pointer into the plugin DLL, the timer
  dies with the HWND, ids cannot collide between instances, the DAW's pump
  dispatches it). `WM_TIMER` is the lowest-priority message, so a busy DAW
  UI stretches frames; the clock uses elapsed time, so that lowers the frame
  rate, not the speed.

**Found and fixed on the way:** the win32 host's `WinMain` handed the
client's `main` UTF-16 `wchar_t**` arguments while every example declares
`int main(int, char**)` (MSVC does not mangle `main`, so it linked):
`argv[1]` read as `"n"`. It now passes UTF-8 and frees the
`CommandLineToArgvW` block. This affected `neui_partial_repaint_example`'s
argument too.

**Decisions:**
- **API slots, not attrs.** The plan had `NEUI_ATTR_ANIMATION_FPS` and a
  per-widget `NEUI_ATTR_ANIMATE`; the rate and the pause are per session, so
  they sit on the shader interface next to the clock, and a layer opts out by
  setting or binding `"time"`.
- **A timer of its own** instead of generalising the toast heartbeat into a
  ref-counted one: separate flags per user cannot stop each other, and the
  clock asks for its own interval.
- **No visibility tracking:** dropping widgets that do not paint after an
  invalidation covers hidden widgets, scrolled-away ones and minimised
  windows on every host with one rule.
- **`platform_now_ms` stays** (`GetTickCount64` on Windows): the clock reads
  `steady_clock`, which is `QueryPerformanceCounter` on MSVC.

**Follow-up: present pacing on Direct2D** (same day, after the measurements
above):
- `Present1(0, ...)`: the UI thread no longer waits for vblank. On a
  windowed flip chain without `ALLOW_TEARING` the compositor still shows the
  newest frame per refresh, so nothing tears, and the canvas copy keeps every
  back buffer complete.
- **Paint pacing** (`hosts/shared/paint_pacer.h` + `win32/paint_pacer_win32.h`,
  per HWND on both Windows hosts): a paint within 90% of a refresh interval of
  that window's last present keeps its update region and comes back from a
  one-shot `WM_TIMER`, so a 1000 Hz drag paints at the refresh rate instead of
  ~3x it, and animation ticks and input share frames. The first paint and the
  one after a resize, DPI change or show never wait; the rate is the
  monitor's (`EnumDisplaySettingsW`). Tier-1: `tests/test_paint_pacer.cpp`.
- **The clock tolerates one late paint:** a widget is dropped after two
  missed frames, not one, since a held paint can outlast an animation tick.
- **Two timing bugs found on Windows and fixed:** stamping the present after
  the paint made each 15.6 ms animation tick look early by the paint's ~1.7 ms
  and get held, and a held paint's 1-2 ms `SetTimer` fired at the next
  15.6 ms tick, so 60 fps animations ran at ~32. Times are now taken when a
  paint starts, and a wait under 4 ms is not held unless that would put two
  presents within half a refresh.
- **Measured** (neui-win, GTX 1080, 60 Hz, Debug, CPU path, 1000 Hz raw-input
  drag; presents/s and CPU % of one core, sync 1 -> paced): xpl paused drag
  59 / 11.0% -> 65 / 17.7%; xpl 60 fps idle 60 / 30.4% -> 58 / 30.3%; xpl
  60 fps + drag 60 / 14.0% -> 68 / 44.0%, with the animated panels at 55-60
  fps during the drag instead of ~20 (no more starvation). K sessions on one
  thread at 60 fps: K = 4 44.5 -> 53.3 fps each, K = 8 21 -> 53.2
  (`paint_frame` 5.3-5.6 ms -> ~1.5 ms; at K = 8 the thread is CPU-bound on the
  CPU-path shaders). 30 fps idle unchanged. Live resize, tab switching, the
  DirectComposition overlay and partial-repaint damage show nothing stale.
  Drags cost more CPU than with vsync at the same paint rate: Windows no longer
  coalesces mouse moves while the thread sat in `Present`, so every move runs
  the value update (deferred: coalesce drag input per frame).
- **Re-measured on the final code:** xpl 60 fps idle 61 / 30.2% (paint
  1.6-1.8 ms), 60 fps + live drag 72 / 42.2% with the panel at 60.6; native
  60 fps + live drag 220-230 presents (summed over the HWND chains) with the
  panel at 47-54 (each HWND is paced on its own; deferred); K = 8 sessions
  54.4 fps each; nothing stale after fast drags. The half-refresh guard is
  untested on a >60 Hz display.
- Still deferred: skipping ticks for a fully occluded (not minimised) window.

## 19. Phase 9 design: texture sampling (2026-10-11)

**Status:** both spikes done (§20) and merged to `next`:
the language, the CPU executor and the D2D draw path work and are tested on
every platform (step 9a without the spec section, and most of 9c). Hosts do
not bind bitmaps yet (9b), so a shader that samples runs on the CPU with
unbound samplers. The spec (`docs/shader-language.md`) changes in step 9d.

Phase 9 lets a shader read a BITMAP or SURFACE asset: a `sampler2D` uniform
bound to an asset, and a sample built-in. Today a shader is a function of
its position, its uniforms and time only (spec §1). `sampler2D`, `texture`
and `texture2D` are already reserved words that report "not supported in
.nsl v1" (`src/shader/nsl_compile.cpp` ~189), so adding them breaks no
program that compiles today.

### 19.1 What it enables

- **Image-based looks:**
  - a knob lit from a **normal map**, with the light or the rotation as a
    uniform bound to the value: one small bitmap instead of a filmstrip;
  - photographic **textures** (brushed metal, wood, paper) under procedural
    light, rings or arcs;
  - **palette LUTs**: a 256×1 bitmap indexed by a value (gauges, heat maps,
    theme-coloured gradients);
  - artist-drawn **masks** and shapes, combined with animated shader work.
- **Cheaper effects:** a noise tile baked once with `bake_bitmap` and
  sampled with `repeat` replaces live fbm (46 ns/px on the M1 Ultra, 131 on
  the i7, §7). `bake_bitmap` returns a BITMAP, so one shader's output feeds
  another without new API.
- **Phase 8:** a filter-graph shader primitive (`NEUI_FE_SHADER`) samples
  its input image. This phase provides the sampling; phase 8 then only binds
  the primitive's input buffer as a sampler.

### 19.2 Language

- **Declaration:** `uniform sampler2D name;` at global scope, at most **4**
  per program. A sampler counts toward the 32 uniform declarations, not
  toward the 64 scalar components, and has no default value.
- **Where a sampler may appear:** as a uniform, and as an `in` parameter of
  a user function (inlining makes it an alias). Everything else is a
  compile error with a `line:col`: a sampler local, an assignment, `out` /
  `inout`, a return type, a comparison, a `?:` operand or an arithmetic
  operand.
- **Sampling:** `vec4 texture2D(sampler2D s, vec2 uv)`. `texture` is
  accepted as an alias, for Shadertoy ports.
  - `uv` is normalised: `(0, 0)` is the bitmap's **top-left** corner and
    `(1, 1)` its bottom-right, y-down like `p`. A Shadertoy port flips
    `uv.y` as it flips `p.y`.
  - The result is **straight** RGBA, like the value `main` returns.
- **Sizes**, both draw-time values (hoisted, so free per pixel):
  - `vec2 neui_texture_size(sampler2D s)`: the bitmap's **logical** size,
    `width_px / scale` with the bitmap's own HiDPI factor, to lay it out
    in px;
  - `vec2 neui_texel(sampler2D s)`: one texel in `uv` units,
    `1 / width_px`, for neighbour taps (blur, normal from height).
- **Modes are per sampler and fixed at compile time**, so the GPU form is
  emitted once: `#pragma neui sampler <name> [clamp|repeat|border]
  [linear|nearest]`. The default is `clamp linear`; `border` reads every
  texel outside the image as transparent. A mode applies to both axes.
- **Defined arithmetic.** The CPU result must stay bit-identical on every
  platform (spec §12). With `W`, `H` the bitmap's size in texels:
  - **Nearest:** texel `(floor(u * W), floor(v * H))`.
  - **Bilinear:** `t = u * W - 0.5`, `x0 = floor(t)`, `x1 = x0 + 1`,
    `fx = floor((t - x0) * 256 + 0.5)`, weights `256 - fx` and `fx`; the
    same for y. The fraction is quantised to 1/256, like GPU filtering
    hardware.
  - **The filter runs on the premultiplied 8-bit texels as integers:**
    `C = (c00*(256-fx) + c10*fx) * (256-fy) + (c01*(256-fx) + c11*fx) * fy`.
    Its largest value is `255 * 256 * 256 = 16 711 680 < 2^24`, so every
    product and sum is exact in float, on the CPU and on the GPU, in any
    order.
  - **Straight result:** colour `C / A` (0 when `A == 0`; never above 1,
    since a premultiplied texel has `c <= a`), alpha `A / 16711680`.
    Nearest is the same with weights 256 and 0. The final division is the
    only rounding.
  - Filtering premultiplied keeps colour from bleeding out of transparent
    edges. There is no sRGB linearisation, as in the filter graph.
- **Identity:** a layer the size of a bitmap, at the bitmap's own scale,
  that returns `texture2D(s, p / neui_size)` reproduces the bitmap's bytes
  exactly. Float noise puts the bilinear position a hair beside the texel
  centre, and the 1/256 quantisation turns that into a weight of 0 or 256.
  A golden pins it.
- **An unbound sampler** (nothing bound, an asset that is not a bitmap, a
  destroyed handle) samples as `vec4(0)` and its sizes are `vec2(0)`.
  That is well-defined and never a crash; dividing by such a size follows
  the spec's IEEE rule, and the output clamp maps the NaN to 0.
- **Not in phase 9:** mipmaps and LOD (a texture drawn much smaller than
  its size aliases; the guide will say to supply textures near their
  display size), derivatives, cube and 3D textures, sampling the window
  being painted (phase 8), writing to a texture, and Metal.

### 19.3 Compiler and CPU executor

As built in the spike (§20); it replaced a first design with one op per
channel, which made a bilinear sample cost as much as `fbm5`.

- **Two new IR ops**, both with an immediate in `c` instead of a register:
  - `OP_TEXEL`: `a` = x and `b` = y (texel coordinates as floats), the
    immediate packs the sampler and the edge mode. It reads one texel and
    returns its 4 premultiplied bytes as the **bits** of a little-endian BGRA
    word. It converts the coordinates to int32 with saturation (NaN reads
    texel 0) and wraps them in **integer** arithmetic (clamp, repeat as a
    floored modulo, border as a range test that returns 0): a float `mod` is
    not exact for large coordinates.
  - `OP_CHANNEL`: one channel of a texel word as an integer-valued float,
    0-255; the immediate is the channel. Only `OP_CHANNEL` reads a texel
    register, and nothing does arithmetic on it.
- **Everything else lowers to existing ops** (`MUL`, `SUB`, `ADD`, `FLOOR`,
  `DIV`, `EQ`, `SEL`): the coordinate math, the weight quantisation, the
  integer-exact lerp (the 4 weights are shared by the 4 channels) and the
  unpremultiply. Deterministic by construction; the HLSL form only adds the
  two new ops. Size: 80 scalar ops per bilinear sample (4 texel reads, 16
  channel extractions) and 19 per nearest sample.
- **Never folded:** texel reads depend on draw data, so constant folding
  never evaluates them; CSE still merges identical reads. A read whose `uv`
  is uniform-only is hoisted into the draw program like any uniform-only
  work, so the draw program, `execute_scalar` and `gpu_constants` all take
  the textures.
- **Sizes** are per-sampler reserved leaves (`W`, `H`, `1/W`, `1/H`, the
  logical size; all 0 when unbound), filled per draw like `neui_size`.
- **`nsl::Draw` gains a texture table:** `nsl::Texture { const uint8_t*
  bgra; int w, h; size_t stride; float scale; }` per sampler. The library
  never sees assets; the host resolves them.
- **The texel read is per lane** (SSE2 and NEON have no gather): one 32-bit
  read per lane, specialised per edge mode. Channel extraction is a SIMD
  shift and mask. Repeat estimates the quotient with a multiply by `1/W`
  and corrects it exactly, since an integer division per lane is slow on
  x86.

### 19.4 Host integration

- **Asset content ids.** An `AssetEntry` has no revision or uid today, and
  slots are reused without a generation check (`include/neui/d/assets.h`
  ~17), so a handle cannot key the image cache. Add
  `AssetEntry::content_id`: a process-unique 64-bit counter (the
  `ShaderAsset::uid` scheme, `shader_asset.h` ~39), set on allocate and
  renewed whenever the pixels change (`paint_surface`, the in-place filter
  ops). A reused slot gets a new id, so a stale image is never hit.
- **Image cache:** `ShaderCacheKey` (`shader_asset.h` ~67) appends the
  content id of every bound sampler (0 for unbound). `release_slot` drops
  the images keyed on the released id, as it already does for a shader's
  `uid`. An image keyed on an id that has since changed can never be hit
  again and ages out through the LRU.
- **Public API** (`include/neui/d/shader.h`, all appended, nothing moves):
  - `neui_shader_texture_t { const char* name; neui_asset_t asset; }`;
  - `textures` + `texture_count` at the end of `neui_shader_bake_t` and
    `neui_shader_draw_t`, both documented as zero-initialised and
    appendable;
  - `sampler_count` and `sampler_at` (name, wrap, filter) at the end of the
    vtable. Samplers stay out of `uniform_count` / `uniform_at`, so a
    client written today never sees a uniform with 0 components.
- **SHADER layer:** a sampler is a `u.<name>` prop like any uniform, set
  with the **existing** `set_asset` and bound with the existing
  `bind_asset` (an int attr that holds a handle). `apply_set_asset`
  (`hosts/shared/compound.h` ~883) learns `u.*` on SHADER layers.
  `apply_bind_asset` already stores `is_asset` for any prop, and
  `shader_layer_uniforms` (`shader_layer.h` ~263), which skips such
  bindings today, resolves them as textures. No new compound slots.
- **Component JSON:** `"textures": { "<sampler>": "<asset name>" }` on a
  shader layer, with names resolved through the top-level `"assets"` map
  exactly like an ASSET layer's `"asset"`, plus the serialize round trip.
  A small fix on the way: `resolve_asset` (`component_loader.h` ~418) is
  not memoized, so N layers that name one image load it N times; memoize
  it per load.
- **Sources:** BITMAP (file, memory, baked) and SURFACE. A filmstrip binds
  as its whole sheet; sampling one frame is deferred.
- **SURFACE content changes:** a layer that samples a surface repaints when
  the client invalidates it, as an ASSET layer showing a surface does
  today. A surface whose paint samples itself reads its previous contents.
- **Bakes** resolve the textures on the host and run on the CPU, as today.

### 19.5 D2D GPU path

- **Resource textures, not effect inputs.** D2D places an effect input
  itself, possibly inside an intermediate surface at an offset the shader
  would have to recover, and an input needs rect mapping in the draw
  transform. A resource texture
  (`ID2D1EffectContext::CreateResourceTexture` from the entry's BGRA bytes,
  bound with `ID2D1DrawInfo::SetResourceTexture`) is a plain texture the
  shader addresses directly. The effect stays a zero-input `'Source'`
  effect (`backends/d2d/d2d_shader.cpp` ~54), so the phase-0 placement on
  `SCENE_POSITION` and the rect mapping stay as they are.
- **HLSL:** one `Texture2D t<i>` per sampler and a function `n_texel<i>`
  per sampler with the same saturation and integer edge mode as the CPU,
  returning `round(t.Load(int3(x, y, 0)) * 255)` packed into a `uint`
  word; a texel register is its own `uint u<r>` variable, and `OP_CHANNEL`
  is a shift and mask of it. The bilinear filter is the same op sequence as
  on the CPU. Hardware filtering is not used: its sub-texel precision and
  rounding are implementation-defined. The fidelity contract (spec §12)
  therefore stays as it is (measured: at most 1/255, §20).
- **Renderer slot** (`include/neui/d/renderer.h`, appended):
  `draw_shader_program_textured(ctx, program, x, y, w, h, constants,
  count, const neui_shader_texture_desc_t* tex, uint32_t tex_count)`, with
  `{ const uint8_t* bgra; uint32_t w_px, h_px; uint64_t content_id; }`.
  As built, each effect (one per program and DC) keeps the textures it
  used most recently, by content id, within 32 MiB; a texture another
  effect on the same DC made is found by its id (`FindResourceTexture`);
  an unbound sampler reads a 1 x 1 transparent texture; device loss drops
  the effects and so their textures. A SURFACE that changes every frame
  makes a new texture per change (a new id); the old ones age out. A
  backend without the slot, or a `false` return, falls back to the CPU as
  today.

### 19.6 Spikes first (9.0) - done, results in §20

1. **CPU fetch cost** (this Mac, then the i7 via neui-win): `OP_FETCH` and
   the lowering on a branch, plus `shader_bench` cases: a pass-through, one
   bilinear sample, a 9-tap blur, a normal-mapped knob, and a baked noise
   tile against `fbm5`. Confirms the identity property and SIMD == scalar,
   and decides between the lowering and a fused op.
2. **D2D resource textures** (neui-win): a resource texture in the existing
   effect; the BGRA channel order and `Load` under ps_4_0; upload and update
   cost (a SURFACE that changes every frame); GPU against CPU at 100, 125,
   150 and 200% on the GTX 1080 and on WARP; re-creation after device loss.

### 19.7 Steps after the spikes

| Step | Scope | Main files |
|---|---|---|
| 9a | **Done in the spike** except the spec section (moved to 9d). Language + CPU: compiler (type, pragma, built-ins, diagnostics), `OP_TEXEL` / `OP_CHANNEL`, the executor's texture table | `src/shader/{nsl.h,nsl_ir.h,nsl_compile.cpp,nsl_exec.cpp}`, `tests/test_shader_texture.cpp`, `tests/shader_bench/` |
| 9b | **Done (§20).** Host side: content ids, cache key, API, layer props, JSON, bakes; the GPU path calls the textured slot | `include/neui/d/shader.h`, `hosts/shared/{asset_store.h,shader_asset.h,shader_layer.h,shader_api.h,compound.h,component_loader.h}`, `tests/test_shader_{asset,layer}.cpp`, `tests/test_component_loader.cpp` |
| 9c | **Done:** the HLSL texel read, resource textures, the textured slot, the smoke cases (spike); the per-effect texture LRU within 32 MiB, an HLSL golden in `test_shader_gpu.cpp` | `src/shader/nsl_gpu.cpp`, `include/neui/d/renderer.h`, `backends/d2d/{d2d_shader.cpp,d2d_backend.cpp}`, `tests/{test_shader_gpu.cpp,shader_gpu_smoke.cpp}` |
| 9d | **Done:** Docs + example: the spec section (`sampler2D`, `texture2D`, the pragma, the filter arithmetic, keywords, limits, §1's "no textures"), guide recipes (all compiling), a normal-mapped knob, a baked noise tile, a LUT gauge | `docs/{shader-guide.md,rendering-and-assets.md,compound-behavior-component.md}`, `CLAUDE.md`, `examples/shader_example.cpp` |

### 19.8 Tests

- **Done in the spike, `tests/test_shader_texture.cpp`:** sampler
  declarations and introspection, every misuse diagnostic, the pragma, the
  4-sampler and 32-uniform limits, `texture` as an alias, sampler
  parameters; the identity property at @1×, @2× and @4× for both filters;
  edge modes, NaN and huge coordinates; unbound samplers (no table, a null
  bitmap, a zero size, a bad stride); sizes; a hoisted sample; SIMD ==
  scalar; CRC goldens for nearest and bilinear under clamp, repeat and
  border, at fractional scales and origins.
- **`test_shader_asset` / `test_shader_layer`:** a pixel change misses the
  cache; a reused slot gives no stale hit; release drops the entries;
  `u.<sampler>` through `set_asset` and `bind_asset`; a bake with textures.
- **`test_component_loader`:** `"textures"` load and round trip.
- **`test_shader_gpu`:** HLSL golden text with a sampler.
- **`neui_shader_gpu_smoke`** (Windows), **done in the spike:** four
  textured programs (bilinear, nearest, repeat, two samplers with border),
  the 1:1 copy, a new content id, two bitmaps in one frame, an unbound
  sampler, and a textured timing.

### 19.9 Risks and open questions

- **CPU cost of the per-lane fetch.** Settled by spike 1: one texel read
  plus channel extraction (§20).
- **D2D resource-texture behaviour** (channel order, `Load`, two bitmaps
  in one frame). Settled by spike 2 (§20); the cost of a SURFACE that
  changes every frame is not measured yet.
- **Minification aliasing** without mipmaps. Guidance only in phase 9.
- **Handle reuse after destroy** still resolves to the new occupant, the
  existing documented caveat (`assets.h`). Content ids prevent stale
  pixels, not a wrong binding.

## 20. Phase 9.0 results: texture sampling spikes (2026-10-11)

Built on branch `spike/texture-sampling` (64fdcbe CPU, d72ce1e D2D), merged to `next`.

**What exists:**
- **Language:** `uniform sampler2D`, `texture2D` / `texture`,
  `neui_texture_size`, `neui_texel`, `#pragma neui sampler <name>
  [clamp|repeat|border] [linear|nearest]`, as designed in §19.2, with every
  misuse diagnostic. `nsl::samplers()` lists them; they stay out of
  `uniforms()`.
- **CPU executor:** `OP_TEXEL` + `OP_CHANNEL` and the exact integer filter
  (§19.3); `nsl::Draw::textures`. A bitmap drawn 1:1 at its own scale comes
  back byte for byte at @1×, @2× and @4×, for both filters.
- **GPU form and D2D:** the HLSL texel read (§19.5), resource textures in
  the existing zero-input effect, and the appended renderer slot
  `draw_shader_program_textured`. Hosts keep shaders that sample on the
  CPU until they bind bitmaps (9b).
- **Tests:** `tests/test_shader_texture.cpp` (§19.8), new
  `neui_shader_gpu_smoke` cases, and five bench programs.

**Verified:**
- The CRC goldens recorded on AppleClang arm64 match MSVC x64 (on the
  Windows machine) and every CI job: macOS, the iOS simulator, Linux GCC
  and Windows MSVC.
- **D2D, GTX 1080:** every textured case at 1.0, 1.25, 1.5 and 2.0× is at
  most 1/255 off the CPU, with no pixel over 2/255 and nothing drawn
  outside the placement. The 1:1 copy is exact (max 0). Two bitmaps in one
  frame, a new content id and an unbound sampler are right. The channel
  order (BGRA bytes read as `Load().xyzw`) and texture register `t<i>` for
  resource texture `i` were as assumed.
- **D2D, WARP:** everything passes; max 0 except the existing alpha 0.5
  case (1/255).
- `neui_shader_example` draws every panel unchanged on the Windows native
  and xpl hosts, GPU on.

**Measured** (SIMD ns/px; `tests/shader_bench/`; M1 Ultra AppleClang 21,
i7-6800K MSVC 19.42):

| Program | Ops (texel reads) | M1 Ultra | i7-6800K |
|---|---|---|---|
| `tex_copy`, one bilinear sample | 80 (4) | 19.7 | 40 |
| `tex_nearest`, one nearest sample | 19 (1) | 5.7 | 12-13 |
| `tex_blur9`, 9 bilinear samples | 642 (36) | 157 | 291-297 |
| `tex_normal`, normal-mapped lit knob | 139 (4) | 30 | 58 |
| `tex_noise`, baked fbm tile, repeat | 71 (4) | 18.8 | 36 |
| for comparison: `fbm5` | 191 | 50 | 137-138 |
| for comparison: `metal_knob` | 128 | 26-28 | 59-61 |

- **A knob** (120 × 120 logical @2×) with one bilinear sample re-shades in
  1.1 ms on the M1 and 2.3 ms on the i7; with a 9-tap blur, 9 and 17 ms.
- **A background** (800 × 600 @2×) with one sample: 35 and 72-76 ms.
- **The baked noise tile** costs 2.7× less than live `fbm5` on the M1 and
  3.8× less on the i7.
- **A normal-mapped knob** costs the same as the procedural `metal_knob`.
- **D2D:** a 240 × 240 px bilinear copy takes 0.094 ms per frame on the GTX
  1080 (CPU 2.31 ms) and 0.554 ms on WARP. Compiling a textured program
  takes 30-78 ms on the worker thread.

**Decisions taken while building it:**
- **A texel read plus channel extraction,** not one read per channel. The
  first design, 16 one-byte reads per bilinear sample, measured 49 ns/px on
  the M1, as much as `fbm5`. Reading the 4 bytes once into a register that
  holds their bits, then extracting channels with SIMD shifts, measured
  20.7: 2.4× faster, and the IR keeps one output per instruction.
- **A texel register holds bits, not a float.** Only `OP_CHANNEL` reads one
  and nothing does arithmetic on it. In HLSL it is a separate `uint`
  variable, so no bit pattern ever passes through float math.
- **The read loop is specialised per edge mode:** about 5%.
- **Repeat without an integer division:** a quotient estimated with `1/W`,
  then corrected exactly. `tex_noise` went from 53-55 to 36 ns/px on the
  i7; the M1 divides fast and did not change.
- **Unbound samplers on the GPU** read a 1 × 1 transparent texture, which
  gives the CPU's transparent result in every edge mode. Sizes are 0 in
  both.
- **Resource textures are kept per content id.** D2D finds one another
  effect on the same device context made (`FindResourceTexture`), so
  programs share a bitmap's upload.

**Not done (deliberately), all in later steps:**
- the spec section and the guide (9d);
- binding bitmaps in the hosts: content ids, the cache key, the API, the
  layer, JSON (9b);
- the D2D texture LRU with a byte cap, and an HLSL golden for a textured
  program (9c);
- re-creation after device loss (not exercised; the effects and their
  textures go with `release_dc`, as for untextured programs);
- the cost of a SURFACE that changes every frame on the GPU.

**What it means for skin authors:**
- One sample costs about as much as a 60-80-op shader.
- Normal maps, palettes, masks and baked noise are cheap.
- Blurs and many-tap effects belong in a bake (or on the GPU path), not in
  a live CPU layer.

**Next:** 9b, the host side.

**Step 9b (same day):**
- **What exists:**
  - `neui_shader_texture_t { name, asset }` and `textures` /
    `texture_count` on the bake and draw structs.
  - `sampler_count` / `sampler_at` with `NEUI_SHADER_WRAP_*` /
    `NEUI_SHADER_FILTER_*`.
  - A SHADER layer's `u.<sampler>` through `set_asset` and `bind_asset`
    (`CompoundLayer::shader_textures`).
  - Component JSON `"textures": { "<sampler>": "<assets name>" }`, plus
    `"asset": true` on a `"bind"` entry for an asset binding, with the
    serialize round trip.
  - `AssetEntry::content_id`, and the image cache keyed on it.
  - Shaders that sample use `draw_shader_program_textured` on the device
    grid, and the CPU elsewhere or without the slot.
- **Decisions:**
  - **Bitmaps resolve through the image cache,** not through a new painter
    thunk. `painter_resolve` (and a bake) point the session's `ShaderCache`
    at its store (`set_texture_lookup`), and `ShaderCache::texture(handle)`
    checks the session bits and the kind. No host code and no painter
    construction site changed.
  - **Content ids** are set in `alloc_slot`, so every kind gets one, and
    renewed in `drop_surface_gpu_cache`, the one place pixels change after
    allocation (`paint_surface` and the filters). That place and
    `release_slot` drop the images made from the old id at once, instead
    of leaving them to the LRU.
  - **Unbound samplers** (none set, a foreign session, a SHADER or other
    kind, a released handle) read as transparent.
- **Tests:**
  - introspection;
  - a layer sampling a set and a bound bitmap byte for byte at 2x;
  - foreign and wrong-kind handles;
  - release, then a new bitmap in the same slot (no stale image);
  - a repainted SURFACE (new id, images dropped);
  - draw and bake textures by name;
  - the GPU slot choice and descriptors;
  - JSON load and round trip.
- **Not done:** memoizing `resolve_asset` per component load. Each texture
  or ASSET layer naming one image still loads its own copy.

**Steps 9c and 9d (same day):**
- **9c:** each D2D effect keeps its textures in an LRU within 32 MiB
  (smoke: 48 MiB of bitmaps through one effect), and `test_shader_gpu.cpp`
  pins the HLSL of a program with three samplers.
- **9d, the spec:**
  - §6.1 samplers;
  - §9.4 the exact sampling definition;
  - pragma, limits, keywords, porting and grammar.
- **9d, the guide:**
  - §5.5 recipes, each compiled and rendered on the CPU: a tinted bitmap,
    a contain fit, a knob lit from a normal map, a seamless noise tile and
    water from it, a palette meter;
  - §7.9 binding bitmaps in the API and JSON;
  - performance rows, errors and porting.
- **9d, the subsystem docs:** `docs/compound-behavior-component.md` and
  `docs/rendering-and-assets.md`.
- **9d, the example:** `neui_shader_example` adds a sampling row. Its three
  textures (a normal map, a seamless noise tile, a 256 x 1 ramp) are baked
  by shaders at start and sampled by live SHADER layers: two lit knobs, a
  meter that follows the first, and water on the animation clock.
