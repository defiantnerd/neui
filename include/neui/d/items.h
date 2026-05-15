#pragma once

#include <neui/neui.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NEUI_API_ITEMS "com.defiantnerd.neui.extension.items/0"

#define NEUI_ITEM_NONE UINT32_MAX   // sentinel: no selection / invalid index

// Items API - for widgets that hold a list of selectable entries (listbox, combobox).
// Item indices are 0-based and stable until an item with a lower index is removed.
// All text is UTF-8.
typedef struct neui_items_api {
  // Remove all items.
  void     (NEUI_ABI *clear)       (neui_session_t session, neui_widget_t widget);
  // Append an item; returns its index, or NEUI_ITEM_NONE on failure.
  uint32_t (NEUI_ABI *add)         (neui_session_t session, neui_widget_t widget, const char* text, void* userdata);
  // Remove the item at index. Items above it shift down.
  void     (NEUI_ABI *remove)      (neui_session_t session, neui_widget_t widget, uint32_t index);
  // Number of items currently in the widget.
  uint32_t (NEUI_ABI *count)       (neui_session_t session, neui_widget_t widget);
  // Returns bytes needed including null terminator (call with buf=NULL to query).
  // Copies up to buflen bytes including null if buf is non-NULL. Returns -1 on error.
  int      (NEUI_ABI *get_text)    (neui_session_t session, neui_widget_t widget, uint32_t index, char* buf, int buflen);
  // Replace the text of an existing item.
  void     (NEUI_ABI *set_text)    (neui_session_t session, neui_widget_t widget, uint32_t index, const char* text);
  // Retrieve the userdata pointer stored with an item.
  void*    (NEUI_ABI *get_userdata)(neui_session_t session, neui_widget_t widget, uint32_t index);
  // Returns the selected index, or NEUI_ITEM_NONE if nothing is selected.
  uint32_t (NEUI_ABI *get_selected)(neui_session_t session, neui_widget_t widget);
  // Set the selected item. Pass NEUI_ITEM_NONE to deselect.
  void     (NEUI_ABI *set_selected)(neui_session_t session, neui_widget_t widget, uint32_t index);
} neui_items_api_t;

#ifdef __cplusplus
}
#endif
