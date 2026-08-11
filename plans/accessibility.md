# Accessibility (`NEUI_API_A11Y`) — implementation plan

Status: **6.0 + 6.1 shipped** (see those sections); 6.2 onward not started. All five open decisions were resolved
on 2026-08-11 (§7), so implementation is unblocked; build order at the end of §8.
This is Wave 6 of
`plans/sst-neuigui-gap-response.md` (its §3 sketch, lines 991-1027), worked out
to the point where it can be built. Where this plan contradicts that sketch, this
plan is the newer decision and the differences are called out in §9.

Revision 2 — a review of revision 1 found that its central artifact (the input
struct feeding the shared model) was underspecified to the point where the phase
built on it could not be implemented, and that node identity ignored widget-slot
reuse. Both are fixed here; §10 records what changed and why, so the review's
findings are not silently absorbed.

Related: `docs/deferred-issues.md:10-12` (KNOB tab-stop on macOS native, macOS
Full Keyboard Access, "Tier B focus parity"). **Only the third retires with this
wave** — see §6.6.

---

## 1. What we are building, and why here

neui has **no accessibility story at all** today: no `WM_GETOBJECT`, no
`IAccessible`/UIA provider, no `NSAccessibility` overrides, no AT-SPI, and no
client-facing API. A screen reader sees a neui window as one opaque rectangle.
For the sst-jucegui use case this is a hard blocker on shipping a plugin UI into
an accessible host.

**The architectural decision that shapes everything else:** the xpl host paints
**one native surface per FRAME** (`WidgetData::native_handle` is non-null only on
APPWINDOW / PLUGWINDOW / DIALOG — `hosts/crossplatform/host.h:81-82`). There is
no native view per widget, so the platform gives us nothing per widget and the
provider tree has to be **synthetic no matter what granularity we pick**.

That is good news for scoping. It means:

- Build it **once, in the xpl host**, and one tree walk serves win32 + macOS +
  Linux.
- A large, bug-prone part (role derivation, ordering, pruning, name resolution,
  offscreen marking, hit-test, value formatting) is **portable C++** and
  therefore Tier-1 testable with no platform code.
- Only three thin platform shims remain, each of which does nothing but answer
  queries from the shared model.

It also means the native win32/macOS hosts get **nothing** from this wave. That
is deliberate and consistent with Wave 3 (UI scale, xpl-only): the native hosts
exist for standalone-app parity, and their per-widget native controls already
carry whatever the OS gives them for free.

### Definition of done

1. A client can declare role / name / description / value / state per widget, and
   a client that declares **nothing** still gets a usable tree from type-derived
   defaults.
2. VoiceOver on macOS can navigate an xpl frame, announce each control with a
   sensible role and name, read a knob's value as a human-readable string, and
   follow focus as Tab moves.
3. The same tree is exposed to UIA on Windows. Linux/AT-SPI is explicitly OUT of
   this wave (§7 decision 1) and Linux therefore has no provider yet.
4. Focus is deterministic and per-frame.
5. Zero measurable cost when no assistive technology is attached.

---

## 2. Substrate audit — what exists, what is missing

### Already in place (verified against `bp-review`)

| Need | Where it lives |
|---|---|
| Per-widget geometry | `WidgetData::x/y/width/height` + frame-local `abs_x/abs_y` (`host.h:43-53`) |
| Focusability + real traversal | `tab_stop` (`host.h:57`), `collect_tab_stops` + `focus_next` (`host.cpp:2657-2700`) |
| Focus change hook | `Session::set_focus` (`host.cpp:874`) fires `WIDGET_FOCUS` both ways (gated on `emit_events`) |
| Enabled / visible | `WidgetData::visible` (`host.h:55`), `::enabled` (`:61`) |
| Hit-testing | virtual `WidgetData::hit_test` (`host.h:170`) |
| Type identity | `WidgetData::type` vs the `NEUI_W_*` strings (`include/neui/d/widgets.h:218-242`) |
| Continuous value | `NEUI_PARAM_VALUE` (`attrs.h:499`), `NEUI_PARAM_DEFAULT` (`:503`) |
| Human-readable value string | `NEUI_ATTR_VALUE_TEXT` (`attrs.h:377`) — see the conflict in §4.4 |
| Check state | `CheckboxWidget::check_state` (`host.h:538`) |
| Selection | `ListItemsWidget::selected_item` (`host.h:593`), `TreeviewWidget::selected_tree_item` (`host.h:672`) |
| Expand state | `TreeviewWidget::TreeItem::expanded` (`host.h:664`), `flatten_visible()` (`:689`) |
| Read-only / password | `NEUI_ATTR_READONLY`, `NEUI_ATTR_PASSWORD` (`hosts/shared/attrs.h:55-56`) |
| Zoom + DPI conversion | `WidgetData::logical_to_physical()` (`host.h:126`) |
| Per-widget attr storage | `WidgetData::attrs` (`host.h:141`) |
| Layout offsets already computed each paint | `paint_widgets_recursive` (`host.cpp:2421-2500`): abs caching `:2432`, SECTION band + scroll `:2470-2479`, menubar inset `:2610-2617` |
| KNOB **already** a tab stop on xpl | `widgets.cpp:196` sets `tab_stop` for KNOB; `KnobWidget::on_keydown` at `host.cpp:3493` |

Role, name, value, state and bounds are therefore all **derivable** for the
built-in widget types. That is the cheap part of this wave.

### Gaps (G1-G10)

