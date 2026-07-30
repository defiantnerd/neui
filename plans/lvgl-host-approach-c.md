# Handoff: neui-on-LVGL host - Approach C prototype & evaluation

Status: **EXECUTED 2026-07-30** - all milestones (M0-M3) done; results appended at the
bottom of this file. This was a **prototype + evaluation**, not production.
Audience: a fresh Claude instance that will implement it end-to-end.

## Goal (what "done" means for this handoff)

Get neui's **crossplatform (xpl) host** rendering through **LVGL** using the retained-`lv_obj`
approach (Option C), **running on Windows on this dev machine**, so we can measure real FPS/CPU
and refine the performance estimate. The concrete deliverable is a running `neui_lvgl_example`
plus reported measurements (see Milestone 3). It does not need to be complete or embedded-ready.

## How to work (important)

- **Ask, don't guess.** When you reach an item in "Open questions" below (or any other real
  fork), stop and ask the user with AskUserQuestion. Ask early rather than assuming.
- **Read first, in order:** this repo's `CLAUDE.md` (auto-loaded); **`lvgl.txt` section 1** (the
  primitive-by-primitive backend spec + the D/W/G gaps - this is the real spec for the backend
  work) and **section 6** (why Option C); `docs/rendering-and-assets.md` (the
  `neui_render_backend_t` contract); `docs/host-linux.md` (the closest platform model - it draws
  its own chrome); `plans/lvgl-port.md` (feature comparison + impedance mismatches);
  `include/neui/d/renderer.h` and `painter.h` (the interfaces you implement/consume).
- **Measure in Release / RelWithDebInfo, not Debug** - Debug FPS is meaningless for estimation.
- Keep the build warning-clean (MSVC `/W4`, `C4100` suppressed), per `CLAUDE.md`.

## Why Option C (brief; full detail in lvgl.txt sec 2 + 6)

