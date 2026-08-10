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
  - I initially also wired the thing this unblocked — `WHEEL`
    `fine_modifier` — and **backed it out** after review. It cannot ship yet
    for two independent reasons, both now documented on the WHEEL branch of
    `hosts/shared/behavior_runtime.h` and in `docs/deferred-issues.md`:
    **(1) Shift is already spoken for** — the win32 + macOS xpl layers turn a
    Shift-held vertical notch horizontal *and negate the delta*, and this path
    derives direction from `sign(delta)`, so Shift **already inverts** a
    behavior wheel on those two hosts; multiplying by `fine_scale` would have
    shipped a default-configured fine mode that also inverts, on exactly the
    plugin hosts. **(2) fine + `steps` starves** — the wheel keeps no unsnapped
    accumulator (DRAG has `drag_continuous`), so once
    `step * fine_scale * |delta| < quantum/2` every fine notch rounds back and
    the wheel goes permanently dead, divergently across platforms. The native
    KNOB dodges (2) by advancing exactly one step per notch when stepped.
    Tier-1 cases pin the current coarse contract *and* the Shift inversion, so
    the eventual fix has to change a test deliberately.

**Three additional fixes the wave surfaced** (all real, all beyond the four
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

Verified: `cmake --build` clean and warning-free on macOS (Xcode/Debug);
`ctest` green — 317 cases, 1843 checks, 0 failures. The win32 and Linux edits
are **compile-unverified locally** (authored on macOS), matching the
established convention for this repo's cross-machine flow.

### Wave 1 — painter completeness

The "annoys you daily" wave. Almost all of it is `hosts/shared/painter.h` plus
one header.

- **1.1 — text metrics + alignment.** Two parts:
  - Backend: add a metrics query returning **ascent / descent / line-height**
    (and keep width). Every backend already computes these — DirectWrite
    `IDWriteFontFace` / `DWRITE_LINE_METRICS`, `CTLineGetTypographicBounds`,
    `cairo_font_extents`. Append to `neui_render_backend_t`.
  - Painter: append a metrics accessor, and an **explicit alignment** form of
    `draw_text` taking horizontal (`start` / `centre` / `end`) and vertical
    (`top` / `middle` / `baseline` / `bottom`) enums. Keep the existing
    `draw_text` as-is (left / centred) so nothing regresses; the compound text
    layer's manual `measure_text` x-offset dance
    (`widget_paint_compound.h:484-495`) then collapses onto it, and the
    `align_y` "approximated by shifting the rect" comment goes away.
  - Bonus in the same pass: make `NEUI_API_METRICS::measure_text` exact on
    desktop (route it through a text-only measure context) and drop the
    "best-effort estimate" caveat.
- **1.2 — rounded rect / ellipse / line.** Ship as **painter-level helpers over
  the existing path API** — no backend or `renderer.h` change. Promote
  `build_rounded_rect_path()` out of `widget_paint_compound.h` into
  `hosts/shared/painter.h`, then append `fill_round_rect` / `draw_round_rect` /
  `fill_ellipse` / `draw_ellipse` / `draw_line` to `neui_painter_api_t`. Have
  the compound `rect` layer call the new helpers so there is one implementation.
- **1.3 — italic.** Add a style axis to `push_font` (append a new
  `push_font_styled` rather than changing the existing signature). All three
  backends carry style in their font descriptors already.
- **1.4 — wrapped text at painter level** *(defer unless he asks)*. The
  algorithm exists in the MULTILINE widget; lifting it into
  `hosts/shared/painter.h` as a measure-and-break helper is a follow-up, not a
  blocker — his ported paint code lays out its own text.

### Wave 2 — timer / idle

- **2.1 — platform seam.** Add `platform_timer_start(id, interval_ms)` /
  `platform_timer_stop(id)` to `hosts/crossplatform/platform.h` and implement
  on win32 (`SetTimer`, joining the existing `XPL_*_TIMER_ID` family), macOS
  (`NSTimer` on the runloop, plus the embedded-view case), Linux (multiplex on
  the existing `timerfd` heartbeat rather than adding fds), iOS, null.
- **2.2 — public API.** New `NEUI_API_TIMER` (`include/neui/d/timer.h`):
  `add_timer(session, interval_ms) -> id`, `remove_timer(session, id)`, and a
  `NEUI_EVENT_TIMER` payload `{ timer_id }`. Prefer this over an `on_idle`
  callback — a plugin wants a stated rate, and an idle callback has no defined
  frequency.
- **2.3 — embedded contract.** Document (and honour) that in embedded mode the
  timer is serviced by the DAW pump on win32/macOS and by `pump_and_tick` on
  Linux — i.e. it composes with `NEUI_API_EMBED` rather than needing `run()`.
  This is the piece that makes it genuinely useful to him.

### Wave 3 — per-frame UI scale (his #9)

First: **resolve the history problem** — `7683adb`'s message claims this
feature and its tree does not contain it. Either the PR content was lost in a
re-merge or the title is wrong. Determine which, and if the work exists on a
lost branch, recover it instead of rewriting.

Then implement, **xpl host only** to start:

- `NEUI_ATTR_UI_SCALE` (float, default 1.0) on a frame widget, plus a
  `k_well_known_attrs` row (`hosts/shared/attrs.h`) per the house rule.
- Effective scale = `real_dpi × ui_scale`, fed to `backend->update_dpi` and to
  the platform layer's window-size and **input-coordinate** conversions. On
  xpl these are a handful of sites, not 81.
- Live: on change, resize the render context, recompute `abs_x/abs_y`, fire
  `NEUI_EVENT_METRICS_CHANGED` (the payload already carries `ui_scale`,
  `include/neui/d/events.h:348-354`), invalidate.
- Explicitly **out of scope**: the win32/macOS native hosts. Native control
  fonts (HFONT / NSFont) do not follow the multiplier and making them follow is
  a separate job. Document the limitation; it does not bite a CUSTOMDRAW-only
  client, which is exactly his case.
- Keep `NEUI_API_METRICS::ui_scale` reporting the process-global painted-UI
  scale unchanged, and document the relationship between the two clearly —
  these are two multipliers on one axis and conflating them in docs will cost
  someone a day.

### Wave 4 — cursor, popups, file dialog

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
