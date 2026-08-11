#pragma once

#include <cstdint>
#include <string>

#include <neui/d/a11y.h>

// UI Automation mapping tables - the portable half of the win32 accessibility
// provider.
//
// WHY THIS FILE EXISTS. The win32 provider cannot be compiled or run on the
// machine it is being written on, so it ships unverified (see
// plans/accessibility.md 6.4). The COM plumbing genuinely cannot be checked from
// here - but the part most likely to be WRONG is not the plumbing, it is these
// tables: a role mapped to the wrong control type, a state bit mapped to the
// wrong property, a pattern advertised that the provider does not implement.
// Pulling them out of the .cpp puts them where Tier-1 tests run on any platform,
// which turns the unverifiable surface into just the COM glue.
//
// TWO SAFEGUARDS, and the second is the important one:
//   1. Tier-1 tests (tests/test_a11y_uia_map.cpp) execute every mapping here.
//      That catches a role falling through to the wrong default, a pattern set
//      that disagrees with the role, an inverted state bit.
//   2. The provider (hosts/crossplatform/a11y_win32.cpp) STATIC-ASSERTS every
//      constant below against the real UIAutomation headers. A test can only
//      confirm the number I wrote down; the static_asserts confirm the number is
//      the one Windows actually uses, and they fail the BUILD rather than
//      producing a subtly wrong tree at runtime. That is the whole reason the
//      constants are duplicated here instead of being used from <UIAutomation.h>.
//
// NO WINDOWS HEADERS HERE ON PURPOSE - this must compile everywhere, exactly like
// the rest of hosts/shared.

namespace neui_detail
{
namespace uia
{
  // ---- Control types (UIA_*ControlTypeId) ----------------------------------
  // Stable, documented values. Verified against the SDK by static_assert in the
  // provider.
  enum ControlType : int32_t
  {
    kButtonControlType      = 50000,
    kCheckBoxControlType    = 50002,
    kComboBoxControlType    = 50003,
    kEditControlType        = 50004,
    kImageControlType       = 50006,
    kListItemControlType    = 50007,
    kListControlType        = 50008,
    kMenuControlType        = 50009,
    kMenuBarControlType     = 50010,
    kMenuItemControlType    = 50011,
    kProgressBarControlType = 50012,
    kRadioButtonControlType = 50013,
    kSliderControlType      = 50015,
    kTabControlType         = 50018,
    kTabItemControlType     = 50019,
    kTextControlType        = 50020,
    kTreeControlType        = 50023,
    kTreeItemControlType    = 50024,
    kCustomControlType      = 50025,
    kGroupControlType       = 50026,
    kDataItemControlType    = 50029,
    kWindowControlType      = 50032,
    kPaneControlType        = 50033,
    kHeaderItemControlType  = 50035,
    kTableControlType       = 50036,
    kSeparatorControlType   = 50038
  };

  // ---- Patterns (UIA_*PatternId) ------------------------------------------
  enum PatternId : int32_t
  {
    kInvokePattern         = 10000,
    kValuePattern          = 10002,
    kRangeValuePattern     = 10003,
    kExpandCollapsePattern = 10005,
    kSelectionItemPattern  = 10010,
    kTogglePattern         = 10015,
    kScrollItemPattern     = 10017
  };

  // ---- ToggleState / ExpandCollapseState ----------------------------------
  enum ToggleState : int32_t
  {
    kToggleOff           = 0,
    kToggleOn            = 1,
    kToggleIndeterminate = 2
  };

  enum ExpandCollapseState : int32_t
  {
    kCollapsed         = 0,
    kExpanded          = 1,
    kPartiallyExpanded = 2,
    kLeafNode          = 3
  };

