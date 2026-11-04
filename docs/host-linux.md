<!-- neui reference: Linux (X11 + Cairo) xpl host internals. Extracted from CLAUDE.md. -->

## Linux host (X11 + Cairo) internals

- **Linux** (X11): `neui-xplhost`; backend `neui-backend-cairo` (software, blitted via XShm/XPutImage); xpl platform `platform_linux.cpp`. No native host - the xpl host is the only one. Clipboard via the CLIPBOARD + PRIMARY selections (`hosts/shared/linux/clipboard_linux.h` - owner window serves `SelectionRequest` from a `DataItem`, reads via a synchronous `XConvertSelection`/`SelectionNotify` pump; text + arbitrary MIMEs). **PRIMARY** (select-to-copy / middle-click-paste, text-only): `set_primary_text`/`get_primary_text`/`clear_primary` (serve_request picks the source `DataItem` by `req.selection`); xpl text widgets publish PRIMARY on mouse drag-select / double-click-word / Ctrl+A / Ctrl+C, and `platform_linux.cpp` button-2 pastes it (caret placed at the click via a synthetic BUTTON_DOWN, then `WidgetData::insert_text`). **INCR** (chunked transfers for payloads over the server max-request size) is implemented both ways: `serve_bytes` streams large served payloads on each requestor `PropertyDelete` (`_incr_sends` state, driven from `handle_event` `PropertyNotify`), and `read_property`/`read_incr` reassemble large incoming ones (draining the stale INCR-announce `PropertyNewValue` first). Verify: `tests/incr_smoke.cpp` (`neui_incr_smoke`, ~1 MB round-trip between two `ClipboardX11` instances - exercises both send + receive halves). Format-32 X properties (the TARGETS atom list) are read at `sizeof(long)` per item, not `fmt/8` - Xlib widens format-32 to C `long`, 8 bytes on LP64, so `nitems*4` would truncate the offered-formats list. Full DnD via XDND v5 (`hosts/shared/linux/dnd_linux.h` pure helpers + both halves in `platform_linux.cpp`). **Drop-target**: advertises `XdndAware`, handles Enter/Position/Leave/Drop ClientMessages → `Session::dispatch_dnd_*`, replies XdndStatus/Finished, pulls drop bytes via an XdndSelection pump. **Drag-source** (`begin_drag` + `make_drag_preview`): a blocking spin that grabs the pointer **on root** (the source frame can be momentarily non-viewable mid-reparent → GrabNotViewable), walks the window stack (`XQueryTree` geometry + descend) to the deepest `XdndAware` window under the cursor, runs the Enter/Position/Status loop for foreign targets and dispatches `Session::dispatch_dnd_*` **directly** for our own windows (internal drags - no X selection round-trip, which would self-deadlock the spin), serves `SelectionRequest`s from the item, and shows a follow-the-cursor override-redirect preview window. Message box (`notify->message_box`): X11 has no native dialog, so it's neui-drawn - `hosts/shared/linux/message_box_linux.h` decodes the `NEUI_MB_*` flags (button set / default / Esc / icon) and `platform_linux.cpp::run_message_box` renders it with the Cairo backend (icon disc + glyph, word-wrapped text, themed buttons) in a nested modal loop (transient + `_NET_WM_STATE_MODAL`), returning the `NEUI_ID_*`. DAW-embedding seams (`platform_set_embed_parent` / `platform_embed_event_fd` / `platform_embed_pump_and_tick`) live here, fronted by the public `NEUI_API_EMBED` interface (`include/neui/d/embed.h` - `set_parent`/`event_fd`/`pump_and_tick`; the parent X Window id travels as `void*` through `uintptr_t`); win32 (WS_CHILD of the foreign HWND) and macOS (NEUIView subview of the foreign NSView) implement the same interface, with `event_fd = -1` and a no-op pump because the DAW's own message pump / runloop services a child there. The XDND receiver + drop/clipboard selection reads run on the **window's own `Display`** (`g_display` standalone, the DAW's connection when embedded), and a process-wide `XSetErrorHandler` swallows the BadWindow/BadMatch races a vanishing foreign drag-source can trigger so they can't abort the host. `platform_set_cursor` is wired (`XCreateFontCursor(XC_sb_h_double_arrow)` for GRID column-resize, per-`Display` cached). **Window icon** (`NEUI_ATTR_ICON_PATH`): `platform_set_window_icon` loads the image and sets `_NET_WM_ICON` (CARD32 `[w, h, ARGB...]`, format 32; the premultiplied BGRA loader output is un-premultiplied back to straight ARGB); applied at `create_frame` + live. **In-UI menubar**: X11 has no native menu bar, so the host draws one itself - a horizontal band at the top of the frame's client area with click-to-open, hover-to-switch cascading dropdowns (arbitrary submenu depth), right-aligned shortcut labels, separators, and popup-open auto-disable (`WidgetData::can_perform_command` + `neui_menu_client_t::validate`). It lives in shared code (`Session::paint_menubar` + `handle_menubar_click/hover/key` + the `mb_build_band`/`mb_build_columns` layout helpers in `host.cpp`), gated by the `platform_menubar_in_frame()` seam (true only on Linux; Win32 HMENU / macOS NSMenu / null return false). The band reserves a `MENUBAR_BAND_H` (24 px) top inset via `Session::frame_top_inset`: `paint_frame` offsets the whole child walk down by it (so cached `abs_x/abs_y` stay screen-accurate for hit-test) and the Linux `ConfigureNotify` subtracts it from the reported `RESIZE` height. The content area is queryable - `widgets->get_client_rect(widget, x, y, w, h)` (the Win32 `GetClientRect` analogue, backed by `Session::widget_client_rect`) returns a frame's usable area excluding the band (origin `(0, inset)`, size `(w, h-inset)`); on native-menu hosts / menubar-less frames / non-frame widgets it is `(0, 0, width, height)`. Clients should prefer it over `get_size` when laying out against a frame that may carry a menubar; the toast anchor uses it internally. The menu *model* still lives in `MenubarWidget` (reconstructed from `parent_item_id` links at paint/hit-test time); `platform_menubar_create` returns a non-null sentinel so `t_add` populates it, and the other `platform_menubar_*` mutators stay no-ops. Accelerators are matched in the key path via `Session::try_menubar_accel` (the Win32 HACCEL path is `MSG`-based, so it doesn't run here) → `dispatch_menu_event` (built-in command first, then client `TREE_ITEM_ACTIVATED`). Verify: `tests/menubar_smoke.cpp` (`neui_menubar_smoke`, built-not-ctest-registered - needs a live display; drives synthetic clicks + accelerators and asserts the activated item, incl. a cascaded submenu). **Smooth scroll**: GRID / SECTION pixel-precise + inertial + rubber-band scrolling is wired via XInput2 (`#ifdef NEUI_HAS_XI2`, gated on `libxi-dev`). `xi2_select_window` selects `XI_Motion` per frame window; `handle_xi2_scroll` diffs the scroll-class valuators (`XIScrollClassInfo.increment`) into wheel notches and `feed_scroll` routes them through the same shared kinetics the other hosts use (`grid_scroll_wheel` / `section_scroll_wheel_kinetic`, pixel-precise; classic line-quantised `MOUSE_WHEEL` for stepped surfaces, via a fractional notch accumulator). The 16 ms timerfd heartbeat (`arm_timer` / `tick_animations` / `step_scroll_bounce`, also stepped in `platform_embed_pump_and_tick`) runs `grid_scroll_bounce_step` / `section_scroll_bounce_step` spring-back per window (`bouncing_grid_index` / `bouncing_section_index`). Legacy core Button 4-7 stay as the fallback and are suppressed only AFTER the first real XI2 scroll arrives (`g_xi2_scroll_seen`), so servers/XWayland without scroll valuators degrade cleanly to stepped scrolling; `g_xi2_scroll_seen` also flips the `PLATFORM` kinetics default to SMOOTH on Linux. When `libxi-dev` is absent the whole XI2 block compiles out and only core-button stepped scrolling is built. Selecting `XI_Motion` suppresses core `MotionNotify` on most servers, so when XI2 is active `XI_Motion` also drives hover/drag (`do_motion`, gated by `g_xi2_motion_seen`), not just scroll - the core `MotionNotify` path is skipped once XI2 motion is seen. **System dark/light** (`hosts/shared/linux/theme_provider_linux.h`, `#ifdef NEUI_HAS_DBUS`, gated on `libdbus-1-dev`): `ensure_theme_provider_linux` (run from `platform_init`, before any Session) reads `org.freedesktop.appearance` / `color-scheme` from the XDG desktop portal over D-Bus into `mutable_current_palette()` (1=dark, 2=light, 0=no-pref→dark), and a `SettingChanged` signal filter updates it live + `broadcast_theme_change()` → `Session::on_theme_changed` (repaints `NEUI_ATTR_FOLLOW_SYSTEM_THEME=1` frames). The session-bus fd is folded into the `platform_run` `select()` loop (`theme_dbus_fd` / `theme_dbus_dispatch`; also dispatched in the embedded pump). Absent `libdbus-1` (or no running portal), the block compiles out / no-ops and the default palette stands. Deferred (clean no-op stubs / not-yet-wired): `image/png` clipboard/DnD - see the Clipboard / drag&drop section of `TODO.md`.

