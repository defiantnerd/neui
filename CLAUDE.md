# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**This file is the orientation map, not the full spec.** It holds what is relevant every session (overview, build, file map, cross-cutting invariants, client-authoring rules). Deep per-subsystem detail lives in `docs/` - see **Subsystem reference** at the bottom and read the relevant file before doing non-trivial work in that area.

## Project Overview

**neuilib** is an early-stage C/C++ GUI framework separating a client C API from platform host implementations. Windows, macOS, and Linux (X11 + Cairo, via the crossplatform host) are implemented; any other platform falls back to a null platform layer. Tier-1 unit tests in `tests/`. No dedicated linter; instead the build runs at a high warning level as the static-analysis safety net (MSVC `/W4`, GCC/AppleClang `-Wall -Wextra`), with `C4100`/`-Wunused-parameter` suppressed (fixed-signature params) - keep the build warning-clean.

## Build

CMake 3.15+, C++17 (MSVC on Windows, AppleClang on macOS, GCC/Clang on Linux).

Linux dev packages (Debian/Ubuntu): `pkg-config libx11-dev libxext-dev libxi-dev libcairo2-dev libfreetype-dev libfontconfig1-dev` (Fedora: `libX11-devel libXext-devel libXi-devel cairo-devel freetype-devel fontconfig-devel`). Two **optional** packages, each auto-detected via `pkg_check_modules` with graceful fallback: `libxi-dev` (XInput2 -> `NEUI_HAS_XI2`, pixel-precise/kinetic smooth scroll; absent -> classic stepped scroll) and `libdbus-1-dev` (D-Bus -> `NEUI_HAS_DBUS`, system dark/light tracking via the XDG portal; absent -> default palette). `stb_image.h` is vendored (`third_party/stb/`). The QR generator (Project Nayuki, MIT) is vendored in `third_party/qrcode/` and compiled into a small static lib (`neui-qrcodegen`) backing the `NEUI_COMPOUND_LAYER_QR` layer.

```bash
# Windows / Linux
cmake -B out/build -DCMAKE_BUILD_TYPE=Debug && cmake --build out/build

# macOS (Xcode generator - multi-config; IDE + lldb integration)
cmake -B out/build -G Xcode && cmake --build out/build --config Debug
```

Outputs - Windows: `out/build/Debug/{neui_example.exe, neui.lib, neui-win32host.lib, neui-xplhost.lib, neui-backend-d2d.lib}`. macOS: `out/build/Debug/{neui_example.app, libneui.a}` + per-subdir `libneui-*.a`. Example apps (CMake targets): `neui_example`, `neui_grid_example`, `neui_section_scroll_example`, `neui_surface_example`, `neui_dnd_example`, `neui_dnd_source_example`, `neui_tabview_example`, `neui_font_loading_example`, `neui_filter_knob_example`, `neui_path_example`, `neui_overlay_example` (transparent CUSTOMDRAW overlay - DirectComposition on win32), `neui_arc_example` (value-driven arc / ring / pie compound layer), `neui_zoom_example` (`NEUI_ATTR_UI_SCALE` 100/150/200 % + logical vs device-pixel CUSTOMDRAW painting), and `readme_example`.

**Tests**: `tests/` is a Tier-1 header-only unit suite (`neui_tests`) over the portable logic in `hosts/shared/*.h` - links no host and no backend, builds everywhere including the null platform. Toggle with `-DNEUI_BUILD_TESTS=OFF`. Run directly or via `ctest --test-dir out/build -C Debug`. Linux-only extra targets: `neui_cairo_smoke` (offscreen Cairo, ctest-registered) and `neui_embed_smoke` (fake-DAW embedding, needs a live X display).

## Per-platform host + backend selection

- **Windows**: `neui-win32host` + `neui-xplhost`; backend `neui-backend-d2d`; xpl platform `platform_win32.cpp`.
- **macOS**: `neui-macoshost` + `neui-xplhost`; backend `neui-backend-cg`; xpl platform `platform_macos.mm`.
- **Linux** (X11): `neui-xplhost` only (no native host); backend `neui-backend-cairo` (software, blitted via XShm/XPutImage); xpl platform `platform_linux.cpp`. Clipboard (CLIPBOARD + PRIMARY + INCR), full XDND v5, neui-drawn message box, in-frame menubar, XI2 smooth scroll, D-Bus theme tracking, and DAW-embedding seams all live here - **see `docs/host-linux.md`**.
- **Other**: `neui-xplhost`; backend `neui-backend-null`; xpl platform `platform_null.cpp`.

