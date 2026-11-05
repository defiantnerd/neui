#pragma once

#include <cstdint>

#include "../../include/neui/d/dnd.h"

// The portable core of the macOS NSDragOperation <-> NEUI_DND_ACTION_* mapping.
//
// NSDragOperation is a bitmask whose values do NOT match NEUI_DND_ACTION_* /
// DROPEFFECT_*, and AppKit filters it by the modifier keys held during a drag
// (documented on -[NSDraggingInfo draggingSourceOperationMask]):
//
//   Control -> Link, Option -> Copy, Command -> NSDragOperationGeneric
//
// Generic is the Finder's spelling of Command-drag, which means MOVE. A source
// that offered only NSDragOperationMove would therefore have its mask filtered to
// nothing while Command is held (no-drop cursor, no drop), and a drag from the
// Finder with Command held reaches a target with the mask Generic alone. So this
// mapping treats Generic as a second spelling of MOVE in all four directions
// below.
//
// The decoding is plain bit arithmetic over integers, so it lives HERE - in a
// header that includes no AppKit - and is Tier-1 tested
// (tests/test_dnd_mac_ops.cpp). hosts/shared/macos/dnd_helpers_macos.h and
// dnd_source_macos.h are the thin wrappers that hand it the live NSDragOperation
// values; the former static_asserts that the raw constants below are AppKit's own.

namespace neui_detail
{
  // NSDragOperation bits (NSUInteger; AppKit's own values, checked in
  // dnd_helpers_macos.h).
  namespace ns_dragop
  {
    constexpr uint64_t kCopy    = 1ull << 0;   // NSDragOperationCopy
    constexpr uint64_t kLink    = 1ull << 1;   // NSDragOperationLink
    constexpr uint64_t kGeneric = 1ull << 2;   // NSDragOperationGeneric
    constexpr uint64_t kPrivate = 1ull << 3;   // NSDragOperationPrivate
    constexpr uint64_t kMove    = 1ull << 4;   // NSDragOperationMove
    constexpr uint64_t kDelete  = 1ull << 5;   // NSDragOperationDelete
  }

  // A drag SOURCE's allowed NEUI_DND_ACTION_* -> the NSDragOperation mask it
  // offers AppKit. MOVE is offered as Move AND Generic, so Command-drag (which
  // AppKit reports as Generic) still leaves an operation after the filter.
  inline uint64_t dnd_mac_op_from_action(uint32_t action)
  {
    uint64_t mask = 0;
    if (action & NEUI_DND_ACTION_COPY) mask |= ns_dragop::kCopy;
    if (action & NEUI_DND_ACTION_MOVE) mask |= ns_dragop::kMove | ns_dragop::kGeneric;
    if (action & NEUI_DND_ACTION_LINK) mask |= ns_dragop::kLink;
    return mask;
  }

  // The source operation mask a drop TARGET is handed (draggingSourceOperationMask,
  // already filtered by the modifier keys) -> the NEUI_DND_ACTION_* it makes
  // available. Generic counts as MOVE.
  inline uint32_t dnd_available_from_mac_op(uint64_t mask)
  {
    uint32_t available = 0;
    if (mask & ns_dragop::kCopy)                          available |= NEUI_DND_ACTION_COPY;
    if (mask & (ns_dragop::kMove | ns_dragop::kGeneric))  available |= NEUI_DND_ACTION_MOVE;
    if (mask & ns_dragop::kLink)                          available |= NEUI_DND_ACTION_LINK;
    return available;
  }

  // What a drop TARGET answers from draggingEntered / draggingUpdated, given the
  // action the client accepted and the source's operation mask: AppKit only takes
  // an answer inside that mask. MOVE is Move when the mask has it, else Generic
  // (what Command-drag leaves); with neither it stays Move, as it was before
  // Generic was known. COPY and LINK map one to one.
  inline uint64_t dnd_mac_op_answer(uint32_t accepted, uint64_t mask)
  {
    uint64_t op = 0;
    if (accepted & NEUI_DND_ACTION_COPY) op |= ns_dragop::kCopy;
    if (accepted & NEUI_DND_ACTION_MOVE)
      op |= (mask & ns_dragop::kMove)    ? ns_dragop::kMove
          : (mask & ns_dragop::kGeneric) ? ns_dragop::kGeneric
          :                                ns_dragop::kMove;
    if (accepted & NEUI_DND_ACTION_LINK) op |= ns_dragop::kLink;
    return op;
  }

  // The operation a drag SESSION ended with (draggingSession:endedAtPoint:operation:)
  // -> the NEUI_DND_ACTION_* the source's begin_drag returns. AppKit reports one
  // bit; probed in the order COPY > MOVE > LINK, with Generic read as MOVE.
  inline uint32_t dnd_action_from_mac_op(uint64_t op)
  {
    if (op & ns_dragop::kCopy)                          return NEUI_DND_ACTION_COPY;
    if (op & (ns_dragop::kMove | ns_dragop::kGeneric))  return NEUI_DND_ACTION_MOVE;
    if (op & ns_dragop::kLink)                          return NEUI_DND_ACTION_LINK;
    return 0;
  }
}
