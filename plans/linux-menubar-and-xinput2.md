# Linux port — remaining work: in-UI menubar + XInput2 smooth scroll

Handoff notes for the two larger remaining chunks of the Linux (Cairo/X11) port.
The core port + clipboard + full XDND + message box shipped (see
`linux-port-cairo-x11.md`). A code-review pass fixed the LP64 clipboard read,
embedded-Display XDND/clipboard routing, the global X error handler, the stale
`XdndStatus` guard, `cairo_draw_rect` edge parity, the idle-timer gating, the
per-motion XDND target-scan cache, cursor shapes, the image-loader overflow
guard, and the message-box `shade` dedup. All committed.

Three smaller deferrables remain besides the two below: PRIMARY selection
(middle-click paste), INCR (chunked clipboard/XDND transfers — currently *guarded*
to fail cleanly, not supported), `image/png` clipboard/DnD, window icon
(`_NET_WM_ICON`), and `theme_provider_linux.h` dark-mode via the
`org.freedesktop.appearance` portal.

---

## 1. In-UI menubar — ✅ SHIPPED (2026-06-14)

Implemented as designed below, with full cascading submenus (arbitrary depth).
Summary of what landed:
- New seam `platform_menubar_in_frame()` (true only on Linux; Win32/macOS/null
  false). `platform_menubar_create` on Linux now returns a non-null sentinel so
  `widgets.cpp::t_add` actually populates the model (it bails on a null hmenu).
- Shared runtime in `host.cpp`: `Session::frame_top_inset` /
  `paint_menubar` / `handle_menubar_click` / `handle_menubar_hover` /
  `handle_menubar_key` / `close_menubar_menu` / `try_menubar_accel`, plus the
  free layout helpers `mb_build_band` / `mb_build_columns` and the
  anon-namespace `MenuRowL` / `MenuColL` / `MenuBandItem` types. Geometry is
  recomputed on demand from `(_menu_open, _menu_path)` using the frame's own
  render ctx for measurement (no cached rects → resize- and multi-frame-safe).
- `paint_frame` offsets the child walk down by `frame_top_inset` (band height
  `MENUBAR_BAND_H = 24`) so children sit below the band and cached `abs_x/abs_y`
  stay screen-accurate. Linux `ConfigureNotify` subtracts the inset from the
  reported `RESIZE` height; input wired in `platform_linux.cpp` (band/dropdown
  pre-checks in button-press, motion, key; release-swallow; close on FocusOut +
  right-click).
- Auto-disable honours `can_perform_command` + `neui_menu_client_t::validate`.
  Accelerators matched in the Linux key path (`try_menubar_accel` →
  `dispatch_menu_event`), since the Win32 HACCEL path is `MSG`-based.
- Verification: `tests/menubar_smoke.cpp` (`neui_menubar_smoke`,
  built-not-ctest-registered) drives synthetic clicks + Ctrl+Z/Ctrl+S
  accelerators and asserts the activated item, including a File→Recent→doc1
  cascade. All four checks pass on X11.

Divergence from the original sketch below: the dropdown uses the **combo-overlay
interaction model** (non-blocking, hover-to-switch + cascade) rather than the
blocking `open_popup_menu` pump — a menubar needs to switch top-level menus and
open submenus on hover, which the single-shot popup pump can't express. It still
reuses the popup's layout constants / row styling.

Original design notes (kept for reference):

**Why it's needed:** X11 has no native menubar. The menu *model* already works
(`tree->add` on a `NEUI_W_MENUBAR`, `set_shortcut`, `set_menu_cmd`,
`MenubarWidget` + `Session::_menubars` exist), but `platform_menubar_*` in
`platform_linux.cpp` are all no-op stubs, so nothing renders. Win32/macOS
delegate to the OS (`HMENU`/`NSMenu`); Linux must draw it itself — same situation
the message box was in.

**Shape of the work:**
- Render the MENUBAR as a horizontal band at the top of the frame's client area
  (below the WM title bar). The xpl paint walk currently skips menubars
  (`!wd.is_menubar()` guards in `host.cpp` paint/layout at ~lines 468, 533, 1773,
  1835). On Linux the band needs to (a) reserve vertical space so child widgets
  start below it, and (b) get painted with top-level popup labels.
- Top-level popups open a dropdown. **Reuse the existing popup overlay machinery**
  rather than inventing a new one: `Session::_popup_active`,
  `handle_popup_click` / `handle_popup_hover` (`platform_linux.cpp` ~330/356/409),
  and the blocking `popup_menu` primitive built on `platform_run_modal_until`.
  The message box (`run_message_box`) is the reference for neui-drawing a themed,
  interactive surface with the Cairo backend + nested modal pump.
