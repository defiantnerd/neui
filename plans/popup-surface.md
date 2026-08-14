# Popup surfaces that can leave the frame — design note

Response to [#23](https://github.com/defiantnerd/neui/issues/23). Decisions are the
project owner's (2026-08-13); everything marked *proposed* is still open, and this
note exists to be argued with before anything is built.

## Implementation status (2026-08-13)

**macOS / xpl host: landed.** `NEUI_W_POPUPSURFACE` + `NEUI_API_POPUP`
(`include/neui/d/popup.h`), the frame-kind plumbing, the session popup stack and
input gate, the placement / flip / work-area clamp, and the macOS desktop backing
(non-activating `NSPanel`, `addChildWindow:` ownership, `NSEvent` local monitor +
`NSApplicationDidResignActive`). Verified by
`tests/popup_surface_smoke_macos.mm` — which asks AppKit rather than neui: the
panel's rect genuinely leaves the owner, it is in the owner's `-childWindows`, it
cannot become key, placement stays in the work area and flips near the bottom
edge, a cascade is flat-owned and unwinds deepest-first, and destroying the owner
leaves no window behind. Mutation-verified (dropping the swallow, and dropping
`addChildWindow:`, each fail the phase that covers them).
`examples/popup_surface_example.cpp` is the interactive counterpart.

**Not written yet**, in the order they matter:

1. **The in-frame backing.** `platform_supports_popup_surface()` returns false on
   win32 / Linux / iOS / null, so `open()` fails honestly there instead of
   half-working, and `get_clamp_size` already reports the owner's client area.
   Until the fallback exists the "one type, two backings" promise is a design
   commitment rather than a shipped fact.
2. **win32 and X11 desktop backings.** The seams are declared and stubbed with
   the per-platform notes (`WS_EX_NOACTIVATE` owned by `GetAncestor(.., GA_ROOT)`;
   override-redirect + `XGrabPointer(owner_events=True)`), and win32's
   `platform_get_work_area` / `_client_to_screen` / `_get_pointer_pos` are already
   implemented.
3. **Keyboard beyond Escape**, accessibility, and re-pointing the built-ins — the
   three deferrals below, unchanged.

## The decision

**One widget type, backed two ways.** A new `NEUI_W_POPUPSURFACE` exists on *every*
host. Where the platform allows it, the host backs it with an owned, non-activating,
borderless top-level window. Where the platform cannot, the host backs it with the
in-frame overlay machinery that already draws menus today. The client authors the
content once — children in popup-local coordinates — and it paints identically under
either backing.

Keep the name **surface**, not window. Half the hosts will not have a window behind
it, and the name is the first place the two-backing model either reads honestly or
does not.

## It is a frame kind, not a child widget

The structural claim that makes the rest cheap: on the xpl host a popup surface is a
third **frame kind** alongside APPWINDOW / PLUGWINDOW — a `FrameWidget` subclass
(`host.h:342`) that owns its own `render_ctx`, its own coordinate origin, and is the
root of its own child subtree. The two backings are then just whether that frame has
a `native_handle`:

- **Desktop backing** — the frame has a `native_handle` from a new
  `platform_create_popupsurface` seam, alongside the existing
  `platform_create_appwindow` / `_plugwindow` / `_dialog` (`platform.h:42/53/92`).
- **In-frame backing** — the frame has *no* `native_handle`, and the owner's
  `paint_frame` composites the popup's subtree into the owner's surface at an offset,
  clipped to the owner's client rect.

Both cases then run the **same widget paint walk** and the **same hit-test walk**. A
`GRID`, an `INPUTBOX` or a scrolling `SECTION` inside a popup works identically under
either backing, for free, because nothing in the popup knows which one it got.

That is a materially stronger position than today's popup code. `paint_popup_menu` /
`paint_tree_popup` are a bespoke paint pass with their own layout and hit-testing,
and that path can never host a real widget — which is exactly why the client
workaround in #23 had to hand-roll menu rows inside `CUSTOMDRAW`.

## Why not the two narrower options in the issue

**An opt-in attr on `NEUI_W_POPUPMENU`** solves menus and leaves the identical
ceiling on combo drop-lists, tooltips and preset browsers, which have the same
shape and the same problem.

**Owner-only on `PLUGWINDOW`** is the smallest diff and would unblock the existing
client workaround without a rewrite — but `widget_set_owner` currently rejects
anything that is not a DIALOG (`host.cpp:926`: `if (!dwd.is_dialog()) return;`), so
the relationship genuinely cannot be expressed today, and lifting that gate puts new
code in the path of the single most load-bearing widget for plugin use. Worse, on a
host that cannot honour it (below) `set_owner` would silently do nothing: the same
type quietly behaving differently per platform, which is the failure mode most
expensive to debug from the client side. A distinct type makes the capability
visible instead.

The repo's own precedent decides it: `MULTILINE` is a separate *type* because
single- vs multi-line was the wrong thing to toggle at runtime, while `CHECKBOX3`
stayed attribute-driven because tri-state genuinely is a live property. A popup
surface has a different lifetime, focus story and coordinate story from a
`PLUGWINDOW`. It is a `MULTILINE`, not a `CHECKBOX3`.

## Why "backed two ways" rather than "absent where unsupported"

Whether a popup may exceed its frame is not a platform quirk to be papered over,
but it is also not something a client should have to write two UIs for:

| | may exceed the frame | why |
|---|---|---|
| win32 / macOS / X11 | yes | owned top-level windows exist |
| iOS | no | an AUv3 view lives inside the host app's hierarchy |
| WASM / canvas | no | a canvas cannot paint outside the page |
| LVGL | no | a single framebuffer |
| null | no | nothing to show |

Even on the first row it is **best-effort**: a DAW running plugin UIs
out-of-process or sandboxed may not z-order a plugin's desktop window against its
own, and full-screen exclusive modes break it outright.

So two placements exist permanently. One type with two backings keeps one content
path in the client and one layout path in the host; a type that vanishes forces the
client to fork its UI, and an absent type on iOS would also make the same client
code fail to build a menu at all.

**The client still needs one fact, at design time**: not "is this a desktop
window", but **"what box will I be clamped to"**. That changes content, not just
placement — a 1030×970 FX selector that must live in-frame is not a smaller grid,
it is a scrolling or paged browser. *Proposed*: a single query returning the clamp
rect the popup will get, so a client lays out against a box rather than a boolean.
This is the second feature to want the capability query recorded in
`docs/deferred-issues.md` (zoom was the first); this proposes one predicate, not a
general capability system.

**Pair the query with a documented degradation idiom**: put popup content in a
scrolling `SECTION`. Then Surge's FX grid is a 1030×970 popup on the desktop and the
*same client code* is that grid scrolling inside a 700×500 box on LVGL — different
affordance, no second UI. Without this the clamp-rect query only tells a client that
it has a problem; with it, the query tells the client which affordance to pick and
the fallback already works. This is what makes option 2 pay off over option 1.

**The clamp rect must never lie**, and it must not be asked to cover placement
failures. If a host reports the monitor work area and a sandboxing DAW then z-orders
the popup underneath its own window anyway, that is a *placement* failure, not a
clamp failure. Different concept, different documentation, do not blur them.

## Clamping does not go away — it moves

Today popup layout clamps to the frame. With a desktop backing it clamps to the
**monitor work area**. Note the consequence: a large menu can still be clamped, on
a 1080p display with a DAW docked at the bottom. Desktop placement buys a much
bigger box, not an unbounded one.

The layout engine is already shaped for this: `mb_build_columns(..., int frame_w,
int frame_h, ...)` takes its clamp bounds as parameters rather than hardcoding the
frame, and it owns the cascade, the submenu left-flip and the edge clamping. What is
missing is the box to hand it, hence:

**Add a `platform_get_work_area` seam** (`MonitorFromWindow` + `GetMonitorInfo` /
`NSScreen.visibleFrame` / `_NET_WORKAREA`). It must return a **rect with an
origin**, not a width/height pair: a popup anchored near the right edge of a second
monitor clamps to *that* monitor's work area, where the frame case gets away with an
origin-anchored box. Nothing in the xpl platform layer knows where a screen ends
today. This is worth adding on its own merits — `ComboBoxWidget::overlay_rect`
(`host.cpp:4505`) has the same latent bug once a frame sits near a screen edge.

## Size, not just position

A popup larger than the editor gets **its own render target at its own size**. On
the Cairo software backend a 1030×970 popup is a ~4 MB ARGB buffer, and a
three-level cascade is three of them, allocated on a gesture. On constrained targets
that is a second, independent reason the desktop backing should be off — not merely
that they cannot, but that they should not. The in-frame backing needs no extra
surface at all, which is the third: it is the cheap path, not an emulation of the
expensive one.

## Ownership: the owner is usually the DAW's window

"Owned by the frame" is right for a standalone app and quietly wrong for the case
that motivates the issue. An embedded plugin frame has no top-level window of its
own — it is a `WS_CHILD` HWND or an `NEUIView` subview inside the **DAW's** window.
So the owner a popup must be attached to is the root ancestor, which we do not own:

- **win32** — owner for `CreateWindowEx` is `GetAncestor(frame_hwnd, GA_ROOT)`, the
  DAW's editor window. `WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW` plus that owner gets
  z-order-follows-owner and hide-on-owner-minimize from the OS.
- **macOS** — `NSPanel` with `nonactivatingPanel`, `canBecomeKeyWindow = NO`.
  `addChildWindow:ordered:NSWindowAbove` on the DAW's `NSWindow` is what gives
  owner-follows-z-order, but that mutates a window we do not own and some hosts will
  fight it. Fallback is a window level plus manual follow — worse, survivable.
- **X11** — override-redirect plus `_NET_WM_WINDOW_TYPE_POPUP_MENU`. The WM is not
  involved, so ownership is largely moot.

**Cascade ownership is flat**: every level is owned by the root ancestor, not by the
level above it. Deep owner chains buy nothing on win32, are moot under
override-redirect, and on macOS a chain of child windows is only more ways for a DAW
to break it. Z-order *among* the levels is the host's own business, driven by the
popup stack it has to keep anyway.

**Out-of-process cannot be made to work** — AUv3, VST3 with a separate UI process,
sandboxed hosts. Document it as a limitation rather than fighting it.

## Asynchronous only — no nested pump

`open_popup_menu` today runs a nested message loop (`_popup_running` gates it,
`host.h:1556`). A plugin cannot nest a pump inside the DAW's pump, and a popup whose
lifetime spans a nested loop is precisely the hazard class already fixed once in both
native hosts (the modal-pump use-after-free). `show_tree_popup`'s model — the pick
arrives later as one `ITEM_SELECTED` — is the correct shape, and it is the **only**
shape the new type gets. The synchronous `popup_tree_menu` variant must not be
extended to it.

## Input: a session gate, not an OS capture

Per-level OS windows already deliver the common cases with no capture and no grab:

- pointer over a level → that window's own mouse messages, so hover, row highlight
  and open-submenu-on-hover are all local;
- pointer crossing level 1 → level 2 → each window sees its own enter/leave, no
  forwarding and no central hit-test dispatcher;
- click inside a level → that window handles it.

What capture was actually buying is not delivery but **suppression**: while a popup
is open the editor underneath must stop behaving like a live UI. Without it, moving
off the popup and across the editor lights up hover on every button on the way,
which is the one thing that reads as "this is not a menu".

**So do the suppression in the session, not the OS.** While the popup stack is
non-empty, the session gates dispatch to non-popup widgets: swallow hover / enter /
leave, and convert the first press outside the stack into a dismissal. Three
properties fall out:

- **It is portable.** Identical for the in-frame backing, which has no OS window to
  grab in the first place. One code path, no platform code in it.
- **It is not `set_owner`'s modality.** The owner window stays *enabled* — no
  `platform_set_window_enabled` (`platform.h:104`), the click physically reaches us,
  and the session decides that it means "dismiss". That is exactly the distinction
  #23 could not express from the client side.
- **Partial dismissal is expressible.** A press landing on level 1 while levels 2–3
  are open pops the stack to depth 1 rather than closing everything.

This is a generalization, not new machinery: `handle_popup_click` (`host.cpp:4761`)
already documents "when a popup is up, all clicks are popup-owned (clicks outside
dismiss)", and `_popup_active` / `_menu_open` / `_menu_path` are an embryonic stack
already. The work is turning those into one stack that both backings share.

