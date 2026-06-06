# CLAUDE.md

Guidance for Claude Code in this repository.

## Project Overview

**neuilib** is an early-stage C/C++ GUI framework separating a client C API from platform host implementations. Windows + macOS are fully implemented; Linux falls back to a null platform layer. No test suite, no linter.

## Build

CMake 3.15+, C++17 (MSVC on Windows, AppleClang on macOS).

```bash
# Windows / Linux
cmake -B out/build -DCMAKE_BUILD_TYPE=Debug && cmake --build out/build

# macOS (Xcode generator - multi-config; IDE + lldb integration)
cmake -B out/build -G Xcode && cmake --build out/build --config Debug
```

Outputs - Windows: `out/build/Debug/{neui_example.exe, neui.lib, neui-win32host.lib, neui-xplhost.lib, neui-backend-d2d.lib}`. macOS: `out/build/Debug/{neui_example.app, libneui.a}` + per-subdir `libneui-*.a`. Example apps: `neui_example`, `neui_grid_example`.

## Per-platform host + backend selection

- **Windows**: `neui-win32host` + `neui-xplhost`; backend `neui-backend-d2d`; xpl platform `platform_win32.cpp`.
- **macOS**: `neui-macoshost` + `neui-xplhost`; backend `neui-backend-cg`; xpl platform `platform_macos.mm`.
- **Other**: `neui-xplhost`; backend `neui-backend-null`; xpl platform `platform_null.cpp`.

Top-level CMakeLists gates each platform-specific subdirectory; the example links the native host only when present.

## Architecture (file map)

- **Public C API** `include/neui/`: `neui.h` (`neui_init` + `neui_register` + `neui_get_api`); sub-headers under `d/`: `api.h`, `keys.h`, `widgets.h`, `events.h`, `items.h`, `tree.h`, `attrs.h`, `clipboard.h`, `commands.h`, `renderer.h`, `painter.h`, `assets.h`, `compound.h`, `behavior.h`, `grid.h`, `menu.h`, `theme.h`.
- **Core library** `src/neui.c`: host registry + `neui_init()` (fans out to per-host registration wrappers gated by `NEUI_HAS_*HOST` defines CMake sets on the `neui` target).
- **Shared portable utilities** `hosts/shared/`, header-only, ODR-safe via `inline`: `tree.h` (`Tree<T>` slot-reused), `attrs.h` (`AttrBag` + `attr_as_float` + `k_well_known_attrs`), `clipboard_item.h`, `edit_history.h`, `shortcut_format.h`, `theme_palette.h` (`ColorRole` / `Palette` / `current_palette()` / `ScopedPaletteOverride`), `compound.h` (`CompoundLayer` / `CompoundAsset` / `CompoundBinding` + `{key}` parser + 9-pt anchor resolver), `behavior.h` (`BehaviorAsset` / `BehaviorHandler` + prop setters), `behavior_runtime.h` (`BehaviorRuntime` drag state + `BehaviorDispatchCtx` seam + `behavior_dispatch_mouse`/`_key` + sweep/fine/snap math), `grid_model.h` (column/row/scroll/selection model + viewport / hit-test + smooth-scroll & rubber-band math), `widget_paint_grid.h`, `scrollbar.h`, `painter.h` (`neui_painter` + `k_painter_api`), `widget_font.h` (`EffectiveFont` + `read_/push_/pop_widget_font`), `widget_paint_knob.h` / `_section.h` / `_compound.h`.
- **Shared platform-specific** `hosts/shared/win32/` (clipboard, accel table, icon, theme provider + brushes, dark menu / menubar) and `hosts/shared/macos/` (clipboard, theme provider, image loader, keys, menubar).
- **Win32 Host** `hosts/win32/`: native HWND host. `window.cpp` WinMain + pump; `host.cpp` Session + `get_interface`; `widgets.cpp` full API; `host.h` `WidgetData`; `asset_manager_w32.h` `W32AssetManager`.
- **macOS Host** `hosts/macos/`: native AppKit host (`neui.host.macos`). `host.{h,mm}` Session; `widgets.mm` full API; `window.mm` NSApp + `NEUINativeContentView` (`isFlipped=YES`) + `NEUINativePaintedView` (per-widget CG context; IMAGE / KNOB / SECTION / CUSTOMDRAW / GRID via per-type `drawRect:` branch); `asset_manager_macos.h` `MacOSAssetManager`. The painted view forwards raw input for CUSTOMDRAW as `NEUI_EVENT_MOUSE_*` / `_KEY*`, and runs internal handling for KNOB (drag + wheel + reset popup) and GRID (selection ladder + nav + smooth scroll). Per-ctx GPU cache dropped in `release_native_control_macos`.
- **Crossplatform Host** `hosts/crossplatform/`: polymorphic widget hierarchy (`neui.host.crossplatform`). `host.{h,cpp}` `WidgetData` base + per-type subclasses (`FrameWidget` … `GridWidget`) with virtuals (`paint` / `paint_after_children` / `on_keydown` / `on_keychar` / `on_mouse_event` / `hit_test` / `on_destroy` / `on_composition` / `is_frame` / `is_menubar` / `perform_command` / `can_perform_command` / `grid_model_ptr`). `widgets.cpp` full API + `make_widget()` factory. `platform.h` cross-cutting seam (window / menubar / image / clipboard / IME / modal / focus); per-OS impls `platform_{win32.cpp,macos.mm,null.cpp}`.
- **Backends** `backends/`: `d2d/` (Direct2D, `ID2D1HwndRenderTarget`), `cg/` (CoreGraphics, one `CGContextRef` per frame via `set_current_frame` from `drawRect:`), `null/` (no-op).

## Rendering Backend (`d/renderer.h`)

`neui_render_backend_t` is the interface backends fill in: context lifecycle + `resize`, `begin_frame`/`end_frame`, `fill_rect`/`draw_rect`, `draw_text`/`measure_text`, `get_scale_factor`/`update_dpi`, nestable `push_clip`/`pop_clip`, `create/destroy/draw_bitmap` (host-internal; clients use `painter_api->draw_asset`), path API, transform stack, `get_context_generation` (bumped on device-loss; cached resources re-upload on mismatch), **alpha stack** `push_alpha`/`pop_alpha` (cumulative 0..1 opacity; D2D + CG software stack, no-op null), **font stack** `push_font`/`pop_font` ((family, weight); size per-call; d2d `IDWriteTextFormat` + cg `CTFont`/`NSFont`, no-op null). Transform / alpha / font stacks reset at every `begin_frame`. Coordinates: logical px at 96 DPI. Colour `0xAARRGGBB`.

## Events (`d/events.h`)

App: `APP_QUIT`. Mouse: `MOUSE_MOVE/ENTER/LEAVE`, `MOUSE_BUTTON_DOWN/UP/CLICK/DBLCLICK`, `MOUSE_RBUTTON_DOWN/UP`, `MOUSE_WHEEL`. Key: `KEYDOWN/KEYCHAR/KEYUP`. Widget: `WIDGET_UPDATED/PREUPDATE/FOCUS/PAINT`, `CHECKBOX_CHANGED`, `RESIZE`, `VALUE_CHANGED`, `ATTR_CHANGED`. Item: `ITEM_SELECTED`. Tree: `TREE_ITEM_SELECTED/ACTIVATED`. Grid: `GRID_ROW_SELECTED`, `GRID_CELL_SELECTED`, `GRID_CELL_CLICKED`, `GRID_ROW_ACTIVATED`, `GRID_COLUMN_RESIZED`, `GRID_SORT_CHANGED`, `GRID_CELL_EDIT_BEGIN`, `GRID_CELL_CHANGED`, `GRID_CELL_EDIT_CANCEL`. DnD: `DND_ENTER`, `DND_MOVE`, `DND_LEAVE`, `DND_DROP`.

