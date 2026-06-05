# Clipboard expansion + drag&drop

## Context

Clipboard and drag&drop both move MIME-typed data across the application boundary, so they share storage and lifetime rules. This plan covers the unified `neui_data_item_t` primitive, broader clipboard format coverage, drop-target reception, and drag-source initiation.

## What shipped

### Unified data item

- `neui_detail::DataItem` (mime -> bytes map) + `DataItemStore` (per-session slot table) in `hosts/shared/clipboard_item.h`.
- Public handle `neui_data_item_t` (`include/neui/d/clipboard.h`). `NEUI_CLIPBOARD_MIME_TEXT` kept as a `#define` alias to the new canonical `NEUI_MIME_TEXT`.
- Per-host Session field `_data_items` backs both clipboard items and (transient) DnD drop payloads.

### Clipboard format expansion

OS round-trips the built-in MIMEs plus arbitrary MIME passthrough:

| MIME | Win32 | macOS |
|------|-------|-------|
| `text/plain;charset=utf-8` | `CF_UNICODETEXT` (UTF-8 <-> UTF-16) | `NSPasteboardTypeString` |
| `text/html` | `CF_HTML` with descriptor header (`hosts/shared/win32/clipboard_format_html_win32.h`) | `NSPasteboardTypeHTML` |
| `text/uri-list` | `CF_HDROP` (DROPFILES struct, RFC 2483 URL encode/decode via `hosts/shared/win32/clipboard_format_urilist_win32.h`) | `NSPasteboardTypeFileURL` (per-URL, joined `\r\n`) |
| `application/*`, `*/*` | `RegisterClipboardFormatA(mime)` | pasteboard type = MIME string |

Platform seam: `platform_clipboard_write_item(item)` / `platform_clipboard_read_item(item)` in `hosts/crossplatform/platform.h`, implemented in `platform_win32.cpp` / `platform_macos.mm` / `platform_null.cpp`. Native hosts call the OS helpers directly via `clipboard_{read,write}_item_win32` / `_macos`.

The convenience `set_text` / `get_text` / `has_text` still bypass the item path for the hot Ctrl+C/X/V case.

