#pragma once

#include <cstdint>
#include <string>

#include <neui/d/a11y.h>

// UI Automation mapping tables - the portable half of the win32 accessibility
// provider.
//
// WHY THIS FILE EXISTS. The win32 provider was written on a machine that could
// not compile or run it, so it shipped unverified (see plans/accessibility.md
// 6.4; it has since been run on Windows). The COM plumbing could not be checked
// from there - but the part most likely to be WRONG is not the plumbing, it is
// these tables: a role mapped to the wrong control type, a state bit mapped to
// the wrong property, a pattern advertised that the provider does not implement.
// Pulling them out of the .cpp puts them where Tier-1 tests run on any platform,
// which turned the unverifiable surface into just the COM glue.
//
// That call was vindicated by the first Windows run: nothing here was wrong, and
// the single defect it did find was in the hand-written glue this file exists to
// shrink. The tables stay here - they are still the only part of the provider
// that a non-Windows machine can execute, which is what keeps it editable.
//
// TWO SAFEGUARDS, and the second is the important one:
//   1. Tier-1 tests (tests/test_a11y_uia_map.cpp) execute every mapping here.
//      That catches a role falling through to the wrong default, an inverted
//      state bit, and - since patterns_for is now DERIVED from action_allowed and
//      the provider gates on the same predicate - a pattern advertised that the
//      provider will refuse. The first cut had those as two independent lists and
//      the tests compared the table with the table, which is how four
//      advertised-but-refused patterns got through.
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
  // The one A11ySubKind value this header needs. Mirrored rather than including
  // a11y_tree.h, so the mapping tables stay dependency-free; static_asserted
  // against the enum in a11y_win32.cpp so the two cannot drift.
  enum { A11ySubKindTreeItem = 2 };

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

  // ---- Which patterns an element advertises, and which it can perform -------
  //
  // THE RULE: advertise a pattern only if the provider really implements it for
  // that element. A client that asks for an advertised pattern and gets
  // UIA_E_INVALIDOPERATION is worse off than one told the pattern is absent - it
  // has already told the user the control can be operated.
  //
  // Keeping that rule was the problem. Revision 1 had a `patterns_for` here and
  // the refusals scattered through the provider's action methods, and review
  // found FOUR patterns advertised that the actions always refused (menu-item
  // Invoke, SelectionItem on a widget row, ScrollItem on a sub-row,
  // ExpandCollapse on a submenu item). The Tier-1 tests could not catch any of
  // them, because they tested the table against the table. So there is now ONE
  // predicate - action_allowed - and `patterns_for` is derived from it, while the
  // provider gates every action on it. They cannot disagree, and the tests check
  // named cases against it rather than against themselves.
  struct ActionInputs
  {
    int  role      = NEUI_A11Y_ROLE_GROUP;
    int  sub_kind  = 0;             // A11ySubKind
    bool is_widget_row = true;      // false = a sub-element (list row, cell, ...)

    // The HOST can write this widget's value (a built-in KNOB / SLIDER). False
    // for a CUSTOMDRAW whose value lives in the client's own state - writing the
    // attribute there would put the two out of step, so RangeValue has to be
    // read-only rather than accept a write it cannot honour.
    bool host_owns_value = false;
    bool has_range       = false;
    bool has_value_text  = false;

    bool selectable_row  = false;   // a row / item / chip that selection applies to
    bool expandable      = false;   // the model reported EXPANDED or COLLAPSED
    bool in_scrollable   = false;   // some ancestor scrolls

    // Activation is hit-tested at FRAME level by the platform layer rather than
    // by the owning widget - menu items, and an open COMBOBOX's drop rows. A
    // synthesised click into the widget lands nowhere, so no action can be
    // offered. Both remain fully keyboard-operable.
    bool activation_is_frame_level = false;
  };

  inline bool action_allowed(const ActionInputs& in, int32_t pattern_id)
  {
    if (in.activation_is_frame_level) {
      // Nothing actionable, but the STATE-only patterns still answer honestly.
      return false;
    }
    switch (pattern_id) {
      case kInvokePattern:
        if (!in.is_widget_row) return false;    // a sub-row selects, it does not invoke
        return in.role == NEUI_A11Y_ROLE_BUTTON ||
               in.role == NEUI_A11Y_ROLE_COLUMN_HEADER;
      case kTogglePattern:
        if (!in.is_widget_row) return false;
        return in.role == NEUI_A11Y_ROLE_CHECKBOX ||
               in.role == NEUI_A11Y_ROLE_TOGGLE_BUTTON;
      case kSelectionItemPattern:
        // A sub-row is selected by a synthesised click; a WIDGET row (a declared
        // radio / tab CUSTOMDRAW) by the same key dispatch a press uses.
        if (!in.is_widget_row) return in.selectable_row;
        return in.role == NEUI_A11Y_ROLE_RADIO_BUTTON ||
               in.role == NEUI_A11Y_ROLE_TAB;
      case kValuePattern:
        if (!in.is_widget_row) return false;
        if (in.role == NEUI_A11Y_ROLE_TEXT_FIELD ||
            in.role == NEUI_A11Y_ROLE_TEXT_AREA) return true;
        if (in.role == NEUI_A11Y_ROLE_COMBOBOX)  return in.has_value_text;
        // A value control with no declared range: the formatted string is all
        // there is, and RangeValue would have to invent bounds.
        if (in.role == NEUI_A11Y_ROLE_SLIDER || in.role == NEUI_A11Y_ROLE_PROGRESS ||
            in.role == NEUI_A11Y_ROLE_METER)     return !in.has_range;
        return false;
      case kRangeValuePattern:
        if (!in.is_widget_row || !in.has_range) return false;
        return in.role == NEUI_A11Y_ROLE_SLIDER ||
               in.role == NEUI_A11Y_ROLE_PROGRESS ||
               in.role == NEUI_A11Y_ROLE_METER;
      case kExpandCollapsePattern:
        // Only a COMBOBOX (widget row) and a TREE ITEM can actually be opened or
        // closed - those are the two the widgets have keys for. A submenu item is
        // expandable in the MODEL but its cascade is driven at frame level.
        if (!in.expandable) return false;
        if (in.is_widget_row) return in.role == NEUI_A11Y_ROLE_COMBOBOX;
        return in.sub_kind == static_cast<int>(A11ySubKindTreeItem);
      case kScrollItemPattern:
        // ensure_widget_visible works on WIDGETS. Bringing a windowed sub-row
        // into view would need the container to scroll to an index, which is not
        // wired - so it is not offered.
        return in.in_scrollable && in.is_widget_row;
      default:
        return false;
    }
  }

  // RangeValue's IsReadOnly. NOT derived from NEUI_A11Y_STATE_READONLY: that bit
  // describes the WIDGET, this describes whether the PROVIDER can write it, and
  // for a client-owned CUSTOMDRAW value the two differ. Getting this wrong is not
  // cosmetic - a client reads IsReadOnly before offering adjustment, so a stray
  // TRUE makes every knob and slider unoperable through UIA.
  inline bool range_value_is_read_only(const ActionInputs& in, uint32_t state)
  {
    if (!in.host_owns_value) return true;
    if (state & NEUI_A11Y_STATE_DISABLED) return true;
    if (state & NEUI_A11Y_STATE_READONLY) return true;
    return false;
  }

  struct PatternSet
  {
    bool invoke          = false;
    bool toggle          = false;
    bool value           = false;
    bool range_value     = false;
    bool selection_item  = false;
    bool expand_collapse = false;
    bool scroll_item     = false;
  };

  inline PatternSet patterns_for(const ActionInputs& in)
  {
    PatternSet p;
    p.invoke          = action_allowed(in, kInvokePattern);
    p.toggle          = action_allowed(in, kTogglePattern);
    p.value           = action_allowed(in, kValuePattern);
    p.range_value     = action_allowed(in, kRangeValuePattern);
    p.selection_item  = action_allowed(in, kSelectionItemPattern);
    p.expand_collapse = action_allowed(in, kExpandCollapsePattern);
    p.scroll_item     = action_allowed(in, kScrollItemPattern);
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