`WIDGET_PAINT` fires only on `NEUI_W_CUSTOMDRAW` (and only when no compound asset is attached). `CHECKBOX_CHANGED` fires on every user-driven toggle on all three hosts. `VALUE_CHANGED` is the widget-scoped (no attr name) user-driven event for the native KNOB / SLIDER; `ATTR_CHANGED` (`{ widget, attr_key, value }`) is the parallel event a behavior asset fires when it writes through. `KEYDOWN.modifiers` + accelerator modifiers share `NEUI_KMOD_*` bits. `NEUI_MK_*` mouse-modifier bits (matching Win32 `MK_*`) live in `<neui/d/events.h>` so behavior handlers can read `mouse.buttonmap` portably; `NEUI_MK_ALT` reserved but not yet populated.

## Per-widget implementation details

Implementation-specific constants, scrollbar geometry, multiline cursor math, edit-history coalescing, IME composition state machine, dark-mode uxtheme ordinals - all live in code with comments. Heavy lifting: `hosts/crossplatform/host.cpp` (LISTBOX / COMBOBOX / TREEVIEW / MULTILINE / KNOB / GRID / overlays); undo in `hosts/shared/edit_history.h`; dark HMENU in `hosts/shared/win32/dark_menu*.h`; palette sources in the `theme_provider_*.h` files.

## Clipboard

