#pragma once

#include <cstdint>

#include "../../include/neui/d/dnd.h"

// Shared modifier -> suggested-action convention for DnD drop targets.
// Used by both the Win32 IDropTarget path (dnd_target_win32.h) and the
// macOS NSDraggingDestination paths (platform_macos.mm / window.mm) so
// the same client code reading `suggested_action` sees the same
// modifier-driven behaviour on every host.

namespace neui_detail
{
  // Modifier convention shared by all hosts:
  //   Ctrl+Shift = Link
  //   Ctrl       = Copy
  //   Shift      = Move
  //   none       = first available (Copy > Move > Link)
  // The returned bit is masked against `available` (NEUI_DND_ACTION_*
  // bitmask). When the user expressed an explicit modifier intent that the
  // source can't satisfy, return 0 (NEUI_DND_ACTION_NONE) rather than
  // silently substituting another action - the OS then renders the no-drop
  // cursor, which is the honest signal.
  inline uint32_t dnd_suggest_action(uint32_t available, bool ctrl, bool shift)
  {
    if (ctrl && shift) return (available & NEUI_DND_ACTION_LINK) ? NEUI_DND_ACTION_LINK : 0;
    if (ctrl)          return (available & NEUI_DND_ACTION_COPY) ? NEUI_DND_ACTION_COPY : 0;
    if (shift)         return (available & NEUI_DND_ACTION_MOVE) ? NEUI_DND_ACTION_MOVE : 0;
    // No modifier: default priority.
    if (available & NEUI_DND_ACTION_COPY) return NEUI_DND_ACTION_COPY;
    if (available & NEUI_DND_ACTION_MOVE) return NEUI_DND_ACTION_MOVE;
    if (available & NEUI_DND_ACTION_LINK) return NEUI_DND_ACTION_LINK;
    return 0;
  }
}
