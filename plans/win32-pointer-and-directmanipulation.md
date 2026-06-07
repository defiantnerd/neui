# Plan: Win32 WM_POINTER + DirectManipulation integration

**Status**: deferred. Not implementing now. This document is the binding spec for when it lands.

## Context

Two Win32 input gaps have surfaced together:

1. **Smooth-scroll quality.** `WM_MOUSEWHEEL` does not expose whether a notch is active user input or precision-touchpad inertia. The current `hosts/shared/scroll_kinetics.h` works around this with a quiet-tick debounce (`SCROLL_BOUNCE_DEBOUNCE_TICKS = 9`, ~150 ms) - the spring-back holds until the wheel stream goes silent. This is the right thing for a portable kinetics layer, but it can't compete with the OS's native PTP rubber-band feel; the debounce is a static guess at "user stopped" that DirectManipulation could derive from the actual input state machine.
2. **Pen + multi-touch.** neui has no public events for pen pressure / tilt / eraser or for multi-touch contacts. `WM_MOUSE*` synthesises a single pointer with no pressure data. On modern Windows, exposing real pen / touch needs `WM_POINTER*` - WinTab is legacy, RealTimeStylus conflicts with `WM_POINTER` on the same HWND, `WM_TOUCH` has no pressure.

The two pair naturally: DirectManipulation speaks the same `WM_POINTER*` stream we'd need for pen / multi-touch, and its `IDirectManipulationViewportEventHandler` exposes the running-vs-inertia disambiguation that the current debounce hand-waves. Migrating once buys both wins.

This plan adopts the **pragmatic middle path** (the one Firefox ships, per their Windows pointing-device docs): `EnableMouseInPointer` stays **off**, mouse keeps using `WM_MOUSE*` on every existing HWND, and `WM_POINTER*` is wired only on the specific custom-rendered widgets where it earns its keep. DirectManipulation then layers on top of that for scrolling surfaces (SECTION + GRID smooth mode).

## TL;DR

- **Two-level opt-in. Off by default.** A client that doesn't change anything sees the exact behaviour we ship today: mouse-only `NEUI_EVENT_MOUSE_*`, the existing kinetics path on scrolling SECTION + GRID, no `WM_POINTER*` traffic, no DirectManipulation viewports allocated. The new capabilities are gated by **(1)** a session-level master switch `NEUI_ATTR_SESSION_INPUT_POINTER` (boolean, default `0`) that authorises the host to install the `WM_POINTER*` plumbing + create DM viewports at all, and **(2)** a per-widget `NEUI_ATTR_INPUT_POINTER` (boolean, default `0`) that opts a specific widget into receiving the new pen / touch events. The master switch is the kill switch: with it off, the per-widget attr is inert, DM is never reached, and any future regressions in the new code path can't affect a client that hasn't asked for it. **Document this prominently in CLAUDE.md**: clients use the normal API; pen / touch / multi-touch / native PTP smoothness are explicitly enabled features.
- **One opt-in per widget HWND.** Even with the session switch on, only the widgets carrying `NEUI_ATTR_INPUT_POINTER=1` subscribe to `WM_POINTER*`. Each such HWND either handles the full `WM_POINTER*` family (and answers `DM_POINTERHITTEST` if DM is attached) or it handles none and lets `DefWindowProc` synthesise back to `WM_MOUSE*`.
- **Two new event categories** in `<neui/d/events.h>`: `NEUI_EVENT_PEN_*` (pressure / tilt / eraser, single contact) and `NEUI_EVENT_TOUCH_*` (per-contact pointerId / frameId, multi-touch). Mouse stays on its existing `NEUI_EVENT_MOUSE_*` events.
- **DirectManipulation owns wheel + PTP pan + spring-back** for scrolling SECTION + GRID on Win32 when DM is active. The shared `scroll_kinetics.h` stays as the macOS implementation + as the fallback on builds where DM viewport creation fails. The Win32 debounce path becomes dead code on the DM-active surfaces; we keep it in the binary for the fallback.
- **Pen pressure plumbs into KNOB and CUSTOMDRAW first.** KNOB gains pressure-modulated fine drag (replaces the current Ctrl-modifier fine path on pen). CUSTOMDRAW gets the raw pen event for clients building paint / draw surfaces.
- **Multi-touch goes to GRID + scrolling SECTION first** (two-finger pan, eventually pinch). KNOB / SLIDER do not gain multi-touch in v1.
- **Minimum Windows version**: Windows 8 / Server 2012 for the API surface. The `directmanipulation.h` header has shipped in the standard SDK since 8.x. We keep our existing `_WIN32_WINNT` floor (currently Win10) so the typed entry points and `EnableMouseInPointer` are always available; runtime-detect DM viewport creation success and fall through to the existing kinetics path if it fails.

