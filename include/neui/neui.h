#ifndef NEUI_H
#define NEUI_H

#define NEUI_VERSION (0x00000001)

#include <stdint.h>
#include <stdbool.h>

#include "d/api.h"
#include "d/keys.h"
#include "d/widgets.h"
#include "d/assets.h"
#include "d/compound.h"
#include "d/behavior.h"
#include "d/painter.h"
#include "d/events.h"
#include "d/items.h"
#include "d/tree.h"
#include "d/renderer.h"
#include "d/attrs.h"
#include "d/clipboard.h"
#include "d/dnd.h"
#include "d/commands.h"
#include "d/menu.h"
#include "d/theme.h"
#include "d/grid.h"
#include "d/scroll.h"

#ifdef __cplusplus
extern "C" {
#endif

  // Register a host implementation under a given identifier.
  // Called by the host at startup, before main() runs.
  void neui_register(const char* id, neui_api_t* api);

  // Retrieve the API for a registered host by identifier.
  // If id is NULL, returns the first registered host, or NULL if none are registered.
  neui_api_t* neui_get_api(const char* id);

  // Register every host that was compiled into this build of neuilib.
  // Single startup entry point - clients call this once before
  // neui_get_api() and don't need to know which host static libs the
  // current platform produced. Registration order is native-first
  // (win32 / macos) then xpl, so neui_get_api(NULL) returns the native
  // host where one exists. See also: neui_register / neui_get_api.
  void neui_init(void);

#ifdef __cplusplus
}
#endif      



#endif // NEUI_H