  // ---- Role -> control type ------------------------------------------------
  //
  // Choices worth stating, because they are judgement rather than lookup:
  //   * KNOB / SLIDER -> Slider. The model already collapses KNOB to
  //     ROLE_SLIDER; UIA has no knob either, and Slider is the control type
  //     Narrator knows how to drive.
  //   * A neui COMBOBOX has no text entry, but UIA's ComboBox does not imply
  //     one (that is the Value pattern's business), so ComboBox is right here -
  //     unlike macOS, where AXComboBox does imply editability and the mapping
  //     goes to AXPopUpButton instead. The two platforms differ on purpose.
  //   * GRID -> Table, its rows -> DataItem, its cells -> Custom. Cell is not a
  //     UIA control type; DataItem/Custom with a Name is what a grid without the
  //     Table/GridItem patterns can honestly claim (those patterns are deferred -
  //     see the header docs).
  //   * A grid HEADER cell -> HeaderItem, which is a real UIA type, unlike on
  //     macOS where the honest answer was a button with a role description.
  //   * ROLE_METER -> ProgressBar. UIA has no meter; a progress bar is the only
  //     read-only-value control type it offers.
  //   * ROLE_NONE never reaches here (the model prunes it), but map it to Custom
  //     rather than assert - a provider must not crash on unexpected input.
  inline int32_t control_type_for_role(int role)
  {
    switch (role) {
      case NEUI_A11Y_ROLE_WINDOW:        return kWindowControlType;
      case NEUI_A11Y_ROLE_STATIC_TEXT:   return kTextControlType;
      case NEUI_A11Y_ROLE_BUTTON:        return kButtonControlType;
      // A toggle button is a Button in UIA with the Toggle pattern on it; there
      // is no separate control type, and reporting CheckBox would make Narrator
      // announce "checkbox" for something drawn as a button.
      case NEUI_A11Y_ROLE_TOGGLE_BUTTON: return kButtonControlType;
      case NEUI_A11Y_ROLE_CHECKBOX:      return kCheckBoxControlType;
      case NEUI_A11Y_ROLE_RADIO_BUTTON:  return kRadioButtonControlType;
      case NEUI_A11Y_ROLE_SLIDER:        return kSliderControlType;
      case NEUI_A11Y_ROLE_PROGRESS:      return kProgressBarControlType;
      case NEUI_A11Y_ROLE_METER:         return kProgressBarControlType;
      case NEUI_A11Y_ROLE_TEXT_FIELD:    return kEditControlType;
      case NEUI_A11Y_ROLE_TEXT_AREA:     return kEditControlType;
      case NEUI_A11Y_ROLE_LIST:          return kListControlType;
      case NEUI_A11Y_ROLE_LIST_ITEM:     return kListItemControlType;
      case NEUI_A11Y_ROLE_COMBOBOX:      return kComboBoxControlType;
      case NEUI_A11Y_ROLE_TREE:          return kTreeControlType;
      case NEUI_A11Y_ROLE_TREE_ITEM:     return kTreeItemControlType;
      case NEUI_A11Y_ROLE_TABLE:         return kTableControlType;
      case NEUI_A11Y_ROLE_ROW:           return kDataItemControlType;
      case NEUI_A11Y_ROLE_CELL:          return kCustomControlType;
      case NEUI_A11Y_ROLE_COLUMN_HEADER: return kHeaderItemControlType;
      case NEUI_A11Y_ROLE_TAB_LIST:      return kTabControlType;
      case NEUI_A11Y_ROLE_TAB:           return kTabItemControlType;
      case NEUI_A11Y_ROLE_MENU_BAR:      return kMenuBarControlType;
      case NEUI_A11Y_ROLE_MENU:          return kMenuControlType;
      case NEUI_A11Y_ROLE_MENU_ITEM:     return kMenuItemControlType;
      case NEUI_A11Y_ROLE_IMAGE:         return kImageControlType;
      // A scroll area is a Pane: UIA's Scroll PATTERN is what makes it
      // scrollable, and that pattern is not implemented yet (deferred), so
      // claiming ScrollBar or anything scroll-flavoured would overstate it.
      case NEUI_A11Y_ROLE_SCROLL_AREA:   return kPaneControlType;
      case NEUI_A11Y_ROLE_GROUP:         return kGroupControlType;
      case NEUI_A11Y_ROLE_NONE:
      default:                           return kCustomControlType;
    }
  }