## Goals / non-goals

**Goals**
- Pen pressure + tilt + eraser visible to clients on Win32.
- Multi-touch contacts visible to clients on Win32 (per-contact lifetime, frame grouping).
- Native PTP smooth-scroll + rubber-band on scrolling SECTION + GRID, matching the OS feel.
- The OS supplies the "active vs inertia" signal so the kinetics debounce isn't load-bearing on DM-active surfaces.
- No regressions for mouse-only users. Every existing widget keeps working unchanged.
- Cross-platform parity: pen + touch events flow through the same `neui_event_t` shape on every host that supports them (macOS NSEvent has `pressure` / `tilt` / `NSTouch`; the public payload is portable).

**Non-goals**
- No process-wide `EnableMouseInPointer(TRUE)`. Mouse stays on `WM_MOUSE*` everywhere.
- No WM_POINTER on widgets that don't earn it (BUTTON, LABEL, INPUTBOX, CHECKBOX, LISTBOX, COMBOBOX, TREEVIEW). They keep their `WM_MOUSE*` handlers.
- No pinch-zoom in v1. The first DM viewport allows `DIRECTMANIPULATION_MOTION_TRANSLATEX | TRANSLATEY` only; zoom motion type is reserved for a follow-up.
- No DM on the macOS or null hosts. DM is Win32-only; macOS gets its native PTP via NSEvent (already in place) and keeps the shared kinetics.
- No replacement of the kinetics primitive. `hosts/shared/scroll_kinetics.h` stays the macOS path and the Win32 fallback.

## Decisions (resolved)

