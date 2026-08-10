# Accessibility (`NEUI_API_A11Y`) — implementation plan

Status: **plan only, nothing implemented.** This is Wave 6 of
`plans/sst-neuigui-gap-response.md` (its §3 sketch, lines 991-1027), worked out
to the point where it can be built. Where this plan contradicts that sketch, this
plan is the newer decision and the differences are called out in §9.

Related: `docs/deferred-issues.md` "Tier B focus parity" and the two macOS
focus items above it — the same request from a different direction. All three
retire when this lands.

---

## 1. What we are building, and why here

neui has **no accessibility story at all** today: no `WM_GETOBJECT`, no
`IAccessible`/UIA provider, no `NSAccessibility` overrides, no AT-SPI, and no
client-facing API. A screen reader sees a neui window as one opaque rectangle.
For the sst-jucegui use case this is a hard blocker on shipping a plugin UI into
an accessible host.

**The architectural decision that shapes everything else:** the xpl host paints
**one native surface per FRAME** (`WidgetData::native_handle` is non-null only on
APPWINDOW / PLUGWINDOW / DIALOG — `hosts/crossplatform/host.h:82`). There is no
native view per widget, so the platform gives us nothing per widget and the
provider tree has to be **synthetic no matter what granularity we pick**.

That is good news for scoping. It means:

- Build it **once, in the xpl host**, and one widget-tree walk serves win32 +
  macOS + Linux.
- The interesting, bug-prone half (role derivation, ordering, pruning, bounds,
  hit-test, value formatting) is **portable C++** and therefore Tier-1 testable
  with no platform code — the same trick `hosts/shared/*.h` uses everywhere.
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
3. The same tree is exposed to UIA on Windows and (minimally) AT-SPI on Linux.
4. Focus is deterministic and per-frame.
5. Zero measurable cost when no assistive technology is attached.

---

## 2. Substrate audit — what exists, what is missing

### Already in place (verified, with references)

| Need | Where it lives |
|---|---|
| Per-widget geometry | `WidgetData::x/y/width/height` + frame-local `abs_x/abs_y` (`host.h:43-53`) |
| Focusability + real traversal | `tab_stop` (`host.h:57`), `collect_tab_stops` + `focus_next` (`host.cpp:2657-2700`) |
| Focus change hook | `Session::set_focus` (`host.cpp:874`) already fires `WIDGET_FOCUS` both ways |
| Enabled / visible | `WidgetData::enabled`, `::visible` (`host.h:56-61`) |
| Hit-testing | virtual `WidgetData::hit_test` (`host.h:168`) |
| Type identity | `WidgetData::type` vs the `NEUI_W_*` strings (`include/neui/d/widgets.h:218-242`) |
| Continuous value | `NEUI_PARAM_VALUE` (`attrs.h:499`), `NEUI_PARAM_DEFAULT` (`:503`) |
| Human-readable value string | `NEUI_ATTR_VALUE_TEXT` (`attrs.h:377`) — see the conflict in §4.4 |
| Check state | `CheckboxWidget::check_state` (`host.h:538`) |
| Selection | `ListItemsWidget::selected_item` (`host.h:593`), `TreeviewWidget::selected_tree_item` (`host.h:673`) |
| Expand state | `TreeviewWidget::TreeItem::expanded` (`host.h:664`) |
| Read-only / password | `NEUI_ATTR_READONLY`, `NEUI_ATTR_PASSWORD` (`hosts/shared/attrs.h:55-56`) |
| Zoom + DPI conversion | `WidgetData::logical_to_physical()` (`host.h:126`) |

Role, name, value, state and bounds are therefore all **derivable** for the
built-in widget types. That is the cheap part of this wave.

### Gaps that the Wave 6 sketch did not account for

These are the reason this plan exists rather than going straight at 6.1.

