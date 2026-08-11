<!-- neui reference. Extracted from CLAUDE.md - read when working on these topics. -->

## Accessibility (`NEUI_API_A11Y`)

The client seam is `include/neui/d/a11y.h`, and that header is the reference for
what to call and why. This file is about the parts a client cannot see: which
platform actually reads the declarations, how the tree is built, and — the point
of the first section — **how much of it has been verified.**

### Platform status, and what "unverified" means

| Platform | Provider | Verified? |
|---|---|---|
| macOS (xpl host) | NSAccessibility (`hosts/crossplatform/a11y_macos.mm`) | **Yes** — 94-check harness driving the real protocol, plus VoiceOver. |
| Windows (xpl host) | UI Automation (`hosts/crossplatform/a11y_win32.cpp`) | **NO. Never compiled, never run.** See below. |
| Linux (xpl host) | none | n/a — declarations are stored and nothing reads them. |
| iOS / null | none | n/a — same. |
| win32 / macOS **native** hosts | none | n/a — `NEUI_API_A11Y` is xpl-only, like `_TIMER` / `_POINTER` / `_EMBED`. |

**The Windows provider ships unverified, deliberately and with the risk stated.**
It was written on a machine that cannot compile it, let alone run Narrator or
Inspect.exe against it. Nobody should rely on Windows accessibility working until
someone has actually looked. What stands in for execution:

- The **node tree, the adapter and the cache-invalidation scheme are shared with
  macOS**, where they are verified against VoiceOver. Be precise about what that
  buys: the shared *model* is verified, but each provider re-implements the same
  decisions in its own glue, and a review of the first cut found **two of the nine
  macOS defects re-broken in the win32 glue** (the notify path rebuilding the tree
  on every event; the root omitting unparented children). Sharing a design is not
  inheriting the fixes.
- The **role / pattern / state mapping tables are portable and Tier-1 tested**
  (`hosts/shared/a11y_uia_map.h`, `tests/test_a11y_uia_map.cpp`) on every
  platform. That is where a provider is most likely to be wrong, so it is the
  part that was deliberately made testable.
- Every **UIA constant is `static_assert`ed against the real SDK headers** in
  `a11y_win32.cpp`. A duplicated id that does not match what Windows uses fails
  the *build* rather than quietly announcing the wrong control type. This is the
  check that matters most, and it only fires on the first Windows build.
- A **parse check** (`tests/parse_check/run.sh`) compiles the file against
  hand-written stubs under `-Wall -Wextra`. Every COM method carries `override`,
  so all ~40 signatures are checked — but only against **my own transcription** of
  the UIA headers, not against the SDK. It has already caught real mistakes
  (undeclared symbols, a duplicated member) and it is blind to whole classes of
  bug: the worst defect in the first cut (`get_IsReadOnly` serving two patterns
  with one answer) is perfectly-formed C++. The stubs are also demonstrably not an
  oracle — the first version of them had `ProviderOptions_ServerSideProvider` set
  to the wrong value. Treat it as a typo-and-signature net, nothing more.

None of that is a substitute for running it. **Expect real bugs on first run**,
and treat "run Inspect.exe against a neui window" as the first task of the next
Windows session, before anything is built on top.

### How the tree is built

The xpl host paints one native surface per FRAME, so the OS sees a window as a
single opaque rectangle and the accessibility tree has to be *synthesised*
whatever the platform provider API looks like. That happens in three layers:

1. **`hosts/crossplatform/a11y_adapter.cpp`** walks one frame's live widget tree
   and emits flat `A11yInput` rows — including sub-element rows for things that
   are paint state rather than widgets (LISTBOX / COMBOBOX rows, TREEVIEW items,
   GRID headers / rows / cells, TABVIEW chips, menu items). A 10000×8 grid is one
   widget by design, so containers emit only the **visible window** of children
   plus the true totals.
2. **`hosts/shared/a11y_tree.h`** turns those rows into an ordered, parented,
   pruned tree: roles (declared beats derived), names (declared > `labelled_by` >
   text), states, and re-parenting so pruning a decorative middle node does not
   orphan its children. No host types, fully Tier-1 tested.
3. **The platform provider** publishes that tree. Per frame, built lazily on
   query behind `Session::a11y_revision()` — **paint is the invalidation
   signal**, because anything that changes what the window shows also repaints
   it. Focus, structural mutations and a client's `notify()` bump it explicitly.

**Node identity** is not a widget id: ids carry a per-widget-**instance**
generation from a process-wide counter, because tree slots are recycled and both
UIA and NSAccessibility hold element references for a long time. A stale
reference resolves to *nothing* rather than to whatever widget took the slot.

### What is not implemented

Recorded in full in `docs/deferred-issues.md`; the headlines:

- **No Linux provider.** A neui window is opaque to Orca. AT-SPI is its own wave.
- **No text interfaces** on any platform: an AT reads a text field's value but
  cannot navigate it by character or word, and cannot edit through the provider.
- **No table / grid patterns**, so a GRID does not advertise its true row count
  as a set size.
- **Menu items and an open COMBOBOX's drop rows decline activation** — their
  input is hit-tested at frame level by the platform layer rather than by the
  owning widget, so a synthesised click would land nowhere. Both stay fully
  keyboard-operable, and declining beats offering an action that does nothing.
  (On win32 the first cut *advertised* those actions and then refused them; the
  pattern set and the action gate now come from one predicate so they cannot
  disagree.)
- **A UIA client cannot write a CUSTOMDRAW's value.** For a built-in KNOB / SLIDER
  the host owns `NEUI_PARAM_VALUE` and `RangeValue::SetValue` writes it (raising
  the same gesture triple a drag does). For a client-declared CUSTOMDRAW the value
  lives in the client's own state, so RangeValue reports itself **read-only** there
  rather than accepting a write it cannot honour — and unlike macOS there is no
  arrow-key step fallback on win32, because UIA drives sliders through SetValue.
- **Selection changes are reported on the container, not the item.** The notify
  seam carries a widget id, so a row's own element is not addressable from it;
  win32 raises `Selection_Invalidated` on the list / tree / grid ("selection in me
  changed, go look") rather than `ElementSelected` naming the wrong element.
- Editing text raises no change notification (the value read afterwards is
  correct, but nothing prompts an AT to re-read).

### For a client author

The short version, in priority order:

1. If your UI is built from `CUSTOMDRAW` widgets — as most plugin UIs are —
   **declare their roles**. Nothing else here comes close in value. A declared
   role also makes the AT offer that role's *actions*, and how they reach you
   differs by platform: on **macOS** a press arrives as `NEUI_KEY_SPACE` and a
   step as an arrow key on your widget, so handle those keys or the action is
   offered and does nothing; on **win32** a press arrives the same way, but a
   value *write* comes through UIA's `RangeValue::SetValue`, which the provider
   only honours for built-in KNOB / SLIDER — a declared CUSTOMDRAW slider is
   reported read-only rather than being offered an adjustment it cannot apply.
2. Call `set_value_range` on anything with a normalized value. "Minus six
   decibels" is useful; "zero point four two" is not.
3. Pair your labels with `set_labelled_by`. The framework cannot see that a
   LABEL next to an INPUTBOX names it, and guessing from proximity would be
   wrong often enough to be worse than nothing.

`neui_get_api(NULL)` returns the **native** host first on Windows and macOS, and
the native hosts do not implement this interface — so feature-detect, or ask for
`"neui.host.crossplatform"` explicitly.
