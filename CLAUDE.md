# CLAUDE.md

Guidance for Claude Code when working in this repository.

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

Outputs - Windows: `out/build/Debug/{neui_example.exe, neui.lib, neui-win32host.lib, neui-xplhost.lib, neui-backend-d2d.lib}`. macOS: `out/build/Debug/{neui_example.app, libneui.a}` + per-subdir `libneui-*.a`.

## Per-platform host + backend selection

- **Windows**: `neui-win32host` + `neui-xplhost`; backend `neui-backend-d2d`; xpl platform `platform_win32.cpp`.
- **macOS**: `neui-macoshost` + `neui-xplhost`; backend `neui-backend-cg`; xpl platform `platform_macos.mm`.
- **Other**: `neui-xplhost`; backend `neui-backend-null`; xpl platform `platform_null.cpp`.

Top-level CMakeLists gates each platform-specific subdirectory; the example links the native host only when present.

## Architecture (file map)

- **Public C API** `include/neui/`: `neui.h` (`neui_init` + `neui_register` + `neui_get_api`); sub-headers under `d/`: `api.h`, `keys.h`, `widgets.h`, `events.h`, `items.h`, `tree.h`, `attrs.h`, `clipboard.h`, `commands.h`, `renderer.h`, `painter.h`, `assets.h`, `compound.h`, `menu.h`, `theme.h`.
- **Core library** `src/neui.c`: host registry + `neui_init()` (fans out to per-host registration wrappers gated by `NEUI_HAS_*HOST` defines CMake sets on the `neui` target).
- **Shared portable utilities** `hosts/shared/`, header-only, ODR-safe via `inline`: `tree.h` (`Tree<T>` slot-reused), `attrs.h` (`AttrBag` type-strict + `attr_as_float` INT-promoting reader), `clipboard_item.h`, `edit_history.h`, `shortcut_format.h`, `theme_palette.h` (`ColorRole` + `Palette` + `current_palette()` + `ScopedPaletteOverride`), `compound.h` (`CompoundLayer` / `CompoundAsset` / `CompoundBinding` + `{key}` template parser + 9-point anchor resolver + `apply_*` mutator helpers), `painter.h` (`neui_painter` struct + `k_painter_api` singleton), `widget_font.h` (`EffectiveFont` + `read_widget_font` / `push_widget_font` / `pop_widget_font` - reads `NEUI_ATTR_FONT_*` from a widget's bag and brackets the font stack around its `draw_text` calls), `widget_paint_knob.h` / `widget_paint_section.h` / `widget_paint_compound.h`.
- **Shared platform-specific** `hosts/shared/win32/` (clipboard, accel table, icon, theme provider + brushes, dark menu / menubar) and `hosts/shared/macos/` (clipboard, theme provider, image loader, keys, menubar).
- **Win32 Host** `hosts/win32/`: native HWND host. `window.cpp` WinMain + pump; `host.cpp` Session + `get_interface`; `widgets.cpp` full API; `host.h` `WidgetData`; `asset_manager_w32.h` `W32AssetManager`.
- **macOS Host** `hosts/macos/`: native AppKit host (`neui.host.macos`). `host.{h,mm}` Session; `widgets.mm` full API; `window.mm` NSApp + `NEUINativeContentView` (`isFlipped=YES`) + `NEUINativePaintedView` (per-widget CG context; hosts IMAGE / KNOB / SECTION / CUSTOMDRAW via per-type branch in `drawRect:`); `asset_manager_macos.h` `MacOSAssetManager`.
- **Crossplatform Host** `hosts/crossplatform/`: polymorphic widget hierarchy (registers as `neui.host.crossplatform`). `host.{h,cpp}` `WidgetData` base + subclasses (`FrameWidget`, `LabelWidget`, `ButtonWidget`, `InputBoxWidget`, `MultilineWidget`, `CheckboxWidget`, `ListItemsWidget`→`ComboBoxWidget`, `TreeviewWidget`, `MenubarWidget`, `KnobWidget`, `ImageWidget`, `CustomDrawWidget`) with virtuals `paint`, `paint_after_children`, `on_keydown`, `on_keychar`, `on_mouse_event`, `hit_test`, `on_destroy`, `on_composition`, `is_frame`, `is_menubar`, `perform_command`, `can_perform_command`. `widgets.cpp` full API + `make_widget()` factory. `platform.h` cross-cutting seam (window / menubar / image / clipboard / IME / modal / focus); per-OS implementations in `platform_win32.cpp` / `platform_macos.mm` / `platform_null.cpp`.
- **Backends** `backends/`: `d2d/` (Direct2D, `ID2D1HwndRenderTarget`), `cg/` (CoreGraphics, one `CGContextRef` per frame via `set_current_frame` from `drawRect:`), `null/` (no-op).

## Rendering Backend (`d/renderer.h`)

`neui_render_backend_t` is the implementation interface backends fill in: `create_context` / `destroy_context` / `resize`, `begin_frame` / `end_frame`, `fill_rect` / `draw_rect`, `get_scale_factor` / `update_dpi`, `draw_text` / `measure_text`, `push_clip` / `pop_clip` (nestable), `create_bitmap` / `destroy_bitmap` / `draw_bitmap` (host-internal; clients reach bitmaps via `painter_api->draw_asset`), path API, transform stack, `get_context_generation` (bumped on device-loss; cached target-bound resources re-upload on mismatch), alpha stack `push_alpha` / `pop_alpha` (cumulative 0..1 opacity multiplied into every subsequent fill / stroke / text / path / bitmap draw; reset on `begin_frame`; software stack on D2D + CG, no-op on null), font stack `push_font` / `pop_font` ((family, weight) pair feeding `draw_text` / `measure_text`; size stays per-call; reset on `begin_frame`; honoured by d2d, no-op on cg + null). Transform / alpha / font stacks reset to identity / empty at every `begin_frame`. Coordinates: logical pixels at 96 DPI. Colour: `0xAARRGGBB`.

## Events (`d/events.h`)

App: `APP_QUIT`. Mouse: `MOUSE_MOVE/ENTER/LEAVE`, `MOUSE_BUTTON_DOWN/UP/CLICK/DBLCLICK`, `MOUSE_RBUTTON_DOWN/UP`, `MOUSE_WHEEL`. Key: `KEYDOWN/KEYCHAR/KEYUP`. Widget: `WIDGET_UPDATED/PREUPDATE/FOCUS/PAINT`, `CHECKBOX_CHANGED`, `RESIZE`, `VALUE_CHANGED`. Item: `ITEM_SELECTED`. Tree: `TREE_ITEM_SELECTED/ACTIVATED`. `WIDGET_PAINT` fires only on `NEUI_W_CUSTOMDRAW` (and only when no compound asset is attached). `CHECKBOX_CHANGED` fires on every user-driven toggle (click / space) on all three hosts - the xpl `CheckboxWidget` dispatches it via `checkbox_dispatch_changed` to match the native hosts. `KEYDOWN.modifiers` + accelerator modifiers share `NEUI_KMOD_*` bits.

## Per-widget implementation details

Implementation-specific constants, scrollbar geometry, multiline cursor math, edit-history coalescing rules, IME composition state machine, dark-mode uxtheme ordinals - all live in the code with comments. Read the relevant file when working there:

- `hosts/crossplatform/host.cpp` - the heavy lifting (LISTBOX / COMBOBOX / TREEVIEW / MULTILINE / KNOB drag / popup overlay / combo overlay).
- `hosts/shared/edit_history.h` - undo coalescing.
- `hosts/shared/win32/dark_menu_win32.h` / `dark_menubar_win32.h` - uxtheme dark mode HMENU.
- `hosts/shared/win32/theme_provider_win32.h` / `hosts/shared/macos/theme_provider_macos.h` - palette sources.

## Clipboard

`NEUI_API_CLIPBOARD`: convenience `set_text` / `get_text` / `has_text`; item-based `read`, `create_item`, `release`, `write`, `item_set_format(mime, data, len)`, `item_get_format`, `item_has_format`. v1 only round-trips `NEUI_CLIPBOARD_MIME_TEXT = "text/plain;charset=utf-8"`; shape forward-compatible. Optional `neui_clipboard_client_t { onchange(token) }` via `get_interface(NEUI_API_CLIPBOARD_CLIENT)`. Per-session `ClipboardItemStore` (slot-reused). xpl text widgets handle Ctrl+C/X/V via `xpl_host::platform_clipboard_*`; native `Edit` (Win32) does it automatically; NSTextField (macOS) likewise.

## Attribute API

`NEUI_API_ATTRS`. String-keyed bag per widget (`std::unique_ptr<AttrBag>` on `WidgetData`, lazy). API: `set_int`/`get_int(default)`, `set_float`/`get_float(default)`, `set_string`/`get_string`, `has`, `remove`. Type-strict: wrong-kind returns the default. **Well-known keys are debug-asserted to match their documented kind at set time** via `k_well_known_attrs` in `hosts/shared/attrs.h`; mismatches abort under `assert()` in debug builds (with a `key=... expected=... actual=...` stderr line), release silently stores the wrong kind so reads keep returning the default. Session-level: `set_session_int`/`get_session_int` on `Session::_session_attrs`. `NEUI_ATTR_THEME_MODE` is the only session key with behaviour today.

**Well-known keys**:

| Key | Type | Applies | Notes |
|---|---|---|---|
| `tristate` | int | CHECKBOX | Implicit on CHECKBOX3. |
| `multiline` | int | INPUTBOX | Implicit on MULTILINE. |
| `readonly` | int | INPUTBOX, MULTILINE | Gates modifying keys. |
| `password` / `border` / `align_text` | int / int / string | various | Reserved (impl pending) — except `align_text` on SECTION, live. |
| `tab_stop` | int | focusable | Replaces deprecated `widgets->set_tab_stop`. |
| `min_width` / `min_height` / `max_width` / `max_height` | int (logical px) | APPWINDOW | Drives `WM_GETMINMAXINFO` / `NSWindow.min/maxSize`. |
| `icon_path` | string | APPWINDOW | `.ico` / `.png` / `.bmp` / `.jpg`. Live-applied. |
| `modal` | int | DIALOG | `1`/unset → owner disabled. Read once at `widget_show`. |
| `background` | int ARGB | KNOB, IMAGE, painted widgets, frame on xpl | Honoured **unconditionally** (independent of follow-system-theme). |
| `follow_system_theme` | int bool | APPWINDOW, PLUGWINDOW, DIALOG | Per-frame opt-in: `1` = DWM dark + dark HMENU + theme-aware `WM_CTLCOLOR*`; `0`/unset = OS-default chrome, no auto-invalidate on theme flips. |
| `rotation` | float (rad) | IMAGE | Around the destination centre. Positive = clockwise (Y-down). Live. |
| `polarity` | string | KNOB | `"min"` (default) / `"center"` / `"max"`. Anchor end of fill arc. |
| `steps` | int | SLIDER, KNOB | `>=2` snaps to N positions on `[0..1]` + draws ticks; `<2` continuous. Also snaps programmatic `set_float(NEUI_PARAM_VALUE)`. |
| `orientation` | string | SLIDER | `"horizontal"` (default) / `"vertical"`. Read at `widget_show`. |
| `value_text` | string | KNOB | Overlay text below the disc. Read each paint. |
| `knob_mode` | int | KNOB | Drag style: `NEUI_KNOB_MODE_ROTATIONAL=0` (default) / `_VERTICAL=1` / `_HORIZONTAL=2`. Cached at mouse-down. |
| `font_family` | string | text-bearing widgets | Family name (e.g. `"Consolas"`). Empty / unset = host default (Segoe UI on D2D). Honoured by the d2d backend + native win32 controls (per-widget `HFONT` via `WM_SETFONT`); cg / null ignore (system default until wired). Live. |
| `font_size` | float (logical px) | text-bearing widgets | Overrides each widget's hardcoded default (typically 12). Honoured everywhere `draw_text` / `measure_text` flow through the widget's paint code, including MULTILINE caret + IME composition geometry. Live. |
| `font_weight` | int | text-bearing widgets | CSS-style 100..900. 400 = Normal, 700 = Bold; 0 / unset = Normal. Mapped to nearest `DWRITE_FONT_WEIGHT_*` (d2d) / `FW_*` (HFONT). Italic not exposed in v1. Live. |
| `theme_mode` | int session-level | session | AUTO (0) follows OS; LIGHT (1) / DARK (2) force the palette. Accent stays live. |

(All keys are `neui.attr.<name>`; macros `NEUI_ATTR_*`. Namespace `neui.attr.*` reserved; clients use their own. Host-specific reserved: `neui.win32.*`, `neui.macos.*`, `neui.linux.*`. Unknown keys are stored but inert.)

## Routed commands

`NEUI_API_COMMANDS`. `neui_command_t`: `NONE/UNDO/REDO/CUT/COPY/PASTE/SELECT_ALL/DELETE`, `USER_BASE = 0x10000`. API: `invoke_focused(cmd) → bool`, `invoke(widget, cmd) → bool`. `tree->set_menu_cmd(menubar, item, cmd)` binds a menu item to a built-in command. On activation, `dispatch_menu_event` calls `invoke_focused_command(cmd)` first; if a focused widget consumes it, no `TREE_ITEM_ACTIVATED` fires. Otherwise (or `cmd == 0` / `cmd >= USER_BASE`) the event reaches the client. `WidgetData::perform_command(cmd) → bool` is the virtual seam; xpl text widgets route to `on_keydown` with a synthetic Ctrl+letter.

## Popup menus

`widgets->popup_menu(session, anchor, x, y, items[])` - blocking; items is NULL-terminated UTF-8 (`"-"` = separator). Returns 1-based pick or 0 on dismiss. Win32: `TrackPopupMenuEx` with `TPM_RETURNCMD`. xpl: Session-level overlay + nested message pump via `platform_run_modal_until(bool*)`. Used by the KNOB right-click context menu ("Reset to default" → `NEUI_PARAM_DEFAULT`); right-click is `NEUI_EVENT_MOUSE_RBUTTON_DOWN`.

## Keyboard shortcuts and accelerators

`tree->set_shortcut(menubar, item, modifiers, key)` (`NEUI_KMOD_*` bits + `NEUI_KEY_*`). `NEUI_KEY_NONE` clears. Display formatted by `shortcut_format.h`, appended to menu text after `\t`. Win32 builds an HACCEL via `accel_table_win32.h` and walks it from the pump via `try_translate_accel(MSG*)` before `TranslateMessage`. macOS sets `NSMenuItem.keyEquivalent` + `keyEquivalentModifierMask` directly. `NEUI_KMOD_CTRL` = platform-primary (Cmd on macOS, Ctrl on Win/Linux); `NEUI_KMOD_META` = secondary (Control on macOS).

## Enabled / disabled state

`widgets->set_enabled(sess, w, bool)` / `get_enabled(sess, w)`. Default for every widget is enabled=true. Disabled widgets paint dimmed and do not receive input.

- **win32 native**: `EnableWindow(hwnd, enabled)`. If the HWND has not been created yet (deferred), the flag is stored on `WidgetData::enabled` and applied in `create_child_windows` right after HWND creation + custom-font setup.
- **xpl**: `WidgetData::enabled` flag. Paint brackets `wd.paint()` with `push_alpha(0.5)` / `pop_alpha` (per-widget dim, not subtree). Hit-test (`widget_at_recursive`) skips disabled widgets so clicks fall through to ancestors (matches Win32 EnableWindow click-transparency); `collect_tab_stops` skips them; `dispatch_mouse_event` bails on disabled. If the focused widget becomes disabled, focus advances to the next tab-stop.
- **macOS native**: `apply_enabled_native_macos(wd)` (in `window.mm`) pushes the flag into the live control - `[NSControl setEnabled:]` for the NSControl-backed leaves (LABEL / BUTTON / INPUTBOX / CHECKBOX / COMBOBOX / SLIDER), document-view disable for NSScrollView-hosted controls (LISTBOX / TREEVIEW = NSTableView / NSOutlineView; MULTILINE = NSTextView gated via `editable` / `selectable` + `disabledControlTextColor`). Painted views (IMAGE / KNOB / CUSTOMDRAW / SECTION) dim in `drawRect:` via `push_alpha(0.5)` bracketing the content paint (after `begin_frame`, which resets the alpha stack), and the KNOB mouse / wheel handlers bail when disabled. Deferred-creation parity: `create_native_for_widget` re-applies `wd.enabled` right after instantiation, mirroring win32's `create_child_windows`.

Frames + non-interactive containers (SECTION, MENUBAR) accept the call but the effect is host-defined.

## Frame resize, window icon, focus

Resize: Win32 `WM_SIZE` (skip `SIZE_MINIMIZED`; physical → logical via `MulDiv(phys, 96, dpi)`); macOS `windowDidResize:`. Both emit `RESIZE { widget, width, height }` in logical px. Min/max attrs drive `WM_GETMINMAXINFO` / `NSWindow.min/maxSize`. `max < min` = "no maximum". `WM_DPICHANGED` triggers a follow-up `WM_SIZE`. Icon (`NEUI_ATTR_ICON_PATH`): Win32 `icon_win32.h` → `WM_SETICON`; macOS → `NSApp.applicationIconImage`.

Focus: clients see **logical** focus only. Tab traversal is hand-rolled on xpl (`Session::_focused_widget`, `focus_next`). Frame's `WM_SETFOCUS` / `WM_KILLFOCUS` (Win32) / `NSWindowDidBecomeKey` / `DidResignKey` (macOS) → `WIDGET_FOCUS`. `Session::_os_focused`: when false, paint reports "no focus" → caret + focus outline hide; logical state preserved. **Tier B** (real focus-proxy HWND for UIAutomation) deferred.

## Theme palette

Process-wide `neui_detail::Palette` (`theme_palette.h`) - flat array indexed by `ColorRole` (frame_bg, panel_bg, control_bg, accent, text_primary, border, scrollbar_*, ime_underline_*, …). Win32 + macOS providers populate from system sources and fire `Session::on_theme_changed` on system flips.

**Per-session override** (`NEUI_ATTR_THEME_MODE`): Session computes `_effective_palette` per mode (AUTO/LIGHT/DARK), points `active_palette_override_ptr()` at it; `current_palette()` consults the override first. AUTO copies system; LIGHT/DARK start from defaults overlaid with live system accent. Multi-session: last-set-wins (process-wide override; documented limitation).

**Per-frame opt-in** via `NEUI_ATTR_FOLLOW_SYSTEM_THEME = 1`. With it: DWM dark title bar + dark HMENU on Win32, palette-driven brushes for native controls, and the frame invalidates on every theme flip. Without it: OS-default chrome, no auto-invalidate (the session's `_effective_palette` still tracks system colours - painted widgets that invalidate for unrelated reasons pick up the new palette). Default off so an audio-plugin host can own the look.

**Win32 application manifest** (`examples/neui_example.manifest`) declares Win10/11 `supportedOS` GUIDs + Per-monitor v2 DPI + UTF-8 ACP. Without the GUIDs, Windows gates off uxtheme dark mode. `NEUI_API_THEME_CLIENT` (`d/theme.h`) - optional client theme-change callback, fires after framework invalidation.

## Key Design Patterns

- **Host registry** - `neui_register(id, api)` at startup; `neui_get_api(NULL)` returns first registered. IDs: `"neui.host.win32"`, `"neui.host.macos"`, `"neui.host.crossplatform"`. **Clients call `neui_init()` once** to register every host the linked neuilib has compiled in (gated on `NEUI_HAS_*HOST` CMake defines on the `neui` target). Order: native first, then xpl, so `neui_get_api(NULL)` picks the native host where one exists.
- **Per-host registration wrappers** - each host static lib exposes `extern "C"` wrappers (`neui_register_xplhost` / `neui_register_win32host` / `neui_register_macoshost`) that thunk to the namespaced `register_host()`. These also serve as the linker forced-symbol references that pull each host's object files out of its static lib. `neui_init()` fans out to whichever wrappers are compiled in; the wrappers remain callable individually as escape hatches.
- **Named interface dispatch** - `get_interface(sess, name)` with version suffix (`/0`). Active: `NEUI_API_WIDGETS/_ITEMS/_TREE/_ATTRS/_CLIPBOARD/_COMMANDS/_ASSETS/_COMPOUND`. Optional client-side: `_CLIPBOARD_CLIENT`, `_MENU_CLIENT`, `_THEME_CLIENT`.
- **Session model** - 32-bit ID; slot-reused vector. Client passes `neui_client_t` with `get_interface` callback; host passes opaque token back on every callback. Explicit dtor unregisters listeners.
- **Widget IDs** - upper 16 = owning session id, lower 16 = tree slot. Every API entry validates via `get_session_for_widget`; cross-session handles silently dropped. Sentinels (`widget_root = 0`, `widget_none = UINT32_MAX`) pass. Stale-after-slot-reuse not detected (no generation counter; deferred).
- **Deferred HWND** (win32 host) - logical state stored immediately; HWND / HMENU / HACCEL / HICON created on `widget_show()`; pending state flushed in `create_child_windows()`. Guard every API call with `hwnd == nullptr`.
- **Event routing** - host → client via `neui_widget_client_t::onevent()`. Client gets first chance for mouse events; false forwards to `widget->on_mouse_event()`.
- **`emit_events` gate** - `widget_at()` and `dispatch_mouse_event()` both require `emit_events = true`. Auto-set for BUTTON, INPUTBOX, CHECKBOX, CHECKBOX3, LISTBOX, COMBOBOX, MULTILINE, TREEVIEW, CUSTOMDRAW.
- **Coordinates** - logical pixels at 96 DPI. Child widget x/y is **relative to the immediate parent's top-left** on all three hosts. Top-level children of a frame are frame-local. Win32 native: HWND parenting takes care of it. macOS native: NSView subview parenting via `create_descendants_native` (recognises container types like SECTION so children land in the right subview). xpl: stored as parent-relative `x`/`y` on `WidgetData`, with frame-local `abs_x`/`abs_y` recomputed top-down each frame by `paint_widgets_recursive`; the paint walk pushes `translate(wd.x, wd.y)` on the renderer transform stack around the recursive descent so widget `paint()` overrides keep drawing at `(this->x, this->y)`.
- **`NEUI_ABI` (`__cdecl`)** on all API function pointers.
- **`DEF_` prefix on event macros** - avoids collision with Windows SDK `MOUSE_EVENT` / `KEY_EVENT` in `wincontypes.h`.
- **`interface` reserved by MSVC** - parameters use `iface`.
- **Type-as-default + attributes-override** - implicit variants (CHECKBOX3, MULTILINE) set their attr at create; runtime reads the attr.
- **Vtable-append for evolution** - append new methods at end of any public-API struct so slot offsets stay stable. Pre-1.0: slots can change when all hosts rebuild.
- **Shared inline-only headers** - both static libs include the same defs; ODR-safe via `inline` storage.
- **`platform_clipboard_*` / `platform_menubar_*` are the cross-cutting seams.** xpl text widgets + public clipboard API call `platform_clipboard_*` rather than into `hosts/shared/{win32,macos}/clipboard_*` directly. Same shape for menubar.

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
| `KNOB` | Painted rotary; right-click → "Reset to default" popup; drag mode switchable via `NEUI_ATTR_KNOB_MODE`. |
| `IMAGE` | Source = file path (`set_text`) OR pre-loaded handle (`set_asset`). Last-set-wins; `""` / `asset_none` clears. Aspect-preserving fit; honours `NEUI_ATTR_ROTATION`. The widget does NOT retain a refcount on the asset - clear or destroy the widget before `assets->destroy`. |
| `SECTION` | Non-interactive container. Body filled with `NEUI_ATTR_BACKGROUND` (fallback: `shade(frame_bg, +24)`; macOS native flips the lift direction when +24 saturates against the light system background). Optional `set_text` header drawn as a "title chip" in a top band; rest of the band is transparent (parent shows through). Chip position via `NEUI_ATTR_ALIGN_TEXT`. `emit_events=false` - clicks pass through. Children paint on top via normal tree traversal. |
| `CUSTOMDRAW` | Client-rendered surface. Emits `NEUI_EVENT_WIDGET_PAINT` each frame with `neui_painter_api_t* painter_api` + opaque `neui_painter_t* p` + widget-local size + focus state. Origin (0, 0) is widget-local; framework wraps the dispatch in `push_transform / push_clip(widget bounds) / pop_clip / pop_transform`. Standard MOUSE / KEY events flow normally (`emit_events` auto-set). Supported on all three hosts. Client invalidates with `widgets->invalidate(session, widget)`. CUSTOMDRAW also accepts a **compound asset** via `widgets->set_asset` - when attached, the framework paints the compound's layer stack instead of firing WIDGET_PAINT. |

## Painter + asset API

Two public interfaces back `NEUI_W_CUSTOMDRAW`:

**`neui_painter_api_t`** (`include/neui/d/painter.h`) - curated drawing surface handed to clients via `WIDGET_PAINT`. Exposes only the draw-safe subset of `neui_render_backend_t`: shapes (`fill_rect`/`draw_rect`/`draw_text`), path API, state stack (`push_clip`/`pop_clip`/`push_transform`/`pop_transform`/`translate`/`rotate`/`scale`/`push_alpha`/`pop_alpha`), queries (`get_scale_factor`/`measure_text`), and handle-based `draw_asset`. **Excluded by design**: context lifecycle, `begin_frame`/`end_frame`, raw `create_bitmap`/`destroy_bitmap`. The opaque `neui_painter_t*` (defined in `hosts/shared/painter.h`) carries `backend`, `ctx`, `host_token`, `draw_asset_thunk`; hosts stack-allocate one per WIDGET_PAINT dispatch. The vtable singleton is `neui_detail::k_painter_api` (inline-static).

`push_alpha(p, factor)` / `pop_alpha(p)` multiplies a cumulative 0..1 opacity into every subsequent draw call until popped. Backend support: D2D + CG software stack; null no-op.

`push_font(p, family_utf8, weight)` / `pop_font(p)` selects the active font family + weight for every subsequent `draw_text` / `measure_text`. Family / weight default to host system (Segoe UI / Normal on D2D) when the stack is empty; `font_size` stays a per-call parameter. Weight is CSS-style 100..900 (400 = Normal, 700 = Bold). D2D resolves to a cached `IDWriteTextFormat` keyed by `(family, weight, size_q10)`; cg + null are no-op for now.

**`neui_asset_api_t`** (`include/neui/d/assets.h`, `NEUI_API_ASSETS`) - session-scoped media handles loaded **outside** the paint loop. Methods: `create_bitmap(sess, w_px, h_px, bgra, scale)`, `create_from_file(sess, path)` (resolves `@2x` / `@3x` per current display scale), `destroy`, `get_size`, `get_kind`, `create_compound(sess)`. Returns `neui_asset_t` (handle layout `(session_id << 16) | slot`, matching `neui_widget_t`); cross-session handles rejected. `neui_asset_kind_t`: `NEUI_ASSET_KIND_BITMAP = 1`, `NEUI_ASSET_KIND_COMPOUND = 2`; SVG / vector / font reserved.

The renderer struct's `create_bitmap`/`destroy_bitmap`/`draw_bitmap` are **internal** to the host's image / asset pipeline - clients reach bitmap draws through `painter_api->draw_asset(p, asset, x, y, w, h)`, which resolves the handle and uploads to the current ctx lazily (one upload per (asset, ctx) pair, cached for the ctx's lifetime).

**Asset storage**: xpl extends `neui_detail::AssetManager` (`hosts/crossplatform/asset_manager.{h,cpp}`) with a slot-reused handle table alongside the legacy path-keyed cache used by `NEUI_W_IMAGE`. Win32 native has `W32AssetManager` (`hosts/win32/asset_manager_w32.h`). macOS native has `MacOSAssetManager` (`hosts/macos/asset_manager_macos.h`) - same slot-vector + free-list + per-ctx GPU cache shape (CG `CGImageRef` is device-independent so the per-ctx generation check is structurally redundant but kept for symmetry). All managers release per-ctx caches when the owning paint context goes away.

**Device-loss recovery** (D2D only): `EndDraw` returning `D2DERR_RECREATE_TARGET` triggers an in-place rebuild in the next `begin_frame` from stashed `hwnd`/`width`/`height`/`dpi` on `D2DContext`; per-ctx `generation` bump. Asset managers key their per-ctx GPU cache by `(ctx, generation)` and re-upload on mismatch. CG + null backends return a constant generation.

## Compound drawables

A **compound** is `NEUI_ASSET_KIND_COMPOUND` - a mutable, declarative shape attachable to a CUSTOMDRAW widget. Owns a slot-reused layer stack; each layer is `text` or `asset` (v1 kinds), positioned by a 9-point anchor pair (parent + self) + `(offset_x, offset_y)` + `(width, height)` with `NEUI_COMPOUND_FILL = -1` per axis to span the widget; signed-int `z` interleaves layers with the widget's children (`z<0` below, `z>=0` above; insertion order breaks ties). When attached, the framework walks the layers and `WIDGET_PAINT` is suppressed.

**API surface** (`include/neui/d/compound.h`, `NEUI_API_COMPOUND`): `add_layer(kind, z)`, `remove_layer` / `clear` / `set_z`, `set_anchor`, typed setters `set_int` / `set_float` / `set_string` (templates) / `set_asset`, numeric `bind(prop, attr_key, scale, offset)`, asset `bind_asset(prop, attr_key)`, `unbind`. Recognised props - **text**: `text` (template), `size`, `color` (optional, theme-fallback), `align_x` / `align_y`; **asset**: `asset`, `rotation`; **both**: `offset_x` / `offset_y` / `width` / `height` / `alpha`.

**Values live in the widget's `AttrBag`**, not on the compound. A text layer with `text = "{label}: {value}"` resolves the keys against the widget's attrs at paint time; `bind(layer, "rotation", "value", 2π, 0)` reads `value` as float (int promotes via `attr_as_float`, string/missing yields 0), computes `scale * x + offset`, and applies. Float-to-int prop targets round to nearest. One compound can back many widgets - shape is shared, attrbag is per-widget.

**Template syntax**: `{key}` substitution with `{{` / `}}` for literal braces. Malformed or missing keys yield empty / literal pass-through (no throws). Pre-parsed at `set_string`. `{key:.2f}` format specs reserved but deferred.

**Invalidation**: any `attrs->set_*` on a widget with a compound attached invalidates the widget; compound mutations walk session widgets and invalidate every CUSTOMDRAW whose `compound_asset` matches.

**Storage + paint**: `AssetEntry` / `W32AssetEntry` / `MacOSAssetEntry` carry a `std::unique_ptr<CompoundAsset>`. Layer table + parsing + geometry + binding eval in `hosts/shared/compound.h`; paint pass in `hosts/shared/widget_paint_compound.h::paint_compound_below` / `paint_compound_above`. All three hosts call `_below` before child descent and `_above` after - xpl wires `_above` through `WidgetData::paint_after_children` so layer z interleaves with child widgets; native hosts call them back-to-back inside the parent's paint, so z relative to children only works on xpl (child HWNDs / NSView subviews always paint above the parent surface).

## Typical Client Usage

```cpp
static neui_widget_client_t widget_client = {
  NEUI_VERSION,
  [](void*, neui_widget_t, void*) { /* ondestroy */ },
  [](void*, neui_event_t*) -> bool { return false; }
};
static neui_client_t host_client = {
  NEUI_VERSION,
  [](void*, const char* iface) -> void* {
    if (!strcmp(iface, NEUI_API_WIDGETS)) return &widget_client;
    return nullptr;
  }
};

neui_init();
neui_api_t*          neui    = neui_get_api(NULL);   // or pass an explicit host id
neui_session_t       sess    = neui->create_session(&host_client, &app);
neui_widget_api_t*   widgets = (neui_widget_api_t*) neui->get_interface(sess, NEUI_API_WIDGETS);
neui_tree_api_t*     tree    = (neui_tree_api_t*)   neui->get_interface(sess, NEUI_API_TREE);
neui_attr_api_t*     attrs   = (neui_attr_api_t*)   neui->get_interface(sess, NEUI_API_ATTRS);

neui_widget_t win = widgets->create(sess, widget_none, NEUI_W_APPWINDOW, 100, 100, 800, 600, nullptr);
attrs->set_int   (sess, win, NEUI_ATTR_MIN_WIDTH,  400);
attrs->set_string(sess, win, NEUI_ATTR_ICON_PATH,  "app.ico");

auto mb       = widgets->create(sess, win, NEUI_W_MENUBAR, 0, 0, 0, 0, nullptr);
auto edit_pop = tree->add(sess, mb, tree_item_root, "Edit", nullptr);
auto undo_it  = tree->add(sess, mb, edit_pop, "Undo", nullptr);
tree->set_shortcut(sess, mb, undo_it, NEUI_KMOD_CTRL, NEUI_KEY_Z);
tree->set_menu_cmd(sess, mb, undo_it, NEUI_CMD_UNDO);

widgets->show(sess, win);
neui->run(sess);
```

## Platform Implementation Gotchas

Win32:
- **`HTREEITEM` is 64-bit on x64** - bidirectional maps; never truncate.
- **Treeview text** - always `SendMessageW(..., TVM_INSERTITEMW, ...)`.
- **Menu separator IDs** - `AppendMenuW(MF_SEPARATOR)` ignores ID. Use `InsertMenuItemW` with `MIIM_TYPE | MIIM_ID | MFT_SEPARATOR`.
- **`EnableMenuItem` for submenus** - `HMENU` is 64-bit; never cast to `UINT`. Use `GetSubMenu` + `MF_BYPOSITION`.
- **Deferred HWND** - guard every API call with `hwnd == nullptr`; flush pending state in `create_child_windows()`.
- **`WM_CHAR` surrogate pairs** - two messages per supplementary codepoint; assemble via `pending_surrogate`.
- **`CS_DBLCLKS`** required for `WM_LBUTTONDBLCLK` → `MOUSE_BUTTON_DBLCLICK`. Widgets that need every click (CheckboxWidget) treat DOWN and DBLCLICK identically.
- **`TranslateAccelerator` order** - runs *before* `TranslateMessage` and (xpl) *before* `IsDialogMessage`. `set_menu_cmd` routing is what keeps widget-local Ctrl+Z working when a shortcut is bound.
- **HACCEL / HICON / HMENU lifetimes** - owned per widget; freed on destroy / replace; rebuilt on `set_shortcut` / `tree_remove` / `tree_clear`.
- **`WM_COMMAND` ID space partitioned** - control IDs `[1, 0x7FFF]` (tree slots, debug-asserted in `CreateChildHwnd`); menu cmd_ids `[0x8000, 0xFFFF]` (recycled via `WidgetData::free_menu_cmd_ids`). Don't cross-pollinate.

macOS:
- **`NEUIView.isFlipped = YES`** - CGContext CTM matches Y-down renderer convention without an explicit flip.
- **`drawRect:` rect is logical pixels** - backing scale via `NSWindow.backingScaleFactor`; `CGContextScaleCTM` already accounts for it.
- **`keyEquivalent`** is a single lowercase character string (`@"z"`), modifier mask is `NSEventModifierFlagCommand | ...`.
- **NSPasteboard change-count** - no `NSPasteboardDidChange` notification; listener polls `changeCount` on activate + each runloop tick (cheap int compare).
- **Cmd-Q quit** - `applicationShouldTerminate:` calls `Session::request_quit`; let runloop unwind cleanly rather than `exit()`.
- **Xcode bundle: `XCODE_ATTRIBUTE_COMBINE_HIDPI_IMAGES NO`** - default Xcode pipeline combines `name.png` + `name@2x.png` into a single `name.tiff`, breaking the path-keyed image loader. Set the property on the example target when adding new HiDPI images.

## Architecture rules to not relitigate

- **String hashing in attribute lookup is fine** (~50 ns × hundreds/sec ≪ frame budget). Don't shorten `neui.attr.`. If a profile proves hot, cache the parsed value as a direct field.
- **Adding a new `NEUI_ATTR_*` / `NEUI_PARAM_*` macro requires a matching row in `k_well_known_attrs`** (`hosts/shared/attrs.h`). The setter assert relies on that table to catch client kind-mismatches in debug; an unregistered new key would silently fall through and only surface as a "why doesn't my attribute work" mystery downstream.
- **CHECKBOX3 / MULTILINE stay as public type strings** even though they map to `CHECKBOX + tristate` / `INPUTBOX + multiline` internally. Public constructor identity > internal compression.
- **Shortcut display is framework-formatted** - clients pass `(mods, key)`; Win/Linux get `"Ctrl+S"` labels; macOS uses `NSMenuItem.keyEquivalent` directly.
- **Client undo/redo via menu** uses routed commands. Client binds `NEUI_CMD_UNDO` to Edit > Undo; framework routes Ctrl+Z and the menu pick to the focused text widget's history.
- **Menu-item validation (auto-disable on popup-open)** - built-in commands auto-gray via `WidgetData::can_perform_command` → `Session::can_focused_perform_command`. Optional `neui_menu_client_t` (`NEUI_API_MENU_CLIENT`) → `validate(token, menubar, item, cmd) → bool` for every non-separator item. `enabled = mi.enabled && (no built-in OR can_focused) && (no validate OR validate())`.
- **IME composition lives on the widget.** `WidgetData::on_composition(kind, utf8, len, caret_byte)` is the platform-agnostic seam. State on `InputBoxWidget`/`MultilineWidget`. `text` is not mutated until `COMP_RESULT`, which pushes one `EditHistory` entry against the pre-composition snapshot (kanji undoes as one unit). `caret_rect_local` reused for `ImmSetCompositionWindow` / `firstRectForCharacterRange:`. Win32 native inherits IME via native `Edit`.
- **xpl static-text widgets are palette-driven, not stub-coloured.** `LabelWidget` draws text only (parent surface shows through); `ButtonWidget` fills `panel_bg` with `border` outline; `CheckboxWidget` paints glyph + check + text with no surrounding rect. The xpl `CheckboxWidget` also fires `NEUI_EVENT_CHECKBOX_CHANGED` on user toggle (was previously silent - native hosts always emitted it); event carries the full packed `widget_id`, matching the slider/knob `VALUE_CHANGED` convention.
- **Knob drag-mode is widget-attribute, mode-cached-at-down.** `NEUI_ATTR_KNOB_MODE` is read once at `MOUSE_BUTTON_DOWN` / `WM_LBUTTONDOWN` and stashed on the drag state so per-frame mouse-move is a single int branch. Slider modes use `KNOB_SLIDER_SWEEP_PX = 200`; rotational keeps the `1.5π` sweep + 4-px centre dead-zone. All three modes share the `drag_continuous` accumulator so STEPS snapping behaves identically.
- **Compound is a new asset kind, not a new widget type.** CUSTOMDRAW is the host; attaching a `NEUI_ASSET_KIND_COMPOUND` via `set_asset` switches its paint mode from imperative WIDGET_PAINT to declarative layer walk. Compounds are mutable + shareable across widgets; per-widget state lives in the widget's existing `AttrBag` (no parallel "values" store on the compound). Mixing modes (WIDGET_PAINT + compound on the same widget) is not supported in v1.
- **Text-layer `color` is optional** - falls back to `ColorRole::text_primary` from the active palette when neither set explicitly nor bound, so labels stay legible across system theme flips without client follow-up.
- **Compound bindings always read attrs as float** via `attr_as_float`. String props use template substitution (`{key}`) instead of bindings - the two paths don't overlap. Asset props use a separate `bind_asset` because asset handles aren't meaningfully float-arithmeticable.
- **macOS section bg uses a direction-aware lift.** The default `shade(frame_bg, +24)` saturates against macOS's light-mode `windowBackgroundColor`; the macOS paint helper detects no-op lifts and shades the other direction so the section reads as a depressed panel in light mode and a raised panel in dark mode.

## Deferred Issues

- Tier B focus parity (xpl proxy HWND per widget for UIAutomation / accessibility on Windows; NSAccessibility seam on macOS).
- Custom clipboard formats (HTML / image); v1 only routes `text/plain` (API shape forward-compatible).
- Multi-level redo on win32 host - `NEUI_CMD_REDO` maps to `EM_UNDO` (toggle, single-level). Use xpl text widgets for multi-level via `EditHistory`.
- Multi-session palette correctness - `active_palette_override_ptr` is process-wide (last-set-wins). Fine for single-session.
- Compound layer kinds beyond `text` / `asset`: `rect` / `path` / `group` (the last unlocks nested transform scopes). Template format specs (`{key:.2f}`). Declarative interactivity (per-layer hit targets, drag-to-attr bindings) - v1 routes input through CUSTOMDRAW's normal MOUSE_* path. Asset-layer `tint` (ARGB multiplier) reserved but not wired.
- `NEUI_W_IMAGE` on the native macOS host still loads via `[view ensureImageBitmap:]`; migration to the asset-manager + `set_asset` path is a one-pager follow-on after the macOS asset work.
- Native blocking modal (Cocoa `runModalForWindow`, Win32 `DialogBoxIndirect`). Current non-blocking modal matches the event-loop shape.

## Plans

Design plans live in `plans/`: `painter-and-asset-api.md` (compound + asset infrastructure, shipped - kept for rationale), `winui3-host.md` (feasibility analysis, deferred), `how-to-port.md` (reference playbook for new platform ports). Smaller features (fonts, attribute-kind asserts, enabled/disabled state) shipped without a dedicated plan file - their rationale lives in this file and the relevant TODO.md entries.