**G1 — `abs_x/abs_y` are only valid after the first paint.** They are recomputed
top-down by `paint_widgets_recursive` each frame, and `host.h:47-53` documents
that before the first paint they read 0. That was acceptable for hit-testing
("input cannot arrive before the window is first painted") but it is **not**
acceptable for accessibility: UIA probes a window on creation, and VoiceOver can
be pointed at a window that has never been painted (or whose frame is occluded).
An AT asking for bounds must not get `(0,0)` for every element. Fix in **6.0**.

**G2 — the a11y tree is not the widget tree.** Four widget types contain
sub-elements that are *model state*, not widgets:

- LISTBOX / COMBOBOX rows — `ListItemsWidget::items` (`host.h:592`)
- TREEVIEW items — `TreeviewWidget::tree_items` + `flatten_visible()` (`host.h:670,688`)
- GRID rows/cells/headers — `neui_detail::GridModel` (`host.h:761`); a 10000×8
  grid is deliberately *one* widget
- MENUBAR / POPUPMENU items — a `Tree<>` of items

A screen reader needs to reach these. So a node is **not** identified by a widget
id alone; it needs `{widget, sub_kind, sub_index}`. The sketch's "walks the widget
tree once and yields nodes: id, parent, ordered children…" silently assumed a 1:1
mapping that does not hold. Designed in **6.2**.

**G3 — focus is session-global and Tab crosses frames.** `Session::_focused_widget`
is one value per session (`host.h:1257`), and `focus_next` collects tab stops
from **every root child** — i.e. every frame of the session (`host.cpp:2676-2681`).
Two consequences: (a) Tab can today move focus from a widget in one window into a
widget in another window, which is wrong independently of accessibility; (b) each
platform provider is rooted at **one frame's** native view, so it must expose only
its own frame's subtree and its own frame's focused element. Fix in **6.6**.

**G4 — bounds must reach the AT in screen coordinates.** `abs_x/abs_y` are
frame-local logical px at 96 DPI. Every provider wants screen coordinates, and
each in a different unit (UIA: physical px; NSAccessibility: screen points,
bottom-left origin, Y flipped relative to our Y-down convention; AT-SPI: px).
The zoom must be folded in via `logical_to_physical()`, and the flip is exactly
the kind of thing that silently half-works. One conversion helper per platform,
and the shared model stays in frame-local logical px.

**G5 — no accessible name for a control labelled by a separate LABEL.** The
established neui layout idiom is a LABEL widget next to an INPUTBOX. There is no
`label-for` relationship, so the input would be announced as unnamed. Needs an
explicit client call (`set_labelled_by`), because inferring it from proximity is
a heuristic that will be wrong often enough to be worse than nothing.

**G6 — CUSTOMDRAW is semantically empty, and it is the case that matters most.**
sst-jucegui's controls are *all* custom-painted (`CustomDrawWidget` +
compound/behavior assets, `host.h:721-751`). A CUSTOMDRAW has no intrinsic role.
We cannot guess: the same widget type is a knob, a meter, a decorative
background, and a whole composited panel. Handled in §4.5.

**G7 — a normalized value is useless to a screen reader.** KNOB and SLIDER carry
`NEUI_PARAM_VALUE` as normalized `[0..1]`. "Zero point four two" tells the user
nothing; "minus six decibels" does. Handled in §4.4.

---

## 3. Phases

Ordered by dependency, then by what is verifiable on the machine this gets built
on. **Deviation from the sketch: macOS lands before win32.** Two Windows build
breaks shipped in earlier waves of this same programme, both from code that
could not be compiled here; a provider is far more intricate than either. Doing
the platform we can actually run VoiceOver against first means the shared model's
design gets validated by a real AT before a second, unverifiable provider is
written on top of it.

### 6.0 — prerequisites (no public API change)

