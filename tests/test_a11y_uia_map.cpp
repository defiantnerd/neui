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

TEST_CASE("uia: patterns follow the role, and only ones we implement")
{
  PatternSet b = patterns_for(NEUI_A11Y_ROLE_BUTTON, false, false, false, false, false);
  CHECK(b.invoke);
  CHECK(!b.toggle);
  CHECK(!b.value);
  CHECK(!b.range_value);

  PatternSet c = patterns_for(NEUI_A11Y_ROLE_CHECKBOX, false, false, false, false, false);
  CHECK(c.toggle);
  CHECK(!c.invoke);          // a checkbox is toggled, not invoked

  PatternSet tb = patterns_for(NEUI_A11Y_ROLE_TOGGLE_BUTTON, false, false, false, false, false);
  CHECK(tb.toggle);

  PatternSet li = patterns_for(NEUI_A11Y_ROLE_LIST_ITEM, false, false, false, false, false);
  CHECK(li.selection_item);

  PatternSet e = patterns_for(NEUI_A11Y_ROLE_TEXT_FIELD, false, false, false, false, false);
  CHECK(e.value);
  CHECK(!e.range_value);

  PatternSet g = patterns_for(NEUI_A11Y_ROLE_GROUP, false, false, false, false, false);
  CHECK(!g.invoke && !g.toggle && !g.value && !g.range_value &&
        !g.selection_item && !g.expand_collapse && !g.scroll_item);
}

TEST_CASE("uia: a slider gets RangeValue only when a real range was declared")
{
  // With a range: real-world numbers, so RangeValue is meaningful.
  PatternSet with = patterns_for(NEUI_A11Y_ROLE_SLIDER, true, false, false, false, false);
  CHECK(with.range_value);
  CHECK(!with.value);
  // Without: the value is a bare normalized number. Advertising RangeValue would
  // force the provider to invent bounds; the formatted string is honest instead.
  PatternSet without = patterns_for(NEUI_A11Y_ROLE_SLIDER, false, false, false, false, false);
  CHECK(!without.range_value);
  CHECK(without.value);
}

TEST_CASE("uia: expandable / selectable / scrollable add patterns to any role")
{
  PatternSet p = patterns_for(NEUI_A11Y_ROLE_GROUP, false, false,
                              /*selectable*/true, /*expandable*/true,
                              /*in_scrollable*/true);
  CHECK(p.selection_item);
  CHECK(p.expand_collapse);
  CHECK(p.scroll_item);
  // ...and they do not turn into an Invoke claim.
  CHECK(!p.invoke);
}

TEST_CASE("uia: supports_pattern agrees with the set, and rejects the rest")
{
  PatternSet p = patterns_for(NEUI_A11Y_ROLE_BUTTON, false, false, false, false, false);
  CHECK(supports_pattern(p, kInvokePattern));
  CHECK(!supports_pattern(p, kTogglePattern));
  CHECK(!supports_pattern(p, kValuePattern));
  CHECK(!supports_pattern(p, kRangeValuePattern));
  CHECK(!supports_pattern(p, kSelectionItemPattern));
  CHECK(!supports_pattern(p, kExpandCollapsePattern));
  CHECK(!supports_pattern(p, kScrollItemPattern));
  // An unknown pattern id must be refused, not fall through to true - a client
  // that gets a non-null provider for a pattern we do not implement is worse off
  // than one told the pattern is absent.
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
