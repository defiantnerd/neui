# Popup surfaces (`NEUI_W_POPUPSURFACE` / `NEUI_API_POPUP`)

An overlay that may leave its owner frame. Public API and the client-facing
contract live in `include/neui/d/popup.h`; the design rationale and the open
questions are in `plans/popup-surface.md` (response to issue #23). This file is
the implementer's map.

## Why it exists

A popup painted into the owner frame's surface cannot be bigger than that frame.
For a desktop app that is fine; for a plugin editor it is a hard ceiling. Preset
browsers, modulation pickers and FX selectors routinely need more room than the
editor window has, and every commercial plugin serves them by putting the menu on
the desktop as its own window over the DAW.

`popup_tree_menu` solved the *richness* half of that (submenus, checkmarks,
enable, shortcut labels, command routing). This is the *placement* half, and
generalized: not a menu, but a surface a client fills with ordinary widgets.

## It is a frame kind

`PopupSurfaceWidget : FrameWidget` (`hosts/crossplatform/host.h`). That is the
load-bearing choice: a frame already owns a coordinate origin, a render context
and the root of a subtree, so the two backings differ only in whether the widget
carries a `native_handle`.

| backing | `native_handle` | painting |
|---|---|---|
| desktop | from `platform_create_popup_surface` | its own window, its own surface |
| in-frame | none | composited into the owner's surface, clipped to its client rect |

Both run the **same** paint walk and the **same** hit-test walk over the
children, so a `GRID` or a scrolling `SECTION` inside a popup behaves identically
under either. The older popup code (`paint_popup_menu` / `paint_tree_popup`) is a
bespoke paint pass that can never host a real widget — which is exactly why the
client workaround in #23 had to hand-roll menu rows into a `CUSTOMDRAW`.

Consequences worth knowing:

- A surface is created with `parent = widget_none`, like any other frame, and is
  its **own root child**. The owner's subtree does not contain it — see the
  destroy path below.
- `widgets->show` on a surface is **ignored**. It has no position until something
  anchors it, and `wd.x`/`wd.y` are zero until placed. `NEUI_API_POPUP::open` is
  the entry point.
- A surface stores **screen** coordinates in `x`/`y`. It is the one frame kind
  whose position is not parent-relative, because it has no parent to be relative
  to and the platform needs screen placement.

## Placement

`Session::open_popup_surface` (`host.cpp`) does all of it, portably:

1. resolve the anchor's frame, and from it the **root owner** — walk out through
   any popup levels until a real window frame is reached. Ownership is **flat**:
   every level is owned by that root, never by the level above it.
2. `ensure_abs_positions` on the anchor's frame, then take the anchor rect in
   client coordinates.
3. `platform_client_to_screen` → screen logical px; `NEUI_POPUP_AT_POINTER` uses
   `platform_get_pointer_pos` instead.
4. offset per `neui_popup_side_t`, then **flip** to the opposite side if the
   preferred one does not fit and the other does.
5. clamp the **position** into `platform_get_work_area`. Never the size:
   silently shrinking a surface would change a layout the client already
   committed to. `get_clamp_size` is how a client learns the box in advance.
6. inherit the owner's `NEUI_ATTR_UI_SCALE`, or a 100 % popup appears over a
   150 % editor.

There is deliberately **no screen-coordinate form** in the public API: neui's
contract is logical px at 96 DPI and `UI_SCALE` is built on it, so absolute
screen placement would introduce a second coordinate space that multi-monitor DPI
then makes ambiguous.

## The input gate — not a pointer capture

While a stack is open the widgets underneath must stop behaving like a live UI.
That **suppression**, not event delivery, is what an OS pointer capture was ever
buying, so it lives in the session (`popup_gate_press` / `_hover` / `_key` /
`popup_take_release`) and is shared by both backings and all platforms:

- press outside the stack → close everything, **swallow** (and swallow the paired
  release). One click must not both close a picker and move a parameter.
- press on a shallower level → close the deeper levels, do **not** swallow, so
  clicking a parent row re-targets in one click.
- hover outside the stack → swallowed, and `set_hovered(0)`. Without this,
  moving off the popup lights up every widget it passes, which is the one thing
  that reads as "not a menu".
- Escape → close. That is the whole of v1's keyboard story.

Unlike `widget_set_owner`'s dialog modality, the owner window stays **enabled** —
the press physically reaches us and the session decides it means "dismiss". That
distinction is what #23 could not express client-side.

The platform layer contributes only the presses our own windows never see:

| | mechanism | coverage |
|---|---|---|
| macOS | `addLocalMonitorForEventsMatchingMask` (in-process → the DAW's own views) + `NSApplicationDidResignActive` | complete, no grab |
| X11 | `XGrabPointer` with `owner_events=True` | complete — that grab *is* the menu mechanism there and still delivers normally over our own windows |
| win32 | our windows + `WM_ACTIVATEAPP` | **gap**: a press in the DAW's own UI outside our child HWND raises no activation change, because we never held activation to lose |

X11 needs one extra step the other two do not. With `owner_events=True` a press
that landed outside *every* window of ours is reported against the **grab
window** — the popup itself — with coordinates outside its bounds. Read through
`popup_gate_press` that looks like a press *inside* the stack, so nothing would
ever dismiss. `popup_press_landed_outside` in `platform_linux.cpp` makes the
outside-ness call from the coordinates and routes it to
`Session::popup_gate_press_outside`, which owns the dismissal and the
release-swallow bookkeeping just like the ordinary path.

## Dismissal

Host-owned, all of it, and every route reports `NEUI_EVENT_POPUP_DISMISSED` with
a `neui_popup_dismiss_reason_t` so a client has one place to drop its state. A
dismissal cannot be vetoed — a popup that could refuse to close is a window stuck
over someone's DAW.

Triggers: outside press; owner deactivated or another app activated; owner moved
/ resized (that is what OS menus do — following is not worth the machinery);
owner hidden or destroyed; Escape; `close` / `close_all`.

Two lifetime hazards the code exists to prevent:

- **Destroying the owner.** The surface is its own root child, so the owner's
  subtree does not contain it. `close_popup_surfaces_if_within` therefore checks
  the surface, the recorded **anchor** and the recorded **owner**. Without it,
  closing a plugin editor with a picker open leaves a live borderless window over
  the DAW belonging to a dead frame, with the outside-press watch still running.
- **Session teardown.** `~Session` closes the stack and ends the watch, next to
  the relative-pointer and cursor releases, for the same reason they are there.

Closing pops the level from `_popup_surfaces` **before** any teardown or client
dispatch, so the stack strictly shrinks and a client that closes another surface
from its dismissal handler simply pops more. There is no re-entrancy flag on
purpose: a blocking guard would leave a level in the stack naming a widget the
handler had already destroyed.

## Per-platform notes

### macOS

- Non-activating borderless `NSPanel`
  (`NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel`),
  `hidesOnDeactivate = NO` (dismissal is ours to decide),
  `worksWhenModal = YES`, `NSPopUpMenuWindowLevel`.
- `addChildWindow:ordered:NSWindowAbove` on the owner is what makes the popup
  travel with the editor instead of being stranded behind it. Re-established on
  every show, because the owner can differ between opens. **In the embedded case
  the owner window is the DAW's**, which we did not create — the documented
  best-effort part of the feature.
- `NEUIView`'s tracking area switches to `NSTrackingActiveAlways` for a popup
  view. `NSTrackingActiveInKeyWindow` would deliver no `mouseMoved:` /
  `mouseEntered:` / `mouseExited:` at all to a window that never becomes key —
  this is the enter/leave cost of not taking a capture.
- `xpl_host::PopupPlacingScope` (in `platform.h`, shared by all three backings)
  brackets our own geometry calls, so the dismiss-on-owner-moved hooks do not
  fire for the window being placed - which would otherwise dismiss the level
  below a cascade the moment a second level opens.
- Press-drag-release from the anchor into the popup is **not** supported (v1 is
  click-click everywhere; that gesture needs a capture on win32).

### win32

- `WS_POPUP` + `WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW`, owned via the hwndParent
  slot of `CreateWindowEx` - for `WS_POPUP` that slot is the OWNER, which is what
  buys z-order-follows-owner and hide-on-owner-minimize from the OS.
- `GetAncestor(frame_hwnd, GA_ROOT)`: an embedded plugin frame is a `WS_CHILD`
  inside the DAW's window, so the thing to sit above is that top-level window.
  Re-owning on re-open goes through `SetWindowLongPtrW(GWLP_HWNDPARENT)`.
- `SWP_NOACTIVATE` + `SW_SHOWNOACTIVATE`, never `SW_SHOW`.
- Borderless `WS_POPUP` means client rect == window rect, so unlike frame
  creation there is no `AdjustWindowRectExForDpi` round-trip on placement.
- No `SetCapture`. `WM_ACTIVATEAPP` in the WndProc is the whole watch, with the
  documented gap above; `WM_MOVE` / `WM_SIZE` drive owner-moved dismissal.
- **`WS_EX_NOACTIVATE` is not by itself enough, and this cost a defect.** That
  style only stops Windows from activating a window that is *clicked*; it does
  nothing about a programmatic `SetFocus`, and `SetFocus` on a top-level window
  activates it as well. `XplWndProc`'s `WM_CREATE` gives every non-`WS_CHILD`
  window the keyboard focus (so `WM_KEYDOWN` / `WM_CHAR` arrive), which meant a
  popup surface took the foreground, the activation *and* the thread's focus away
  from the editor the instant it opened - precisely the "the editor lost focus"
  bug report the non-activating style exists to prevent. `WM_CREATE` now skips
  the focus grab for a `WS_EX_NOACTIVATE` window. It tests the ex-style rather
  than the widget type so any later non-activating window kind inherits it, and
  the toast window is unaffected (it runs its own WndProc). The lesson
  generalizes: on win32 the *style bits are not the promise*, so
  `tests/popup_surface_smoke_win32.cpp` asserts the observable
  `GetForegroundWindow` / `GetActiveWindow` / `GetFocus` around a real open and a
  real click, not the styles alone.

### Linux (X11)

- Built on `create_frame(borderless)`, then `override_redirect` + a
  `_NET_WM_WINDOW_TYPE_POPUP_MENU` hint. Override-redirect is the load-bearing
  one: it takes the WM out of decoration, focus and stacking entirely, which is
  how every X11 menu is built.
- `XGrabPointer(owner_events=True, GrabModeAsync)` on show, released when the
  stack empties. `platform_show_popup_surface` ends with `XSync` rather than
  `XFlush` because the grab that follows fails with `GrabNotViewable` if the map
  has not landed yet.
- `create_frame` folds the frame zoom into the create-time position, which is
  wrong for a screen coordinate - harmless only because the window is created
  unmapped and re-placed before the map.
- `_NET_WORKAREA` for the work area (falls back to the whole screen); a
  per-monitor answer wants RandR and is deferred. The fallback is not exotic:
  WSLg's Weston publishes no `_NET_WORKAREA` at all.
- **X11's PSEUDO focus events are not deactivations, and this cost a defect.**
  X sends a `FocusOut` / `FocusIn` pair to the focused window around any
  keyboard grab taken by *another* client - a window-manager keybinding, a
  screen locker, another application opening its own menu - with
  `mode == NotifyGrab` / `NotifyUngrab`. It also sends events whose `detail` is
  `NotifyPointer` (the window is merely being tracked by the pointer) or
  `NotifyInferior` (focus moved to a *descendant*, so it is still ours). Nobody
  switched applications in any of those cases. `dispatch_x_event`'s `FocusOut`
  branch acted on all of them, so an idle popup surface vanished a few seconds
  after opening - reported as `DEACTIVATED` - whenever anything on the desktop
  briefly grabbed the keyboard. `is_real_focus_change` now gates both `FocusIn`
  and `FocusOut` on `mode ∈ {NotifyNormal, NotifyWhileGrabbed}` and
  `detail ∉ {NotifyPointer, NotifyInferior}`. The bug predates popup surfaces:
  the same branch closes menubar menus and tree popups and clears `_os_focused`
  (the focus ring), so those were spuriously dropped too - popup surfaces are
  simply the first thing that stays on screen long enough to notice.

## Per-platform status

| | desktop backing | in-frame backing | notes |
|---|---|---|---|
| macOS (xpl) | **yes** | — | verified: `tests/popup_surface_smoke_macos.mm` + an interactive pass |
| win32 (xpl) | **yes** | not yet | verified 2026-08-14: `tests/popup_surface_smoke_win32.cpp` (one defect found, below) |
| Linux (xpl) | **yes** | not yet | verified 2026-08-14: `tests/popup_surface_smoke_linux.cpp` (one defect found, below) |
| iOS / null | never | not yet | an AUv3 view has no top-level window to escape into |
| native win32 / macOS hosts | n/a | n/a | `NEUI_API_POPUP` is xpl-only, like `_EMBED` / `_TIMER` / `_POINTER` / `_A11Y` |

**Linux has now been built and run too** (2026-08-14, X.Org 24.1 under WSLg /
Weston XWM). Like win32 it compiled warning-clean on the first attempt, and both
of its check-first items came back clean:

- the `XGrabPointer` **does** succeed after the map - asserted from a second X
  connection, which is refused with `AlreadyGrabbed` while a popup is up and
  gets `GrabSuccess` again once the stack closes. The `XSync` at the end of
  `platform_show_popup_surface` is doing its job.
- an `override_redirect` window **does not** steal the focus: the owner keeps
  the input focus across the open, and the popup's window is never the focus
  window.

Placement, sizing, the window-type hint, the cascade, deepest-first unwind,
owner-moved dismissal, the input gate and the destroy path all held unchanged.
But like win32 it shipped **one real defect**, and again it was in the half no
amount of care on macOS could have caught: X11's pseudo-focus events were read as
deactivations, so an idle popup dismissed itself a few seconds after opening. See
the Linux notes above.

**win32 was in the same position and has now been built and run** (2026-08-14).
It compiled warning-clean at `/W4` on the first attempt and every placement,
ownership, cascade, dismissal and gate claim held, so the design port was sound -
but it shipped **one real defect**, and it was in the half no amount of care on
macOS could have caught: the pre-existing `WM_CREATE` focus grab defeated
`WS_EX_NOACTIVATE`, so opening a picker stole the editor's focus. See the win32
notes above. Two of the three check-first items are answered - the
`GWLP_HWNDPARENT` re-own works (asserted against a second frame), and
`WS_EX_NOACTIVATE` keeps the editor's focus *now*. The third, how bad the
`WM_ACTIVATEAPP` gap feels in a real DAW, still needs a plugin in a real host.

Where `platform_supports_popup_surface()` is false, `open()` fails rather than
half-working, and `get_clamp_size` reports the owner's client area. That is a
temporary implementation state, not the design: the type is meant to exist
everywhere with the in-frame backing behind it.

## Testing

`tests/popup_surface_smoke_macos.mm` — realizes real windows and asks **AppKit**
rather than neui. Built but not ctest-registered; run
`./tests/<config>/neui_popup_surface_smoke_macos`. Phases: placement (the rect
genuinely leaves the owner), ownership (`-childWindows`), activation (never key),
clamping + flip, cascade (flat ownership, deepest-first unwind), lifetime (owner
destroy leaves no window), and the input gate driven through `Session` directly
(synthetic HID events would need an unlocked screen and Accessibility
permission).

`tests/popup_surface_smoke_win32.cpp` — the counterpart, and **not** a port. The
portable half is proven once on macOS and is the same code here, so this targets
what win32 writes for itself and asks **win32** rather than neui: that the popup
is a real top-level `WS_POPUP` (not a child HWND) whose rect leaves the owner,
that `GetWindow(GW_OWNER)` is the owner's `GA_ROOT` (win32's `-childWindows`, and
what buys z-order-follows-owner from the OS), that re-opening against a different
frame really moves ownership through `GWLP_HWNDPARENT` — the one call with no
macOS counterpart, since `addChildWindow:` is idempotent and this is a live
re-parent — and that opening **and clicking** the popup moves neither the
foreground window nor the thread's focus. Plus the three WndProc branches only
win32 has: `WM_ACTIVATEAPP` as the whole outside-press watch (driven directly,
both polarities — `TRUE` must *not* dismiss), `WM_MOVE` owner-dismissal, and the
`PopupPlacingScope` that keeps placing a cascade level from reading as "the owner
moved". One phase drives a real outside press through the WndProc and asserts the
widget underneath saw **no** click, which is the swallow proven end-to-end rather
than at the gate. Built but not ctest-registered; run
`tests/<config>/neui_popup_surface_smoke_win32.exe`.

`tests/popup_surface_smoke_linux.cpp` — the third counterpart, same rule: it asks
**X11** rather than neui, over a **second X connection**, which is what lets it
act as a different X client. That the popup is a mapped `override_redirect`
**direct child of the root window** (a window parented into the owner could not
leave it and would not appear there at all), sized exactly as asked, carrying
`_NET_WM_WINDOW_TYPE_POPUP_MENU`, with a rect that genuinely leaves the owner's;
that its top-left really is the anchor's bottom-left plus the offset in root
coordinates; the two check-first items (the pointer grab is held — the second
connection is refused with `AlreadyGrabbed` — and the input focus never moves off
the owner); that another client's **keyboard grab does not dismiss the stack**
while a real focus change still does, which is the regression guard for the
defect above; the cascade, its deepest-first unwind, and owner-moved dismissal;
the gate, including the X11-only `popup_gate_press_outside` route that exists
because `owner_events=True` reports an outside press against the grab window; and
that destroying the owner leaves neither a window nor the **pointer grab** behind
— a stranded grab on X11 means a desktop that no longer responds to the mouse.
Needs a live display, so built but not ctest-registered; run
`./tests/neui_popup_surface_smoke_linux`.
