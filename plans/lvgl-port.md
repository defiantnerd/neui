# Investigation: neui on LVGL as a host

## Context

This is a **feasibility investigation**, not an implementation. The question: can neui
(the C/C++ GUI framework in this repo) use LVGL (https://github.com/lvgl/lvgl) as a host,
and where do the two architectures clash? The user asked for a thorough feature comparison
and an impedance-mismatch analysis covering all viable approaches plus a recommendation.

The deliverable is this report. No code changes are proposed for now; a follow-up
implementation plan would be written only if the user decides to proceed.

---

## TL;DR

**Feasible: yes - via one specific approach.** neui's crossplatform (xpl) host already
draws *every* widget itself through an abstract `neui_render_backend_t` (no OS controls).
That makes LVGL viable as a **rendering + display substrate**: write a new backend
(`neui-backend-lvgl`) that rasterises via LVGL's draw/canvas API, plus a `platform_lvgl.cpp`
that owns an `lv_display_t` + `lv_indev_t` + the `lv_timer_handler` tick loop. The xpl host,
all of neui's widgets, and neui's existing "draw-my-own-chrome" Linux path are reused wholesale.

**Not recommended: mapping neui widgets onto LVGL's native widgets** (`lv_button`,
`lv_slider`, ...) the way the win32/macOS native hosts wrap OS controls. That is a large
parallel-host rewrite with serious feature gaps (no real tree, no desktop GRID, weak text
editing, no DnD/clipboard) and it throws away the xpl host that already does the job.

