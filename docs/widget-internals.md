<!-- neui reference. Extracted from CLAUDE.md - read when working on these topics. -->

## Per-widget implementation details

Implementation-specific constants, scrollbar geometry, multiline cursor math, edit-history coalescing, IME composition state machine, dark-mode uxtheme ordinals - all live in code with comments. Heavy lifting: `hosts/crossplatform/host.cpp` (LISTBOX / COMBOBOX / TREEVIEW / MULTILINE / KNOB / GRID / overlays); undo in `hosts/shared/edit_history.h`; dark HMENU in `hosts/shared/win32/dark_menu*.h`; palette sources in the `theme_provider_*.h` files.

**xpl MULTILINE performance / large text.** Pixel hit-testing (`ml_pos_from_col`) and the wrap layout (`ml_fit_chars_end`) binary-search character boundaries (O(log K) `measure_text` calls), so click / drag-select / Up-Down / wrap on a long line cost O(K log K) shaping rather than the old O(K^2) - long lines no longer freeze the UI. `EditHistory::would_push` lets `te_history_mark` skip the full-text snapshot copy on coalesced keystrokes (no O(N) copy per character). `MultilineWidget::cached_line_starts()` caches the visual-row model (rebuilt only on text / width / font / wrap change), and `paint()` caps shaping/drawing to the visible-width span (`vis_char_cap`) + reuses a `_paint_scratch` buffer, so a frame costs O(visible) not O(line length). Known limits (deferred): wrap recomputes the whole document on each edit (no incremental reflow); the caret has no affinity at a soft-wrap boundary, so End on a wrapped row shows the caret at the start of the next visual row.

**xpl widget-level hover / pressed visuals.** `WidgetData::hovered` / `pressed` flags (`host.h:57-58`) are maintained by `Session::set_hovered` / `set_pressed`, which unconditionally invalidate the owning frame on every transition (not just for state-filtered compounds). `ButtonWidget::paint`, `CheckboxWidget::paint`, and the collapsed `ComboBoxWidget::paint` shade their fill via `neui_detail::shade(bg, ±16)` - pressed wins over hovered. `ListItemsWidget` and `TreeviewWidget` carry a per-widget `hover_row` (UINT32_MAX = none) updated in their `on_mouse_event` MOUSE_MOVE branch; `paint_scrollable_list` and `TreeviewWidget::paint` paint a subtle `shade(bg, +14)` background on the hovered unselected row, gated on `this->hovered` so leaving the widget naturally clears the highlight. Scrollbar-column hits and sb-drag-start clear `hover_row`. `ButtonWidget::on_keydown` activates on Space / Enter with a synthetic `MOUSE_BUTTON_CLICK` (no modifier filter, mirroring CheckboxWidget's keyboard pattern). Native hosts get hover / pressed / keyboard activation for free from the underlying control classes.

**xpl win32 DBLCLK→CLICK parity.** `platform_win32.cpp::WM_LBUTTONDBLCLK` mirrors the WM_LBUTTONDOWN setup (`set_focus` / `set_pressed` / `SetCapture`) so the trailing WM_LBUTTONUP fires `MOUSE_BUTTON_CLICK` on widgets that don't opt into DBLCLICK (BUTTON in particular). Without this, the system's CS_DBLCLKS-driven replacement of the second rapid DOWN with DBLCLK would silently swallow every-other rapid click. Widgets that explicitly handle DBLCLICK (CHECKBOX, TREEVIEW, MULTILINE, GRID, KNOB-reset) are unaffected - they now just see an additional trailing CLICK they already ignore. Matches the native win32 host where the system "Button" class has no CS_DBLCLKS and every fast click is a plain click.

## Enabled / disabled state

`widgets->set_enabled(sess, w, bool)` / `get_enabled(sess, w)`. Default enabled=true. Disabled widgets paint dimmed and don't receive input.

- **win32 native**: `EnableWindow(hwnd, enabled)`; deferred flag applied in `create_child_windows` if HWND not yet created.
- **xpl**: `WidgetData::enabled` flag; paint brackets `wd.paint()` with `push_alpha(0.5)`; hit-test / `collect_tab_stops` / `dispatch_mouse_event` skip disabled. Focused-then-disabled advances focus.
- **macOS native**: `apply_enabled_native_macos(wd)` - `[NSControl setEnabled:]` for control leaves, document-view disable for scroll-hosted controls, `push_alpha(0.5)` dim in `drawRect:` for painted views. Re-applied in `create_native_for_widget`.

Frames + non-interactive containers (SECTION, MENUBAR) accept the call; effect host-defined.