**G1 — `abs_x/abs_y` are only valid after the first paint.** Recomputed top-down
by `paint_widgets_recursive`; `host.h:46-53` documents that before the first
paint they read 0. Acceptable for hit-testing ("input cannot arrive before the
window is first painted"), **not** for accessibility: UIA probes a window on
creation, and an AT can be pointed at a never-painted or occluded frame. An AT
asking for bounds must not get `(0,0)` for everything. Fix in **6.0**.

**G2 — the a11y tree is not the widget tree.** Five widget types contain
sub-elements that are *model state*, not widgets:

- LISTBOX / COMBOBOX rows — `ListItemsWidget::items` (`host.h:592`)
- TREEVIEW items — `tree_items` + `flatten_visible()` (`host.h:670,689`)
- GRID rows / cells / headers — `GridModel` (`host.h:760`); a 10000×8 grid is
  deliberately *one* widget
- MENUBAR / POPUPMENU items — a `Tree<>` of items
- **TABVIEW tab chips** — `TabViewWidget::chips` (`host.h:412`), cached for
  hit-testing; the chip strip is painted by the widget, so a tab is not a widget

So a node is **not** identified by a widget id. See §4.3.

**G3 — focus is session-global and Tab crosses frames.** `_focused_widget` is one
value per session (`host.h:1257`) and `focus_next` collects tab stops from
**every root child**, i.e. every frame (`host.cpp:2676-2681`). Consequences:
(a) Tab can move focus between windows today, which is wrong independently of
accessibility; (b) each provider is rooted at one frame's native view and must
expose only that frame's subtree and focused element. Fix in **6.0**.

**G4 — bounds must reach the AT in screen coordinates.** `abs_x/abs_y` are
frame-local logical px at 96 DPI. UIA wants physical px; NSAccessibility wants
screen points with a **bottom-left origin** (Y flipped from our Y-down
convention); AT-SPI wants px. Zoom folds in via `logical_to_physical()`. The
shared model stays frame-local logical; one conversion helper per platform.

**G5 — no accessible name for a control labelled by a separate LABEL.** The
established neui idiom is a LABEL next to an INPUTBOX. There is no `label-for`
relationship, so the input would be announced unnamed. Needs an explicit client
call — inferring from proximity is a heuristic that will be wrong often enough to
be worse than nothing.

**G6 — CUSTOMDRAW is semantically empty, and it is the case that matters most.**
sst-jucegui's controls are *all* custom-painted (`host.h:721-751`). The same
widget type is a knob, a meter, a decorative background, and a whole composited
panel. See §4.5.

**G7 — a normalized value is useless to a screen reader.** KNOB/SLIDER carry
`NEUI_PARAM_VALUE` as normalized `[0..1]`. "Zero point four two" tells the user
nothing; "minus six decibels" does. See §4.4.

**G8 — overlay content paints outside its widget's rect, or has no rect at all.**
Three distinct cases, none expressible as "a widget's own rectangle":

- **COMBOBOX drop list** — `ComboBoxWidget::overlay_rect` (`host.h:653`) paints
  *below or above* the collapsed bar, outside the widget rect.
- **MENUBAR / POPUPMENU** — created 0×0, flagged `is_menu_model()`, skipped by
  both the paint walk and `collect_tab_stops`. Item geometry exists only while a
  cascade is open, in **session** overlay state, not on the widget. A naive
  "drop zero-size nodes" rule would delete the very nodes the role table
  promises.
- **Toasts and the neui-drawn message box** — `FrameWidget::toast`
  (`host.h:327`), `Session::toast_show` (`:1139`). Not widgets at all.

The first two need overlay geometry sourced from session state (6.2/6.3); the
third is not a tree problem but an **announcement** problem — a screen reader
expects transient surfaces to be spoken. See §4.8.

**G9 — widget ids are reused, so a node id is not automatically stable.** Widget
ids are `session << 16 | tree slot` and `CLAUDE.md` states plainly that
"stale-after-slot-reuse [is] not detected (deferred)". Both UIA and
NSAccessibility hold element references across long spans. Without a generation
counter, an AT-held element whose widget was destroyed and whose slot was reused
silently resolves to a **different live widget** and answers with the wrong
role/name/bounds — worse than answering "invalid". See §4.3.

**G10 — a virtualized container cannot be materialized eagerly.** A 10000-row
GRID expanded into rows + cells is ~90k nodes on every rebuild. Whatever the
model does here has to be decided once and honoured by all three providers, not
left as a per-provider optimisation. See §4.2.

---

## 3. Phases

Ordered by dependency, then by what is verifiable on the machine this gets built
on. **Deviation from the sketch: macOS lands before win32.** Two Windows build
breaks shipped in earlier waves of this same programme, both from code that could
not be compiled here; a provider is far more intricate than either. Doing the
platform we can run VoiceOver against first means the shared model's design gets
validated by a real AT before a second, unverifiable provider is written on it.

### 6.0 — prerequisites — **SHIPPED 2026-08-11**, with a design change

Implemented, but **not** the way this section proposed. Building it surfaced a
dependency the plan had missed, so the approach changed; the original text is
kept below for the record.

**What the plan missed:** it assumed the whole layout computation could be moved
out of the paint path into a non-painting helper. It cannot. `TabViewWidget::paint`
derives its body rect from chip widths measured with `backend->measure_text`
(`host.cpp:1665-1685`), so TABVIEW layout is not reproducible without a live
render context — and `SectionWidget::paint` mutates scroll state as a side effect
of computing its layout (`host.cpp:1357-1365`). A second, non-painting
implementation of either would have been a drift hazard sitting under an
accessibility provider, which is the worst place for one.

**What shipped instead — a hybrid that keeps layout single-sourced:**

1. Only the **origin arithmetic** was extracted, into `child_origin_of`
   (`host.cpp`), and it *reads* the layout the paint pass cached rather than
   recomputing anything. `paint_widgets_recursive` and the new
   `refresh_abs_positions_recursive` both go through it, so the two walks cannot
   disagree by construction.
2. `Session::refresh_abs_positions(frame)` — the non-painting walk. No PREUPDATE
   dispatch, no drawing, no layout recomputation.
3. `Session::ensure_abs_positions(frame)` — the entry point for out-of-band
   positional queries. For the cold-start case (frame never painted, so the
   layout caches are empty) it forces **one synchronous paint** via a new
   `platform_force_paint` seam, then walks. This is the plan's own "fallback",
   promoted to the primary cold-start path: it reuses the real layout code
   instead of duplicating it, and Wave 5 already measured the cost at ~0.9 ms for
   a 100-widget frame, which is nothing for a once-per-frame-lifetime event.
   `platform_force_paint` is implemented on all five platform layers
   (macOS `-display`, win32 `RDW_UPDATENOW`, Linux direct `paint_window`, iOS
   degrades to invalidate, null no-op) and documented as best-effort — a hidden
   or unmapped window may legitimately do nothing, leaving cached geometry stale
   rather than wrong.
4. Per-frame focus traversal: `focus_next` now scopes to the frame owning the
   focused widget via a new `Session::frame_of`, falling back to the first
   visible frame.

**Verification:** `tests/focus_smoke_macos.mm` (new, 15 checks, two real
NSWindows) — and it was confirmed to *fail* (6 failures) against the old
cross-frame behaviour before being accepted, with the symptom "want focus on
A.button1, got B.button1". Repaint bench: **0.87 ms before and after in Release**
(the shared helper inlines away); Debug is ~2.7 % slower, which is the expected
cost of a non-inlined call in the descent path and does not matter. All 434
Tier-1 cases, ctest, and all seven macOS harnesses pass.

**Review follow-up (same day).** A review of the first cut found three real
defects, all fixed before push:

1. **The "first visible frame" fallback was really "first root child".** Every
   widget is created `visible = true` (`widgets.cpp:191`) and `hide()` on a
   *realized* frame deliberately leaves the flag alone (`widgets.cpp:403-406`),
   so `visible` does not distinguish shown frames from unshown ones. The fallback
   could therefore focus a control in a never-shown window, or — worse — pick a
   first root child with no tab stops (a splash frame of LABELs, or a POPUPMENU,
   which is `isroot` but not a window) and leave **Tab dead session-wide**. Also
   corrected: the claim that the OS-focused frame "does not exist yet" was wrong.
   Every `focus_next` call site already knows the frame that received the key, so
   `focus_next` now takes a `frame_hint` and all four platform layers pass it.
   The last-resort fallback tests `is_frame() && native_handle`, not `visible`.
2. **`painted_once` meant "painted once ever", not "layout caches valid".** A
   SECTION added *after* the last paint — post-show dynamic creation is a
   supported pattern — has an empty body-rect cache in a frame that has painted,
   so `ensure_abs_positions` would have skipped the force-paint and placed that
   section's children at the section origin. Added a `layout_dirty` flag set by
   `Session::mark_layout_dirty` from create / destroy / set_pos / set_size / hide
   and cleared by `paint_frame`.
3. **"Stale rather than wrong" was documented but not implemented.** When the
   forced paint was a no-op (hidden / unmapped / unrealized window),
   `ensure_abs_positions` still walked and *overwrote* good cached geometry with
   band-less values. It now returns `bool` and leaves the cache untouched when it
   cannot make the positions valid, so the caller must handle an unanswerable
   query rather than silently trusting wrong numbers.

Also fixed: two comments describing things that do not exist (a
`Session::abs_positions_valid` member; a claim that `get_all_parents`' last entry
is the root child — it is the root *sentinel*, and the loop works by skipping
it); an unbounded `while` in the harness that would have **hung** on exactly the
empty-tab-stop regression above instead of reporting it; and the harness's
coordinate check, which was near-vacuous because hit-testing and widget-local
conversion both derive from the same cache, so any *consistent* origin error kept
them agreeing. It now records the child's top edge and asserts the widget-local
y at that row; verified discriminating by breaking `child_origin_of`'s abs output
and watching it fail.

**Remaining known limitation:** `ensure_abs_positions` is documented as unsafe to
call during a paint (it can re-enter `paint_frame` on the same context) but
nothing enforces it. Harmless today — it has no callers — but a `WIDGET_PREUPDATE`
handler runs mid-paint, so **6.3 must add an in-paint guard** before the provider
becomes reachable from client code.

Original text follows.

### 6.0 — prerequisites (no public API change) — as originally planned

- **Factor the absolute-position walk out of the paint path.** Extract from
  `paint_widgets_recursive` (`host.cpp:2421-2500`) the part computing a child's
  frame-local origin — parent origin + `x/y`, SECTION band offset + scroll
  (`:2470-2479`), TABVIEW page visibility, menubar inset (`:2610-2617`) — into
  one helper called by both the paint walk and a new non-painting
  `Session::refresh_layout()`. Fixes **G1**, and incidentally hit-test-before-paint.
  - **The riskiest item in the wave**: it touches the paint hot path and the
    offset rules are currently spread through the walk. Pure refactor; run
    `tests/repaint_bench.cpp` before/after to show no regression, and re-run all
    six macOS harnesses.
  - Fallback if it proves invasive: force one synchronous paint before answering
    the first AT query. Correct but ugly (an AT query causes a repaint). Prefer
    the refactor; keep this in reserve.
- **Per-frame tab-stop collection.** Give `collect_tab_stops` a frame-root
  parameter; have `focus_next` use the frame owning `_focused_widget`, falling
  back to the frame with OS focus. Fixes **G3**(a). Needs its own harness check —
  two frames, Tab must not cross.

### 6.1 — client seam: `include/neui/d/a11y.h` — **SHIPPED 2026-08-11**

Landed as specified, with `set_value` included (the gap revision 1's review found)
and one addition the writing surfaced: **input validation belongs at the door, not
in the model.** `set_value_range` rejects `min == max` (it would divide by zero in
any normalized→real mapping downstream), `set_value` clamps out-of-range values
and turns NaN into 0 (an AT announcing "103 percent" is worse than a pinned
value), `set_state` masks the stored values so out-of-mask bits cannot become
silent noise, and `set_labelled_by` rejects a self-reference rather than leaving it
for the model's cycle guard. Every clearing path removes its key so the derived
default returns, instead of storing an empty value that would override the default
with nothing.

`notify` / `announce` / `is_active` are honest no-ops until 6.3: a client can write
its declarations today and they start being read when the provider exists, with no
client change. `is_active` returning false is correct, not merely conservative.

Verified by `tests/a11y_smoke_macos.mm` (new, 31 checks, no window needed) —
including that the interface is NULL on the **native** macOS host, which is the
documented trap since `neui_get_api(NULL)` returns the native host first there.

Original specification follows.

### 6.1 — client seam: `include/neui/d/a11y.h` — as originally planned

New public header, `NEUI_API_A11Y`, xpl-host-only — so it carries the same
feature-detect warning `timer.h` / `pointer.h` / `embed.h` do: on win32/macOS
`neui_get_api(NULL)` returns the **native** host first (`src/neui.c:65-77`),
which returns NULL for this interface.

```c
#define NEUI_API_A11Y "com.defiantnerd.neui.extension.a11y/0"

typedef enum neui_a11y_role_t {
  NEUI_A11Y_ROLE_DEFAULT = 0,   // derive from widget type (table below)
  NEUI_A11Y_ROLE_NONE,          // decorative: prune this node AND its subtree
  NEUI_A11Y_ROLE_GROUP,         // present, but only as a container
  NEUI_A11Y_ROLE_WINDOW,
  NEUI_A11Y_ROLE_STATIC_TEXT,
  NEUI_A11Y_ROLE_BUTTON,
  NEUI_A11Y_ROLE_TOGGLE_BUTTON,
  NEUI_A11Y_ROLE_CHECKBOX,
  NEUI_A11Y_ROLE_RADIO_BUTTON,
  NEUI_A11Y_ROLE_SLIDER,        // KNOB maps here too - no platform has "knob"
  NEUI_A11Y_ROLE_PROGRESS,
  NEUI_A11Y_ROLE_METER,
  NEUI_A11Y_ROLE_TEXT_FIELD,
  NEUI_A11Y_ROLE_TEXT_AREA,
  NEUI_A11Y_ROLE_LIST, NEUI_A11Y_ROLE_LIST_ITEM,
  NEUI_A11Y_ROLE_COMBOBOX,
  NEUI_A11Y_ROLE_TREE, NEUI_A11Y_ROLE_TREE_ITEM,
  NEUI_A11Y_ROLE_TABLE, NEUI_A11Y_ROLE_ROW, NEUI_A11Y_ROLE_CELL,
  NEUI_A11Y_ROLE_COLUMN_HEADER,
  NEUI_A11Y_ROLE_TAB_LIST, NEUI_A11Y_ROLE_TAB,
  NEUI_A11Y_ROLE_MENU_BAR, NEUI_A11Y_ROLE_MENU, NEUI_A11Y_ROLE_MENU_ITEM,
  NEUI_A11Y_ROLE_IMAGE,
  NEUI_A11Y_ROLE_SCROLL_AREA
} neui_a11y_role_t;

// State bits. A client sets/clears via (mask, values); bits it does not name in
// `mask` stay framework-derived.
#define NEUI_A11Y_STATE_DISABLED   0x0001u
#define NEUI_A11Y_STATE_FOCUSED    0x0002u
#define NEUI_A11Y_STATE_FOCUSABLE  0x0004u
#define NEUI_A11Y_STATE_SELECTED   0x0008u
#define NEUI_A11Y_STATE_EXPANDED   0x0010u
#define NEUI_A11Y_STATE_COLLAPSED  0x0020u
#define NEUI_A11Y_STATE_CHECKED    0x0040u
#define NEUI_A11Y_STATE_MIXED      0x0080u   // CHECKBOX3 indeterminate
#define NEUI_A11Y_STATE_READONLY   0x0100u
#define NEUI_A11Y_STATE_OFFSCREEN  0x0200u   // outside its scroll clip
#define NEUI_A11Y_STATE_PROTECTED  0x0400u   // password
#define NEUI_A11Y_STATE_MULTILINE  0x0800u

typedef enum neui_a11y_change_t {
  NEUI_A11Y_CHANGE_VALUE = 0,
  NEUI_A11Y_CHANGE_NAME,
  NEUI_A11Y_CHANGE_STATE,
  NEUI_A11Y_CHANGE_STRUCTURE,   // children added / removed / reordered
  NEUI_A11Y_CHANGE_SELECTION
} neui_a11y_change_t;

typedef struct neui_a11y_api {
  uint32_t neui_version;

  void (NEUI_ABI *set_role)(neui_session_t, neui_widget_t, neui_a11y_role_t);
  void (NEUI_ABI *set_name)(neui_session_t, neui_widget_t, const char* utf8);
  void (NEUI_ABI *set_description)(neui_session_t, neui_widget_t, const char* utf8);

  // Real-world range for a continuous control, so an AT can announce
  // "-6.0 dB" instead of "0.42". step 0 = continuous.
  void (NEUI_ABI *set_value_range)(neui_session_t, neui_widget_t,
                                   float min, float max, float step);
  // Normalized [0..1] current value for a control whose value lives in the
  // CLIENT's own state and so never passes through the attribute bag - i.e.
  // a hand-painted CUSTOMDRAW. Built-in KNOB / SLIDER need neither this nor
  // notify(): the framework reads NEUI_PARAM_VALUE directly.
  void (NEUI_ABI *set_value)(neui_session_t, neui_widget_t, float normalized);
  // Explicit display string. Highest-priority value source (see 4.4).
  void (NEUI_ABI *set_value_text)(neui_session_t, neui_widget_t, const char* utf8);

  void (NEUI_ABI *set_state)(neui_session_t, neui_widget_t,
                             uint32_t mask, uint32_t values);
  void (NEUI_ABI *set_labelled_by)(neui_session_t, neui_widget_t widget,
                                   neui_widget_t label);   // fixes G5

  // Tell the AT something changed. The framework already does this for every
  // built-in widget (focus, native value, selection, structure); a client only
  // needs it after set_value / set_name on a hand-painted widget.
  void (NEUI_ABI *notify)(neui_session_t, neui_widget_t, neui_a11y_change_t);

  // Speak a transient message that is not a tree node - the accessibility
  // counterpart of a toast. The framework calls this itself from toast_show
  // and the neui-drawn message box (G8); a client needs it only for its own
  // hand-drawn transient UI. `assertive` = interrupt current speech.
  void (NEUI_ABI *announce)(neui_session_t, const char* utf8, bool assertive);

  // True when an assistive technology has actually queried this session. Lets a
  // client skip building expensive display strings. Advisory: never gate
  // CORRECTNESS on it (see 4.6 - macOS cannot answer this eagerly).
  bool (NEUI_ABI *is_active)(neui_session_t);

  // Append new methods at the end (vtable-append evolution rule).
} neui_a11y_api_t;
```

Default role derivation (when the client declares nothing) — all 23 `NEUI_W_*`
types are covered:

| Widget type | Default role |
|---|---|
| APPWINDOW / PLUGWINDOW / DIALOG | `WINDOW` |
| LABEL | `STATIC_TEXT` |
| BUTTON | `BUTTON` |
| INPUTBOX | `TEXT_FIELD` (+ `PROTECTED` when `NEUI_ATTR_PASSWORD`) |
| MULTILINE | `TEXT_AREA` (+ `MULTILINE`) |
| CHECKBOX / CHECKBOX3 | `CHECKBOX` |
| LISTBOX | `LIST` + `LIST_ITEM` children |
| COMBOBOX | `COMBOBOX` + `LIST_ITEM` children (only while open, G8) |
| TREEVIEW | `TREE` + `TREE_ITEM` children |
| GRID | `TABLE` + `COLUMN_HEADER` / `ROW` / `CELL` (windowed, §4.2) |
| MENUBAR / POPUPMENU | `MENU_BAR` / `MENU` + `MENU_ITEM` children |
| SLIDER / KNOB | `SLIDER` |
| IMAGE | `IMAGE` |
| SECTION (non-scrolling) | `GROUP` |
| SECTION (scrolling) | `SCROLL_AREA` |
| TABVIEW | `TAB_LIST` + `TAB` children **from the chip strip** (G2) |
| TABPAGE | `GROUP` (see §4.7 for the non-selected-page rule) |
| **CUSTOMDRAW** | **`GROUP`** — §4.5 |

Storage: role / name / description / range / value / value-text / state-override /
labelled-by go in the existing per-widget `AttrBag` (`host.h:141`) under
`neui.a11y.*` keys, each with a `k_well_known_attrs` row per the house rule. No
new per-widget struct, no cost on widgets that declare nothing.

### 6.2 — the portable model: `hosts/shared/a11y_tree.h`

Header-only, `inline`, **no host types** — the contract `grid_model.h` and
`scrollbar.h` already honour (verified: they include only shared + public
headers), which is what makes it Tier-1 testable. So it does **not** walk
`Tree<WidgetData>` directly (the sketch said it would; that would drag the host
into the test binary).

**The input contract is one POD row per NODE CANDIDATE, not per widget.** This is
the correction that revision 1 got wrong: sub-elements are rows too, and each row
is fully self-describing. The host-side adapter does the extraction; the model
does the tree logic. Anything the model needs, the adapter has already resolved.

```cpp
enum class A11ySubKind : int32_t {
  widget = 0,  list_row, tree_item, grid_header, grid_row, grid_cell,
  tab_chip, menu_item
};

// Stable across rebuilds AND across slot reuse (G9): `generation` is the
// destroy counter for this widget's tree slot at the time the node was minted.
struct A11yNodeId {
  uint32_t widget_id  = 0;
  uint32_t generation = 0;
  int32_t  sub_kind   = 0;   // A11ySubKind
  int32_t  sub_index  = -1;  // -1 = the widget itself
};

struct A11yInput {
  A11yNodeId  id, parent;
  const char* type = nullptr;      // NEUI_W_* on widget rows; null on sub rows

  // Frame-local logical px, ALREADY including section band + scroll offsets and
  // overlay placement (the 6.0 walk and the overlay state supply these, so the
  // model never re-derives layout).
  int x = 0, y = 0, w = 0, h = 0;
  bool has_clip = false;                       // enclosing scroll / overlay clip
  int  clip_x = 0, clip_y = 0, clip_w = 0, clip_h = 0;

  // Framework-derived state
  bool visible = true, enabled = true, focused = false, tab_stop = false;
  bool selected = false, expanded = false, expandable = false;
  bool readonly = false, password = false, multiline = false;
  bool modal_blocked = false;                  // input-blocked dialog owner
  int  check_state = -1;                       // -1 = not checkable, else NEUI_CHECK_*

  // Text: widget text, row label, cell text, chip label, menu item label.
  const char* text = nullptr;

  // Value
  bool  has_value = false;  float value = 0.0f;   // normalized when has_range
  bool  has_range = false;  float vmin = 0, vmax = 1, vstep = 0;

  // Client declarations (from the AttrBag)
  int         declared_role = 0;                 // NEUI_A11Y_ROLE_DEFAULT = derive
  const char* name = nullptr;
  const char* description = nullptr;
  const char* value_text = nullptr;
  uint32_t    state_mask = 0, state_values = 0;
  A11yNodeId  labelled_by;

  // Virtualized container (G10): when the adapter emits only a window of
  // children, the container row reports the true totals so a provider can
  // advertise the real set size and index.
  int total_child_count = -1;                    // -1 = not virtualized
  int first_child_index = 0;
};

struct A11yNode {
  A11yNodeId  id, parent;
  std::vector<A11yNodeId> children;   // ordered
  int         role  = 0;
  std::string name, description, value_text;
  uint32_t    state = 0;
  int         x = 0, y = 0, w = 0, h = 0;
  int         tab_index = -1;                    // -1 = not a tab stop
  int         total_child_count = -1, first_child_index = 0;
};

inline std::vector<A11yNode> build_a11y_tree(const std::vector<A11yInput>&);
inline const A11yNode* a11y_find(const std::vector<A11yNode>&, A11yNodeId);
inline const A11yNode* a11y_hit_test(const std::vector<A11yNode>&, int x, int y);
inline std::string a11y_format_value(const A11yInput&);
inline int  a11y_derive_role(const A11yInput&);
inline bool a11y_node_id_equal(A11yNodeId, A11yNodeId);
```

`build_a11y_tree` responsibilities: derive default roles from `type` + the state
flags; default the name from `text` when no `name` is declared; resolve
`labelled_by`; prune `ROLE_NONE` subtrees; compute the effective state (derived,
then client overrides applied per `state_mask`); mark `OFFSCREEN` from
`clip_*`; assign `tab_index`; drop invisible and zero-size rows **except**
menu-model containers (G8); and validate parentage.

**Tier-1 tests** (`tests/test_a11y_tree.cpp`) — the invariants worth pinning:

- every non-root node's `parent` exists, and `children` is exactly the inverse of
  `parent` (no orphans, no duplicates) — the invariant every provider crash comes from
- `ROLE_NONE` prunes the whole subtree, not just the node
- a declared role beats the type default; `ROLE_DEFAULT` does not
- name priority: declared `name` > resolved `labelled_by` > `text`
- `labelled_by` cycles (`A`←`B`←`A`) terminate; a pruned or nonexistent target
  degrades to unnamed
- state override honours `mask` precisely: bits outside it stay derived
- `check_state` indeterminate → `MIXED`, not `CHECKED`
- sub-element rows: N list rows → N ordered `LIST_ITEM`s; grid headers + rows +
  cells get correct parentage; tree items keep their nesting; an empty list
  yields the container and no children
- a virtualized container reports `total_child_count` / `first_child_index`
  unchanged while emitting only a window of children
- `OFFSCREEN` set for a row outside its `clip_*`, clear for one inside
- a `modal_blocked` frame's subtree reports disabled
- `tab_index` ordering matches document order for the rows marked `tab_stop`
- hit-test returns the **topmost, innermost** node, skips pruned and offscreen
  nodes, and returns null outside the frame
- **node identity survives slot reuse** (G9): a node id whose `generation` is
  stale must not resolve to the row now occupying that slot
- `a11y_format_value`: full priority chain (§4.4), incl. `NaN`/inf value,
  `vmin == vmax`, inverted (`vmin > vmax`) ranges, and `has_value == false`
  (a BUTTON must get no value at all)
- degenerate input: empty vector, nonexistent `parent`, and a **cycle in
  `parent`** — must terminate, because a malformed tree must not hang the AT

### 6.2b — the host adapter (`hosts/crossplatform/a11y_adapter.cpp`)

Called out as its own sub-phase because revision 1 called it "thin" and it is
not — this is where most of the wave's real work lives. It walks
`Tree<WidgetData>` and emits `A11yInput` rows:

- one row per widget, using the 6.0 layout helper for `x/y/w/h` and the
  enclosing scroll clip;
- sub-element rows per G2: list rows from `items`, tree items from
  `flatten_visible()` (preserving nesting), grid headers/rows/cells from
  `GridModel` **windowed to the visible range** (§4.2), tab chips from
  `TabViewWidget::chips`, menu items from the menu `Tree<>`;
- **overlay geometry** (G8): combo drop rows from `ComboBoxWidget::overlay_rect`,
  menu items from the open-cascade session state — emitted only while open;
- the per-slot generation counter (G9), bumped on widget destroy;
- reading the `neui.a11y.*` attrs into the declaration fields.

Because it needs host types it cannot be Tier-1 tested. Its coverage is the macOS
harness (§5), and that is a real asymmetry to state rather than paper over: on
win32 and Linux the adapter is shared code that is *exercised* only through
providers we cannot run.

### 6.3 — macOS provider (`hosts/crossplatform/platform_macos.mm`)

`NEUIView` (`platform_macos.mm:99`) becomes the accessibility container:

- Override `accessibilityChildren`, returning one `NSAccessibilityElement`
  subclass instance per node, cached and rebuilt on a dirty flag.
- Per element: `accessibilityRole` (+ `accessibilityRoleDescription` so VoiceOver
  says "knob" while the role stays `NSAccessibilitySliderRole`),
  `accessibilityLabel`, `accessibilityHelp`, `accessibilityValue`,
  `accessibilityFrame` (**the G4 Y-flip lives here**), `isAccessibilityEnabled`,
  `isAccessibilityFocused`, `accessibilityParent`, `accessibilityHitTest:`.
- Actions: `accessibilityPerformPress` on BUTTON/CHECKBOX/MENU_ITEM/TAB,
  `accessibilityPerformIncrement`/`Decrement` on SLIDER/KNOB — routed through the
  same path as the arrow keys, so an AT increment fires
  `GESTURE_BEGIN`/`VALUE_CHANGED`/`GESTURE_END` exactly like a keypress.
- Notifications from `Session::set_focus` (`host.cpp:874`) and the value/attr
  write paths; `announce` → `NSAccessibilityAnnouncementRequestedNotification`.
- Modern `NSAccessibilityProtocol` style, not the deprecated attribute bag.

### 6.4 — win32 provider (`hosts/crossplatform/platform_win32.cpp`)

- `WM_GETOBJECT` in `XplWndProc` (`platform_win32.cpp:420`) →
  `UiaReturnRawElementProvider`.
- `IRawElementProviderSimple` + `Fragment` + `FragmentRoot` on the frame;
  per-node fragments with `IValueProvider` / `IRangeValueProvider` /
  `IInvokeProvider` / `IToggleProvider` / `ISelectionItemProvider` /
  `IExpandCollapseProvider` / `ITableProvider` as the role dictates.
- `UiaRaiseAutomationEvent`, `UiaRaiseAutomationPropertyChangedEvent`,
  `UiaRaiseNotificationEvent` (for `announce`).
- New link library `uiautomationcore`; `UiaClientsAreListening()` backs `is_active`.
- **Ships unverified — decided 2026-08-11** (§7 decision 2). It cannot be compiled
  or run on the machine this is being built on. It gets the stub-header parse check
  that caught the Wave 4.3 win32 defects, and ships marked **inspection-only,
  unverified at runtime** until someone runs Narrator/Inspect against it. Given the
  size of the UIA surface *and* that the adapter feeding it (6.2b) has no executed
  coverage on Windows at all (§5), **expect real bugs on first run** — this is the
  largest block of unverified code the programme has carried. Two obligations that
  follow from the decision, not optional:
  1. `docs/accessibility.md` says so in prose, not just the commit message — a
     reader evaluating whether to rely on it must not have to dig through git log.
  2. The first Windows session after this lands treats "run Inspect.exe against
     `neui_a11y_example`" as the opening move, before anything is built on top.

### 6.5 — Linux AT-SPI — **OUT OF THIS WAVE** (decided 2026-08-11)

Split into its own wave per §7 decision 1. Wave 6 is therefore
**6.0-6.4 + 6.6 + 6.7**, and Linux ships with **no** accessibility provider —
a neui window on Linux stays opaque to Orca until the AT-SPI wave lands. That is
a real, user-visible gap and belongs in `docs/deferred-issues.md` when this wave
lands, not just in this plan.

Retained here as the scoping note for that future wave: gate on `NEUI_HAS_DBUS`,
no-op when absent (the pattern theme tracking and the file-dialog portal already
use). Registering with `org.a11y.Bus` and hand-implementing even a minimal
`Accessible` + `Component` + `Value` + `Action` + `Selection` surface over raw
libdbus is comparable in size to everything in 6.0-6.4 combined, and cannot be
runtime-verified on the machine this is being built on either. The open question
for that wave is hand-rolled libdbus vs an atk/at-spi2 dependency — deliberately
left undecided, since the shared model makes AT-SPI a pure add-on whenever it
happens and the choice is better made with the providers' shape already known.
That is the point of building the model first.

### 6.6 — focus determinism (scope corrected)

Revision 1 claimed this wave retires three `deferred-issues.md` items and that it
needed to make KNOB a tab stop. Both were wrong:

- KNOB **is already** a tab stop on xpl (`widgets.cpp:196`) and
  `KnobWidget::on_keydown` already exists (`host.cpp:3493`). Nothing to do.
- `deferred-issues.md:10` (KNOB refuses first responder) and `:11` (Full Keyboard
  Access) are **native-macOS-host** items. An xpl-only wave cannot retire them.

What actually belongs here:

- Per-frame traversal from 6.0 (**G3**), with a two-frame harness check.
- A focused widget scrolled out of view must report `OFFSCREEN` and stay in the
  tree, not vanish.
- Focus must be reported per frame: each provider answers with its own frame's
  focused element, or none.
- Only **`deferred-issues.md:12` ("Tier B focus parity")** retires with this wave.

### 6.7 — docs + retirement

- New `docs/accessibility.md`; a **Subsystem reference** row in `CLAUDE.md`; a
  `NEUI_API_A11Y` entry in the named-interface-dispatch list (xpl-only, alongside
  `_EMBED` / `_TIMER` / `_POINTER`).
- `k_well_known_attrs` rows for every `neui.a11y.*` key.
- Retire `deferred-issues.md:12` only; add the new deferred items this wave
  leaves open (AT-SPI, text interfaces, full GRID virtualization, MSAA).
- New example `neui_a11y_example`: a declared CUSTOMDRAW knob with a real
  `set_value_range` + `set_value`, and a `set_labelled_by` pairing — the pattern
  a plugin author actually needs.

---

## 4. Design decisions worth stating up front

### 4.1 Pull, not push

The node tree is built **lazily on query** behind a dirty flag, never eagerly on
change. Rebuilding on every value change with no AT attached is pure waste, and
macOS has no reliable attach signal to gate on anyway. Invalidate the flag from
the mutation paths; build in the provider.

### 4.2 Full rebuild when dirty, but containers are windowed (resolves G10)

Two halves that revision 1 had in contradiction — it specified an eager full
expansion *and* promised lazy virtualized rows in its risk table:

- **Rebuild:** whole-frame, no incremental diffing. Precedent is the Wave 5
  measurement: a full 100-widget *paint* costs 0.84 ms, and building POD nodes is
  far cheaper than painting them. Diffing has a real correctness cost and should
  not precede a measurement saying it is needed.
- **Containers:** a GRID (and a long LISTBOX / TREEVIEW) emits only the
  **currently visible window** of children, plus `total_child_count` and
  `first_child_index` so a provider advertises the true set size and index. A
  10000-row grid therefore costs ~30 nodes, not ~90k.
- **The limitation this accepts, explicitly:** an AT cannot reach a row that is
  not scrolled into view. Both UIA (`IItemContainerProvider`, virtualized
  patterns) and NSAccessibility support true virtualization; we are not implementing
  it in v1. It goes in `docs/deferred-issues.md`, and if a real screen-reader user
  hits it, that is the trigger to build it.

### 4.3 Node identity must survive rebuilds *and* slot reuse (resolves G9)

`A11yNodeId` is `{widget_id, generation, sub_kind, sub_index}`. `generation` is a
per-slot destroy counter maintained by the adapter and bumped whenever a widget
is destroyed, which is what makes a stale AT-held reference resolve to **nothing**
instead of to whatever widget later occupied the slot. This is the correctness
issue behind `CLAUDE.md`'s "stale-after-slot-reuse not detected (deferred)": we
do not need to fix it framework-wide, only inside the a11y layer, and only here
does an external party hold references long enough for it to matter. It gets its
own Tier-1 test.

### 4.4 Value string priority (resolves G7)

`a11y_format_value` resolves in order:

1. explicit `set_value_text` — always wins
2. `NEUI_ATTR_VALUE_TEXT`, if set
3. `has_range` → map normalized `[0..1]` onto `[vmin..vmax]` and format
4. `has_value` only → percentage ("42 %"), which at least beats "0.42"
5. no value → **no value string at all** (a BUTTON must not report one)

**Known conflict, documented rather than papered over:** step 2 reuses
`NEUI_ATTR_VALUE_TEXT`, which on a KNOB is *also* the painted on-widget overlay
(`attrs.h:372-377`). A client wanting an AT-only string with no drawn overlay must
use `set_value_text`. Reading VALUE_TEXT as a fallback is still right — a client
that already sets it for the overlay gets accessibility for free.

### 4.5 CUSTOMDRAW: declared, never guessed (G6)

Default role is `GROUP`; CUSTOMDRAW is where `set_role` earns its place. Two
rejected alternatives, recorded so they are not re-proposed:

- *Infer from the attached behavior asset* (a `DRAG_*` handler writing
  `NEUI_PARAM_VALUE` ⇒ slider). Tempting and wrong: behavior assets also drive
  buttons, toggles, XY pads and drag sources, and a confidently wrong role is
  worse for a screen-reader user than a generic one.
- *Infer from compound layers.* Same objection, more indirection.

What we **can** do without guessing: when a CUSTOMDRAW carries a behavior asset
that writes a value attr, emit `FOCUSABLE` and wire increment/decrement to the
path the behavior already uses. Mechanism without semantics.

### 4.6 `is_active` is advisory

win32 can answer honestly (`UiaClientsAreListening`). macOS cannot — the API is
lazy by design with no attach signal, so the best available answer is "has
anything queried us yet". The header must say so, and no correctness may depend
on it.

### 4.7 Composition with existing features

- **Zoom** (`NEUI_ATTR_UI_SCALE`): model stays logical; each provider multiplies
  by `logical_to_physical()`. A zoomed frame must report zoomed screen rects or AT
  cursor tracking lands in the wrong place.
- **Embedding** (`NEUI_API_EMBED`): an embedded PLUGWINDOW has no NSWindow of its
  own and is a child HWND. The provider roots at the **view**, not the window, and
  lets the DAW's tree be the parent. Getting this wrong is how a plugin ends up
  invisible to a screen reader inside an otherwise accessible host — the entire
  use case.
- **Tabs**: non-selected TABPAGEs are `visible=false` and are correctly dropped —
  **but the TAB nodes come from the chip strip, not from the pages** (G2), so all
  tabs remain reachable and switchable via `accessibilityPerformPress` even though
  only one page's content is in the tree. Revision 1 got this wrong by mapping
  TABPAGE→TAB, which would have exposed only the selected tab.
- **Modal dialogs**: an input-blocked owner frame reports its subtree disabled
  (`modal_blocked`) rather than offering navigable-but-dead controls.
- **`NEUI_ATTR_INPUT_TRANSPARENT` / `NEUI_ATTR_OVERLAY`**: input-transparent
  widgets are decorative by construction — default them to `ROLE_NONE` unless a
  role is declared, since they cannot be interacted with.
- **DnD**: out of scope. No platform drag-and-drop accessibility in v1; note it as
  deferred.
- **Multiple sessions / iOS / null**: the interface is exposed per session and the
  provider hangs off the frame's native view, so multiple sessions are naturally
  independent. iOS and the null platform expose **no** provider (iOS would want
  UIAccessibility, a separate wave) — `get_interface` still returns the vtable so
  declarations are accepted and simply have no consumer, matching how
  `platform_supports_ui_scale()` makes zoom inert rather than half-working.

### 4.8 Transient surfaces are announced, not modelled (G8)

Toasts (`host.h:327`) and the neui-drawn message box are not widgets and should
not become tree nodes — a node an AT can never reach or dismiss is worse than
none. Instead `Session::toast_show` and the message box call `announce()`
internally, so the text is spoken when it appears. `announce` is public for
clients with their own hand-drawn transients.

---

## 5. Testing strategy

**Tier-1 (`tests/test_a11y_tree.cpp`)** — the invariant list in §6.2. Runs
everywhere including the null platform.

**macOS harness (`tests/a11y_smoke_macos.mm`)** — following the six existing
harnesses. It calls the `NSAccessibility` protocol methods **in-process, directly
on the view** rather than acting as an AX client via
`AXUIElementCreateApplication`: the client route needs TCC accessibility
permission, which would make the test un-runnable in a fresh checkout or CI,
while the in-process route needs no permission and still exercises every override.
Checks: child count + roles for a known layout; name resolution through
`labelled_by`; a knob's value string; hit-test at a known point; focus following
`focus_next` (and **not** crossing frames); press/increment producing the same
events a keypress does; combo overlay rows appearing only while open; a stale node
id after a destroy+recreate resolving to nothing.

**Known coverage hole, stated rather than glossed:** the adapter (6.2b) is where
most of the extraction bugs will live, it needs host types so Tier-1 cannot reach
it, and on win32/Linux it is exercised only through providers we cannot run. The
macOS harness is its only executed coverage. This is the same asymmetry Wave 4.3
had, and it is the argument behind §7 decision 2.

**Manual, once** — VoiceOver on `neui_a11y_example` plus Accessibility Inspector's
audit. Automated tests cannot tell us whether announcements are *sensible*, only
that they exist. Record the result in the commit message.

**win32 / Linux** — stub-header parse checks only, explicitly labelled unverified.

---

## 6. Risks

| Risk | Mitigation |
|---|---|
| The 6.0 layout refactor destabilises paint | Pure refactor; repaint bench before/after; all six visual harnesses re-run. Fallback: force-paint before first query. |
| UIA provider is large and unverifiable here | Land it last, parse-check it, ship labelled unverified, expect a follow-up. |
| Adapter bugs invisible on win32/Linux | §5 hole is acknowledged; keep the adapter's per-type extraction as small, obvious functions and prefer shared helpers already covered by Tier-1. |
| Stale node references misresolve after slot reuse | Generation counter (§4.3) + its own test. |
| Scope creep into a11y *text* interfaces (`AXTextMarker`, UIA TextPattern) | Out of scope. INPUTBOX/MULTILINE expose value + selection only; full text navigation is a separate wave. |
| AT-SPI swallows the wave | Resolved: split out of this wave (§7 decision 1). |
| Windowed containers read as "the list is short" | Providers must always report `total_child_count`; the Tier-1 test pins it. |

---

## 7. Decisions — **all five resolved 2026-08-11**, 6.1 is unblocked

1. **AT-SPI (6.5) → its own wave.** Wave 6 is 6.0-6.4 + 6.6 + 6.7. Linux ships
   with no provider in the meantime; that gap goes in `docs/deferred-issues.md`
   (§6.5).
2. **win32 UIA (6.4) → written and shipped unverified**, with the two obligations
   spelled out in §6.4: the limitation stated in `docs/accessibility.md` prose,
   and Inspect.exe as the opening move of the next Windows session.
3. **MSAA fallback → no.** UIA only; deferred item recorded.
4. **Text interfaces → no.** INPUTBOX/MULTILINE expose value + selection only;
   character/word/line navigation is its own wave.
5. **Full container virtualization → no.** Windowed children per §4.2, with the
   "an AT cannot reach an unscrolled row" limitation documented.

Decisions 3-5 were taken as the recommended defaults; 1 and 2 were called
explicitly. Nothing in §7 blocks implementation any more.

---

## 8. Rough effort

Revised upward from revision 1, which under-sized 6.2 by calling the adapter thin.

| Phase | Size | Verifiable here? |
|---|---|---|
| 6.0 prerequisites | **done** — landed as a hybrid, see 6.0 | yes, incl. a new harness |
| 6.1 client seam | **done** | yes — 31-check harness |
| 6.2 shared model + Tier-1 tests | medium | yes, fully |
| 6.2b host adapter | **large — the real bulk of the wave** | macOS only |
| 6.3 macOS provider | medium | **yes, incl. VoiceOver** |
| 6.4 win32 provider | large | no — **ships unverified by decision** |
| ~~6.5 AT-SPI~~ | ~~large~~ | **out of this wave** (§7 decision 1) |
| 6.6 focus determinism | small (most of it turned out to be already done) | yes |
| 6.7 docs + example | small | yes |

Build order: **6.0 → 6.1 → 6.2 → 6.2b → 6.3 → 6.6 → 6.4 → 6.7.** 6.6 sits before
6.4 deliberately — per-frame focus is verifiable here and the win32 provider reads
focus, so the unverifiable phase should be built against settled behaviour.

---

## 9. Differences from the Wave 6 sketch in `sst-neuigui-gap-response.md`

1. **The shared model takes POD input; it does not walk `Tree<WidgetData>`.**
   Walking it would drag host types into the Tier-1 binary and break the
   `hosts/shared/*.h` contract.
2. **Nodes are `{widget_id, generation, sub_kind, sub_index}`, not widget ids** —
   LISTBOX / COMBOBOX / TREEVIEW / GRID / MENUBAR / **TABVIEW** all violate a 1:1
   widget↔node mapping (G2), and slot reuse makes the raw id unstable (G9).
3. **macOS lands before win32**, for verifiability.
4. **6.0 exists at all** — paint-dependent `abs_x/abs_y` (G1) and cross-frame Tab
   (G3) both have to be fixed before a provider can be correct.
5. **AT-SPI is proposed as its own wave**, not folded in as "lowest priority".
6. **`set_labelled_by` added** (G5) — `set_name` alone leaves the standard
   LABEL-next-to-INPUTBOX idiom unnamed.
7. **Value formatting is specified** (G7) — the sketch said "value reporting for
   continuous controls" without confronting that `NEUI_PARAM_VALUE` is normalized.
8. **`announce` added** (G8) — toasts and the drawn message box are not widgets and
   need speaking, not modelling.
9. **Only one `deferred-issues.md` item retires**, not three — the other two are
   native-host items an xpl-only wave cannot touch (§6.6).

## 10. What revision 2 changed, and why

A review of revision 1 found two blockers and four contradictions. Recorded so
the same ground is not re-litigated:

- **`A11yInput` was underspecified to the point of being unimplementable.** It
  carried `sub_count` + `sub_kind` and *no widget text at all*, while
  `build_a11y_tree` was required to produce grid header/row/cell parentage, tree
  nesting, default names from widget text, `CHECKED`/`MIXED` from check state, and
  `OFFSCREEN` from scroll clips. A count and a kind cannot express any of that. Now
  one self-describing row per node candidate, with the extraction named as its own
  phase (6.2b) and sized as the bulk of the wave.
- **Node identity ignored slot reuse** (G9), defeating §4.3's own stated goal.
  Generation counter added.
- **TABVIEW was missing from G2 and mapped TABPAGE→TAB**, which — combined with
  the "drop invisible nodes" rule — would have exposed only the *selected* tab,
  leaving an AT user unable to see or switch to the others. Tabs now come from the
  chip strip.
- **Overlay geometry was unsolved** for the combo drop list, menus, toasts and the
  message box (G8). First two get overlay-sourced rows; last two get `announce`.
- **§4.2 contradicted its own risk table** — eager full expansion vs "report rows
  lazily". Resolved as windowed containers with true totals.
- **No way to report a numeric value for the flagship CUSTOMDRAW case** — `notify`
  documented a client whose value never reaches the attribute bag, but there was
  no `set_value` for it to call. Added.
- **Wrong citations and a false retirement claim**: `deferred-issues.md` items are
  at :10-12, not :6-8; KNOB is already a tab stop on xpl (`widgets.cpp:196`) with
  `on_keydown` already present (`host.cpp:3493`), so 6.6's first bullet described
  completed work while the item it claimed to retire is native-host-only.
