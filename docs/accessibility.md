<!-- neui reference. Extracted from CLAUDE.md - read when working on these topics. -->

## Accessibility (`NEUI_API_A11Y`)

The client seam is `include/neui/d/a11y.h`, and that header is the reference for
what to call and why. This file is about the parts a client cannot see: which
platform actually reads the declarations, how the tree is built, and — the point
of the first section — **how much of it has been verified.**

### Platform status, and what "verified" means here

| Platform | Provider | Verified? |
|---|---|---|
| macOS (xpl host) | NSAccessibility (`hosts/crossplatform/a11y_macos.mm`) | **Yes** — 106-check harness driving the real protocol, plus VoiceOver. |
| Windows (xpl host) | UI Automation (`hosts/crossplatform/a11y_win32.cpp`) | **Yes, programmatically** (2026-08-11) — 57-check harness through the real UIA *client* stack. **No screen-reader pass yet.** See below. |
| Linux (xpl host) | none | n/a — declarations are stored and nothing reads them. |
| iOS / null | none | n/a — same. |
| win32 / macOS **native** hosts | none | n/a — `NEUI_API_A11Y` is xpl-only, like `_TIMER` / `_POINTER` / `_EMBED`. |

**The Windows provider shipped unverified and has since been run.** It was written
on a machine that could not compile it; the first Windows session compiled it,
executed it, and found one real defect. What that session established, and what it
did *not*, both matter:

- **It runs, through the real UIA client stack.**
  `tests/a11y_provider_smoke_win32.cpp` drives every query via `CUIAutomation` —
  the same client API Narrator uses — with the client on its own **MTA thread**
  while the UI thread pumps. That arrangement is the point: the provider declares
  `ProviderOptions_UseComThreading` precisely so UIA cannot reach the widget tree
  from its own threads, and a client living on the UI thread would exercise the one
  arrangement no assistive technology ever produces.
- **All five risk areas the provider was shipped with hold up**: element lifetime
  across a frame destroy, `Navigate` sibling walks cross-checked against `FindAll`
  (membership *and* order), `UseComThreading`, the `UiaRaiseNotificationEvent`
  version guard, and `nullptr` from `ElementProviderFromPoint` for the frame itself
  (UIA does fall back to the host provider, as hoped).
- **The `static_assert`s on ~40 UIA constants pass against the real SDK.** This was
  named as the highest-value safeguard precisely because a test can only confirm
  the number someone wrote down, not that it is the number Windows uses. It fires
  only on a Windows build, so this was its first exercise.
- **Geometry is cross-validated by an independent witness.** A reported screen
  rectangle is confirmed by posting a real click at its centre and asserting the
  *production* hit-test names the same widget — a rect and an a11y hit-test drawn
  from one cache agree with each other even when both are wrong.
- **One real defect, exactly where the header predicted it** — in the hand-written
  glue, not anywhere the shared model reaches. `range_of()` read the value from
  `NEUI_PARAM_VALUE` and never consulted `NEUI_ATTR_A11Y_VALUE`, so a CUSTOMDRAW
  holding its value in client state — the one case `a11y->set_value` exists to
  serve — reported 0.0: a declared 0.6 of −60..0 dB was announced as −60 dB. It was
  invisible to a native SLIDER, whose value really does live in the attribute bag,
  which is why nothing else had caught it. Fixed by mirroring the adapter's
  precedence (`read_declarations`); the node cannot supply the number, because the
  portable model keeps only the *formatted* string.

**What is still not verified: whether announcements are sensible.** No screen
reader and no Inspect.exe session has looked at a neui window. An automated client
can prove the tree, the roles, the names, the geometry and the actions are what the
code intends; it cannot tell you that Narrator reads a knob in a way a blind user
would want, or that the reading order is comfortable. macOS had that pass
(VoiceOver on `neui_a11y_example`); Windows has not. Treat **Narrator on
`neui_a11y_example`** as the remaining task, and note it is a judgement call, not a
pass/fail.

The safeguards that stood in for execution are still in place and still earn their
keep for anyone authoring on a non-Windows machine — with one correction worth
keeping visible:

- The **node tree, the adapter and the cache-invalidation scheme are shared with
  macOS**. Be precise about what that buys: the shared *model* is verified, but
  each provider re-implements the same decisions in its own glue, and a review of
  the first cut found **two of the nine macOS defects re-broken in the win32 glue**
  (the notify path rebuilding the tree on every event; the root omitting
  unparented children). Sharing a design is not inheriting the fixes — and the one
  defect that survived to the first run was in the glue too.
- The **role / pattern / state mapping tables are portable and Tier-1 tested**
  (`hosts/shared/a11y_uia_map.h`, `tests/test_a11y_uia_map.cpp`) on every
  platform. That is where a provider is most likely to be wrong, so it is the
  part that was deliberately made testable.
- A **parse check** (`tests/parse_check/run.sh`) compiles the file against
  hand-written stubs under `-Wall -Wextra`. Every COM method carries `override`,
  so all ~40 signatures are checked — but only against a **hand transcription** of
  the UIA headers, not against the SDK. It caught real mistakes (undeclared
  symbols, a duplicated member) and it is blind to whole classes of bug: the worst
  defect in the first cut (`get_IsReadOnly` serving two patterns with one answer)
  is perfectly-formed C++, and so was the `range_of` defect the first run found.
  The stubs are also demonstrably not an oracle — the first version had
  `ProviderOptions_ServerSideProvider` set to the wrong value. It remains a
  typo-and-signature net for editing this file from macOS or Linux, nothing more.

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