**Main costs of the recommended approach** (details below): a software transform stack in
the backend; filled paths/arcs need LVGL's vector module (ThorVG) or a custom rasteriser;
scalable text needs FreeType or Tiny TTF (not the compiled bitmap fonts); single top-level
"window" only; clipboard/DnD/IME degrade to stubs (already graceful in neui's design); input
is full on the SDL desktop simulator but degrades on touch hardware (no hover/right-click/wheel).

**Licensing: no conflict** - LVGL is MIT.

---

## The two architectures, and why it matters

neui has two host *styles* already in-tree:

1. **Native hosts** (`hosts/win32`, `hosts/macos`): each neui widget wraps an OS control
   (HWND `Button`, `NSPopUpButton`, ...). Thin where the OS has the widget; bespoke where it
   doesn't (KNOB, GRID are painted even here).
2. **Crossplatform host** (`hosts/crossplatform`): a polymorphic `WidgetData` tree where
   every widget subclass paints itself via `neui_render_backend_t` (D2D / CoreGraphics /
   Cairo / null). The platform layer (`hosts/crossplatform/platform.h`) supplies only
   window creation, the event pump, clipboard, DnD, menubar, IME, cursor, image decode.

Because the xpl host renders everything itself, LVGL can plug in at **two very different depths**:

| | **Approach A: LVGL as substrate** (recommended) | **Approach B: map to LVGL widgets** |
|---|---|---|
| What you write | a backend (`neui_render_backend_t`) + a `platform_lvgl.cpp` | a whole new native host (`host.cpp` + `widgets.cpp` equivalents) |
| Reuses xpl host? | yes, entirely | no |
| neui widget set | kept as-is (neui paints them) | re-expressed as LVGL objects |
| LVGL used for | its rasteriser + display drivers + input + tick loop | its retained widget tree, styling, layout |
| Effort | ~2 new files, well-bounded by existing seams | comparable to win32+macOS hosts combined |
| Feature parity | high (neui owns behaviour) | low (gated by LVGL's widgets) |

Approach B is where LVGL's *widgets* live, but neui already has richer widgets, so B trades
neui's strengths for LVGL's and incurs the most code. **The rest of this report assumes
Approach A** and treats B only in the feature-comparison table.

---

## What a host must implement (the seams)

### Render backend - `include/neui/d/renderer.h` (`neui_render_backend_t`, ~39 fn ptrs)
Mandatory groups, all stub-able to no-op (see `backends/null/`):
- Context lifecycle: `create_context(native_handle,w,h)`, `destroy_context`, `resize`,
  `begin_frame(clear_argb)`, `end_frame`.
- DPI: `get_scale_factor`, `update_dpi`.
- Primitives: `fill_rect`, `draw_rect`, `draw_text`, `measure_text`.
- Stacks: `push_clip/pop_clip` (nested AABB), `push_transform/pop_transform` +
  `translate/rotate/scale` (post-multiply, Y-down), `push_alpha/pop_alpha` (cumulative 0..1),
  `push_font/pop_font` (family+weight; size per-call).
- Path API: `begin_path/move_to/line_to/arc/close_path/fill_path/stroke_path` (radians, Y-down CW).
- Bitmaps (host-internal): `create_bitmap(BGRA8 premul)/destroy_bitmap/draw_bitmap(+tint)`.
- Offscreen (SURFACE asset): `create_offscreen_context`, `read_pixels_bgra` (may return null/false).
- Font registration (factory-level): `register_font(bytes)/register_font_file/unregister_font`
  - may return false.
- Device loss: `get_context_generation` (constant 0 for software).

Coordinates are logical px @ 96 DPI; colour is `0xAARRGGBB`. Stacks reset at every `begin_frame`.

### Platform layer - `hosts/crossplatform/platform.h` (`xpl_host::*`, ~50 fns)
The newest full port is `platform_linux.cpp` (X11+Cairo) - the closest model, since Linux
already draws its own menubar, message box, popups, and modal dialogs. Key seams:
- Init/backend: `platform_init`, `platform_get_backend`.
- Windows: `platform_create_appwindow/_plugwindow/_dialog`, `platform_destroy_window`,
  show/hide/enable/activate/title/pos/post_close.
- Invalidation + animation: `platform_invalidate`, `platform_start/stop_toast_animation`,
  `platform_now_ms` (the 16 ms heartbeat that drives scroll-bounce + toast).
- Loop: `platform_run` (blocks until all appwindows close), `platform_pump_once`,
  `platform_run_modal_until(bool*)` (nested pump for popups/modal dialogs).
- Services: clipboard (text + item), DnD (register/begin/preview), menubar (or
  `platform_menubar_in_frame()==true` to draw it in-frame, as Linux does), `platform_message_box`,
  `platform_load_image`/`platform_free_image`, `platform_set_cursor`, size constraints, icon.

### Host registration - `include/neui/neui.h`, `include/neui/d/api.h`
Implement `neui_api_t { create_session, destroy, get_interface, run, endsession, pump_once }`
and call `neui_register(NEUI_HOST_LVGL, &api)`. The xpl host's `register_host()`
(`hosts/crossplatform/host.cpp`) is the template: it calls `platform_init()` then registers.
The `Session` object + widget tree are provided by the xpl host - a new platform target adds
no widget code.

---

## LVGL profile (what it gives us)

- **What it is**: MIT-licensed, pure-C embedded graphics library (C++-compatible). Software
  renderer into draw buffers; optional GPU backends (OpenGL ES, VG-Lite, ThorVG, SDL). Runs on
  MCUs, embedded Linux (fbdev/DRM/Wayland/X11), and a desktop SDL simulator. Color formats
  RGB565 / RGB888 / XRGB8888 / ARGB8888 / L8 / I1.
- **Display** (`lv_display_t`): renders into a draw buffer, then calls a user `flush_cb` to push
  pixels to hardware. It does **not** create OS windows - the porting layer (SDL/X11/fbdev) does.
- **Input** (`lv_indev_t`): pointer / keypad / encoder / button, via a polled `read_cb`.
- **Loop**: cooperative single-thread. `lv_tick_inc(ms)` feeds time; `lv_timer_handler()` (called
  ~every 5 ms) drives refresh, input, animations, timers. Invalidation is dirty-rect based.
- **Draw API**: `lv_draw_rect/label/image/line/arc/triangle`, plus a vector layer (`lv_draw_vector`
  / ThorVG) when enabled. Custom drawing via `lv_canvas` or `LV_EVENT_DRAW_*` events.
- **Fonts**: `lv_font_t` objects; built-ins are compiled bitmap fonts at fixed sizes. Scalable
  runtime fonts via `LV_USE_FREETYPE` or `LV_USE_TINY_TTF`. Measure via `lv_text_get_size`.
- **Threading**: not thread-safe; all LVGL calls (incl. `lv_timer_handler`) must be serialized
  (`lv_lock`/`lv_unlock`). Single-threaded use avoids this entirely.
- **Widgets** (for Approach B comparison): label, button, checkbox, switch, slider, bar, arc,
  dropdown, roller, textarea, list, table, tabview, msgbox, menu, calendar, chart, canvas, image,
  keyboard, spinbox, etc. No tree widget; table is paint-only (no editing/sort); menu is not a
  desktop menubar.

---

## Feature comparison (neui widget/capability -> LVGL)

Legend: ✅ direct, 🟡 workable with effort, 🔴 gap/hand-roll.

### Under Approach A (neui paints; LVGL only rasterises) - parity is high
Every neui widget keeps working because neui draws it; the only question is whether the
**backend primitives** it uses are expressible on LVGL:

| neui primitive | LVGL mapping | Notes |
|---|---|---|
| `fill_rect` / `draw_rect` | `lv_draw_rect` | ✅ incl. rounded via radius |
| `draw_text` / `measure_text` | `lv_draw_label` / `lv_text_get_size` | 🟡 needs scalable font engine (below) |
| `draw_bitmap` (+tint) | `lv_draw_image` (recolor/opa) | ✅ tint via recolor; BGRA<->LVGL color convert |
| clip stack (nested AABB) | per-draw clip area, stack kept in backend | ✅ matches LVGL's AABB-only clip |
| alpha stack | descriptor `opa`, cumulative in backend | ✅ |
| transform stack (translate) | apply in software before `lv_draw_*` | 🟡 backend maintains CTM |
| transform (rotate / scale) | image rotate/scale only; geometry must be pre-transformed | 🔴 rotated text/rects limited (see mismatch #1) |
| path: line/arc/fill/stroke | `lv_draw_line` / `lv_draw_arc`; arbitrary fill via ThorVG | 🔴 generic `fill_path` needs vector module (#2) |
| offscreen + read_pixels (SURFACE) | `lv_canvas` buffer + read | ✅ |
| font registration by family name | backend name->`lv_font_t*` map | 🟡 LVGL has no font registry (#3) |
| device-loss generation | constant 0 | ✅ |

### Under Approach B (neui widget -> LVGL widget) - large gaps
| neui widget | LVGL equivalent | Verdict |
|---|---|---|
| LABEL / BUTTON / CHECKBOX / SLIDER / IMAGE | label / button / checkbox / slider / image | ✅ direct |
| CHECKBOX3 (tristate) | checkbox (2-state) | 🟡 custom state |
| LISTBOX / COMBOBOX | list / dropdown | 🟡 selection-model differences |
| INPUTBOX / MULTILINE | textarea | 🔴 neui has caret/selection/undo/IME/word-nav/clipboard; LVGL textarea is basic |
| TREEVIEW | (none) | 🔴 no tree widget |
| GRID (sortable, editable, multi-col) | table | 🔴 table is paint-only, no edit/sort |
| KNOB (rotary, drag modes, reset popup) | arc | 🔴 arc is display-only |
| MENUBAR / popup menu | menu | 🔴 not a desktop menubar |
| SECTION (scroll + chip + kinetics) | obj + flex/scroll | 🟡 partial |
| TABVIEW / TABPAGE | tabview | ✅ direct |
| CUSTOMDRAW + compound/behavior assets | canvas + draw events | 🟡 substantial glue |
| APPWINDOW / PLUGWINDOW / DIALOG | screen / top-layer | 🔴 single-display model (#4) |

### Capabilities (both approaches)
| Capability | LVGL | Verdict |
|---|---|---|
| Clipboard | none | 🔴 stub (or platform-specific on SDL/Linux) |
| Drag & drop | none | 🔴 unsupported or hand-rolled in-frame |
| IME / composition | none | 🔴 unsupported |
| Native menubar | menu (not desktop) | 🟡 use neui's in-frame menubar (Linux precedent) |
| Message box | msgbox | 🟡 or neui-drawn (Linux precedent) |
| Modal dialog (nested pump) | no OS modality | 🟡 nested `lv_timer_handler` loop (#5 reentrancy) |
| Multiple top-level windows | one display (multi via screens) | 🔴 single-window (#4) |
| Cursor shapes | pointer cursor image | 🟡 limited |
| HiDPI / per-monitor DPI | no DPI concept | 🟡 scale = 1.0; size in `lv_conf` |

---

## Impedance mismatches (ranked)

1. **No global transform stack in LVGL.** neui's backend assumes a push/pop affine CTM that
   affects *all* primitives. LVGL's `lv_draw_*` take absolute coords; only images rotate/scale
   natively. **Fix:** maintain the CTM in the backend and pre-transform geometry before each
   `lv_draw_*`. **Residual gap:** rotated/scaled *text and rects* are not natively supported -
   acceptable because neui uses rotation mainly for IMAGE (image rotate exists) and KNOB
   (drawn via arcs); rotated text is rare. Worst case: render-to-canvas then rotate the canvas.

2. **Filled arbitrary paths/arcs.** neui's path API (`fill_path`/`stroke_path`, used by KNOB
   arcs, rounded-rect/compound PATH layers, GRID/scrollbar geometry) maps cleanly only for
   lines and simple arcs (`lv_draw_line`/`lv_draw_arc`). Generic polygon fill needs the vector
   module (`LV_USE_VECTOR_GRAPHIC` / ThorVG), which is optional and heavier. **Fix:** require
   ThorVG, or write a small scanline polygon rasteriser in the backend for `fill_path`.

3. **Font model mismatch.** LVGL fonts are `lv_font_t*` objects; built-ins are *compiled bitmap
   fonts at fixed sizes*. neui addresses fonts by **family-name string at arbitrary float px**.
   **Fix:** keep a name->`lv_font_t*` map in the backend and require `LV_USE_FREETYPE` or
   `LV_USE_TINY_TTF` for scalable text (so arbitrary `font_size` works). On an MCU this is a
   real RAM/flash cost; on the SDL sim it is free. `register_font*` maps to FreeType face load.

4. **Single-display windowing.** LVGL has one (sometimes more) display and no OS windows.
   neui's APPWINDOW/PLUGWINDOW/DIALOG assume independent OS windows with their own render
   contexts and quit-count semantics. **Fix:** treat the LVGL display as the single top-level
   "window"; render DIALOG/popups as overlay objects on the top layer (neui already draws these
   in-frame). Multi-window desktop apps are out of scope - fine for the embedded UIs LVGL targets.

5. **Cooperative loop + nested-pump reentrancy.** neui's blocking `platform_run` maps cleanly to
   `while(running){ lv_timer_handler(); delay(5); }`, and the 16 ms heartbeat to an `lv_timer`.
   But `platform_run_modal_until` runs a *nested* pump from inside neui's event dispatch -
   recursively calling `lv_timer_handler` is unsafe. **Fix:** decouple input - buffer LVGL indev
   reads into a neui queue; the nested pump drains that queue + repaints the canvas without
   recursively entering `lv_timer_handler`. neui already drives its own painting, so this is a
   clean split.

6. **Touch-centric input degrades desktop affordances.** LVGL pointer input has no hover, no
   right-click, no mouse wheel on real touch hardware; keypad is group-navigation oriented;
   encoder has no neui analogue. **On the SDL desktop simulator you get full mouse + keyboard.**
   neui's hover highlights, right-click reset popups, and wheel scrolling work on the sim and on
   any platform with a real pointer; they no-op gracefully on bare touch. Map encoder -> wheel /
   focus-nav if needed.

7. **No clipboard / DnD / IME.** All stub out. neui's design already degrades these gracefully
   (the null platform and parts of Linux do the same), so this is "unsupported," not "broken."

---

## Recommended approach (if the user proceeds)

**Approach A: a new backend + a new xpl platform target. Reuse the xpl host.**

Sketch (not a committed plan):
- `backends/lvgl/lvgl_backend.cpp` implementing `neui_render_backend_t`:
  - rects/text/image/clip/alpha straight onto `lv_draw_*` into the display's active layer (or a
    full-screen `lv_canvas`); software CTM; `fill_path` via ThorVG or a scanline rasteriser;
    fonts via a name-map over FreeType/Tiny TTF; offscreen via `lv_canvas`.
- `hosts/crossplatform/platform_lvgl.cpp` implementing `xpl_host::*`:
  - model `platform_linux.cpp` (it already draws menubar/message box/popups/modal in-frame);
    own one `lv_display_t` + `lv_indev_t`; `platform_run` = the LVGL tick loop; heartbeat = an
    `lv_timer`; clipboard/DnD/IME stubbed; cursor via indev cursor image.
- Top-level `CMakeLists.txt` + `hosts/crossplatform/CMakeLists.txt`: gate an LVGL target
  (find/fetch LVGL, define `NEUI_HAS_LVGL`, register via `neui_register_xplhost`-style wrapper).
- Build/validate first against the **LVGL SDL simulator on desktop** (full mouse+keyboard),
  before any MCU/fbdev target.

This keeps neui's entire widget set, behaviours, and self-drawn chrome intact and confines new
code to the two seams neui was explicitly designed to swap.

---

## Verification (for a future implementation)

- **Smoke parity with existing backends:** mirror `tests/` Tier-1 header suite (host-neutral,
  builds everywhere) and add an offscreen render smoke like `neui_cairo_smoke`
  (rect/text/arc/bitmap + `read_pixels_bgra`) for the LVGL backend.
- **Desktop sim run:** build the LVGL SDL simulator target, run `neui_example` /
  `neui_grid_example` / `neui_section_scroll_example`, confirm paint + mouse + keyboard.
- **Font/path coverage:** verify KNOB arcs, rounded rects, and compound PATH layers render
  (exercises mismatch #2) and that arbitrary `NEUI_ATTR_FONT_SIZE` text measures/draws
  (mismatch #3).
- **Nested pump:** open a popup menu and a modal DIALOG; confirm no reentrancy hang (mismatch #5).

---

## Open decisions to resolve before implementing

- LVGL version target (v9 draw/display API assumed here).
- ThorVG dependency (for `fill_path`) vs a bundled scanline rasteriser.
- Font engine: FreeType vs Tiny TTF (memory vs capability).
- First concrete target: SDL sim only, or a specific embedded board.
