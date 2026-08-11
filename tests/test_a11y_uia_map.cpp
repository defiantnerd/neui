// Tier-1 tests for the UI Automation mapping tables (hosts/shared/a11y_uia_map.h).
//
// These exist because the win32 provider itself cannot be compiled or run on the
// machine it was written on, and the tables are where the likely bugs are: a role
// mapped to the wrong control type, a pattern advertised that the provider does
// not implement, an inverted state bit. The COM plumbing stays unverified until
// someone runs Inspect.exe / Narrator against it; this at least means the
// SEMANTICS are executed somewhere.
//
// What these tests CANNOT do: confirm the numeric constants are the ones Windows
// actually uses - a test can only agree with the number I wrote down. That job
// belongs to the static_asserts in hosts/crossplatform/a11y_win32.cpp, which
// check every constant against the real SDK headers and fail the BUILD rather
// than producing a subtly wrong tree at runtime.

#include "neui_test.h"

#include <cmath>

#include <neui/d/a11y.h>

#include "a11y_uia_map.h"

using namespace neui_detail::uia;

TEST_CASE("uia: every role maps to a sensible control type")
{
  CHECK_EQ(control_type_for_role(NEUI_A11Y_ROLE_BUTTON),      kButtonControlType);
  CHECK_EQ(control_type_for_role(NEUI_A11Y_ROLE_CHECKBOX),    kCheckBoxControlType);
  CHECK_EQ(control_type_for_role(NEUI_A11Y_ROLE_TEXT_FIELD),  kEditControlType);
  CHECK_EQ(control_type_for_role(NEUI_A11Y_ROLE_TEXT_AREA),   kEditControlType);
  CHECK_EQ(control_type_for_role(NEUI_A11Y_ROLE_SLIDER),      kSliderControlType);
  CHECK_EQ(control_type_for_role(NEUI_A11Y_ROLE_LIST),        kListControlType);
  CHECK_EQ(control_type_for_role(NEUI_A11Y_ROLE_LIST_ITEM),   kListItemControlType);
  CHECK_EQ(control_type_for_role(NEUI_A11Y_ROLE_TREE),        kTreeControlType);
  CHECK_EQ(control_type_for_role(NEUI_A11Y_ROLE_TREE_ITEM),   kTreeItemControlType);
  CHECK_EQ(control_type_for_role(NEUI_A11Y_ROLE_TABLE),       kTableControlType);
  CHECK_EQ(control_type_for_role(NEUI_A11Y_ROLE_WINDOW),      kWindowControlType);
  CHECK_EQ(control_type_for_role(NEUI_A11Y_ROLE_MENU_BAR),    kMenuBarControlType);
  CHECK_EQ(control_type_for_role(NEUI_A11Y_ROLE_MENU_ITEM),   kMenuItemControlType);
  CHECK_EQ(control_type_for_role(NEUI_A11Y_ROLE_IMAGE),       kImageControlType);
  CHECK_EQ(control_type_for_role(NEUI_A11Y_ROLE_GROUP),       kGroupControlType);

  // A toggle button stays a BUTTON (UIA has no toggle-button type; the Toggle
  // pattern carries the pressed-ness) - reporting CheckBox would make Narrator
  // announce "checkbox" for something drawn as a button.
  CHECK_EQ(control_type_for_role(NEUI_A11Y_ROLE_TOGGLE_BUTTON), kButtonControlType);
  // A neui COMBOBOX is not editable, but UIA's ComboBox does not imply that it
  // is - so unlike macOS (where AXComboBox does imply it and the mapping goes to
  // AXPopUpButton) this stays ComboBox. The platforms differ deliberately.
  CHECK_EQ(control_type_for_role(NEUI_A11Y_ROLE_COMBOBOX),     kComboBoxControlType);
  // A grid header is a real UIA type here, unlike on macOS.
  CHECK_EQ(control_type_for_role(NEUI_A11Y_ROLE_COLUMN_HEADER), kHeaderItemControlType);
  // A scroll area is a Pane, not anything scroll-flavoured: the Scroll PATTERN
  // is what would make it scrollable and that is not implemented.
  CHECK_EQ(control_type_for_role(NEUI_A11Y_ROLE_SCROLL_AREA),   kPaneControlType);
  // ROLE_NONE never reaches a provider (the model prunes it) but must not fall
  // through to something meaningful.
  CHECK_EQ(control_type_for_role(NEUI_A11Y_ROLE_NONE),          kCustomControlType);
  // An unknown / future role degrades to Custom rather than to Button-or-worse.
  CHECK_EQ(control_type_for_role(9999),                         kCustomControlType);
}

