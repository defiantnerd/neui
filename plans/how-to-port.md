# How to port neui to a new target

This is the porting playbook for adding a new platform target to neui — Linux/X11, Linux/Wayland, iOS, Android, embedded (framebuffer or RTOS GUI), or any future platform. Read this once before starting; then walk the steps in order.

The document is written so a fresh Claude session (or any contributor) can execute a port end-to-end without re-discovering the architecture. Every seam, contract, and concrete reference implementation is named with file paths.

---

## 1. What kind of port are you adding?

neui has three composable layers. Decide which you're adding before opening any file.

| Layer | Purpose | Existing examples | When to add a new one |
|---|---|---|---|
| **Render backend** (`backends/<name>/`) | Implements `neui_render_backend_t` — primitives, text, bitmaps, paths, transform stack. Pure rendering; no windowing. | `d2d/` (Direct2D), `cg/` (CoreGraphics), `null/` (no-op). | Your platform has a 2D graphics API that the xpl host can drive (Cairo, Skia, GL with a custom rasteriser, framebuffer software renderer). |
| **xpl platform layer** (`hosts/crossplatform/platform_<name>.cpp`) | Implements `xpl_host::platform_*` — window creation, message pump, mouse/keyboard/IME/clipboard input, menubar, image loading. | `platform_win32.cpp`, `platform_macos.mm`, `platform_null.cpp`. | Your platform has windowing (X11 / Wayland / UIKit / Android Views). Pair with one render backend. |
| **Native host** (`hosts/<name>/`) | A complete alternative to the xpl host that uses native widgets directly (no painted widgets). Implements the full `neui_widget_api_t` / `neui_tree_api_t` / etc. against the platform's native control set. | `hosts/win32/` (HWND controls), `hosts/macos/` (AppKit controls). | You want **native** controls (look-and-feel parity with the OS, free accessibility, free IME). Otherwise prefer xpl + backend. |

**Common port shapes:**

- **Linux / X11**: new backend (Cairo or Skia) + new xpl platform layer (`platform_linux.cpp`). No native host — Linux has no single "native" toolkit.
- **Linux / Wayland**: new backend (likely Skia or Cairo) + new xpl platform layer. May share backend with X11.
- **iOS**: reuse `backends/cg/` (CoreGraphics works on iOS) + new xpl platform layer (`platform_ios.mm`) backed by UIKit. macOS-native-host port is not transferable to iOS.
- **Android**: new backend (Skia is a natural fit; the AOSP Skia headers are public) + new xpl platform layer with a JNI bridge.
- **Embedded / framebuffer**: software backend (RGBA buffer writes) + tiny platform layer driving a kernel framebuffer or RTOS GUI service.

If you're only adding a **new backend** on an already-supported platform (e.g. a Skia backend for Windows that swaps out d2d), you don't need to touch the platform layer — only the backend and the per-platform CMake selection.

---

## 2. Core architecture (one-screen refresher)

```
                  client app (examples/main.cpp)
                          │
                          │ NEUI_API_WIDGETS / TREE / ATTRS / CLIPBOARD / COMMANDS
                          │ + ASSETS / COMPOUND / PAINTER (for CUSTOMDRAW)
                          ▼
              ┌────────────────────────┐
              │  neui core registry    │   src/neui.c — neui_init / neui_register / neui_get_api
              └───────────┬────────────┘
                          │ host id (e.g. "neui.host.crossplatform")
                          ▼
        ┌────────────────────────────────────────────────────┐
        │  HOST                                              │
        │  - Session lifecycle, widget tree, event dispatch  │
        │                                                    │
        │  hosts/win32   ── native HWND controls             │
        │  hosts/macos   ── native AppKit controls           │
        │  hosts/crossplatform ── painted widgets via a      │
        │                         render backend + a thin    │
        │                         platform layer             │
        └─────────┬──────────────────────┬───────────────────┘
                  │                      │
                  │ render API           │ platform API
                  ▼                      ▼
        ┌──────────────────┐   ┌──────────────────────────┐
        │ render backend   │   │ xpl platform layer       │
        │ (neui_render_    │   │ (xpl_host::platform_*)   │
        │  backend_t)      │   │ window/pump/clipboard/   │
        │ d2d / cg / null  │   │ menubar/IME/icon         │
        └──────────────────┘   └──────────────────────────┘
```