Top-level CMakeLists gates each platform-specific subdirectory; the example links the native host only when present.

## Architecture (file map)

- **Public C API** `include/neui/`: `neui.h` (`neui_init` + `neui_register` + `neui_get_api`); sub-headers under `d/`: `api.h`, `keys.h`, `widgets.h`, `events.h`, `items.h`, `tree.h`, `attrs.h`, `clipboard.h`, `dnd.h`, `commands.h`, `timer.h`, `renderer.h`, `painter.h`, `pointer.h`, `gradient.h`, `path_style.h`, `assets.h`, `compound.h`, `behavior.h`, `component.h`, `filter.h`, `grid.h`, `scroll.h`, `menu.h`, `theme.h`, `notify.h`, `metrics.h`, `embed.h`.
- **Core library** `src/neui.c`: host registry + `neui_init()` (fans out to per-host registration wrappers gated by `NEUI_HAS_*HOST`). Also `src/mujson.{h,cpp}`: `neui::mujson`, a minimal JSON-*like* parser (bare keys, `//` + `/* */` comments, single trailing comma, `\uXXXX`, depth-cap 128) compiled into `libneui`; `parse(str)->object_t` / `serialize(object_t)`; Tier-1 tested via `tests/test_mujson.cpp`.
- **Shared portable utilities** `hosts/shared/`, header-only, ODR-safe via `inline`: `tree.h` (`Tree<T>`), `attrs.h` (`AttrBag` + `attr_as_float` + `k_well_known_attrs`), `asset_store.h` (`AssetStore<Loader>` slot table), `clipboard_item.h`, `cursor_kind.h`, `relative_pointer.h`, `dnd_dispatch.h`, `dnd_modifier_suggest.h`, `edit_history.h`, `text_edit.h` (`TextEditState` shared by every text-input surface), `shortcut_format.h`, `theme_palette.h` (`ColorRole` / `Palette` / `current_palette`), `compound.h`, `behavior.h` / `behavior_runtime.h`, `image_filter.h`, `filter_graph.h`, `grid_model.h` / `widget_paint_grid.h`, `scrollbar.h`, `scroll_kinetics.h`, `widget_section_scroll.h`, `timer_table.h`, `painter.h`, `widget_font.h`, `widget_paint_knob.h` / `_section.h` / `_compound.h`, `component_loader.h`. Platform-specific shared code under `hosts/shared/{win32,macos,linux}/`.
- **Win32 Host** `hosts/win32/`: native HWND host. `window.cpp` WinMain + pump; `host.{cpp,h}` Session + `WidgetData`; `widgets.cpp` full API; `asset_manager_w32.h`.
- **macOS Host** `hosts/macos/`: native AppKit host (`neui.host.macos`). `host.{h,mm}` Session; `widgets.mm` full API; `window.mm` (NSApp + `NEUINativeContentView` `isFlipped=YES` + `NEUINativePaintedView`); `asset_manager_macos.h`.
- **Crossplatform Host** `hosts/crossplatform/`: polymorphic widget hierarchy (`neui.host.crossplatform`). `host.{h,cpp}` `WidgetData` base + per-type subclasses (`FrameWidget` … `GridWidget`) with virtuals; `widgets.cpp` full API + `make_widget()`; `platform.h` cross-cutting seam (window / menubar / image / clipboard / IME / modal / focus); per-OS impls `platform_{win32.cpp,macos.mm,linux.cpp,null.cpp}`.
- **Backends** `backends/`: `d2d/` (Direct2D), `cg/` (CoreGraphics), `cairo/` (Linux software), `null/` (no-op). Backend-agnostic math in `backends/shared/backend_util.h`. Backend interface + per-backend draw/path/gradient/surface/font details: `docs/rendering-and-assets.md`.

## Events (`d/events.h`)