**No push-style onchange callback.** Clients poll `has_text` / `item_has_format` on demand (e.g. inside a menu's `WM_INITMENUPOPUP` / `NSMenuValidation` handler) to gate Paste-like UI. The old `NEUI_API_CLIPBOARD_CLIENT` + `neui_clipboard_client_t` were removed; `hosts/shared/win32/clipboard_listener_win32.h` deleted.

### DnD drop targets

`NEUI_API_DND` (`include/neui/d/dnd.h`):
- `set_drop_target(widget, bool)`, `get_drop_target`, `set_accepted_formats(widget, mimes[], count)` (empty = wildcard), `accept(session, neui_dnd_action_t)` (`NONE=0 / COPY=1 / MOVE=2 / LINK=4`).
- `accept(...)` is the synchronous "what action I'm willing to take" call invoked from inside `onevent` during ENTER/MOVE/DROP. Cached in `Session::_last_accepted_action`, reported back to the OS pasteboard so the cursor reflects accept/reject. Calls outside a DnD dispatch are no-ops (gated by `Session::_in_dnd_dispatch`).

Events (`include/neui/d/events.h`, category `0x0007`):
- `NEUI_EVENT_DND_ENTER / MOVE / LEAVE / DROP`.
- Payload `neui_event_dnd_t { widget, x, y, buttonmap, formats, formats_count, data, suggested_action }`. `x/y` widget-local logical px. `formats` borrowed dispatch-scoped MIME list. `data` is `neui_data_item_none` on ENTER/MOVE/LEAVE, a live `neui_data_item_t` on DROP - **released the instant the callback returns**, so clients must `item_get_format` during dispatch.

Platform seam (`hosts/crossplatform/platform.h`): `platform_dnd_register_window(native_handle, session_ptr, frame_widget_id)` / `platform_dnd_unregister_window`. Called by widget_show for APPWINDOW / PLUGWINDOW / DIALOG, revoked on widget_destroy.

- **Win32** (xpl + native): `hosts/shared/win32/dnd_target_win32.h` defines an `IDropTarget` COM impl that extracts MIME bytes from `IDataObject` (probes `CF_UNICODETEXT`, `CF_HTML`, `CF_HDROP`, enumerates remaining MIME-named registered formats). One-time `OleInitialize` from `platform_init`. xpl host wires via `platform_win32.cpp::xpl_dnd_on_*`; native via `hosts/win32/widgets.cpp::register_frame_as_drop_target_w32`. Frame HWND owns the target; child HWNDs don't register their own (OS walks up the parent chain). **Do not combine `WS_EX_ACCEPTFILES` with `RegisterDragDrop` - the two paths conflict; no neui widget opts into the former.**
- **macOS** (xpl + native): `NEUIView` / `NEUINativeContentView` conform to `<NSDraggingDestination>`; `registerForDraggedTypes:` covers string / HTML / file-URL. Drag location goes through `convertPoint:fromView:nil` (content views are `isFlipped=YES`). Pasteboard read via shared `pb_read_item_macos` / `pb_collect_mimes_macos` (`hosts/shared/macos/clipboard_macos.h`).

Session DnD dispatch (`dispatch_dnd_enter / move / leave / drop`):
- **xpl host**: walks descendants of `frame_widget_idx` (uses xpl WidgetData's cached `abs_x` / `abs_y`), picks the deepest visible+enabled `drop_target` whose `accepted_mimes` intersects the drag's `formats`, falls back to the frame itself if no descendant matches.
- **win32 native host**: same descendant walk, but computes frame-local absolute coords on the fly by accumulating each WidgetData's parent-relative `wd.x` / `wd.y` (`Session::find_drop_target_in_frame_w32`). Caches `_current_drop_abs_x/y` so MOVE / LEAVE on the same target compute widget-local coords without re-walking.
- **macOS native host**: frame-only target for now (per-painted-view drop-target opt-in is deferred).

Re-targeting fires LEAVE on the previous target + ENTER on the new one before MOVE. DROP allocates a transient `DataItem` slot in `_data_items`, snapshots the OS bytes, fires the event with the live id, then releases the slot.

### DnD drag source

Initiating drags from inside the app. Shape that shipped:

- Public API on `neui_dnd_api_t` (`include/neui/d/dnd.h`): `neui_dnd_action_t begin_drag(session, source_widget, neui_data_item_t payload, uint32_t allowed_actions)`. Synchronous - blocks while the OS drag is in flight; returns the negotiated action (`NONE` on cancel / non-target drop).
- Client builds the payload via `clipboard_api->create_item` + `item_set_format` for each MIME, hands the id to `begin_drag`, releases the item the instant `begin_drag` returns. The framework snapshots formats internally before spinning the OS drag loop.
- Re-entry: `Session::_in_dnd_dispatch` blocks `begin_drag` from inside a DnD callback. Drop-target callbacks within the same session still fire while a drag-source spin is in flight, so an internal drag (left pane -> right pane in the same window) works.
- Platform seam: `platform_dnd_begin_drag(native_handle, item, allowed_actions)` in `hosts/crossplatform/platform.h`.
- **Win32** (`hosts/shared/win32/dnd_source_win32.h`): `DataObjectImpl : IDataObject` (read-only; format snapshot pre-encoded at construction - `CF_UNICODETEXT` for `text/plain`, `CF_HTML` via `clipboard_encode_cf_html`, `CF_HDROP` via inline DROPFILES build, arbitrary MIMEs via `RegisterClipboardFormatA`) + `DropSourceImpl : IDropSource` (default cursors; cancel on Esc; drop on left-button release) + `EnumFORMATETCImpl`. `DoDragDrop` on the calling thread. `OleInitialize` reuses the idempotent helper from `dnd_target_win32.h`.
- **macOS** (`hosts/shared/macos/dnd_source_macos.h`): `NEUIDragSource : NSObject<NSDraggingSource>` captures the final `NSDragOperation` from `draggingSession:endedAtPoint:operation:` and flips a `done` flag. `macos_run_drag_source` builds `NSDraggingItem`s from the DataItem (one shared `NSPasteboardItem` for text/HTML/MIME, one item per URL for `text/uri-list`), calls `beginDraggingSessionWithItems:event:source:` on the frame's content view, then spins `platform_run_modal_until(&src->done)` to match the synchronous Win32 contract. `NSDragOperation` constants do NOT match `DROPEFFECT_*`; explicit mapping in `nsop_to_dnd_action`.
- Host wiring: `dnd_api->begin_drag` shim in `hosts/crossplatform/widgets.cpp` (xpl: `find_parent_native_handle`), `hosts/win32/widgets.cpp` (native win32: `find_parent_hwnd`), `hosts/macos/widgets.mm` (native macOS: walk parents for `native_window`).

### Verification

- `examples/dnd_example.cpp` -> `neui_dnd_example.exe`. A CUSTOMDRAW that accepts `text/uri-list` + `text/plain;charset=utf-8`. Highlights its border on hover, parses dropped URIs / text, renders them as a list.
- `examples/dnd_source_example.cpp` -> `neui_dnd_source_example.exe`. Side-by-side source + drop panes in one window. Left pane records mouse-down, calls `begin_drag` past a 5 px threshold with `text/plain` + `text/uri-list` payload; right pane accepts the drop; status label reports `copy` / `move` / `link` / `cancelled`. Internal and external (to another app) drags both work.
- Builds + runs on Win32 (native + xpl). macOS code compiles in the same shape; not yet built on macOS hardware (must verify there).

## Deferred follow-ups

- Behavior-asset handler kind `DRAG_SOURCE` (declarative initiation from compound + behavior CUSTOMDRAW, paralleling `DRAG_VERTICAL` / `DRAG_HORIZONTAL` / etc.).
- Custom drag image (`NSImage` on macOS, `IDragSourceHelper` on Win32). Current implementation uses a small generic placeholder rect.
- Lazy promise data (`CFSTR_FILECONTENTS` / `NSFilePromiseProvider`).
- `image/png` clipboard + drag formats (separate item on the deferred list).

## Critical files

| Path | Purpose |
|------|---------|
| `include/neui/d/clipboard.h` | Public clipboard API + `neui_data_item_t` + MIME macros |
| `include/neui/d/dnd.h` | Public DnD API |
| `include/neui/d/events.h` | DnD event category 0x0007 + `neui_event_dnd_t` |
| `hosts/shared/clipboard_item.h` | `DataItem` / `DataItemStore` |
| `hosts/shared/win32/clipboard_win32.h` | OS-level multi-format read/write |
| `hosts/shared/win32/clipboard_format_html_win32.h` | CF_HTML descriptor encode/decode |
| `hosts/shared/win32/clipboard_format_urilist_win32.h` | CF_HDROP <-> text/uri-list (URL encode) |
| `hosts/shared/win32/dnd_target_win32.h` | `IDropTarget` impl + dispatch seam |
| `hosts/shared/win32/dnd_source_win32.h` | `IDataObject` + `IDropSource` + `IEnumFORMATETC` + `DoDragDrop` entry point |
| `hosts/shared/macos/clipboard_macos.h` | macOS multi-format read/write + DnD pasteboard helpers |
| `hosts/shared/macos/dnd_source_macos.h` | `NEUIDragSource <NSDraggingSource>` + `macos_run_drag_source` |
| `hosts/crossplatform/platform.h` | `platform_clipboard_*_item` + `platform_dnd_register_window` |
| `hosts/crossplatform/platform_win32.cpp` + `platform_macos.mm` + `platform_null.cpp` | Per-OS implementations |
| `hosts/crossplatform/host.{h,cpp}` + `hosts/crossplatform/widgets.cpp` | xpl Session DnD dispatch + `dnd_api` |
| `hosts/win32/host.{h,cpp}` + `hosts/win32/widgets.cpp` | Win32 native Session DnD dispatch + `dnd_api` + frame registration |
| `hosts/macos/host.{h,mm}` + `hosts/macos/window.mm` + `hosts/macos/widgets.mm` | macOS native Session DnD dispatch + `dnd_api` + content-view conformance |
| `examples/dnd_example.cpp` | Drop-receiver demo |
| `examples/dnd_source_example.cpp` | Drag-source + drop demo |
