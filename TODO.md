# TODO

Running list of open work. Items are terse; design rationale lives in
`CLAUDE.md` or in `plans/` files. Shipped work is tracked in git, not
here.

## Host parity (win32 ↔ macOS)

The win32 and macOS native hosts are at **functional parity**. Shipped
on both: event dispatch (full mouse / key / focus / resize), CUSTOMDRAW
mouse + keyboard, `set_focus` + Tab traversal (creation order), layout
(`set_pos` / `set_size` / `hide` / tree traversal), blocking
`popup_menu` + KNOB right-click reset, routed commands +
`tree->set_menu_cmd`, clipboard text + item API, `NEUI_W_IMAGE` via the
shared asset manager, enabled/disabled, and fonts (cg backend font
stack + native `NSFont`). Two small macOS-only divergences remain:

- **KNOB is not a keyboard tab-stop on macOS.** On win32 a KNOB carries
  `WS_TABSTOP`; on macOS its `NSView` refuses first responder, so Tab
  skips it (CUSTOMDRAW is a tab-stop on both). Closing it needs the KNOB
  made focusable + arrow-key value handling.
- **macOS Tab participation follows the system "Full Keyboard Access"
  setting.** The traversal *order* matches win32 (widget-creation
  order), but *which* control types Tab visits beyond text fields /
  lists is OS policy - buttons / checkboxes / sliders join only when
  Full Keyboard Access is on, whereas win32 always visits every
  `WS_TABSTOP`. Hand-roll Tab (like the xpl host's `focus_next`) if
  fully deterministic cross-platform traversal is required.

(Remaining cross-platform gaps below - native blocking modal, custom
clipboard formats, Tier B accessibility - are deferred symmetrically on
*both* hosts, not macOS-only.)

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
  Format specs in templates (`{key:.2f}`). Asset-layer `tint`
  (ARGB multiplier) reserved in docs but not yet wired through the
  backends.
- **Behavior follow-ups.** Declarative interactivity v1 shipped as
  `NEUI_ASSET_KIND_BEHAVIOR` (drag V/H/rotational/biaxial, wheel,
  key step, click toggle/cycle, context reset) - one asset bundles
  multiple handlers + reuses compound's 9-pt anchor system for
  per-region hit zones. Open extensions:
  - **Detent / plateau modifier** on drag handlers. `set_string(h,
    "detents", "0.5:0.02:0.04")` (`value:pull_radius:release`); sticky
    values that resist near specific points without quantizing
    everywhere. Distinct from `steps`. Slot it into
    `behavior_runtime.h` between range-clamp and step-snap.
  - **Pen pressure / tilt** through the dispatch (needs the platform
    layer to surface `WM_POINTER*` / `NSEventTypePressure` first; the
    dispatch is additive when the data lands).
  - **Cursor hint** on handlers - `cursor` string prop is parsed but
    no-op in v1. Needs a public `set_cursor` seam first.
  - **`bind`-style attr indirection on handler props** (min/max as
    bound attrs). v1 takes static numbers.
  - **WHEEL modifier-fine.** `neui_event_wheel_t` carries no
    `buttonmap` today, so `fine_modifier` is a no-op on WHEEL even
    though it works on drag + key. Add modifier bits to the wheel
    event payload (or factor wheel through the mouse struct) before
    wiring the fine path in `behavior_runtime.h`.

## Widgets

- **Native blocking modal** (Cocoa `runModalForWindow`, Win32
  `DialogBoxIndirect`). Current non-blocking modal matches the
  event-loop shape; revisit only if a real use case demands it.

## Grid sort follow-ups

Multi-column sort shipped (`plans/grid-sorting.md`); a handful of
extensions are deferred:

- **Locale-aware STRING compare.** v1 uses byte-level `strcmp`. Switch
  to ICU `ucol_strcoll` or `std::collate` once a client needs
  locale-correct sorting (accented characters, German Umlauts, etc.).
- **Custom per-column comparator callback.** v1 covers STRING / INT /
  FLOAT / NATURAL; a `set_column_sort_compare(col, fn, userdata)` hook
  would let clients sort by an external date / version / opaque-tag
  scheme. Slot lifetime + ABI shape needs thought before exposing.
- **Built-in DATE / TIME kind.** Common enough that we'll regret asking
  clients to NATURAL-sort ISO-8601 strings forever; defer until the
  parser policy (epoch / locale / formats accepted) is decided.

## Clipboard / drag&drop

`text/plain`, `text/html`, `text/uri-list`, and arbitrary MIME passthrough
are wired for both clipboard and drag&drop drop-target. See
`plans/clipboard-and-dnd.md` for the full handoff. Remaining:

- **Image formats.** `image/png` (or any bitmap MIME) is not yet on the
  clipboard / DnD path. Needs Win32 `CF_DIBV5` ↔ PNG decode and macOS
  `NSPasteboardTypePNG` / `NSPasteboardTypeTIFF` wrap. The asset API
  already loads PNG bytes (`hosts/shared/win32/image_loader_win32.h`,
  `hosts/shared/macos/image_loader_macos.h`) - the clipboard converters
  can share that decode path.
- **Drag source.** Drop-target side ships on all three hosts; the
  next phase is letting widgets initiate drags from inside the app
  (Win32 `DoDragDrop` + `IDataObject` wrapping a `DataItem`, macOS
  `beginDraggingSessionWithItems:` + `<NSDraggingSource>`). API shape
  will likely be `dnd->begin_drag(session, source_widget, payload,
  allowed_actions)` plus (deferred) a behavior-asset handler kind.
- **Per-child-widget DnD on macOS native.** `NEUINativeContentView`
  dispatches drops at frame level; per-painted-view `<NSDraggingDestination>`
  opt-in is deferred. Win32 native + xpl already walk the widget tree.

## Accessibility / IME

- **Tier B native focus parity.** Real focus-proxy HWNDs per widget on
  win32; NSAccessibility seam on macOS. Defer until UIAutomation /
  VoiceOver work needs it - clients see only logical focus today.

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
| `plans/grid-macos-port.md` | Shipped (GRID macOS native port). |
| `plans/grid-sorting.md` | Shipped (multi-column sort + per-column kind/sortable + visual nav). |
| `plans/clipboard-and-dnd.md` | Shipped (unified data-item, multi-format clipboard, DnD drop targets). Drag source is the next phase. |
| `plans/winui3-host.md` | Feasibility analysis; deferred indefinitely. |
| `plans/wasm-host.md` | Feasibility analysis; deferred. |
| `plans/how-to-port.md` | Reference playbook for new platform ports. |
