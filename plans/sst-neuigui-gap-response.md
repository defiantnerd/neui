# Plan: response to the `sst-neuigui` gap analysis

**Status**: active. This document is the verified reply to
`baconpaul/neui-noodle/doc/sst-neuigui-gap-analysis.md` and the work plan
derived from it.

The analysis was read at neui `0077fe0`. Verified here at `7683adb`
(`bp-review`). Every claim below was re-checked against the tree; `file:line`
references are the evidence. Section numbers in **§** refer to the analysis;
**#N** refers to its §9 ask list.

## TL;DR

- **His #1 blocker is already closed.** §2.2 / cross-platform DAW embedding
  shipped in `edf5f9c` as `NEUI_API_EMBED` (`include/neui/d/embed.h`) on
  win32 / macOS / Linux, and the README now explicitly points plugin authors at
  the crossplatform host. He was reading a tree from before that landed.
- **One item is falsely marked done in our own history.** Commit `7683adb` is
  titled *"add user UI zoom (NEUI_ATTR_UI_SCALE)"* but has a **tree identical
  to `edf5f9c`** (`git diff edf5f9c 7683adb` is empty) and no `UI_SCALE` symbol
  exists anywhere in the repo. #9 is **not** implemented. Fix the history
  confusion before anything else, then implement it (Wave 3).
- **Choosing the xpl host resolves #11 and re-shapes #12.** His §5.4 (a DXGI
  swap chain per painted widget) is real — but only in the **win32 native**
  host. The xpl host already paints one frame through one context
  (`Session::paint_frame`, `hosts/crossplatform/host.cpp:2200`) and has no child
  HWND/NSView at all, which *is* the architecture he asks for in #11. Since
  plugins are already routed to xpl, **#11 is not on the plugin path** and we
  should not do it now. The flip side: on xpl the platform cannot hand him an
  accessibility tree either, so **#12 must be built inside the xpl host** — one
  implementation serving all three platforms, over a widget tree that already
  carries bounds, focus, tab order and values.