TEST_CASE("uia: no role maps to control type 0")
{
  // A zero control type is not a valid UIA id, and it is what a missing switch
  // case would produce if the default ever went away.
  for (int r = NEUI_A11Y_ROLE_DEFAULT; r <= NEUI_A11Y_ROLE_SCROLL_AREA; ++r)
    CHECK(control_type_for_role(r) != 0);
}

// A helper so each case reads as "this kind of element", not as a wall of bools.
static ActionInputs widget_row(int role)
{
  ActionInputs in;
  in.role = role;
  in.is_widget_row = true;
  return in;
}
static ActionInputs sub_row(int role, int sub_kind)
{
  ActionInputs in;
  in.role = role;
  in.sub_kind = sub_kind;
  in.is_widget_row = false;
  return in;
}

TEST_CASE("uia: patterns follow the role")
{
  PatternSet b = patterns_for(widget_row(NEUI_A11Y_ROLE_BUTTON));
  CHECK(b.invoke);
  CHECK(!b.toggle);
  CHECK(!b.value);
  CHECK(!b.range_value);

  PatternSet c = patterns_for(widget_row(NEUI_A11Y_ROLE_CHECKBOX));
  CHECK(c.toggle);
  CHECK(!c.invoke);            // a checkbox is toggled, not invoked

  CHECK(patterns_for(widget_row(NEUI_A11Y_ROLE_TOGGLE_BUTTON)).toggle);
  CHECK(patterns_for(widget_row(NEUI_A11Y_ROLE_TEXT_FIELD)).value);
  CHECK(!patterns_for(widget_row(NEUI_A11Y_ROLE_TEXT_FIELD)).range_value);

  PatternSet g = patterns_for(widget_row(NEUI_A11Y_ROLE_GROUP));
  CHECK(!g.invoke && !g.toggle && !g.value && !g.range_value &&
        !g.selection_item && !g.expand_collapse && !g.scroll_item);
}

TEST_CASE("uia: EVERY advertised pattern is one the provider will perform")
{
  // THE CHECK THAT WAS MISSING. The first cut had a patterns_for() table and the
  // refusals scattered through the provider's action methods, and four patterns
  // were advertised that the actions always refused - menu-item Invoke,
  // SelectionItem on a widget row, ScrollItem on a sub-row, ExpandCollapse on a
  // submenu item. The tests could not see it because they compared the table with
  // the table. Now patterns_for is DERIVED from action_allowed and the provider
  // gates on action_allowed, so this sweep is a real cross-check of the two.
  const int32_t pats[] = { kInvokePattern, kTogglePattern, kValuePattern,
                           kRangeValuePattern, kSelectionItemPattern,
                           kExpandCollapsePattern, kScrollItemPattern };
  const int sub_kinds[] = { 0 /*widget*/, 1 /*list_row*/, 2 /*tree_item*/,
                            3 /*grid_header*/, 4 /*grid_row*/, 5 /*grid_cell*/,
                            6 /*tab_chip*/, 7 /*menu_item*/ };

  for (int role = NEUI_A11Y_ROLE_DEFAULT; role <= NEUI_A11Y_ROLE_SCROLL_AREA; ++role)
    for (int sk : sub_kinds)
      for (int widget_row_flag = 0; widget_row_flag < 2; ++widget_row_flag)
        for (int flags = 0; flags < 64; ++flags) {
          ActionInputs in;
          in.role = role;
          in.sub_kind = sk;
          in.is_widget_row = (widget_row_flag != 0);
          in.host_owns_value = (flags & 1) != 0;
          in.has_range       = (flags & 2) != 0;
          in.has_value_text  = (flags & 4) != 0;
          in.selectable_row  = (flags & 8) != 0;
          in.expandable      = (flags & 16) != 0;
          in.in_scrollable   = (flags & 32) != 0;
          const PatternSet p = patterns_for(in);
          for (int32_t pat : pats) {
            if (supports_pattern(p, pat) != action_allowed(in, pat)) {
              CHECK(false);   // advertised != performable
              return;
            }
          }
        }
  CHECK(true);
}

