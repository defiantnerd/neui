#pragma once

#include <neui/neui.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NEUI_API_TREE "com.defiantnerd.neui.extension.tree/0"

// neui_item_t is defined in d/api.h

static const neui_item_t tree_item_root = { 0 };          // sentinel: parent of top-level items
static const neui_item_t tree_item_none = { UINT32_MAX };  // sentinel: invalid / no result

// Tree API - for hierarchical controls: treeview and menu bar.
// Both use the same interface; behaviour differences are noted per function.
// All text is UTF-8.
typedef struct neui_tree_api {
  // Add an item under parent (use tree_item_root for top-level).
  // Returns tree_item_none on failure.
  neui_item_t (NEUI_ABI *add)         (neui_session_t session, neui_widget_t widget,
                                        neui_item_t parent, const char* text, void* userdata);
  // Remove a single item. For treeview, children are also removed.
  // For menu bar, if the item has children (submenu) it and its contents are removed.
  void        (NEUI_ABI *remove)      (neui_session_t session, neui_widget_t widget, neui_item_t item);
  // Remove all items.
  void        (NEUI_ABI *clear)       (neui_session_t session, neui_widget_t widget);

  // Per-item text. Two-pass convention: call with buf=NULL to query byte count
  // including null terminator, then call again with a buffer.
  int         (NEUI_ABI *get_text)    (neui_session_t session, neui_widget_t widget,
                                        neui_item_t item, char* buf, int buflen);
  void        (NEUI_ABI *set_text)    (neui_session_t session, neui_widget_t widget,
                                        neui_item_t item, const char* text);

  void*       (NEUI_ABI *get_userdata)(neui_session_t session, neui_widget_t widget, neui_item_t item);

  // Enable or disable an item.
  // Menu bar: uses native EnableMenuItem - item is grayed. Treeview: prevents selection.
  void        (NEUI_ABI *set_enabled) (neui_session_t session, neui_widget_t widget,
                                        neui_item_t item, bool enabled);
  bool        (NEUI_ABI *get_enabled) (neui_session_t session, neui_widget_t widget, neui_item_t item);

  // Bind a keyboard shortcut to a menu item.
  // modifiers is a bitwise-OR of NEUI_KMOD_* values. key is one of NEUI_KEY_*.
  // Pass NEUI_KEY_NONE to clear an existing shortcut.
  // The host appends a platform-appropriate display label after the menu
  // item's text (e.g. "Save\tCtrl+S" on Win/Linux, "Save\tCmdS" on macOS) and
  // wires the keystroke up so pressing it fires NEUI_EVENT_TREE_ITEM_ACTIVATED
  // for this item - same event a mouse pick produces.
  // Menu bar: takes effect; the parent frame's accelerator table is rebuilt.
  // Treeview: ignored (no accelerator semantics for tree items).
  void        (NEUI_ABI *set_shortcut)(neui_session_t session, neui_widget_t widget,
                                        neui_item_t item,
                                        uint32_t modifiers, uint32_t key);

  // Tree navigation.
  neui_item_t (NEUI_ABI *get_first_child) (neui_session_t session, neui_widget_t widget, neui_item_t parent);
  neui_item_t (NEUI_ABI *get_next_sibling)(neui_session_t session, neui_widget_t widget, neui_item_t item);

  // Selection.
  // Treeview: currently highlighted node.
  // Menu bar: always returns tree_item_none (menu clicks fire events, no persistent selection).
  neui_item_t (NEUI_ABI *get_selected)(neui_session_t session, neui_widget_t widget);
  void        (NEUI_ABI *set_selected)(neui_session_t session, neui_widget_t widget, neui_item_t item);

  // Bind a menu item to a built-in command (neui_command_t). When the item
  // activates, the framework first tries to invoke the command on the
  // focused widget; if no widget consumes it, the client receives
  // NEUI_EVENT_TREE_ITEM_ACTIVATED as usual. Pass NEUI_CMD_NONE to clear.
  // Treeview: ignored.
  void        (NEUI_ABI *set_menu_cmd)(neui_session_t session, neui_widget_t widget,
                                        neui_item_t item, uint32_t command);

  // Show or hide a checkmark next to a menu item (a checkable / toggle entry).
  // The client owns the logical on/off state: flip it from the item's
  // NEUI_EVENT_TREE_ITEM_ACTIVATED handler. The host renders the mark natively -
  // Win32 MF_CHECKED, macOS NSMenuItem state, iOS UIMenu element state - and the
  // Linux in-frame menubar draws a check glyph in a left gutter. A checkmark and
  // a submenu arrow are mutually exclusive, so a top-level popup or a submenu
  // parent ignores this; separators ignore it too.
  // Treeview: ignored (no checkmark semantics for tree items).
  void        (NEUI_ABI *set_checked) (neui_session_t session, neui_widget_t widget,
                                        neui_item_t item, bool checked);
  bool        (NEUI_ABI *get_checked) (neui_session_t session, neui_widget_t widget,
                                        neui_item_t item);
} neui_tree_api_t;

#ifdef __cplusplus
}
#endif
