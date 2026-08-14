#pragma once

// events API

#ifdef __cplusplus
extern "C" {
#endif

  typedef struct neui_widget {
    uint32_t id;
  } neui_widget_t;

  // Defined here (and re-guarded in d/clipboard.h) so the DnD event
  // payload below can carry it without depending on header order. Same
  // primitive backs clipboard items and DnD drop payloads.
#ifndef NEUI_DATA_ITEM_T_DEFINED
#define NEUI_DATA_ITEM_T_DEFINED
  typedef struct neui_data_item { uint32_t id; } neui_data_item_t;
#endif

// remove #define NEUI_API_EVENTS   "com.defiantnerd.neui.extension.events/0"

// the upper 24 bits are being used for event ids, the lower 16 bits for event categories,
// so we can easily filter by category and have a large number of events per category 
// without collisions
#define DEF_APP_EVENT(x)    (((x)<<16) | 0x000A)
#define DEF_MOUSE_EVENT(x)  (((x)<<16) | 0x0001)
#define DEF_KEY_EVENT(x)    (((x)<<16) | 0x0002)
#define DEF_WIDGET_EVENT(x) (((x)<<16) | 0x0003)
#define DEF_ITEM_EVENT(x)   (((x)<<16) | 0x0004)
#define DEF_TREE_EVENT(x)   (((x)<<16) | 0x0005)
#define DEF_GRID_EVENT(x)   (((x)<<16) | 0x0006)
#define DEF_DND_EVENT(x)    (((x)<<16) | 0x0007)
#define DEF_TAB_EVENT(x)    (((x)<<16) | 0x0008)

  typedef enum neui_event_type
  {
    NEUI_EVENT_APP_QUIT                 = DEF_APP_EVENT(1),

    NEUI_EVENT_MOUSE_MOVE               = DEF_MOUSE_EVENT(1),
    NEUI_EVENT_MOUSE_ENTER              = DEF_MOUSE_EVENT(2),
    NEUI_EVENT_MOUSE_LEAVE              = DEF_MOUSE_EVENT(3),
    NEUI_EVENT_MOUSE_BUTTON_DOWN        = DEF_MOUSE_EVENT(0x10),
    NEUI_EVENT_MOUSE_BUTTON_UP          = DEF_MOUSE_EVENT(0x11),
    NEUI_EVENT_MOUSE_BUTTON_CLICK       = DEF_MOUSE_EVENT(0x12),
    NEUI_EVENT_MOUSE_BUTTON_DBLCLICK    = DEF_MOUSE_EVENT(0x13),
    NEUI_EVENT_MOUSE_RBUTTON_DOWN       = DEF_MOUSE_EVENT(0x14),
    NEUI_EVENT_MOUSE_RBUTTON_UP         = DEF_MOUSE_EVENT(0x15),
    NEUI_EVENT_MOUSE_WHEEL              = DEF_MOUSE_EVENT(0x20),

    NEUI_EVENT_KEYDOWN                  = DEF_KEY_EVENT(1),
    NEUI_EVENT_KEYCHAR                  = DEF_KEY_EVENT(2),
    NEUI_EVENT_KEYUP                    = DEF_KEY_EVENT(3),

    NEUI_EVENT_WIDGET_UPDATED           = DEF_WIDGET_EVENT(1),
    NEUI_EVENT_WIDGET_PREUPDATE         = DEF_WIDGET_EVENT(2),
    NEUI_EVENT_WIDGET_FOCUS             = DEF_WIDGET_EVENT(3),
    NEUI_EVENT_CHECKBOX_CHANGED         = DEF_WIDGET_EVENT(4),  // fired when checkbox state changes
    NEUI_EVENT_RESIZE                   = DEF_WIDGET_EVENT(5),  // frame client area resized
    NEUI_EVENT_VALUE_CHANGED            = DEF_WIDGET_EVENT(6),  // slider/knob value changed by user
    NEUI_EVENT_WIDGET_PAINT             = DEF_WIDGET_EVENT(7),  // NEUI_W_CUSTOMDRAW: client draws via backend/ctx
    NEUI_EVENT_ATTR_CHANGED             = DEF_WIDGET_EVENT(8),  // behavior wrote an attr (user-driven)
    NEUI_EVENT_SCROLL_CHANGED           = DEF_WIDGET_EVENT(9),  // scrolling SECTION's offset moved
    NEUI_EVENT_METRICS_CHANGED          = DEF_WIDGET_EVENT(0xA),// platform layout metrics changed (iOS Dynamic Type / rotation / safe-area)
    NEUI_EVENT_GESTURE_BEGIN            = DEF_WIDGET_EVENT(0xB),// user grabbed a value control (see neui_event_gesture_t)
    NEUI_EVENT_GESTURE_END              = DEF_WIDGET_EVENT(0xC),// user released it (always paired with a BEGIN)
    NEUI_EVENT_POPUP_DISMISSED          = DEF_WIDGET_EVENT(0xD),// a NEUI_W_POPUPSURFACE closed (see neui_event_popup_t)

    NEUI_EVENT_TIMER                    = DEF_APP_EVENT(2),   // a NEUI_API_TIMER timer elapsed (session-scoped; see neui_event_timer_t)

    NEUI_EVENT_ITEM_SELECTED            = DEF_ITEM_EVENT(1),  // fired when selection changes in listbox/combobox

    NEUI_EVENT_TREE_ITEM_SELECTED       = DEF_TREE_EVENT(1),  // treeview: selection changed
    NEUI_EVENT_TREE_ITEM_ACTIVATED      = DEF_TREE_EVENT(2),  // treeview: dbl-click/Enter; menu: item clicked

    NEUI_EVENT_GRID_ROW_SELECTED        = DEF_GRID_EVENT(1),  // grid: selected row changed
    NEUI_EVENT_GRID_CELL_SELECTED       = DEF_GRID_EVENT(2),  // grid (cell-focus mode): (row, col) cursor moved
    NEUI_EVENT_GRID_ROW_ACTIVATED       = DEF_GRID_EVENT(3),  // grid: dbl-click / Enter on a row
    NEUI_EVENT_GRID_CELL_CLICKED        = DEF_GRID_EVENT(4),  // grid: raw "click at (row, col)" fallback - only fires if the high-level events above weren't consumed by the client
    NEUI_EVENT_GRID_COLUMN_RESIZED      = DEF_GRID_EVENT(5),  // grid: user-driven column-divider drag released
    NEUI_EVENT_GRID_SORT_CHANGED        = DEF_GRID_EVENT(6),  // grid: user-driven header click changed the sort stack
    NEUI_EVENT_GRID_CELL_EDIT_BEGIN     = DEF_GRID_EVENT(7),  // grid: in-place cell editor opened at (row, col)
    NEUI_EVENT_GRID_CELL_CHANGED        = DEF_GRID_EVENT(8),  // grid: cell text committed (after validate_cell returned true)
    NEUI_EVENT_GRID_CELL_EDIT_CANCEL    = DEF_GRID_EVENT(9),  // grid: in-place cell editor closed without committing

    NEUI_EVENT_DND_ENTER                = DEF_DND_EVENT(1),  // cursor entered a drop-target widget during drag
    NEUI_EVENT_DND_MOVE                 = DEF_DND_EVENT(2),  // cursor moved while over a drop-target widget
    NEUI_EVENT_DND_LEAVE                = DEF_DND_EVENT(3),  // cursor left a drop-target widget (or drag cancelled)
    NEUI_EVENT_DND_DROP                 = DEF_DND_EVENT(4),  // payload released over a drop-target widget

    NEUI_EVENT_TAB_DESELECTED           = DEF_TAB_EVENT(1),  // tabview: outgoing tab, fired before the page swap / repaint
    NEUI_EVENT_TAB_SELECTED             = DEF_TAB_EVENT(2),  // tabview: incoming tab, fired before the page swap / repaint

    NEUI_EVENT_CUSTOM                   = 0x1ffff,
  } neui_event_type_t;

#undef DEF_TAB_EVENT
#undef DEF_DND_EVENT
#undef DEF_GRID_EVENT
#undef DEF_TREE_EVENT
#undef DEF_ITEM_EVENT
#undef DEF_WIDGET_EVENT
#undef DEF_KEY_EVENT
#undef DEF_MOUSE_EVENT
#undef DEF_APP_EVENT

  typedef enum neui_check_state {
    NEUI_CHECK_UNCHECKED     = 0,
    NEUI_CHECK_CHECKED       = 1,
    NEUI_CHECK_INDETERMINATE = 2,
  } neui_check_state_t;

  // Bit values for neui_event_mouse_t::buttonmap and
  // neui_event_wheel_t::buttonmap. Numeric values match the Win32 MK_*
  // constants, but a host must still MASK rather than forward a raw wParam:
  // Win32's MK_XBUTTON1 is 0x0020, which collides with NEUI_MK_ALT, and
  // MK_XBUTTON2 (0x0040) is not in this enum at all. The per-OS translations
  // in hosts/shared/{win32,macos,linux}/keys_*.h do the masking - use them.
  // Note: NEUI_MK_ALT is not produced by any host layer today (Win32 MK_* has
  // no ALT bit and the macOS / Linux layers do not populate it). It is
  // reserved so behavior handlers can opt into ALT-fine semantics when hosts
  // later plumb it through.
  enum {
    NEUI_MK_LBUTTON = 0x0001,
    NEUI_MK_RBUTTON = 0x0002,
    NEUI_MK_SHIFT   = 0x0004,
    NEUI_MK_CONTROL = 0x0008,
    NEUI_MK_MBUTTON = 0x0010,
    NEUI_MK_ALT     = 0x0020,
  };

  // event_mouse for all events like move/click/button etc.
  // x / y are WIDGET-local logical pixels at 96 DPI: (0, 0) is the top-left
  // of the widget that .widget identifies, matching the NEUI_EVENT_WIDGET_PAINT
  // origin and the DnD payload. This contract is identical on every host -
  // the native win32 / macOS hosts get widget-local coords directly from the
  // per-widget HWND/NSView; the single-surface crossplatform host translates
  // frame-local input to the target widget's origin before dispatch.
  typedef struct neui_event_mouse
  {
    neui_widget_t widget;
    int x;
    int y;
    uint32_t buttonmap;
  } neui_event_mouse_t;

  // Scroll-wheel event. delta is in lines: positive = scroll up / left,
  // negative = scroll down / right.
  //   is_horizontal = 0  -> vertical wheel (default; classic WM_MOUSEWHEEL,
  //                          NSEvent scrollingDeltaY).
  //   is_horizontal = 1  -> horizontal wheel: trackpad two-finger horizontal
  //                          (Win32 WM_MOUSEHWHEEL; NSEvent scrollingDeltaX)
  //                          OR Shift+wheel converted by the platform layer
  //                          as the conventional mouse fallback.
  // x / y are WIDGET-local logical pixels (same convention as
  // neui_event_mouse_t): relative to the widget named by .widget, which on a
  // bubbling wheel is the actual recipient (an inner widget or the scrolling
  // ancestor that consumed it).
  // buttonmap carries the same NEUI_MK_* bits as neui_event_mouse_t, so a
  // client can implement Shift-for-fine / Ctrl-for-page on the wheel without
  // tracking key state itself. Note the platform layer may already have
  // consumed Shift to synthesise a horizontal wheel (see is_horizontal above);
  // when it does, is_horizontal = 1 AND NEUI_MK_SHIFT is set, so a handler
  // that wants Shift purely as a fine modifier should check is_horizontal
  // first.
  typedef struct neui_event_wheel
  {
    neui_widget_t widget;
    int x;
    int y;
    int delta;
    int is_horizontal;
    uint32_t buttonmap;
    // 1 when the OS ALREADY INVERTED this delta relative to the user's physical
    // gesture - macOS "Natural scrolling", which is on by default.
    //
    // Why you may need it: for CONTENT scrolling the delta is right as delivered
    // (moving the content with your fingers is the point of the preference), so a
    // scrolling consumer should ignore this field. For a VALUE control there is no
    // content to move and the user's mental model is the direction their fingers
    // travelled - so a knob or fader wants `is_flipped ? -delta : delta`, or the
    // same physical swipe raises the value with the preference on and lowers it
    // with the preference off.
    //
    // NOT UNIFORM ACROSS PLATFORMS, and the field is named for what it can
    // actually promise: 1 means "definitely inverted", 0 means "not inverted, OR
    // this platform cannot tell". Only macOS can answer
    // (NSEvent.isDirectionInvertedFromDevice). Windows applies the
    // Precision-Touchpad "reverse scrolling direction" setting in the driver with
    // no queryable API, and on Linux libinput applies natural-scroll in the input
    // stack before XI2 valuators reach us - on both, a user who inverts scrolling
    // system-wide inverts neui's value controls with it, and neui cannot know.
    int is_flipped;
  } neui_event_wheel_t;

  // event_key for all events like keydown/keyup/emitted key
  typedef struct neui_event_key
  {
    neui_widget_t widget;
    uint32_t keycode;
    uint32_t modifiers;
  } neui_event_key_t;

  typedef struct neui_event_focus
  {
    neui_widget_t widget;
    bool focused;  // true if gained focus, false if lost focus
  } neui_event_focus_t;

  // event_checkbox for checkbox state changes
  typedef struct neui_event_checkbox
  {
    neui_widget_t      widget;
    neui_check_state_t state;
  } neui_event_checkbox_t;

  // event_item for listbox/combobox selection changes
  typedef struct neui_event_item
  {
    neui_widget_t widget;
    uint32_t index;     // selected item index, or NEUI_ITEM_NONE
  } neui_event_item_t;

  // event_tree for treeview selection/activation and menu item activation
  typedef struct neui_event_tree
  {
    neui_widget_t widget;
    neui_item_t   item;
  } neui_event_tree_t;

  // event_resize for frame size changes (NEUI_EVENT_RESIZE).
  // width/height are the new client-area dimensions in logical pixels (96 DPI).
  typedef struct neui_event_resize
  {
    neui_widget_t widget;   // the frame (APPWINDOW / PLUGWINDOW) being resized
    int width;
    int height;
  } neui_event_resize_t;

  // Fired before each repaint of an opt-in widget so the client can refresh
  // attribute-driven state (e.g. NEUI_PARAM_VALUE on a slider/knob) before
  // the paint pass reads it. Only fires when the widget has emit_events set.
  typedef struct neui_event_preupdate
  {
    neui_widget_t widget;   // widget about to be painted
  } neui_event_preupdate_t;

  // User-initiated value change on a slider / knob. Programmatic sets via
  // attrs->set_float(NEUI_PARAM_VALUE, ...) do NOT fire this.
  typedef struct neui_event_value
  {
    neui_widget_t widget;
    float         value;    // current normalized value [0..1]
  } neui_event_value_t;

  // User-driven attribute mutation by a behavior asset attached to the
  // widget. Programmatic attrs->set_* calls do NOT fire this; only
  // mouse / keyboard / wheel input routed through a behavior handler.
  // attr_key points into the host's interned strings and stays valid
  // for the duration of the event dispatch only.
  typedef struct neui_event_attr
  {
    neui_widget_t widget;
    const char*   attr_key;
    float         value;    // value-as-float; INT attrs promoted via attr_as_float
  } neui_event_attr_t;

  // Value-edit gesture lifecycle (NEUI_EVENT_GESTURE_BEGIN / _END). Brackets
  // a run of user-driven value changes so a client can forward host-automation
  // begin/end edits (VST3 beginEdit/endEdit, CLAP GESTURE_BEGIN/END):
  //   * A pointer grab on a value control (KNOB / SLIDER drag, or a behavior
  //     DRAG_* handler) fires BEGIN on the grab and END on release / cancel -
  //     the pair fires even when the value never moves, and every
  //     VALUE_CHANGED / ATTR_CHANGED of the drag lands between them.
  //   * A one-shot change (wheel tick, arrow / Home / End / Page key,
  //     double-click or context-menu reset, behavior CLICK_TOGGLE / _CYCLE /
  //     _SELECT) fires an implicit BEGIN + change + END triple, and only
  //     when the value actually changed.
  //   * Programmatic attrs->set_* writes never fire gestures (mirrors
  //     VALUE_CHANGED / ATTR_CHANGED).
  // attr_key names the attribute the gesture edits (NEUI_PARAM_VALUE for the
  // built-in KNOB / SLIDER; the handler's target attr for behaviors - a
  // BIAXIAL drag fires one pair per target). Same lifetime rule as
  // neui_event_attr_t: valid for the duration of the dispatch only. value is
  // the attr's value at BEGIN, and its final value at END.
  typedef struct neui_event_gesture
  {
    neui_widget_t widget;
    const char*   attr_key;
    float         value;
  } neui_event_gesture_t;

  typedef struct neui_event_custom
  {
    neui_widget_t widget;
    const char*   identifier;
    uint32_t      length;
    uint8_t*      data;
  } neui_event_custom_t;

  // Grid row event - row selected (NEUI_EVENT_GRID_ROW_SELECTED) or
  // activated by dbl-click / Enter (NEUI_EVENT_GRID_ROW_ACTIVATED).
  // row is 0-based; -1 indicates the selection was cleared.
  typedef struct neui_event_grid_row
  {
    neui_widget_t widget;
    int           row;
  } neui_event_grid_row_t;

  // Grid cell event - cell-focus cursor moved (NEUI_EVENT_GRID_CELL_SELECTED)
  // or raw cell click fallback (NEUI_EVENT_GRID_CELL_CLICKED, fires only
  // when the higher-level row/cell selected events were not consumed by
  // the client's onevent). Also delivered for the cell-edit lifecycle
  // events: NEUI_EVENT_GRID_CELL_EDIT_BEGIN (editor opened),
  // NEUI_EVENT_GRID_CELL_CHANGED (committed; the new text is already
  // written to the model and reachable via grid->get_cell_text), and
  // NEUI_EVENT_GRID_CELL_EDIT_CANCEL (closed without committing).
  typedef struct neui_event_grid_cell
  {
    neui_widget_t widget;
    int           row;
    int           col;
  } neui_event_grid_cell_t;

  // Grid column resize event - fires once on mouse-up at the end of a
  // user-driven column-divider drag. Programmatic set_column_width does
  // NOT fire it (mirrors NEUI_EVENT_VALUE_CHANGED semantics).
  typedef struct neui_event_grid_column_resize
  {
    neui_widget_t widget;
    int           col;
    int           old_width;
    int           new_width;
  } neui_event_grid_column_resize_t;

  // Drag&drop event payload. Carries the cursor position in widget-local
  // logical pixels, the MIME format names advertised on the drag source,
  // and (only on NEUI_EVENT_DND_DROP) the data item to read bytes from.
  //
  // `formats` is borrowed dispatch-scoped storage (same lifetime contract
  // as `neui_event_attr_t::attr_key`); copy any string out before
  // returning. The `data` item id is valid only inside the dispatch
  // callback - the framework releases the underlying DataItem the moment
  // the client returns, so clients that need the bytes past the call must
  // pull them via clipboard_api->item_get_format(item, mime, ...) before
  // returning.
  //
  // `suggested_action` carries the source's hint (NEUI_DND_ACTION_*).
  // The client signals what it will accept by calling
  // dnd_api->accept(session, action) inside the callback - the framework
  // caches the value and reports it back to the OS pasteboard.
  typedef struct neui_event_dnd
  {
    neui_widget_t      widget;          // hit widget (set_drop_target = true)
    int                x;               // widget-local x (logical px)
    int                y;               // widget-local y (logical px)
    uint32_t           buttonmap;       // NEUI_MK_* mouse-modifier bits
    const char* const* formats;         // borrowed list of MIME strings
    uint32_t           formats_count;
    neui_data_item_t   data;            // populated on DROP; none on ENTER/MOVE/LEAVE
    uint32_t           suggested_action; // neui_dnd_action_t bitmask
  } neui_event_dnd_t;

  // Scroll-changed event for a scrolling SECTION. Fires whenever the
  // committed scroll position changes (wheel, scrollbar drag, spring-back
  // tick, programmatic scroll->set_scroll / ensure_visible). Suppressed
  // when the new offset equals the previous one. scroll_x / scroll_y are
  // in logical pixels at 96 DPI; the values are the same range
  // scroll->set_scroll accepts.
  typedef struct neui_event_scroll
  {
    neui_widget_t widget;     // the scrolling SECTION
    int           scroll_x;   // current horizontal offset (logical px)
    int           scroll_y;   // current vertical offset   (logical px)
  } neui_event_scroll_t;

  // Platform layout metrics changed (NEUI_EVENT_METRICS_CHANGED). Fired when
  // the values returned by the metrics API (NEUI_API_METRICS) change - on iOS
  // when the Dynamic Type content-size category changes, or on rotation /
  // safe-area change. Desktop hosts do not fire this today (metrics are
  // static there). `widget` is the frame whose client should re-run its
  // responsive layout; `ui_scale` is the new UI scale, inline so a handler need
  // not call back into the API.
  //
  // CAUTION - two different scales reach this field, and they ask for OPPOSITE
  // responses:
  //   - iOS Dynamic Type (the value metrics_api->ui_scale returns): the client's
  //     logical space is unchanged but default text got bigger, so a handler
  //     that lays out by hand SHOULD re-scale.
  //   - a frame's NEUI_ATTR_UI_SCALE zoom (crossplatform host): everything
  //     already scales through the paint transform, so a handler MUST NOT
  //     re-scale - doing so double-applies it.
  // A handler that multiplies its layout by this value therefore breaks under
  // zoom. Distinguish by source: the zoom is per-frame and readable from the
  // frame's own attr, while metrics_api->ui_scale stays 1.0 on desktop.
  typedef struct neui_event_metrics
  {
    neui_widget_t widget;     // the frame
    float         ui_scale;   // new painted / text UI scale
  } neui_event_metrics_t;

  // Timer-elapsed event (NEUI_API_TIMER, <neui/d/timer.h>). Deliberately the
  // ONLY event with no `.widget`: a timer belongs to the session, not to a
  // widget, and omitting the field keeps a handler from being written as though
  // it did. Gate on `timer_id` instead - the same discipline, one field over.
  //
  // A tick that arrives late fires once rather than catching up on missed
  // periods, so a slow handler cannot accumulate a backlog of its own events.
  typedef struct neui_event_timer
  {
    uint32_t timer_id;        // the id add_timer returned
    uint32_t interval_ms;     // the timer's configured interval (as clamped)
  } neui_event_timer_t;

  // Grid sort-changed event - fires after a user-driven header click that
  // mutates the sort stack (or removes a level). Carries the column that
  // was clicked and its new direction in the stack; clients that need the
  // full stack call grid->get_sort_count + grid->get_sort_level.
  // Programmatic set_sort / add_sort / clear_sort do NOT fire this.
  // dir == NEUI_GRID_SORT_NONE means the level was just removed.
  typedef struct neui_event_grid_sort
  {
    neui_widget_t widget;
    int           col;
    int           dir;       // neui_grid_sort_dir_t
  } neui_event_grid_sort_t;

  // Tabbed-view selection event. Fires on every tab change (mouse click,
  // keyboard, or programmatic tabs_api->set_selected): NEUI_EVENT_TAB_DESELECTED
  // for the outgoing tab, then NEUI_EVENT_TAB_SELECTED for the incoming one.
  // Both fire synchronously BEFORE the framework swaps page visibility and
  // invalidates, so a client handler can update the incoming page's widgets
  // before the user sees them. `widget` is the TABVIEW; `tab_index` is the
  // affected tab (0-based); `page` is the affected TABPAGE widget.
  typedef struct neui_event_tab
  {
    neui_widget_t widget;
    uint32_t      tab_index;
    neui_widget_t page;
  } neui_event_tab_t;

  // A NEUI_W_POPUPSURFACE closed (NEUI_EVENT_POPUP_DISMISSED). `widget` is the
  // popup surface; `reason` is a neui_popup_dismiss_reason_t (see
  // <neui/d/popup.h>). Fired for every close - host-driven (outside press,
  // deactivation, owner moved, Escape) and client-driven (popup->close) alike -
  // so a client has exactly one place to drop the state it opened the popup
  // with. Purely a notification: the close has already happened and cannot be
  // vetoed, because a popup that could refuse to close is a window stuck over
  // someone's DAW. When a cascade closes, the deepest level is reported first.
  typedef struct neui_event_popup
  {
    neui_widget_t widget;
    uint32_t      reason;
  } neui_event_popup_t;

  // Forward-declarations for the curated paint surface. Clients that
  // handle NEUI_EVENT_WIDGET_PAINT include <neui/d/painter.h> (or just
  // <neui/neui.h>) to call painter_api->* functions on the handle.
  struct neui_painter;
  struct neui_painter_api;

  // Fired during paint for NEUI_W_CUSTOMDRAW widgets. The client draws
  // through `painter_api` with the opaque `p` handle. Origin (0, 0) is
  // the widget's top-left in logical pixels; width/height are the
  // widget size at 96 DPI (use painter_api->get_scale_factor(p) for the
  // physical scale). The framework wraps the callback in
  // push_transform / push_clip(widget bounds) / pop_clip / pop_transform
  // so client state changes are isolated from sibling widgets and from
  // the rest of the frame.
  //
  // The painter forwards to the active render backend but exposes only
  // the draw-safe subset - context lifecycle and raw bitmap APIs are
  // host-internal. Load bitmaps outside the paint callback via
  // get_interface(session, NEUI_API_ASSETS) and draw them through
  // painter_api->draw_asset.
  typedef struct neui_event_paint
  {
    neui_widget_t              widget;
    struct neui_painter_api*   painter_api;  // curated drawing surface
    struct neui_painter*       p;             // opaque handle (paint-call scoped)
    float                      width;         // widget size (see `scale`)
    float                      height;
    bool                       focused;
    // Physical pixels per unit drawn: the monitor's DPI ratio times the
    // frame's NEUI_ATTR_UI_SCALE zoom. 1.0 on a 96-DPI display at 100 % zoom.
    //
    // width/height are LOGICAL pixels by default and the framework scales what
    // you draw - so ignoring `scale` entirely is correct, and your rendering
    // follows DPI and zoom for free. Read it when you need the device grid:
    // to keep a hairline exactly one physical pixel (`1.0f / scale` units
    // wide), to pick a bitmap or bake resolution, or to snap to whole pixels.
    //
    // With NEUI_ATTR_PAINT_DEVICE_PIXELS set on the widget, width/height
    // instead arrive already multiplied by the zoom and one unit is one
    // device-side pixel - see that attribute in <neui/d/attrs.h>.
    float                      scale;
  } neui_event_paint_t;

  typedef struct neui_event {
    neui_event_type_t type;
    union {
      neui_event_mouse_t     mouse;
      neui_event_wheel_t     wheel;
      neui_event_key_t       key;
      neui_event_focus_t     focus;
      neui_event_checkbox_t  checkbox;
      neui_event_item_t      item;
      neui_event_tree_t      tree;
      neui_event_resize_t    resize;
      neui_event_preupdate_t preupdate;
      neui_event_value_t     value;
      neui_event_attr_t      attr;
      neui_event_gesture_t   gesture;
      neui_event_paint_t     paint;
      neui_event_custom_t    custom;
      neui_event_grid_row_t           grid_row;
      neui_event_grid_cell_t          grid_cell;
      neui_event_grid_column_resize_t grid_column_resize;
      neui_event_grid_sort_t          grid_sort;
      neui_event_dnd_t                dnd;
      neui_event_scroll_t             scroll;
      neui_event_tab_t                tab;
      neui_event_metrics_t            metrics;
      neui_event_timer_t              timer;
      neui_event_popup_t              popup;
    } data;

    // more event data can be added here
} neui_event_t;

#ifdef __cplusplus
}
#endif