  // ---- Which patterns a role supports --------------------------------------
  //
  // THE RULE: advertise a pattern only if the provider really implements it for
  // that role. A client that asks for an advertised pattern and gets E_NOINTERFACE
  // (or a no-op) is worse off than one told the pattern is absent - it has already
  // told the user the control can be operated.
  //
  // `has_range` matters because RangeValue without a declared min/max would have
  // to invent bounds, and a screen reader announcing a percentage as if it were
  // the real value is the G7 problem all over again.
  struct PatternSet
  {
    bool invoke         = false;
    bool toggle         = false;
    bool value          = false;   // IValueProvider (read-only text value)
    bool range_value    = false;   // IRangeValueProvider
    bool selection_item = false;
    bool expand_collapse = false;
    bool scroll_item    = false;   // "bring me into view"
  };

  inline PatternSet patterns_for(int role, bool has_range, bool has_value_text,
                                 bool selectable, bool expandable,
                                 bool in_scrollable_container)
  {
    PatternSet p;
    switch (role) {
      case NEUI_A11Y_ROLE_BUTTON:
      case NEUI_A11Y_ROLE_COLUMN_HEADER:
      case NEUI_A11Y_ROLE_MENU_ITEM:
        p.invoke = true;
        break;
      case NEUI_A11Y_ROLE_CHECKBOX:
      case NEUI_A11Y_ROLE_TOGGLE_BUTTON:
        p.toggle = true;
        break;
      case NEUI_A11Y_ROLE_RADIO_BUTTON:
      case NEUI_A11Y_ROLE_TAB:
      case NEUI_A11Y_ROLE_LIST_ITEM:
      case NEUI_A11Y_ROLE_TREE_ITEM:
      case NEUI_A11Y_ROLE_ROW:
        p.selection_item = true;
        break;
      case NEUI_A11Y_ROLE_SLIDER:
        // RangeValue only with a real range. Without one the value is a bare
        // normalized number, and Value (the string the model formatted, e.g.
        // "42 %") is the honest thing to expose.
        p.range_value = has_range;
        p.value       = !has_range;
        break;
      case NEUI_A11Y_ROLE_PROGRESS:
      case NEUI_A11Y_ROLE_METER:
        p.range_value = has_range;
        p.value       = !has_range;
        break;
      case NEUI_A11Y_ROLE_TEXT_FIELD:
      case NEUI_A11Y_ROLE_TEXT_AREA:
        p.value = true;
        break;
      case NEUI_A11Y_ROLE_COMBOBOX:
        p.value = has_value_text;   // the selected entry's text, when there is one
        break;
      default:
        break;
    }
    // Orthogonal to the role: anything the model says is expandable gets
    // ExpandCollapse, anything selectable gets SelectionItem, and anything inside
    // a scrolling container gets ScrollItem so an AT can bring it into view.
    if (expandable) p.expand_collapse = true;
    if (selectable) p.selection_item  = true;
    if (in_scrollable_container) p.scroll_item = true;
    return p;
  }

  inline bool supports_pattern(const PatternSet& p, int32_t pattern_id)
  {
    switch (pattern_id) {
      case kInvokePattern:         return p.invoke;
      case kTogglePattern:         return p.toggle;
      case kValuePattern:          return p.value;
      case kRangeValuePattern:     return p.range_value;
      case kSelectionItemPattern:  return p.selection_item;
      case kExpandCollapsePattern: return p.expand_collapse;
      case kScrollItemPattern:     return p.scroll_item;
      default:                     return false;
    }
  }

  // ---- State -> UIA property values ---------------------------------------

  inline bool is_enabled(uint32_t state)
  { return (state & NEUI_A11Y_STATE_DISABLED) == 0; }