**Key invariants** (don't violate these):

- Coordinates everywhere in the public API are **logical pixels at 96 DPI**. Each host translates physical→logical at the platform boundary.
- **Y is screen-down.** Arc sweep: positive angle = clockwise.
- Colors are `0xAARRGGBB`. Bitmaps are **BGRA8 premultiplied**.
- Single UI thread. Palette + listener tables are NOT atomic; mutate from one thread.
- The widget id packs **upper 16 = session id, lower 16 = tree slot**. Cross-session handles are silently dropped.
- All `neui_*_api_t` function pointers are `__cdecl` (`NEUI_ABI`).

---

## 3. The render backend contract

**Source of truth: `include/neui/d/renderer.h`** — read it once. The vtable is `neui_render_backend_t`. A backend lives in `backends/<name>/`, exposes a `get_backend()` accessor in its own namespace, and is selected by the platform layer's `platform_get_backend()`.

### 3.1 Function-pointer surface (all MUST-implement)

| Group | Function | Semantics |
|---|---|---|
| Lifecycle | `create_context(native_handle, w, h) → ctx` | Native handle = HWND on Win32, `NSView*` on macOS, your equivalent. Bind a context for the lifetime of the window. |
| Lifecycle | `destroy_context(ctx)` | Free GPU / OS resources. |
| Lifecycle | `resize(ctx, w, h)` | Notify of window resize in logical px. |
| Frame | `begin_frame(ctx, clear_argb)` | Start a frame. Must reset transform stack to identity. Must reset clip stack. Must clear the entire surface to `clear_argb`. |
| Frame | `end_frame(ctx)` | Flush / present. |
| Primitives | `fill_rect(ctx, x, y, w, h, argb)` | Solid-fill axis-aligned rect. |
| Primitives | `draw_rect(ctx, x, y, w, h, stroke_w, argb)` | Stroke axis-aligned rect. |
| Text | `draw_text(ctx, x, y, w, h, utf8, font_size, argb)` | Text laid out and clipped to the (x,y,w,h) box. Vertical alignment is "paragraph baseline" — backend chooses. |
| Text | `measure_text(ctx, utf8, len, font_size) → float` | Width in logical px. `len = -1` for null-terminated. Returning `0.0f` is allowed for backends without text but disables features that depend on it (e.g. SECTION header chip sizing). |
| Clip | `push_clip(ctx, x, y, w, h)` | Axis-aligned, intersects with all prior pushes. Nestable. |
| Clip | `pop_clip(ctx)` | Pop most recent. |
| Bitmap | `create_bitmap(ctx, w_px, h_px, bgra_premult, scale) → handle` | Create from a heap buffer of **BGRA8 premultiplied** pixels. `w_px` / `h_px` are physical pixels; `scale` is the HiDPI factor (1.0 = @1x). Backend must consume the pixels into its own storage (caller may free immediately after). |
| Bitmap | `destroy_bitmap(ctx, handle)` | Free. |
| Bitmap | `draw_bitmap(ctx, handle, sx, sy, sw, sh, dx, dy, dw, dh)` | Source + destination rect in logical px. `sw==0 && sh==0` means "use full bitmap". |
| Path | `begin_path / move_to / line_to / arc / close_path` | Build a path. `arc` is centre + radius + start..end radians, sweep is clockwise (Y-down). |
| Path | `fill_path(ctx, argb) / stroke_path(ctx, stroke_w, argb)` | Render. Path stays valid after either (can stroke + fill). |
| Transform | `push_transform / pop_transform / translate / rotate / scale` | Post-multiplied (`t' = t * T`); applies to all subsequent draws until popped. Identity at every `begin_frame`. |
| DPI | `get_scale_factor(ctx) → float` | Physical px per logical px. |
| DPI | `update_dpi(ctx, dpi)` | Called before `begin_frame` when DPI changes (Win32 WM_DPICHANGED). |

### 3.2 Backend boilerplate

A new backend lives entirely under `backends/<name>/`:

```
backends/<name>/
  CMakeLists.txt       # produces neui-backend-<name>
  <name>_backend.h     # namespace neui_<name>_backend { get_backend(); }
  <name>_backend.cpp/.mm
```

Pattern (from `backends/null/null_backend.cpp:39-72`): one `static` function per vtable slot, one `static neui_render_backend_t backend = { ... }` aggregate, `get_backend()` returns its address. `neui_null_backend` is the smallest complete example — copy it as the skeleton.

### 3.3 Coordinate system inside the backend

- All public-API coords arrive in **logical px at 96 DPI**.
- The backend internally scales by DPI (D2D: `SetDpi(dpi, dpi)` on the render target; CG: `CGContextScaleCTM(scale, scale)`).
- For bitmaps, `create_bitmap` takes **physical** px + scale so HiDPI assets keep their crispness. Logical rendering size = `physical / scale`.
- Color = `0xAARRGGBB`. Premultiplied bitmap pixels. Backend chooses whether to premultiply solid-color alpha internally (D2D and CG both expect premultiplied; verify for your API).

### 3.4 Hot paths

`fill_rect`, `draw_rect`, `draw_text`, `push/pop_clip`, and the transform-stack ops are called many times per paint. Don't allocate per call; pre-create brushes / pens / cached colour objects keyed by ARGB. See d2d_backend.cpp's solid-colour brush cache for the pattern.

---

## 4. The xpl platform layer contract

**Source of truth: `hosts/crossplatform/platform.h`**. Implement every function in `namespace xpl_host`. `platform_null.cpp` is the minimum-viable stub — start by copying it and progressively implementing real behaviour.

### 4.1 Function surface, grouped

**Bootstrap & runloop**
```cpp
void platform_init();                       // class registration, theme listener install
neui_render_backend_t* platform_get_backend();
bool platform_run();                        // block on event loop until last window closes
bool platform_pump_once();                  // drain pending events nonblocking; false on quit
bool platform_run_modal_until(bool* keep);  // nested loop for popup_menu (see §10)
```

**Windowing**
```cpp
void platform_create_appwindow (Session*, uint32_t widget_index, WidgetData&);
void platform_create_plugwindow(Session*, uint32_t widget_index, WidgetData&);   // borderless embed
void platform_create_dialog    (Session*, uint32_t widget_index, WidgetData&, void* owner_native);
void platform_destroy_window   (WidgetData&);

void platform_show_window      (void* native_handle);
void platform_hide_window      (void* native_handle);
void platform_set_window_enabled(void*, bool);   // for modal dialog: disable owner
void platform_activate_window  (void*);          // bring to front, focus
void platform_set_window_title (void*, const char* utf8);
void platform_set_window_pos   (void*, int x, int y, int w, int h, uint32_t dpi);
void platform_post_close       (void*);          // async close (WM_CLOSE / [window performClose:nil])
void platform_invalidate       (void*);          // mark client area dirty
float platform_get_scale_factor(void*);
void  platform_apply_size_constraints(void*, int minw, int minh, int maxw, int maxh);
void  platform_set_window_icon(WidgetData&, const char* path);
```

**Clipboard**
```cpp
bool platform_clipboard_set_text(const char* utf8, uint32_t length);
int  platform_clipboard_get_text(char* buf, int buflen);   // NULL queries size; bytes inc. NUL
bool platform_clipboard_has_text();
uint32_t platform_register_clipboard_listener(ClipboardChangeCallback cb, void* token);
void     platform_unregister_clipboard_listener(uint32_t handle);
```

**Image loading** (BGRA8 premultiplied output)
```cpp
uint8_t* platform_load_image(const char* path, uint32_t* w_out, uint32_t* h_out);
void     platform_free_image(uint8_t* pixels);
```

**Menubar** (12 functions; see `platform.h:140-209` for the exact set)
- `create / destroy / attach / refresh`
- `add_popup / add_item / add_separator`
- `remove_popup / remove_item`
- `enable_item / enable_popup`
- `set_item_text / set_item_shortcut`

Many platforms have no menu strip (iOS, Android, embedded) — on those, follow `platform_null.cpp:55-68` and make every menubar function a no-op.

### 4.2 What the platform must dispatch into the Session

Beyond the API surface, the platform layer must translate OS events into Session methods. The contract:

| OS event | Session call / event |
|---|---|
| Mouse move | `dispatch_event(MOUSE_MOVE)` (with hit-test, hover tracking) |
| Mouse enter / leave widget | `MOUSE_ENTER` / `MOUSE_LEAVE` |
| Button press | `set_focus(hit)`; capture pointer; `MOUSE_BUTTON_DOWN` |
| Button release | release capture; `MOUSE_BUTTON_UP`; if same widget, `MOUSE_BUTTON_CLICK` |
| Double-click | `MOUSE_BUTTON_DBLCLICK` (window class needs CS_DBLCLKS on Win32; macOS uses `clickCount==2`) |
| Right-button | `MOUSE_RBUTTON_DOWN` / `_UP` |
| Wheel | `MOUSE_WHEEL` with delta normalised to "signed lines" |
| Key press / release | `dispatch_key_to_focused(KEYDOWN/KEYUP, keycode, modifiers)` |
| Character input | `KEYCHAR` with codepoint |
| OS focus gained / lost | set `Session::_os_focused`; emit `WIDGET_FOCUS(focused=true/false)` to focused widget |
| Paint | `Session::paint_frame(...)` between `backend->begin_frame()` and `backend->end_frame()` |
| Resize | resize render context (physical px); `RESIZE` event (logical px); suppress when minimized |
| DPI change | `Session::on_dpi_changed`, update `wd.dpi`, resize backend |
| IME start / update / commit / end | `WidgetData::on_composition(kind, utf8, len, caret_byte, attrs)` |
| Close request | `APP_QUIT`; decrement appwindow count; quit pump on last close |
| Menu pick / accelerator | `Session::dispatch_menu_event(cmd_id)` |
| Clipboard external change | invoke registered listener callbacks on the UI thread |

The Session pointer is reachable from the native handle via a per-window user-data slot (`GWLP_USERDATA` on Win32 / window delegate / associated object on macOS). Decide how you'll attach it; copy the existing pattern.

### 4.3 Required Session-facing translations

- **Physical → logical px** at the platform boundary. The OS reports physical pixel coords (Win32 client area is phys px at the per-monitor DPI; AppKit also varies). Convert with `logical = phys * 96 / dpi`. Win32 reference: `phys_to_log` at `platform_win32.cpp:65`. macOS reference: `NSView` with `isFlipped=YES` keeps the CTM Y-down and matches our logical space automatically (`platform_macos.mm:80` and `cg_backend.mm:11-13`).
- **DPI source**: Win32 = `GetDpiForWindow(hwnd)` (and re-read in `WM_DPICHANGED`). macOS = `window.backingScaleFactor` × 96. Choose the equivalent on your platform.
- **Y-axis flip if needed**: Wayland / X11 are already top-left = (0,0). iOS UIView is top-left. AppKit `NSWindow` frame is bottom-left in screen coords, but `NSView` content can be flipped — set `isFlipped = YES` and you're done.

---

## 5. Event model — precise reference

### 5.1 Pointer / mouse

**Buttonmap bits** (declared in `platform.h:11-15`):
```cpp
enum : uint32_t {
  NEUI_MK_LBUTTON = 0x0001,   // matches Win32 MK_LBUTTON
  NEUI_MK_SHIFT   = 0x0004,   // matches Win32 MK_SHIFT
  NEUI_MK_CTRL    = 0x0008,   // matches Win32 MK_CONTROL
};
```
On Win32 you can forward `wParam` directly. On other platforms, OR the bits manually from the OS modifier state.

**Capture model**: when a button goes down on a widget, the platform layer must capture the pointer (Win32 `SetCapture`, macOS `_pressed_widget` field on the view, X11 `XGrabPointer`, etc.) so drag-out events keep routing to the original target. Release on UP.

**Hover tracking**: when the pointer enters a widget, emit `MOUSE_ENTER`; when it leaves (including window leave), emit `MOUSE_LEAVE`. Win32 uses `TrackMouseEvent(TME_LEAVE)`; AppKit uses `NSTrackingArea`.

**Double-click**: a second BUTTON_DOWN within the platform's double-click time + distance budget should arrive as `MOUSE_BUTTON_DBLCLICK`, not a second `_DOWN`. Win32: set `CS_DBLCLKS` on the window class. macOS: read `NSEvent.clickCount`. Other platforms: synthesise from timestamps + last-click position.

**Wheel**: `event.wheel.delta` is a **signed integer "lines" count** (positive = up). Normalise from your platform's raw delta. Win32: divide `WHEEL_DELTA` (120) and multiply by `SPI_GETWHEELSCROLLLINES`. macOS: scrollingDeltaY, with precise scrolling treated as `/16.0` and clamped. Match those for consistent feel.

**Pointer beyond mouse** (touch, pen): there's no separate touch event today. Touch should synthesise as button events with the appropriate modifiers. If the new platform is touch-primary (iOS, Android), map single-finger tap → BUTTON_DOWN/UP/CLICK on the same widget. Long-press → RBUTTON for context menus.

**Coordinate space inside events**: `event.mouse.x` / `event.mouse.y` are **frame-local logical px**. The platform layer must convert from window-client coordinates if those differ.

### 5.2 Keyboard

**Keycodes**: `NEUI_KEY_*` (in `include/neui/d/keys.h`) **share values with Win32 VK_***. On Win32 the platform forwards `wParam` of `WM_KEYDOWN` directly; on macOS it translates via `hosts/shared/macos/keys_macos.h::mac_keycode_to_neui` (kVK_* → NEUI_KEY_*). On a new platform, write the equivalent translation table.

**Modifier bits**: `NEUI_KMOD_NONE / SHIFT / CTRL / ALT / META`. **Convention: `NEUI_KMOD_CTRL` is the platform-primary modifier** (`Ctrl` on Windows/Linux, `Cmd` on macOS); `NEUI_KMOD_META` is the secondary (`Win`/`Super` on Windows/Linux, `Control` on macOS). Make sure your translation respects this — accelerators read in this order.

**KEYDOWN vs KEYCHAR**: emit `KEYDOWN` for the raw key press (with keycode + modifiers); emit `KEYCHAR` for the resulting Unicode codepoint after the platform's text composition (single codepoint per event). On Win32 this is `WM_KEYDOWN` → KEYDOWN, then `WM_CHAR` → KEYCHAR. UTF-16 surrogate pairs assemble across two `WM_CHAR`s (pattern: `pending_surrogate` on the window-user-data). On macOS, `keyDown:` emits KEYDOWN, and `NSTextInputClient::insertText:` emits KEYCHAR per codepoint.

**Tab key**: the platform should consume Tab / Shift-Tab and call `Session::focus_next` directly, NOT dispatch KEYDOWN. (Otherwise widgets that swallow KEYDOWN break focus traversal.)

### 5.3 Focus

**Logical focus** lives on `Session::_focused_widget` (a widget index). It is set by `set_focus()` and queried by paint and event handlers. The OS-level focus is mirrored as a bool on `Session::_os_focused`. When `_os_focused == false`, painters should hide caret + focus outline but keep the logical state intact.

The platform must mirror OS focus into the Session:
- On gain (`WM_SETFOCUS`, `becomeFirstResponder`, X11 FocusIn): `session->_os_focused = true`; emit `WIDGET_FOCUS(focused=true)` to the currently-focused widget; invalidate.
- On loss: same with `false`.

### 5.4 IME

Composition flows through `WidgetData::on_composition(kind, utf8, len, caret_byte, attrs)` where `kind ∈ { COMP_START, COMP_UPDATE, COMP_RESULT, COMP_END }`. The platform layer is responsible for:
- Suppressing OS-default composition window UI (Win32: clear `ISC_SHOWUICOMPOSITIONWINDOW` in `WM_IME_SETCONTEXT`).
- Translating the OS event stream to the four `kind` values.
- Converting UTF-16 / NSAttributedString into UTF-8 + byte-indexed caret + optional per-byte clause-attribute bytes.
- Positioning the OS candidate window: query `WidgetData::caret_rect_local`, convert to screen, hand to `ImmSetCandidateWindow` (Win32) / `firstRectForCharacterRange:` (macOS).

The widget itself accumulates `composition_text` / `composition_caret` and applies the EditHistory step only on `COMP_RESULT`.

If IME isn't required on your target (kiosk, embedded), make it a no-op — text widgets fall back to plain `KEYCHAR` typing.

### 5.5 Paint

Paint is the platform's job to trigger but the Session's job to execute:
1. OS paint event fires (`WM_PAINT`, `drawRect:`, X11 `Expose`).
2. Platform calls `Session::paint_frame(wd_index, w, h)` (which internally calls `backend->begin_frame`, walks the widget tree, calls `backend->end_frame`).
3. For per-frame CG-style backends, platform binds the current `CGContextRef` via the backend's `set_current_frame` helper before `paint_frame`. See `platform_macos.mm:111-124` (`drawRect:`) and `backends/cg/cg_backend.mm` for the pattern.

### 5.6 Resize / DPI

- Window resize → resize render context (physical px) → emit `NEUI_EVENT_RESIZE` (logical px). Suppress when minimised.
- DPI change → call `backend->update_dpi(ctx, new_dpi)` → update `wd.dpi` → cascade DPI to child painted contexts → repaint.
- `NEUI_ATTR_MIN_WIDTH` / `MAX_WIDTH` / etc. should clamp drag-resize. Win32: `WM_GETMINMAXINFO`. macOS: `NSWindow.min/maxSize`. Set via `platform_apply_size_constraints`.

---

## 6. Theme palette and provider

**Source of truth: `hosts/shared/theme_palette.h`**.

The `Palette` struct (a flat array indexed by `ColorRole`) is **process-wide** and **single-threaded** (`mutable_current_palette()`). Code reads via `current_palette()` or `color(role)`. A per-session override pointer (`active_palette_override_ptr`) wins when set, so a session can force a LIGHT / DARK / AUTO mode via `NEUI_ATTR_THEME_MODE`. Multi-session override is last-set-wins (acknowledged limitation).

To add system-theme integration on a new platform:
1. Write a `hosts/shared/<plat>/theme_provider_<plat>.h` header that:
   - On `platform_init`, reads the system colours and populates `mutable_current_palette()` (set every `ColorRole`).
   - Registers a system listener for theme / accent changes that, on change, repopulates the palette, **bumps `Palette::version`**, and calls `broadcast_theme_change()`.
2. Add an analogous `ScopedPaletteOverride scope(_effective_palette)` in your platform's frame creation / theme-change handler — see `platform_win32.cpp:1062` and the comments at `theme_palette.h:166-184`.
3. The Session subscribes to `register_theme_listener` in its constructor and unsubscribes in its destructor (already wired in the xpl host).

If your platform has no system theme to read, do nothing in this step — the defaults (`default_light_palette()` / `default_dark_palette()`) ship a reasonable look and `NEUI_ATTR_THEME_MODE` still works for per-session forcing.

---

## 7. Clipboard

The xpl host calls `platform_clipboard_*` for both the public `NEUI_API_CLIPBOARD` API and for `Ctrl-C/X/V` inside text widgets. The text fast path uses `platform_clipboard_{set,get,has}_text`; the item-based path uses `platform_clipboard_{read,write}_item` over the shared `neui_detail::DataItem` (`hosts/shared/clipboard_item.h`). Built-in MIMEs: `text/plain;charset=utf-8`, `text/html`, `text/uri-list`. Arbitrary MIMEs pass through as registered clipboard formats (Win32) or pasteboard UTI types (macOS). The same `DataItem` primitive will back drag&drop drop payloads.

Implement `set_text` / `get_text` / `has_text` for any platform that has a system clipboard. For platforms without one, the null-host stubs are fine.

The **listener** is what lets clients observe external clipboard changes. Two patterns:
- **Push** (Win32): hidden `HWND_MESSAGE` window + `AddClipboardFormatListener` → `WM_CLIPBOARDUPDATE` → callback. See `hosts/shared/win32/clipboard_listener_win32.h`.
- **Poll** (macOS): timer / runloop tick that compares `NSPasteboard.changeCount`. See `hosts/shared/macos/clipboard_macos.h`.

If your platform has neither, `platform_register_clipboard_listener` may return 0 (handle 0 = not registered; clients tolerate this).

---

## 8. Menubar and accelerators

If your platform has a system menu bar, implement the 12 `platform_menubar_*` functions. The framework formats accelerator labels itself via `hosts/shared/shortcut_format.h` and passes them appended to the item text after `\t` — the platform just renders them.

If your platform has no system menu bar (iOS, Android, embedded), make every `platform_menubar_*` function a no-op. Clients can still bind `set_shortcut` + `set_menu_cmd` for in-app shortcut routing; the framework converts those to `WM_COMMAND`-equivalent dispatch internally.

**Accelerator dispatch order matters**: in the pump, accelerator translation runs **before** `TranslateMessage` (Win32) so a focused text widget that handles Ctrl+Z via routed commands isn't bypassed. See `platform_win32.cpp:1276` for the canonical placement. On macOS, AppKit handles `keyEquivalent` directly on the `NSMenuItem` — no pump-time intervention needed.

---

## 9. Modal pump and popups

`platform_run_modal_until(bool* keep_running)` is a **nested message loop**. Pop the OS event queue, dispatch each event normally, and exit when `*keep_running == false`. The xpl host uses it for two things:

1. **Popup menus** (`widgets->popup_menu`, used by KNOB right-click "Reset to default"). The popup-menu overlay is Session-level; it sets a bool flag, runs `platform_run_modal_until(&flag)`, and clears the flag on click / Esc.
2. **Modal dialogs** (`NEUI_W_DIALOG` with `NEUI_ATTR_MODAL=1`). The owner's input is disabled via `platform_set_window_enabled(owner, false)`; the dialog runs its own message loop in this nested pump.

If your platform doesn't naturally support re-entrant pumps (some console RTOSes), you can simulate it by buffering events and dispatching synchronously — but the popup model assumes you can pump while another pump is on the stack. Verify before promising.

---

## 10. Build wiring (CMake)

Top-level `CMakeLists.txt` already has the gating pattern:
```cmake
if(WIN32)
  add_subdirectory("hosts/win32")
  add_subdirectory("backends/d2d")
endif()
if(APPLE)
  add_subdirectory("backends/cg")
  add_subdirectory("hosts/macos")
endif()
add_subdirectory("backends/null")
add_subdirectory("hosts/crossplatform")
```

For a new platform — say Linux — add:
```cmake
if(UNIX AND NOT APPLE)
  add_subdirectory("backends/cairo")   # or skia, etc.
endif()
```

The xpl host's `hosts/crossplatform/CMakeLists.txt` picks its platform source via `if(WIN32) / elseif(APPLE) / else()`. Add an `elseif(UNIX AND NOT APPLE)` branch:
```cmake
elseif(UNIX AND NOT APPLE)
  target_sources(neui-xplhost PRIVATE platform_linux.cpp)
  target_link_libraries(neui-xplhost PRIVATE neui-backend-cairo
    X11 cairo Xrandr)   # whatever you actually need
```

In `hosts/<your-backend>/CMakeLists.txt`, follow `backends/null/CMakeLists.txt` shape: produce a static lib `neui-backend-<name>` with `target_include_directories(... PUBLIC ${CMAKE_SOURCE_DIR}/include)` so consumers see `neui/neui.h`.

The example app (top-level CMakeLists `add_executable(neui_example ...)`) needs an executable kind appropriate to your platform (`WIN32` for windowed Win32, `MACOSX_BUNDLE` for an .app bundle, plain executable elsewhere) — copy the pattern at `CMakeLists.txt:52-82`.

---

## 11. Step-by-step port skeleton

Order matters — each step unlocks the next.

1. **Pick the scope.** Backend only? xpl-platform layer? Both? Native host? Refer to §1.
2. **Stub the CMake wiring** so the build still completes on the target platform. Use `null` backend + `platform_null.cpp`-style stubs as starting points. Confirm `cmake -B out/build && cmake --build out/build` succeeds and links `neui_example`.
3. **Implement `platform_get_backend`** to return your real or null backend. Decide if you're writing a new backend first or reusing an existing one.
4. **Implement window creation** (`platform_create_appwindow`) end-to-end:
   - Open a window of the requested logical size at the right physical resolution.
   - Bind the render context via `backend->create_context(native_handle, w_px, h_px)`.
   - Store the Session pointer on the window's user-data slot.
   - Read DPI and store on `wd.dpi`.
   - Return; the framework will call `platform_show_window` next.
5. **Implement the pump** (`platform_run`, `platform_pump_once`). Just enough to deliver paint events. Verify you can see the cleared frame colour.
6. **Wire paint**: on the OS paint event, call `Session::paint_frame(...)`. At this point you should see the example's static widgets drawn.
7. **Wire mouse**: enter/leave + button down/up/click/dblclick. Verify the button widget visibly reacts and the demo's check-boxes flip.
8. **Wire keyboard**: KEYDOWN + KEYCHAR. Verify INPUTBOX accepts text.
9. **Wire focus + tab traversal**: Tab cycles, focused widget paints its focus ring.
10. **Wire resize + DPI**: drag-resize updates the layout; moving to a different-DPI monitor (where applicable) re-flows.
11. **Wire close**: last APPWINDOW close exits the pump.
12. **Implement secondary features** in roughly this order: clipboard text → clipboard listener → image loading → menubar (if applicable) → IME → theme provider → modal dialogs / popup menus → window icon → size constraints.
13. **Sweep the gotchas in §13.**

---

## 12. Test checklist

After each step in §11, run the example (`out/build/.../neui_example`) and confirm:

- [ ] Window opens at the requested size; close button quits.
- [ ] Frame paints with the expected clear colour (verifies backend `begin_frame`).
- [ ] All static widgets render (labels, button, checkbox, section, knob).
- [ ] Mouse cursor over button triggers visible state change (hover invalidate).
- [ ] Click on button fires the example's onclick handler (verifies BUTTON_DOWN/UP/CLICK + emit_events routing).
- [ ] Double-click on the knob resets to default.
- [ ] Drag the knob clockwise: value increases (verifies MOUSE_MOVE + capture).
- [ ] Right-click the knob: popup menu appears, "Reset to default" picks dispatch through `platform_run_modal_until`.
- [ ] Tab cycles focus through tab-stop widgets; focused widget shows the focus ring.
- [ ] Type into the INPUTBOX. Backspace, Home/End, Ctrl-A/C/X/V work.
- [ ] Resize the window: widget layout adapts; `WIDGET_RESIZE` events fire on the frame.
- [ ] System dark/light toggle (if your platform has one): theme listener fires, palette flips, every painted widget repaints.
- [ ] (If menubar) menu picks dispatch; accelerators translate before TranslateMessage; menu items auto-grey on popup-open when their bound `NEUI_CMD_*` can't reach a consumer.
- [ ] Clipboard set/get/has work in INPUTBOX with Ctrl-C/X/V (or the platform's primary modifier).
- [ ] (If image loading) IMAGE widget shows `myimage.png` aspect-fitted.
- [ ] Modal DIALOG blocks input on the owner while open.
- [ ] No GDI/CG/GL/Vulkan handle leaks across open/close cycles (run a stress loop if you're unsure).

If any step fails, the gotcha checklist in §13 is the first place to look.

---

## 13. Known gotchas

**Coordinate space confusion** — `wd.x` / `wd.y` are **parent-relative** logical px. The xpl host caches a frame-local `abs_x` / `abs_y` recomputed top-down each paint (see `host.cpp::paint_widgets_recursive`). Use `abs_x` / `abs_y` when translating frame-local mouse coords (`event.mouse.x` / `.y`) into widget-local. Don't mix.

**Deferred HWND-style creation** — on win32 (native host) widget state is stored immediately; the OS handle is created in `widget_show()`. The xpl host doesn't have this split — `platform_create_appwindow` creates the OS window inside `widget_show`'s code path. Don't try to create an OS window inside `widget_create`; pending state would be lost.

**Widget id stale-after-slot-reuse** — slots are reused after destroy. Stale `neui_widget_t` from a destroyed widget can collide with a new widget in the same slot. No generation counter today (acknowledged limitation). Clients are expected to listen for `ondestroy` and drop their handle.

**Y-axis on macOS** — `NSWindow.frame` is bottom-left in screen coords, but `NEUIView` sets `isFlipped = YES` so the **content** coords are top-left. Don't flip again inside paint; the CTM is already configured correctly. When converting between screen and view coords, use AppKit's `convertRect:toView:` rather than manual arithmetic.

**Surrogate pairs (Win32)** — `WM_CHAR` delivers UTF-16 surrogate pairs as TWO messages. Buffer the high surrogate on the window user-data; combine on the next `WM_CHAR` to form the codepoint before emitting KEYCHAR. See `platform_win32.cpp:829`.

**Double-click window class flag** — without `CS_DBLCLKS` on the registered Win32 window class, `WM_LBUTTONDBLCLK` is never delivered and you'll only see DOWN events on every click. See `platform_win32.cpp:1036`.

**Section children parent to the section HWND on win32** — children of a SECTION are parented to the section's HWND in the xpl host's win32 build (not siblings of the section). STATIC labels inside a section get their background brush from the section's `PaintedWndProc::WM_CTLCOLORSTATIC` handler (`hosts/win32/window.cpp:386`). When implementing a new "painted container" widget, replicate this if you want native text controls hosted inside to follow your container's colours.

**Wheel delta normalisation** — the public event delta is **lines**, not raw OS units. Convert: divide Win32 raw by `WHEEL_DELTA` (120) and scale by `SPI_GETWHEELSCROLLLINES`; on macOS divide precise-scrolling deltas by 16 and clamp. Inconsistent normalisation makes scroll feel "way too fast" or "stuck".

**Capture release on UP must be unconditional** — if a drag goes outside the window and the OS forcibly cancels capture (alt-tab, system modal), the next button-down must still work. Always `ReleaseCapture` / clear `_pressed_widget` in the UP handler regardless of where the cursor ended up.

**Theme listener fires on every system colour-set change**, not just dark/light flip — accent changes, high-contrast toggles, etc. The palette `version` bump is the signal a downstream cache should listen for, not the dark/light bool alone. See `hosts/shared/win32/theme_brushes_win32.h` for the version-keyed brush cache pattern.

**Multi-session palette correctness** — `active_palette_override_ptr` is process-wide (last-set-wins). Single-session apps are unaffected. If you need true per-session isolation, swap the global pointer for thread-local storage in `theme_palette.h::active_palette_override_ptr()` — single-line change.

**`interface` is a Win32 macro** — never name a function parameter `interface` in any code that might compile on Windows. Use `iface`.

**`DEF_` prefix on event macros** — `MOUSE_EVENT` and `KEY_EVENT` collide with `wincontypes.h`. Event names in `include/neui/d/events.h` use `NEUI_EVENT_*`; don't rename for terseness.

**`emit_events = false` is the default** — a widget you create won't fire input events until you call `set_emit_events(true)` (or the widget type auto-sets it: BUTTON, INPUTBOX, CHECKBOX, LISTBOX, COMBOBOX, MULTILINE, TREEVIEW). Your custom container widget probably wants `false` so clicks pass through to children.

**Atomically: vtable evolution rule** — `neui_render_backend_t` and the other API vtables are append-only. New methods go at the end so the slot offsets stay stable. Pre-1.0 we allow breaking changes IF every host rebuilds; post-1.0 the contract freezes.

---

## 14. Reference implementations to crib from

| You need | Look at | Why it's a good model |
|---|---|---|
| Skeleton backend | `backends/null/null_backend.cpp` | Smallest complete vtable. |
| Real 2D backend | `backends/d2d/d2d_backend.cpp` | Per-call brush cache, transform stack, bitmap DPI handling. |
| Y-flipped backend | `backends/cg/cg_backend.mm` | One-context-per-frame via `set_current_frame`; `isFlipped=YES` interaction. |
| Skeleton platform | `hosts/crossplatform/platform_null.cpp` | Stub-and-link starting point. |
| Win32 platform | `hosts/crossplatform/platform_win32.cpp` | Full HWND pump, IME, focus, DPI, clipboard listener, menubar. |
| Cocoa platform | `hosts/crossplatform/platform_macos.mm` | NSResponder mouse/key, NSTextInputClient IME, NSPasteboard clipboard, NSMenu menubar. |
| Theme provider | `hosts/shared/win32/theme_provider_win32.h` | Registry + UISettings + WinRT events. |
| Theme provider (Apple) | `hosts/shared/macos/theme_provider_macos.h` | NSAppearance + KVO. |
| Keymap | `hosts/shared/macos/keys_macos.h` | kVK_* → NEUI_KEY_* translation table; modifier mapping including the Cmd-as-CTRL convention. |
| Clipboard | `hosts/shared/win32/clipboard_win32.h`, `hosts/shared/macos/clipboard_macos.h` | Both shapes (CF_UNICODETEXT vs NSPasteboardTypeString). |
| Clipboard listener | `hosts/shared/win32/clipboard_listener_win32.h` | `HWND_MESSAGE` pattern (push). macOS uses change-count polling — see the comments in `clipboard_macos.h`. |

---

## 15. Document maintenance

When the seams change (a function added to `neui_render_backend_t`, a new `platform_*`, a new shared helper):

1. Bump `NEUI_VERSION` in `include/neui/d/api.h` if the change is breaking.
2. Append (don't reorder) the new function pointer at the end of its vtable.
3. Update the matching table above so a future port sees the new requirement.
4. Add a one-line "Recent architecture decisions" entry to `CLAUDE.md`.

`plans/painter-and-asset-api.md` is the closest worked example still on disk - design rationale for an API extension that crossed all hosts. Two previous worked examples (`macos-port.md`, `native-macos-host.md`) shipped and were retired; their effective summary lives in CLAUDE.md.
