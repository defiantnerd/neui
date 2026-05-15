#pragma once

// events API

#ifdef __cplusplus
extern "C" {
#endif

  typedef struct neui_widget {
    uint32_t id;
  } neui_widget_t;

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

    NEUI_EVENT_ITEM_SELECTED            = DEF_ITEM_EVENT(1),  // fired when selection changes in listbox/combobox

    NEUI_EVENT_TREE_ITEM_SELECTED       = DEF_TREE_EVENT(1),  // treeview: selection changed
    NEUI_EVENT_TREE_ITEM_ACTIVATED      = DEF_TREE_EVENT(2),  // treeview: dbl-click/Enter; menu: item clicked

    NEUI_EVENT_CUSTOM                   = 0x1ffff,
  } neui_event_type_t;

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

  // event_mouse for all events like move/click/button etc.
  typedef struct neui_event_mouse
  {
    neui_widget_t widget;
    int x;
    int y;
    uint32_t buttonmap;
  } neui_event_mouse_t;

  // Scroll-wheel event. delta is in lines: positive = scroll up, negative = scroll down.
  typedef struct neui_event_wheel
  {
    neui_widget_t widget;
    int x;
    int y;
    int delta;
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

  typedef struct neui_event_custom
  {
    neui_widget_t widget;
    const char*   identifier;
    uint32_t      length;
    uint8_t*      data;
  } neui_event_custom_t;

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
      neui_event_custom_t    custom;
    } data;

    // more event data can be added here
} neui_event_t;

#ifdef __cplusplus
}
#endif
