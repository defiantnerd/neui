#pragma once

#include <neui/neui.h>
#include "events.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NEUI_API_WIDGETS "com.defiantnerd.neui.extension.widgets/0"

static const neui_widget_t widget_root = { 0 };
static const neui_widget_t widget_none = { UINT32_MAX };

typedef struct neui_widget_client {
  uint32_t neui_version;
  void (NEUI_ABI *ondestroy)(void* token, neui_widget_t widget, void* userdata);
  bool (NEUI_ABI *onevent)  (void* token, neui_event_t* event);
} neui_widget_client_t;

typedef struct neui_widget_api {
  // Create a widget under `parent`. (x, y) are logical pixels at 96 DPI,
  // relative to the parent's top-left (top-level children of a frame are
  // therefore frame-local). (width, height) are the widget's size in
  // logical pixels.
  neui_widget_t      (NEUI_ABI *create)(neui_session_t session, neui_widget_t parent, const char* type, int x, int y, int width, int height, void* userdata);
  void               (NEUI_ABI *destroy)(neui_session_t session, neui_widget_t widget);
  void               (NEUI_ABI *show)(neui_session_t session, neui_widget_t widget);
  void               (NEUI_ABI *hide)(neui_session_t session, neui_widget_t widget);
  // (x, y) are parent-relative logical pixels, same convention as create().
  void               (NEUI_ABI *set_pos)(neui_session_t session, neui_widget_t widget, int x, int y, int width, int height);
  void               (NEUI_ABI *set_size)(neui_session_t session, neui_widget_t widget, int width, int height);
  void               (NEUI_ABI *set_emit_events)(neui_session_t session, neui_widget_t widget, bool enabled);
  void               (NEUI_ABI *set_text)(neui_session_t session, neui_widget_t widget, const char* text);
  // Returns bytes needed including null terminator (call with buf=NULL to query size).
  // Copies up to buflen bytes including null terminator if buf is non-NULL.
  // Returns -1 on error.
  int                (NEUI_ABI *get_text)(neui_session_t session, neui_widget_t widget, char* buf, int buflen);
  neui_widget_t      (NEUI_ABI *get_first_child)(neui_session_t session, neui_widget_t widget);
  neui_widget_t      (NEUI_ABI *get_next_sibling)(neui_session_t session, neui_widget_t widget);
  void               (NEUI_ABI *set_focus)(neui_session_t session, neui_widget_t widget);
  void               (NEUI_ABI *set_check)(neui_session_t session, neui_widget_t widget, neui_check_state_t state);
  neui_check_state_t (NEUI_ABI *get_check)(neui_session_t session, neui_widget_t widget);
  // Returns the platform-native window handle (e.g. HWND on Win32) for the widget,
  // or NULL if the widget has no native handle yet. Intended for NEUI_W_PLUGWINDOW
  // so the client can hand it to an external host for embedding.
  void*              (NEUI_ABI *get_native_handle)(neui_session_t session, neui_widget_t widget);
  // Mark the widget as a tab-stop so TAB / SHIFT+TAB keyboard traversal can move
  // focus to it. Has no effect on hosts that use native tab-order (e.g. Win32 host).
  void               (NEUI_ABI *set_tab_stop)(neui_session_t session, neui_widget_t widget, bool enabled);
  // Establish a modal owner for a dialog frame. While the dialog is shown,
  // the owner is input-blocked; on dialog destroy the owner is re-enabled
  // and re-activated. Must be called before show(dialog). Both widgets must
  // be frames; dialog is typically NEUI_W_DIALOG. Pass owner=widget_none to clear.
  void               (NEUI_ABI *set_owner)(neui_session_t session, neui_widget_t dialog, neui_widget_t owner);
  // Read back current widget geometry. Coordinates are logical pixels at
  // 96 DPI (matching set_pos / set_size / create). For frames the position
  // is relative to the screen / parent frame; for child widgets it is
  // relative to the parent widget's top-left (the same parent-relative
  // space create / set_pos use). Out-pointers may be NULL to ignore
  // individual fields. No-op if the widget does not exist.
  void               (NEUI_ABI *get_pos)(neui_session_t session, neui_widget_t widget, int* x, int* y);
  void               (NEUI_ABI *get_size)(neui_session_t session, neui_widget_t widget, int* width, int* height);
  // Show a popup menu anchored to a widget. (x, y) are in the anchor's
  // local logical coordinates; the host converts to screen. items is a
  // null-terminated array of UTF-8 strings; an item that equals "-" is a
  // separator. The call is BLOCKING - it returns when the user picks an
  // item or dismisses the menu. Returns the 1-based index of the picked
  // item (counting separators), or 0 if dismissed without picking.
  // Useful for context menus on KNOB / future widgets that need a
  // popup-style menu independent of the menubar.
  int                (NEUI_ABI *popup_menu)(neui_session_t session, neui_widget_t anchor,
                                             int x, int y,
                                             const char* const* items);
} neui_widget_api_t;

#define NEUI_W_APPWINDOW  "neui.std.appwindow"
#define NEUI_W_PLUGWINDOW "neui.std.plugwindow"
#define NEUI_W_DIALOG     "neui.std.dialog"
#define NEUI_W_LABEL      "neui.std.label"
#define NEUI_W_BUTTON     "neui.std.button"
#define NEUI_W_INPUTBOX   "neui.std.inputbox"
#define NEUI_W_CHECKBOX   "neui.std.checkbox"
#define NEUI_W_CHECKBOX3  "neui.std.checkbox3"
#define NEUI_W_LISTBOX    "neui.std.listbox"
#define NEUI_W_COMBOBOX   "neui.std.combobox"
#define NEUI_W_MULTILINE  "neui.std.multiline"
#define NEUI_W_TREEVIEW   "neui.std.treeview"
#define NEUI_W_MENUBAR    "neui.std.menubar"
#define NEUI_W_IMAGE      "neui.std.image"
#define NEUI_W_SLIDER     "neui.std.slider"
#define NEUI_W_KNOB       "neui.std.knob"
#define NEUI_W_SECTION    "neui.std.section"

#ifdef __cplusplus
}
#endif