- **Factor the absolute-position walk out of the paint path.** Extract from
  `paint_widgets_recursive` the part that computes a child's frame-local origin —
  parent origin + `x/y`, SECTION band offset, scroll offsets, TABVIEW page
  visibility — into one helper in `host.cpp`, called by both the paint walk and a
  new non-painting `Session::refresh_layout()`. Fixes **G1** for accessibility and
  incidentally for hit-testing.
  - **This is the riskiest item in the wave**: it touches the paint hot path, and
    the offset rules are currently spread through the walk. It must be a pure
    refactor with the repaint bench (`tests/repaint_bench.cpp`) run before and
    after to show no regression, and the existing visual harnesses re-run.
  - Cheaper fallback if the refactor proves invasive: have the provider call
    `platform_invalidate` + one synchronous paint before answering the first
    query. Correct but ugly, and it makes an AT query cause a repaint. Prefer the
    refactor; keep this in reserve.
- **Per-frame tab-stop collection.** Give `collect_tab_stops` a frame root
  parameter and have `focus_next` use the frame owning `_focused_widget`
  (falling back to the frame that has OS focus). Fixes **G3**(a).

### 6.1 — client seam: `include/neui/d/a11y.h`

New public header, `NEUI_API_A11Y`, xpl-host-only (like `NEUI_API_TIMER` /
`_POINTER` / `_EMBED`) — so the header must carry the same feature-detect warning
those do: on win32/macOS `neui_get_api(NULL)` returns the **native** host first,
which returns NULL for this interface.

```c
#define NEUI_API_A11Y "com.defiantnerd.neui.extension.a11y/0"

typedef enum neui_a11y_role_t {
  NEUI_A11Y_ROLE_DEFAULT = 0,   // derive from widget type (see the table below)
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

// State bits. A client sets/clears via (mask, values); everything it does not
// name in `mask` stays framework-derived.
#define NEUI_A11Y_STATE_DISABLED   0x0001u
#define NEUI_A11Y_STATE_FOCUSED    0x0002u
#define NEUI_A11Y_STATE_FOCUSABLE  0x0004u
#define NEUI_A11Y_STATE_SELECTED   0x0008u
#define NEUI_A11Y_STATE_EXPANDED   0x0010u
#define NEUI_A11Y_STATE_COLLAPSED  0x0020u
#define NEUI_A11Y_STATE_CHECKED    0x0040u
#define NEUI_A11Y_STATE_MIXED      0x0080u   // CHECKBOX3 indeterminate
#define NEUI_A11Y_STATE_READONLY   0x0100u
#define NEUI_A11Y_STATE_OFFSCREEN  0x0200u   // scrolled out of its clip rect
#define NEUI_A11Y_STATE_PROTECTED  0x0400u   // password
#define NEUI_A11Y_STATE_MULTILINE  0x0800u

typedef enum neui_a11y_change_t {
  NEUI_A11Y_CHANGE_VALUE = 0,   // value / value text changed
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
  // Explicit display string. Highest-priority source (see 4.4).
  void (NEUI_ABI *set_value_text)(neui_session_t, neui_widget_t, const char* utf8);

  void (NEUI_ABI *set_state)(neui_session_t, neui_widget_t,
                             uint32_t mask, uint32_t values);
  void (NEUI_ABI *set_labelled_by)(neui_session_t, neui_widget_t widget,
                                   neui_widget_t label);   // fixes G5

  // Tell the AT something changed. The framework already does this for every
  // built-in widget (focus, native value, selection); a client only needs it
  // for a CUSTOMDRAW whose value lives in the CLIENT's own state and therefore
  // never passes through the attribute bag.
  void (NEUI_ABI *notify)(neui_session_t, neui_widget_t, neui_a11y_change_t);

  // True when an assistive technology has actually queried this session. Lets
  // a client skip building expensive display strings otherwise. Advisory:
  // never gate CORRECTNESS on it (see 4.6 - macOS cannot answer this eagerly).
  bool (NEUI_ABI *is_active)(neui_session_t);

  // Append new methods at the end (vtable-append evolution rule).
} neui_a11y_api_t;
```