neui's xpl host is redraw-the-world immediate mode - whole-window invalidation, full surface
clear, full recursive tree walk, no dirty-rect, no widget cache - which is the dominant cost on
constrained hardware. Option C backs each neui widget with a **passive** `lv_obj` so LVGL owns
invalidation / dirty-rect / compositing while neui still owns widget logic, input, and pixels
(each object's draw event calls neui's existing per-widget paint). This prototype validates the
mechanism and measures it. (Rejected alternatives: A = one flat canvas, neui keeps whole-window
redraw, inherits the problem; B = map neui widgets onto `lv_button`/`lv_slider`/..., which
discards neui's widget set and hits severe feature gaps.)

## Architecture - get this framing right

LVGL is **not** a new registered host. It is three things:
1. a new backend `backends/lvgl/` -> `neui-backend-lvgl` implementing `neui_render_backend_t`;
2. a new xpl platform `hosts/crossplatform/platform_lvgl.cpp` implementing `xpl_host::*`;
3. **conditional retained-mode logic inside the shared xpl host** (`host.cpp`), active only on the
   LVGL platform.

The registered host stays `neui.host.crossplatform` (`neui_register_xplhost`). Backend + platform
are paired per build, exactly like Linux = `neui-backend-cairo` + `platform_linux.cpp`. Do not
invent a new host-registry id.

## Build / CMake requirements (from the user)

- Gate everything behind a CMake option: `option(NEUI_WITH_LVGL "Build the LVGL host + backend" OFF)`.
- When ON, pull LVGL via **FetchContent from git main** (`GIT_REPOSITORY https://github.com/lvgl/lvgl.git`,
  `GIT_TAG master`); version pin is deferred - note it as a follow-up. Provide a repo-local
  `lv_conf.h` (via `LV_CONF_PATH` or `LV_CONF_INCLUDE_SIMPLE`) enabling: a scalable font engine
  (see Open questions), `LV_USE_PERF_MONITOR`, and the Windows display driver.
- **Configuration must FAIL (`message(FATAL_ERROR ...)`) if the LVGL host cannot run on the build
  platform** - no silent fallback. On this Windows machine it must succeed.
- Windows display + input: prefer LVGL's native Windows backend (`LV_USE_WINDOWS`) to avoid an SDL
  dependency (SDL2 is the fallback - ask if you'd rather). Drive `lv_tick` + `lv_timer_handler`.
- Add a prototype example target `neui_lvgl_example` that builds a representative measurement
  screen (Milestone 3) and uses the crossplatform host (select via
  `neui_get_api("neui.host.crossplatform")`, or link only the xpl host in this target). Existing
  builds must be unaffected when `NEUI_WITH_LVGL=OFF` (the default).

## The core mechanism (Phase 3) - the #1 risk, spike it FIRST

**M0 spike (throwaway):** prove neui's backend can render into an LVGL per-object draw event.
In an `lv_obj`'s draw event (v9: `LV_EVENT_DRAW_MAIN`, get the target via `lv_event_get_layer(e)`
-> `lv_layer_t*`; verify against current main), wrap that layer as a `neui_render_ctx` and have
`neui-backend-lvgl` issue a `fill_rect` + `draw_text` at the object's coords. If clean, C is
viable. If not, **ask the user** before proceeding (documented fallback: draw all widgets into one
full-screen canvas = Approach A, still useful for estimation).

Then build the retained layer:
- **Mirror tree:** each `WidgetData` gets a matching `lv_obj` (store the handle on `WidgetData` or
  a side map keyed by widget id). Parent obj = parent widget's obj; z-order follows sibling order
  (already matches neui's paint + hit-test order). Lifecycle choke points: `w_create` ->
  `Tree::add_child` and `w_destroy` -> `Tree::remove` (`widgets.cpp:120-161, 166, 247`).
- **Per-object draw:** on the widget obj's draw event, bind the ctx to the event layer, set the
  palette override, translate to the object's content origin, and call
  `wd.paint(backend, ctx, is_focused)` then `paint_after_children`. Precedent: `CustomDrawWidget::paint`
  already isolates via `translate(x,y)` + `push_clip` + a `neui_painter` (`host.cpp:2813-2855`).
- **Move parent-applied mechanics** out of `paint_widgets_recursive`/`paint_frame` onto the obj
  layer: SECTION clip+scroll (`host.cpp:2133-2165`), disabled-dim alpha (`2107-2110`), the
  `paint_after_children` compound pass (`2161-2163`). Keep abs-coord recompute (`2092-2093`) - it
  is still needed for hit-testing.
- **Per-widget invalidation:** reroute `w_invalidate` (`widgets.cpp:692-703`) and
  `WidgetData::repaint()` (`host.cpp:1464-1470`) - today both collapse to whole-window
  `platform_invalidate` - to `lv_obj_invalidate(<that widget's obj>)` on the LVGL platform.
- **Geometry/visibility:** hook `w_set_pos`/`w_set_size`/`w_show`/`w_hide` (`widgets.cpp:382-410,
  294/368`) plus the ad-hoc mutators (`TabViewWidget::apply_page_geometry` `host.cpp:1279-1282`;
  the Windows WM_SIZE resize path) to move/resize/show the mirror obj. Reparenting: none exists
  (`Tree` has add/remove only) - destroy+recreate is fine for the prototype.
- **Overlays as LVGL top-layer objects**, not per-widget canvases: combo drop
  (`host.cpp:3635-3665`), popup menu (`3918-...`), toast (`4869-...`). In-frame menubar
  (`4301-...`) is Linux-only; on Windows the native menu path is used, so it is out of scope here.
  Modal dialogs already map to separate native windows.
- **Input stays neui's.** Keep `widget_at` / `dispatch_mouse_event` (`host.cpp:483-533,
  2329-2351`); the LVGL indev feeds frame-local pointer/keys into `platform_lvgl`, which calls the
  existing session methods. Keep the `lv_obj`s passive - do **not** enable LVGL's own
  widget input/focus/scroll behaviours on them (except where you deliberately use obj scroll for
  SECTION).

## Backend scope for the prototype (lvgl.txt sec 1 is the full spec)

- **Needed to render the example:** begin/end_frame + clear, `fill_rect`/`draw_rect`,
  `draw_text`/`measure_text`, `push/pop_clip`, `push/pop_alpha`, `push/pop_font`, `draw_bitmap`,
  and the **path API** (`begin_path`/`move_to`/`line_to`/`arc`/`close_path`/`fill_path`/`stroke_path`)
  which the KNOB needs. `get_scale_factor`/`update_dpi` can return a fixed scale.
- **Text** needs a scalable font engine for arbitrary sizes (Open question: FreeType vs Tiny TTF).
- **Paths:** KNOB arcs must render for a meaningful measurement. A small scanline rasteriser or
  LVGL's own arc/line primitives may suffice for the prototype; ThorVG/VGLite is the production
  path (Open question). A minimal `fill_path` stub is OK to get first pixels, but land real KNOB
  arcs before Milestone 3.
- **Defer/stub:** offscreen surfaces, gradients, font registration, the filter graph. Compile out
  DnD/clipboard/IME.

## Milestones (each independently verifiable)

- **M0 - Spike:** per-object draw-into-event bridge proven (throwaway code).
- **M1 - Bring-up / baseline:** `NEUI_WITH_LVGL=ON` configures + builds on Windows; neui paints
  the whole frame into a single LVGL surface (de-facto Approach A) in a window with working mouse
  + keyboard. This is the measurement **baseline**.
- **M2 - Option C:** retained `lv_obj` per widget + per-widget invalidation + overlays on the top
  layer. Verify a hover / knob-drag invalidates **only that object's rect** (use
  `LV_USE_REFR_DEBUG` / the perf monitor, or log invalidated area); an idle screen = **0 repaints**.
- **M3 - Evaluation:** `neui_lvgl_example` renders a representative screen (buttons, labels, a
  SECTION, an INPUTBOX, one or two KNOBs, a block of text, optionally a small GRID) with
  `LV_USE_PERF_MONITOR` on. Record FPS + CPU for (a) idle, (b) a knob drag, (c) a full-screen
  change, for **both M1 (baseline) and M2 (Option C)**, in Release/RelWithDebInfo. Report the
  numbers back so we can refine the estimate.

## Verification

- Configure fails cleanly with `NEUI_WITH_LVGL=ON` on an unsupported platform; succeeds here.
- `neui_lvgl_example` runs, shows a window, responds to mouse + keyboard.
- M2 dirty-rect proof: idle = 0 repaints; hover/knob-drag repaints only that widget's rect.
- Tier-1 header tests (`neui_tests`) still pass; win32/macOS/Linux builds unaffected with
  `NEUI_WITH_LVGL=OFF`.

## Open questions - ASK the user when you reach these (do not guess)

1. **Font engine:** FreeType vs Tiny TTF.
2. **Path fill:** scanline rasteriser vs LVGL vector (ThorVG) for the prototype (VGLite is later /
   embedded-only).
3. **Windows LVGL driver:** native `LV_USE_WINDOWS` (no SDL) vs SDL2. (Recommend native Windows.)
4. **Framebuffer depth for measurement:** XRGB8888 (matches desktop) vs RGB565 (matches MCU), or
   measure both.
5. **If the M0 spike shows per-object draw-into-event is impractical:** fall back to Approach A for
   the prototype, or rethink?
6. **Representative screen:** which widgets/layout best mirror the real product, so the measured
   numbers transfer to the estimate?

---

## RESULTS (executed 2026-07-30)

Open questions were resolved with the user before implementation: **Tiny TTF** (font engine),
**ThorVG** via `LV_USE_VECTOR_GRAPHIC` (path fill), **native `LV_USE_WINDOWS`** driver (no SDL),
**XRGB8888**, and a **knob-heavy audio panel** as the M3 screen. LVGL fetched from git master
(9.6.0-dev, commit 066d8db0, 2026-07-30) - version pin remains a follow-up.

### What was built

- `backends/lvgl/` - `neui-backend-lvgl`: full `neui_render_backend_t` over LVGL draw tasks.
  Rects/borders via `lv_draw_fill`/`lv_draw_border`; text via `lv_draw_label` over per-(file,
  size) Tiny TTF instances resolved from `C:\Windows\Fonts` by family+weight (registration API
  stubbed); the whole path model (arcs flattened to cubics, fill rules, styled strokes,
  linear/radial gradients on fill+stroke) via the ThorVG vector pipeline, with consecutive
  path ops batched into a single vector task (see the RGB565 findings); clip stack by
  save/intersect/restore of `layer->_clip_area`; SW 2x3 CTM (axis-aligned fast path, general
  affine through the vector matrix); bitmaps as `ARGB8888_PREMULTIPLIED` image dscs (sub-rect
  draws via a per-ctx arena that outlives the deferred draw tasks). Off-screen surfaces return
  null (SURFACE assets degrade to `asset_none`), so the filter graph is unreachable - per plan.
- `hosts/crossplatform/platform_lvgl.cpp` - one LVGL display (own Win32 window+thread, driver-
  managed) per neui frame; input captured by subclassing the driver HWND and queueing raw
  messages to the main thread (the driver thread takes `lv_lock()` in its WndProc, so Session
  calls must stay on the `lv_timer_handler` thread); drain replicates platform_win32's dispatch
  (hit-test -> hover -> focus/pressed -> events, popup/toast/combo hooks, Tab traversal,
  surrogate assembly, synthesized double-click - the LVGL window class lacks CS_DBLCLKS).
  Clipboard/DnD/IME/menubar/message-box are stubs per plan; images decode via stb_image.
- **Option C retained layer** (runtime switch `NEUI_LVGL_RETAINED`, default ON; `0` = the M1
  whole-frame baseline in the same binary): a passive `lv_obj` mirror per widget, synced from
  the widget tree by a dirty-flag walk that also maintains `abs_x/abs_y` (hit-testing) and gives
  SECTION/TABVIEW a clipped body container obj (children positioned body-relative minus scroll).
  Mirrors draw via `LV_EVENT_DRAW_MAIN` (`Session::paint_widget_retained` - palette bracket +
  PREUPDATE + disabled-dim identical to the walk) and `LV_EVENT_DRAW_POST`
  (`paint_after_children`); the screen obj paints frame bg below and overlays (combo drop, popup
  menu, toast) above everything. Per-widget invalidation is routed through two `#ifdef
  NEUI_PLATFORM_LVGL` seams (`platform_retained_widget_invalidate` / `_tree_changed`) called
  from `WidgetData::repaint`, set_focus/hovered/pressed, set_text/set_asset/attr setters,
  w_invalidate, and the structural mutators (create/destroy/show/hide/set_pos/set_size,
  tab-page reflow, section scroll). Invalidations arriving inside a draw dispatch are deferred
  (LVGL forbids invalidating while rendering) and flushed after `lv_timer_handler`.
- `neui_lvgl_example` (`examples/lvgl_example.cpp`) - the measurement screen: 8 KNOBs with live
  value labels in a SECTION, channel buttons, checkbox, inputbox, text block; 'S' toggles a
  full-screen all-knobs animation driven from WIDGET_PREUPDATE.

### Milestone outcomes

- **M0 spike: PASS.** Per-object draw-into-event works; deferred draw tasks require copying
  transient data (`text_local=1`; vector paths are deep-copied at `add_path`); ThorVG arcs and
  `_clip_area` clipping work inside draw events; `lv_obj_invalidate` bounds the dirty AREA to
  the object (a disjoint sibling never repainted). Note: LVGL is not a retained pixel cache -
  ancestors overlapping the dirty rect re-issue draw tasks clipped to it (correct compositing).
- **M1: PASS.** Whole-frame Approach A renders the full panel through LVGL; real mouse (drag,
  click, hover), keyboard (typing into INPUTBOX incl. caret), and clean APP_QUIT close verified.
- **M2: PASS.** Pixel-parity with M1. Idle = 0 redraws; a knob drag repaints only the knob +
  label rects; the KNOB right-click "Reset to default" popup renders above the mirrors, its
  nested modal pump runs, and the item click resets the value.
- **M3 measurements** below.

### M3 measurements

RelWithDebInfo, 800x480, LVGL SW renderer + ThorVG, `LV_DEF_REFR_PERIOD 16` (~60 FPS cap; the
pump waits in 10 ms slices, so ~45 FPS is the practical ceiling), Windows 11 ARM64
(Snapdragon-class desktop core). Numbers from `LV_USE_PERF_MONITOR` LOG_MODE; `render` is the
average per-refresh render time - the platform-transferable figure. Framebuffer depth is a
configure option (`-DNEUI_LVGL_COLOR_DEPTH=32|16`, template `backends/lvgl/lv_conf.h.in`); both
depths were measured with the same binary layout and the same screen (8 KNOBs + labels +
buttons + INPUTBOX + one IMAGE widget + font-check labels), with the backend's vector-task
batching (below) in place.

**XRGB8888 (LV_COLOR_DEPTH 32):**

| Scenario                      | M1 baseline (whole-frame)     | M2 Option C (retained)      |
|-------------------------------|-------------------------------|-----------------------------|
| (a) idle                      | 0 redraws, render 0 ms        | 0 redraws, render 0 ms      |
| (b) knob drag (rotational)    | render ~21 ms, CPU ~67%       | render **~1.4 ms**, CPU ~31% |
| (c) full-screen (8-knob anim) | render ~15 ms, ~54 FPS        | render ~16 ms, ~54 FPS      |

**RGB565 (LV_COLOR_DEPTH 16):**

| Scenario                      | M1 baseline (whole-frame)     | M2 Option C (retained)      |
|-------------------------------|-------------------------------|-----------------------------|
| (a) idle                      | 0 redraws, render 0 ms        | 0 redraws, render 0 ms      |
| (b) knob drag (rotational)    | render ~31 ms, CPU ~79%       | render **~3.0 ms**, CPU ~31% |
| (c) full-screen (8-knob anim) | render ~25 ms, ~36 FPS        | render ~26 ms, ~34 FPS      |

Reading of the numbers:

- **The Option C mechanism does what it exists to do at both depths**: a local interaction
  costs the widget's rect, not the screen - drag render is ~15x cheaper than the whole-frame
  baseline at 32 bpp and ~10x at 565. The dirty AREA ratio is ~26x (knob+label ~15 k px^2 vs
  384 k px^2); per-refresh fixed overhead dominates at small areas on a fast desktop CPU, and
  on an MCU where per-pixel fill is the bottleneck the win scales toward the area ratio. This
  is the bound that makes 200 MHz-tier local interactions tractable (lvgl.txt sec 3).
- **Full-screen cost is mode-independent** at both depths, as expected - Option C bounds what
  must repaint; it cannot reduce the price of genuinely repainting everything.
- **Idle is free in both modes and both depths** (0 redraws).
- **RGB565's real cost is the vector fallback, not the pixel format.** LVGL's SW vector path
  (`lv_draw_sw_vector.c`) renders ThorVG only into ARGB8888/XRGB8888 targets; on any other
  format EVERY vector draw task allocates, clears, renders into and blends down a temporary
  ARGB8888 buffer sized to the LAYER - and in direct render mode the layer is the full
  framebuffer, so even a knob-sized repaint pays an 800x480 round-trip per task. Measured
  before mitigation: 565 full-frame ~52 ms, 565 knob drag 10-17 ms.
- **Backend mitigation (implemented): vector-task batching.** The backend now coalesces
  consecutive `fill_path` / `stroke_path` calls into ONE `lv_draw_vector` task (per-path
  fill/stroke state set before each `add_path`; flushed at any non-path draw, clip change, or
  end of dispatch, preserving z-order and clip semantics). A KNOB paint becomes 1 vector task
  instead of ~6. Result: 565 full-frame ~52 -> ~25 ms, 565 knob drag ~10-17 -> ~3 ms. 32 bpp is
  largely indifferent (no temp buffer on that path).
- The remaining 565-vs-32 drag gap (~3.0 vs ~1.4 ms) is the one leftover per-task full-
  framebuffer round-trip; it would shrink if LVGL sized the temp buffer to the task clip
  (upstream improvement opportunity), and disappears entirely on VGLite-class hardware - which
  remains the production answer for path chrome on 565 targets (lvgl.txt sec 5). Text / rects /
  images are unaffected by the 565 fallback, and the flush is ~2x cheaper (half the bytes).
- **IMAGE widget cost (measured by adding one to the screen)**: an LVGL SW downscale blit of a
  500x375 ARGB source into a 96x72 box costs ~10 ms per full-frame repaint at 32 bpp (stress
  drops 16 -> ~5 ms with the image hidden; the pre-image tables measured ~6 ms full-frame).
  Scaled image draws are per-frame transform work in LVGL's SW renderer - production wants
  pre-scaled / cached blits (part of the appearance-cache recommendation below). Option C
  already avoids the cost for local interactions (the image repaints only when its rect is
  touched).
- Anchor for the tier table in lvgl.txt: a knob-heavy 800x480 full frame through the LVGL SW
  renderer + ThorVG costs ~5-6 ms at 32 bpp on this desktop-class ARM core (~15 ms with a
  naively-scaled photo on screen); ~25 ms at RGB565 due to the vector fallback. The M1-vs-M2
  *ratios* (not the absolute times) are the transferable result.

### Post-evaluation verification additions (same session)

- **IMAGE widget verified** (`neui_lvgl_example` bottom strip): myimage.png (500x375) drawn
  into an exactly-4:3 96x72 box fills it edge to edge with correct colours + alpha - the
  backend's `draw_bitmap` scaling and premultiplied-BGRA conversion are correct. This test
  caught a real retained-mode bug: per-draw sub-image descriptors were arena-freed at the next
  `bind_layer`, but retained mode binds per WIDGET while LVGL's deferred draw tasks from the
  same refresh still reference them (use-after-free -> noise). Fixed by freeing them only
  after the refresh completes (`neui_lvgl_backend::collect_deferred`, called from the
  platform's loop turn after `lv_timer_handler`).
- **Font sizes verified em-accurate**: labels at NEUI_ATTR_FONT_SIZE 12/16/22/32 measure ink
  (cap) heights of 8/11/15/24 px against the Segoe UI cap-height expectation of 0.70 em =
  8.4/11.2/15.4/22.4 px (+-1 px AA fringe) - Tiny TTF's `ScaleForMappingEmToPixels` matches
  the DirectWrite em-size semantics. `NEUI_ATTR_FONT_FAMILY` resolves (Consolas renders
  visibly monospaced). Caveat: Tiny TTF instances are cached per integer pixel size, so
  fractional neui sizes quantize to <=0.5 px.

### Known prototype limitations (deliberate)

- Windows-only; configure hard-fails elsewhere (`NEUI_WITH_LVGL` + `message(FATAL_ERROR)`).
- Closing a frame hides its window instead of destroying it - the LVGL Windows driver's display
  watchdog `exit(0)`s the process when the last display dies mid-loop. Fine for the prototype;
  a production port would run its own display driver (as an embedded target would anyway).
- Clipboard / DnD / IME / native menubar / message boxes stubbed; off-screen surfaces (SURFACE /
  filter graph) unavailable; font registration API returns false (family names resolve from the
  Windows fonts directory instead); no DPI scaling (logical px == LVGL px == physical px);
  dialogs are resizable; smooth-scroll kinetics not wired (stepped wheel only).
- Overlay changes (combo/popup/toast) still invalidate the whole frame - transient, acceptable.
- LVGL pinned to `master` at fetch time - **pin a release tag before any further work**.
- Synthetic-input caveat for future test automation: PostMessage'd mouse input to the driver
  window is unreliable; SendMessageTimeout works (subclass runs either way for real input).

### Suggested next steps (if the direction is pursued)

1. Pin LVGL to a tagged release; move the retained layer from prototype to reviewed design
   (reparenting, mid-order sibling inserts, mirror teardown on window destroy).
2. Per-widget appearance cache (render-once-to-image for static-but-expensive chrome) - the
   biggest remaining MCU win per lvgl.txt sec 7.
3. An embedded display driver (fbdev / vendor flush_cb) replacing the Windows driver, and a
   VGLite draw-unit path for the vector half on RT1176-class silicon.
4. Wire smooth-scroll kinetics (the shared `scroll_kinetics` on a 16 ms lv_timer) and the
   remaining stubs as needed by the product.
