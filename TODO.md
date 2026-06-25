# TODO

Running list of open work. Items are terse; design rationale lives in
`CLAUDE.md` or in `plans/` files. Shipped work is tracked in git, not
here. Accepted behavioral quirks that are *not* work items live in
`known_issues.md`.

## Host parity (win32 ↔ macOS)

The win32 and macOS native hosts are at **functional parity**. Shipped
on both: event dispatch (full mouse / key / focus / resize), CUSTOMDRAW
mouse + keyboard, `set_focus` + Tab traversal (creation order), layout
(`set_pos` / `set_size` / `hide` / tree traversal), blocking
`popup_menu` + KNOB right-click reset, routed commands +
`tree->set_menu_cmd`, clipboard text + item API, `NEUI_W_IMAGE` via the
shared asset manager, enabled/disabled (including macOS via
`apply_enabled_native_macos`), fonts (cg backend font stack + native
`NSFont`), drag-source + drop-target with custom preview image,
per-widget DnD hit-testing in software so child widgets receive ENTER /
MOVE / LEAVE / DROP on every host, native blocking modal dialogs.

Two small macOS-only divergences remain:

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

(Remaining cross-platform gaps below - Tier B accessibility, image
clipboard formats, lazy DnD promises - are deferred symmetrically on
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
- **Compound layer kinds beyond v1.** `text` / `asset` / `rect` /
  `path` / `qr` / `group` shipped (`group` = a nested transform+clip
  scope holding a tree of child layers added via `add_child_layer`; the
  group is one layer at the widget level, child `z` orders only within
  it, children read the same widget AttrBag). Remaining: template format
  specs (`{key:.2f}` reserved but parser not implemented -
  `include/neui/d/compound.h:36`), SVG-mini string form for `set_path`.
- **Behavior follow-ups.** Declarative interactivity v1 shipped as
  `NEUI_ASSET_KIND_BEHAVIOR` (drag V/H/rotational/biaxial, wheel,
  key step, click toggle/cycle, context reset, drag-source) - one
  asset bundles multiple handlers + reuses compound's 9-pt anchor
  system for per-region hit zones. Open extensions:
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

## Grid sort follow-ups

Multi-column sort shipped; a handful of extensions are deferred:

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
are wired for both clipboard and drag&drop (drop-target **and**
drag-source on all three hosts, with custom drag-preview image on
macOS + Win32). Shipped — design rationale in `docs/design-notes.md`.
Remaining:

- **`image/tiff` (and other bitmap MIMEs).** `image/png` ships on all
  three hosts (Win32 publishes as `CF_DIBV5` alongside the registered
  MIME via WIC; macOS stamps `NSPasteboardTypePNG`); TIFF / JPEG /
  WebP would follow the same shape but are deferred until a concrete
  need surfaces. Helpers live in
  `hosts/shared/win32/clipboard_format_png_win32.h`.
- **File-promise interop on drag-source.** The generic
  `item_set_format_callback` provider lands lazily for arbitrary MIMEs
  on Win32 `IDataObject::GetData` and macOS `NSPasteboardItemDataProvider`,
  but Explorer / Outlook / Finder look specifically for
  `CFSTR_FILECONTENTS` + `CFSTR_FILEDESCRIPTOR` (Win32) and
  `NSFilePromiseProvider` (macOS). These map a lazy MIME to "promise of
  a file on disk" semantics. Wire on top of the generic provider so
  file-export-on-drop interops with native shells.
- **True deferred-render on clipboard write.** Lazy MIMEs on
  `clipboard->write` materialise at write time today (one provider
  invocation, bytes eagerly placed on the OS clipboard). Lifting to
  WM_RENDERFORMAT (needs a persistent owner HWND) on Win32 and
  `NSPasteboardItem` setDataProvider: on macOS would defer encoding
  until the receiver actually reads the clipboard. Deferred until a
  real use case justifies the WM_RENDERFORMAT plumbing.

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
- **WASM host.** WebAssembly target via Canvas-2D backend - see
  `plans/wasm-host.md`. Deferred; phased path documented.
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

Only open/deferred plans live in `plans/`. Completed plans were removed once
shipped — their full text is in git history, and the distilled design rationale
for shipped features lives in `docs/design-notes.md`.

| File | Status |
|---|---|
| `plans/win32-pointer-and-directmanipulation.md` | Deferred. WM_POINTER pen/touch + DirectManipulation smooth-scroll; binding spec for when it lands. |
| `plans/winui3-host.md` | Feasibility analysis; deferred indefinitely. |
| `plans/wasm-host.md` | Feasibility analysis; deferred. |
| `plans/lvgl-port.md` | Feasibility investigation (neui-on-LVGL); no implementation proposed. |
| `plans/how-to-port.md` | Reference playbook for new platform ports. |