Default role derivation (used when the client declares nothing):

| Widget type | Default role |
|---|---|
| APPWINDOW / PLUGWINDOW / DIALOG | `WINDOW` |
| LABEL | `STATIC_TEXT` |
| BUTTON | `BUTTON` |
| INPUTBOX | `TEXT_FIELD` (+ `PROTECTED` when `NEUI_ATTR_PASSWORD`) |
| MULTILINE | `TEXT_AREA` (+ `MULTILINE`) |
| CHECKBOX / CHECKBOX3 | `CHECKBOX` |
| LISTBOX | `LIST` + synthetic `LIST_ITEM` children |
| COMBOBOX | `COMBOBOX` + synthetic `LIST_ITEM` children |
| TREEVIEW | `TREE` + synthetic `TREE_ITEM` children |
| GRID | `TABLE` + synthetic `ROW` / `CELL` / `COLUMN_HEADER` |
| MENUBAR / POPUPMENU | `MENU_BAR` / `MENU` + `MENU_ITEM` children |
| SLIDER / KNOB | `SLIDER` |
| IMAGE | `IMAGE` |
| SECTION (non-scrolling) | `GROUP` |
| SECTION (scrolling) | `SCROLL_AREA` |
| TABVIEW / TABPAGE | `TAB_LIST` / `TAB` |
| **CUSTOMDRAW** | **`GROUP`** — see §4.5 |

Storage: role/name/description/range/value-text/state-override/labelled-by go in
the existing per-widget `AttrBag` (`WidgetData::attrs`, `host.h:141`) under
`neui.a11y.*` keys, each with its `k_well_known_attrs` row per the house rule.
No new per-widget struct, no memory cost on widgets that declare nothing.

### 6.2 — the portable model: `hosts/shared/a11y_tree.h`

Header-only, `inline`, **no host types** — the same contract as `grid_model.h`
and `scrollbar.h`, which is what makes it Tier-1 testable. So it does **not**
walk `Tree<WidgetData>` directly (the sketch said it would; that would drag the
host into the test binary). Instead:

```cpp
// What the host feeds in: one POD row per widget, already flattened in tree
// order by a thin adapter in the xpl host.
struct A11yInput {
  uint32_t widget_id   = 0;
  uint32_t parent_id   = 0;
  const char* type     = nullptr;      // NEUI_W_* string
  int  x = 0, y = 0, w = 0, h = 0;     // frame-local logical px
  bool visible = true, enabled = true, focused = false, tab_stop = false;
  int  declared_role   = 0;            // NEUI_A11Y_ROLE_DEFAULT = derive
  const char* name = nullptr, *description = nullptr, *value_text = nullptr;
  bool has_range = false; float vmin = 0, vmax = 1, vstep = 0, value = 0;
  uint32_t state_mask = 0, state_values = 0;
  uint32_t labelled_by = 0;
  int  sub_count = 0;                  // synthetic children (rows/items/cells)
  int  sub_kind  = 0;                  // A11ySubKind
  ...
};

// Node identity - fixes G2. sub_index < 0 means "the widget itself".
struct A11yNodeId { uint32_t widget_id; int32_t sub_kind; int32_t sub_index; };

struct A11yNode {
  A11yNodeId  id, parent;
  std::vector<A11yNodeId> children;    // ordered
  int         role  = 0;
  std::string name, description, value_text;
  uint32_t    state = 0;
  int         x = 0, y = 0, w = 0, h = 0;   // frame-local logical px
  int         tab_index = -1;               // -1 = not a tab stop
};

inline std::vector<A11yNode> build_a11y_tree(const std::vector<A11yInput>&);
inline const A11yNode* a11y_hit_test(const std::vector<A11yNode>&, int x, int y);
inline std::string a11y_format_value(const A11yInput&);   // see 4.4
inline int  a11y_derive_role(const char* type, /* attr flags */ ...);
inline bool a11y_node_id_equal(A11yNodeId, A11yNodeId);
```