  inline bool has_keyboard_focus(uint32_t state)
  { return (state & NEUI_A11Y_STATE_FOCUSED) != 0; }

  inline bool is_keyboard_focusable(uint32_t state)
  { return (state & NEUI_A11Y_STATE_FOCUSABLE) != 0; }

  inline bool is_offscreen(uint32_t state)
  { return (state & NEUI_A11Y_STATE_OFFSCREEN) != 0; }

  inline bool is_password(uint32_t state)
  { return (state & NEUI_A11Y_STATE_PROTECTED) != 0; }

  inline bool is_read_only(uint32_t state)
  { return (state & NEUI_A11Y_STATE_READONLY) != 0; }

  inline bool is_selected(uint32_t state)
  { return (state & NEUI_A11Y_STATE_SELECTED) != 0; }

  // MIXED must win over CHECKED: a tri-state's indeterminate reported as On is a
  // wrong answer, not a rounding. (The model already guarantees the two bits are
  // not both set, but a client's set_state override can set whatever it likes,
  // so the ordering here is what makes that safe.)
  inline int32_t toggle_state(uint32_t state)
  {
    if (state & NEUI_A11Y_STATE_MIXED)   return kToggleIndeterminate;
    if (state & NEUI_A11Y_STATE_CHECKED) return kToggleOn;
    return kToggleOff;
  }

  // A node that is not expandable at all is a LEAF, which is a different answer
  // from "collapsed" - an AT offers an expand action for the latter.
  inline int32_t expand_collapse_state(uint32_t state)
  {
    if (state & NEUI_A11Y_STATE_EXPANDED)  return kExpanded;
    if (state & NEUI_A11Y_STATE_COLLAPSED) return kCollapsed;
    return kLeafNode;
  }

  // ---- RangeValue numbers --------------------------------------------------
  //
  // The model keeps the value normalized [0..1] and the client's declared range
  // separately, because that is what the widget stores. UIA wants real-world
  // numbers on the RangeValue pattern, so this is the one place the mapping
  // happens. An inverted range is ordered rather than rejected, matching
  // a11y_format_value - the announced number stays inside the two bounds the
  // client named either way.
  struct RangeValues
  {
    double value = 0.0, minimum = 0.0, maximum = 1.0, small_change = 0.0;
  };

  inline RangeValues range_values(float normalized, float vmin, float vmax,
                                  float vstep)
  {
    RangeValues r;
    float lo = vmin, hi = vmax;
    if (lo > hi) { float t = lo; lo = hi; hi = t; }
    if (!(normalized == normalized)) normalized = 0.0f;   // NaN
    if (normalized < 0.0f) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;
    r.minimum = static_cast<double>(lo);
    r.maximum = static_cast<double>(hi);
    r.value   = static_cast<double>(lo) +
                (static_cast<double>(hi) - static_cast<double>(lo)) *
                static_cast<double>(normalized);
    // 0 = continuous. UIA has no "continuous" marker, and a 0 SmallChange makes
    // some clients refuse to step, so fall back to a hundredth of the span -
    // the same order of magnitude the arrow keys use (a tenth) without
    // pretending to a precision the client never declared.
    const double span = r.maximum - r.minimum;
    r.small_change = (vstep > 0.0f) ? static_cast<double>(vstep) : (span / 100.0);
    return r;
  }

  // Normalized position for a real-world value an AT wrote back through
  // RangeValue::SetValue. Clamped, because a client can send anything.
  inline float normalized_from_real(double real, float vmin, float vmax)
  {
    float lo = vmin, hi = vmax;
    if (lo > hi) { float t = lo; lo = hi; hi = t; }
    const double span = static_cast<double>(hi) - static_cast<double>(lo);
    if (!(span > 0.0)) return 0.0f;
    double n = (real - static_cast<double>(lo)) / span;
    if (!(n == n)) n = 0.0;
    if (n < 0.0) n = 0.0;
    if (n > 1.0) n = 1.0;
    return static_cast<float>(n);
  }
}
}
