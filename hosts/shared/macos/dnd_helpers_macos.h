#pragma once

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#include <cstdint>

#include "../dnd_modifier_suggest.h"
#include "../../../include/neui/d/dnd.h"

// Drop-target-side NSDragOperation <-> NEUI_DND_ACTION_* translation,
// shared by the two macOS <NSDraggingDestination> implementations
// (NEUIView in hosts/crossplatform/platform_macos.mm and
// NEUINativeContentView in hosts/macos/window.mm). The drag-source side
// (dnd_source_macos.h) reuses dnd_nsop_from_action for its allowed-ops
// mask. NSDragOperation constants do NOT match DROPEFFECT_* numerically,
// hence the explicit bit-by-bit mapping.

namespace neui_detail
{
  // NEUI_DND_ACTION_* bitmask -> NSDragOperation bitmask.
  inline NSDragOperation dnd_nsop_from_action(uint32_t action)
  {
    NSDragOperation mask = NSDragOperationNone;
    if (action & NEUI_DND_ACTION_COPY) mask |= NSDragOperationCopy;
    if (action & NEUI_DND_ACTION_MOVE) mask |= NSDragOperationMove;
    if (action & NEUI_DND_ACTION_LINK) mask |= NSDragOperationLink;
    return mask;
  }

  // Source operation mask -> suggested NEUI_DND_ACTION_* for the client,
  // applying the cross-host modifier convention (dnd_modifier_suggest.h)
  // via the live [NSEvent modifierFlags] so suggested_action behaves the
  // same as the Win32 grfKeyState path.
  inline uint32_t dnd_suggested_from_nsop(NSDragOperation op)
  {
    uint32_t available = 0;
    if (op & NSDragOperationCopy) available |= NEUI_DND_ACTION_COPY;
    if (op & NSDragOperationMove) available |= NEUI_DND_ACTION_MOVE;
    if (op & NSDragOperationLink) available |= NEUI_DND_ACTION_LINK;
    NSEventModifierFlags mods = [NSEvent modifierFlags];
    return dnd_suggest_action(
      available,
      (mods & NSEventModifierFlagControl) != 0,
      (mods & NSEventModifierFlagShift)   != 0);
  }
}

#endif // __APPLE__
