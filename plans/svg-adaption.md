# SVG adaption: rendering many composited knobs efficiently

Status: design note / investigation. No implementation proposed yet.

Context: a designer-supplied SVG (a 134x134 rotary knob: layered drop-shadows,
gradient face, knurled bezel ring, ring highlights, rotating indicator line) was
evaluated as a blueprint for a compound widget, with a target of ~50 instances
on one panel. This note records the conclusions.

## 1. Can the SVG drive a compound layer 1:1?

Not verbatim. Mapping each SVG element onto the compound layer kinds
(`text` / `asset` / `rect` / `path` / `qr`):

| SVG element | Compound mapping | Verdict |
|---|---|---|
| Outer / left ring highlights (`fill-opacity` crescents) | PATH layer | OK if re-authored as `ARC` commands (SVG uses cubic `C` approximations) |
| Two shadowed `r=31` circles | rect/path + filter | needs blur filter; note it is the same circle drawn twice for a two-stop layered shadow |
| `r=29` gradient face | rect/path `fill_color` | BLOCKED: compound fills are solid ARGB, no gradient primitive |
| Knurled bezel ring (`#111111`) | PATH layer | BLOCKED: heavily cubic-bezier; PATH v1 is `MOVE_TO/LINE_TO/ARC/CLOSE` only |
| Indicator line (rounded rect) | rect layer + `corner_radius` | trivial; the only part that should rotate |

Two structural gaps beyond filters: **gradient fill** and **cubic/quad beziers**
(both currently deferred in the compound roadmap). A literal "feed SVG geometry
into `set_path`" is impossible today and, if forced, not efficient.

## 2. Procedural render vs bitmaps for 50 knobs

Assuming gradients + beziers + blur filters were all implemented, fully
procedural rendering of 50 knobs is still the wrong call.

Per-frame cost, all 50 knobs repainting (order-of-magnitude estimates; ratio is
robust):

| | Procedural (gradients+beziers+blur) | 3 bitmaps (bg + rotated + top) |
|---|---|---|
| Work / knob | 2 Gaussian blurs, 1 bezier bezel tessellation, gradient fill, 2 arcs, rect | 3 textured-quad blits (1 rotated) |
| D2D / CG (GPU) | ~15-30 ms | ~1-3 ms |
| Cairo (software) | ~100-250 ms (unusable, ~4-10 fps) | ~15-30 ms (~30-60 fps) |

Roughly **10x faster** with bitmaps on GPU; on software it is the difference
between unusable and fine.

Why:
- **The blur dominates.** 2 shadows x 50 = ~100 Gaussian-blur applies/frame. On
  GPU the per-apply overhead (intermediate RT alloc/bind, effect graph, draw
  calls) is ~0.1-0.3 ms each. On Cairo blur is a CPU convolution, ~0.5-2 ms each.
  Gradients/beziers being available does not help this; it is a different
  primitive class.
- **Recomputing static content.** Only the indicator rotation changes frame to
  frame; shadows / face / bezel are pixel-identical every frame and across all
  50 instances. neui makes this worse: hover/pressed transitions invalidate the
  whole frame (xpl), and the compound replays layers each paint with no
  "nothing value-bound changed" cache. So the realistic worst case is all 50
  repainting, routinely.

3-bitmap cost is just draw-call overhead (150 textured quads, ~5-20 us each);
the bg and top are one shared texture each (2 uploads total, not 100), only the
rotated middle differs by angle.

### Recommended runtime architecture

Bake once, blit many:
- Static stack (shadows + gradient + bezel + highlights) -> one shared `SURFACE`
  via `create_surface` + `paint_surface`, rendered once.
- Compound = 2 layers: an `asset` layer (the baked surface, anchored fill, z
  below) + a `rect` indicator layer (z above) with
  `bind(rotation, neui.param.value, ...)`.
- Re-bake only on theme flip / DPI change / resize, never during interaction.

Per-frame worst case becomes 50 cached blits + 50 small rotated rects:
sub-millisecond on any backend, fine even on Cairo. VRAM is ~150KB for one
shared 134x134 @2x surface.

## 3. When/where to rasterize: startup vs build pipeline

Performance is a non-issue either way: 50 identical knobs collapse to **one
bitmap**, so startup rasterization is a one-time sub-ms to few-ms cost. Decide on
**DPI exactness + theme recolor**, not speed.

