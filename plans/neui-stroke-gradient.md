# Plan: add `stroke_path_gradient` to neui

## Why

`neui` can **fill** a path with a gradient (`fill_path_gradient`) but can only **stroke**
with a solid ARGB (`stroke_path` / `stroke_path_styled`). An SVG importer (asvglib)
needs to render `stroke="url(#grad)"` — e.g. a dashed ring whose stroke is a
blue→green→red linear gradient. Today that falls back to the gradient's first
stop (a solid colour), which is visibly wrong.

D2D, Cairo and CoreGraphics all support stroking with a gradient brush natively;
the capability simply isn't exposed through neui's backend/painter vtables. This
plan adds one new entry point, `stroke_path_gradient`, end to end.

**Consumer:** asvglib bakes gradient strokes into a SURFACE via the **painter API**
(exactly as it already bakes gradient fills), so it calls:

```c
painter_api->stroke_path_gradient(p, stroke_width, &grad, style_or_null);
```

where `grad` is a `neui_gradient_t` (already used by `fill_path_gradient`) and
`style` is a `neui_stroke_style_t*` (cap/join/miter/dash) or `NULL` for a plain
stroke. **No compound-layer / component-JSON changes are required** — asvglib
rasterises gradient strokes, it does not need them expressible as a live PATH
layer. (If you later want gradient strokes on live compound PATH layers too,
that's a separate follow-up; this plan deliberately scopes it out.)

## The new entry point

Signature (mirrors `fill_path_gradient` for the brush + `stroke_path_styled` for
the stroke geometry/style):

```c
void stroke_path_gradient(<ctx-or-painter>, float stroke_width,
                          const neui_gradient_t* grad,
                          const neui_stroke_style_t* style);
```

Semantics:
- Strokes the **current path** (built via `begin_path`/`move_to`/`line_to`/`cubic_to`/…).
- `grad` geometry is in the same logical-pixel space as the path (identical
  convention to `fill_path_gradient`); fold the alpha stack into stop colours
  exactly as `fill_path_gradient` does.
- `style == NULL` → plain stroke (butt cap, miter join, solid line), same as
  `stroke_path`. Non-NULL → apply cap/join/miter/dash like `stroke_path_styled`.
- `grad == NULL` or `stop_count < 2` → no-op.
- `neui_gradient_t` already carries everything needed (kind, stops, extend,
  axis/centre, radius) — **no change to `include/neui/d/gradient.h`.**

## ⚠️ ABI ordering rule (read first)

The vtables are **positional initializers**. Add the new function pointer in the
**same relative position** in (a) the struct declaration and (b) **every** vtable
initializer that fills that struct. The safe, unambiguous choice: insert it
**immediately after `stroke_path_styled`** in the struct, and immediately after
each backend's `*_stroke_path_styled` entry in that backend's initializer.

Do NOT insert it after `fill_path_gradient` in the *initializers* unless you also
move the *struct field* there — struct field order and initializer order must
match exactly. Pre-1.0 neui rebuilds all hosts together, so shifting slots is
fine as long as struct + all initializers stay consistent.

## Edit map

All paths under the neui repo root.

### 1. `include/neui/d/renderer.h` — `neui_render_backend_t`
Right after the `stroke_path_styled` member (end of the path-curves/stroke block,
~line 332), add:

```c
  // Stroke the current path with a gradient brush (the stroke analogue of
  // fill_path_gradient). NULL style == plain stroke; NULL/<2-stop grad == no-op.
  void (NEUI_ABI *stroke_path_gradient)(neui_render_ctx_t ctx, float stroke_width,
                                        const neui_gradient_t* grad,
                                        const neui_stroke_style_t* style);
```

### 2. `include/neui/d/painter.h` — `neui_painter_api_t`
Right after the `stroke_path_styled` member (~line 189), add the matching
declaration (same params, `neui_painter_t* p` instead of the ctx):

```c
  void (NEUI_ABI *stroke_path_gradient)(neui_painter_t* p, float stroke_width,
                                        const neui_gradient_t* grad,
                                        const neui_stroke_style_t* style);
```

### 3. `hosts/shared/painter.h` — thunk + singleton
Add a forwarder next to `painter_stroke_path_styled`:

```c
inline void painter_stroke_path_gradient(neui_painter_t* p, float stroke_w,
                                         const neui_gradient_t* grad,
                                         const neui_stroke_style_t* style)
{ if (p && p->backend && p->backend->stroke_path_gradient)
    p->backend->stroke_path_gradient(p->ctx, stroke_w, grad, style); }
```

Then add `painter_stroke_path_gradient,` to the `k_painter_api` initializer in the
**same position** chosen in step 2 (i.e. right after `painter_stroke_path_styled,`).

### 4. `backends/d2d/d2d_backend.cpp`
Add `d2d_stroke_path_gradient` after `d2d_stroke_path_styled`. Reuse the two
existing helpers verbatim:
- `d2d_make_gradient_brush(ctx, grad)` — already used by `d2d_fill_path_gradient`
  (builds the linear/radial `ID2D1*GradientBrush`, folds the alpha stack).
- the `ID2D1StrokeStyle` build/cache logic from `d2d_stroke_path_styled`
  (cap/join/miter + dash lengths divided by width).

Body:
```cpp
finalise_path(ctx);
ID2D1Brush* br = d2d_make_gradient_brush(ctx, grad);
if (!br) return;
if (!style) { ctx->target->DrawGeometry(ctx->path, br, stroke_width); br->Release(); return; }
// ...build/cache ID2D1StrokeStyle exactly as d2d_stroke_path_styled does...
ctx->target->DrawGeometry(ctx->path, br, stroke_width, ctx->cached_stroke_style);
br->Release();
```
Add the entry to the d2d backend vtable initializer in the matching position
(after the `d2d_stroke_path_styled` entry).

> Tip to avoid duplication: you could factor the stroke-style build out of
> `d2d_stroke_path_styled` into a small helper that both functions call, but a
> straight copy is acceptable.

### 5. `backends/cairo/cairo_backend.cpp`
Add `cairo_stroke_path_gradient` after `cairo_stroke_path_styled`:
```cpp
cairo_pattern_t* pat = cairo_make_gradient(st, grad);   // existing helper
if (!pat) return;
finalise_path(st);
if (style) { /* cairo_set_line_cap/join/miter + cairo_set_dash, as in styled */ }
cairo_set_source(st->cr, pat);
cairo_stroke(st->cr);
cairo_pattern_destroy(pat);
```
Add the vtable entry in the matching position.

### 6. `backends/cg/cg_backend.mm`
Add `cg_stroke_path_gradient` after `cg_stroke_path_styled`. CG can't set a
gradient as a stroke "colour" directly; stroke→outline→clip→draw-gradient:
```objc
if (style) { /* CGContextSetLineCap/Join/MiterLimit + CGContextSetLineDash */ }
CGContextSaveGState(st->cg_ctx);
CGContextReplacePathWithStrokedPath(st->cg_ctx); // current path -> its stroke outline
CGContextClip(st->cg_ctx);                       // clip to the stroke region
cg_draw_gradient(st, grad);                      // existing helper (linear/radial)
CGContextRestoreGState(st->cg_ctx);
```
(`CGContextReplacePathWithStrokedPath` honours the line width/cap/join/dash set
just above, so the clipped region matches a real stroke.) Add the vtable entry in
the matching position.

### 7. `backends/null/null_backend.cpp`
```cpp
static void null_stroke_path_gradient(neui_render_ctx_t, float,
                                      const neui_gradient_t*,
                                      const neui_stroke_style_t*) {}
```
Add the vtable entry in the matching position.

### 8. `include/neui/d/gradient.h`
No change.

### 9. Compound PATH layer
No change required for this task (asvglib bakes). Optional future work: a
stroke-gradient slot on `CompoundLayer` + `set_stroke_gradient` + JSON
parse/serialize + a gradient-stroke branch in `widget_paint_compound.h`'s PATH
paint. Out of scope here.

## Verification

1. Build neui + the d2d backend (Windows) warning-clean (`/W4`).
2. Run the neui test suite (`neui_tests`) — should stay green; nothing here
   changes existing behaviour. (Optionally add a smoke test that calls
   `stroke_path_gradient` with a 3-stop linear gradient on the offscreen/cairo
   backend and checks a left-vs-right pixel differs.)
3. End-to-end (asvglib side, done separately): import `svg-gradient-logo.svg`;
   the decorative dashed ring (`stroke="url(#brandGradient)"`, blue→green→red)
   must render as a gradient — leftmost dash ≈ `#3399CC`, rightmost dash ≈
   `#E64E4E` — instead of solid blue.

## Notes for the asvglib side (already prepped, FYI — not your task)

asvglib will: resolve the stroke gradient (same `resolve_grad` it uses for fills,
honouring `userSpaceOnUse` vs `objectBoundingBox` and the element CTM), route any
shape with a gradient stroke (even with `fill:none`) to the bake path, and in its
`bake_paint` call `api->stroke_path_gradient(p, width, &grad, style)` with the
already-CTM-scaled width/dash. It null-checks the function pointer and falls back
to the solid first-stop stroke if the running neui predates this change.