Responsibilities of `build_a11y_tree`: derive default roles; resolve
`labelled_by` into a name; prune `ROLE_NONE` subtrees; expand `sub_count` into
synthetic children; compute the effective state mask (framework-derived, then
client overrides applied per `state_mask`); mark `OFFSCREEN` for nodes outside
their scroll clip; assign `tab_index` in `collect_tab_stops` order; and drop
invisible / zero-size nodes.

**Tier-1 tests** (`tests/test_a11y_tree.cpp`), the invariants worth pinning:

- every non-root node's `parent` exists in the output, and `children` is exactly
  the inverse of `parent` (no orphans, no duplicates) — the invariant every
  provider crash comes from
- `ROLE_NONE` prunes the whole subtree, not just the node
- a client-declared role beats the type default; `ROLE_DEFAULT` does not
- `labelled_by` resolves; a cycle (`A` labelled by `B` labelled by `A`)
  terminates and does not hang
- `labelled_by` pointing at a pruned or nonexistent widget degrades to unnamed
- state override honours `mask` precisely: bits outside the mask stay derived
- `CHECKBOX3` indeterminate → `MIXED`, not `CHECKED`
- synthetic children: N list items → N `LIST_ITEM` nodes in row order; a GRID
  yields headers + rows + cells with correct parentage; an empty list yields the
  container and no children
- `tab_index` ordering matches the widget-tree order `collect_tab_stops` produces
- hit-test returns the **topmost, innermost** node, skips pruned and offscreen
  nodes, and returns null outside the frame
- `a11y_format_value`: full priority chain (§4.4), including `NaN`/inf value,
  `vmin == vmax`, and inverted (`vmin > vmax`) ranges
- degenerate input: empty vector, a parent id that does not exist, a cycle in
  `parent_id` (must terminate — a malformed tree must not hang the AT)

### 6.3 — macOS provider (`hosts/crossplatform/platform_macos.mm`)

`NEUIView` (`platform_macos.mm:99`) becomes the accessibility container:

- Override `accessibilityChildren`, returning one `NSAccessibilityElement`
  subclass instance per node from the shared model, cached and rebuilt on a
  dirty flag.
- Per-element: `accessibilityRole` (+ `accessibilityRoleDescription` for KNOB, so
  VoiceOver says "knob" while the role stays `NSAccessibilitySliderRole`),
  `accessibilityLabel`, `accessibilityHelp`, `accessibilityValue`,
  `accessibilityFrame` (screen coords — **the G4 Y-flip lives here**),
  `isAccessibilityEnabled`, `isAccessibilityFocused`,
  `accessibilityParent`, and `accessibilityHitTest:`.
- Actions: `accessibilityPerformPress` on BUTTON/CHECKBOX/MENU_ITEM,
  `accessibilityPerformIncrement`/`Decrement` on SLIDER/KNOB (routing through the
  same code path as the arrow keys, so gesture events stay correct — an AT
  increment is a user-driven change and must fire `GESTURE_BEGIN`/`END` +
  `VALUE_CHANGED` exactly like a keypress does).
- Notifications: `NSAccessibilityPostNotification` from `Session::set_focus`
  (`host.cpp:874`) and from the value/attr write paths.
- macOS 10.13+ protocol style (`NSAccessibilityProtocol`), not the deprecated
  `accessibilityAttributeNames` bag.

### 6.4 — win32 provider (`hosts/crossplatform/platform_win32.cpp`)

- `WM_GETOBJECT` in `XplWndProc` (`platform_win32.cpp:420`) → `UiaReturnRawElementProvider`.
- `IRawElementProviderSimple` + `IRawElementProviderFragment` +
  `IRawElementProviderFragmentRoot` on the frame; per-node fragment objects with
  `IValueProvider` / `IRangeValueProvider` / `IInvokeProvider` /
  `IToggleProvider` / `ISelectionItemProvider` / `IExpandCollapseProvider` as the
  role dictates.