- Accelerators already work (HACCEL is win32-only; on Linux shortcuts are matched
  in the key path — verify `set_shortcut` display strings still append after `\t`
  and that `set_menu_cmd` → `invoke_focused_command` routing fires). The menubar
  draw just needs to show the shortcut text right-aligned in dropdown rows
  (`shortcut_format.h`).
- Menu-item auto-disable on open: honor `WidgetData::can_perform_command` +
  optional `neui_menu_client_t::validate` (same contract as the other hosts).

**Decisions to make:** whether the band lives inside the frame's render ctx
(simplest — paint in the frame's paint pass, hit-test in the frame's mouse path)
or as a child overlay. Inside-the-frame matches how the toast + message box are
done. Watch the client-area math: `RESIZE` events and child (0,0) origins must
account for the reserved band height, consistently across paint, hit-test, and
`ensure_visible`.

**Verification:** `neui_example` builds a menubar (Edit → Undo etc. with
`set_shortcut`/`set_menu_cmd`). Run it on X11; the menu should appear, open on
click, navigate by hover, fire `TREE_ITEM_ACTIVATED` / routed commands, and gray
disabled items. Add a smoke harness in the style of `tests/notify_smoke.cpp`
(background thread sends synthetic clicks/keys, asserts the activated command) —
built-but-not-ctest-registered since it needs a live display.

---

## 2. XInput2 smooth scroll (GRID + SECTION kinetics) — ✅ SHIPPED (2026-06-14)

Implemented as designed. Summary of what landed in `platform_linux.cpp`:
- **Optional build dependency**: CMake `pkg_check_modules(XI xi)` defines
  `NEUI_HAS_XI2` + links libXi when present; otherwise the whole XI2 block
  compiles out and only the classic core Button 4-7 stepped path is built (so
  `libxi-dev` is not a hard build dependency). CLAUDE.md build deps updated.
- **Setup**: `ensure_xi2` negotiates XI2 ≥ 2.0 per connection (opcode is
  server-global); `xi2_select_window` selects `XI_Motion` (`XIAllMasterDevices`)
  on every frame window in `create_frame` — standalone and embedded (own `dpy`).
- **Extraction**: `XI_Motion` arrives as a `GenericEvent` cookie, handled at the
  top of `dispatch_x_event` (before the `xany.window` lookup, since cookies
  carry none) with `XGetEventData`/`XFreeEventData` bracketing.
  `handle_xi2_scroll` looks up the device's `XIScrollClassInfo` (cached lazily by
  device id), diffs each scroll valuator against its last value
  (`(value-last)/increment`) into wheel notches (sign-flipped to the neui
  convention: +dv up, +dh left).
- **Routing** (`feed_scroll`) mirrors `platform_win32`'s WM_MOUSEWHEEL: combo
  overlay first, then GRID-smooth / SECTION kinetics fed pixel-precise
  (`grid_scroll_wheel` / `section_kinetic_wheel_linux` → `section_scroll_wheel_kinetic`),
  else a classic line-quantised `MOUSE_WHEEL` for stepped surfaces via a
  fractional notch accumulator (`scroll_v_accum`/`scroll_h_accum`) so sub-notch
  trackpad deltas still eventually emit whole lines.
- **Spring-back**: per-window `bouncing_grid_index` / `bouncing_section_index`;
  `any_window_animating` + `tick_animations` + `step_scroll_bounce` extend the
  toast-only 16 ms timerfd heartbeat to run `grid_scroll_bounce_step` /
  `section_scroll_bounce_step` (also stepped in `platform_embed_pump_and_tick`
  for embedded mode), arming/disarming the timer around active bounces.
- **Fallback / default**: legacy core Button 4-7 stay active and are suppressed
  only after the first real XI2 scroll (`g_xi2_scroll_seen`) — so a server /
  XWayland setup without scroll valuators degrades cleanly to stepped scroll and
  never double-counts. `g_xi2_scroll_seen` also flips the `PLATFORM` kinetics
  default (`grid_smooth_enabled` / `scroll_kinetics_smooth_enabled`
  `platform_default_smooth`) to SMOOTH on Linux once XI2 is confirmed.

Verification is hardware-dependent (precise touchpad / hi-res wheel) per the
notes below; the no-XI2 fallback build is verified warning-clean, and direction
signs are noted for a one-line flip if a device scrolls inverted.

