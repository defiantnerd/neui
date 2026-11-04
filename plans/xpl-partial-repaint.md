# Plan: partial repaint + rect invalidation on the xpl host

## Status

**P1-P4 implemented on every xpl platform and verified on screen** (macOS,
Windows, and Linux on 2026-10-11). Only the tuning below is left. On
Linux, the damage of an animated frame covered exactly the animated widgets,
and idle windows stayed at 0% CPU. The same check found one bug, fixed in
c9d8452: `paint_window` cleared the pending damage after painting, so an
invalidation raised during a paint was lost. It now takes the pending state
first, as the other platforms do.

**P1 implemented (branch `shaders`; Linux verified on screen 2026-10-11).** The pieces:
- `hosts/shared/damage.h` with Tier-1 `tests/test_damage.cpp`;
- `damage_rect_to_device` in `backends/shared/backend_util.h`;
- the `set_frame_damage` backend slot;
- the Cairo clip / clear / per-rect blit, covered by `neui_cairo_smoke`;
- `LinuxWindow::damage` + `platform_invalidate_rect`;
- the culled paint walk;
- `Session::invalidate_widget_rect`.

Two deviations from the design below:
- **`set_frame_damage` returns `bool`.** A backend may refuse a partial frame
  (fresh surface after create / resize, DPI change), and culling against a
  full clear would blank the skipped widgets. The host therefore culls only
  after `true`.
- **The `invalidate_widget_visual` funnel moved into P1**, so P1 is
  observable: attr writes, `set_text` / `set_asset` / `set_enabled`, the
  public `invalidate`, GRID content. That is exactly the DAW-automation path.
  `WidgetData::repaint()` (the internal hover / press / caret / focus sites)
  stays full-frame until P3.

**P2 implemented (macOS verified on screen, iOS built and tested by CI).**
- **Platform side:** `setNeedsDisplayInRect:` + `drawRect:` damage on both. The CG backend accepts damage for window ctxs; off-screen ctxs accept once they hold a frame.
- **Finding:** a layer-backed view gets one dirty rect, the bounding box of everything invalidated since the last draw. Scattered changes therefore coalesce into a full frame, and only localized ones pay: 0.10 ms against 1.15 ms per paint, with one of 48 knobs per tick, on an Apple-silicon Mac.
- **Added in P2:**
  - `WidgetData::paint_bounds()`, which a rotated IMAGE overrides. Its unclipped overflow was outside its damage. Windows found it and a negative test on macOS confirmed it.
  - `NEUI_DEBUG_PAINT_STATS`.
  - `neui_partial_repaint_example`.
- **Reference:** `docs/widget-internals.md`, "Partial repaint".