**Process-exit teardown.** `g_windows` (the `Window` -> `LinuxWindow*` map) is
**immortal** - a reference to a deliberately leaked map - for the same reason
`client_timer_sessions()` is: the `sessions` vector lives in `host.cpp`, so the
relative static-destruction order across the two TUs is unspecified, and
`~Session` reaches `g_windows` through four of its six teardown steps
(`platform_timer_stop` -> `refresh_timer_arm` -> `any_window_animating` iterates
it; `end_relative_pointer`, `close_all_popup_surfaces` and `release_cursor` all
look windows up in it). Before this, an application that simply returned from
`main()` without calling `api->destroy(session)` - which is most example code -
**segfaulted in static destruction**, after doing all of its work correctly. The
failure mode is worth remembering because of how it presents: the crash happens
with stdio buffers still unflushed, so a program that printed a full successful
log appears to have printed *nothing at all*. Three Linux harnesses
(`neui_menubar_smoke`, `neui_notify_smoke`, `neui_dnd_source_smoke`) looked
totally broken for exactly this reason while in fact passing every check. If a
Linux binary ever goes silent, re-run it under `stdbuf -o0` before concluding it
produced no output.

**Driving X11 harnesses deterministically.** `neui_dnd_source_smoke` used to fail
roughly two runs in five, and both causes were the same mistake: waiting a fixed
number of milliseconds for something asynchronous. `begin_drag` takes an
`XGrabPointer` on the root window, and pointer motion warped in before that grab
lands is delivered to whatever is under the cursor instead of to the drag, so the
target is never entered and nothing is dropped. The fix is to wait on
**observable state** rather than on time: the driver thread waits for the
`XdndSelection` owner to appear (the drag announcing itself - the line directly
above the grab), then **keeps jiggling the pointer until the target reports it
was entered**, and only then synthesizes the button-release. A warp that arrives
too early simply gets followed by another one, so no estimate of grab latency is
needed anywhere. Two details that matter: probing readiness with our own
`XGrabPointer` would be more direct but can *steal* the grab from `begin_drag`
and fail the drag outright (the observation must not perturb what it observes),
and the jiggle alternates two positions because X coalesces a warp to where the
pointer already sits into no motion event at all - so "warp to the centre" is a
no-op when a previous run left the pointer there.