- `UiaRaiseAutomationEvent` + `UiaRaiseAutomationPropertyChangedEvent` on focus
  and value change.
- New link libraries: `uiautomationcore`. `UiaClientsAreListening()` backs
  `is_active`.
- **Verification constraint, stated plainly:** this cannot be compiled or run on
  the machine this is being built on. It gets the stub-header parse check that
  caught the Wave 4.3 win32 defects, and it ships marked as
  **inspection-only, unverified at runtime** until someone runs Narrator/Inspect
  against it. Given the size of the UIA surface, expect real bugs on first run.

### 6.5 — Linux AT-SPI (`hosts/crossplatform/platform_linux.cpp`)

Gated on `NEUI_HAS_DBUS`, graceful no-op when absent (the `NEUI_HAS_DBUS`
pattern already established by theme tracking and the file-dialog portal).

**Honest scoping:** AT-SPI is not a small interface. Registering with
`org.a11y.Bus` and implementing even a minimal
`Accessible` + `Component` + `Value` + `Action` + `Selection` surface by hand
over raw libdbus is comparable in size to everything in 6.1-6.4 combined, and it
cannot be runtime-verified here either. **Recommendation: split 6.5 out and
defer it** — land 6.0-6.4 + 6.6 + 6.7 as Wave 6, and treat AT-SPI as its own
wave with its own decision about whether hand-rolled libdbus or a dependency on
atk/at-spi2 is the right trade. The shared model makes this a pure add-on
whenever it happens, which is the whole point of building it first.

### 6.6 — focus parity

- KNOB is not a keyboard tab-stop on macOS (`docs/deferred-issues.md:6`): on xpl
  this is our own traversal, so set `tab_stop` and confirm arrow-key handling in
  `KnobWidget::on_keydown` (`host.h:585`).
- Per-frame traversal from 6.0 (**G3**).
- Verify focus survives a scrolling SECTION (a focused widget scrolled out of
  view must report `OFFSCREEN`, not vanish from the tree).
- The macOS Full-Keyboard-Access item (`deferred-issues.md:7`) stays deferred for
  the **native** host — it is OS policy there and xpl does not inherit it.

### 6.7 — docs + retirement

- New `docs/accessibility.md`; a **Subsystem reference** row in `CLAUDE.md`; a
  `NEUI_API_A11Y` entry in the named-interface-dispatch list (noting xpl-only,
  alongside `_EMBED` / `_TIMER` / `_POINTER`).
- `k_well_known_attrs` rows for every `neui.a11y.*` key.
- Retire the "Tier B focus parity" line and the KNOB tab-stop line in
  `docs/deferred-issues.md`; add whatever new deferred items 6.5 leaves open.
- New example `neui_a11y_example` showing a declared CUSTOMDRAW knob with a real
  `set_value_range` + a `set_labelled_by` pairing, since that is the pattern a
  plugin author actually needs.

---

## 4. Design decisions worth stating up front

### 4.1 Pull, not push

The node tree is built **lazily on query** behind a dirty flag, never eagerly on
change. Rebuilding a tree on every value change when no AT is attached would be
pure waste, and on macOS there is no reliable "is an AT attached" signal to gate
on anyway. Invalidate the flag from the mutation paths; build in the provider.

### 4.2 One dirty flag, not incremental diffing

First implementation rebuilds the whole frame's node vector when dirty. The Wave 5
measurement is the precedent: a full 100-widget paint costs 0.84 ms, and building
POD nodes is far cheaper than painting them. Incremental diffing is a later
optimisation with a real correctness cost, and should not be built before a
measurement says it is needed.

### 4.3 Node identity must be stable across rebuilds

