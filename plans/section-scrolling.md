# Plan: Scrolling SECTION + chip "none" option

**Status**: phases 0-3 + 5 (example) shipped on all three hosts; the
kinetics opt-in (phase 4) is deferred. macOS native + Win32 native
landed in a follow-up after the v1 xpl-only landing, sharing the same
`SectionScrollState` + `widget_section_scroll.h` runtime; per-host
glue lives in `hosts/win32/widgets.cpp` (`painted_msg_section_w32` +
`section_apply_layout_changes_w32` etc.) and `hosts/macos/window.mm`
(`NEUINativePaintedView` `sectionScrollWheel:` / `sectionBounceTick:` +
`macos_host::section_apply_layout_changes_macos` etc.). Child positions
on the native hosts are scroll-adjusted at `widget_set_pos` /
`apply_geometry_native_macos` time via `parent_scroll_offset_{w32,macos}`
- the body-local (x, y) the client stores stays stable; the native
HWND / NSView frame is the section's scroll subtracted.

## Context

`NEUI_W_SECTION` today is a non-interactive painted band. The xpl `SectionWidget` paints a background fill + optional title chip; native hosts back it with a real container (Win32: a `neui.painted` HWND with a window region; macOS: an `NEUINativePaintedView` recognised as a container by `widget_is_native_container`). Children of a SECTION are native children of the SECTION's HWND / NSView on both native hosts (`hosts/win32/widgets.cpp:1482` recurses with the SECTION's HWND as `parent_hwnd`; `hosts/macos/window.mm:3383-3387` swaps `child_parent` to the SECTION's NSView before recursing). On the xpl host the SECTION is paint-only and the paint walk in `paint_widgets_recursive` (`hosts/crossplatform/host.cpp:1443-1492`) pushes a `translate(wd.x, wd.y)` around descendants.

This is a happy starting point: the parent / child container relationship is already there on every host. The two missing pieces are (a) a scroll offset applied to the children + a scrollbar gutter, and (b) widening the title-chip alignment to include a "none" option so untitled scrolling sections don't reserve a header band.

Outcome: a SECTION can be made scrollable via `NEUI_ATTR_SCROLL_MODE`; children are clipped to the section body; mouse wheel + scrollbar drag move the content; `NEUI_ATTR_ALIGN_TEXT = "none"` hides the chip + skips the header band so the body fills the whole rect; the smooth-scroll math currently locked inside GRID becomes a generic primitive that SECTION reuses.

## Decisions (resolved)