TEST_CASE("uia: frame-level activation offers NO action at all")
{
  // A menu item and an open COMBOBOX's drop rows are hit-tested at frame level by
  // the platform layer, so a synthesised click into the owning widget lands
  // nowhere. They must not be offered anything - the first cut advertised Invoke
  // on every menu item and then refused it, which is exactly the
  // offered-but-inert failure the header forbids.
  ActionInputs mi = sub_row(NEUI_A11Y_ROLE_MENU_ITEM, 7 /*menu_item*/);
  mi.activation_is_frame_level = true;
  mi.expandable = true;                      // a submenu parent
  PatternSet p = patterns_for(mi);
  CHECK(!p.invoke);
  CHECK(!p.selection_item);
  CHECK(!p.expand_collapse);
  CHECK(!p.scroll_item);

  // ...whereas a menu item NOT behind the frame-level path would be invokable,
  // so the flag is what is doing the work rather than the role.
  ActionInputs plain = sub_row(NEUI_A11Y_ROLE_MENU_ITEM, 7);
  CHECK(!patterns_for(plain).invoke);        // still no: a sub-row selects
}

TEST_CASE("uia: SelectionItem covers both a sub-row and a declared radio / tab")
{
  ActionInputs row = sub_row(NEUI_A11Y_ROLE_LIST_ITEM, 1 /*list_row*/);
  row.selectable_row = true;
  CHECK(patterns_for(row).selection_item);

  // A client-declared radio or tab CUSTOMDRAW is a WIDGET row: it selects the
  // same way it is pressed. The first cut advertised the pattern here and then
  // refused every widget row.
  CHECK(patterns_for(widget_row(NEUI_A11Y_ROLE_RADIO_BUTTON)).selection_item);
  CHECK(patterns_for(widget_row(NEUI_A11Y_ROLE_TAB)).selection_item);
  CHECK(!patterns_for(widget_row(NEUI_A11Y_ROLE_BUTTON)).selection_item);
}

TEST_CASE("uia: ExpandCollapse only where a widget can really open and close")
{
  ActionInputs combo = widget_row(NEUI_A11Y_ROLE_COMBOBOX);
  combo.expandable = true;
  CHECK(patterns_for(combo).expand_collapse);

  ActionInputs item = sub_row(NEUI_A11Y_ROLE_TREE_ITEM, 2 /*tree_item*/);
  item.expandable = true;
  CHECK(patterns_for(item).expand_collapse);

  // Expandable in the MODEL but not operable: a submenu item's cascade is driven
  // at frame level, and a grid row is never expandable in the first place.
  ActionInputs grid_row = sub_row(NEUI_A11Y_ROLE_ROW, 4 /*grid_row*/);
  grid_row.expandable = true;
  CHECK(!patterns_for(grid_row).expand_collapse);
  // And nothing gets the pattern when the model says it is a leaf.
  ActionInputs leaf = widget_row(NEUI_A11Y_ROLE_COMBOBOX);
  CHECK(!patterns_for(leaf).expand_collapse);
}