- **Off by default; switchable per-session and per-widget.** Master switch `NEUI_ATTR_SESSION_INPUT_POINTER` (session-level, set via `attrs->set_session_int`, default `0`). When `0`, the host never subscribes to `WM_POINTER*`, never calls `EnableMouseInPointer`, never co-creates the DirectManipulation manager, and never allocates DM viewports - the behaviour is byte-identical to today's binary for clients who don't touch the new attr. When `1`, the per-widget `NEUI_ATTR_INPUT_POINTER` becomes meaningful and DM viewports are created on scrolling SECTION + GRID smooth-mode. The master switch is one-way *per session lifetime* - flipping it back to `0` after widgets have been created is treated as a no-op (we don't tear down running viewports; clients seed the value at session create time).
- **Mouse-in-pointer**: off. Per-widget `WM_POINTER*` opt-in only.
- **All-or-nothing per HWND**: every HWND that handles any `WM_POINTER*` message handles the full family for pen + touch, and returns 0 (not `DefWindowProc`). Plain mouse keeps falling through. Microsoft's "selectively consume = undefined behaviour" rule is per-HWND, not per-process.
- **Coordinate space**: `WM_POINTER*` `lParam` is **screen pixels**. We `ScreenToClient` per message in a single helper (`hosts/shared/win32/pointer_input_win32.h::pointer_to_client_logical`) that returns logical px at 96 DPI, matching every other input path.
- **Double-click**: `WM_POINTER` has no DBLCLK message. We debounce in the shared helper via `GetDoubleClickTime()` + `SM_CXDOUBLECLK / CYDOUBLECLK`, mirroring the existing CheckboxWidget pattern. Result is synthesised into `NEUI_EVENT_MOUSE_BUTTON_DBLCLICK` for pen tap-taps (and into a future `NEUI_EVENT_PEN_TAP_DBL` if clients need pen-specific timing).
- **Capture**: `SetPointerCapture(hwnd, pointerId)` per contact. `WM_POINTERCAPTURECHANGED` is the explicit cancel signal - it does NOT pair with enter/leave. Every widget handles it as a release-and-cancel-drag.
- **DM viewport scope**: one viewport per scrolling SECTION (Win32 native + xpl); one per GRID HWND when `NEUI_ATTR_GRID_SCROLL_MODE == SMOOTH` (and on xpl). The viewport rect is the SECTION body / GRID body (excluding header + scrollbar gutter). Content rect uses the auto-bounding content extent already computed by `widget_section_scroll.h` / `grid_model.h`.
- **DM delegate-thread**: callbacks can fire from a worker thread. We marshal back to the UI thread via a `PostMessage(hwnd, NEUI_WM_DM_UPDATE, ...)` registered window message and read the latest transform inside the wndproc. No direct UI-state mutation from the callback.
- **DM input forwarding**: `WM_POINTERDOWN` / `WM_POINTERUPDATE` / `WM_POINTERUP` / `DM_POINTERHITTEST` / `WM_MOUSEWHEEL` on the SECTION / GRID HWND are forwarded to `manager->ProcessInput`. `SetContact(pointerId)` is called from `WM_POINTERDOWN` / `DM_POINTERHITTEST` so the contact is captured for the viewport.
- **Wheel ownership when DM is active**: forwarding `WM_MOUSEWHEEL` to `ProcessInput` consumes it. The existing kinetics path on that widget is bypassed (no double-handling). The kinetics path remains alive as the fallback if DM viewport creation fails (e.g. compositor unavailable).
- **DM and the modal pump**: a modal dialog runs `platform_run_modal_until` (xpl) or `run_modal_until` (native) - both use `dispatch_one_message` / equivalent. DM doesn't need special pumping inside the modal loop: `manager->Update(nullptr)` is called from the `WM_PAINT` path which still runs under the modal pump. Confirm during implementation that the modal dialog can host a scrolling SECTION (small risk; verifiable with the existing dialog example).
- **DM and DnD**: DnD's `OleInitialize` is already called from `platform_init`. DM and DnD coexist on the same HWND (Edge / Office both ship this combination). The drop-target's `IDropTarget` callbacks remain on the message thread.
- **Backward compatibility**: a widget that doesn't opt into the new events sees zero change. The new event categories are additive; existing handlers ignore them.

## Phase 0: Public API additions

`include/neui/d/events.h`:

```c
// Category 0x0008
#define NEUI_EVENT_PEN_DOWN     (NEUI_EVENT_CAT_PEN     | 0x01)
#define NEUI_EVENT_PEN_UPDATE   (NEUI_EVENT_CAT_PEN     | 0x02)
#define NEUI_EVENT_PEN_UP       (NEUI_EVENT_CAT_PEN     | 0x03)
#define NEUI_EVENT_PEN_HOVER    (NEUI_EVENT_CAT_PEN     | 0x04)  // in-range, not in-contact

// Category 0x0009
#define NEUI_EVENT_TOUCH_DOWN   (NEUI_EVENT_CAT_TOUCH   | 0x01)
#define NEUI_EVENT_TOUCH_UPDATE (NEUI_EVENT_CAT_TOUCH   | 0x02)
#define NEUI_EVENT_TOUCH_UP     (NEUI_EVENT_CAT_TOUCH   | 0x03)
#define NEUI_EVENT_TOUCH_CANCEL (NEUI_EVENT_CAT_TOUCH   | 0x04)  // capture lost
```

Payloads:

```c
typedef struct neui_event_pen_t {
  neui_widget_t widget;
  int      x, y;             // widget-local logical px
  float    pressure;         // 0.0 .. 1.0
  float    tilt_x, tilt_y;   // -1.0 .. 1.0, normalised from -90..+90 deg
  float    rotation;         // 0.0 .. 1.0, normalised from 0..359 deg
  uint32_t flags;            // NEUI_PEN_FLAG_BARREL / _ERASER / _INVERTED
  uint32_t buttonmap;        // mirrors NEUI_MK_*
  uint32_t pointerid;        // OS-stable contact id (Win32 POINTER_INFO.pointerId)
} neui_event_pen_t;

typedef struct neui_event_touch_t {
  neui_widget_t widget;
  int      x, y;             // widget-local logical px (contact centroid)
  int      w, h;             // contact ellipse, logical px (Win32 rcContact, NSTouch majorRadius)
  float    pressure;         // 0.0 .. 1.0 (0.5 default if device doesn't report)
  uint32_t pointerid;        // per-contact lifetime
  uint32_t frameid;          // groups contacts reported together
} neui_event_touch_t;

#define NEUI_PEN_FLAG_BARREL    0x01
#define NEUI_PEN_FLAG_ERASER    0x02
#define NEUI_PEN_FLAG_INVERTED  0x04
```

`<neui/d/attrs.h>`:

```c
// Session-level master switch. Authorises the host to install the WM_POINTER*
// plumbing + create DirectManipulation viewports for this session. Default
// 0 = no-op: client sees the existing mouse-only API and the existing
// scroll-kinetics behaviour, byte-identical to a build without this plan.
// Set to 1 at session creation time to enable the new capabilities; the
// per-widget NEUI_ATTR_INPUT_POINTER below then becomes meaningful and
// scrolling SECTION + GRID-smooth begin allocating DM viewports.
#define NEUI_ATTR_SESSION_INPUT_POINTER  "neui.attr.session.input.pointer"  // int (bool)

// Per-widget opt-in. With the session switch above on, setting this on a
// widget (CUSTOMDRAW, KNOB, GRID, MULTILINE, scrolling SECTION) tells the
// host to handle the full WM_POINTER* family on that widget's HWND and to
// emit NEUI_EVENT_PEN_* / NEUI_EVENT_TOUCH_* to the client. Default 0.
// With the session switch off, this attr is inert (the host never reaches
// the code that would read it).
#define NEUI_ATTR_INPUT_POINTER          "neui.attr.input.pointer"          // int (bool)
```

Both rows are mandatory in `k_well_known_attrs` (the static assertion catches the omission). The session attr joins `NEUI_ATTR_THEME_MODE` as one of the very few session-level keys.

## Phase 1: Shared WM_POINTER plumbing

All entry points in this phase are no-ops unless `attrs->get_session_int(NEUI_ATTR_SESSION_INPUT_POINTER, 0) != 0`. The session-attr read happens once at `widget_show` time per widget (cached on `WidgetData`); after the cached bit is clear, the wndproc dispatch table picks the existing mouse-only path and the helpers below are never reached.

New header `hosts/shared/win32/pointer_input_win32.h`:

- `bool wm_pointer_is_relevant(UINT msg)` - returns true for the WM_POINTER* family + DM_POINTERHITTEST + WM_POINTERCAPTURECHANGED.
- `PointerInputClassify pointer_classify(WPARAM, LPARAM, HWND)` - returns `{ kind=mouse|pen|touch, pointerid, frameid, in_range, in_contact, primary, new_frame }`. Wraps `GET_POINTERID_WPARAM` + `GetPointerType` + `GetPointerInfo`.
- `bool pen_fill_event(POINTER_PEN_INFO&, neui_event_pen_t&, HWND, uint32_t dpi)` - populates the public payload (pressure, tilt, eraser via `PEN_FLAG_*`, screen->client coord, physical->logical px).
- `bool touch_fill_event(POINTER_TOUCH_INFO&, neui_event_touch_t&, HWND, uint32_t dpi)` - same shape for touch.
- `bool pointer_double_tap_track(PointerDblTapState&, int x, int y, DWORD msg_time)` - returns true on the second tap within `GetDoubleClickTime()` and the `SM_CXDOUBLECLK`/`SM_CYDOUBLECLK` box.

Coexistence rule enforced in the helper: **if `pointer_classify().kind == mouse`**, the helper returns `false` from a `pointer_should_consume(...)` predicate so the widget's wndproc returns `DefWindowProc` and the legacy `WM_MOUSE*` synthesis kicks in. Pen + touch return `true` and are fully handled.

`WM_POINTERCAPTURECHANGED` is unconditional: every opted-in HWND handles it as "release any active drag tracked by this pointerid" and returns 0.

## Phase 2: Per-widget WM_POINTER opt-in (Win32 native + xpl)

Each opted-in widget grows a `painted_msg_pointer_*` (or `*_pointer_w32`) handler alongside its existing `painted_msg_*` mouse handler. The wndproc dispatch table picks the pointer handler for `WM_POINTER*` messages, the mouse handler for `WM_MOUSE*`, and the existing key handler for keyboard.

Order of adoption:

1. **CUSTOMDRAW** - the widely-extensible surface. Clients opt in via `NEUI_ATTR_INPUT_POINTER`. Pen / touch events flow through `WIDGET_PAINT`'s peer events: `NEUI_EVENT_PEN_*` and `NEUI_EVENT_TOUCH_*` payloads, identical to how `MOUSE_*` flow today.
2. **KNOB** - replaces the existing Ctrl-fine-drag path with pressure-modulated fine drag when input arrives via pen. Pressure 0..1 maps to fine-scale 1.0 -> 0.2 (current fine_scale). Mouse + finger-touch keep the existing Ctrl-modifier path.
3. **MULTILINE** - pen tap = caret place + start drag-select; pen tap-tap = word select; pen tap-tap-tap = line select. Touch single-finger drag = scroll (delegated to DM viewport once Phase 3 lands).
4. **GRID** - pen click = cell select; pen drag = rubber-band selection (deferred follow-up). Touch single-finger drag = scroll (via DM in Phase 3). Two-finger drag = pan (Phase 3).
5. **Scrolling SECTION** - touch drag = scroll (via DM in Phase 3).

The xpl host (`hosts/crossplatform/widgets.cpp` / `platform_win32.cpp`) routes the new payloads through the existing `Session::dispatch_event` so clients receive them via `onevent`. The native host (`hosts/win32/widgets.cpp`) does the same on the per-HWND seam.

## Phase 3: DirectManipulation viewport on scrolling SECTION

New file pair:
- `hosts/shared/win32/direct_manipulation_win32.h` - shared seam (viewport creation, event handler base class, transform read-out).
- `hosts/crossplatform/platform_win32.cpp` (and `hosts/win32/widgets.cpp`) - per-section viewport lifecycle.

### Manager lifetime

Process-singleton `IDirectManipulationManager` lazily created on the first scrolling SECTION belonging to a session whose `NEUI_ATTR_SESSION_INPUT_POINTER` is set (same lazy pattern as the `OleInitialize` call). Sessions with the master switch off never trigger creation, so a client that doesn't opt in never pays the DM import / COM activation cost. Released at process shutdown when the last opted-in session is destroyed.

### Per-section viewport

On `widget_show` of a SECTION with `NEUI_ATTR_SCROLL_MODE != "none"` **and** the session-level `NEUI_ATTR_SESSION_INPUT_POINTER` set:

1. `manager->CreateViewport(/*frameInfo=*/nullptr, hwnd, IID_PPV_ARGS(&viewport))`.
2. `viewport->SetViewportRect(rect_for_body_in_physical_px)`.
3. `viewport->SetContentRect(rect_for_content_extent_in_physical_px)`.
4. `viewport->SetViewportOptions(DIRECTMANIPULATION_VIEWPORT_OPTIONS_MANUALUPDATE)` - we drive `Update()` from `WM_PAINT`.
5. `viewport->ActivateConfiguration(DIRECTMANIPULATION_CONFIGURATION_INTERACTION | _TRANSLATION_X | _TRANSLATION_Y | _RAILS_X | _RAILS_Y)`.
6. `viewport->SetChaining(DIRECTMANIPULATION_VIEWPORT_INTERACTION_CONFIGURATION_PAN_X | _PAN_Y)`.
7. `viewport->AddEventHandler(hwnd, my_handler, &cookie)`.
8. `manager->Activate(hwnd)`.
9. `viewport->Enable()`.

If any of steps 1-7 fails (no compositor, old build, transient failure), set `wud->dm_unavailable = true` for that HWND and the fallback kinetics path stays in force. Log the HRESULT via the existing debug helpers.

### Resize + content-extent changes

On `WM_SIZE` / `RESIZE` and on attr changes to `NEUI_ATTR_CONTENT_WIDTH` / `_HEIGHT`, call `viewport->SetViewportRect` / `SetContentRect`. DM clamps any in-flight pan to the new extent automatically.

### Input forwarding

In the section's HWND wndproc, before any local handling:

```c
case WM_POINTERDOWN:
case WM_POINTERUPDATE:
case WM_POINTERUP:
case WM_POINTERCAPTURECHANGED:
case DM_POINTERHITTEST:
case WM_MOUSEWHEEL: {
  if (dm_viewport_for(hwnd)) {
    if (msg == WM_POINTERDOWN || msg == DM_POINTERHITTEST) {
      UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
      dm_viewport_for(hwnd)->SetContact(pointerId);
    }
    manager->ProcessInput(&dispatched_handled);
    if (dispatched_handled) return 0;  // DM consumed it
  }
  // Fall through to existing handler (the fallback kinetics path).
}
```

Only POINTER and WHEEL are forwarded. Pen-pressure and touch-contact events that DM doesn't care about (because it's panning, not painting) still fire `NEUI_EVENT_PEN_*` / `NEUI_EVENT_TOUCH_*` on the widget through the Phase 2 path. The two coexist because DM intercepts only panning gestures; tap-only / pressure-only events fall through.