- **Everything in Tier 1 and Tier 2 is confirmed open and small.** Three of them
  are smaller than he thinks (#6 needs zero backend work), one is bigger (#4
  needs a new platform seam, not just exposure), and one of his premises is
  wrong in a way that changes the API design (#5 — see Corrections).
- Two items he did not raise belong in the same programme: **win32-native key
  modifiers are hardcoded to 0** (same bug family as #1), and **xpl `invalidate`
  repaints the whole frame**, which is the perf item his §5.4 was reaching for,
  translated to the host he will actually use.

---

## 1. Verified status of every ask

### Already shipped (since his read)

| Ask | State | Evidence |
|---|---|---|
| §2.2 / **#2-equivalent** — cross-platform embedding into a host-provided parent | **DONE** | `include/neui/d/embed.h` — `NEUI_API_EMBED` with `set_parent` / `event_fd` / `pump_and_tick`; parent travels as `void*` (HWND / `NSView*` / X11 `Window`). xpl host only, by design. |
| Plugin host guidance | **DONE** | `README.md:24-26` — prefer `neui_get_api("neui.host.crossplatform")` for VST3/CLAP/AU; native hosts are standalone-only. |
| Host automation begin/end edits (not in his list, plugin-critical) | **DONE** | `NEUI_EVENT_GESTURE_BEGIN` / `_END` with `{ widget, attr_key, value }`; wired on xpl, macOS-native, win32-native, iOS. Maps directly to VST3 `beginEdit`/`endEdit` and CLAP gestures. |

Known follow-ups on embedding are already tracked in
`docs/deferred-issues.md:19` (no auto-track of parent resize, no `WM_DPICHANGED`
for embedded win32 frames, click-to-focus only). None block him.

### Tier 1 — parity bugs

| # | Claim | Verified | Detail |
|---|---|---|---|
| 1 | `buttonmap` hardcoded `0` on win32-native `MOUSE_MOVE` | **CONFIRMED, and worse** | `hosts/win32/window.cpp:1343` MOUSE_MOVE `= 0`; `:1337` MOUSE_ENTER `= 0`; `:1352` MOUSE_LEAVE `= 0`; `:1385` **RBUTTONDOWN `= 0`** (should be `NEUI_MK_RBUTTON`). LBUTTONDOWN/DBLCLK pass a literal `1`, which is accidentally `NEUI_MK_LBUTTON`. xpl forwards the real `wParam` (`platform_win32.cpp:714,788,824`), so this is native-host-only. |
| 1b | **not in his list** — win32-native `KEYDOWN`/`KEYUP` `modifiers` hardcoded `0` | **CONFIRMED** | `hosts/win32/window.cpp:1399,1406` — `event.data.key = { wid, wParam, 0 }`. Same bug family; kills his 26 `KeyPress` + 13 `ModifierKeys` uses on the native host exactly as #1 kills drag detection. |
| 2 | No `NEUI_EVENT_RESIZE` from xpl on macOS | **CONFIRMED** | Fires from `platform_win32.cpp:588` and `platform_linux.cpp:1385`, and from macOS-**native** `hosts/macos/window.mm:2554` (`windowDidResize:`). Nothing in `platform_macos.mm`. Since plugins are told to use xpl, this sits **on the plugin path** — promote it above #1. |
| 3 | Wheel events carry no modifier bits | **CONFIRMED** | `include/neui/d/events.h` — `neui_event_wheel_t` is `{ widget, x, y, delta, is_horizontal }`. Already tracked at `docs/deferred-issues.md:17`. |

### Tier 2 — API additions

| # | Claim | Verified | Correction / detail |
|---|---|---|---|
| 4 | No public timer / idle | **CONFIRMED**, but **harder than "exposure"** | Nothing timer-shaped in `include/neui/`. Internally the tickers are **per-feature, not a general seam**: Linux has a general 16 ms `timerfd` (`platform_linux.cpp:291`), macOS/iOS use one `NSTimer` per feature (`platform_macos.mm:412,453,487` — grid bounce, section bounce, toast), win32 xpl uses distinct `SetTimer` ids (`XPL_TOAST_TIMER_ID`, `XPL_SECTION_BOUNCE_TIMER_ID`, `XPL_GRID_BOUNCE_TIMER_ID`). A public timer needs a **new `platform_timer_*` seam** plus per-platform impls, then the public API on top. Still small, but it is implementation, not just plumbing. |
| 5 | Text metrics / `draw_text` alignment | **CONFIRMED, premise wrong** | `draw_text` **already vertically centres** in the rect on every backend, from real font metrics: d2d `DWRITE_PARAGRAPH_ALIGNMENT_CENTER` (`backends/d2d/d2d_backend.cpp:578`), CG via `CTLineGetTypographicBounds` (`backends/cg/cg_backend.mm:427-432`), cairo via `cairo_font_extents` (`backends/cairo/cairo_backend.cpp:546-553`). So "you cannot vertically centre text correctly" is inverted — **centred is the only thing you can get**. The real gaps: (a) no way to select top / baseline / bottom or left / centre / right, (b) no metrics query. Both numbers already exist inside every backend → plumbing. This changes the API: it is an **alignment enum + a metrics struct**, not "add centring". |
| 6 | Rounded rect, ellipse, `draw_line` | **CONFIRMED, cheaper than stated** | Absent from **both** `include/neui/d/painter.h` and `include/neui/d/renderer.h`. His "the backends can already do rounded rects" is not right — no backend has a rounded-rect primitive; compound `corner_radius` is **emulated on the path API** by `build_rounded_rect_path()` (`hosts/shared/widget_paint_compound.h:36-44`, used at `:707,716`). Consequence: **all three ship as painter-level helpers over the existing path API — zero backend changes, one file.** Ellipse = arc inside a scale sandwich; line = 2-point path. |
| 6b | §3.5 no italic | **CONFIRMED** | `push_font(p, family, weight)` — family + weight only, no style axis (`painter.h:137`). |
| 6c | §3.6 no measured / wrapped multi-line at painter level | **CONFIRMED** | Backends split on `'\n'` only. The wrap algorithm does exist one layer up — MULTILINE's `NEUI_ATTR_LINE_WRAP` / `cached_line_starts()` — so it is liftable. |
| 7 | No cursor API | **CONFIRMED** | `hosts/crossplatform/platform.h:382-385` — `CursorKind` has exactly two values (`NEUI_CURSOR_DEFAULT`, `NEUI_CURSOR_EW_RESIZE`); `platform_set_cursor(int)` at `:387` is host-private. No hide-cursor, no warp / unbounded movement. `docs/deferred-issues.md:17` already blocks the behavior asset's `cursor` prop on this. |
| 8 | Popup menus too weak | **CONFIRMED** | `include/neui/d/widgets.h:88` — `popup_menu(sess, anchor, x, y, const char* const*)`, blocking, 1-based index, `"-"` = separator. Submenus / `set_checked` / shortcuts / `NEUI_API_MENU_CLIENT::validate` all exist but only for `MENUBAR`. |
| 9 | Per-frame client UI scale | **NOT IMPLEMENTED** (despite the commit title) | No `NEUI_ATTR_UI_SCALE` / `set_ui_scale` / `frame_zoom` anywhere. `NEUI_API_METRICS::ui_scale` exists and is documented process-global and read-only (`include/neui/d/metrics.h:54-71`). **Cheaper than his estimate on xpl**: xpl is one surface per frame and DPI enters at exactly two kinds of place — `backend->update_dpi` (D2D `SetDpi`, CG/cairo CTM) and the platform layer's input/window conversions. There is no 81-call-site `get_dpi_for_widget` fan-out to audit unless we also do the win32 **native** host. |
| 10 | No file dialog | **CONFIRMED** | `include/neui/d/notify.h` exposes only `toast` (`:70`) and `message_box` (`:94`). |

### Tier 3 — architectural

| # | Claim | Verified | Assessment |
|---|---|---|---|
| 11 | A DXGI swap chain per painted widget | **CONFIRMED for win32 native; MOOT for the plugin path** | `hosts/win32/window.cpp:259` creates one render context per painted widget → `d2d_build_hwnd_target` → `CreateSwapChainForHwnd` (`backends/d2d/d2d_backend.cpp:649`). His cost analysis is right. **But the xpl host already is the fix he proposes**: `Session::paint_frame` (`host.cpp:2200`) does one `begin_frame`/`end_frame` per *frame* over one context, walking the tree with per-widget transform + clip from cached `abs_x/abs_y` — and xpl has **no child native windows at all**, so there is nothing to make render-passive. Plugins are already on xpl. **Recommendation: do not do #11 now.** It becomes worth doing only if standalone *native*-host apps need dozens of painted widgets. |
| 12 | No accessibility seam | **CONFIRMED**, and **his §5.2 reasoning inverts on xpl** | No `WM_GETOBJECT`, no `IAccessible`/UIA, no `NSAccessibility` overrides, no client API (only unrelated `accessibilityDescription:` on two `NSImage`s). Tracked as "Tier B focus parity" at `docs/deferred-issues.md:8`. **On xpl there is one native view per frame**, so widget count buys nothing from the platform — the provider tree must be synthetic regardless of granularity. That is *better* for scoping: build it **once in the xpl host** and it serves win32 + macOS + Linux from one walk. The substrate is already in place — per-widget bounds (`abs_x/abs_y`), a `tab_stop` flag plus real traversal (`collect_tab_stops` / `focus_next`, `host.cpp:2283-2326`), hit-testing, enabled/visible, and `NEUI_PARAM_VALUE` on KNOB/SLIDER. Role/name/value/state/bounds/focus are all **derivable**; what is missing is the two platform provider shims and one client declaration API. |

### Additions not in his list

| Item | Why it belongs | Evidence |
|---|---|---|
| **xpl `invalidate` repaints the whole frame** | `w_invalidate` → `platform_invalidate(frame)` — no dirty rect. A 60 Hz VU meter repaints the entire editor every tick, at 2× DPI. This is the perf ceiling that actually applies to him, in place of #11. | `hosts/crossplatform/widgets.cpp:692-702` |
| **`metrics->measure_text` is a desktop estimate** | Documented as "best-effort … a font-metric-based average advance … not pixel-exact" on desktop. He will size labels outside paint constantly. Worth making exact while we are in the text code for #5. | `include/neui/d/metrics.h:82-87` |
| **No capability-query API** (`NEUI_API_CAPS`) | Interface-level feature detection works (`get_interface` → null), but there is no way to ask whether a host honours a given **attribute**. `NEUI_ATTR_UI_SCALE` is the case in hand: zoom is xpl-only *by design*, yet on a native host the client's `set_float` succeeds, `get_float` reads it back, and nothing zooms — silently. A client cannot grey out a zoom menu it cannot support. The host already knows internally (`platform_supports_ui_scale()`, `platform_menubar_in_frame()`, `NEUI_HAS_XI2`/`_DBUS`); none of it is reachable. Wanted: `bool supports(session, const char* feature_key)`. **Deferred to its own wave** — cross-cutting, and best designed once Waves 4–6 have added the features whose support actually varies. | `docs/deferred-issues.md` (No capability-query API) |
| **No layout-on-resize** | Deferred by design; examples deliberately omit it. Plugin editors resize, so the client owns relayout. Not a gap to close — a thing to **document** alongside his §5.1 `place()` discipline. | `CLAUDE.md` (Writing client code) |

---

## 2. What to tell him

Four things, in this order:

1. **Embedding is done** — `NEUI_API_EMBED`, all three platforms, plus
   `GESTURE_BEGIN`/`_END` for automation edits. His Phase 3 is unblocked
   ahead of his Phase 2.
2. **Target the xpl host, explicitly.** `neui_get_api("neui.host.crossplatform")`.
   That deletes #11 from his critical path entirely, makes zoom (#9) cheap,
   makes his §5.1 `set_pos` "flash" concern a non-issue (no HWNDs to move), and
   removes the win32-native `buttonmap`/modifier bugs (#1, #1b) from his path
   too — they are native-host-only. We will still fix them.
3. **Accessibility changes shape, not size.** On xpl the platform gives him
   nothing regardless of widget count, so #12 is neui's to build — and we would
   rather build it once in the xpl host than twice in the native hosts. His
   granularity recommendation still stands (widget per accessible element,
   decoration painted by the parent), and it is still the right shape for the
   provider walk. It just is not "free from the OS".
4. **#5's premise, corrected.** Vertical centring already works and is
   metric-correct. What he needs is explicit alignment control plus a metrics
   query — which is what we will ship, and it is a different signature than
   "add centring" would have been.

---

## 3. Work plan

Ordered by what unblocks his Phase 0b / Phase 1 soonest, then by leverage.
Waves 0-4 are independent of each other and can land in any order; 5 and 6 want
the earlier waves in place.

### Wave 0 — parity bugs (no design work) — **DONE**

Small, mechanical, and two of them are on the plugin path.

- **0.1 — xpl macOS `NEUI_EVENT_RESIZE`.** **Done.** Hooked on
  `NEUIView -setFrameSize:` (`platform_macos.mm`) rather than the window
  delegate: NEUIView is the content view of a standalone frame *and* the root
  subview of a DAW-embedded PLUGWINDOW, so one hook covers both — an embedded
  frame has no `NSWindow` of ours to take `windowDidResize:` from. Reports the
  height below `frame_top_inset` for parity with Linux (0 on macOS). Verified
  on both a programmatic `set_size` and an **AppKit-initiated** resize
  (`performZoom:`, which streams ~21 events through the zoom animation, the same
  shape a live drag produces), with neui's cached size agreeing with the view.
- **0.2 — win32-native mouse `buttonmap`.** **Done.** `ChildSubclassProc`
  (`hosts/win32/window.cpp`) now forwards masked `wParam` on MOVE / ENTER /
  L+R DOWN / UP / DBLCLK; WM_MOUSELEAVE carries no key state in its message, so
  it reads the live state instead (a drag leaving the widget still reports its
  held button).
- **0.3 — win32-native key `modifiers`.** **Done.** `WM_KEYDOWN` / `WM_KEYUP`
  populate `NEUI_KMOD_*` from the live key state.
- **0.4 — wheel modifiers.** **Done.** `neui_event_wheel_t` grew a `buttonmap`,
  populated on win32 native + xpl, macOS native + xpl, and Linux xpl (core +
  XI2). iOS synthesises the wheel from touch pans and reports 0, which is
  accurate there.
  - **Fine-on-wheel now ships too** (the thing this unblocked). It was wired,
    backed out after review found it incoherent, then re-landed once you ruled
    that **the Shift->horizontal flip belongs in the platform layer**. That
    ruling makes the consumer responsible for undoing it: a value handler has no
    horizontal axis, so `hosts/shared/behavior_runtime.h` un-negates an
    `is_horizontal + NEUI_MK_SHIFT` notch to recover the physical direction
    (a genuine tilt-wheel horizontal notch is untouched). The second blocker -
    fine starving against `steps` snapping - is solved by the native KNOB's own
    rule: with detents a notch advances exactly one detent and fine is ignored,
    so there is no sub-quantum change to round away. 6 Tier-1 cases, including
    the exact config that used to go dead.

**Four additional fixes the wave surfaced** (all real, all beyond the four
items — back them out if the waves should stay strictly scoped):

- **A shared `hosts/shared/win32/keys_win32.h`**, completing the set alongside
  `keys_macos.h` / `keys_linux.h`, so both win32 hosts share one mask
  translation. It exists because forwarding a raw `wParam` is actively unsafe:
  **`MK_XBUTTON1` is `0x0020`, which collides with `NEUI_MK_ALT`** — a 5-button
  mouse was making `behavior_runtime`'s Alt-fine modifier fire spuriously. The
  helper masks to the five documented bits. Applied to the xpl host too, where
  it fixed a second live bug: `MOUSE_MOVE` was masking to the three *button*
  bits only, so Shift-for-fine on a KNOB / SLIDER drag saw the modifier on the
  initial DOWN and then lost it for the rest of the drag.
- **`platform_set_window_pos` on macOS xpl sized the OUTER window** to the
  requested `(w, h)` instead of the client area, contradicting both `create()`
  (which passes the same rect to `initWithContentRect:`) and win32 (which
  maintains the client size via `AdjustWindowRectExForDpi`). `set_size(520, 300)`
  produced a **520x268** client. Now converted through
  `frameRectForContentRect:`, which also preserves create()'s position
  semantics. Found only because 0.1 made the reported size honest.
- **The macOS hosts never reported `NEUI_MK_SHIFT` / `_CONTROL` on mouse events
  at all** — the same defect class as item 1 (which was about win32), on the
  plugin path. macOS native hardcoded `buttonmap` to `0` / `1` /
  `NEUI_MK_LBUTTON` across ENTER / LEAVE / MOVE / DOWN / UP / RBUTTON
  (`hosts/macos/window.mm`), so behavior DRAG Shift-fine was dead there; macOS
  xpl left `BUTTON_UP` / `CLICK` / `RBUTTON_UP` at `0` while win32 xpl populated
  the same events. All now route through the existing `mac_buttonmap` helper.
  The two control-action CLICK paths (win32 `BN_CLICKED`, macOS `NSControl`
  action) deliberately stay `0` — the keyboard can raise them with no mouse
  involved — and now carry a comment saying so.
- **The xpl KNOB and SLIDER were reading the wheel delta as a modifier mask.**
  `host.cpp` tested `event->data.mouse.buttonmap & NEUI_MK_SHIFT` inside its
  `MOUSE_WHEEL` branch, but `neui_event_mouse_t::buttonmap` and
  `neui_event_wheel_t::delta` sit at the **same offset (12)** in the event union
  — verified with `offsetof`. So "fine" was `(delta & 0x4)`: a negative delta
  (wheel-down) sign-extends to `0xFFFFFFF...` and always read as fine, while
  wheel-up never did. Undiagnosable before this wave because the wheel payload
  had no `buttonmap` to read; now a one-word fix at both sites.

Verified: `cmake --build` clean and warning-free on macOS (Xcode/Debug);
`ctest` green — 317 cases, 1843 checks, 0 failures. The win32 and Linux edits
are **compile-unverified locally** (authored on macOS), matching the
established convention for this repo's cross-machine flow.

### Wave 1 — painter completeness — **DONE** (1.4 deferred)

- **1.1 — text metrics + alignment.** **Done.** The design question was where the
  numbers live, and the answer shaped the API: ascent/descent/line-height are
  properties of the **font at a size**, not of a string, so they became a
  separate `font_metrics(p, size, &a, &d, &lh)` rather than an extension of the
  string-level `measure_text` (whose "pen advance" meaning the grid caret math
  and compound centring already depend on).
  - Backend gets **one** new method. `line_height` is contractually *the
    per-line advance that backend's own `draw_text` uses* (CoreGraphics counts
    leading, cairo does not) — and that equivalence is the trick: because every
    backend derives the block position **from the rect it is handed**,
    `draw_text_aligned` needs **zero** backend work. Alignment is implemented in
    `hosts/shared/painter.h` by passing a tightened rect.
  - Overflow degrades deliberately: horizontal alignment keeps the rect's right
    edge (so over-wide text clips to the *widget*, not to the text box) and
    clamps so text never draws left of `x`; an over-tall block pins to the top.
  - No `BASELINE` mode — with a rect to align in it has no unambiguous meaning.
    `font_metrics` + `VALIGN_TOP` is the documented recipe.
  - Verified two ways: 22 Tier-1 cases with a mock backend recording the exact
    rect that reaches `draw_text`, plus an on-device check of the real
    CoreGraphics numbers (at 20 px: ascent 19.34 / descent 4.22 / line_height
    23.55, scaling exactly 2.0× to 40 px).
- **1.2 — rounded rect / ellipse / line.** **Done**, and confirmed to need **no
  backend or `renderer.h` change at all**: `build_rounded_rect_path` and
  `append_elliptical_arc` moved from `widget_paint_compound.h` into
  `hosts/shared/painter.h`, so the compound layers and the five new public
  entries share one implementation (a value ring and a `fill_ellipse` agree on
  their outline). 10 Tier-1 cases, including the radius clamp and the
  deliberately-open `draw_line` path.
- **1.3 — italic.** **Done.** `push_font_styled(family, weight, italic)` on both
  the backend interface and the painter; `push_font` is now exactly
  `push_font_styled(..., false)` and pops the same way. Italic is **resolved,
  never synthesised** — `DWRITE_FONT_STYLE_ITALIC`, `NSFontManager
  convertFont:toHaveTrait:` / `UIFontDescriptorTraitItalic`, `FC_SLANT_ITALIC`
  — and a family with no italic member stays upright rather than being sheared
  (so no `_OBLIQUE`). The style is part of every backend's font-cache key, so
  upright and italic at the same (family, weight, size) cannot collide. The
  painter forwarder degrades to plain `push_font` on a backend predating the
  append, which matters for stack balance: the client calls `pop_font`
  regardless. Verified on the real CG backend (Times upright 184.86 vs italic
  182.65 — genuinely different advances) plus 5 Tier-1 cases.
- **1.4 — wrapped text at painter level** — deferred unless asked; the wrap
  algorithm exists in the MULTILINE widget and is liftable.

### Wave 2 — timer / idle — **DONE**

- **2.1 — platform seam.** `platform_timer_start(Session*, interval_ms)` /
  `platform_timer_stop(Session*)`, implemented on win32 / macOS / Linux / iOS /
  null. The design point: **one native tick per session at the shortest live
  interval**, not one native timer per client timer — the portable
  `hosts/shared/timer_table.h` owns ids, deadlines and multiplexing, so each
  platform layer is ~30 lines and everything error-prone is Tier-1 tested.
  - win32 uses a **thread** timer (`SetTimer(NULL, …, TIMERPROC)`) rather than a
    window timer, because timers are session-scoped and a session may own
    several frames (or, embedded, a frame the DAW owns). The payoff: `WM_TIMER`
    with a NULL hwnd lands on the thread queue, so `platform_run`,
    `platform_pump_once` **and a DAW's own pump** all dispatch it with no
    per-loop plumbing.
  - macOS / iOS use an `NSTimer` in `NSRunLoopCommonModes` (not the default
    mode) so a client animation keeps running during AppKit/UIKit tracking
    loops instead of freezing while a button is held.
  - Linux multiplexes the existing 16 ms `timerfd` heartbeat rather than adding
    an fd — but the heartbeat is **shared with animations**, so `arm_timer`
    became interval-aware (`refresh_timer_arm` takes the min of both demands):
    a client asking for 8 ms must not be slowed to the animation's 16 ms, and an
    animation must not be starved by a client's 1000 ms timer.
  - Two paths needed fixing to honour the documented contract:
    `platform_pump_once` never read the timerfd (so timers would have worked
    only under `run()`), and `platform_embed_pump_and_tick` gated everything
    behind its 16 ms animation window (which would have halved an 8 ms plugin
    timer). Client timers now bypass that gate — their own deadlines already
    decide what is due.
- **2.2 — public API.** `NEUI_API_TIMER` (`include/neui/d/timer.h`):
  `add_timer` / `remove_timer` / `set_timer_interval` + `NEUI_EVENT_TIMER`.
  Chosen over an `on_idle` callback because a plugin wants a *stated rate*,
  and idle has no defined frequency. Session-scoped, and deliberately the **only
  event with no `.widget`** — a timer does not belong to a widget, and omitting
  the field stops a handler being written as though it did.
  - `interval_ms` is a minimum, clamped up to `NEUI_TIMER_MIN_INTERVAL_MS` (4);
    0 is rejected rather than becoming a spin loop. A late tick fires **once**
    rather than catching up, so a slow handler cannot accumulate a backlog of
    its own events.
  - Mutation from inside a handler is safe (the table tombstones rather than
    erases, so the dispatch walk stays valid), including a timer removing
    itself.
- **2.3 — embedded contract.** Documented and honoured: no thread, no `run()`
  from a plugin; the DAW's pump services win32/macOS and `pump_and_tick` drives
  Linux.
- **Review found three real bugs, two reproduced under ASan; all fixed.**
  (1) The per-platform timer registries were function-local statics, so they
  were destroyed *before* the global `sessions` vector whose `~Session` calls
  `platform_timer_stop` — a **SEGV at exit for any client that uses a timer and
  never calls `destroy(session)`, which is what CLAUDE.md's own canonical usage
  does**. All four are now immortal (leaked) maps. (2) The due list was a
  Session *member*, and every nested pump in the tree services timers, so a
  handler opening a modal dialog re-entered the walk and reallocated the vector
  the outer frame was iterating — a use-after-free. Fixed by moving the whole
  walk into `TimerTable::tick_and_dispatch` with a local vector *and* a
  re-entrancy guard (nested ticks are suppressed, not nested). (3)
  Destroying the session from a handler is a use-after-free; documented as
  illegal rather than papered over. Also fixed: a Linux phase-reset regression
  (`timerfd_settime` every tick turned the period into `period + handler time`),
  and a win32 path where a failed `SetTimer` still cached "armed" so timers went
  silently dead.
- The review's sharpest process point: the mutation-during-dispatch tests
  *simulated* the caller's loop, so the suite passed **with the re-entrancy bug
  shipped**. Moving the walk into the portable table made it Tier-1 testable;
  8 new cases now exercise the real walk, including the nesting case.
- Verified: 25 Tier-1 cases over the table (clamping, deadlines, late-tick
  coalescing, shortest-interval arming, self-removal, add-during-tick,
  tombstone reaping) plus an end-to-end run on macOS under a **hand-rolled
  `pump_once` loop** — the standalone case the review called out as impossible:
  16 ms fired 23× and 100 ms 5× over ~600 ms, a self-removing timer fired
  exactly once, retiming took effect, and the tick went silent after the last
  removal.

### Wave 3 — per-frame UI scale (#9) — **DONE, by RECOVERING the lost commit**

The "history problem" flagged in the TL;DR turned out to be recoverable, and the
lost work was **better than the plan**. `20f2b04` was sitting dangling in the
reflog — *"add user UI zoom (NEUI_ATTR_UI_SCALE) to the crossplatform host"*,
874 insertions across 23 files, parented on `edf5f9c`. PR #19 merged the title
and dropped the content. Cherry-picked rather than reimplemented.

Why it beat the plan: this document proposed feeding `real_dpi × ui_scale` into
`backend->update_dpi`. That commit had already **rejected** that approach for a
reason the plan missed — it works on D2D and Cairo but **not CoreGraphics**,
where a window context applies no CTM scale of its own (AppKit hands `drawRect:`
an already-point-based context), so macOS simply would not have zoomed. It uses
one CTM scale in `Session::paint_frame` instead, needing **zero** backend
changes, and goes beyond the plan with `scale` on `neui_event_paint_t` plus
`NEUI_ATTR_PAINT_DEVICE_PIXELS` (real device pixels for meters / scopes) and
`extra_scale` on the painter so `get_scale_factor` stops under-reporting — which
would otherwise have made the QR compound layer bake at 1/Z resolution.

Cherry-pick resolution — two conflicts, both where earlier waves overlapped:
- `platform_macos.mm`: both sides had independently fixed the **same**
  `platform_set_window_pos` client-vs-outer-frame bug. Kept the zoom-aware
  `(zw, zh)` geometry with the fuller explanation.
- `platform_linux.cpp`: orthogonal — its `window_scale(lw)` (which folds zoom
  into the DPI factor, replacing every open-coded `dpi/96`) plus Wave 0.4's XI2
  modifier `buttonmap`. Kept both.

**One real interaction bug this surfaced**, caught by the recovered commit's own
smoke test rather than by reasoning: Wave 0.1's `-setFrameSize:` RESIZE hook
stored the **native** size in `wd.width/height`, but the zoom design's core
invariant is that those stay **logical** at every zoom. At zoom 2.0 a 400×240
frame reported `get_client_rect` as 800×480, and resetting the zoom did not
restore the original. The hook now divides out `frameZoom`. Its suppression test
also had to change: comparing against `wd.width/height` is wrong because
`widget_set_size` updates those *before* calling the platform layer (every
programmatic resize would suppress itself), and comparing the view's previous
frame is wrong because a pure zoom change moves the native frame while the
logical size is unchanged. It now tracks the last **reported logical** size on
the view, seeded at creation.

Verified together: the recovered `neui_zoom_smoke_macos` passes (native content
grows by the zoom, `get_client_rect` stays logical, CUSTOMDRAW gets logical size
with a zoom-inclusive scale, device-pixel mode gets pre-multiplied dimensions,
plus live change / METRICS_CHANGED / reset), both Wave 0.1 resize harnesses still
pass (programmatic *and* the 21-event AppKit `performZoom` stream), and the suite
is 385 cases / 2081 checks green.

**Its review found seven findings; all addressed.** Four were exactly the
"recovered code vs. newer waves" disagreements the cherry-pick was warned about:
- **win32 `platform_menubar_attach` was not zoom-aware** while both sibling
  sizing seams in the same file were — so attaching a menubar to a zoomed frame
  resized it back to the *unzoomed* client size, and the resulting `WM_SIZE`
  then divided that by the full factor: a 400-wide frame at zoom 2 stored
  `wd.width = 200`. That destroys the layout, and it fires at `widget_show` for
  every frame with a MENUBAR plus on every menubar rebuild. Fixed.
- **Fractional-zoom drift, reproduced empirically.** `logical -> native ->
  logical` double-rounds: 402 px at zoom 0.75 became 403 and fired a spurious
  RESIZE, so the commit's own stated invariant broke at z < 1 (and win32
  *truncated*, drifting down at z > 1). Root fix: stop round-tripping at all
  when *we* set the size — `wd.width/height` are already authoritative then.
  macOS/win32 bracket the call with a self-resize flag; Linux records an
  expected size instead, because X11's `ConfigureNotify` is asynchronous and a
  flag would never still be live. win32's `WM_SIZE` also now rounds instead of
  truncating, and got the RESIZE suppression macOS had (it was asymmetric).
- **A live zoom change teleported a user-moved window** to its create position,
  because no xpl platform tracks moves back into `wd.x/y`. Added a documented
  `NEUI_WINDOW_POS_KEEP` sentinel; the zoom only ever wanted to change the size.
- **iOS scaled paint but not input.** `paint_frame`'s CTM is
  platform-unconditional, but the iOS layer (which landed *after* this commit
  was authored) divides no touch coordinates — a zoomed iOS frame would have
  drawn in one space and hit-tested in another. New
  `platform_supports_ui_scale()` gate makes the attr inert on iOS and null.
Plus: min/max constraints ignored the zoom on win32 and macOS (a MIN_WIDTH of
400 at zoom 2 let the user shrink to logical 200) and went stale on every zoom
change everywhere — both fixed; and two public contracts were simply false —
`NEUI_ATTR_PAINT_DEVICE_PIXELS` claims *physical* pixels but only undoes the
zoom, and `events.h` equated `METRICS_CHANGED.ui_scale` with
`metrics_api->ui_scale` when the two now differ *and imply opposite responses*
(iOS Dynamic Type says "re-scale your layout", zoom says "do not"). Documented
honestly rather than papered over.

Verified after the fixes: the fractional-zoom harness passes at 0.75 / 1.25 /
1.5 / 0.5 / 2.0 / 1.0 with the client rect pinned at 402x302 and **zero**
spurious RESIZEs, a genuine `set_size` while zoomed still lands exactly, and a
user-moved window keeps its top-left across three zoom changes while its
min-size constraint tracks the zoom.

Remaining documented gaps in `docs/deferred-issues.md`: baked SURFACEs and `@Nx`
assets are not re-baked on a zoom change (client's call, as for DPI), scroll
deltas are not zoom-divided, DnD drop coords are not zoom-divided on
win32/Linux, and Linux scales window position while win32/macOS do not.

### Wave 4 — cursor, popups, file dialog

**4.1 status: DONE**, with one deliberate deviation from what this plan specified.

The plan called for `set_cursor(session, widget, kind)` on an interface. It shipped
as a **string attribute, `NEUI_ATTR_CURSOR`**, instead. Reasons:

- A per-widget sticky cursor is *state*, not an action, and every comparable
  enum-ish knob in the framework is already a string attr (`NEUI_ATTR_ORIENTATION`,
  `_POLARITY`, `_KNOB_MODE`, `_SCROLL_MODE`, `_ALIGN_TEXT`). An interface method
  would have been the only stateful setter outside the attr bag.
- It gets a `k_well_known_attrs` row, `docs/attributes.md` documentation, and
  reachability from JSON components for free.
- The behavior asset's `cursor` prop is *already a string*, so wiring it later is a
  pass-through rather than a name->enum translation at the boundary.

Pointer warping stays an interface (4.1b) — that genuinely is an action with
begin/end bracketing.

What landed:

- `hosts/shared/cursor_kind.h` — the 17-shape `enum neui_cursor_kind` plus the
  name<->kind parse, replacing **two** hand-kept copies of a two-value list
  (`xpl_host::CursorKind` and an unnamed-namespace mirror in
  `hosts/win32/widgets.cpp` commented "Mirrors xpl_host::CursorKind values"). CSS
  aliases accepted; an unrecognised name means *inherit*, not arrow. Tier-1 tested
  (`tests/test_cursor_kind.cpp`, 10 cases).
- `hosts/shared/win32/cursor_win32.h` — one kind->HCURSOR table for both win32 hosts.
- Inheritance resolved in portable host code (`Session::resolve_cursor_for` walks
  ancestors; `refresh_cursor` dedupes before touching the platform, which matters on
  X11 where each push is a round-trip per window).
- All five platforms implement the full set; `platform_supports_ui_scale`-style
  degradation is documented per shape in `docs/deferred-issues.md`.
- **Two latent defects fixed on the way**, both of which meant there was no
  stickiness to build an API on:
  - win32 never handled `WM_SETCURSOR`, and the class registers `IDC_ARROW`, so
    `DefWindowProc` reverted the cursor on every pointer move. The GRID resize
    cursor only appeared because `WM_MOUSEMOVE` re-set it immediately afterwards.
  - macOS never implemented `-cursorUpdate:` and its tracking area lacked
    `NSTrackingCursorUpdate`, so AppKit reset the cursor from cursor rects on every
    mouse-moved. Same "works by accident" mechanism.
- The GRID's positional override now carries an owner widget, so it cannot leak
  outside the GRID, survives a column-resize drag that leaves the widget, and
  yields to (rather than stomps) a client's `NEUI_ATTR_CURSOR` off the divider.
- `tests/cursor_smoke_macos.mm` — 12-check acceptance harness driving **real**
  `-mouseMoved:` events and reading `[NSCursor currentCursor]`. Negative-probed:
  removing the `-cursorUpdate:` body makes the stickiness check fail.

**Deferred out of 4.1 into 4.1b**: the behavior asset's per-handler `cursor` prop is
still a no-op. The infrastructure it was blocked on now exists, but wiring it needs a
per-handler hit-region hover track plus a new `BehaviorDispatchCtx` callback across
all five platforms. Its TODO in `docs/deferred-issues.md` is therefore **kept**, not
deleted — deleting it while the prop is inert would be a false claim.

Original plan text for 4.1 follows.

- **4.1 — cursor (his #7).** Widen `xpl_host::CursorKind`
  (`platform.h:382`) to a real set (arrow, ibeam, crosshair, hand, EW/NS/NESW/NWSE
  resize, move, none/hidden, wait), implement on all platforms, then expose
  publicly — a `set_cursor(session, widget, kind)` on `NEUI_API_WIDGETS` (or a
  small `NEUI_API_CURSOR`) with per-widget stickiness so hover changes work.
  Unblocks the behavior asset's `cursor` prop; delete that TODO
  (`docs/deferred-issues.md:17`).
  - **Pointer warping / unbounded movement** is a separate call in the same
    interface (`set_pointer_capture_relative` or similar):
    `SetCursorPos` + hide on win32,
    `CGAssociateMouseAndMouseCursorPosition(false)` +
    `CGWarpMouseCursorPosition` on macOS, `XWarpPointer` on Linux. Needed for
    good knob feel; design it as begin/end bracketing so it pairs with the
    existing gesture events.
- **4.2 — rich popups (his #8).** Reuse the tree model rather than inventing a
  second one: build the popup as a `MENUBAR`-shaped tree (a widget the client
  populates with `tree->add` / `set_shortcut` / `set_checked` / `set_menu_cmd`),
  then `popup_tree_menu(session, anchor, x, y, menu_widget)` dispatching
  **async by item id** through an event (`NEUI_EVENT_ITEM_SELECTED` on the menu
  widget) with `NEUI_API_MENU_CLIENT::validate` already applying. Keep the
  existing flat `popup_menu` for compatibility. Section headers = a disabled
  item kind. **Custom components in menus: explicitly out of scope** — agree
  with his own suggestion that a type-in overlay drawn on the surface is the
  better answer.
- **4.3 — file dialog (his #10).** Append `open_file` / `save_file` to
  `NEUI_API_NOTIFY` alongside `message_box`: filter list, initial dir/name,
  multi-select flag, returns paths. `IFileDialog` on win32, `NSOpenPanel` /
  `NSSavePanel` on macOS, XDG portal on Linux (D-Bus is already an optional
  dep via `NEUI_HAS_DBUS`) with a neui-drawn fallback when absent — same
  graceful-degradation shape as the existing message box.

### Wave 5 — xpl partial repaint

Not in his list; it is the perf item that actually applies to him.

- Accumulate a per-frame dirty rect union from `w_invalidate`
  (`hosts/crossplatform/widgets.cpp:692`) instead of invalidating the whole
  frame, pass it to `platform_invalidate`, and have `Session::paint_frame`
  `push_clip` it and skip subtrees whose `abs_*` rect misses it.
- Keep the existing "one repaint per event-loop tick at most" coalescing
  promise; this only narrows *what* gets painted.
- Measure before and after with a 100-widget CUSTOMDRAW grid at 60 Hz — which
  doubles as **his Phase 0a measurement, on the host he should actually use**.
  Worth running early and handing him the number: it may well retire his
  granularity anxiety outright.

### Wave 6 — accessibility (his #12)

The one real programme. Build it in the **xpl host**, once.

- **6.1 — client seam.** `NEUI_API_A11Y` (`include/neui/d/a11y.h`): per-widget
  `set_role` (button / slider / knob-as-slider / checkbox / text / list /
  group / static-decorative), `set_name`, `set_description`, plus value
  reporting for continuous controls (min / max / current / display string) and
  a state mask (disabled / focused / selected / expanded / readonly). Default
  role derives from widget type, so a client that declares nothing still gets
  a usable tree; `static-decorative` is the opt-**out** for the parent-painted
  decoration in his §5.
- **6.2 — the shared provider model.** A portable `hosts/shared/a11y_tree.h`
  that walks the widget tree once and yields nodes: id, parent, ordered
  children, bounds (from `abs_x/abs_y` + size), role, name, value, state,
  tab index (reusing `collect_tab_stops` ordering, `host.cpp:2283`). This is
  Tier-1 testable with no platform code — the same trick `hosts/shared/*.h`
  already uses everywhere.
- **6.3 — win32 provider.** `WM_GETOBJECT` on the frame HWND, UIA fragment
  provider over the shared model (`IRawElementProviderFragmentRoot` on the
  frame, `IRawElementProviderFragment` + `IValueProvider` /
  `IRangeValueProvider` / `IInvokeProvider` per node), and
  `UiaRaiseAutomationEvent` on focus / value change.
- **6.4 — macOS provider.** `NSAccessibility` overrides on the frame view
  returning `NSAccessibilityElement` children from the shared model, with
  `NSAccessibilityPostNotification` on focus / value change.
- **6.5 — Linux.** AT-SPI over D-Bus, gated on `NEUI_HAS_DBUS`, graceful no-op
  when absent. Lowest priority of the three.
- **6.6 — focus parity prerequisites.** Accessibility needs deterministic
  focus, so fold in the open items: KNOB is not a keyboard tab-stop on macOS
  and macOS Tab participation follows Full Keyboard Access
  (`docs/deferred-issues.md:6-7`). On xpl this is our own traversal, so it is
  tractable; on the native hosts it is OS policy. Another reason 6.x lives in
  xpl.
- Retire the "Tier B focus parity" line (`docs/deferred-issues.md:8`) when this
  lands — it and #12 are the same request from two directions, exactly as he
  observed.

### Explicitly not doing now

- **#11 — render-passive child widgets in the win32 native host.** Correct
  analysis, wrong host for this use case. The xpl host already has the
  architecture. Revisit only if a standalone native-host app needs many painted
  widgets; if it ever happens, his §9.11 sketch (shared device context, per-child
  `{offset, size, clip}`, one `Present` per frame, children no-op'ing
  `WM_PAINT`/`WM_ERASEBKGND`, and the `WS_CLIPCHILDREN` subtlety) is the right
  spec and should be kept.
- **Custom components inside popup menus.** Out of scope, per §2.3 and his own
  recommendation.
- **UI scale on the native hosts.** Wave 3 is xpl-only by design.

---

## 4. Sequencing against his phases

| His phase | What he needs from us | Ready when |
|---|---|---|
| 0a — measure the widget ceiling | Nothing. Re-target the measurement at the xpl host; we will run it as Wave 5's baseline and hand him the number. | now |
| 0b — prove the `Graphics` shim | Wave 1 (metrics + alignment, round rect / ellipse / line). Everything else in the shim already maps. | Wave 1 |
| 1 — the `Component` adapter | Wave 0 (RESIZE on xpl macOS is the one that would actually break him). | Wave 0 |
| 2 — the neui asks | Waves 2, 3, 4. | Waves 2-4 |
| 3 — two-filters | Embedding: **already shipped**. Plus Wave 4.2 (preset menu) and 4.3 (patch load/save). | Wave 4 |
| accessibility parity with JUCE | Wave 6. | Wave 6 |

## 5. House rules that apply to this work

- Every new `NEUI_ATTR_*` needs a `k_well_known_attrs` row
  (`hosts/shared/attrs.h`) — see `docs/attributes.md`.
- Public struct evolution is **vtable-append only**; do not reorder slots.
  `neui_event_wheel_t` (0.4) is a payload struct, not a vtable — growing it
  still requires all hosts to rebuild, so land it as one commit across every
  host.
- Portable logic goes in `hosts/shared/*.h` with Tier-1 tests in `tests/`
  (this is what makes 6.2 testable without a screen reader).
- Keep the build warning-clean at `/W4` / `-Wall -Wextra`.
- Document each landed item in the matching `docs/` file and prune the
  corresponding `docs/deferred-issues.md` entry.
