#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "api.h"
#include "events.h"   // neui_widget_t, neui_item_t

#ifdef __cplusplus
extern "C" {
#endif

// Optional client-side interface for menu-item validation.
//
// The host calls client->get_interface(token, NEUI_API_MENU_CLIENT) once
// per session at create time; if a non-null neui_menu_client_t is returned,
// the host invokes validate() once per menu item every time a popup opens
// (Win32: WM_INITMENUPOPUP). Use this to gray menu items based on
// app-level state ("Save only when the document is dirty", "Find Next only
// when there's an active search") - including user-range commands the
// framework cannot auto-validate on its own.
//
// The framework's existing built-in auto-disable (based on the focused
// widget's WidgetData::can_perform_command) still runs first. validate()
// can only further restrict, not enable an item the framework chose to
// gray. Items the framework left enabled are passed through validate(),
// so returning false grays them; returning true leaves them enabled.

#define NEUI_API_MENU_CLIENT \
  "com.defiantnerd.neui.extension.menu.client/0"

typedef struct neui_menu_client {
  uint32_t neui_version;

  // Called once per non-separator menu item right before its parent popup
  // is shown. menubar identifies the menubar widget; item is the tree-item
  // id of the entry being validated; cmd is the routed command bound to
  // the item via neui_tree_api_t::set_menu_cmd (0 if none).
  //
  // Return true to leave the item in its current enabled state.
  // Return false to gray the item for this popup-open cycle.
  bool (NEUI_ABI *validate)(void* token,
                             neui_widget_t menubar,
                             neui_item_t   item,
                             uint32_t      cmd);
} neui_menu_client_t;

#ifdef __cplusplus
}
#endif