### Reading the transform

Implement `IDirectManipulationViewportEventHandler::OnContentUpdated(IDirectManipulationViewport*, IDirectManipulationContent* content)`:

```c
HRESULT __stdcall OnContentUpdated(...) override {
  float matrix[6];
  content->GetContentTransform(matrix, 6);
  // matrix is [m11, m12, m21, m22, tx, ty]. For pan-only we read tx, ty.
  // Marshal to UI thread (we are on the DM delegate thread).
  PostMessage(hwnd_, NEUI_WM_DM_UPDATE, 0, 0);
  std::lock_guard g(pending_lock_);
  pending_scroll_x_ = -matrix[4];   // DM gives content delta; section scroll_x is the opposite sign
  pending_scroll_y_ = -matrix[5];
  return S_OK;
}
```

`OnViewportStatusChanged` tracks the running / inertia / ready transitions. We expose this to the kinetics fallback as an authoritative "user has stopped scrolling" signal - but on DM-active HWNDs the kinetics path is already bypassed, so this is just diagnostic logging in v1.

In the UI thread `NEUI_WM_DM_UPDATE` handler: copy `pending_scroll_*` to the section's `SectionScrollState`, clear `kinetic_over_*` (DM owns the rubber-band visualisation; our `kinetic_over_*` paint-clamp guard is unused on DM-active sections), and `InvalidateRect`.