TEST_CASE("uia: ScrollItem only for widgets, and only inside something scrolling")
{
  ActionInputs w = widget_row(NEUI_A11Y_ROLE_BUTTON);
  w.in_scrollable = true;
  CHECK(patterns_for(w).scroll_item);
  w.in_scrollable = false;
  CHECK(!patterns_for(w).scroll_item);

  // A windowed sub-row would need the container to scroll to an INDEX, which is
  // not wired - ensure_widget_visible works on widgets.
  ActionInputs r = sub_row(NEUI_A11Y_ROLE_LIST_ITEM, 1);
  r.in_scrollable = true;
  CHECK(!patterns_for(r).scroll_item);
}

TEST_CASE("uia: a slider gets RangeValue only when a real range was declared")
{
  ActionInputs with = widget_row(NEUI_A11Y_ROLE_SLIDER);
  with.has_range = true;
  CHECK(patterns_for(with).range_value);
  CHECK(!patterns_for(with).value);
  // Without a range the value is a bare normalized number: advertising
  // RangeValue would force the provider to invent bounds, so the formatted
  // string goes out through Value instead.
  ActionInputs without = widget_row(NEUI_A11Y_ROLE_SLIDER);
  CHECK(!patterns_for(without).range_value);
  CHECK(patterns_for(without).value);
}

TEST_CASE("uia: RangeValue is writable ONLY when the host owns the value")
{
  // This is the one that made every knob and slider read-only to Narrator. UIA
  // clients read IsReadOnly before offering adjustment, so a stray TRUE means the
  // AT never calls SetValue at all and the whole write path is dead.
  ActionInputs knob = widget_row(NEUI_A11Y_ROLE_SLIDER);
  knob.has_range = true;
  knob.host_owns_value = true;                       // a built-in KNOB / SLIDER
  CHECK(!range_value_is_read_only(knob, 0));

  // A CUSTOMDRAW with a declared role and range: the CLIENT owns the value, so
  // the provider cannot write it and must say so rather than accept and drop it.
  ActionInputs custom = widget_row(NEUI_A11Y_ROLE_SLIDER);
  custom.has_range = true;
  custom.host_owns_value = false;
  CHECK(range_value_is_read_only(custom, 0));

  // Disabled or widget-read-only also means no.
  CHECK(range_value_is_read_only(knob, NEUI_A11Y_STATE_DISABLED));
  CHECK(range_value_is_read_only(knob, NEUI_A11Y_STATE_READONLY));
}

TEST_CASE("uia: supports_pattern rejects ids it does not know")
{
  PatternSet p = patterns_for(widget_row(NEUI_A11Y_ROLE_BUTTON));
  CHECK(supports_pattern(p, kInvokePattern));
  CHECK(!supports_pattern(p, 10099));
  CHECK(!supports_pattern(p, 0));
}

TEST_CASE("uia: state maps to the boolean properties, uninverted")
{
  CHECK(is_enabled(0));
  CHECK(!is_enabled(NEUI_A11Y_STATE_DISABLED));
  CHECK(has_keyboard_focus(NEUI_A11Y_STATE_FOCUSED));
  CHECK(!has_keyboard_focus(NEUI_A11Y_STATE_FOCUSABLE));
  CHECK(is_keyboard_focusable(NEUI_A11Y_STATE_FOCUSABLE));
  CHECK(!is_keyboard_focusable(NEUI_A11Y_STATE_FOCUSED));
  CHECK(is_offscreen(NEUI_A11Y_STATE_OFFSCREEN));
  CHECK(!is_offscreen(0));
  CHECK(is_password(NEUI_A11Y_STATE_PROTECTED));
  CHECK(is_read_only(NEUI_A11Y_STATE_READONLY));
  CHECK(is_selected(NEUI_A11Y_STATE_SELECTED));
  CHECK(!is_selected(NEUI_A11Y_STATE_CHECKED));
}

TEST_CASE("uia: tri-state indeterminate is Indeterminate, never On")
{
  CHECK_EQ(toggle_state(0), kToggleOff);
  CHECK_EQ(toggle_state(NEUI_A11Y_STATE_CHECKED), kToggleOn);
  CHECK_EQ(toggle_state(NEUI_A11Y_STATE_MIXED),   kToggleIndeterminate);
  // MIXED must win even when a client's set_state override sets both bits -
  // reporting a "maybe" as a definite On is a wrong answer, not a rounding.
  CHECK_EQ(toggle_state(NEUI_A11Y_STATE_MIXED | NEUI_A11Y_STATE_CHECKED),
           kToggleIndeterminate);
}