Both UIA and NSAccessibility hold references across queries. `A11yNodeId` is
`{widget_id, sub_kind, sub_index}` — derived from stable ids, not from a vector
index. A destroyed widget's element must answer "invalid" rather than dangle;
this is the most likely source of an AT-side crash and deserves its own tests.

### 4.4 Value string priority (fixes G7)

`a11y_format_value` resolves in this order:

1. explicit `a11y->set_value_text` — always wins
2. `NEUI_ATTR_VALUE_TEXT`, if set
3. `set_value_range` present → map normalized `[0..1]` onto `[vmin..vmax]` and
   format
4. bare normalized value as a percentage ("42 %"), which at least beats "0.42"

**Known conflict to document, not to paper over:** step 2 reuses
`NEUI_ATTR_VALUE_TEXT`, which on a KNOB is *also* the painted on-widget overlay
(`attrs.h:374-377`). A client that wants an AT-only string and no drawn overlay
must use `set_value_text`. Reading VALUE_TEXT as a fallback is still right — a
client that already sets it for the overlay gets accessibility for free.

### 4.5 CUSTOMDRAW: declared, never guessed (G6)

Default role is `GROUP`, and CUSTOMDRAW is where `set_role` earns its place. Two
rejected alternatives, recorded so they are not re-proposed:

- *Infer from the attached behavior asset* (a `DRAG_*` handler writing
  `NEUI_PARAM_VALUE` ⇒ slider). Tempting, and wrong: a behavior asset also drives
  buttons, toggles, XY pads and drag-sources, and a confidently wrong role is
  worse for a screen-reader user than a generic one.
- *Infer from compound layers.* Same objection, more indirection.

What we **can** do without guessing: when a CUSTOMDRAW carries a behavior asset
that writes a value attr, emit `FOCUSABLE` and wire increment/decrement to the
same path the behavior uses. Mechanism without semantics.

### 4.6 `is_active` is advisory

win32 can answer honestly (`UiaClientsAreListening`). macOS cannot — the
accessibility API is lazy by design, with no attach signal, so the best available
answer is "has anything queried us yet". The header must say so, and no
correctness may depend on it.

### 4.7 Composition with existing features

- **Zoom** (`NEUI_ATTR_UI_SCALE`): the shared model stays logical; each provider
  multiplies by `logical_to_physical()`. A zoomed frame must report zoomed screen
  rects or AT cursor tracking lands in the wrong place.
- **Embedding** (`NEUI_API_EMBED`): an embedded PLUGWINDOW has no NSWindow of its
  own and is a child HWND. The provider must root at the **view**, not the
  window, and let the DAW's own tree be the parent. Getting this wrong is how a
  plugin ends up invisible to a screen reader inside an otherwise accessible
  host — which is the entire use case.
- **Modal dialogs**: an input-blocked owner frame should report as such rather
  than offering navigable-but-dead controls.
- **Tabs**: a TABPAGE that is not the selected page must not appear as navigable
  content.

---

## 5. Testing strategy

**Tier-1 (`tests/test_a11y_tree.cpp`)** — the invariant list in §6.2. This is
where the real coverage lives, and it runs on every platform including null.

**macOS harness (`tests/a11y_smoke_macos.mm`)** — following the six existing
harnesses. Key point: it calls the `NSAccessibility` protocol methods
**in-process, directly on the view**, rather than acting as an AX *client* via
`AXUIElementCreateApplication`. The client route needs TCC accessibility
permission and would make the test un-runnable in a fresh checkout or CI; the
in-process route needs no permission and still exercises every override we wrote.
Checks: child count and roles for a known layout, name resolution through
`labelled_by`, a knob's value string, hit-test at a known point, focus following
`focus_next`, and press/increment actions producing the same events a real
keypress does.

**Manual, once** — VoiceOver on `neui_a11y_example`, plus Accessibility
Inspector's audit. Automated tests cannot tell us whether the announcements are
*sensible*, only that they exist. Record the result in the commit message.