App: `APP_QUIT`, `TIMER` (`NEUI_API_TIMER`; the only event with no `.widget` - gate on `timer_id`). Mouse: `MOUSE_MOVE/ENTER/LEAVE`, `MOUSE_BUTTON_DOWN/UP/CLICK/DBLCLICK`, `MOUSE_RBUTTON_DOWN/UP`, `MOUSE_WHEEL`. Key: `KEYDOWN/KEYCHAR/KEYUP`. Widget: `WIDGET_UPDATED/PREUPDATE/FOCUS/PAINT`, `CHECKBOX_CHANGED`, `RESIZE`, `VALUE_CHANGED`, `ATTR_CHANGED`, `SCROLL_CHANGED`, `METRICS_CHANGED`, `GESTURE_BEGIN/END`. Item: `ITEM_SELECTED`. Tree: `TREE_ITEM_SELECTED/ACTIVATED`. Grid: `GRID_ROW_SELECTED`, `GRID_CELL_SELECTED`, `GRID_CELL_CLICKED`, `GRID_ROW_ACTIVATED`, `GRID_COLUMN_RESIZED`, `GRID_SORT_CHANGED`, `GRID_CELL_EDIT_BEGIN`, `GRID_CELL_CHANGED`, `GRID_CELL_EDIT_CANCEL`. DnD: `DND_ENTER/MOVE/LEAVE/DROP`. Tab: `TAB_DESELECTED/SELECTED`.