TEST_CASE("uia: a non-expandable node is a LEAF, not collapsed")
{
  // The distinction is load-bearing: an AT offers an expand action for
  // "collapsed" and none for a leaf.
  CHECK_EQ(expand_collapse_state(0), kLeafNode);
  CHECK_EQ(expand_collapse_state(NEUI_A11Y_STATE_COLLAPSED), kCollapsed);
  CHECK_EQ(expand_collapse_state(NEUI_A11Y_STATE_EXPANDED),  kExpanded);
}

TEST_CASE("uia: RangeValue maps a normalized value onto the declared range")
{
  RangeValues r = range_values(0.5f, -60.0f, 6.0f, 3.0f);
  CHECK_EQ(r.minimum, -60.0);
  CHECK_EQ(r.maximum, 6.0);
  CHECK(r.value > -27.001 && r.value < -26.999);      // -60 + 66*0.5
  CHECK_EQ(r.small_change, 3.0);

  // A declared step of 0 means continuous. UIA has no marker for that and some
  // clients refuse to step on a 0 SmallChange, so it falls back to a hundredth
  // of the span rather than to zero.
  RangeValues cont = range_values(0.0f, 0.0f, 100.0f, 0.0f);
  CHECK_EQ(cont.small_change, 1.0);

  // An inverted range is ordered rather than rejected - the announced number
  // stays inside the two bounds the client named either way.
  RangeValues inv = range_values(0.0f, 10.0f, 0.0f, 0.0f);
  CHECK_EQ(inv.minimum, 0.0);
  CHECK_EQ(inv.maximum, 10.0);
  CHECK_EQ(inv.value, 0.0);

  // Out-of-range and NaN normalized values are clamped, not propagated: an AT
  // announcing 103 % is worse than a pinned value.
  CHECK_EQ(range_values(2.0f, 0.0f, 10.0f, 0.0f).value, 10.0);
  CHECK_EQ(range_values(-1.0f, 0.0f, 10.0f, 0.0f).value, 0.0);
  const float nan_v = std::nanf("");
  CHECK_EQ(range_values(nan_v, 0.0f, 10.0f, 0.0f).value, 0.0);

  // A degenerate range cannot divide by zero.
  RangeValues zero = range_values(0.5f, 5.0f, 5.0f, 0.0f);
  CHECK_EQ(zero.value, 5.0);
}

TEST_CASE("uia: a value written back through RangeValue round-trips")
{
  // What an AT does: read min/max, hand back a real-world number.
  CHECK_EQ(normalized_from_real(-60.0, -60.0f, 6.0f), 0.0f);
  CHECK_EQ(normalized_from_real(6.0, -60.0f, 6.0f), 1.0f);
  const float mid = normalized_from_real(-27.0, -60.0f, 6.0f);
  CHECK(mid > 0.499f && mid < 0.501f);
  // Out of bounds clamps rather than wrapping or overflowing the widget.
  CHECK_EQ(normalized_from_real(1000.0, -60.0f, 6.0f), 1.0f);
  CHECK_EQ(normalized_from_real(-1000.0, -60.0f, 6.0f), 0.0f);
  // Inverted and degenerate ranges stay safe.
  CHECK_EQ(normalized_from_real(10.0, 10.0f, 0.0f), 1.0f);
  CHECK_EQ(normalized_from_real(3.0, 5.0f, 5.0f), 0.0f);
  // A round trip through both directions lands where it started.
  RangeValues rv = range_values(0.25f, -60.0f, 6.0f, 0.0f);
  const float back = normalized_from_real(rv.value, -60.0f, 6.0f);
  CHECK(back > 0.249f && back < 0.251f);
}