### Rubber-band visuals

DM applies the rubber-band by translating the *content* past the viewport bounds. Our SECTION paints children with a `translate(-scroll_x, -scroll_y)` already. When DM is active and the user pulls past the edge, `scroll_x` / `scroll_y` go negative or exceed max - exactly the existing rubber-band path's invariants. `clamp_section_scroll_idle` already skips the clamp while the kinetics own the overshoot; the DM-active path uses the same guard but driven by the DM viewport's status flag.

### Disabling DM

If a SECTION's `NEUI_ATTR_SCROLL_MODE` flips back to `"none"`, the viewport is `Disable()`d + the event handler removed + the viewport released. The kinetics path takes over.

## Phase 4: DirectManipulation viewport on GRID (smooth mode)

Same shape as Phase 3, with two adjustments:

- The viewport rect is the GRID body (excluding header + scrollbar gutter).
- The content rect is `(0, 0, total_columns_width, row_count * row_h)`.
- DM produces a flat px translation. We decompose into `scroll_offset_y` (rows) + `scroll_px_offset` (fine px) using the same `grid_scroll_commit` helper, but skip the `scroll_rubber` map (DM already applied damping).
- `NEUI_ATTR_GRID_SCROLL_MODE` interactions: `_STEPPED` keeps the existing `grid_scroll_step_rows` path and does NOT create a DM viewport. `_SMOOTH` and `_PLATFORM` (which already resolves to smooth on Win32 PTP) create the viewport.

