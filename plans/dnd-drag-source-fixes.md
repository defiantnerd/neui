# Drag-source code-review fixes

**Status**: Batches 1-8 implemented; Batch 9 deferred as planned. Win32 compile + behavior verified on the Windows machine (landed in `399b704` / `4ebc2e8`). macOS interactively verified 2026-06-06 on real hardware: drag flow under the tracking-mode pump, drag-preview image + centred hot-spot, modifier-aware cursor (Ctrl/Shift/Ctrl+Shift incl. no-drop on unsatisfiable intent), TextEdit/Finder external round-trips, internal re-targeting, Esc cancel, repeated-drag stability - all pass. This plan is fully shipped.

## Context

A code review of the drag-source PR (`d6bc846..HEAD`) found 15 issues. This plan groups them into batches ordered by severity, so they can be landed incrementally. Batches 1-4 are high priority (hang risk, cross-host divergence, API safety); 5-7 are correctness/data-loss fixes; 8-9 are cleanup that can be deferred to a follow-up.

Verification at the end covers all batches together.

## Batch 1 - macOS drag-loop safety (Findings #1, #2)

`hosts/shared/macos/dnd_source_macos.h`.

**#1 - dnd_pump_until has no recovery path.** It blocks on `[NSDate distantFuture]` and the only signal that flips `done` is `draggingSession:endedAtPoint:operation:`. AppKit teardown paths that skip that delegate (window destroyed mid-drag, exception in a pasteboard writer, runloop-mode mismatch) leave the app frozen.

Fix:
- Drop `distantFuture`. Pump with a short timeout (50 ms is fine; the loop already wakes on every event so the timeout only matters when no events arrive):

  ```objc
  NSDate* until = [NSDate dateWithTimeIntervalSinceNow:0.05];
  NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                  untilDate:until
                                     inMode:NSEventTrackingRunLoopMode
                                    dequeue:YES];
  if (ev) [NSApp sendEvent:ev];
  ```
- Switch the runloop mode from `NSDefaultRunLoopMode` to `NSEventTrackingRunLoopMode`. AppKit drives the drag in tracking mode; pumping default mode is the suspected cause of the hang under modifier flags or rapid mouse motion. (Test both modes - if one is flaky, fall back to the other and document.)
- Add a watchdog: after the pump returns and `done` is still false, ask `[NSDraggingSession isEnumerationOrderEnumerable]` style probe (or hold an `NSDraggingSession*` returned by `beginDraggingSessionWithItems:` - store it on `NEUIDragSource` and check `[session draggingFormation]` for non-zero or use a separate `did_begin` flag). If the session was never observed entering, exit the pump after a few seconds with `finalOp = NSDragOperationNone`.

**#2 - nil-window bail.** `macos_run_drag_source` continues even when `[anchor_view window]` is nil, eventually handing AppKit an NSEvent with `windowNumber:0`.

Fix: add an explicit early-return at line 241:
```objc
NSWindow* win = [anchor_view window];
if (!win) return 0;
```
Move the `win` lookup before `build_dragging_items` and reuse it instead of re-fetching at line 245.

## Batch 2 - Native-host API validation (Finding #3)

`hosts/win32/widgets.cpp:5441`, `hosts/macos/widgets.mm:1473`. Both call `get_session(session)` instead of the documented cross-session-validating `get_session_for_widget(session, source_widget)`. The xpl shim is already correct.

Fix: in both native shims, change

```cpp
auto* s = get_session(session);
if (!s) return NEUI_DND_ACTION_NONE;
```
to
```cpp
auto* s = get_session_for_widget(session, source_widget);
if (!s) return NEUI_DND_ACTION_NONE;
```
Mechanical, two lines each. Convention already used 77+ times in `hosts/win32/widgets.cpp` and 80+ times in `hosts/macos/widgets.mm`.

## Batch 3 - Frame-as-source cross-host parity (Finding #4)

`Session::find_parent_native_handle` (xpl, `host.cpp:432`) and `Session::find_parent_hwnd` (win32, `widgets.cpp:2074`) walk `get_all_parents` which excludes the widget itself. The macOS native walker `find_owning_nswindow_macos` (`widgets.mm:1451-1466`) has an explicit self-fallback. Same `dnd->begin_drag(session, frame_widget, ...)` call works on macOS, returns NONE on Win32 + xpl.