### Outside-press detection, per platform

The gate handles anything our own message loop sees. What is left is presses that
never reach us, and the platforms are **not** symmetric here — this is where
`SetCapture` is the wrong reach:

| | mechanism | coverage |
|---|---|---|
| macOS | `addLocalMonitorForEventsMatchingMask` (in-process → the DAW's own views) + `addGlobalMonitorForEventsMatchingMask` (other apps) + owner `resignKey` | **complete**, no grab; mouse masks need no Accessibility permission (only keyboard does) |
| X11 | `XGrabPointer` with `owner_events=True, confine_to=None` | **complete** |
| win32 | our own windows' handlers + `WM_ACTIVATEAPP(FALSE)` | **gap**: a press in the DAW's own UI outside our child HWND is never seen |

The X11 row deserves separating from win32: a grab with `owner_events=True` *is* the
droplist / context-menu mechanism. Events over our own windows are delivered
normally, so hover keeps working, and only events outside our client come to the grab
window. GTK and Qt menus both work this way. The behaviour asymmetry to avoid is
specific to `SetCapture`, which redirects everything to one HWND and has no
owner-events concept. "No capture" must not be read as "no grab on X11".

The win32 gap is real. Because we never take activation, the DAW's window is already
active, so a press on its transport raises no activation event and produces no
message for us. The only fixes are a hook or nothing: a **thread-level `WH_MOUSE`**
hook would see it, since our editor HWND lives on the DAW's GUI thread and so do the
DAW's other windows — cheaper and better-scoped than `WH_MOUSE_LL`, but still a hook
installed inside a host process.

*Proposed*: **accept the gap for v1** and document it. The failure mode is a menu
that stays open while the user pokes the transport, dismissed by their next click in
the editor — the same best-effort class as z-order under a sandboxing host, annoying
and self-correcting. Leave the hook as an opt-in escape hatch if a real DAW makes it
intolerable. Note the shape of this: macOS is the easy platform for once, and win32
is the one carrying the compromise.

### Three consequences of no capture

- **Enter/leave must be handled properly.** The in-frame overlay never needed it —
  the pointer could not leave the surface. With real windows, a level the pointer has
  left must clear its hovered row: `TrackMouseEvent` / `WM_MOUSELEAVE` (already
  wired for frames at `platform_win32.cpp:806`), `NSTrackingArea`, X11
  `LeaveNotify`. And "keep the parent row highlighted while the pointer is in the
  submenu" has to be driven by the shared stack state, not by either window's own
  hover — per-window hover will say the parent is not hovered, and it is right.
- **Press-drag-release across the window boundary will not work on win32.** Press on
  the anchor, drag into the list, release to pick is a capture-requiring gesture
  there; it works on X11 via the grab and on macOS via the tracking loop.
  Click-click works everywhere. *Proposed*: spec v1 as click-click only and say so,
  rather than shipping a gesture that silently works on two platforms of three.
- **The dismissing press is swallowed, not passed through.** Every OS menu and
  droplist does this, and pass-through means one click both closes a menu and mutates
  a parameter, which is bad in a plugin. Report the dismissal to the client as an
  event so it can drop its own state; keep click-through out of v1 (an attr later if
  anyone wants it).

## Dismissal triggers

Host-owned, all of them:

- press outside the stack (per the matrix above) → dismiss, or pop to the level hit;
- owner deactivated / another application activated;
- **owner moved or resized** → dismiss. That is what OS menus do; following the owner
  is not worth the machinery;
- owner destroyed, **including the DAW tearing the editor down**. On macOS that can
  be a bare `removeFromSuperview` with no window notification, so the popup must be
  reachable from the frame's teardown path, not only from window callbacks.

## v1 scope: mouse-only

**In:** the type; both backings; the frame-kind plumbing; flat owner-follows z-order;
non-activating windows; placement; the session-level popup-stack input gate;
per-platform outside-press detection (with the win32 gap documented); the dismissal
triggers above; the work-area seam; the clamp-rect query; the scrolling-content
degradation idiom; `UI_SCALE` inherited from the owner (or a 100 % menu appears over
a 150 % editor).

**Out, deliberately, and each is its own follow-up:**

- **Keyboard.** Arrows, type-ahead and Escape need key focus while the popup must
  *not* take activation, so keys arrive at the owner's window and have to be routed
  into the popup. Once the popup is a frame in the same session with a stack, that
  routing is mostly bookkeeping — the genuinely hard part is only the embedded case,
  where the DAW owns keyboard focus and plugin key handling is already host-dependent
  and unreliable. That is a plugin-world problem, not a neui one, which is why this
  is a wave rather than a blocker.
- **Accessibility.** A window that never takes focus is close to invisible to a
  screen reader unless it is published into the owner's tree. Wave 6 already records
  that menu items decline activation for a related reason.
- **Re-pointing the built-ins.** `popup_tree_menu`, combo drop-lists and tooltips
  keep their current in-frame behaviour. Once the primitive exists they can be moved
  onto it internally, per widget, as a visible behaviour change decided separately.
  Combo drop-lists are the natural first candidate: same shape, same ceiling, and
  `overlay_rect` already owns the flip.

## Prototype these first

Not the type — the two things that can invalidate the design:

1. **A non-activating owned window over a real DAW editor**, win32 and macOS:
   z-order follows, focus is never stolen, a click in the editor dismisses. Two open
   risks live here — whether macOS `addChildWindow:` on a host-owned `NSWindow`
   misbehaves in real DAWs, and how bad the win32 un-dismissed-menu gap actually
   feels in two or three hosts.
2. **`platform_get_work_area` returning an origin'd rect.** Small, testable, and
   useful whether or not the rest lands — it also fixes the latent combo-box flip bug
   near a screen edge.

## Open questions for baconpaul

1. **Placement coordinates.** The issue asks for screen coordinates. Our contract is
   logical px at 96 DPI, parent-relative, and `UI_SCALE` is built on that; screen
   placement introduces a new coordinate space and multi-monitor DPI makes it worse.
   *Proposed*: anchor to a widget plus a logical offset (what `popup_tree_menu`
   already takes), **plus an explicit at-pointer mode and a preferred-side/flip hint**
   — a droplist wants "below the bar, left-aligned, flip above" and a context menu
   wants "at the pointer". With those two, no screen-coordinate space is needed
   anywhere in the public API. Does that cover the real cases?
2. **Dismissal semantics.** *Proposed*: report every dismissal as an event (a client
   holding state needs the edge), and **no veto** — a vetoable dismissal is how you
   get an unclosable window floating over a DAW. Agreed?
3. **Cascade ownership.** *Proposed above*: flat, every level owned by the root
   ancestor. Anything you have seen fail in JUCE that argues for chaining instead?
4. **The clamp-rect query.** *Proposed*: rect only, no capability bit — a bit invites
   `if (desktop) { … } else { … }`, i.e. the two UIs this design exists to avoid.
5. **Does mouse-only v1 unblock the FX-selector case?** Our read is yes (that menu is
   mouse-driven in practice), but a *preset browser* without type-ahead will read as
   broken — so which of the two is the one you actually need first?
