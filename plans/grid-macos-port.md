# GRID widget - macOS native host port

## Context

The GRID widget already ships in two hosts:
- **xpl host** (the reference; uses the same shared headers as macOS will): `hosts/crossplatform/host.{h,cpp}` + `hosts/crossplatform/widgets.cpp`.
- **win32 native host** (the closest template - it's also a painted-widget host with its own session + widget hierarchy, exactly like macOS): `hosts/win32/host.h` + `hosts/win32/widgets.cpp`.

The shared headers in `hosts/shared/` do the heavy lifting for both hosts:
- `grid_model.h` - column / row / scroll state, viewport calc, hit-test, attr-config reader.
- `widget_paint_grid.h` - the entire paint pass (sticky header, body cells with per-column alignment, focus-row band, dual scrollbars, cell-focus outline, border).
- `scrollbar.h` - axis-agnostic scrollbar geometry + drag helpers.

Your job: parity for `neui.host.macos`. The structural pattern is identical to the win32 port - read the recent commit on the win32 host as your template before starting.

User requirements that ship with the GRID (already wired in both hosts; preserve on macOS):
- Vertical + horizontal scroll, sticky column header that tracks horizontal scroll.
- User-adjustable column widths via header-divider drag (ew-resize cursor on hover).
- Two focus modes via `NEUI_ATTR_GRID_CELL_FOCUS` (default 0 = row-focus, 1 = cell-focus).
- Event ladder on click: `GRID_ROW_SELECTED` -> (cell-focus only) `GRID_CELL_SELECTED` -> `GRID_CELL_CLICKED`, each only fires if the previous wasn't consumed by the client.
- `GRID_ROW_ACTIVATED` on double-click / Return.
- `GRID_COLUMN_RESIZED` on resize-drag release with the old + new width payload.
- Optional row-focus highlight (`NEUI_ATTR_GRID_SHOW_FOCUS_ROW` + `_FOCUS_ROW_COLOR`).
- Per-cell color + enabled overrides (sparse `std::unordered_map<uint64_t, GridCellOverride>`).
- Keyboard nav: Up/Down by 1, PgUp/PgDn by visible-row count, Home/End first/last row (or first/last column in cell-focus mode), Ctrl+Home/Ctrl+End grid corners, Left/Right column-cursor or horizontal scroll, Return = activate.
- Tab in/out as one tab stop (no per-cell Tab).

## Read before starting

In this order, ~15 minutes:

1. `plans/neui-needs-a-grid-drifting-creek.md` - the approved design plan (skim, especially "Two focus modes" + "Sticky headers" + "Event delivery ladder").
2. `hosts/win32/widgets.cpp` - search for `paint_grid_w32`, `painted_msg_grid_w32`, `grid_api`, `resolve_grid_w32`. This is the reference template you are porting from. The shape is parallel: dispatch helpers (`grid_fire_row_selected_w32` etc.), paint_fn (delegates to `paint_grid`), painted_msg_fn (the big switch over WM_MOUSE* / WM_KEYDOWN with the dispatch ladder), `grid_api` (28 methods, all thin wrappers).
3. `hosts/win32/widgets.cpp` - the **NEUI_W_GRID branch in CreateChildHwnd** (search "NEUI_W_GRID"). Shows the creation pattern.
4. `hosts/macos/window.mm` - the `NEUINativePaintedView` `@interface` + `drawRect:` (around line 315) and the per-type branches (`NEUI_W_IMAGE`, `NEUI_W_KNOB`, `NEUI_W_CUSTOMDRAW`). This is where you'll add the GRID paint branch. Also read the mouse + key event methods (`mouseDown:`, `mouseUp:`, `mouseDragged:`, `mouseMoved:`, `scrollWheel:`, `keyDown:`) - this is where the GRID input dispatch goes.
5. `include/neui/d/grid.h` + the `NEUI_EVENT_GRID_*` block in `include/neui/d/events.h` - the public API surface. No changes needed; you're implementing this.

## Step-by-step

### 1. Add `GridModel` to `macos_host::WidgetData`

File: `hosts/macos/host.h`.

- Add `#include "../shared/grid_model.h"` next to the other shared includes.
- Add a lazy-allocated field at the end of `WidgetData` (mirror of `hosts/win32/host.h`):
  ```cpp
  // GRID (NEUI_W_GRID) state - column model, row data, scroll state,
  // selection, column-resize / scrollbar drag state. Lazy-allocated;
  // every other widget pays a single pointer.
  std::unique_ptr<neui_detail::GridModel> grid_model;
  ```

### 2. Add GRID paint + input glue to `NEUINativePaintedView`

File: `hosts/macos/window.mm`.

- At the top, add `#include "../shared/widget_paint_grid.h"` next to `widget_paint_knob.h` / `widget_paint_section.h` / `widget_paint_compound.h`.
- In `drawRect:`, add a new `else if (wd->type && !strcmp(wd->type, NEUI_W_GRID))` branch parallel to the existing `NEUI_W_KNOB` / `NEUI_W_CUSTOMDRAW` ones. Body is short:
  ```cpp
  } else if (wd->type && !strcmp(wd->type, NEUI_W_GRID)) {
    if (!wd->grid_model) wd->grid_model = std::make_unique<neui_detail::GridModel>();
    bool focused = (self.window.firstResponder == self);
    neui_detail::paint_grid(backend, render_ctx,
                              0.0f, 0.0f,
                              (float)sz.width, (float)sz.height,
                              *wd->grid_model, wd->attrs.get(),
                              focused);
  }
  ```
  Note: `paint_grid` paints the body background itself, so you don't need the `panel_bg` clear that `KNOB` relies on. The default clear from `begin_frame` is fine - `paint_grid`'s `fill_rect(0,0,w,h, body_bg)` overpaints it.
- Make GRID accept first responder so it receives keys. Update `acceptsFirstResponder`:
  ```cpp
  - (BOOL)acceptsFirstResponder
  {
    auto* wd = macos_host::widget_for_id(widget_id);
    if (!wd || !wd->type) return NO;
    return !strcmp(wd->type, NEUI_W_CUSTOMDRAW) ||
           !strcmp(wd->type, NEUI_W_GRID);
  }
  ```
- Add a dedicated `painted_msg_grid_macos` style dispatcher OR inline the GRID input logic into the existing mouseDown:/mouseDragged:/mouseUp:/mouseMoved:/scrollWheel:/keyDown: methods, branching on `wd->type`. The inline approach is what the existing KNOB code does (read those methods first). **Translate `painted_msg_grid_w32` from win32/widgets.cpp directly** - the logic is identical, you only need to change:
  - **Coordinate source**: Win32 uses `GET_X_LPARAM(lParam)` in physical pixels and divides by dpi. macOS uses `[event locationInWindow]` -> `[self convertPoint:fromView:nil]` which is already in logical points. Drop the `phys_to_log_w32` calls; the coords are already logical.
  - **Modifier reads**: `wParam & MK_SHIFT` becomes `event.modifierFlags & NSEventModifierFlagShift`; `wParam & MK_LBUTTON` becomes "we're in `mouseDragged:` so the button is held" (no explicit check needed - AppKit gives you mouseDragged: only while the button is down).
  - **Repaint**: `InvalidateRect(wd.hwnd, nullptr, FALSE)` becomes `[self setNeedsDisplay:YES]`.
  - **Cursor**: `platform_set_cursor_w32(NEUI_CURSOR_EW_RESIZE)` becomes `[[NSCursor resizeLeftRightCursor] set]`; reset is `[[NSCursor arrowCursor] set]`. The cleanest path is a cursor-rect (`addCursorRect:cursor:`) inside `resetCursorRects`, **but** that fights the grid because the divider position depends on scroll_x and column widths. Use direct `[NSCursor set]` calls from `mouseMoved:` like the win32 path - it's accurate.
  - **Keyboard**: Win32 `WM_KEYDOWN` with `wParam == VK_*` becomes macOS `keyDown:` with `event.keyCode`. Use the existing `neui_detail::mac_keycode_to_neui` helper (see `KNOB` keyDown: handling) to get an `NEUI_KEY_*` code, then the switch is identical to win32 (just match on `NEUI_KEY_UP` / `NEUI_KEY_DOWN` / ... instead of `VK_UP` / `VK_DOWN` / ...). Modifiers via `neui_detail::mac_modifiers_to_neui`.
  - **Wheel**: Win32 `WM_MOUSEWHEEL` becomes macOS `scrollWheel:`. AppKit gives you continuous scroll deltas; the existing painted view already accumulates them in `wheel_accum_y` for KNOB. For GRID, treat each accumulated tick as one row of scroll, same as win32's `WHEEL_DELTA` notch step.
  - **Tracking area for `mouseMoved:`**: `mouseMoved:` only fires when an `NSTrackingArea` with `NSTrackingMouseMoved | NSTrackingActiveInKeyWindow` is installed on the view. The existing painted view already does this for KNOB hover - reuse it; if it's only enabled for KNOB / CUSTOMDRAW today, add a `NEUI_W_GRID` branch to the tracking-area setup.

- **Event dispatch helpers** (`grid_fire_row_selected_macos`, etc.) are the same shape as the win32 versions - copy them verbatim, change the `_w32` suffix to `_macos`, and use `sess->dispatch_event(&ev)` (it already returns `bool` for "client consumed it").

- **Focus on click**: `mouseDown:` should call `[[self window] makeFirstResponder:self]` for a GRID so subsequent `keyDown:` events route here. KNOB does this too.

### 3. Wire GRID creation

File: `hosts/macos/widgets.mm`.

- Find the existing `create_painted_view` / `create_native_for_widget` (or wherever the per-type creation switch lives - search for `NEUI_W_KNOB`). Add a GRID branch that creates a `NEUINativePaintedView` exactly like KNOB / CUSTOMDRAW do - no native control wrapping needed.
- Find the auto-set `emit_events` / `tab_stop` list (search for `NEUI_W_CUSTOMDRAW` in this file) and add `NEUI_W_GRID` to both.
- Make sure the GRID participates in the key-view loop (search for `rebuild_key_view_loop_macos` or `setNextKeyView:`). Add `NEUI_W_GRID` to whatever filter selects which views to include - same set as `NEUI_W_CUSTOMDRAW`.

### 4. Implement `neui_grid_api_t`

File: `hosts/macos/widgets.mm`.

The 28-method table is mechanical translation of `hosts/win32/widgets.cpp`'s `grid_api`. Copy that block verbatim, change the `_w32` suffix to `_macos`, and replace `InvalidateRect(wd->hwnd, nullptr, FALSE)` with `[(__bridge NSView*)wd->native_control setNeedsDisplay:YES]` (or whatever the macOS `WidgetData` uses for its painted view pointer - check `native_control` vs the equivalent field).

Then declare the table at the bottom of `widgets.mm`:
```cpp
neui_grid_api_t grid_api = {
  NEUI_VERSION,
  gr_add_column, gr_get_column_count, gr_set_column_width, gr_get_column_width,
  gr_set_column_min_width, gr_set_column_align, gr_set_column_header, gr_get_column_header,
  gr_remove_column, gr_clear_columns,
  gr_add_row, gr_get_row_count, gr_remove_row, gr_clear_rows,
  gr_set_cell_text, gr_get_cell_text, gr_set_cell_color, gr_set_cell_enabled,
  gr_clear_cell_overrides,
  gr_set_selected_row, gr_get_selected_row, gr_set_selected_cell, gr_get_selected_cell,
  gr_ensure_row_visible, gr_ensure_cell_visible,
  gr_set_scroll_x, gr_get_scroll_x, gr_hit_test,
};
```

### 5. Register `grid_api` in `get_interface`

File: `hosts/macos/host.mm`.

Add to the externs block + the `get_interface` switch:
```cpp
extern neui_grid_api_t grid_api;
...
if (!strcmp(iface, NEUI_API_GRID)) return &grid_api;
```

### 6. Build + verify

```bash
cmake -B out/build -G Xcode
cmake --build out/build --config Debug
open out/build/Debug/neui_grid_example.app
```

Functional checklist:
- Grid renders with header band, ~500 visible rows, dual scrollbars.
- Vertical scroll: trackpad / Magic Mouse / two-finger swipe; scrollbar drag; Up / Down / PgUp / PgDn / Home / End.
- Horizontal scroll: bottom scrollbar drag.
- Column-header sticky during vertical scroll; tracks horizontal scroll.
- Column resize: hover header divider -> cursor becomes ew-resize; drag changes width; release fires `GRID_COLUMN_RESIZED`.
- Row select via click fires `GRID_ROW_SELECTED`; arrow keys move + auto-scroll.
- Row activate via double-click or Return fires `GRID_ROW_ACTIVATED`.
- Toggle the "Toggle row/cell focus" button (sets `NEUI_ATTR_GRID_CELL_FOCUS = 1`): cell cursor appears as a 1px accent outline; Left / Right move cursor across columns; Home / End jump to row endpoints; Ctrl+Home / Ctrl+End jump to grid corners; `GRID_CELL_SELECTED` fires on cell-cursor move.
- Cell-click fallback: when the client returns false from the higher-level events, `GRID_CELL_CLICKED` arrives with the correct (row, col).
- "show_focus_row" checkbox toggles the row highlight band.
- Disabled cell (row 5 col 2 in the example app) paints dimmed and suppresses `GRID_CELL_CLICKED` for that cell.
- Tab in / out of the grid as one tab stop.
- Light <-> dark system theme flip: grid colours follow.

## Things you don't need to change

- The shared headers (`grid_model.h`, `widget_paint_grid.h`, `scrollbar.h`) are done.
- The public C API (`include/neui/d/grid.h`, the `NEUI_EVENT_GRID_*` block + payload structs in `events.h`, `NEUI_W_GRID` in `widgets.h`) is done.
- The well-known attrs table (`hosts/shared/attrs.h::k_well_known_attrs`) is done.
- The example app (`examples/grid_example.cpp`) is done - on macOS it currently selects `neui.host.crossplatform` (line ~22). Once your native port works, switch the macOS branch to `neui.host.macos`:
  ```cpp
  #elif defined(__APPLE__)
  #define GRID_HOST "neui.host.macos"
  ```

## Architectural notes (worth knowing, not blocking)

- **`paint_grid`'s body fill is unconditional**: it issues `fill_rect(fx, fy, fw, fh, body_bg)` first thing. Don't wrap your `drawRect:` GRID branch in any "if section, transparent clear" branch; let `paint_grid` own the surface.
- **Cell coords are widget-local logical pixels** going into `grid_hit_test`. Convert your `event.locationInWindow` -> view-local via `[self convertPoint:fromView:nil]` and pass `point.x` / `point.y` directly. `paint_grid` expects positive Y = down (matches `isFlipped = YES` which `NEUINativePaintedView` already sets).
- **The column-resize hit hot zone is +-3 logical px** around each column boundary. The hit-test math in `grid_hit_test` accounts for scroll_x already - you don't need to adjust.
- **Wheel direction**: the win32 path negates the delta (`m.scroll_offset_y -= delta_lines` where positive delta = scroll up). macOS `scrollingDeltaY` is the *content motion*, not the wheel direction - test this on your hardware and flip the sign if rows scroll the wrong way. KNOB's existing handling is a good reference.
- **Focus on click**: GRID is a tab-stop, so `[[self window] makeFirstResponder:self]` in `mouseDown:` keeps Tab traversal sensible.
- **No NSTableView**: the user already decided against it in the plan. Don't be tempted by it - the painted approach matches the xpl + win32 hosts, supports the layerable-cells V2 path, and avoids the cell-template / data-source plumbing that NSTableView would impose.

## Verification artefacts to commit

- The plan file you're reading should stay as-is (don't delete it; it's the record).
- One commit per logical chunk is nice: (1) `hosts/macos/host.h` GridModel field, (2) `hosts/macos/window.mm` paint + input branches, (3) `hosts/macos/widgets.mm` creation + grid_api, (4) `hosts/macos/host.mm` get_interface registration, (5) `examples/grid_example.cpp` host select flip. Or one commit total - the repo doesn't dictate.

That's everything. The whole port is ~600-800 lines of code, almost all of it mechanical translation from `hosts/win32/widgets.cpp`'s GRID block.