**win32 / Linux** — stub-header parse checks only, explicitly labelled
unverified. Same honesty as Wave 4.3.

---

## 6. Risks

| Risk | Mitigation |
|---|---|
| The 6.0 layout refactor destabilises paint | Pure refactor, repaint bench before/after, all six visual harnesses re-run. Fallback: force-paint before first query. |
| UIA provider is large and unverifiable here | Land it last, parse-check it, ship it labelled unverified, expect a follow-up. |
| Stale node references crash the AT | Stable `A11yNodeId` + explicit invalid-node tests (§4.3). |
| Synthetic children make huge trees (10000-row GRID) | Report rows lazily / virtualized where the platform allows; cap and log if a cap is ever introduced (no silent truncation). |
| Scope creep into a11y *text* interfaces (`AXTextMarker`, UIA TextPattern) | Explicitly out of scope for this wave. INPUTBOX/MULTILINE expose value + selection only; full text navigation is a separate wave. |
| AT-SPI swallows the wave | Split it out per §6.5. |

---

## 7. Open decisions — these need a call before 6.1 is written

1. **Is 6.5 (AT-SPI) in this wave or its own?** Recommendation: **its own.** It
   is plausibly as large as the rest combined and is the least-used of the three.
2. **Does 6.4 (win32 UIA) ship unverified, or wait for a Windows session?** The
   two Wave 4.3 win32 defects were both caught by review + parse checks, but UIA
   is a much bigger surface than a file dialog. Recommendation: **write it, ship
   it labelled unverified**, consistent with how the rest of this programme has
   handled win32 — but say so in the docs, not just the commit.
3. **MSAA fallback on win32?** Some older audio hosts and screen readers still go
   through `IAccessible` rather than UIA. Recommendation: **no** — UIA only, and
   note it as a deferred item if a real host turns out to need it.
4. **Text interfaces in scope?** Recommendation: **no** (see §6 risks).

---

## 8. Rough effort

| Phase | Size | Verifiable here? |
|---|---|---|
| 6.0 prerequisites | medium — the layout refactor is the risk | yes |
| 6.1 client seam | small | yes (compiles + attr round-trip) |
| 6.2 shared model + Tier-1 tests | medium-large — the bulk of the real logic | yes, fully |
| 6.3 macOS provider | medium | **yes, incl. VoiceOver** |
| 6.4 win32 provider | large | no |
| 6.5 AT-SPI | large — recommend deferring | no |
| 6.6 focus parity | small | yes |
| 6.7 docs + example | small | yes |

---

## 9. Differences from the Wave 6 sketch in `sst-neuigui-gap-response.md`

Recorded so the two documents do not silently disagree:

1. **The shared model takes POD input, it does not walk `Tree<WidgetData>`.** The
   sketch had it walking the widget tree; that would drag host types into the
   Tier-1 binary and break the `hosts/shared/*.h` contract.
2. **Nodes are `{widget, sub_kind, sub_index}`, not widget ids.** The sketch
   assumed a 1:1 widget↔node mapping, which LISTBOX / TREEVIEW / GRID / MENUBAR
   all violate (**G2**).
3. **macOS lands before win32**, for verifiability.
4. **6.0 exists at all.** The sketch listed no prerequisites; `abs_x/abs_y`
   being paint-dependent (**G1**) and Tab crossing frames (**G3**) both have to
   be fixed before a provider can be correct.
5. **AT-SPI is proposed for its own wave**, not folded in as "lowest priority".
6. **`set_labelled_by` added** (**G5**) — the sketch's `set_name` alone leaves the
   standard LABEL-next-to-INPUTBOX idiom unnamed.
7. **Value formatting is specified** (**G7**) — the sketch said "value reporting
   for continuous controls" without confronting the fact that
   `NEUI_PARAM_VALUE` is normalized and useless to a screen reader on its own.
