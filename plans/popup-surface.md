# Popup surfaces — what is still open

Response to [#23](https://github.com/defiantnerd/neui/issues/23). **The feature
shipped** (2026-08-14): `NEUI_W_POPUPSURFACE` + `NEUI_API_POPUP`
(`include/neui/d/popup.h`), the frame-kind plumbing, the session popup stack and
input gate, placement / flip / work-area clamp, and the desktop backing on all
three desktop platforms — macOS, win32 and Linux/X11, each verified by a harness
that asks the **platform** rather than neui, and each of the two ports found
exactly one platform-specific defect on its first real run.

The design rationale, the per-platform mechanisms and the full status table moved
to **`docs/popup-surfaces.md`**; the client-visible limitations are in
`docs/deferred-issues.md`. This file is only the remaining work, in the order it
matters. The original design note — including the reasoning against the two
narrower options in the issue and the five open questions put to baconpaul — is in
git history (`plans/popup-surface.md` at `7ffe9da`); its answers are folded into
the two docs above, and the answers themselves did not change during
implementation.

## 1. The in-frame backing

The only piece of the shipped *design* that is not shipped **code**.
`platform_supports_popup_surface()` returns false on iOS and null, so `open()`
fails honestly there and `get_clamp_size` reports the owner's client area. Until
the fallback exists, "one type, two backings" is a design commitment rather than a
fact, and a client that follows the recommended idiom still cannot run its popup
UI on a platform without top-level windows.

What it needs, all of it portable (no new platform seams):

- a `PopupSurfaceWidget` with **no** `native_handle`, composited by the owner's
  `paint_frame` at the placed offset and clipped to the owner's client rect;
- hit-testing that consults the popup stack **before** the owner's own subtree, so
  a press inside the overlay reaches the popup's children rather than what is
  underneath — the desktop backing gets this from the OS for free;
- placement clamped to the owner's client rect instead of the work area (which
  `get_clamp_size` already reports there, so the client's layout already matches);
- the input gate unchanged — it was written session-side precisely so this backing
  inherits it.

Worth doing on iOS specifically: an AUv3 view has no window to escape into, so
in-frame is not a degradation there, it is the only shape available.

## 2. Keyboard beyond Escape — **shipped, except type-ahead**

Response to [#25](https://github.com/defiantnerd/neui/issues/25). Shipped
2026-08-16: the key gate is a **retarget** rather than a pass-through
(`Session::popup_gate_key` / `popup_diverts_keys`), and `focus_next` overrides its
frame hint with the deepest open level. Mechanisms in `docs/popup-surfaces.md`,
client contract in `<neui/d/popup.h>`.

Two things that wave got right and one it got wrong. Right: the routing really was
bookkeeping, and the *hard* half named in #25 — a focusable `INPUTBOX` inside a
non-activating window — turned out to need no work at all. `_focused_widget` is a
**session** index and every platform's key dispatch reads it without a frame
check, so a click already focused the widget inside the popup and typing already
landed there; verified end-to-end on macOS before anything was written. Wrong: the
plan framed the gap as *missing navigation*, when the fall-through was an active
**input leak** — with a popup open, an arrow key operated the last-focused control
in the editor and typing edited the text field behind it. The mouse had had that
promise since v1 (`popup_gate_press` swallows); the keyboard simply never got it.

The cost the plan understated is that the CHARACTER paths are not unified: win32
`WM_CHAR`, AppKit `interpretKeyEvents:` → `insertText:` and X11's
`Xutf8LookupString` tail are three different shapes and none of them passes the
keydown gate, so the leak had to be closed three times — plus IME composition,
which is not a keystroke at all and needs the predicate directly.

**Host-side navigation followed** (same day), once the question "navigate
*what*?" had an answer. The host cannot see inside a `NEUI_W_CUSTOMDRAW`, so the
client DECLARES a list instead — `NEUI_ATTR_NAV_COUNT` / `_INDEX` / `_PAGE` /
`_WRAP` — and the host owns arrows / Home / End / Page / Enter over the
declaration while the client keeps painting from the index. That is the smallest
thing that serves a client-drawn menu without the host pretending to understand
the content. The override needed no new API: the client gets each key first and
`true` means it handled it, the same two-tier contract as mouse events, which
also makes the opt-out per keystroke rather than per popup. Arithmetic in
`hosts/shared/popup_nav.h`, Tier-1 tested; the live half (attrs, events,
override) is asserted in the macOS harness, since none of it is platform-specific.

**Escape always belongs to the host — decided, not deferred** (2026-08-16). It
closes the stack before the client sees it: no `KEYDOWN` is dispatched for it, no
opt-out attribute exists, and focus inside the popup does not change it. The cost
is real and accepted — a popup carrying an editable field cannot use Escape to
cancel the edit first, and must use a different key or an on-screen Cancel. The
reason is the rule already written into `<neui/d/popup.h>`: a dismissal cannot be
vetoed, because a popup that can refuse to close is a window stuck over someone's
DAW. A client that swallowed Escape by mistake would produce exactly that, and no
amount of opt-in ceremony makes that failure less bad than the inconvenience it
buys. Asserted in the macOS harness both ways (client claiming keys, and focus
inside the surface).

Still open:

- **Type-ahead.** The declaration carries a count and no item TEXT, so there is
  nothing to match a prefix against. It needs a per-row names channel — the
  `NEUI_API_*_CLIENT` pattern (`_MENU_CLIENT` / `_GRID_CLIENT`) is the shape:
  `item_text(session, surface, index, buf, buflen)`, called only while a
  type-ahead buffer is live. Deliberately not guessed at here; the a11y
  declarations are per-WIDGET and so cannot supply per-row names.
- **The built-ins still have their own walk.** `popup_tree_menu` navigates
  through `handle_menubar_key` over `_menu_path`; a surface navigates through the
  declared list. Re-pointing the built-ins onto the primitive (§4) is where those
  two converge, and combo drop-lists remain the natural first candidate.
- **Getting focus in.** Nothing focuses a popup when it opens and no key moves
  focus into one; Tab only cycles once focus is already inside. Whether `open()`
  should auto-focus the first tab stop wants a flag rather than a default — a
  menu wants it, a tooltip does not.
- **The embedded case**, which was always the genuinely hard part: the DAW owns
  keyboard focus and plugin key handling is host-dependent. Unverified in a real
  host.
- **Verified on macOS only.** The gate decision is asserted in all three
  harnesses, but the end-to-end path (real click → focus in popup → real key at
  the owner → text in the popup's field) has only been run on macOS.

## 3. Accessibility

A window that never takes focus is close to invisible to a screen reader unless it
is published into the owner's tree. `NEUI_API_A11Y` already has the three-layer
shape (`plans/accessibility.md`) and a popup surface is a frame, so the adapter
has something to walk — the open question is whether it is published as a child of
the owner frame or as its own root, and macOS and UIA answer that differently.

## 4. Re-pointing the built-ins

`popup_tree_menu`, combo drop-lists and tooltips keep their current in-frame
behaviour and their current frame-sized ceiling. Each can move onto the primitive
independently, as a visible behaviour change decided per widget. **Combo
drop-lists are the natural first candidate**: same shape, same ceiling, and
`ComboBoxWidget::overlay_rect` already owns the flip that
`Session::open_popup_surface` now does against the work area.

## 5. Two things only a real DAW can answer

Neither blocks anything; both are best-effort by design and are documented as
such.

- **macOS `addChildWindow:` against a host-owned `NSWindow`.** Verified only with
  an owner we created. A DAW may fight it; the fallback is a window level plus
  manual follow.
- **The win32 `WM_ACTIVATEAPP` gap.** A press in the DAW's own UI outside our
  child HWND raises no activation change, so the popup stays up until the next
  click in the editor. The escape hatch, if a real host makes it intolerable, is a
  thread-level `WH_MOUSE` hook — cheaper and better-scoped than `WH_MOUSE_LL`, but
  still a hook inside a host process, which is why v1 accepts the gap instead.