GRID's keyboard nav + scrollbar drag + ensure-visible API write to `scroll_offset_y` directly today. On a DM-active GRID we additionally call `viewport->ZoomToRect(0, target_y, body_w, target_y + body_h, animate=true)` so the DM-owned scroll state stays in sync with the model-driven move.

## Phase 5: Touch-only widget interactions

After Phases 1-4, touch contacts arriving on widgets that don't currently subscribe (BUTTON, CHECKBOX, etc.) keep falling through to synthetic `WM_MOUSE*` - i.e. a tap acts like a click, exactly as today. No additional work needed.

Phase 5 widens the touch path on the opted-in widgets:

- **GRID + scrolling SECTION**: two-finger pan is implicitly handled by DM. No code.
- **KNOB**: two-finger rotation gesture (deferred follow-up). v1 = single-finger drag identical to mouse drag.
- **MULTILINE / INPUTBOX**: single-finger drag = scroll. Tap = caret. Tap-tap = word select. Tap-tap-tap = line select. Long-press = context menu (uses `DIRECTMANIPULATION_INERTIA_END` + `GetMessagePos` for the timer, or a separate `SetTimer` on touch-down).

## Phase 6: Documentation + verification

- `CLAUDE.md`:
  - New "Pen + touch input" section between "Drag & drop" and "Attribute API". Leads with the **opt-in design**: clients use the normal mouse API by default; pen / touch / multi-touch / DM-driven smooth scroll are switchable features authorised by the session-level `NEUI_ATTR_SESSION_INPUT_POINTER` and enabled per-widget via `NEUI_ATTR_INPUT_POINTER`. Then documents `NEUI_EVENT_PEN_*` / `_TOUCH_*` payloads and the all-or-nothing rule.
  - New "DirectManipulation (Win32 smooth scroll)" section after "GRID widget". Documents the manager / viewport lifecycle, the fallback path, and the modal-pump interaction.
  - Update SECTION + GRID rows in the widget table to note Win32 PTP smooth-scroll via DM.
  - Update the `Platform Implementation Gotchas` -> Win32 list with: (a) `WM_POINTER*` all-or-nothing per HWND, (b) `WM_POINTERCAPTURECHANGED` handling, (c) DM delegate-thread marshalling, (d) `EnableMouseInPointer` deliberately off.
