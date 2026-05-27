# TODO

Running list of open work. Items are terse; design rationale lives in
`CLAUDE.md` or in `plans/` files. Shipped work is tracked in git, not
here.

## Audio-plugin / drawable framework

Path toward asset-driven, fully skinnable plugin controls. Each entry
is a real initiative, not trivial; together they form one programme.

- **`describe_params` introspection API.** `size_t describe_params(s,
  w, neui_param_info_t* out, size_t max)` returning `{ name, label,
  default }` per slot. Lets generic tooling (parameter inspector,
  MIDI-learn UI, automation routing, state save/restore) walk a
  widget's parameter surface without hard-coded knowledge of widget
  type. Becomes mandatory once multi-value widgets land.
- **Multi-value widgets.** ADSR (4 normalised slots), N-band EQ,
  multi-stage envelope (DAHDSR), XY pad, modulation matrix. Each is
  a consumer of the named-slot system + introspection above; not
  worth designing the slot system without them, not worth shipping
  them without it. Build the abstraction alongside the first one.
- **Compound layer kinds beyond v1.** `rect` / `path` / `group` (the
  last unlocks nested transform scopes for a tree of layers).
  Format specs in templates (`{key:.2f}`). Declarative interactivity
  (per-layer hit targets, drag-to-attr bindings) - v1 routes input
  through CUSTOMDRAW's normal MOUSE_* path. Asset-layer `tint`
  (ARGB multiplier) reserved in docs but not yet wired through the
  backends.

## Widgets

- **`widgets->set_enabled` on macOS native.** Win32 + xpl shipped;
  macOS host stores the flag but does not yet drive
  `[NSControl setEnabled:]` or dim the painted-view subset. One-pager
  follow-up: per-type branch in `w_set_enabled` mirroring the existing
  `w_set_text` / `w_set_check` shape.
- **`NEUI_W_IMAGE` on the native macOS host** still loads via
  `[view ensureImageBitmap:]`. The `MacOSAssetManager` and
  `macos_painter_draw_asset_thunk` already exist (used by CUSTOMDRAW
  + compound); migrating IMAGE to `set_asset` is a one-pager
  follow-on - re-point the existing view code at the shared asset
  manager.
- **Native blocking modal** (Cocoa `runModalForWindow`, Win32
  `DialogBoxIndirect`). Current non-blocking modal matches the
  event-loop shape; revisit only if a real use case demands it.

## Clipboard

- **Custom clipboard formats.** v1 only round-trips `text/plain`. API
  shape is forward-compatible. HTML / PNG / image are the obvious
  next kinds; each needs a Win32 + macOS converter plus the per-host
  read/write seam.

## Accessibility / IME

- **Tier B native focus parity.** Real focus-proxy HWNDs per widget on
  win32; NSAccessibility seam on macOS. Defer until UIAutomation /
  VoiceOver work needs it - clients see only logical focus today.
- **`NSPasteboardChanged` listener** on macOS, if Apple ever ships
  one. Current change-count poll is cheap but not event-driven.

## Hosts

- **WinUI3 host.** Third host backend - see `plans/winui3-host.md`.
  Re-evaluate when (a) UIAutomation accessibility cert becomes a hard
  requirement, (b) Microsoft ships first-class CMake support for
  Windows App SDK, or (c) a user specifically asks for Fluent
  Design.
- **Multi-level redo on win32 native.** `NEUI_CMD_REDO` maps to
  `EM_UNDO` (single-level toggle). Clients that need multi-level
  redo should select the xpl host's text widgets (full `EditHistory`).
- **Other platform ports** (Linux/X11, Linux/Wayland, iOS, Android,
  embedded). Playbook in `plans/how-to-port.md`.

## Theme

- **Multi-session palette correctness.** `active_palette_override_ptr`
  is process-wide (last-set-wins). Fine for single-session and
  for multi-session apps where every session uses the same mode.
  Replace the global with TLS or per-session-thread routing if a
  real conflicting-mode multi-session use case shows up.

## Identity / lifetimes

- **Widget id stale-after-slot-reuse detection.** No generation
  counter today; a saved handle that survives `widget_destroy` can
  silently bind to a new widget that reused the slot. Same applies
  to `neui_asset_t` and `neui_compound_layer_t`. Add a small
  generation field if a real bug surfaces; until then deferred.

## Plans

| File | Status |
|---|---|
| `plans/painter-and-asset-api.md` | Shipped; kept for the design-decision rationale. |
| `plans/winui3-host.md` | Feasibility analysis; deferred indefinitely. |
| `plans/how-to-port.md` | Reference playbook for new platform ports. |
