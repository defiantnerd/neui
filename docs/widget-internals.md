<!-- neui reference. Extracted from CLAUDE.md - read when working on these topics. -->

## Per-widget implementation details

Implementation-specific constants, scrollbar geometry, multiline cursor math, edit-history coalescing, IME composition state machine, dark-mode uxtheme ordinals - all live in code with comments. Heavy lifting: `hosts/crossplatform/host.cpp` (LISTBOX / COMBOBOX / TREEVIEW / MULTILINE / KNOB / GRID / overlays); undo in `hosts/shared/edit_history.h`; dark HMENU in `hosts/shared/win32/dark_menu*.h`; palette sources in the `theme_provider_*.h` files.

**xpl MULTILINE performance / large text.** Pixel hit-testing (`ml_pos_from_col`) and the wrap layout (`ml_fit_chars_end`) binary-search character boundaries (O(log K) `measure_text` calls), so click / drag-select / Up-Down / wrap on a long line cost O(K log K) shaping rather than the old O(K^2) - long lines no longer freeze the UI. `EditHistory::would_push` lets `te_history_mark` skip the full-text snapshot copy on coalesced keystrokes (no O(N) copy per character). `MultilineWidget::cached_line_starts()` caches the visual-row model (rebuilt only on text / width / font / wrap change), and `paint()` caps shaping/drawing to the visible-width span (`vis_char_cap`) + reuses a `_paint_scratch` buffer, so a frame costs O(visible) not O(line length). Known limits (deferred): wrap recomputes the whole document on each edit (no incremental reflow); the caret has no affinity at a soft-wrap boundary, so End on a wrapped row shows the caret at the start of the next visual row.