Decision: do NOT change `find_parent_hwnd` / `find_parent_native_handle` - other callers may rely on the parent-only semantics. Instead, add the self-fallback at the begin_drag shim site, matching what macOS already does.

Fix: in `hosts/crossplatform/widgets.cpp::dnd_begin_drag` (around line 2799) and `hosts/win32/widgets.cpp::dnd_begin_drag` (around line 5447):

```cpp
void* native = s->find_parent_native_handle(WidgetToIndex(source_widget));
if (!native) {
  // Source widget itself might be the frame.
  auto* wd = s->get_widget(WidgetToIndex(source_widget));
  if (wd) native = wd->native_handle;  // or wd->hwnd on win32
}
if (!native) return NEUI_DND_ACTION_NONE;
```

For win32 use `wd->hwnd`, for xpl use `wd->native_handle`. Matches the macOS pattern exactly.

## Batch 4 - macOS modifier-aware suggested_action (Finding #5)

The Win32 path was just made modifier-aware via `dnd_dropeffect_suggested(effects, grfKeyState)`. The macOS paths (`platform_macos.mm:769` for xpl, `window.mm:966` for native) still use `[sender draggingSourceOperationMask]` alone - same client code that reads `suggested_action` gets modifier-driven cursor on Windows, constant cursor on macOS.

Fix: lift the modifier convention into a shared helper next to `dnd_dropeffect_suggested`, parameterised on the bit names:

Add `hosts/shared/dnd_modifier_suggest.h` (header-only, used by both platforms):

```cpp
namespace neui_detail {
  // Modifier convention shared by all hosts:
  //   Ctrl+Shift = Link
  //   Ctrl       = Copy
  //   Shift      = Move
  //   none       = first available (Copy > Move > Link)
  // Returned bit is masked against `available_actions` (NEUI_DND_ACTION_* bitmask).
  inline uint32_t dnd_suggest_action(uint32_t available, bool ctrl, bool shift)
  {
    if (ctrl && shift && (available & NEUI_DND_ACTION_LINK)) return NEUI_DND_ACTION_LINK;
    if (ctrl         && (available & NEUI_DND_ACTION_COPY)) return NEUI_DND_ACTION_COPY;
    if (shift        && (available & NEUI_DND_ACTION_MOVE)) return NEUI_DND_ACTION_MOVE;
    if (available & NEUI_DND_ACTION_COPY) return NEUI_DND_ACTION_COPY;
    if (available & NEUI_DND_ACTION_MOVE) return NEUI_DND_ACTION_MOVE;
    if (available & NEUI_DND_ACTION_LINK) return NEUI_DND_ACTION_LINK;
    return 0;
  }
}
```

Update:
- `dnd_target_win32.h::dnd_dropeffect_suggested` to call into it (translate DROPEFFECT_* ↔ NEUI_DND_ACTION_* once; the values happen to align 1:1 so this is just a name change).
- `platform_macos.mm::neui_dnd_suggested_from_op` and `window.mm::neui_native_dnd_suggested_from_op` to take the operation mask + read `[NSEvent modifierFlags]` for `NSEventModifierFlagControl` and `NSEventModifierFlagShift`, then call `dnd_suggest_action`.

Touchpoints on macOS:
- `[NSEvent modifierFlags]` is the modern call; reads the live modifier state. Safe to call from any thread but conventionally main-thread only.
- Pass the modifier bools into the existing `*_suggested_from_op` helpers and call `dnd_suggest_action`.

## Batch 5 - Modifier intent rejection (Finding #8)

`dnd_dropeffect_suggested` (and the new shared `dnd_suggest_action` from Batch 4) currently falls through to the default when Ctrl is pressed but COPY isn't available. The user pressed Ctrl expecting Copy; the framework silently picks MOVE. Cursor lies about modifier intent and a confirmed drop may delete the source content.

Fix: when a modifier is pressed but the requested action isn't in the available mask, return 0 (NEUI_DND_ACTION_NONE) instead of falling through. The OS will then render the no-drop cursor, giving the user honest feedback.

