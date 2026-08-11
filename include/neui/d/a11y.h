#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "api.h"
#include "events.h"   // neui_widget_t

#ifdef __cplusplus
extern "C" {
#endif

// Accessibility - the seam that lets a screen reader, switch control, or any
// other assistive technology (AT) see what a neui window contains.
//
// THE PROBLEM. neui paints its own widgets. On the crossplatform host there is
// exactly ONE native surface per FRAME, so the operating system sees a neui
// window as a single opaque rectangle: VoiceOver reads out "window", and nothing
// inside it. Every control - button, knob, list row, grid cell - is invisible to
// the AT, which for a user who navigates by screen reader means the UI does not
// exist. For an audio plugin loaded into an accessible host, it means the plugin
// is the one part of the session that cannot be operated.
//
// THE FIX. The host walks its widget tree and publishes a synthetic accessibility
// tree to the platform's provider API (NSAccessibility on macOS, UI Automation on
// Windows). Because the platform gives us nothing per widget, that tree has to be
// built by us in any case - which is why it is built ONCE, in the crossplatform
// host, and serves every platform from one walk.
//
// WHAT YOU GET FOR FREE. A client that calls NOTHING here still produces a usable
// tree: each widget's role is derived from its TYPE (BUTTON -> button, INPUTBOX ->
// text field, KNOB / SLIDER -> slider, LISTBOX -> list with one item per row,
// GRID -> table, and so on), its name from its text, and its state from the flags
// the host already tracks (disabled, focused, selected, expanded, checked). You
// only need this interface for what the framework cannot know.
//
// WHAT ONLY YOU KNOW - and therefore what this interface is actually for:
//
//   * A CUSTOMDRAW's meaning. A custom-painted widget could be a knob, a level
//     meter, a toggle, an XY pad, or pure decoration. The framework deliberately
//     does NOT guess (see set_role) - it defaults to a plain group. If your UI is
//     built from custom-painted controls, declaring their roles is the single
//     highest-value thing you can do here.
//   * What a value MEANS. NEUI_PARAM_VALUE is normalized [0..1]. A screen reader
//     announcing "zero point four two" tells the user nothing; "minus six
//     decibels" tells them everything. See set_value_range / set_value_text.
//   * Which LABEL names which control. A LABEL next to an INPUTBOX is a layout
//     convention, not a relationship the framework can see. See set_labelled_by.
//
// Every method is safe to call before the frame is shown, and safe to call when
// no AT is running (declarations are simply stored). Calling none of them is also
// fine - see "what you get for free" above.
//
// Host support: the CROSSPLATFORM host only ("neui.host.crossplatform"), like
// NEUI_API_TIMER, NEUI_API_POINTER and NEUI_API_EMBED. On Windows and macOS
// neui_get_api(NULL) returns the NATIVE host first, so a client taking the
// default there gets NULL. Feature-detect; never assume the pointer is non-null.
//
// Platform coverage is NOT uniform, and a client should know which promise it is
// relying on:
//   macOS   - NSAccessibility provider (VoiceOver).
//   Windows - UI Automation provider.
//   Linux   - NO provider yet. Declarations are accepted and stored, and cost
//             nothing, but nothing reads them: AT-SPI is a separate piece of work.
//             A neui window stays opaque to Orca today.
//   iOS / null - no provider (iOS would need UIAccessibility).
// Declaring roles is still worthwhile on every platform: the calls are cheap, and
// a platform gaining a provider later picks them up with no client change.
#define NEUI_API_A11Y "com.defiantnerd.neui.extension.a11y/0"

// What KIND of thing a widget is, as far as an AT is concerned. Roles are
// deliberately generic: they name interaction contracts the platforms all
// understand, not neui widget types.
typedef enum neui_a11y_role_t
{
  // Derive the role from the widget's TYPE. The default for every widget, so a
  // client that declares nothing still gets a sensible tree.
  NEUI_A11Y_ROLE_DEFAULT = 0,

  // Decorative: remove this node AND ITS WHOLE SUBTREE from the tree. The
  // opt-out for backgrounds, frames, dividers, and any parent-painted chrome
  // that a user gains nothing from reaching. Prefer this over leaving a
  // meaningless node in place - navigating past decoration is a real cost to a
  // screen-reader user.
  NEUI_A11Y_ROLE_NONE,

  // Present, but only as a container - it groups children and does nothing
  // itself. The default for SECTION and for CUSTOMDRAW.
  NEUI_A11Y_ROLE_GROUP,
  NEUI_A11Y_ROLE_WINDOW,

  NEUI_A11Y_ROLE_STATIC_TEXT,
  NEUI_A11Y_ROLE_BUTTON,
  NEUI_A11Y_ROLE_TOGGLE_BUTTON,   // a button that stays pressed; report CHECKED
  NEUI_A11Y_ROLE_CHECKBOX,
  NEUI_A11Y_ROLE_RADIO_BUTTON,

  // Continuous or stepped value control. KNOB maps here too: no platform has a
  // "knob" role, and a slider is what every AT knows how to drive. Pair it with
  // set_value_range so the announced value is meaningful.
  NEUI_A11Y_ROLE_SLIDER,
  NEUI_A11Y_ROLE_PROGRESS,        // read-only, task progress
  NEUI_A11Y_ROLE_METER,           // read-only, a measured level (VU, gain)

  NEUI_A11Y_ROLE_TEXT_FIELD,
  NEUI_A11Y_ROLE_TEXT_AREA,

  NEUI_A11Y_ROLE_LIST,
  NEUI_A11Y_ROLE_LIST_ITEM,
  NEUI_A11Y_ROLE_COMBOBOX,
  NEUI_A11Y_ROLE_TREE,
  NEUI_A11Y_ROLE_TREE_ITEM,
  NEUI_A11Y_ROLE_TABLE,
  NEUI_A11Y_ROLE_ROW,
  NEUI_A11Y_ROLE_CELL,
  NEUI_A11Y_ROLE_COLUMN_HEADER,
  NEUI_A11Y_ROLE_TAB_LIST,
  NEUI_A11Y_ROLE_TAB,
  NEUI_A11Y_ROLE_MENU_BAR,
  NEUI_A11Y_ROLE_MENU,
  NEUI_A11Y_ROLE_MENU_ITEM,
  NEUI_A11Y_ROLE_IMAGE,
  NEUI_A11Y_ROLE_SCROLL_AREA
} neui_a11y_role_t;

// State bits, reported to the AT as the node's condition.
//
// The framework derives all of these from state it already tracks. set_state
// exists to OVERRIDE specific bits on a widget whose real state the framework
// cannot see - typically a CUSTOMDRAW - and it takes a (mask, values) pair so
// that overriding one bit does not silently freeze the rest at whatever they
// happened to be. Bits outside the mask stay framework-derived, live.
#define NEUI_A11Y_STATE_DISABLED   0x0001u  // not interactive (widget disabled)
#define NEUI_A11Y_STATE_FOCUSED    0x0002u  // currently has keyboard focus
#define NEUI_A11Y_STATE_FOCUSABLE  0x0004u  // can take keyboard focus
#define NEUI_A11Y_STATE_SELECTED   0x0008u  // selected within its container
#define NEUI_A11Y_STATE_EXPANDED   0x0010u  // expandable AND open
#define NEUI_A11Y_STATE_COLLAPSED  0x0020u  // expandable AND closed
#define NEUI_A11Y_STATE_CHECKED    0x0040u  // checkbox / toggle is on
#define NEUI_A11Y_STATE_MIXED      0x0080u  // tri-state indeterminate (CHECKBOX3)
#define NEUI_A11Y_STATE_READONLY   0x0100u  // value visible but not editable
#define NEUI_A11Y_STATE_OFFSCREEN  0x0200u  // scrolled outside its clip rect
#define NEUI_A11Y_STATE_PROTECTED  0x0400u  // password field; do not speak content
#define NEUI_A11Y_STATE_MULTILINE  0x0800u  // text field accepts newlines

// What changed, for notify(). The framework raises these itself for every
// built-in widget; a client needs them only for a hand-painted control whose
// state lives in the client's own variables.
typedef enum neui_a11y_change_t
{
  NEUI_A11Y_CHANGE_VALUE = 0,   // value or value text changed
  NEUI_A11Y_CHANGE_NAME,
  NEUI_A11Y_CHANGE_STATE,
  NEUI_A11Y_CHANGE_STRUCTURE,   // children added / removed / reordered
  NEUI_A11Y_CHANGE_SELECTION
} neui_a11y_change_t;

typedef struct neui_a11y_api
{
  uint32_t neui_version;

  // Override the derived role. NEUI_A11Y_ROLE_DEFAULT restores derivation.
  //
  // THIS IS THE CALL THAT MATTERS FOR CUSTOM-PAINTED UI. A CUSTOMDRAW defaults
  // to NEUI_A11Y_ROLE_GROUP and the framework will not guess otherwise, even
  // when the widget carries a behavior asset that writes a value: behavior
  // assets also drive buttons, toggles, XY pads and drag sources, and telling a
  // screen-reader user "slider" about something that is not one is worse than
  // telling them "group". Declare it and the guessing problem disappears.
  //
  // WHAT A DECLARED ROLE OBLIGES YOU TO HANDLE. Declaring a role also makes the
  // AT offer that role's ACTIONS, and for a hand-painted widget only you can
  // perform them. They arrive as ordinary key events on your widget, exactly as
  // if the user had pressed the key - your NEUI_EVENT_KEYDOWN handler gets first
  // refusal, and returning true consumes it:
  //   * a press (BUTTON / CHECKBOX / TOGGLE_BUTTON / RADIO_BUTTON) arrives as
  //     NEUI_KEY_SPACE;
  //   * an increment / decrement (SLIDER) arrives as NEUI_KEY_RIGHT /
  //     NEUI_KEY_LEFT - or, when you declared a `step` via set_value_range and
  //     the widget is a built-in KNOB / SLIDER, the host applies that step
  //     itself and no key is sent.
  // A CUSTOMDRAW that declares an actionable role and ignores those keys leaves
  // the AT offering an action that does nothing. The built-in widgets already
  // handle them, so a declared role on a BUTTON / KNOB / SLIDER needs nothing.
  void (NEUI_ABI *set_role)(neui_session_t session, neui_widget_t widget,
                            neui_a11y_role_t role);

  // The accessible NAME - the short label an AT speaks to identify the control
  // ("Cutoff", "Save"). Defaults to the widget's own text; set this when the
  // visible text is absent, decorative, or too terse to be spoken (an icon-only
  // button, a knob whose label is drawn by its parent). NULL / "" restores the
  // default. Do NOT include the role in it: "Save" not "Save button", or the AT
  // says "Save button button".
  void (NEUI_ABI *set_name)(neui_session_t session, neui_widget_t widget,
                            const char* utf8);

  // Longer supplementary help, spoken after the name when the user asks for
  // detail. Optional; leave unset unless it earns its place.
  void (NEUI_ABI *set_description)(neui_session_t session, neui_widget_t widget,
                                   const char* utf8);

  // The REAL-WORLD range behind a normalized value, so the AT can announce
  // something meaningful. Without this a KNOB at 0.42 is announced as a
  // percentage; with min=-60, max=+6 it becomes "-32.3" (and with
  // set_value_text, "-32.3 dB").
  //
  // `step` is the increment an AT increment/decrement action should move by, in
  // real-world units; 0 = continuous (the host picks a sensible fraction).
  // min == max clears the range.
  void (NEUI_ABI *set_value_range)(neui_session_t session, neui_widget_t widget,
                                   float min, float max, float step);

  // The current value, NORMALIZED [0..1], for a control whose value lives in the
  // CLIENT's own state and therefore never passes through the attribute bag -
  // i.e. a hand-painted CUSTOMDRAW. Built-in KNOB / SLIDER need neither this nor
  // notify(): the framework reads NEUI_PARAM_VALUE directly and raises its own
  // change notifications. Follow this with notify(NEUI_A11Y_CHANGE_VALUE).
  void (NEUI_ABI *set_value)(neui_session_t session, neui_widget_t widget,
                             float normalized);

  // The exact string to speak for the current value ("-6.0 dB", "Sine", "3/4").
  // Highest-priority value source, ahead of NEUI_ATTR_VALUE_TEXT and any range
  // mapping. NULL / "" clears it.
  //
  // Note on NEUI_ATTR_VALUE_TEXT: the framework falls back to that attribute, so
  // a KNOB that already sets it for its painted overlay gets accessibility for
  // free. But that attribute is ALSO what gets drawn on the knob, so use THIS
  // call when you want a spoken string without changing what is painted.
  void (NEUI_ABI *set_value_text)(neui_session_t session, neui_widget_t widget,
                                  const char* utf8);

  // Override specific state bits. `mask` selects which bits you are taking over;
  // `values` supplies them. Bits outside `mask` remain framework-derived and
  // live. A zero mask restores full derivation.
  //
  //   // this custom toggle is on, but leave focus/enabled derivation alone
  //   a11y->set_state(sess, w, NEUI_A11Y_STATE_CHECKED,
  //                            NEUI_A11Y_STATE_CHECKED);
  void (NEUI_ABI *set_state)(neui_session_t session, neui_widget_t widget,
                             uint32_t mask, uint32_t values);

  // Declare that `label` names `widget` - the LABEL-next-to-INPUTBOX idiom. The
  // label's text becomes the control's accessible name, and the label itself is
  // dropped from the tree (an AT would otherwise read it twice). This is not
  // inferred from layout: proximity guesses are wrong often enough to be worse
  // than an unnamed control. Pass widget_none as `label` to clear.
  void (NEUI_ABI *set_labelled_by)(neui_session_t session, neui_widget_t widget,
                                   neui_widget_t label);

  // Tell any attached AT that something about `widget` changed, so it can
  // re-read it. Needed ONLY after set_value / set_name / set_state on a
  // hand-painted widget: the framework raises its own notification for every
  // built-in USER-DRIVEN change it already reports as an event - value, checkbox
  // state, list / tree / grid / tab selection, scroll, and a behavior asset's
  // attribute writes - plus focus changes.
  //
  // Two things it does NOT cover, both worth knowing: a PROGRAMMATIC change
  // (attrs->set_float, widgets->set_text) raises nothing, because it is
  // indistinguishable from any other client write - call this after one if an AT
  // should hear about it; and editing text in an INPUTBOX / MULTILINE raises no
  // notification of its own (the value an AT reads afterwards is correct, but
  // nothing prompts it to re-read).
  //
  // Cheap and safe when no AT is attached.
  void (NEUI_ABI *notify)(neui_session_t session, neui_widget_t widget,
                          neui_a11y_change_t what);

  // Speak a transient message that is not part of the tree - the accessibility
  // counterpart of a toast. `assertive` interrupts whatever is being spoken; use
  // it only for something the user must hear now (an error), because
  // interrupting is disruptive.
  //
  // The framework already announces its own toasts and message boxes; this is
  // for a client's own hand-drawn transient UI. A tree node would be the wrong
  // shape for these: a node the user can neither reach in time nor dismiss is
  // worse than a spoken sentence.
  void (NEUI_ABI *announce)(neui_session_t session, const char* utf8,
                            bool assertive);

  // Whether an AT has actually queried this session - useful to skip building
  // expensive display strings that nothing will read.
  //
  // ADVISORY ONLY; never gate correctness on it. macOS accessibility is lazy by
  // design and offers no "an AT attached" signal, so the honest answer there is
  // "has anything queried us yet", which is false until the first query even
  // when VoiceOver is running. A client that skips maintaining state because
  // this returned false will be wrong the moment the first query arrives.
  bool (NEUI_ABI *is_active)(neui_session_t session);

  // Append new methods at the end (vtable-append evolution rule).
} neui_a11y_api_t;

// ---------------------------------------------------------------------------
// Attribute keys backing the calls above.
//
// The declarations are stored in the widget's ordinary attribute bag, so they
// are inspectable with NEUI_API_ATTRS, survive like any other attribute, and
// cost nothing on a widget that declares none of them. Prefer the typed calls
// above - these keys are documented because they are observable, not because
// they are the intended interface.
#define NEUI_ATTR_A11Y_ROLE         "neui.a11y.role"          // int (neui_a11y_role_t)
#define NEUI_ATTR_A11Y_NAME         "neui.a11y.name"          // string
#define NEUI_ATTR_A11Y_DESCRIPTION  "neui.a11y.description"   // string
#define NEUI_ATTR_A11Y_VALUE_TEXT   "neui.a11y.value_text"    // string
#define NEUI_ATTR_A11Y_VALUE        "neui.a11y.value"         // float, normalized
#define NEUI_ATTR_A11Y_RANGE_MIN    "neui.a11y.range_min"     // float
#define NEUI_ATTR_A11Y_RANGE_MAX    "neui.a11y.range_max"     // float
#define NEUI_ATTR_A11Y_RANGE_STEP   "neui.a11y.range_step"    // float
#define NEUI_ATTR_A11Y_STATE_MASK   "neui.a11y.state_mask"    // int
#define NEUI_ATTR_A11Y_STATE_VALUES "neui.a11y.state_values"  // int
#define NEUI_ATTR_A11Y_LABELLED_BY  "neui.a11y.labelled_by"   // int (widget id)

#ifdef __cplusplus
}
#endif
