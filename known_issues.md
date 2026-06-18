# Known issues

Behavioral quirks and accepted compromises that are NOT bugs to fix but are
worth knowing about. Open *work* lives in `TODO.md`; design rationale lives in
`CLAUDE.md` / `plans/`. Each entry states the behavior, why it is the way it
is, and what (if anything) would change it.

## Linux: disabled menu items swallow their keyboard accelerator

`Session::try_menubar_accel` (`hosts/crossplatform/host.cpp`) skips a menu item
whose static `enabled` flag is false (`if (!mi.enabled) continue;`), so the
item's keyboard accelerator does nothing while it is disabled. This is the
intended behavior (a grayed-out item shouldn't fire from the keyboard either),
but note two things:

- It only affects the **Linux X11 (xpl) host** - the one existing host that
  routes accelerators through `try_menubar_accel`. win32 (HACCEL) and macOS
  (`NSMenuItem.keyEquivalent`) gate disabled-item accelerators in the OS, so
  they already behaved this way; Linux now matches them.
- The check consults only the **static** `mi.enabled` flag, not the dynamic
  `WidgetData::can_perform_command` / client `neui_menu_client_t::validate`
  gating that the menu uses to gray items at popup-open time. So on Linux an
  accelerator can stay live for an item that *would* gray out on popup (or vice
  versa) when enablement is driven purely by `validate`. Folding the dynamic
  gate into the accelerator path would close the gap if it ever matters.

## iPadOS: the hamburger menu button is always shown when a frame has a MENUBAR

On iOS a frame with a `NEUI_W_MENUBAR` always shows the in-app hamburger menu
button (`menu_ios_hamburger_should_hide` returns false unconditionally), even on
iPadOS 26+ where the system menu bar (swipe-from-top) exists.

WHY: the iPadOS 26 system menu bar is only revealable in **windowed /
Stage-Manager** mode - in true full-screen there is no top edge to swipe it down
from, so a full-screen app has the menu-bar *capability* but no way to reach it
and still needs the hamburger. Distinguishing windowed from full-screen has no
reliable public API: in Stage Manager both `UIScreen.bounds` and
`UIScreen.nativeBounds` track the (shrunken) workspace, so every window can read
as full-screen and a "hide on iPad-26" rule never fires correctly. Rather than
ship detection that doesn't hold, the hamburger is the universal,
always-reachable menu on every device/mode; when the system menu bar *is*
revealable (windowed), it coexists as an additive bonus - the same menu model is
contributed to it via `-buildMenuWithBuilder:`, so both surfaces stay in sync.

Consequence: on a windowed iPadOS 26 app the menu is reachable two ways (system
bar **and** hamburger). That redundancy is deliberate and harmless - the
heuristic only ever errs toward showing the hamburger, never toward stranding
the menu. `menu_ios_window_is_fullscreen()` (`hosts/shared/ios/menu_ios.h`) is
kept but unused by the visibility rule, as a reference point should a reliable
windowed-vs-full-screen signal appear in a future iPadOS release.

## iOS/iPadOS: a frame binds to the foreground scene, not its originating scene

When a root frame is first shown, `Session::widget_show`
(`hosts/ios/window.mm`) binds its new `UIWindow` to whatever scene
`active_window_scene()` returns - the foreground-active `UIWindowScene` (or the
first connected window scene as a fallback). The session is never correlated
with the specific `UIWindowScene` whose `-scene:willConnectToSession:` triggered
the UI build; the scene is discovered on demand at show time and not stored.

For a single-scene app (the common iPhone / single-window case) this is always
correct. Under **iPad multi-window / Stage Manager**, where several
`UIWindowScene`s connect, it is not: a frame shown while a *different* scene is
foreground-active will bind to that foreground scene rather than its own. The
host is effectively single-scene today.

Proper per-scene routing would thread the connecting `UIWindowScene` from the
scene delegate down through session creation / `widget_show` (e.g. a
session-to-scene association set when the scene connects), so each frame binds
to the scene that owns it. Deferred until multi-window iPad support is a
requirement.