Update `dnd_suggest_action` (defined in Batch 4):
```cpp
inline uint32_t dnd_suggest_action(uint32_t available, bool ctrl, bool shift)
{
  // If the user expressed an explicit modifier intent, only satisfy it -
  // do not silently substitute. NONE (no-drop cursor) is the honest signal.
  if (ctrl && shift) return (available & NEUI_DND_ACTION_LINK) ? NEUI_DND_ACTION_LINK : 0;
  if (ctrl)          return (available & NEUI_DND_ACTION_COPY) ? NEUI_DND_ACTION_COPY : 0;
  if (shift)         return (available & NEUI_DND_ACTION_MOVE) ? NEUI_DND_ACTION_MOVE : 0;
  // No modifier: default priority.
  if (available & NEUI_DND_ACTION_COPY) return NEUI_DND_ACTION_COPY;
  if (available & NEUI_DND_ACTION_MOVE) return NEUI_DND_ACTION_MOVE;
  if (available & NEUI_DND_ACTION_LINK) return NEUI_DND_ACTION_LINK;
  return 0;
}
```

## Batch 6 - Re-entry guard scope (Finding #7)

`_in_dnd_dispatch` is set/cleared callback-scoped (set true around the synchronous `onevent`, false after). begin_drag's re-entry check only catches the case "client called begin_drag from inside a DnD callback". A non-DnD event firing in an idle gap during an active drag (timer, animation tick) can pass the guard and recursively start a second drag.

Fix: add a separate `bool _drag_source_active` field on each `Session` (xpl, win32, macos). Bracket the platform call in `dnd_begin_drag`:

```cpp
if (s->_in_dnd_dispatch)  return NEUI_DND_ACTION_NONE;
if (s->_drag_source_active) return NEUI_DND_ACTION_NONE;
s->_drag_source_active = true;
uint32_t r = platform_dnd_begin_drag(native, item, allowed_actions);
s->_drag_source_active = false;
return static_cast<neui_dnd_action_t>(r);
```

Update all three `dnd_begin_drag` shims (xpl `widgets.cpp:2799`, win32 `widgets.cpp:5447`, macos `widgets.mm:1473`). Field declaration goes next to `_in_dnd_dispatch` on each Session class.

## Batch 7 - Format-encoding correctness

### #6 - non-file:// URIs in text/uri-list

`hosts/shared/win32/dnd_source_win32.h:88-125`. The text/uri-list branch funnels all URIs through `urilist_uri_to_path`, which returns empty for any non-file scheme. If all URIs are http://, mailto:, etc., the entire CF_HDROP entry is skipped with no fallback.

Fix: when `paths.empty()` after filtering, fall back to publishing the raw URIs as CF_UNICODETEXT (joined with CRLF) so browsers and link bars receive something. Implementation: split the URIs into file/non-file lists; emit CF_HDROP only for file URIs; for non-file URIs emit a CF_UNICODETEXT entry containing the URI list. Also consider registering `CFSTR_INETURLW` ("UniformResourceLocatorW") for single-URI HTTP payloads - that's what browsers expect for shortcut creation - but this can be deferred.

For now, the minimum fix is the CF_UNICODETEXT fallback - keeps the URI payload reachable for any text-aware receiver:

```cpp
if (mime == "text/uri-list") {
  auto uris = urilist_parse(bytes.data(), bytes.size());
  std::vector<std::wstring> file_paths;
  std::vector<std::string>  other_uris;
  for (auto& u : uris) {
    auto wp = urilist_uri_to_path(u);
    if (!wp.empty()) file_paths.push_back(std::move(wp));
    else             other_uris.push_back(u);
  }
  if (!file_paths.empty()) {
    // existing CF_HDROP build using file_paths
  }
  if (!other_uris.empty() && !has_text_plain_already(out)) {
    // Build a CF_UNICODETEXT entry joining other_uris with CRLF.
    // has_text_plain_already() avoids overwriting an explicit text/plain.
  }
  return;
}
```

### #9 - text + URLs split into separate NSDraggingItems

`hosts/shared/macos/dnd_source_macos.h:115-208`. `build_dragging_items` puts text/HTML/MIME on one shared `NSPasteboardItem`, then emits one separate `NSDraggingItem` per URL. Win32's `IDataObject` serves all formats from one composite object; macOS splits them.

Fix: duplicate the shared text/HTML/MIME payload onto **every** per-URL `NSPasteboardItem`. That way each NSDraggingItem carries the full context, mirroring the "one composite source" model receivers expect. Code shape:

```objc
// Build text/html/MIME bytes once into a small dictionary keyed by NSPasteboard type.
NSMutableDictionary<NSPasteboardType, id>* shared_payloads = ...;

// For each URL, allocate its own NSPasteboardItem and stamp the shared payloads onto it
// before adding the URL's own NSURL data (NSPasteboardTypeFileURL).
for (NSURL* u : uri_urls) {
  NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
  for (NSPasteboardType type in shared_payloads) {
    id val = shared_payloads[type];
    if ([val isKindOfClass:[NSString class]])
      [item setString:val forType:type];
    else
      [item setData:val forType:type];
  }
  [item setString:[u absoluteString] forType:NSPasteboardTypeFileURL];
  NSDraggingItem* di = [[NSDraggingItem alloc] initWithPasteboardWriter:item];
  [di setDraggingFrame:frame_for() contents:placeholder];
  [items addObject:di];
}

// If there are no URLs, the shared item alone is the drag.
if (uri_urls.empty() && shared_has_any) {
  NSDraggingItem* di = [[NSDraggingItem alloc] initWithPasteboardWriter:shared];
  ...
}
```

The `[u absoluteString] forType:NSPasteboardTypeFileURL` form is one common way to write file URLs onto a per-item pasteboard; if that doesn't survive Finder's round-trip we'd switch to writing the URL via `[NSPasteboardItem setPropertyList:forType:]` instead. Verify against Finder + TextEdit during testing.

### #10 - duplicate CF_UNICODETEXT entries

`hosts/shared/win32/dnd_source_win32.h:53`. When a DataItem holds both `text/plain` and `text/plain;charset=utf-8`, both pass `mime == "..."` checks and the snapshot emits two CF_UNICODETEXT entries.

Fix: de-dup before push. Add a small `has_cf(out, cf)` predicate and skip if already present. The "first wins" semantics keep things stable - if a client sets both MIMEs, the first one encountered is the canonical payload.

```cpp
auto has_cf = [&](CLIPFORMAT cf) {
  for (auto& e : out) if (e.cf == cf) return true;
  return false;
};
// ... inside text/plain branch:
if (has_cf(CF_UNICODETEXT)) return;
```

### #12 - macOS uri-list whitespace trim

`hosts/shared/macos/dnd_source_macos.h:177`. The Win32 `urilist_parse` trims trailing spaces; the macOS inline parser doesn't.

Fix: add the same trim before `[NSURL URLWithString:]`. Or, better, replace the inline parser with a shared helper (see Batch 9).

Trim:
```cpp
while (end > i && (p[end - 1] == ' ' || p[end - 1] == '\t')) --end;
```
inserted before the empty-line check.

## Batch 8 - COM leak (Finding #11)

`hosts/shared/win32/dnd_target_win32.h:137` (in `dnd_pull_item_from_data_object`) and `:347` (in `DropTargetImpl::cache_formats`). Both iterate `IEnumFORMATETC::Next` and ignore `fes[i].ptd`. Per MSDN the caller must `CoTaskMemFree` each non-null ptd.

Fix: after the inner `for (ULONG i = 0; ...)` loop in each spot, add:

```cpp
for (ULONG i = 0; i < fetched; ++i) {
  if (fes[i].ptd) {
    CoTaskMemFree(fes[i].ptd);
    fes[i].ptd = nullptr;
  }
}
```

Alternatively wrap the cleanup inside the existing per-format loop. Two-line addition per call site.

## Batch 9 - Deferred cleanup (Findings #13, #14, #15)

These are altitude / efficiency findings - real but lower priority than the correctness fixes above. Land separately or fold into the next DnD-touching PR.

**#13 - CF_HDROP layout duplication.** `dnd_source_win32.h:87-125` reimplements the DROPFILES build that `urilist_to_hdrop_global` in `clipboard_format_urilist_win32.h` already does. Extract a shared `urilist_to_dropfiles_bytes(paths) -> std::vector<uint8_t>` helper; have both the HGLOBAL writer and the DataObjectImpl snapshot call it. Eliminates the `is_hdrop` flag on `DragSourceFormat` (every entry becomes byte-shaped).

**#14 - find_drop_target_descendants duplication.** Three implementations: xpl (`host.cpp:515`), win32 native (`host.cpp:388`), macOS native (`host.mm`). Same algorithm, slightly different coord-source strategies. Unify into one helper that takes a "how to get widget rect" callback, or push the xpl pattern (cache `abs_x`/`abs_y` on every `WidgetData`) onto the native hosts and remove the accumulator variants. Bigger change; defer.