**P3 implemented (macOS verified on screen; Linux, 2026-10-11).** The internal invalidation paths now repaint only the widget:
- `WidgetData::repaint()` (114 widget-internal call sites);
- hover / press / focus transitions (old + new widget);
- `InputBoxWidget::on_keychar`;
- behavior-driven writes;
- SECTION / GRID scroll + spring-back ticks on Linux and macOS (the container's rect).

Deviations from the P3 plan:
- **Geometry changes stay FULL instead of damaging old + new.** They are rare, and full is simpler and always correct.
- **The audit turned up a gap:** off LVGL, `create` / `destroy` / `show` / `hide` / `set_pos` / `set_size` invalidated nothing at all. `notify_tree_changed` was an LVGL-only hook. It now requests a full repaint, which is what invariant 1 needs.
- **No "compound bounds margin" is needed:** CUSTOMDRAW / compound widgets are clipped to their own rect.

Measured in `neui_example` on the xpl host, macOS:
- **Interaction frames** (hover, typing, checkbox, slider drag): all partial, 0.15-0.42 ms, against 7.5-15 ms for this example's full frames.
- **SECTION / GRID scroll:** correct, but full by coverage, since each container fills most of its example's window.

P4 baseline from Windows: a D2D full frame of the 48-knob example costs 4.1-4.7 ms (Debug, 150%, desktop GPU) regardless of how many knobs change.

**P4 implemented and verified on Windows** (desktop GPU, 150%, Debug). It compiled clean under `NEUI_WERROR`, and the visual checks (damage tint, resize, partly off-screen and back, `neui_example` interaction) showed no stale or garbage pixels.

How it differs from the original design: it keeps a persistent canvas as planned, but copies the WHOLE canvas into the back buffer each frame instead of tracking damage(N) and damage(N-1). It's a GPU copy, cheap, and correct without bookkeeping. Multi-rect frames clip with a geometry-group layer; the mask is built in `set_frame_damage`, so a failure refuses cleanly.

Numbers for `neui_partial_repaint_example`:

| Knobs per tick | Full frame | Partial | Partial share |
|---|---|---|---|
| 1 | 3.8 ms | 1.4 ms (`pump_once` 196 -> 77 ms/s) | all frames |
| 8 | 4.1-4.7 ms | 3.1 ms average | about 45% |

At 8 knobs per tick the rest go full by coverage: nearby rects merge within `k_merge_gap`, and past `k_max_rects` = 8 the cheapest pairs merge until the boxes exceed 60%.

Tuning left open: a larger rect cap, or a smaller merge gap for GPU / per-rect-cheap backends.

The design text below is the original plan, kept for reference.

This is the first track ahead of
`plans/shader-effects.md`. It is worth doing on its own merits, because the
cost it removes already exists without shaders. It is also a hard prerequisite
for time-animated shaders (shader phase 6).

## Context: the whole frame repaints on every invalidate

The xpl host has no damage tracking. Any widget invalidation re-rasterises and
re-presents the entire frame:

- **Funnels:** `WidgetData::repaint()` (`hosts/crossplatform/host.cpp` ~1523)
  and `invalidate_widget_visual` (`hosts/crossplatform/widgets.cpp` ~122) both
  resolve to `platform_invalidate(frame)`. The public `widgets->invalidate`
  says so outright: "Map any widget invalidation to a frame-level repaint".
- **`Session::paint_frame`** (`host.cpp` ~2261) runs `begin_frame(clear)`, then
  the full `paint_widgets_recursive` walk (~2144), then the overlays (combo,
  menubar, popup, toast), then `end_frame`.
- **Per platform:**
  - **Linux / Cairo:** `platform_invalidate` sets a boolean `needs_paint`
    (`platform_linux.cpp` ~2090). `cairo_begin_frame` `cairo_paint`s the
    whole surface (`cairo_backend.cpp` ~414), and `cairo_end_frame` blits the
    whole surface with `XShmPutImage` + `XSync` (~439).
  - **macOS:** `platform_invalidate` is `setNeedsDisplay:YES`, and
    `drawRect:` ignores `dirtyRect` (`platform_macos.mm` ~163).
  - **iOS:** same shape (`platform_ios.mm`, `setNeedsDisplay`).
  - **Win32:** `InvalidateRect(hwnd, nullptr, FALSE)` (`platform_win32.cpp`
    ~1711), and `WM_PAINT` ignores `ps.rcPaint` (~550). The D2D `begin_frame`
    `Clear`s the whole target.

**What this costs.** A knob drag, a hover, a caret blink or one automated DAW
parameter re-rasterises every widget in the window. `plans/svg-adaption.md`
estimates 50 procedurally drawn knobs at ~100-250 ms per frame on Cairo, and
~15-30 ms even when bitmap-based. On CPU backends (Cairo, CG) the cost grows
with the size of the window, not with what changed.

**Precedent.** The LVGL platform already narrows invalidation to one widget
(`platform_retained_widget_invalidate`, `platform.h` ~410). The funnel point
exists; only the non-LVGL branch discards the widget index.

## Design

### Damage model (`hosts/shared/damage.h`, header-only)

- A small rect-list region in frame-logical px. It supports `add(rect)`,
  coalesces overlapping or nearby rects, caps the list at about 8 rects
  (merging the closest pair beyond that), and has `bounds()` and
  `intersects(rect)`.
- **Full-repaint fallback:** when coverage passes ~60% of the frame, or on any
  frame-level invalidation, the region collapses to "full". This is the
  existing behaviour, so every unconverted path stays correct.
- **Snapping:** conversion to device px snaps outward to whole physical pixels
  at the current scale, so fractional DPI (150%) never leaves a 1 px seam.
- Pure logic, so it gets a Tier-1 test (`tests/test_damage.cpp`).

### Session side

- `Session::invalidate_widget_rect(idx)` computes the widget's frame-local
  rect:
  - the cached `abs_x` / `abs_y` (already maintained by the paint walk for
    hit-testing);
  - plus the in-frame menubar inset (`frame_top_inset`);
  - clipped by any ancestor scrolling-SECTION body, using the scroll offset;
  - inflated by an overflow margin for the focus ring and out-of-rect
    decorations (a per-type margin, default a few px).
- **Per-frame damage:** each frame keeps a `damage` region that
  `paint_frame` consumes and resets.
- **Funnel conversion:**
  - `WidgetData::repaint()` (114 call sites in `host.cpp`) and
    `invalidate_widget_visual` (public `invalidate`, attr / value / compound
    writes) switch to `invalidate_widget_rect`.
  - **Not converted (stay full-frame):** the ~36 direct
    `platform_invalidate(frame)` calls (resize, theme, menubar, popup, scroll
    bounce, toast). They are correct as they are; convert them later only
    where profiling shows a need.
- **Geometry changes:** a widget that moves or resizes damages both its old
  and its new rect. Show and hide damage the rect too.
- **Subtrees:** invalidating a SECTION damages its whole rect, which covers
  its children.

### Paint walk

- `paint_frame` receives the damage region:
  - backend `begin_frame` clears only the damage;
  - the walk runs under `push_clip(damage.bounds())`, or one clip per rect if
    multi-rect clipping is worth it;
  - overlays paint only when they intersect the damage.
- **Culling in `paint_widgets_recursive`:**
  - **Skipped:** paint, and `WIDGET_PREUPDATE` (which fires per painted
    widget today) for widgets whose rect plus margin misses the damage.
  - **Still done for culled widgets:** update `abs_x` / `abs_y`, and recurse
    into children that may intersect.
  - **Why `abs_x` / `abs_y` must stay current:** hit-testing and event-coord
    translation read them (`dispatch_mouse_event`). A culled subtree whose
    parent moved must still get fresh absolute coords.
- **Correctness rule: clip, then cull.** Everything that intersects the damage
  is repainted in z-order. Overlapping siblings and transparent widgets
  therefore composite correctly, because the background under them inside the
  clip is cleared and repainted from the bottom up.

### Backend seam (vtable-append, nullable)

`void (*set_frame_damage)(neui_render_ctx_t, const float* rects_xywh, int count);`

- Called before `begin_frame`.
- **When the slot is null or `count == 0`:** the frame is full, which is
  today's path, so the null and LVGL backends need nothing.
- **Validity:** the damage is valid for exactly one `begin_frame` /
  `end_frame` pair.

### Per backend, in priority order

1. **Cairo (Linux):** biggest win, easiest.
   - The window surface already persists across frames (it lives as long as
     the ctx), so pixels outside the damage are still correct.
   - `begin_frame` clips to the damage and clears only there.
   - `end_frame` sends `XShmPutImage` / `XPutImage` only for the damaged
     sub-rects (one call per rect), then `XSync` as today.
   - The Expose path (`platform_linux.cpp` ~1365) stays full-frame.
2. **CG (macOS / iOS):**
   - `platform_invalidate_rect` maps to `setNeedsDisplayInRect:`.
   - `drawRect:` passes `dirtyRect` (or `getRectsBeingDrawn:` on macOS) as
     the damage.
   - AppKit / UIKit already clip the context and keep the layer content
     outside it, so the backend needs no present change. The win is the
     culled walk.
   - **Risk to verify on day 1:** partial invalidation must really be honoured
     for this view configuration (layer-backed by default on modern SDKs,
     `layerContentsRedrawPolicy`). The frame view is opaque, which helps.
3. **D2D (Win32):** smallest win (the GPU rasterises), hardest, **optional.**
   - The swap chain is `FLIP_SEQUENTIAL` with 2 buffers
     (`d2d_backend.cpp` ~638), so the back buffer holds frame N-2. With dirty
     rects the app must keep the content outside them itself.
   - The approach: render into a persistent `ID2D1Bitmap1` canvas, copy it to
     the back buffer, and pass the damage to `Present1` as
     `DXGI_PRESENT_PARAMETERS::pDirtyRects`.
   - `platform_invalidate_rect` maps to `InvalidateRect(hwnd, &rc, FALSE)`,
     and `WM_PAINT` passes `ps.rcPaint`.
   - **Until P4 lands, D2D leaves the slot null.** Culling without the
     persistent canvas is not safe: `begin_frame` clears the whole back
     buffer, so any culled widget would vanish. Win32 therefore keeps today's
     full frames, and its `platform_invalidate_rect` forwards to the
     full-window invalidate.
   - **Invariant for every backend:** the Session culls only when the backend
     accepted the damage, i.e. the slot is non-null and the platform
     delivered a partial rect.

### Platform seam

- `void platform_invalidate_rect(void* native_handle, int x, int y, int w, int h);`
  goes in `hosts/crossplatform/platform.h`, with one implementation each in
  the win32, macOS, iOS, Linux and null platform files.
- The default implementation forwards to `platform_invalidate` (full frame),
  so platforms can convert one at a time.
- On Linux the boolean `needs_paint` becomes a damage region on
  `LinuxWindow`; `needs_paint = true` stays as "full".

### Embedded mode

- **Linux:** `pump_and_tick` already ends in `paint_window`
  (`platform_linux.cpp` ~1940), so the damage flows through unchanged. The
  16 ms tick gate stays.
- **Win32 / macOS:** the DAW's pump delivers `WM_PAINT` / `drawRect:` with the
  OS dirty rect, which is the same path as standalone.

## Risks

- **Out-of-rect painting:**
  - **Problem:** widgets that draw outside their rect (focus ring, value-text
    overlays, KNOB value text, compound layers with negative offsets or
    `NEUI_COMPOUND_FILL` beyond the widget) leave stale pixels.
  - **Mitigation:** a per-type overflow margin. A compound asset could report
    its layer bounds union as the margin, computed once per compound `rev`.
  - **Catch-all:** an env/debug switch that forces full frames, to bisect
    artefacts.
- **SECTION scrolling:** children of a scrolled body have rects offset by
  the scroll position. Damage must use the same transform the paint walk
  applies (`push_clip(body)` + `translate`). Unit-test it against the walk's
  own math.
- **Overlays:** the combo drop list, popup menu and toast sit above
  everything. When the damage intersects an overlay, the overlay repaints.
  When the overlay itself changes, it uses the full-frame path, as today.
- **`WIDGET_PREUPDATE` semantics:** clients that use PREUPDATE to refresh
  state will see it only for widgets being repainted. That is the documented
  intent ("before we read it during paint"), but it needs a note in
  `docs/widget-internals.md`.
- **Correctness first:** every conversion is opt-in per funnel, and the
  full-frame path is always the fallback, so a bug degrades to today's
  behaviour rather than to corruption. The one exception is a missed damage
  rect, which leaves stale pixels.

## Phases

| # | Scope | Main files |
|---|---|---|
| P1 | `damage.h` + Tier-1 tests; Session damage region + `invalidate_widget_rect`; culled walk; backend slot; **Cairo** implementation; Linux platform | `hosts/shared/damage.h`, `tests/test_damage.cpp`, `hosts/crossplatform/{host.cpp,host.h,widgets.cpp,platform.h,platform_linux.cpp}`, `include/neui/d/renderer.h`, `backends/cairo/cairo_backend.cpp` |
| P2 | **CG** on macOS + iOS (`setNeedsDisplayInRect:`, `dirtyRect`) | `hosts/crossplatform/platform_{macos,ios}.mm`, `backends/cg/cg_backend.mm` |
| P3 | Convert the funnels (`repaint()`, `invalidate_widget_visual`), move/resize double damage, overflow margins, compound bounds margin | `hosts/crossplatform/{host.cpp,widgets.cpp}`, `hosts/shared/widget_paint_compound.h` |
| P4 | **D2D** persistent canvas + `Present1` dirty rects (optional) | `backends/d2d/d2d_backend.cpp`, `hosts/crossplatform/platform_win32.cpp` |

Sizing: about 700-1000 lines across 3 OSes; P1 + P3 carry most of the risk.

## Verification

- **Tier-1:** `tests/test_damage.cpp` covers coalescing, the cap, the 60%
  fallback, outward DPI snapping, and section-scroll rect mapping against the
  paint walk's own math.
- **Measurement:** add a many-knob example (or a mode in an existing one)
  with simulated automation that writes `NEUI_PARAM_VALUE` on N knobs per
  tick. Log `paint_frame` time and blitted pixel count before and after, on
  Cairo and CG.
- **Visual regression pass** over the existing examples:
  - focus ring moving between widgets;
  - combo overlay open and flipped above;
  - toast;
  - scrolling SECTION with kinetic bounce;
  - GRID smooth scroll;
  - in-frame menubar on Linux;
  - DPI 1x / 1.5x / 2x.
- **Debug "flash damage" mode:** tints each frame's damage rects for one frame
  to make over- and under-invalidation visible.