- **Build pipeline (bake PNGs):** must pre-pick scales (`@2x`/`@3x`). Fractional
  DPI (125/150/175%) then upscales (soft) or forces many variants (bloat). Theme
  variants multiply the matrix. Adds a non-C++ rasterizer build step
  (rsvg/resvg/Inkscape), complicating the cross-platform CMake story.
- **Startup rasterization:** render at the exact device scale (crisp at any DPI),
  re-render on `WM_DPICHANGED` / monitor move and theme flip, ship the tiny
  vector source instead of an N-scale x M-theme PNG matrix. Preferred for a
  DPI-mobile plugin window.

The catch: neui has **no SVG renderer today** (SVG kind 5 reserved, stb is
raster-only). Two ways to get one:

1. Vendor a real SVG engine (lunasvg or resvg C API) that handles gradients +
   `feGaussianBlur`. nanosvg will NOT render the shadows; rule it out for this
   art. Downside: a non-trivial dependency in every shipped binary.
2. **Preferred: port the knob to painter-API draws into a `SURFACE`.** The SVG
   becomes the human blueprint, not a runtime asset; ~30 lines of `paint_surface`
   callback draw the static art directly. No SVG parser in the binary, exact
   device scale, trivially theme-aware (`current_palette()` colors). This is most
   in-keeping with neui's `create_surface` + `paint_surface` mechanism.

Lifecycle either way: cache the surface, re-render only on DPI change, theme
flip, or logical-size change. Never during interaction.

Recommendation: runtime (startup + re-render on DPI/theme change), via
painter-API-into-SURFACE. Reserve a vendored SVG engine for the case of many
complex SVGs, and the build pipeline only if dropping the SVG dependency
entirely and living with fixed-scale PNGs.

## 4. Filmstrip for the rotating part?

A filmstrip (pre-rendered angle frames, value -> frame index -> sub-rect blit)
replaces a rotation transform with a source-rect pick. It only optimizes away
*rotation*, which is expensive on exactly one backend.

Per-frame, 50 knobs:

| | GPU draws | GPU time | Cairo time |
|---|---|---|---|
| 3 bitmaps, live-rotated needle | 150 | ~1-3 ms | ~15-30 ms (resample dominates) |
| Full-knob filmstrip (1 blit) | 50 | ~0.5-1.5 ms | ~3-5 ms |

- **GPU (D2D/CG):** rotation is free (matrix + sampler), so a filmstrip frame
  blit ~= a live-rotated blit. The only saving is collapsing 3 layers -> 1 blit
  (~1 ms across 50 from fewer draw calls). Not worth it on its own.
- **Software (Cairo):** real ~5-10x win, because live rotation forces a per-pixel
  inverse-transform + bilinear fetch each frame, while a filmstrip frame is an
  axis-aligned near-memcpy blit. This is the historical reason filmstrips
  dominate plugin UIs (pre-GPU era).

Costs: VRAM blowup (128-frame full-knob strip @2x ~= ~37 MB/design vs ~1 MB for
the live-rotated shared textures, ~50-100x); angular quantization (128 frames
~2.1 deg/step, 64 ~4 deg shows stepping; live rotation is continuous); and
DPI/theme re-bake now rasterizes N frames per scale per theme.

Recommendation:
- GPU backends: skip the filmstrip; live-rotate the cached needle quad (same
  speed, ~50-100x less memory, continuous, 1-frame re-bake).
- Linux/Cairo with many simultaneously-repainting knobs: a filmstrip there is
  justified; consider making it backend-conditional (filmstrip on software,
  live-rotate on GPU).
- Memory-conscious middle ground: a lazy pre-rotated frame cache keyed by
  quantized angle (bounded memory, software speedup only for angles in use, adds
  eviction logic).

Net: the filmstrip is a software-rendering optimization; for a GPU-composited
knob it trades large memory for a benefit the GPU already gives for free.

## Open dependencies if this is pursued

- Gradient fill primitive on rect/path layers (compound roadmap, deferred).
- Cubic/quadratic bezier `set_path` commands (compound roadmap, deferred).
- Gaussian-blur / shadow primitive in the painter API (not present), OR bake
  shadows at rasterization time so no runtime blur is needed (preferred).
- An SVG rasterization path (vendored engine) OR a painter-API reimplementation
  of the static art (preferred).