**#15 - DataObjectImpl eager encoding.** `drag_source_snapshot` pre-encodes every format. For large payloads (image/png coming soon per the deferred-issues list) this is wasted work. Switch to: snapshot raw `(mime, bytes)` pairs at construction, materialise the OS-shaped encoded payload inside `GetData` on first request, cache it. EnumFormatEtc can still enumerate eagerly. Defer until the next DnD payload type lands.

## Critical files

| Path | Batches |
|---|---|
| `hosts/shared/macos/dnd_source_macos.h` | 1, 7 (#12), 9 (#15) |
| `hosts/win32/widgets.cpp` | 2, 3, 6 |
| `hosts/macos/widgets.mm` | 2, 6 |
| `hosts/crossplatform/widgets.cpp` | 3, 6 |
| `hosts/shared/dnd_modifier_suggest.h` (new) | 4, 5 |
| `hosts/shared/win32/dnd_target_win32.h` | 4, 5, 8 |
| `hosts/crossplatform/platform_macos.mm` | 4 (xpl drop-target) |
| `hosts/macos/window.mm` | 4 (native drop-target) |
| `hosts/crossplatform/host.{h,cpp}` | 6 (`_drag_source_active`) |
| `hosts/win32/host.{h,cpp}` | 6 |
| `hosts/macos/host.{h,mm}` | 6 |
| `hosts/shared/win32/dnd_source_win32.h` | 7 (#6, #10) |
| `hosts/shared/win32/clipboard_format_urilist_win32.h` | 9 (#13) |

## Verification

Build clean on Win32. Manual smoke tests against `neui_dnd_source_example.exe`:

1. **Modifiers** (Batch 4, 5):
   - Drag from left pane → right pane, no modifier. Cursor shows copy.
   - Hold Ctrl → cursor stays copy (allowed mask includes COPY).
   - Hold Shift → cursor changes to move.
   - Hold Ctrl+Shift → cursor changes to link (because the example now advertises LINK).
   - Modify example to advertise only MOVE | LINK; hold Ctrl → cursor shows no-drop (rejection of unsatisfiable Ctrl intent).
2. **Frame-as-source** (Batch 3): pass `app.win_id` as source_widget to a synthetic begin_drag - drag should start on all three hosts.
3. **Cross-session** (Batch 2): construct two sessions, pass session A with a widget from session B - begin_drag returns NONE silently.
4. **Re-entry** (Batch 6): set up a timer that calls begin_drag every 16 ms; start a drag from a button. Recursive timer-triggered begin_drag calls return NONE; only the first drag proceeds.
5. **macOS hang** (Batch 1): start a drag, then programmatically destroy the source window via `[NSApp terminate:]` mid-drag. The pump should exit within seconds via the timeout watchdog rather than hanging.
6. **Format edges** (Batch 7):
   - DataItem with only `https://example.com` as text/uri-list - drag to Notepad / a browser address bar. Notepad receives the URI (CF_UNICODETEXT fallback worked).
   - DataItem with text + 3 file URIs - drag to TextEdit; text appears. Drag to Finder; the three files attempt to drop.
   - DataItem with both `text/plain` and `text/plain;charset=utf-8` - drag to Notepad. Notepad receives one consistent payload (no duplicate CF_UNICODETEXT entries).
7. **Lifetime** (sanity): repeat 50 drags - no growth in process memory.

macOS-specific verification deferred until you're on a Mac:
- The runloop-mode + pump-timeout fix (Batch 1) needs to be observed under real AppKit drag flow.
- The build_dragging_items multi-item shape (Batch 7 #9) needs Finder + TextEdit round-trip checks.
- The modifier-aware drop-target suggestion (Batch 4) needs a real modifier-key drag against a neui drop target.

## Out of scope

Beyond the 15 review findings, two further items came up during review that are not in this plan:

- **Behavior-asset `DRAG_SOURCE` handler kind** - declarative drag initiation from compound + behavior CUSTOMDRAW. Already on the deferred list in `plans/clipboard-and-dnd.md`.
- **Custom drag image / lazy promises** - `IDragSourceHelper` on Win32, `NSImage` thumbnails on macOS. Deferred.