The same discipline fixed two more harnesses that only misbehaved when the suite
was run back-to-back, and both had been reporting timing artefacts as product
bugs. `neui_embed_smoke` pumped a fixed ~1 s and then sampled the child window's
pixels once, reporting `FAIL: child did not render` when the paint had simply not
landed yet; it now pumps **until** the child renders, bounded, so a genuine
rendering failure still reports in ~11 s rather than hanging.
`neui_popup_surface_smoke_linux` needed the fussiest version of it, because a
newly mapped window passes through three states that each look settled: not yet
reparented (the client sits perfectly still at the position neui asked for, so a
"geometry stopped changing" check returns the PRE-reparent origin - reparenting
was observed to take up to ~3 s here); reparented but parked at a placeholder
position offscreen (`-32730,-32709` under Weston, also perfectly stable); and
finally settled. It waits for reparented **and** on-screen **and** unmoving, then
separately for the WM to hand over the input focus, because phases 5 and 6 are
statements about the focus and assert nothing before it arrives. Getting that
wrong is not merely a stale number: a late WM move is a real `ConfigureNotify` on
the owner, and neui correctly dismisses the stack as `OWNER_MOVED` in the middle
of a phase testing something else - a correct product reaction that reads as a
popup bug.

**One environment quirk worth knowing (WSLg / Weston XWM).** After certain runs
of the suite, a newly created neui frame is reparented into its decoration frame
and then left at Weston's offscreen placeholder (`-32730,-32709`), never placed
on screen - indefinitely, not just slowly. It is not a neui defect and not a
wedged desktop: a pure-Xlib window created in the very same moment maps instantly
at (106,127), and running any unrelated X client in between clears the condition
for the next neui window too. It reproduced exactly on the sequence
`popup_surface_smoke_linux` -> `notify_smoke` -> `dnd_source_smoke`, and on
neither predecessor alone.

It matters because of how it *presents*: `neui_dnd_source_smoke` blocks inside
`begin_drag`, which spins on its own X connection and never runs the neui event
loop, so a window the WM has not finished placing stays unplaced for the drag's
whole lifetime. The driver then warps the pointer to a centre computed from that
offscreen rect and the drag - quite correctly - finds no `XdndAware` window under
it, and the run reports "no drop", "payload mismatch" and "action != COPY": three
convincing product failures from a window that was never on screen. The harness
now waits for its window to be **viewable and on-screen** before dragging,
re-asserts the geometry a few times if it is parked (which unsticks it), and says
`WARN: window never became viewable on screen` if it never gets there - so the
environment reports itself instead of impersonating a drag-source bug.
