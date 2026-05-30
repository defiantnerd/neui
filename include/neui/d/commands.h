#pragma once

#include <stdint.h>
#include "api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NEUI_API_COMMANDS "com.defiantnerd.neui.extension.commands/0"

// Built-in commands that widgets may handle. Bind a menu item to one with
// tree->set_menu_cmd; the framework will then route the item's activation
// (mouse pick or accelerator) through the focused widget first, falling
// through to the client only if no widget claims the command.
//
// Clients use ids >= NEUI_CMD_USER_BASE for their own menu commands; those
// remain opaque to the framework and always fire NEUI_EVENT_TREE_ITEM_ACTIVATED
// to the client (same path as a menu item with no menu_cmd set).
typedef enum neui_command
{
  NEUI_CMD_NONE       = 0,
  NEUI_CMD_UNDO       = 1,
  NEUI_CMD_REDO       = 2,
  NEUI_CMD_CUT        = 3,
  NEUI_CMD_COPY       = 4,
  NEUI_CMD_PASTE      = 5,
  NEUI_CMD_SELECT_ALL = 6,
  NEUI_CMD_DELETE     = 7,

  // Reserved for future built-ins (NEW/OPEN/SAVE/CLOSE/PRINT/FIND/REPLACE/...).
  NEUI_CMD_USER_BASE  = 0x10000,
} neui_command_t;

typedef struct neui_commands_api
{
  uint32_t neui_version;

  // Try to invoke a built-in command on the currently focused widget.
  // Returns 1 if the focused widget handled it, 0 otherwise (no focus,
  // widget doesn't respond to this command, or cmd >= NEUI_CMD_USER_BASE).
  int (NEUI_ABI *invoke_focused)(neui_session_t session, uint32_t cmd);

  // Invoke a command on a specific widget. Useful for toolbar buttons that
  // always operate on a known control. Returns 1 if the widget handled it.
  int (NEUI_ABI *invoke)(neui_session_t session, neui_widget_t widget,
                          uint32_t cmd);
} neui_commands_api_t;

#ifdef __cplusplus
}
#endif