`NEUI_API_CLIPBOARD`: convenience `set_text` / `get_text` / `has_text`; item-based `read`, `create_item`, `release`, `write`, `item_set_format(mime, data, len)`, `item_get_format`, `item_has_format`. Items use the shared `neui_data_item_t` (same primitive that backs DnD payloads). Built-in MIMEs round-tripped through the OS: `NEUI_MIME_TEXT = "text/plain;charset=utf-8"` (kept as `NEUI_CLIPBOARD_MIME_TEXT` alias), `NEUI_MIME_HTML = "text/html"`, `NEUI_MIME_URI_LIST = "text/uri-list"` (RFC 2483 `file:///...\r\n` lines, URL-encoded). Arbitrary MIMEs (any string containing `/`) pass through as opaque bytes - `RegisterClipboardFormatA(mime)` on Win32, the MIME string as a pasteboard UTI type on macOS. There is no push-style onchange callback; clients poll `has_text` / `item_has_format` on demand (e.g. inside a menu's WM_INITMENUPOPUP / NSMenuValidation handler) to gate Paste-like UI. Per-session `DataItemStore` (`hosts/shared/clipboard_item.h`) backs both clipboard items and transient DnD drop payloads. xpl text widgets handle Ctrl+C/X/V via `xpl_host::platform_clipboard_*`; native `Edit` (Win32) does it automatically. macOS native: `NEUINativeContentView::performKeyEquivalent:` routes ⌘C/⌘X/⌘V/⌘A/⌘Z/⌘⇧Z through `invoke_focused_command_macos` to the focused field editor (client menu key-equivalents still win, since NSApp matches the main menu first). Win32 CF_HTML descriptor + CF_HDROP <-> uri-list translation live in `hosts/shared/win32/clipboard_format_html_win32.h` / `clipboard_format_urilist_win32.h`; macOS multi-format read/write in `hosts/shared/macos/clipboard_macos.h`.

## Drag & drop

`NEUI_API_DND` (`include/neui/d/dnd.h`). Both drop-target reception and drag-source initiation are wired on all three hosts. API: `set_drop_target(widget, bool)`, `get_drop_target`, `set_accepted_formats(widget, mimes[], count)` (NULL/0 = accept any), the synchronous `accept(session, neui_dnd_action_t)` (`NONE=0 / COPY=1 / MOVE=2 / LINK=4`, bitmask matching Win32 `DROPEFFECT_*`), and `begin_drag(session, source_widget, data_item, allowed_actions) -> neui_dnd_action_t` (blocking - spins the OS drag loop until drop or cancel; returns the negotiated action or NONE on cancel). Per-widget `drop_target` flag + `accepted_mimes` vector on `WidgetData`.

Events (`include/neui/d/events.h`, category `0x0007`): `NEUI_EVENT_DND_ENTER / MOVE / LEAVE / DROP`. Payload `neui_event_dnd_t { widget, x, y, buttonmap, formats, formats_count, data, suggested_action }`. `x/y` are widget-local logical pixels; `formats` is a borrowed dispatch-scoped list of MIME strings; `data` is `neui_data_item_none` on ENTER/MOVE/LEAVE and a live item id on DROP. The data-item is released the instant the callback returns, so clients copy bytes via `clipboard_api->item_get_format` during dispatch if they want to keep them. The client signals what it will accept by calling `dnd->accept(session, action)` from inside ENTER/MOVE (and DROP) - cached in `Session::_last_accepted_action` and reported back to the OS pasteboard (cursor changes accordingly).

Platform seam (`hosts/crossplatform/platform.h`): `platform_dnd_register_window(native_handle, session_ptr, frame_widget_id)` / `platform_dnd_unregister_window` for drop-target; `platform_dnd_begin_drag(native_handle, item, allowed_actions)` for drag-source. Drop-target registration runs from `widget_show` on APPWINDOW / PLUGWINDOW / DIALOG; revoke on `widget_destroy`. Drag-source spins on demand.

- **Win32** (`hosts/shared/win32/dnd_target_win32.h` + `dnd_source_win32.h`): one-time `OleInitialize` from `platform_init`. **Receive**: each frame HWND owns an `IDropTarget` COM object that pulls MIME bytes from `IDataObject` (probes `CF_UNICODETEXT`, `CF_HTML`, `CF_HDROP`, then enumerates registered MIME-named formats) and calls back into `Session::dispatch_dnd_*`. xpl host wires this via `platform_win32.cpp`; native host wires it inline in `hosts/win32/widgets.cpp::register_frame_as_drop_target_w32`. Child HWNDs don't register their own `IDropTarget` - the OS walks up the parent chain and the frame catches all drops in its client area; the framework hit-tests the widget tree itself. **Do not mix `WS_EX_ACCEPTFILES` with `RegisterDragDrop` - the two paths conflict; no neui widget opts into the former.** **Send**: `DataObjectImpl : IDataObject` (read-only, format snapshot pre-encoded at construction: `CF_UNICODETEXT` for `text/plain`, `CF_HTML` via `clipboard_encode_cf_html`, `CF_HDROP` via inline `DROPFILES` build, arbitrary MIMEs via `RegisterClipboardFormatA`) + `DropSourceImpl : IDropSource` (default cursors; cancel on Esc; drop on left-button release) + small `EnumFORMATETCImpl`. `DoDragDrop` runs on the calling thread.
- **macOS** (`hosts/shared/macos/dnd_source_macos.h`): **Receive**: `NEUIView` (xpl host) and `NEUINativeContentView` (native host) conform to `<NSDraggingDestination>`; `registerForDraggedTypes:` covers string / HTML / file-URL. Drag location is converted via `convertPoint:fromView:nil` (content views are `isFlipped=YES`, so Y is already top-down). Per-content-view registration matches Win32 frame-level scope; AppKit routes the topmost-view callback, but the framework owns hit-test via `Session::dispatch_dnd_*`. **Send**: `NEUIDragSource : NSObject<NSDraggingSource>` captures the negotiated `NSDragOperation` when `draggingSession:endedAtPoint:operation:` fires; `macos_run_drag_source` builds `NSDraggingItem`s (one shared `NSPasteboardItem` for text/HTML/MIME + one item per URL for `text/uri-list`), calls `beginDraggingSessionWithItems:event:source:`, then spins a self-contained runloop pump (`dnd_pump_until(&src->done)` in the shared header - not the xpl-only `platform_run_modal_until` seam, which the native macOS host doesn't implement) to match the synchronous Win32 contract. The header's `@implementation NEUIDragSource` is guarded by `NEUI_DND_SOURCE_MACOS_IMPLEMENTATION` so exactly one co-linked TU (xpl `platform_macos.mm`) emits the class body. `NSDragOperation` constants do NOT match `DROPEFFECT_*` numerically; explicit mapping in `nsop_to_dnd_action`.

Session DnD dispatch: `dispatch_dnd_enter / move / leave / drop`. The xpl host walks descendants of `frame_widget_idx` (using cached `abs_x` / `abs_y` on each xpl WidgetData), picks the deepest visible+enabled `drop_target` whose `accepted_mimes` intersects the drag's `formats`, falls back to the frame itself if nothing deeper matches. The win32 native host walks the same tree, accumulating each WidgetData's parent-relative `x` / `y` into frame-local absolute coords on the fly (`Session::find_drop_target_in_frame_w32`); cached `_current_drop_abs_x/y` keep MOVE / LEAVE coords consistent without re-walking. The macOS native host walks the same tree (`Session::find_drop_target_in_frame_macos` + `_current_drop_abs_x/y`); `NEUINativeContentView` remains the single `<NSDraggingDestination>` (no per-painted-view opt-in) but the framework hit-tests the widget tree in software, so per-widget drop targeting matches win32 native + xpl. Re-targeting fires LEAVE on the previous target + ENTER on the new one before MOVE.

Re-entry: `Session::_in_dnd_dispatch` blocks `begin_drag` calls made from inside a DnD callback (would crash the OS drag loop). Drop targets in the same session DO receive ENTER / MOVE / LEAVE / DROP normally while a drag-source spin is in flight, so an internal drag (left pane -> right pane in the same window) works.

Verification: `examples/dnd_example.cpp` (drop receiver) builds `neui_dnd_example.exe`. `examples/dnd_source_example.cpp` (source + receiver side-by-side) builds `neui_dnd_source_example.exe` - left pane initiates `begin_drag` past a 5 px threshold; right pane accepts the drop; status label reports `copy` / `move` / `link` / `cancelled`. Both internal drags and external drags to other apps work.

## Attribute API

`NEUI_API_ATTRS`. String-keyed bag per widget (`std::unique_ptr<AttrBag>` on `WidgetData`, lazy). API: `set_int`/`get_int(default)`, `set_float`/`get_float(default)`, `set_string`/`get_string`, `has`, `remove`. Type-strict: wrong-kind returns the default. Well-known keys are debug-asserted to match their documented kind at set time via `k_well_known_attrs` (`hosts/shared/attrs.h`); release silently stores the wrong kind so reads keep returning the default. **A new `NEUI_ATTR_*` / `NEUI_PARAM_*` macro needs a matching row in `k_well_known_attrs`.** Session-level: `set_session_int`/`get_session_int`. `NEUI_ATTR_THEME_MODE` is the only session key with behaviour today.

**Well-known keys** (all `neui.attr.<name>`; macros `NEUI_ATTR_*`):

| Key | Type | Applies | Notes |
|---|---|---|---|
| `tristate` | int | CHECKBOX | Implicit on CHECKBOX3. |
| `multiline` | int | INPUTBOX | Implicit on MULTILINE. |
| `readonly` | int | INPUTBOX, MULTILINE | Gates modifying keys. |
| `password` / `border` / `align_text` | int / int / string | various | Reserved — except `align_text` on SECTION, live. |
| `tab_stop` | int | focusable | Replaces deprecated `widgets->set_tab_stop`. |
| `min_width` / `min_height` / `max_width` / `max_height` | int (logical px) | APPWINDOW | Drives `WM_GETMINMAXINFO` / `NSWindow.min/maxSize`. `max < min` = no maximum. |
| `icon_path` | string | APPWINDOW | `.ico` / `.png` / `.bmp` / `.jpg`. Live. |
| `modal` | int | DIALOG | `1`/unset → owner disabled. Read once at `widget_show`. |
| `background` | int ARGB | KNOB, IMAGE, painted widgets, frame on xpl | Honoured unconditionally (independent of follow-system-theme). |
| `follow_system_theme` | int bool | APPWINDOW, PLUGWINDOW, DIALOG | `1` = DWM dark + dark HMENU + theme-aware `WM_CTLCOLOR*` + invalidate on theme flip; `0`/unset = OS-default chrome, no auto-invalidate. Default off. |
| `rotation` | float (rad) | IMAGE | Around dest centre. Positive = clockwise (Y-down). Live. |
| `polarity` | string | KNOB | `"min"` (default) / `"center"` / `"max"`. Anchor end of fill arc. |
| `steps` | int | SLIDER, KNOB | `>=2` snaps to N positions on `[0..1]` + draws ticks; `<2` continuous. Also snaps programmatic `set_float(NEUI_PARAM_VALUE)`. |
| `orientation` | string | SLIDER | `"horizontal"` (default) / `"vertical"`. Read at `widget_show`. |
| `value_text` | string | KNOB | Overlay text below the disc. Read each paint. |
| `knob_mode` | int | KNOB | `NEUI_KNOB_MODE_ROTATIONAL=0` (default) / `_VERTICAL=1` / `_HORIZONTAL=2`. Cached at mouse-down. |
| `font_family` / `font_size` / `font_weight` | string / float px / int | text-bearing | Empty/unset = host default. Weight CSS 100..900 (400 Normal, 700 Bold). Honoured by d2d + cg backends + native controls (win32 `HFONT`, macOS `NSFont`; unknown families fall back); null ignores. Italic not exposed. Live. |
| `theme_mode` | int session-level | session | AUTO (0) follows OS; LIGHT (1) / DARK (2) force the palette. Accent stays live. |
| `grid.row_height` | int (px) | GRID | Body row height. Default 22. |
| `grid.header_height` | int (px) | GRID | Sticky header band height. Default 24. |
| `grid.focus_row_color` | int ARGB | GRID | Focus-row band colour. 0/unset = accent @ 0x33 alpha. |
| `grid.show_focus_row` | int bool | GRID | Paint focus-row highlight. Default 1. |
| `grid.column_min_width_default` | int (px) | GRID | Default per-column min width. Default 24. |
| `grid.cell_focus` | int bool | GRID | 0 = row-focus (default); 1 = cell (row,col) cursor. Live. |
| `grid.scroll_mode` | int | GRID | Wheel kinetics. `NEUI_GRID_SCROLL_PLATFORM=0` (default - macOS = smooth, Win32/null = stepped), `_STEPPED=1` (row-quantized, hard-clamp), `_SMOOTH=2` (pixel-precise + rubber-band + 60 Hz spring-back). Live. |

Namespace `neui.attr.*` reserved; clients use their own. Host-specific reserved: `neui.win32.*`, `neui.macos.*`, `neui.linux.*`. Unknown keys stored but inert.

## Routed commands

`NEUI_API_COMMANDS`. `neui_command_t`: `NONE/UNDO/REDO/CUT/COPY/PASTE/SELECT_ALL/DELETE`, `USER_BASE = 0x10000`. API: `invoke_focused(cmd) → bool`, `invoke(widget, cmd) → bool`. `tree->set_menu_cmd(menubar, item, cmd)` binds a menu item to a built-in command. On activation, `dispatch_menu_event` calls `invoke_focused_command(cmd)` first; if a focused widget consumes it, no `TREE_ITEM_ACTIVATED` fires (otherwise, or `cmd == 0` / `cmd >= USER_BASE`, the event reaches the client). `WidgetData::perform_command(cmd)` is the virtual seam; xpl text widgets route to `on_keydown` with a synthetic Ctrl+letter; macOS native (`invoke_focused_command_macos`) routes to the first responder via AppKit editing actions + `NSUndoManager` for UNDO/REDO.

Menu-item auto-disable on popup-open: built-in commands gray via `WidgetData::can_perform_command`; optional `neui_menu_client_t` (`NEUI_API_MENU_CLIENT`) `validate(token, menubar, item, cmd)` per non-separator item. Final `enabled = mi.enabled && (no built-in OR can_focused) && (no validate OR validate())`.

## Popup menus

`widgets->popup_menu(session, anchor, x, y, items[])` - blocking; items NULL-terminated UTF-8 (`"-"` = separator); returns 1-based pick or 0. Win32: `TrackPopupMenuEx` + `TPM_RETURNCMD`. macOS: `run_popup_menu_macos` (`NSMenu`, per-item target; separators consume an index). xpl: session overlay + nested pump via `platform_run_modal_until(bool*)`. Used by the KNOB right-click "Reset to default" menu (`NEUI_EVENT_MOUSE_RBUTTON_DOWN`).

## Keyboard shortcuts and accelerators

`tree->set_shortcut(menubar, item, modifiers, key)` (`NEUI_KMOD_*` bits + `NEUI_KEY_*`). `NEUI_KEY_NONE` clears. Display formatted by `shortcut_format.h`, appended after `\t`. Win32 builds an HACCEL (`accel_table_win32.h`), walked from the pump via `try_translate_accel(MSG*)` before `TranslateMessage` (and before `IsDialogMessage` on xpl). macOS sets `NSMenuItem.keyEquivalent` + `keyEquivalentModifierMask`. `NEUI_KMOD_CTRL` = platform-primary (Cmd on macOS, Ctrl on Win/Linux); `NEUI_KMOD_META` = secondary (Control on macOS).

## Enabled / disabled state

`widgets->set_enabled(sess, w, bool)` / `get_enabled(sess, w)`. Default enabled=true. Disabled widgets paint dimmed and don't receive input.

- **win32 native**: `EnableWindow(hwnd, enabled)`; deferred flag applied in `create_child_windows` if HWND not yet created.
- **xpl**: `WidgetData::enabled` flag; paint brackets `wd.paint()` with `push_alpha(0.5)`; hit-test / `collect_tab_stops` / `dispatch_mouse_event` skip disabled. Focused-then-disabled advances focus.
- **macOS native**: `apply_enabled_native_macos(wd)` - `[NSControl setEnabled:]` for control leaves, document-view disable for scroll-hosted controls, `push_alpha(0.5)` dim in `drawRect:` for painted views. Re-applied in `create_native_for_widget`.

Frames + non-interactive containers (SECTION, MENUBAR) accept the call; effect host-defined.

## Frame resize, window icon, focus

Resize: Win32 `WM_SIZE` (skip `SIZE_MINIMIZED`; physical→logical via `MulDiv(phys, 96, dpi)`); macOS `windowDidResize:`. Both emit `RESIZE { widget, width, height }` in logical px. Min/max attrs drive `WM_GETMINMAXINFO` / `NSWindow.min/maxSize`. `WM_DPICHANGED` triggers a follow-up `WM_SIZE`. Icon (`NEUI_ATTR_ICON_PATH`): Win32 `WM_SETICON`; macOS `NSApp.applicationIconImage`.

Focus: clients see **logical** focus only. Tab traversal hand-rolled on xpl (`Session::_focused_widget`, `focus_next`); Win32 uses `IsDialogMessage` + `WS_TABSTOP`; macOS builds the key-view loop in **widget-creation order** (`rebuild_key_view_loop_macos`: pre-order DFS over the tab-stop set - BUTTON / INPUTBOX / MULTILINE / CHECKBOX[3] / LISTBOX / COMBOBOX / TREEVIEW / SLIDER / CUSTOMDRAW / GRID, honouring `NEUI_ATTR_TAB_STOP=0`; scroll-hosted controls use their document view), chained via `setNextKeyView:` (`autorecalculatesKeyViewLoop` off). Frame focus → `WIDGET_FOCUS`. `Session::_os_focused` false → paint reports "no focus" (caret + outline hide; logical state preserved).

## Theme palette

Process-wide `neui_detail::Palette` (`theme_palette.h`) - flat array indexed by `ColorRole` (frame_bg, panel_bg, control_bg, accent, text_primary, border, scrollbar_*, ime_underline_*, …); Win32 + macOS providers populate from system sources and fire `Session::on_theme_changed` on flips.

**Per-session override** (`NEUI_ATTR_THEME_MODE`): Session computes `_effective_palette` per mode and points `active_palette_override_ptr()` at it; `current_palette()` consults it first. AUTO copies system; LIGHT/DARK = defaults + live system accent. **Per-frame opt-in** (`NEUI_ATTR_FOLLOW_SYSTEM_THEME = 1`): DWM dark title bar + dark HMENU + palette-driven brushes + invalidate on theme flip; without it OS-default chrome, no auto-invalidate.

**Win32 manifest** (`examples/neui_example.manifest`) declares Win10/11 `supportedOS` GUIDs + Per-monitor v2 DPI + UTF-8 ACP (without the GUIDs Windows gates off uxtheme dark mode). `NEUI_API_THEME_CLIENT` - optional client theme-change callback, fires after framework invalidation.

## Key Design Patterns

- **Host registry** - `neui_register(id, api)` at startup; `neui_get_api(NULL)` returns first registered. IDs: `"neui.host.win32"`, `"neui.host.macos"`, `"neui.host.crossplatform"`. Clients call `neui_init()` once to register every compiled-in host (gated on `NEUI_HAS_*HOST`). Order: native first, then xpl.
- **Per-host registration wrappers** - each host static lib exposes `extern "C"` wrappers (`neui_register_xplhost` / `_win32host` / `_macoshost`) thunking to the namespaced `register_host()`; they double as the linker forced-symbol references pulling the host's objects out of its static lib.
- **Named interface dispatch** - `get_interface(sess, name)` with version suffix (`/0`). Active: `NEUI_API_WIDGETS/_ITEMS/_TREE/_ATTRS/_CLIPBOARD/_DND/_COMMANDS/_ASSETS/_COMPOUND/_BEHAVIOR/_GRID`. Optional client-side: `_MENU_CLIENT`, `_THEME_CLIENT`, `_GRID_CLIENT`.
- **Session model** - 32-bit ID; slot-reused vector. Client passes `neui_client_t` with `get_interface` callback; host passes opaque token back on every callback.
- **Widget IDs** - upper 16 = owning session id, lower 16 = tree slot. Every API entry validates via `get_session_for_widget`; cross-session handles silently dropped. Sentinels (`widget_root = 0`, `widget_none = UINT32_MAX`) pass. Stale-after-slot-reuse not detected (deferred).
- **Deferred HWND** (win32) - logical state stored immediately; HWND / HMENU / HACCEL / HICON created on `widget_show()`; pending state flushed in `create_child_windows()`. Guard every API call with `hwnd == nullptr`.
- **Event routing** - host → client via `neui_widget_client_t::onevent()`. Client gets first chance for mouse events; false forwards to `widget->on_mouse_event()`.
- **`emit_events` gate** - `widget_at()` and `dispatch_mouse_event()` both require `emit_events = true`. Auto-set for BUTTON, INPUTBOX, CHECKBOX, CHECKBOX3, LISTBOX, COMBOBOX, MULTILINE, TREEVIEW, CUSTOMDRAW, KNOB, SLIDER, GRID.
- **Coordinates** - logical pixels at 96 DPI; child x/y relative to the immediate parent's top-left on all hosts. Win32: HWND parenting. macOS: NSView subview parenting via `create_descendants_native` (recognises containers like SECTION). xpl: parent-relative `x`/`y` + frame-local `abs_x`/`abs_y` recomputed each frame; the paint walk pushes `translate(wd.x, wd.y)` around the descent.
- **`NEUI_ABI` (`__cdecl`)** on all API function pointers. **`DEF_` prefix on event macros** avoids Windows SDK `MOUSE_EVENT` / `KEY_EVENT` collision. **`interface` reserved by MSVC** - parameters use `iface`.
- **Type-as-default + attributes-override** - implicit variants (CHECKBOX3, MULTILINE) set their attr at create; runtime reads the attr. Public type strings stay even though they map to base+attr internally.
- **Vtable-append for evolution** - append new methods at the end of any public-API struct so slot offsets stay stable. Pre-1.0: slots can change when all hosts rebuild.
- **`platform_clipboard_*` / `platform_menubar_*`** are the cross-cutting seams; xpl text widgets + public clipboard API call them rather than into `hosts/shared/{win32,macos}/*` directly.

## Widget Types

| Type | Notes |
|---|---|
| `APPWINDOW` | Top-level; participates in quit-on-close. |
| `PLUGWINDOW` | Borderless `WS_POPUP`; not in quit count. |
| `LABEL` | Text only, no fill. |
| `BUTTON` | Centered text + 1px border. |
| `INPUTBOX` / `MULTILINE` | Cursor / selection / undo / clipboard / word nav. `MULTILINE` = INPUTBOX + `multiline=1`. |
| `CHECKBOX` / `CHECKBOX3` | 2-state vs 3-state cycle (`tristate=1` on CHECKBOX3). |
| `LISTBOX` / `COMBOBOX` | COMBOBOX: hover ≠ selection until commit (Enter / click). |
| `TREEVIEW` | Per-item expanded; chevrons; Treeview keys. |
| `MENUBAR` | No HWND. `tree->set_shortcut` + `tree->set_menu_cmd` configure items. |
| `SLIDER` | Horizontal / vertical via `orientation`; `steps` snaps. |
| `KNOB` | Painted rotary; right-click → "Reset to default" popup; drag mode via `NEUI_ATTR_KNOB_MODE`. |
| `IMAGE` | Source = file path (`set_text`) OR pre-loaded handle (`set_asset`); last-set-wins, `""` / `asset_none` clears. Aspect-preserving fit; honours `NEUI_ATTR_ROTATION`. No refcount on the asset - clear/destroy widget before `assets->destroy`. |
| `SECTION` | Non-interactive container. Body filled with `NEUI_ATTR_BACKGROUND` (fallback `shade(frame_bg, +24)`; macOS flips lift direction when it saturates light bg). Optional `set_text` header as a "title chip" (rest of band transparent), positioned via `NEUI_ATTR_ALIGN_TEXT`. `emit_events=false`. Children paint on top. |
| `CUSTOMDRAW` | Client-rendered surface. Emits `WIDGET_PAINT` each frame with `neui_painter_api_t* painter_api` + opaque `neui_painter_t* p` + widget-local size + focus. Origin (0,0) widget-local; framework wraps dispatch in `push_transform / push_clip(bounds) / pop_*`. MOUSE / KEY flow normally. All three hosts. Invalidate via `widgets->invalidate`. Also accepts a **compound asset** (visual) and **behavior asset** (input) via `widgets->set_asset` (kind-routed); the two are independent and compose. |
| `GRID` | Scrollable multi-column table; cells are paint-state, not widgets. Per-column `editable` flag opens an in-place editor on ENTER in cell-focus mode; optional `NEUI_API_GRID_CLIENT::validate_cell` gates the commit. See below. |

## Painter + asset API

Two public interfaces back `NEUI_W_CUSTOMDRAW`:

**`neui_painter_api_t`** (`include/neui/d/painter.h`) - curated drawing surface handed to clients via `WIDGET_PAINT`: the draw-safe subset of `neui_render_backend_t` (shapes, path API, transform/clip/alpha/font state stacks, `get_scale_factor`/`measure_text`, handle-based `draw_asset`), excluding context lifecycle + raw bitmap create/destroy. Opaque `neui_painter_t*` (`hosts/shared/painter.h`) carries `backend`/`ctx`/`host_token`/`draw_asset_thunk`, stack-allocated per dispatch. Vtable singleton `neui_detail::k_painter_api`.

**`neui_asset_api_t`** (`include/neui/d/assets.h`, `NEUI_API_ASSETS`) - session-scoped media handles loaded outside the paint loop: `create_bitmap`, `create_from_file` (resolves `@2x`/`@3x`), `destroy`, `get_size`, `get_kind`, `create_compound`. Returns `neui_asset_t` (handle `(session_id << 16) | slot`; cross-session rejected). `neui_asset_kind_t`: `BITMAP=1`, `COMPOUND=2`, `BEHAVIOR=3` (SVG / vector / font reserved). Clients reach bitmap draws via `painter_api->draw_asset` (lazy per-(asset, ctx) upload, cached for the ctx lifetime).

**Asset storage**: xpl `neui_detail::AssetManager`, win32 `W32AssetManager`, macOS `MacOSAssetManager` - all slot-vector + free-list + per-ctx GPU cache, released when the owning paint context dies. **Device-loss** (D2D only): `EndDraw` → `D2DERR_RECREATE_TARGET` rebuilds next `begin_frame` from stashed `hwnd`/`width`/`height`/`dpi`; per-ctx `generation` bump; asset caches keyed by `(ctx, generation)` re-upload on mismatch (CG + null return a constant generation).

## Compound drawables

A **compound** is `NEUI_ASSET_KIND_COMPOUND` - a mutable, declarative shape attachable to a CUSTOMDRAW widget. Owns a slot-reused layer stack; each layer is `text` or `asset` (v1), positioned by a 9-point anchor pair (parent + self) + `(offset_x, offset_y)` + `(width, height)` with `NEUI_COMPOUND_FILL = -1` per axis to span; signed-int `z` interleaves with the widget's children (`z<0` below, `z>=0` above). When attached, `WIDGET_PAINT` is suppressed.

**API** (`include/neui/d/compound.h`, `NEUI_API_COMPOUND`): `add_layer(kind, z)`, `remove_layer`/`clear`/`set_z`, `set_anchor`, typed `set_int`/`set_float`/`set_string`(templates)/`set_asset`, numeric `bind(prop, attr_key, scale, offset)`, `bind_asset(prop, attr_key)`, `unbind`. Props - **text**: `text` (template), `size`, `color` (optional, → `ColorRole::text_primary`), `align_x`/`align_y`; **asset**: `asset`, `rotation`; **both**: `offset_x`/`offset_y`/`width`/`height`/`alpha`/`show_when`.

**State-filtered layers** (`NEUI_PROP_SHOW_WHEN`, `set_int`): `0` (default) = always visible; otherwise an AND filter across three axes (enabled, hovered, pressed), each with a positive bit (`NEUI_LAYER_STATE_ENABLED`/`_HOVERED`/`_PRESSED`) and a negated `_NOT_*`; rule `(show_when & ~current_state) == 0` (current_state carries one bit per axis each paint). The framework invalidates on transitions only when the compound `has_state_filters`; per-host wiring populates `wd.hovered`/`wd.pressed`.

**Data plane = the widget's `AttrBag`** (no parallel store on the compound). Text layer `"{label}: {value}"` resolves keys against attrs at paint time; `bind(layer, "rotation", "value", 2π, 0)` reads `value` as float (`attr_as_float`), computes `scale*x + offset` (float→int targets round). One compound backs many widgets - shape shared, attrbag per-widget. Template `{key}` substitution, `{{`/`}}` literal braces, malformed/missing → empty/pass-through, pre-parsed at `set_string` (`{key:.2f}` reserved). `attrs->set_*` on a widget with a compound invalidates it; compound mutations invalidate every CUSTOMDRAW whose `compound_asset` matches.

**Storage + paint**: `AssetEntry` / `W32AssetEntry` / `MacOSAssetEntry` carry a `std::unique_ptr<CompoundAsset>`. Layer table + parse + geometry + binding eval in `compound.h`; paint in `widget_paint_compound.h::paint_compound_below` / `_above`. All hosts call `_below` before child descent, `_above` after - xpl wires `_above` through `paint_after_children` so z interleaves with children (native hosts call them back-to-back, so child-relative z is xpl-only).

## Interactive behaviors

A **behavior** is `NEUI_ASSET_KIND_BEHAVIOR` (`3`) - a mutable list of input handlers for a CUSTOMDRAW widget (declarative *input target*, parallel to compound's *visual*; a widget can have both). Mouse / key / wheel events run the handler list; the matching handler mutates a named float attr in the `AttrBag`; companion compound bindings re-render.

**Handler kinds** (v1): `DRAG_VERTICAL`, `DRAG_HORIZONTAL`, `DRAG_ROTATIONAL`, `DRAG_BIAXIAL`, `WHEEL`, `KEY_STEP`, `CLICK_TOGGLE`, `CLICK_CYCLE`, `CONTEXT_RESET`.

**API** (`include/neui/d/behavior.h`, `NEUI_API_BEHAVIOR`): `add_handler(kind)`, `remove_handler`, `clear`, typed `set_int`/`set_float`/`set_string`. Props - **common**: `target` (default `neui.param.value`), `target_default` (CONTEXT_RESET), `min`/`max`, `step`/`coarse`, `snap_attr` (default `neui.attr.steps`), `fine_modifier`/`fine_scale` (default 0.2), `cursor` (no-op v1); **drag**: `sweep` (200 px), `sweep_y` (BIAXIAL), `deadzone` (rotational, 4 px); **cycle**: `wrap`; **hit region**: `anchor_parent`/`anchor_self`/`offset_x`/`offset_y`/`width`/`height` (compound 9-pt anchor, defaults whole widget).

**Attachment**: `widgets->set_asset` kind-routes - BEHAVIOR → `WidgetData::behavior_asset`, COMPOUND → `compound_asset` (independent slots). **Dispatch**: hosts feed mouse / key / wheel into `behavior_dispatch_mouse` / `_key` (`behavior_runtime.h`) after the client's `onevent` returned false; a `BehaviorDispatchCtx` of host callbacks (`invalidate` / `emit_attr_changed` / `popup_menu`) keeps it platform-free. Wheel dispatch multiplies by `|delta|` (one notch = `step * lines_per_notch`). `NEUI_EVENT_ATTR_CHANGED` fires on user-driven writes; programmatic `attrs->set_*` stays silent.

## GRID widget (`NEUI_W_GRID`, `NEUI_API_GRID`)

Scrollable multi-column table (`include/neui/d/grid.h`). Cells are paint-state, not widgets (a 10000×8 grid is one widget). All three hosts; shared logic in `grid_model.h` (`GridModel` state + viewport / hit-test / clamp / ensure-visible) + `widget_paint_grid.h` (sticky header, per-column-aligned cells, focus-row band, dual scrollbars, cell-focus outline) + `scrollbar.h`.

**API** (37 thin methods over `GridModel`): column add/remove/width/min-width/align/header, row add/remove/clear/count, cell text/color/enabled/clear-overrides, selection (`set/get_selected_row`, `set/get_selected_cell`), `ensure_row_visible`/`ensure_cell_visible`, `set/get_scroll_x`, `hit_test`, plus the sort block: `set_column_sortable` / `set_column_sort_kind` / `set_sort` / `add_sort` / `clear_sort` / `get_sort_count` / `get_sort_level` / `logical_to_visual_row` / `visual_to_logical_row`.

**Two focus modes** via `NEUI_ATTR_GRID_CELL_FOCUS`: 0 = row-focus (arrows move row), 1 = cell-focus ((row,col) cursor as 1px accent outline; Left/Right move column, Home/End row endpoints, Ctrl+Home/End grid corners). **Click event ladder** (each fires only if the prior wasn't consumed): `GRID_ROW_SELECTED` → (cell-focus) `GRID_CELL_SELECTED` → `GRID_CELL_CLICKED`. Double-click / Return → `GRID_ROW_ACTIVATED`. Header-divider drag resizes a column (ew-resize cursor) → `GRID_COLUMN_RESIZED` on release (programmatic `set_column_width` does not fire it). Per-cell sparse color / enabled overrides (disabled cell paints dimmed + suppresses `GRID_CELL_CLICKED`). Keyboard nav: Up/Down, PgUp/PgDn, Home/End, Ctrl+Home/End, Left/Right, Return; one tab stop. Nav walks **visual** order (so Up / Down step through the user-visible sorted rows) while `selected_row` stays a stable logical index. Per-host glue: win32 `widgets.cpp`, macOS `window.mm` + `widgets.mm`, xpl `host.cpp::GridWidget`.

**Sorting**: multi-column stable sort over an indirection layer. `GridModel.rows` stays in insertion order; `display_order` (visual → logical) and `logical_to_visual` (inverse) live alongside the row table. Every public API row index is **logical** (data identity, stable across sorts) - including event payloads, `hit_test`, `selected_row`, and `cell_overrides` keys. Sort engine in `hosts/shared/grid_model.h` (`grid_compare_cells` for STRING / INT / FLOAT / NATURAL kinds; `grid_rebuild_display_order` via `std::stable_sort`; `grid_apply_header_click` for the three-state cycle). Stack capped at `NEUI_GRID_SORT_MAX_LEVELS = 8`; Shift+click on a 9th column FIFO-evicts the oldest level. Lazy rebuild via `sort_dirty`: row mutations and `set_cell_text` mark dirty, `paint_grid` / `grid_hit_test` callers / the public translation methods call `grid_ensure_sort_clean` before reading display order. Header click on a column whose `sortable = false` is ignored; programmatic `set_sort` / `add_sort` still works. `set_column_sort_kind` is per-column (default STRING). User-driven header clicks fire `NEUI_EVENT_GRID_SORT_CHANGED { widget, col, dir }`; programmatic `set_sort` / `add_sort` / `clear_sort` stay silent. Header paints a triangle glyph (▲ ASC / ▼ DESC) right-aligned in each sorted header cell, plus a small 1..8 level number when more than one level is active (primary in accent colour, secondary+ in `text_secondary`).

**In-place cell editing**: per-column `editable` flag (`grid->set_column_editable(grid, col, bool)`, default false). When set, pressing ENTER on a selected cell in cell-focus mode (`NEUI_ATTR_GRID_CELL_FOCUS=1`) - or double-clicking any cell in an editable column - opens a single-line text editor over the cell rect with the entire content pre-selected (so the first key replaces). Double-clicking a non-editable cell still fires `GRID_ROW_ACTIVATED` (the editor try-open falls through). Full editing keymap: arrows (+ Shift to extend selection, + Ctrl for word step), Home/End (+ Shift), Backspace/Delete (+ Ctrl for word), Ctrl+A select-all, Ctrl+C/X/V clipboard, Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z) undo/redo with a per-edit-session history. ENTER commits, ESC cancels (reverts to `orig_text`), a click on any cell other than the editing cell commits.

The single-line text-buffer primitives live in `hosts/shared/text_edit.h` (`TextEditState`, `te_*` helpers - selection-aware insert/backspace/delete/paste, caret motion with `word`+`extend` modifiers, undo round-trip, UTF-8 walking, word boundaries, codepoint encoding). Shared by every text-input surface in the codebase today: GRID cell editor, `InputBoxWidget` (xpl host), `MultilineWidget` (xpl host, single-line bits - up/down + per-line Home/End + Return-inserts-newline stay local). Future KNOB double-click value entry will plug in the same way. `GridEditState` (on `GridModel`) embeds a `TextEditState` plus `orig_text` and a per-edit `EditHistory`. `grid_begin_edit` resets the buffer + selects all + clears history; `grid_end_edit` returns the working text and resets. The paint overlay in `widget_paint_grid.h` paints cell-bg fill + 2 px accent border + selection rectangle (`accent_translucent`) + working text + 1 px caret, using `measure_text` to find selection extents.

Per-host dispatch glue: xpl `GridWidget::on_keydown / on_keychar / on_mouse_event / on_focus_change`; win32 `painted_msg_grid_w32` (WM_CHAR routed via `window.cpp`, surrogate pairs reassembled through `TextEditState::pending_high_surrogate`; `WM_GETDLGCODE` returns `DLGC_WANTMESSAGE` for Enter/Esc and `DLGC_WANTCHARS` while editing so `IsDialogMessage` doesn't swallow them; `WM_KILLFOCUS` commits an open editor); macOS `grid_painted_msg_macos` (`GridMsg::Key`) + `grid_painted_char_macos` (called from `NEUINativePaintedView::keyDown:` with NSEvent.characters) + `NEUINativePaintedView::resignFirstResponder`. Each host plumbs its own clipboard layer (xpl `platform_clipboard_*`, win32 `clipboard_set/get_text_win32`, macOS `clipboard_set/get_text_macos`).

**Focus-loss commit**: Tab / click-elsewhere / API focus changes commit an open in-place editor as if the user had pressed Enter; on `validate_cell` rejection the editor cancels (reverts to `orig_text`) rather than fighting the focus change. The seam in xpl is `WidgetData::on_focus_change(bool gained)`, called by `Session::set_focus` before firing `NEUI_EVENT_WIDGET_FOCUS`.

Optional `neui_grid_client_t { validate_cell(token, grid, row, col, new_text) -> bool }` via `NEUI_API_GRID_CLIENT`; returning false leaves the editor open with the proposed text so the user can fix it, true accepts (framework writes `set_cell_text` + fires `NEUI_EVENT_GRID_CELL_CHANGED`). Events: `_CELL_EDIT_BEGIN` when the editor opens, `_CELL_CHANGED` on accept (new text reachable via `grid->get_cell_text`), `_CELL_EDIT_CANCEL` on ESC. Programmatic lifecycle: `grid->begin_cell_edit / end_cell_edit(commit) / is_editing_cell`. Disabling an editable column while its editor is open auto-cancels.

**Smooth scroll + elastic rubber-band** (default on macOS, opt-in on Win32; selected by `NEUI_ATTR_GRID_SCROLL_MODE`): vertical scroll is row-indexed (`scroll_offset_y`) plus a fine `scroll_px_offset` for sub-row-smooth motion, with inertial momentum + WebKit-style overscroll. Math + tuning live once in `grid_model.h` (`GridScrollKinetics` in `GridModel.scroll_kin`; `grid_scroll_wheel` / `_bounce_step` / `_commit` + `GRID_SCROLL_*` constants; shared `grid_scroll_step_rows` for the STEPPED fallback). The mode attr is read each wheel event so flipping it is live. macOS hosts feed NSEvent phase/momentum/precise-delta into `grid_scroll_wheel` and run a 60 Hz `NSTimer` spring-back (native: per-`NEUINativePaintedView`; xpl: per-`NEUIView`). Win32 hosts feed `WM_MOUSEWHEEL` notches × `SPI_GETWHEELSCROLLLINES` × `row_h` as a `precise` synthetic input and run a 60 Hz `SetTimer` spring-back (native: per-grid HWND timer in `widgets.cpp`; xpl: per-frame HWND timer in `platform_win32.cpp` with `WindowUserData::bouncing_grid_index` tracking the active grid). Keyboard / scrollbar-drag / API snap `scroll_px_offset` to 0; the bounce self-cancels if the position moves externally. STEPPED mode (the Win32 default, and what every host falls back to under `NEUI_GRID_SCROLL_STEPPED`) row-quantizes the wheel through `grid_scroll_step_rows`, hard-clamps at the edges, and resets the kinetics integrator so a later flip to SMOOTH starts cleanly.

## Typical Client Usage

Client passes a `neui_client_t { version, get_interface }`; `get_interface(iface)` returns a `neui_widget_client_t { version, ondestroy, onevent }` for `NEUI_API_WIDGETS`. Then:

```cpp
neui_init();
neui_api_t*        neui    = neui_get_api(NULL);   // or an explicit host id
neui_session_t     sess    = neui->create_session(&host_client, &app);
neui_widget_api_t* widgets = (neui_widget_api_t*) neui->get_interface(sess, NEUI_API_WIDGETS);
neui_attr_api_t*   attrs   = (neui_attr_api_t*)   neui->get_interface(sess, NEUI_API_ATTRS);
neui_tree_api_t*   tree    = (neui_tree_api_t*)   neui->get_interface(sess, NEUI_API_TREE);

neui_widget_t win = widgets->create(sess, widget_none, NEUI_W_APPWINDOW, 100, 100, 800, 600, nullptr);
attrs->set_int(sess, win, NEUI_ATTR_MIN_WIDTH, 400);
auto mb  = widgets->create(sess, win, NEUI_W_MENUBAR, 0, 0, 0, 0, nullptr);
auto pop = tree->add(sess, mb, tree_item_root, "Edit", nullptr);
auto it  = tree->add(sess, mb, pop, "Undo", nullptr);
tree->set_shortcut(sess, mb, it, NEUI_KMOD_CTRL, NEUI_KEY_Z);
tree->set_menu_cmd(sess, mb, it, NEUI_CMD_UNDO);
widgets->show(sess, win);
neui->run(sess);
```

## Platform Implementation Gotchas

Win32:
- **`HTREEITEM` is 64-bit on x64** - bidirectional maps; never truncate.
- **Treeview text** - always `SendMessageW(..., TVM_INSERTITEMW, ...)`.
- **Menu separator IDs** - `AppendMenuW(MF_SEPARATOR)` ignores ID; use `InsertMenuItemW` with `MIIM_TYPE | MIIM_ID | MFT_SEPARATOR`.
- **`EnableMenuItem` for submenus** - `HMENU` is 64-bit; never cast to `UINT`. Use `GetSubMenu` + `MF_BYPOSITION`.
- **Deferred HWND** - guard every API call with `hwnd == nullptr`; flush in `create_child_windows()`.
- **`WM_CHAR` surrogate pairs** - two messages per supplementary codepoint; assemble via `pending_surrogate`.
- **`CS_DBLCLKS`** required for `WM_LBUTTONDBLCLK` → `MOUSE_BUTTON_DBLCLICK`. CheckboxWidget treats DOWN and DBLCLICK identically.
- **`TranslateAccelerator` order** - runs before `TranslateMessage` and (xpl) before `IsDialogMessage`; `set_menu_cmd` routing keeps widget-local Ctrl+Z working.
- **HACCEL / HICON / HMENU lifetimes** - owned per widget; freed on destroy/replace; rebuilt on `set_shortcut` / `tree_remove` / `tree_clear`.
- **`WM_COMMAND` ID space** - control IDs `[1, 0x7FFF]` (tree slots); menu cmd_ids `[0x8000, 0xFFFF]` (recycled via `free_menu_cmd_ids`). Don't cross-pollinate.

macOS:
- **`isFlipped = YES`** on content + painted views - CGContext CTM matches Y-down renderer convention.
- **`drawRect:` rect is logical pixels** - backing scale via `NSWindow.backingScaleFactor`; `CGContextScaleCTM` accounts for it.
- **`keyEquivalent`** is a single lowercase character string (`@"z"`); modifier mask `NSEventModifierFlagCommand | ...`.
- **NSPasteboard change-count** - no notification; listener polls `changeCount` on activate + each runloop tick.
- **Cmd-Q quit** - `applicationShouldTerminate:` calls `Session::request_quit`; let the runloop unwind rather than `exit()`.
- **Xcode bundle: `XCODE_ATTRIBUTE_COMBINE_HIDPI_IMAGES NO`** - default pipeline combines `name.png` + `name@2x.png` into one `.tiff`, breaking the path-keyed loader. Set on the example target when adding HiDPI images.

## Deferred Issues

- **KNOB not a keyboard tab-stop on macOS** - the NSView refuses first responder. Needs focusable + arrow-key value handling.
- **macOS Tab participation follows the system Full-Keyboard-Access setting** - traversal *order* matches win32, but *which* control types Tab visits beyond text fields / lists is OS policy. Hand-roll Tab for fully deterministic traversal.
- **Tier B focus parity** (xpl proxy HWND per widget for UIAutomation; NSAccessibility seam on macOS).
- **Image clipboard / DnD** - `text/plain`, `text/html`, `text/uri-list`, and arbitrary MIMEs are wired; `image/png` (or any bitmap format) is not - needs Win32 `CF_DIBV5` ↔ PNG decode and macOS `NSPasteboardTypePNG` / `NSPasteboardTypeTIFF` wrap.
- **DnD drag-source extensions** - drag-source API ships on all three hosts (`begin_drag`); deferred follow-ups: behavior-asset handler kind `DRAG_SOURCE` (declarative initiation from compound + behavior CUSTOMDRAW), custom drag image (`NSImage` / `IDragSourceHelper`), lazy promise data (`CFSTR_FILECONTENTS` / `NSFilePromiseProvider`).
- **Per-child-widget DnD on macOS native** - resolved: the framework hit-tests the widget tree (`find_drop_target_in_frame_macos`), matching win32 native + xpl. `NEUINativeContentView` is still the only `<NSDraggingDestination>` (child painted views don't register their own, exactly like child HWNDs on win32); a per-painted-view native opt-in remains possible but isn't needed for behavioral parity.
- **macOS clipboard change-notification** - `NSPasteboard.changeCount` polling is not wired; the API has no `onchange` event anyway, clients poll on demand.
- **Multi-level redo on win32** - `NEUI_CMD_REDO` maps to `EM_UNDO` (single-level toggle); macOS native + xpl are multi-level.
- **Multi-session palette** - `active_palette_override_ptr` is process-wide (last-set-wins).
- **Compound layer kinds** beyond `text` / `asset`: `rect` / `path` / `group`; template format specs (`{key:.2f}`); asset-layer `tint`.
- **Behavior detent / plateau modifier** - per-handler "sticky values" that resist near specific points (distinct from `steps`); behavior pen pressure / tilt (pen surfaces as mouse); `cursor` prop no-op; wheel modifier-fine needs a `neui_event_wheel_t` payload extension.
- **Native blocking modal** (Cocoa `runModalForWindow`, Win32 `DialogBoxIndirect`); current non-blocking modal matches the event-loop shape.
- **GRID sort follow-ups** - STRING kind is byte-level `strcmp` (locale-aware compare deferred); no per-column custom comparator callback (clients pre-format or pick STRING / INT / FLOAT / NATURAL); no built-in DATE / TIME kind.

## Plans

`plans/`: `painter-and-asset-api.md` (compound + asset, shipped), `grid-macos-port.md` (GRID macOS port, shipped), `grid-sorting.md` (multi-column sort, shipped), `clipboard-and-dnd.md` (unified data-item + clipboard formats + DnD drop-target, shipped - drag source is the next phase), `winui3-host.md` (deferred), `wasm-host.md` (deferred), `how-to-port.md` (new-platform playbook).