- New example: `examples/pointer_input_example.cpp` - a CUSTOMDRAW that paints a pressure-modulated brush stroke, plus a touch-contact visualiser showing per-contact ellipse + id + frame grouping.
- Update `plans/win32-pointer-and-directmanipulation.md` status to "shipped" once landed.
- Verification matrix (Win32 hosts only - DM is Win32-only):
  - **Session switch off (default)**: existing examples (`neui_example`, `neui_grid_example`, `neui_section_scroll_example`) behave byte-identically. No `WM_POINTER*` subscription, no DM manager created, no per-widget allocation for the new paths. Verify with a debugger that `IDirectManipulationManager` is never instantiated.
  - Mouse-only laptop with session switch on but no widgets opted in: every existing widget unchanged. No `WM_POINTER*` traffic on those HWNDs; scrolling SECTION + GRID-smooth do create DM viewports (since they auto-opt-in when the session switch is on).
  - Precision touchpad: scrolling SECTION + GRID feel native (no debounce stall). Two-finger pan works.
  - Pen tablet (Wacom / Surface): pen-pressure brush works in `pointer_input_example`. Eraser-tip flag flips. Tilt visible.
  - Touch screen: tap = click, drag-on-section = scroll, multi-finger contacts visible in the example.
  - Modal dialog hosting a scrolling SECTION: DM viewport works inside the modal pump.
  - Old Win32 build / no compositor: viewport creation fails gracefully, kinetics fallback takes over, scrolling still works (just without DM smoothness).