Original design notes (kept for reference):

**Why it's needed:** GRID/SECTION inertial smooth-scroll + elastic rubber-band is
wired on Win32/macOS but not Linux. Today Linux wheel input arrives as classic
X core Button 4/5 presses (`dispatch_wheel` in `platform_linux.cpp` ~291, delta
±1), so only stepped scrolling works. The shared kinetics math already exists and
is host-neutral: `scroll_kinetics.h` (`ScrollKinetics`, `scroll_wheel`,
`scroll_bounce_step`), `grid_model.h` (`GridScrollKinetics`, `grid_scroll_wheel`/
`_bounce_step`/`_commit`), `widget_section_scroll.h`
(`section_scroll_wheel_kinetic`/`_bounce_step`). The job is feeding it
pixel-precise deltas + running a spring-back heartbeat.

**Shape of the work:**
- **Enable XI2:** `XQueryExtension("XInputExtension")` + `XIQueryVersion(2,x)`;
  `XISelectEvents` on each frame window for `XI_Motion` (carries scroll-class
  valuators) — or the simpler `XI_RawMotion` if per-window is fiddly. Detect the
  smooth-scroll valuators via the device's `XIScrollClassInfo` (horizontal +
  vertical, with `increment`). Fall back to core Button 4/5 stepping when XI2 is
  absent (remote X, old server) — keep `dispatch_wheel` as the fallback.
- **Feed deltas:** convert valuator deltas (pixels / increments) into the
  `precise`-delta form the kinetics integrators expect, mirroring how
  `platform_win32.cpp` feeds synthetic precise notches and `platform_macos.mm`
  feeds NSEvent precise deltas. The xpl host already routes the wheel to the
  nearest scrolling-section ancestor via `dispatch_wheel_event(hit, ev,
  stop_before)`; route the precise delta the same way.
- **Spring-back heartbeat:** the 60 Hz timerfd infra now exists but is gated to
  toasts only (`arm_timer` / `any_window_animating`, `tick_animations` checks
  `toast_anim`). Extend it: track "a grid/section bounce is in flight" (per
  window, or a global active-bounce set), `arm_timer(true)` while any bounce is
  active, and have `tick_animations` step each active integrator
  (`grid_scroll_bounce_step` / `section_scroll_bounce_step`) + invalidate, then
  `arm_timer(false)` when all settle. Win32 uses a `bouncing_grid_index`
  per-frame; macOS a per-view NSTimer. Mirror that bookkeeping.
- `NEUI_ATTR_SCROLL_KINETICS` / `grid.scroll_mode` PLATFORM default should resolve
  to SMOOTH on Linux desktop (matching macOS) once XI2 is present, else STEPPED.
  Decide and document the default; today the CLAUDE.md says PLATFORM = stepped on
  "Win32/null" — add Linux to the smooth side when XI2 is available.

**Gotchas:** XI2 cookie events need `XGetEventData`/`XFreeEventData` bracketing in
the dispatch loop. Embedded windows (own `lw->dpy`) must select XI2 on their own
connection. The heartbeat must also work in embedded mode — there the DAW drives
`platform_embed_pump_and_tick`, which already has a 16 ms gate; step active
bounces there too (it currently only bumps `needs_paint` for `toast_anim`).

**Verification:** `neui_grid_example` + `neui_section_scroll_example` on X11 with
a precise touchpad / high-res wheel — expect pixel-smooth scroll + overscroll
rubber-band + spring-back, matching macOS feel. The earlier session confirmed
"smooth scroll is missing" on Linux; this closes it.

---

## File pointers
- `hosts/crossplatform/platform_linux.cpp` — `dispatch_wheel` (~291), `arm_timer`/
  `any_window_animating`/`tick_animations` (~166–205), `platform_menubar_*` stubs
  (~1588), `run_message_box` (reference neui-drawn modal, ~1059),
  `platform_run_modal_until` (~1560), `platform_embed_pump_and_tick` (~1380).
- `hosts/crossplatform/host.cpp` — `MenubarWidget`, `_menubars`, paint/layout
  `!is_menubar()` guards, `dispatch_wheel_event`.
- Shared kinetics: `scroll_kinetics.h`, `grid_model.h`, `widget_section_scroll.h`.
- Native references: `platform_win32.cpp` / `platform_macos.mm`
  (`platform_set_cursor` done; menubar + smooth-scroll wiring to mirror).