**xpl widget-level hover / pressed visuals.** `WidgetData::hovered` / `pressed` flags (`host.h:57-58`) are maintained by `Session::set_hovered` / `set_pressed`, which unconditionally invalidate the owning frame on every transition (not just for state-filtered compounds). `ButtonWidget::paint`, `CheckboxWidget::paint`, and the collapsed `ComboBoxWidget::paint` shade their fill via `neui_detail::shade(bg, ±16)` - pressed wins over hovered. `ListItemsWidget` and `TreeviewWidget` carry a per-widget `hover_row` (UINT32_MAX = none) updated in their `on_mouse_event` MOUSE_MOVE branch; `paint_scrollable_list` and `TreeviewWidget::paint` paint a subtle `shade(bg, +14)` background on the hovered unselected row, gated on `this->hovered` so leaving the widget naturally clears the highlight. Scrollbar-column hits and sb-drag-start clear `hover_row`. `ButtonWidget::on_keydown` activates on Space / Enter with a synthetic `MOUSE_BUTTON_CLICK` (no modifier filter, mirroring CheckboxWidget's keyboard pattern). Native hosts get hover / pressed / keyboard activation for free from the underlying control classes.

**xpl win32 DBLCLK→CLICK parity.** `platform_win32.cpp::WM_LBUTTONDBLCLK` mirrors the WM_LBUTTONDOWN setup (`set_focus` / `set_pressed` / `SetCapture`) so the trailing WM_LBUTTONUP fires `MOUSE_BUTTON_CLICK` on widgets that don't opt into DBLCLICK (BUTTON in particular). Without this, the system's CS_DBLCLKS-driven replacement of the second rapid DOWN with DBLCLK would silently swallow every-other rapid click. Widgets that explicitly handle DBLCLICK (CHECKBOX, TREEVIEW, MULTILINE, GRID, KNOB-reset) are unaffected - they now just see an additional trailing CLICK they already ignore. Matches the native win32 host where the system "Button" class has no CS_DBLCLKS and every fast click is a plain click.

## Partial repaint (xpl host)

Status and design record: `plans/xpl-partial-repaint.md`. **Live on Linux / Cairo and macOS + iOS / CG (P1-P3)**; win32 / D2D still paints full frames (P4 open), and LVGL has its own retained invalidation.

- **What is partial:** every invalidation that concerns one widget's own pixels goes through `Session::invalidate_widget_rect`. It queues the widget's damage footprint via `platform_invalidate_rect`: cached frame-local `abs_x` / `abs_y` + `paint_bounds()` + a 4 px margin.
  - **Client-driven:** the `invalidate_widget_visual` funnel in `widgets.cpp` - attr writes, `set_text` / `set_asset` / `set_enabled`, the public `invalidate`, GRID content.
  - **Widget-internal:** `WidgetData::repaint()` (caret, typing, value drags, list / tree / grid state, tab switch); hover / press / focus transitions (old + new widget); behavior-driven attr writes.
  - **Scrolling:** SECTION / GRID scroll and spring-back ticks damage just that container, which covers its children since they are clipped to it (Linux + macOS platform paths).
  - **Whole-frame:** overlays reaching outside any widget (combo drop list, popup menus, in-frame menubar, toast), theme / DPI / OS-focus changes, expose / resize, and every geometry change.
- **Invariant 1 - geometry changes repaint fully.**
  - **Why:** a rect queued from the previous paint's `abs_x` / `abs_y` is only safe because every create / destroy / show / hide / `set_pos` / `set_size` requests a FULL repaint, which absorbs the rects queued alongside it.
  - **Where:** `notify_tree_changed` in `widgets.cpp`. Before P3 this was an LVGL-only hook, and off LVGL such changes invalidated nothing at all.
  - **Scrolling** is the one geometry change that stays partial: children of a SECTION / GRID / TABVIEW stay inside the container's rect.
  - **Converting a geometry path** to rect invalidation means damaging both the old and the new rect.
- **Invariant 2 - a widget that paints outside its rect overrides `WidgetData::paint_bounds()`.** Otherwise a change leaves stale overflow behind and a neighbour's repaint culls or clips it. `ImageWidget` does this (`NEUI_ATTR_ROTATION` rotates it unclipped; it reports the half-diagonal square whenever the attr exists). CUSTOMDRAW / compound widgets are clipped to their own rect, and focus rings fit in the margin.
- **Frame path:** `Session::paint_frame(ctx, frame, damage)` hands non-trivial damage (< 60% coverage, `hosts/shared/damage.h`) to the backend's `set_frame_damage`. It culls (`paint_widgets_recursive`: no PREUPDATE / paint for widgets missing the damage, `abs_x` / `abs_y` still refreshed for hit-testing) **only when the backend returned true**. A refusing backend clears everything. Overlays aren't culled; the backend clip limits them.
- **Per platform:**
  - **Cairo:** owns its surface. It clips, clears and blits per rect, and refuses until it holds a complete frame at the current scale. See `docs/host-linux.md`.
  - **CG (macOS / iOS):** AppKit / UIKit own the pixels. `platform_invalidate_rect` -> `setNeedsDisplayInRect:`; `drawRect:` turns `getRectsBeingDrawn:` / the dirty rect into the damage. The layer keeps everything outside it. **A layer-backed view gets ONE dirty rect, the bounding box of everything invalidated since the last draw**, so scattered changes in one display pass coalesce, often past 60%, into a full frame. Localized changes are where it pays: measured on an M-series Mac with `neui_partial_repaint_example 1` (one of 48 compound knobs per tick), 0.10 ms per paint against 1.15 ms full. The draw can't be culled inside AppKit's box: the double-buffered backing store keeps the previous frame only outside it.
- **Debug switches** (env, read once):
  - `NEUI_DEBUG_FULL_REPAINT=1` forces full frames, to bisect an artefact.
  - `NEUI_DEBUG_DAMAGE=1` tints each partial frame's damage rects.
  - `NEUI_DEBUG_PAINT_STATS=1` prints frames / partial / avg / max `paint_frame` ms once a second to stderr.

  `neui_partial_repaint_example [knobs-per-tick]` simulates automation and has a rotating IMAGE for invariant 2.

## Enabled / disabled state

`widgets->set_enabled(sess, w, bool)` / `get_enabled(sess, w)`. Default enabled=true. Disabled widgets paint dimmed and don't receive input.

- **win32 native**: `EnableWindow(hwnd, enabled)`; deferred flag applied in `create_child_windows` if HWND not yet created.
- **xpl**: `WidgetData::enabled` flag; paint brackets `wd.paint()` with `push_alpha(0.5)`; hit-test / `collect_tab_stops` / `dispatch_mouse_event` skip disabled. Focused-then-disabled advances focus.
- **macOS native**: `apply_enabled_native_macos(wd)` - `[NSControl setEnabled:]` for control leaves, document-view disable for scroll-hosted controls, `push_alpha(0.5)` dim in `drawRect:` for painted views. Re-applied in `create_native_for_widget`.

Frames + non-interactive containers (SECTION, MENUBAR) accept the call; effect host-defined.

