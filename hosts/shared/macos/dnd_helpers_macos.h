#pragma once

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#include <cstdint>
#include <string>
#include <vector>

#include "../dnd_mac_ops.h"
#include "../dnd_modifier_suggest.h"
#include "../../../include/neui/d/dnd.h"
#include "keys_macos.h"
#include "pasteboard_mime_macos.h"

// Drop-target-side NSDragOperation <-> NEUI_DND_ACTION_* translation,
// shared by the two macOS <NSDraggingDestination> implementations
// (NEUIView in hosts/crossplatform/platform_macos.mm and
// NEUINativeContentView in hosts/macos/window.mm). The drag-source side
// (dnd_source_macos.h) reuses dnd_nsop_from_action for its allowed-ops
// mask. NSDragOperation constants do NOT match DROPEFFECT_* numerically,
// hence the explicit bit-by-bit mapping - the portable core of it, including
// NSDragOperationGeneric (what AppKit makes of a Command-drag, read as MOVE),
// is hosts/shared/dnd_mac_ops.h.

namespace neui_detail
{
  // The raw NSDragOperation values hosts/shared/dnd_mac_ops.h maps (so that it
  // needs no AppKit) are AppKit's own. Checked here, where AppKit is on hand.
  static_assert(static_cast<uint64_t>(NSDragOperationCopy)    == ns_dragop::kCopy
             && static_cast<uint64_t>(NSDragOperationLink)    == ns_dragop::kLink
             && static_cast<uint64_t>(NSDragOperationGeneric) == ns_dragop::kGeneric
             && static_cast<uint64_t>(NSDragOperationPrivate) == ns_dragop::kPrivate
             && static_cast<uint64_t>(NSDragOperationMove)    == ns_dragop::kMove
             && static_cast<uint64_t>(NSDragOperationDelete)  == ns_dragop::kDelete,
                "ns_dragop values no longer match NSDragOperation");

  // A drag SOURCE's NEUI_DND_ACTION_* bitmask -> the NSDragOperation mask it
  // offers (MOVE also as Generic, so Command-drag still has an operation).
  inline NSDragOperation dnd_nsop_from_action(uint32_t action)
  {
    return static_cast<NSDragOperation>(dnd_mac_op_from_action(action));
  }

  // A drop TARGET's accepted NEUI_DND_ACTION_* -> the NSDragOperation it answers
  // draggingEntered / draggingUpdated with, inside the source's operation `mask`
  // ([sender draggingSourceOperationMask]).
  inline NSDragOperation dnd_nsop_answer(uint32_t accepted, NSDragOperation mask)
  {
    return static_cast<NSDragOperation>(dnd_mac_op_answer(accepted, mask));
  }

  // The pasteboard types a frame's content view is registered for from the start:
  // the three the framework always routes. AppKit only offers a view a drag whose
  // pasteboard carries one of its registered types.
  inline NSArray<NSPasteboardType>* dnd_base_dragged_types_macos()
  {
    return @[ NSPasteboardTypeString, NSPasteboardTypeHTML, NSPasteboardTypeFileURL ];
  }

  // Add `widget`'s accepted MIMEs and those of everything below it to `out`.
  // `Widgets` is a host's neui_detail::Tree<WidgetData>; both hosts keep the
  // list the client gave set_accepted_formats in WidgetData::accepted_mimes.
  template <class Widgets>
  inline void dnd_collect_accepted_mimes(Widgets& widgets, uint32_t widget,
                                          std::vector<std::string>& out)
  {
    if (!widgets.exists(widget)) return;
    for (const std::string& m : widgets[widget].accepted_mimes) out.push_back(m);
    for (uint32_t c = widgets.child(widget); c != 0; c = widgets.next(c))
      dnd_collect_accepted_mimes(widgets, c, out);
  }

  // Register `view` for the pasteboard types of `mimes` on top of what it is
  // registered for already (registerForDraggedTypes: adds). Without it a drag
  // that carries ONLY a custom type is never offered to the view, so the widget
  // that accepts that MIME would not see it. A MIME the system makes no type of
  // is left out.
  inline void dnd_register_mimes_macos(NSView* view,
                                        const std::vector<std::string>& mimes)
  {
    NSMutableArray<NSPasteboardType>* types = [NSMutableArray array];
    for (const std::string& m : mimes)
      if (NSString* t = pb_dragged_type_for_mime_macos(m)) [types addObject:t];
    if ([types count] > 0) [view registerForDraggedTypes:types];
  }

  // The NEUI_MK_* bits a DnD event carries in buttonmap: the live mouse buttons
  // and modifier keys, from the same shared translation as every mouse event.
  // AppKit hands a dragging destination no event, so this reads the class-level
  // state ([NSEvent modifierFlags], like dnd_suggested_from_nsop below) - which
  // is what lets a client tell Option-drag from a plain drag, or Command-drag.
  inline uint32_t dnd_buttonmap_macos()
  {
    return mac_buttonmap(NSEvent.pressedMouseButtons, NSEvent.modifierFlags);
  }

  // Source operation mask -> suggested NEUI_DND_ACTION_* for the client,
  // applying the cross-host modifier convention (dnd_modifier_suggest.h)
  // via the live [NSEvent modifierFlags] so suggested_action behaves the
  // same as the Win32 grfKeyState path.
  inline uint32_t dnd_suggested_from_nsop(NSDragOperation op)
  {
    uint32_t available = dnd_available_from_mac_op(op);
    NSEventModifierFlags mods = [NSEvent modifierFlags];
    return dnd_suggest_action(
      available,
      (mods & NSEventModifierFlagControl) != 0,
      (mods & NSEventModifierFlagShift)   != 0);
  }
}

#endif // __APPLE__