- **Scroll axes**: per-section, attr-driven. `NEUI_ATTR_SCROLL_MODE` accepts `"none"` (default), `"vertical"`, `"horizontal"`, `"both"`. Unknown values fall back to `"none"`.
- **Content extent**: auto from descendants by default; explicit `NEUI_ATTR_CONTENT_WIDTH` / `NEUI_ATTR_CONTENT_HEIGHT` (int, logical px) override when set. `0`/unset = auto. The auto path scans the section's direct widget-tree children and takes `max(child.x + child.width)` / `max(child.y + child.height)`, clamped to at least the visible body extent. SECTION's own band height is excluded from the body.
- **Chip "none"**: `NEUI_ATTR_ALIGN_TEXT = "none"` suppresses the chip + the header band entirely (body fills the whole rect, same as the existing "empty `text`" code path). The other three values (`"left"` / `"center"` / `"right"`) behave as today.
- **Smooth-scroll kinetics**: extracted from `grid_model.h` into a new generic primitive `hosts/shared/scroll_kinetics.h`. GRID + SECTION both call into it. Done as a pre-requisite refactor (see [Phase 0](#phase-0-extract-generic-scroll-kinetics)).
- **Scrollbar visuals**: reuse `hosts/shared/scrollbar.h` (`compute_scrollbar`, `scrollbar_drag_apply`, `ScrollbarDrag`, `SCROLLBAR_W = 10`) - the same primitives GRID uses today.
- **Per-host scroll application**:
  - **xpl**: child paint translate composed with `(-scroll_x, -scroll_y)`; section's body rect pushed as a clip before descent.
  - **win32 native**: child HWNDs repositioned via `SetWindowPos` with the scroll offset baked into `(x, y)`; SECTION HWND gains `WS_CLIPCHILDREN` so children clip to the SECTION's region (band's non-chip area + body). Painted region updated to cover the full body (no longer transparent through the band, only the band's non-chip area).
  - **macOS native**: child NSViews' frames offset by the scroll position. SECTION's NSView is already a normal NSView (clips to its bounds by default), so children below the body or past the right edge are clipped naturally.
- **Wheel**: SECTION consumes `NEUI_EVENT_MOUSE_WHEEL` when scrollable. Vertical-only / both: wheel-Y scrolls vertically. Horizontal-only / both: Shift+wheel-Y scrolls horizontally (matching the existing pattern in `hosts/crossplatform/host.cpp` LISTBOX / MULTILINE). Native trackpad horizontal-X events on macOS map to horizontal directly.
- **Bouncy / smooth scroll**: opt-in via the existing `NEUI_ATTR_GRID_SCROLL_MODE` family - but **renamed and broadened**. See [Phase 4](#phase-4-smooth-scroll--rubber-band).
- **Event payload**: no new events. SECTION already inherits `RESIZE`; the scroll position is internal state, not part of the public API in v1. (A future `NEUI_EVENT_SCROLL_CHANGED` is a deferred follow-up.)
- **`emit_events`**: SECTION stays `emit_events = false` for non-scrolling sections (today's behaviour). Scrolling sections flip to `emit_events = true` at `widget_show` so wheel + scrollbar hit-tests reach the SECTION's `on_mouse_event` on the xpl host (`hosts/crossplatform/host.cpp` widget_at / dispatch_mouse_event gate on this flag).
- **Tab stop**: SECTION is not a tab stop. The scrollable container itself doesn't receive keyboard focus; its children do.

## Phase 0: Extract generic scroll kinetics

Pre-requisite refactor. **No behaviour change.**

New header: `hosts/shared/scroll_kinetics.h`. Owns the rubber-band + bounce math currently in `grid_model.h:141-145, 597-759`. Types:

```cpp
namespace neui_detail {

  // Constants (lifted from grid_model.h - identical values).
  inline constexpr double SCROLL_RUBBER_RESIST    = 0.55;
  inline constexpr double SCROLL_BOUNCE_LERP      = 0.29;
  inline constexpr double SCROLL_BOUNCE_EPS       = 0.5;

  struct ScrollKinetics {
    double raw_px            = 0.0;
    int    last_commit_px    = 0;
    bool   suppress_momentum = false;
  };

  struct ScrollWheelInput {
    double delta_px      = 0.0;
    bool   precise       = false;
    bool   phase_began   = false;
    bool   phase_changed = false;
    bool   phase_ended   = false;
    bool   momentum      = false;
    bool   momentum_ended = false;
  };

  struct ScrollWheelAction {
    bool stop_bounce  = false;
    bool start_bounce = false;
    bool changed      = false;
  };

  // Rubber-band mapping: raw position (unbounded) -> committed display position.
  double scroll_rubber(double raw_px, double max_px, double viewport_px);

  // Apply a wheel event. `position_out` is the new committed position (in
  // logical px). Caller decomposes it into whatever data model it wants
  // (row index + fine offset for GRID; flat px for SECTION).
  ScrollWheelAction scroll_wheel(ScrollKinetics& k,
                                  const ScrollWheelInput& in,
                                  double max_px, double viewport_px,
                                  double& position_out);

  // One spring-back step. Returns true while still animating.
  bool scroll_bounce_step(ScrollKinetics& k,
                           double max_px, double viewport_px,
                           double& position_out);

  // Sync if something else moved the position since the last commit.
  void scroll_resync(ScrollKinetics& k, int current_committed_px);
}
```

The primitive is **position-px in, position-px out**. The GRID-specific row-index decomposition stays in `grid_model.h::grid_scroll_commit` - it now calls `scroll_wheel` / `scroll_bounce_step`, reads the returned position, and decomposes it into `scroll_offset_y` (rows) + `scroll_px_offset` (fine px). Same for `last_commit_px`: GRID keeps writing the row*row_h + px formula into the kinetics' `last_commit_px` field via the resync entry point.

The existing `grid_scroll_wheel` / `grid_scroll_bounce_step` / `GridScrollKinetics` / `GridWheelInput` / `GridWheelAction` names stay - they become thin shims in `grid_model.h` over the new primitives. Call sites in `hosts/macos/window.mm`, `hosts/macos/widgets.mm`, `hosts/win32/widgets.cpp`, `hosts/win32/window.cpp` compile unchanged.

Verify: build clean, run `neui_grid_example` on macOS, confirm trackpad smooth scroll + rubber-band + spring-back unchanged. Then run the Win32 stepped + smooth modes through `examples\grid_example.cpp`.

## Phase 1: Title chip "none"

`hosts/shared/widget_paint_section.h`:
- `paint_section`: when `align && !strcmp(align, "none")`, skip the chip + draw_text path, fill the whole rect with `bg_argb` (same fast path as `!text || !*text`).
- `section_chip_rect`: not called when `align == "none"` (caller short-circuits).

Add `"none"` to the documented values in CLAUDE.md's SECTION row + the `k_well_known_attrs` row stays unchanged (it's the `align_text` string entry; "none" is one more accepted value).

Per-host:
- **xpl**: `SectionWidget::paint` already passes the attr string through; no change.
- **win32**: `apply_section_region_w32` already has a "no band" path when `text.empty()`; extend it to take the same path when `align == "none"`. Live-update on `NEUI_ATTR_ALIGN_TEXT` change already rebuilds the region (`hosts/win32/widgets.cpp:3364-3368`).
- **macOS**: `NEUINativePaintedView::drawRect:` SECTION branch reads the attrs - it just paints what `paint_section` produces, so no extra code.

Add unit test rows in `tests/` over `section_chip_rect` to confirm the new return path / clamping is unchanged for the existing three alignments and verify `"none"` is short-circuited at the helper boundary.

## Phase 2: Scroll state on SectionWidget

Three new attrs (`include/neui/d/attrs.h` + `k_well_known_attrs` row):
- `NEUI_ATTR_SCROLL_MODE` (string, default `"none"`): `"none"` / `"vertical"` / `"horizontal"` / `"both"`.
- `NEUI_ATTR_CONTENT_WIDTH` (int, default `0` = auto).
- `NEUI_ATTR_CONTENT_HEIGHT` (int, default `0` = auto).

State stored on each host's SECTION widget data:

```cpp
// xpl: SectionWidget (hosts/crossplatform/host.h)
class SectionWidget : public WidgetData {
public:
  uint8_t  scroll_mode      = 0;  // 0=none, 1=vert, 2=horz, 3=both
  int      scroll_x         = 0;  // logical px, content offset
  int      scroll_y         = 0;
  int      content_w_cached = 0;  // last auto-computed content extent
  int      content_h_cached = 0;
  ScrollbarDrag vert_drag;
  ScrollbarDrag horz_drag;
  ScrollKinetics scroll_kin;      // unused unless smooth-scroll on
  uint8_t  smooth_scroll_mode = 0;// 0=platform, 1=stepped, 2=smooth
  void paint(neui_render_backend_t*, neui_render_ctx_t, bool) override;
  bool on_mouse_event(neui_event_t*) override;
  bool hit_test(int x, int y, uint32_t* out_idx) override;
};
```

The same state shape ports to `hosts/win32/host.h::WidgetData` and `hosts/macos/host.h::WidgetData` - but only allocated for SECTION widgets (held in a `std::unique_ptr<SectionScrollState>` on WidgetData, lazy on first need). The native hosts read the same `SectionScrollState` and apply the offset to child positioning, drive a per-section bounce timer, and paint scrollbars via the painted-view drawRect path.

Helpers in a new `hosts/shared/widget_section_scroll.h`:
- `compute_section_content_extent(parent_index, attrs, body_w, body_h, tree) -> (content_w, content_h)`: applies auto-from-children + attr override.
- `section_body_rect(fw, fh, has_band, vert_sb, horz_sb) -> body_x/y/w/h + scrollbar gutter rects`.
- `clamp_section_scroll(state, content_w, content_h, body_w, body_h)`: clamps `scroll_x` / `scroll_y` to `[0, content - visible]` (or extends rubber-band when smooth).
- `section_apply_wheel(state, in, content_w, content_h, body_w, body_h, axis)`: thin wrapper over `scroll_wheel` for the chosen axis.

`NEUI_ATTR_SCROLL_MODE` read at `widget_show` per host: if non-`"none"`, the SECTION widget data flips `emit_events = true` (xpl), keeps mouse-event routing enabled (native always routes), and the scroll state struct is allocated.

## Phase 3: Per-host wiring

### Phase 3a: xpl host

`hosts/crossplatform/host.cpp`:
- `SectionWidget::paint`: still paints background + chip. Additionally:
  1. If scroll mode != none, compute `body_rect` accounting for visible scrollbars.
  2. Compute `content_w` / `content_h` via the new helper (auto or attr).
  3. Clamp `scroll_x` / `scroll_y`.
  4. Paint scrollbars (vertical on right of body, horizontal on bottom). Reuse the same `compute_scrollbar` + thumb-paint shape GRID uses (`widget_paint_grid.h:487-526`).
- `paint_widgets_recursive`: when descending into a SECTION's children:
  1. `backend->push_clip(ctx, sec_body_x, sec_body_y, sec_body_w, sec_body_h)` before the translate.
  2. `backend->translate(ctx, wd.x - section_state->scroll_x, wd.y - section_state->scroll_y - band_h_adjustment)`. The `band_h_adjustment` is the chip band height when present, so child `y=0` aligns to the body top - matches today's "children paint at child coords relative to section's top-left including band" semantics (children currently sit on top of the band area by default; clients position around it).

   Wait - clarification needed: today on the xpl host, a SECTION child with `y=0` paints at the SECTION's top-left INCLUDING the band area. We should NOT silently shift children down by the band on the scrolling path - that would diverge from non-scrolling sections + from native hosts. Children continue to be positioned by the client; the section just clips + offsets them as a group. The body rect for clipping purposes is computed from the chip-band height when band exists, but the **scroll origin** is the SECTION's top-left, identical to today's coord system.
  3. `pop_clip` after the recursive paint.
- `SectionWidget::on_mouse_event`:
  - `NEUI_EVENT_MOUSE_WHEEL`: convert to `ScrollWheelInput`, dispatch via `section_apply_wheel`. Consume by returning `true`. Invalidate the SECTION's frame.
  - `NEUI_EVENT_MOUSE_BUTTON_DOWN` over a scrollbar thumb: start a drag (set `state->vert_drag.active = true`, capture start coords). Mouse-move while drag-active maps via `scrollbar_drag_apply`. Mouse-up clears `active`.
  - Hits inside the body but not on a scrollbar return `false` so descendants still receive the event.
- `SectionWidget::hit_test`: when the event hits a scrollbar / thumb / body, return the SECTION as the target. Otherwise fall through to the default descendant walker.
- `dispatch_mouse_event` already accumulates parent-relative coords; the scroll offset must be subtracted when computing child-local coords for descendants of a scrolling SECTION. Implement in the descendant walker by subtracting `(scroll_x, scroll_y)` once when entering a scrolling SECTION. Same subtraction applied to `wd.abs_x` / `wd.abs_y` updates during paint.

### Phase 3b: Win32 native

`hosts/win32/widgets.cpp`:
- SECTION HWND: add `WS_CLIPCHILDREN` to the style so children clip naturally. The window region keeps the "chip is transparent through the parent" trick for the band, but the body now needs to fully repaint - it does already via `paint_section_w32`. The region only carves OUT the band's non-chip area; the body remains in-region.
- Child positioning: when SECTION has a non-`"none"` scroll mode, child HWNDs' physical positions are `(wd.x - scroll_x) * dpi/96`, `(wd.y - scroll_y) * dpi/96`. `SetWindowPos` is called on every scroll change. Children whose rect falls fully outside the SECTION's client area are still positioned but clipped by `WS_CLIPCHILDREN` + the SECTION HWND's bounds.
- `painted_msg_section_w32` (new): forward `WM_MOUSEWHEEL` + `WM_MOUSEHWHEEL` + scrollbar-thumb-drag mouse messages to the shared SECTION scroll handler. Wheel notches translate through `SPI_GETWHEELSCROLLLINES` + a per-line pixel step (default 40 px, matching GRID's `cfg.row_h * SPI_GETWHEELSCROLLLINES` shape).
- Scrollbar paint: extend `paint_section_w32` to call into a shared `paint_section_scrollbars` helper after the body fill.
- Live update: changing `NEUI_ATTR_SCROLL_MODE` / `_CONTENT_WIDTH` / `_HEIGHT` invalidates + reposiitions children (same code path as the wheel scroll, with the new clamps).
- `apply_section_region_w32`: extend the region to include the scrollbar gutter (otherwise scrollbar pixels are clipped out).

### Phase 3c: macOS native

`hosts/macos/window.mm` + `hosts/macos/widgets.mm`:
- SECTION's `NEUINativePaintedView` already clips its subviews to its bounds. Apply scroll by setting each direct subview's `frame.origin` to `(child.x - scroll_x, child.y - scroll_y)` (logical px - the view tree is `isFlipped = YES`).
- `scrollWheel:` on the SECTION painted view: build a `ScrollWheelInput` from `NSEvent` (`scrollingDeltaY`, `hasPreciseScrollingDeltas`, `phase`, `momentumPhase`); call `section_apply_wheel`; trigger a redraw + reposition subviews.
- Spring-back: per-section `NSTimer` at 60 Hz, started via the action returned from `scroll_wheel`. Same shape as the GRID smooth-scroll timer in `widgets.mm` / `window.mm`.
- Mouse-down on a scrollbar thumb: handled in `mouseDown:` on the painted view; route to the shared drag helper.

## Phase 4: Smooth scroll + rubber-band

`NEUI_ATTR_SCROLL_MODE` controls **axes**; a second attr controls **kinetics**. Two options - **pick during implementation**, no breaking-change risk yet since SECTION scrolling is new:

**Option A** - reuse the existing `NEUI_ATTR_GRID_SCROLL_MODE` constants but make the attr name namespace-agnostic. Rename underlying key from `neui.attr.grid.scroll_mode` to `neui.attr.scroll_mode_kinetics` and alias the GRID macro to it. **Breaking** for clients that set the GRID attr by literal key string (rare) - safe path is to keep both keys recognised on GRID for one release.

**Option B** - new attr `NEUI_ATTR_SCROLL_KINETICS` (int, `0`=platform, `1`=stepped, `2`=smooth) usable on both SECTION + GRID. Old `NEUI_ATTR_GRID_SCROLL_MODE` stays as a GRID-only alias.

Defaults match GRID: macOS = smooth, Win32 = stepped, null = stepped. The kinetics primitive from Phase 0 is feature-equal across both widget kinds, so smooth-scroll behaviour is identical to GRID's once turned on.

## Phase 5: Documentation + verification

- Update `CLAUDE.md`:
  - SECTION row in the widget table: scrolling + chip "none".
  - `NEUI_ATTR_ALIGN_TEXT` row in the attr table: add `"none"`.
  - New rows for `NEUI_ATTR_SCROLL_MODE`, `_CONTENT_WIDTH`, `_CONTENT_HEIGHT`, `_SCROLL_KINETICS` (per the Phase 4 outcome).
  - Note in the Architecture section that `hosts/shared/scroll_kinetics.h` is the shared kinetics primitive used by GRID + SECTION.
- New example: `examples/section_scroll_example.cpp`. A SECTION sized 400x200 with 20+ buttons stacked vertically so the content extends past the visible body. Verify wheel + drag + edge clamping on Win32 + macOS (xpl + native). Bonus: a section with `align_text="none"` and `scroll_mode="both"` showing the no-band path.
- `plans/section-scrolling.md` flagged "shipped" once landed; entry added to the `Plans` section of CLAUDE.md alongside the other shipped plans.

## Follow-up shipped: smooth kinetics (2026-06)

The v1 landing scrolled in hard-clamped line steps; the kinetics planned
above shipped in a follow-up. `SectionScrollState` carries per-axis
`ScrollKinetics` (`kin_v` / `kin_h`) + `kinetic_over_v/h` overshoot flags;
`widget_section_scroll.h` adds `section_scroll_max_px` / `_commit` /
`section_scroll_wheel_kinetic` / `section_scroll_bounce_step` (flat-px twins
of the GRID wrappers in `grid_model.h`) plus `clamp_section_scroll_idle`
(the paint-time clamp that leaves a kinetics-owned rubber-band overshoot
alone). Host wiring mirrors GRID: macOS feeds NSEvent phases / precise
deltas per axis + an `NSTimer` spring-back (`sectionBounceTick`); Win32
feeds synthetic precise notches (`SECTION_WHEEL_LINE_PX` = 40 px/line) +
a `SetTimer` spring-back (`XPL_SECTION_BOUNCE_TIMER_ID`). The platform
layer intercepts the wheel for the nearest scrolling-section ancestor and
bubbles the line event only through the widgets below it
(`dispatch_wheel_event(hit, ev, stop_before)`), so children like MULTILINE
keep consuming their own wheel. Tier-1 tests in
`tests/test_section_scroll.cpp`.

## Follow-up shipped: native-host wiring (2026-06)

Phase 3b (Win32 native) and phase 3c (macOS native) ship together using
the same `SectionScrollState` + `SectionLayout` allocated lazily on
`WidgetData` (so non-scrolling sections still pay nothing). Common
shape: `section_refresh_scroll_state_<host>` allocates / drops state
on `NEUI_ATTR_SCROLL_MODE`; `section_compute_layout_<host>` reads the
child tree via the new shared `section_compute_auto_extent` template
(`widget_section_scroll.h`); `section_apply_layout_changes_<host>`
rebuilds + reposits children + invalidates; `section_kinetic_wheel_*`
(or the painted-view's `sectionScrollWheel:` on macOS) feeds the rich
wheel input into `section_scroll_wheel_kinetic`; a per-section
spring-back tick (`SetTimer` on Win32, `NSTimer` on macOS) runs
`section_scroll_bounce_step`. Child widgets store **body-local**
(x, y); the host applies the section's scroll subtraction at HWND
positioning (`SetWindowPos`) / NSView positioning (`setFrame:`) time
via a tiny `parent_scroll_offset_*` helper called from
`widget_set_pos` + `cascade_dpi` (Win32) /
`apply_geometry_native_macos` (macOS). `Tree::get_parent` was lifted
to the public API so the helper can read it without re-implementing
the linear scan.

Win32 specifics: the SECTION HWND gains `WS_CLIPCHILDREN` when scrolling
is on so child HWNDs clip to its bounds; `PaintedWndProc` already
forwards mouse / wheel / timer to `painted_msg_fn`, so the new
`painted_msg_section_w32` plugs into the same seam KNOB / GRID /
CUSTOMDRAW use; `WM_MOUSEHWHEEL` was added to the seam's forwarded set.
The wheel ladder handles `SPI_GETWHEELSCROLLLINES` /
`SPI_GETWHEELSCROLLCHARS` per axis, with Shift+wheel routing to
horizontal for non-tilt-wheel users.

macOS specifics: `NEUINativePaintedView` gained an `NSTimer*
section_bounce_timer` + `sectionScrollWheel:widget:` /
`sectionBounceTick:` methods, and now picks SECTION as an interactive
case in `scrollWheel:` / `mouseDown:` / `mouseDragged:` / `mouseUp:`
when the widget's `section_scroll_state` is allocated. Wheel input is
fed through the shared kinetics with full NSEvent phase / momentum /
precise-delta plumbing - identical kinetic feel to GRID smooth-scroll.

## Deferred follow-ups

- `NEUI_EVENT_SCROLL_CHANGED` event for clients that want to react to scroll position (e.g. lazy-loading content).
- Programmatic `widgets->scroll_to(widget, x, y)` / `ensure_visible(widget)` helpers for SECTION children. Mirror of GRID's `ensure_row_visible`.
- Auto-scroll while drag-hovering near an edge during DnD.
- Nested scrolling SECTIONs (parent re-receives wheel when child reaches its edge). v1 forwards wheel to whichever SECTION is the deepest hit-test target; the ancestor never receives it. Standard wheel-event escalation is a v2.
- Horizontal-only fling on Win32 (no `WM_MOUSEHWHEEL` from precision touchpads on older builds - low priority).
- Per-widget `scroll_into_view` triggered by focus changes (so Tab into an off-screen child auto-scrolls it into view).

## Risks / open questions

- **xpl child clipping interaction with CUSTOMDRAW**: a CUSTOMDRAW inside a scrolling SECTION will receive transformed coordinates; check that its widget-local-origin contract (`hosts/crossplatform/host.cpp:2056-2083`) survives the additional translate. Probably fine - the seam already pushes its own translate + clip - but worth a deliberate test in the new example.
- **Native control children of a scrolling SECTION on Win32**: `SetWindowPos` on every wheel notch is cheap, but at very high content extents (10k+ child widgets) the per-scroll repaint cost may need batching with `BeginDeferWindowPos`. v1: don't optimise; flag if the example noticeably stutters.
- **macOS NSTextField inside a scrolling SECTION**: AppKit's first-responder + caret tracking should follow the view's frame as it moves. Test with an INPUTBOX inside a scrolling SECTION - if the IME positioning seam (`hosts/macos/window.mm` IME branch) breaks, treat as a bug and fix before shipping.
- **DPI changes mid-session on Win32**: child repositioning already runs through `cascade_dpi`; the scroll offset is logical px so the same conversion path applies. Verify on a multi-DPI test rig.