## Risks / open questions

- **All-or-nothing rule audit.** Easy to forget on a new pointer-opted widget. Mitigation: a `pointer_widget_dispatch_w32` helper that owns the wndproc fallthrough decision; widgets call into it rather than writing their own switch. Code review checklist row: "WM_POINTER* and DM_POINTERHITTEST and WM_POINTERCAPTURECHANGED all handled?"
- **DM delegate-thread races.** The `pending_scroll_*` lock is a hot path on every `OnContentUpdated`. If it shows up in profiling, switch to a `std::atomic<int64_t>` packing tx + ty (24/40-bit split is sufficient for `[-32k, 32k]` px). Don't pre-optimise.
- **Modal dialog containing a scrolling SECTION.** `manager->Update(nullptr)` needs to be pumped during the nested modal loop. The xpl `platform_run_modal_until` and native `run_modal_until` both run `dispatch_one_message`; we route the DM update from `WM_PAINT`, which still fires under the modal pump, so this should just work. Confirm with a deliberate test.
- **DPI-change mid-session.** Existing `WM_DPICHANGED` path rescales widget HWNDs. The DM viewport rect + content rect need a fresh `SetViewportRect` / `SetContentRect` call after the rescale. Add to the existing `cascade_dpi` walker.
- **Pen pressure on non-pressure-aware tablets.** `POINTER_PEN_INFO::pressure` is 0 if the device doesn't report. We expose this as `pressure = 0.0` (callers can default-treat as 1.0 if they care). KNOB's pen-fine-drag path checks `pressure > 0.05` before applying pressure modulation; below that threshold it uses the mouse-equivalent fine-scale.
- **GRID `ZoomToRect` interaction with rubber-band.** DM's `ZoomToRect` animates the viewport; a concurrent active gesture might fight it. v1: skip `ZoomToRect` calls while `OnViewportStatusChanged` reports `DIRECTMANIPULATION_RUNNING`. Confirm during implementation.
- **Native (non-xpl) Win32 host scrolling SECTION.** The native host doesn't currently support scrolling SECTION (scrolling SECTION is xpl-only in the existing `section-scrolling.md` plan). Phase 3 lands DM on the xpl-host's SECTION HWND only until the native host gets scrolling. If the native scrolling SECTION lands first, swap the order.
- **The current scroll_kinetics debounce.** With DM active, the debounce is unused. Resist the urge to delete `SCROLL_BOUNCE_DEBOUNCE_TICKS` - it remains the macOS path and the Win32 fallback.

## Deferred follow-ups

- Pinch-zoom (`DIRECTMANIPULATION_CONFIGURATION_ZOOM`). Needs an API for the consumer widget to expose a "zoom level" attr + a render transform. Hardest part is the painter API not exposing arbitrary zoom today - a fixed-DPI logical-px assumption pervades widget paint code.
- Touch + pen on macOS (`NSTouch` for trackpad multi-touch; pen via NSEvent's pressure / tilt fields). The public event payloads in Phase 0 are already shaped for portability, so the macOS implementation is a follow-up wiring exercise, not a new design.
- Pen barrel-button → secondary-click routing (synthesise a `NEUI_EVENT_MOUSE_RBUTTON_DOWN` on barrel-down). v1: surface the flag, let clients decide.
- WinTab / WinInk fallback for ancient pen-only tablets that don't surface through `WM_POINTER`. Very low priority - modern Wacoms ship `WM_POINTER` since Win10.
- Programmatic `widgets->scroll_to(widget, x, y, animate=bool)` that uses `viewport->ZoomToRect` on DM-active surfaces and falls back to direct scroll-position writes elsewhere. Cross-references the deferred follow-up in `plans/section-scrolling.md`.