`WIDGET_PAINT` fires only on `NEUI_W_CUSTOMDRAW` (and only when no compound asset is attached). `CHECKBOX_CHANGED` fires on every user-driven toggle. `VALUE_CHANGED` is the widget-scoped user-driven event for native KNOB / SLIDER; `ATTR_CHANGED` (`{ widget, attr_key, value }`) is the parallel event a behavior asset fires when it writes through. `GESTURE_BEGIN` / `GESTURE_END` (`{ widget, attr_key, value }`) bracket a run of user-driven value changes for host-automation begin/end edits (VST3 `beginEdit`/`endEdit`, CLAP `GESTURE_BEGIN/END`): a pointer grab on KNOB / SLIDER / a behavior `DRAG_*` handler pairs grab..release (even when the value never moves); one-shot changes (wheel tick, value keys, double-click / context reset, `CLICK_*`) fire an implicit begin+change+end triple only when the value actually moved; programmatic sets never fire them. `KEYDOWN.modifiers` + accelerator modifiers share `NEUI_KMOD_*` bits. `NEUI_MK_*` mouse-modifier bits (matching Win32 `MK_*`) live in `<neui/d/events.h>`; `NEUI_MK_ALT` reserved but not yet populated. **`neui_event_wheel_t` carries the same `buttonmap`** so a client (and the behavior asset's `WHEEL` `fine_modifier`) can do Shift-for-fine on the wheel without tracking key state - note a host may already have spent Shift flipping a vertical notch to horizontal **and negating the delta** (`is_horizontal = 1` *and* `NEUI_MK_SHIFT` set), so check `is_horizontal` first if Shift is meant purely as a fine modifier. The Shift->horizontal flip is **owned by the platform layer**, so a consumer with no horizontal axis (a value handler) un-negates a `is_horizontal + NEUI_MK_SHIFT` notch to recover the physical direction - see the WHEEL branch of `hosts/shared/behavior_runtime.h`. **Read `data.wheel.buttonmap`, never `data.mouse.buttonmap`, on a wheel event**: the payloads overlap in the event union and `mouse.buttonmap` sits at the same offset as `wheel.delta`. The three per-OS mask translations live in `hosts/shared/{win32/keys_win32.h,macos/keys_macos.h,linux/keys_linux.h}` (`win32_buttonmap` / `mac_buttonmap` / `x11_buttonmap`) - **use those rather than forwarding a raw Win32 `wParam`**: `MK_XBUTTON1` is `0x0020`, which collides with `NEUI_MK_ALT`. Every event payload carries a `.widget` - see **Writing client code** below. **Mouse / wheel `x`/`y` are WIDGET-local on every host** (origin = the `.widget` top-left, matching `WIDGET_PAINT` and the DnD payload): the native hosts get this from the per-widget HWND/NSView; the single-surface xpl host translates frame-local input to the target's origin in `dispatch_mouse_event` / `dispatch_wheel_event` (the internal `WidgetData::on_mouse_event` handlers still run in frame-local and subtract `abs_x/abs_y` themselves).

## Key Design Patterns

- **Host registry** - `neui_register(id, api)` at startup; `neui_get_api(NULL)` returns first registered. IDs: `"neui.host.win32"`, `"neui.host.macos"`, `"neui.host.crossplatform"`. Clients call `neui_init()` once to register every compiled-in host (gated on `NEUI_HAS_*HOST`). Order: native first, then xpl.
- **Per-host registration wrappers** - each host static lib exposes `extern "C"` wrappers (`neui_register_xplhost` / `_win32host` / `_macoshost`); they double as the linker forced-symbol references pulling the host's objects out of its static lib.
- **Named interface dispatch** - `get_interface(sess, name)` with version suffix (`/0`). Active: `NEUI_API_WIDGETS/_ITEMS/_TREE/_ATTRS/_CLIPBOARD/_DND/_COMMANDS/_ASSETS/_COMPOUND/_BEHAVIOR/_FILTER/_GRID/_SCROLL/_NOTIFY/_EMBED/_TIMER/_POINTER`. Optional client-side: `_MENU_CLIENT`, `_THEME_CLIENT`, `_GRID_CLIENT`. `_FILTER` is itself optional host-side (a host without off-screen surfaces may return nullptr); `_EMBED` (`d/embed.h`, DAW embedding for PLUGWINDOW), `_TIMER` (`d/timer.h`) and `_POINTER` (`d/pointer.h`, relative/unbounded pointer mode so a knob drag survives the screen edge) are exposed by the **xpl host only** - note that on win32/macOS `neui_get_api(NULL)` returns the NATIVE host first, so a client taking the default there gets nullptr for all three.
- **Session model** - 32-bit ID; slot-reused vector. Client passes `neui_client_t` with `get_interface` callback; host passes opaque token back on every callback.
- **Widget IDs** - upper 16 = owning session id, lower 16 = tree slot. Every API entry validates via `get_session_for_widget`; cross-session handles silently dropped. Sentinels (`widget_root = 0`, `widget_none = UINT32_MAX`) pass. Stale-after-slot-reuse not detected (deferred).
- **Deferred HWND** (win32) - logical state stored immediately; HWND / HMENU / HACCEL / HICON created on `widget_show()`; pending state flushed in `create_child_windows()`. Guard every API call with `hwnd == nullptr`.
- **Event routing** - host -> client via `neui_widget_client_t::onevent()`. Client gets first chance for mouse events; false forwards to `widget->on_mouse_event()`.
- **`emit_events` gate** - `widget_at()` and `dispatch_mouse_event()` both require `emit_events = true`. Auto-set for BUTTON, INPUTBOX, CHECKBOX, CHECKBOX3, LISTBOX, COMBOBOX, MULTILINE, TREEVIEW, CUSTOMDRAW, KNOB, SLIDER, GRID.
- **Coordinates** - logical pixels at 96 DPI; child x/y relative to the immediate parent's top-left on all hosts. **`NEUI_ATTR_UI_SCALE`** (xpl host, per frame) adds a user zoom on top of the monitor DPI *without changing this contract*: widget geometry, `get_client_rect`, mouse coords and the painter stay logical at every zoom - one CTM scale in `Session::paint_frame` covers the whole frame paint, each platform divides input by the zoom in its native→logical conversion and multiplies native window sizing by it. A CUSTOMDRAW reads the true device scale from `neui_event_paint_t::scale`, or opts into device-pixel painting with `NEUI_ATTR_PAINT_DEVICE_PIXELS` (see `docs/attributes.md`). Win32: HWND parenting. macOS: NSView subview parenting. xpl: parent-relative `x`/`y` + frame-local `abs_x`/`abs_y` recomputed each frame. **A top-level frame's create() (width, height) is the CLIENT area, not the outer window** - the host grows the outer window for title bar / borders / menu row (win32 via `AdjustWindowRectExForDpi`, macOS `initWithContentRect:`, Linux X window size) so the usable client equals what was asked for. Prefer `get_client_rect` over the create() height when a menubar may be present. See **Writing client code**.
- **Immediate vs deferred child realization** - both native hosts realize a child's native view at `widget_show` AND immediately from `widget_create` when the containing frame is already shown (post-show dynamic creation). Both route scrolling-SECTION children through the inner body container. Without this, widgets added after `show` never appear.
- **`NEUI_ABI` (`__cdecl`)** on all API function pointers. **`DEF_` prefix on event macros** avoids Windows SDK `MOUSE_EVENT` / `KEY_EVENT` collision. **`interface` reserved by MSVC** - parameters use `iface`.
- **Type-as-default + attributes-override** - implicit variants set their attr at create. CHECKBOX3 is genuinely attr-driven (reads `NEUI_ATTR_TRISTATE` live). MULTILINE is NOT: single- vs multi-line is fixed by the widget TYPE at create; `NEUI_ATTR_MULTILINE` is a queryable marker never read, so it cannot flip a live INPUTBOX.
- **Vtable-append for evolution** - append new methods at the end of any public-API struct so slot offsets stay stable. Pre-1.0: slots can change when all hosts rebuild.
- **`platform_clipboard_*` / `platform_menubar_*`** are the cross-cutting seams; xpl text widgets + public clipboard API call them rather than into `hosts/shared/{win32,macos}/*` directly.

## Widget Types

| Type | Notes |
|---|---|
| `APPWINDOW` | Top-level; participates in quit-on-close. |
| `PLUGWINDOW` | Borderless top-level (`WS_POPUP` / borderless NSWindow / override-redirect-less X window); not in quit count. With `NEUI_API_EMBED::set_parent` (before show) it instead embeds into a DAW-provided native parent: win32 = `WS_CHILD` of the parent HWND, macOS = `NEUIView` subview of the parent NSView (no NSWindow of its own; window-level ops - title/pos/close/constraints/activate - become no-ops), Linux = child of the foreign X Window over a dedicated Display. Embedded mode: never call `run()`/`pump_once()` - the DAW's pump services win32/macOS; on Linux register `embed->event_fd` with the host run loop + call `embed->pump_and_tick` from its timer. |
| `LABEL` | Text only, no fill. |
| `BUTTON` | Centered text + 1px border. |
| `INPUTBOX` / `MULTILINE` | Cursor / selection / undo / clipboard / word nav. Distinct types (not a live attr toggle): INPUTBOX is single-line; MULTILINE adds explicit-newline editing + vertical scroll/scrollbar + per-line Home/End + up/down column-tracking nav, and **optional soft word-wrap via `NEUI_ATTR_LINE_WRAP`** (default off). With wrap off a long line clips at the right edge (no horizontal scroll); with wrap on it breaks into visual rows at the content width. Nav / selection / hit-test / scroll all run on the visual-row model (`MultilineWidget::cached_line_starts()` - logical lines when off, word-wrapped rows when on; cached on text/width/font/wrap). Perf + IME + edit-history internals: `docs/widget-internals.md`. |
| `CHECKBOX` / `CHECKBOX3` | 2-state vs 3-state cycle (`tristate=1` on CHECKBOX3). |
| `LISTBOX` / `COMBOBOX` | COMBOBOX: hover != selection until commit (Enter / click). The client lays out only the **collapsed** bar (the widget x/y/width/height); the drop list is sized independently from the item count - `min(count, NEUI_ATTR_COMBO_MAX_VISIBLE [default 10])` rows - and an optional `NEUI_ATTR_COMBO_DROP_WIDTH` width override (else auto-fit to the widest entry, floored at the bar width). xpl host paints the overlay below the collapsed bar (flipping **above** it when the list would overflow the frame bottom but fits above - `ComboBoxWidget::overlay_rect` is the shared source of truth for paint + hit-test); win32 native bumps the HWND height + `CB_SETMINVISIBLE`; macOS native NSPopUpButton auto-sizes. |
| `TREEVIEW` | Per-item expanded; chevrons; Treeview keys. |
| `MENUBAR` | No HWND (Linux draws an in-frame band - `docs/host-linux.md`). `tree->set_shortcut` + `tree->set_menu_cmd` configure items; `tree->set_checked` toggles a per-item checkmark (`docs/menus-commands-dialogs.md`). |
| `SLIDER` | Horizontal / vertical via `NEUI_ATTR_ORIENTATION`; `NEUI_ATTR_STEPS` snaps to N positions + draws ticks. `NEUI_PARAM_VALUE` is the normalized [0..1] value; user changes fire `VALUE_CHANGED`. |
| `KNOB` | Painted rotary. Right-click -> "Reset to default" popup (`NEUI_PARAM_DEFAULT`); drag mode via `NEUI_ATTR_KNOB_MODE` (rotational / vertical / horizontal); fill anchor via `NEUI_ATTR_POLARITY`; `NEUI_ATTR_VALUE_TEXT` overlay; `NEUI_ATTR_STEPS` detents. Not a keyboard tab-stop on macOS (deferred). |
| `IMAGE` | Source = file path (`set_text`) OR pre-loaded handle (`set_asset`); last-set-wins, `""` / `asset_none` clears. Aspect-preserving fit; honours `NEUI_ATTR_ROTATION`. No refcount on the asset - clear/destroy widget before `assets->destroy`. |
| `SECTION` | Visual container. Body filled with `NEUI_ATTR_BACKGROUND` (fallback `shade(frame_bg, +24)`). Optional `set_text` header as a "title chip" (rest of band transparent), positioned via `NEUI_ATTR_ALIGN_TEXT` (`"none"` hides the band so the body fills the rect). **Children's (x, y) is relative to the BODY top-left on every host** - so a chip-less section auto-expands the child area into the former band, and a child at `(10, 6)` sits 6 px below the chip band. xpl applies the band offset in its paint walk; native hosts parent children to an inner body container at the body rect (macOS `NEUISectionBodyView`, win32 `body_hwnd`), created the first time the section carries a chip OR scrolls and kept for its lifetime. `emit_events=false` when non-scrolling; `=true` when `NEUI_ATTR_SCROLL_MODE != "none"` (then children clip to the body + scrollbars + smooth kinetics). Scroll + per-host body-container details: `docs/attributes.md`. |
| `CUSTOMDRAW` | Client-rendered surface. Emits `WIDGET_PAINT` each frame with `neui_painter_api_t* painter_api` + opaque `neui_painter_t* p` + widget-local size + focus. Origin (0,0) widget-local; framework wraps dispatch in `push_transform / push_clip(bounds) / pop_*`. MOUSE / KEY flow normally. All three hosts. Invalidate via `widgets->invalidate`. Also accepts a **compound asset** (visual) and **behavior asset** (input) via `widgets->set_asset` (kind-routed); the two are independent and compose. See `docs/rendering-and-assets.md` + `docs/compound-behavior-component.md`. |
| `GRID` | Scrollable multi-column table; cells are paint-state, not widgets (a 10000x8 grid is one widget). Per-column `editable` flag opens an in-place editor on ENTER in cell-focus mode; optional `NEUI_API_GRID_CLIENT::validate_cell` gates the commit. Multi-column sort, two focus modes, smooth scroll: `docs/grid.md`. |

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

## Writing client code: window sizing + event routing (recurring pitfalls)

Two mistakes recur in generated neui client code. Apply both explicitly when authoring examples or client layouts.

**1. Size a top-level frame from its content; account for the menubar band, don't undersize.** `widgets->create(sess, widget_none, NEUI_W_APPWINDOW, x, y, w, h, ...)` takes `w`/`h` as the **logical client area** at 96 DPI (1:1 - the host grows the outer window for title bar / borders and scales for DPI; never add chrome or DPI math to the create size yourself). The symptom to avoid is content clipped by a small fixed margin (~10-30 px), which is **not** a DPI ratio - it is the non-client chrome / in-frame menubar band eating into where you assumed `(0,0)..(w,h)` was usable:
   - Compute `w`/`h` from the laid-out children: `w >= max(child.x + child.width) + margin`, `h >= max(child.y + child.height) + margin`, leaving an 8-12 px margin at the right and bottom edges (a widget flush to `w`/`h` reads as cramped and can clip).
   - When the frame carries a `NEUI_W_MENUBAR`, the **usable content height is less than `h`** by the menubar band on the in-frame-menubar host (Linux ~24 px). Lay children out against `widgets->get_client_rect(sess, frame, &x,&y,&w,&h)` (origin `(0, inset)`, size `(w, h-inset)`), not the raw create height - this is exactly the ~10-20 px shortfall.
   - Don't fall back to a habitual small default (e.g. 400x300 / 100,100,...). Pick a size that actually holds the content plus margins.

**2. Every event payload carries a `.widget` - gate every handler on it.** `onevent(token, event)` is called for *all* widgets with `emit_events` set, so a handler that branches on `event->type` alone fires for the wrong widget. Always test `event->data.<category>.widget.id == my_expected_id` before acting (the established pattern throughout `examples/main.cpp`):

```cpp
case NEUI_EVENT_MOUSE_BUTTON_CLICK:
  if (event->data.mouse.widget.id == app->save_button_id)   { /* ... */ return true; }
  if (event->data.mouse.widget.id == app->cancel_button_id) { /* ... */ return true; }
  break;
case NEUI_EVENT_RESIZE:
  // resize.widget is the FRAME being resized. With more than one frame
  // (APPWINDOW + DIALOG, multiple windows) you MUST check it or you
  // relayout the wrong window:
  if (event->data.resize.widget.id == app->main_win_id) {
    int w = event->data.resize.width, h = event->data.resize.height; /* ... */
  }
  break;
```
   The category field name matches the payload union in `include/neui/d/events.h`: `mouse` / `wheel` / `key` / `focus` / `checkbox` / `item` / `tree` / `resize` / `value` / `attr` / `grid_row` / `grid_cell` / `grid_column_resize` / `grid_sort` / `dnd` / `scroll` / `tab` / `metrics`. (Examples deliberately do **not** add a `NEUI_EVENT_RESIZE` handler to re-fit children - live layout-on-resize is a deferred feature - but any RESIZE handler you do write must check the frame id.)

## Platform Implementation Gotchas

Win32:
- **`HTREEITEM` is 64-bit on x64** - bidirectional maps; never truncate.
- **Treeview text** - always `SendMessageW(..., TVM_INSERTITEMW, ...)`.
- **Menu separator IDs** - `AppendMenuW(MF_SEPARATOR)` ignores ID; use `InsertMenuItemW` with `MIIM_TYPE | MIIM_ID | MFT_SEPARATOR`.
- **`EnableMenuItem` for submenus** - `HMENU` is 64-bit; never cast to `UINT`. Use `GetSubMenu` + `MF_BYPOSITION`.
- **Deferred HWND** - guard every API call with `hwnd == nullptr`; flush in `create_child_windows()`.
- **`WM_CHAR` surrogate pairs** - two messages per supplementary codepoint; assemble via `pending_surrogate`.
- **`CS_DBLCLKS`** required for `WM_LBUTTONDBLCLK` -> `MOUSE_BUTTON_DBLCLICK`. CheckboxWidget treats DOWN and DBLCLICK identically.
- **`TranslateAccelerator` order** - runs before `TranslateMessage` and (xpl) before `IsDialogMessage`; `set_menu_cmd` routing keeps widget-local Ctrl+Z working.
- **HACCEL / HICON / HMENU lifetimes** - owned per widget; freed on destroy/replace; rebuilt on `set_shortcut` / `tree_remove` / `tree_clear`.
- **`WM_COMMAND` ID space** - control IDs `[1, 0x7FFF]` (tree slots); menu cmd_ids `[0x8000, 0xFFFF]` (recycled via `free_menu_cmd_ids`). Don't cross-pollinate.
- **Frame create sizing** - the xpl host grows the requested logical client to the outer window via `AdjustWindowRectExForDpi` (reserving the menu row); `platform_set_window_pos` + `platform_menubar_attach` keep the client = requested size on resize / late menubar.

macOS:
- **`isFlipped = YES`** on content + painted views - CGContext CTM matches Y-down renderer convention.
- **`drawRect:` rect is logical pixels** - backing scale via `NSWindow.backingScaleFactor`; `CGContextScaleCTM` accounts for it.
- **`keyEquivalent`** is a single lowercase character string (`@"z"`); modifier mask `NSEventModifierFlagCommand | ...`.
- **NSPasteboard change-count** - no notification; listener polls `changeCount` on activate + each runloop tick.
- **Cmd-Q quit** - `applicationShouldTerminate:` calls `Session::request_quit`; let the runloop unwind rather than `exit()`.
- **Xcode bundle: `XCODE_ATTRIBUTE_COMBINE_HIDPI_IMAGES NO`** - default pipeline combines `name.png` + `name@2x.png` into one `.tiff`, breaking the path-keyed loader.

## Subsystem reference

Deep per-subsystem detail lives in `docs/`. **Read the relevant file before doing non-trivial work in that area** - the entries below are a map, not the full spec.

- **Attributes & well-known keys; Scroll API** -> `docs/attributes.md`. Every `NEUI_ATTR_*` / `NEUI_PARAM_*` key (type, applies-to, live-ness), SECTION scrolling, `scroll_kinetics`, programmatic scroll + `ensure_visible`. Read before adding or relying on any attribute - a new key also needs a `k_well_known_attrs` row.
- **Clipboard & drag-and-drop** -> `docs/clipboard-and-dnd.md`. `NEUI_API_CLIPBOARD` + `NEUI_API_DND`, data items, MIME round-trips, per-host COM / NSPasteboard / XDND wiring, suggested-action convention.
- **Rendering backend, painter, surfaces, filters, fonts** -> `docs/rendering-and-assets.md`. `neui_render_backend_t`, `neui_painter_api_t`, `NEUI_API_ASSETS`, render-to-surface, SVG filter graphs, client font loading.
- **Compound / behavior / component assets** -> `docs/compound-behavior-component.md`. Declarative drawables (layers, anchors, bindings, gradients, QR, groups), input handlers, the JSON component format.
- **GRID widget** -> `docs/grid.md`. `NEUI_API_GRID`, focus modes, click ladder, multi-column sort, in-place cell editing, smooth scroll.
- **Menus, routed commands, dialogs, notifications** -> `docs/menus-commands-dialogs.md`. Routed commands, popup menus, modal dialogs, toasts + message boxes, keyboard shortcuts / accelerators.
- **Theme palette; frame resize / icon / focus** -> `docs/theming.md`.
- **Per-widget internals & enabled/disabled** -> `docs/widget-internals.md`. MULTILINE perf, hover/pressed visuals, DBLCLK->CLICK parity, disabled state per host.
- **Linux (X11 + Cairo) host internals** -> `docs/host-linux.md`. Selections + INCR clipboard, XDND, in-frame menubar, XI2 smooth scroll, D-Bus theme, embedding seams.
- **Timers** -> `<neui/d/timer.h>` + `hosts/shared/timer_table.h`. `NEUI_API_TIMER`: `add_timer` / `remove_timer` / `set_timer_interval`, firing `NEUI_EVENT_TIMER`. ONE native tick per session at the shortest live interval (not one native timer per client timer); the portable `TimerTable` owns ids + deadlines and is Tier-1 tested. `interval_ms` is a **minimum**, clamped up to `NEUI_TIMER_MIN_INTERVAL_MS`; a late tick fires **once** rather than catching up, so a slow handler cannot build a backlog. Works under `run()`, a hand-rolled `pump_once()` loop, and `NEUI_API_EMBED` (the DAW's pump on win32/macOS, `pump_and_tick` on Linux) - a client never needs a thread. Mutation from inside a `NEUI_EVENT_TIMER` handler is safe, including removing the timer being handled.
- **Deferred / known-limitation list** -> `docs/deferred-issues.md`.
- **Design rationale for shipped features** -> `docs/design-notes.md`. **Component-widget authoring how-to** -> `docs/component-widgets-howto.md`.

## Plans

Active plans (`plans/`) are open or deferred work only - completed plans were removed once shipped (full text in git history; distilled rationale in `docs/design-notes.md`):

- `sst-neuigui-gap-response.md` - verified reply to an external gap analysis (building an sst-jucegui-style widget toolkit on neui) + the derived work plan: parity bugs, painter completeness (text metrics/alignment, rounded rect/ellipse/line), public timer, per-frame UI scale, cursor, rich popups, file dialog, xpl partial repaint, accessibility seam.
- `win32-pointer-and-directmanipulation.md` - WM_POINTER pen/touch + DirectManipulation smooth-scroll on scrolling SECTION + GRID (deferred; binding spec for when it lands).
- `winui3-host.md` - third host backend feasibility analysis (deferred indefinitely).
- `wasm-host.md` - WebAssembly / Canvas-2D host feasibility analysis (deferred; phased path documented).
- `lvgl-port.md` - neui-on-LVGL feasibility investigation (no implementation proposed).
- `how-to-port.md` - reference playbook for new-platform ports.
