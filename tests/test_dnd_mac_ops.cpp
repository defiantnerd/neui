// Tier-1 coverage for hosts/shared/dnd_mac_ops.h - the portable core of the
// macOS NSDragOperation <-> NEUI_DND_ACTION_* mapping.
//
// AppKit filters a drag's operation mask by the modifier keys held (Control ->
// Link, Option -> Copy, Command -> NSDragOperationGeneric), and Generic is the
// Finder's Command-drag = MOVE. The cases below pin that Generic is read as MOVE
// in every direction: what a source offers, what a target is told is available,
// what a target answers, and what a finished session reports.
//
// The AppKit values are written out (Copy = 1, Link = 2, Generic = 4, Private = 8,
// Move = 16, Delete = 32); hosts/shared/macos/dnd_helpers_macos.h static_asserts
// that they are the SDK's own.

#include "neui_test.h"

#include "dnd_mac_ops.h"
#include "dnd_modifier_suggest.h"

using namespace neui_detail;

namespace
{
  const uint64_t kCopy = 1, kLink = 2, kGeneric = 4, kPrivate = 8, kMove = 16, kDelete = 32;
  const uint64_t kEvery = ~0ull;   // NSDragOperationEvery is NSUIntegerMax
}

TEST_CASE("ns_dragop: the constants are the NSDragOperation values")
{
  CHECK_EQ(ns_dragop::kCopy,    kCopy);
  CHECK_EQ(ns_dragop::kLink,    kLink);
  CHECK_EQ(ns_dragop::kGeneric, kGeneric);
  CHECK_EQ(ns_dragop::kPrivate, kPrivate);
  CHECK_EQ(ns_dragop::kMove,    kMove);
  CHECK_EQ(ns_dragop::kDelete,  kDelete);
}

// ---------------------------------------------------------------------------
// A drag source's mask
// ---------------------------------------------------------------------------

TEST_CASE("dnd_mac_op_from_action: a source that allows MOVE also offers Generic")
{
  CHECK_EQ(dnd_mac_op_from_action(NEUI_DND_ACTION_MOVE), kMove | kGeneric);
  CHECK_EQ(dnd_mac_op_from_action(NEUI_DND_ACTION_COPY | NEUI_DND_ACTION_MOVE),
           kCopy | kMove | kGeneric);
  CHECK_EQ(dnd_mac_op_from_action(NEUI_DND_ACTION_COPY | NEUI_DND_ACTION_MOVE | NEUI_DND_ACTION_LINK),
           kCopy | kLink | kMove | kGeneric);
}

TEST_CASE("dnd_mac_op_from_action: COPY and LINK are unchanged, and only MOVE adds Generic")
{
  CHECK_EQ(dnd_mac_op_from_action(NEUI_DND_ACTION_NONE), 0ull);
  CHECK_EQ(dnd_mac_op_from_action(NEUI_DND_ACTION_COPY), kCopy);
  CHECK_EQ(dnd_mac_op_from_action(NEUI_DND_ACTION_LINK), kLink);
  CHECK_EQ(dnd_mac_op_from_action(NEUI_DND_ACTION_COPY | NEUI_DND_ACTION_LINK), kCopy | kLink);
}

TEST_CASE("a MOVE source's mask, filtered the way AppKit does for Command, is not empty")
{
  // Command held: AppKit keeps only the Generic bit of what the source offered.
  const uint64_t offered  = dnd_mac_op_from_action(NEUI_DND_ACTION_MOVE);
  const uint64_t filtered = offered & kGeneric;
  CHECK(filtered != 0);
  // A source that offered Move alone would have been left with nothing.
  CHECK_EQ(kMove & kGeneric, 0ull);
}

// ---------------------------------------------------------------------------
// What a drop target is told is available
// ---------------------------------------------------------------------------

TEST_CASE("dnd_available_from_mac_op: Copy, Move and Link map one to one")
{
  CHECK_EQ(dnd_available_from_mac_op(0), 0u);
  CHECK_EQ(dnd_available_from_mac_op(kCopy), (uint32_t)NEUI_DND_ACTION_COPY);
  CHECK_EQ(dnd_available_from_mac_op(kMove), (uint32_t)NEUI_DND_ACTION_MOVE);
  CHECK_EQ(dnd_available_from_mac_op(kLink), (uint32_t)NEUI_DND_ACTION_LINK);
  CHECK_EQ(dnd_available_from_mac_op(kCopy | kMove | kLink),
           (uint32_t)(NEUI_DND_ACTION_COPY | NEUI_DND_ACTION_MOVE | NEUI_DND_ACTION_LINK));
  // Private and Delete are nothing neui has an action for.
  CHECK_EQ(dnd_available_from_mac_op(kPrivate | kDelete), 0u);
}

TEST_CASE("dnd_available_from_mac_op: Generic counts as MOVE")
{
  CHECK_EQ(dnd_available_from_mac_op(kGeneric), (uint32_t)NEUI_DND_ACTION_MOVE);
  CHECK_EQ(dnd_available_from_mac_op(kMove | kGeneric), (uint32_t)NEUI_DND_ACTION_MOVE);
  CHECK_EQ(dnd_available_from_mac_op(kCopy | kGeneric),
           (uint32_t)(NEUI_DND_ACTION_COPY | NEUI_DND_ACTION_MOVE));
  // NSDragOperationEvery: everything, as a drag with no modifier from some apps reports.
  CHECK_EQ(dnd_available_from_mac_op(kEvery),
           (uint32_t)(NEUI_DND_ACTION_COPY | NEUI_DND_ACTION_MOVE | NEUI_DND_ACTION_LINK));
}

TEST_CASE("a Command-filtered mask (Generic only) suggests MOVE")
{
  // With Command held and no Ctrl / Shift, the suggestion is what the mask makes
  // available: before Generic was known that was nothing at all.
  const uint32_t available = dnd_available_from_mac_op(kGeneric);
  CHECK_EQ(dnd_suggest_action(available, false, false), (uint32_t)NEUI_DND_ACTION_MOVE);
  // A drag from a MOVE source, filtered by Command, the same.
  const uint64_t mask = dnd_mac_op_from_action(NEUI_DND_ACTION_MOVE) & kGeneric;
  CHECK_EQ(dnd_suggest_action(dnd_available_from_mac_op(mask), false, false),
           (uint32_t)NEUI_DND_ACTION_MOVE);
}

// ---------------------------------------------------------------------------
// What a drop target answers
// ---------------------------------------------------------------------------

TEST_CASE("dnd_mac_op_answer: accepting MOVE answers Generic when the mask is Generic only")
{
  CHECK_EQ(dnd_mac_op_answer(NEUI_DND_ACTION_MOVE, kGeneric), kGeneric);
}

TEST_CASE("dnd_mac_op_answer: accepting MOVE answers Move when the mask has Move")
{
  CHECK_EQ(dnd_mac_op_answer(NEUI_DND_ACTION_MOVE, kMove), kMove);
  CHECK_EQ(dnd_mac_op_answer(NEUI_DND_ACTION_MOVE, kMove | kGeneric), kMove);
  CHECK_EQ(dnd_mac_op_answer(NEUI_DND_ACTION_MOVE, kCopy | kMove | kGeneric), kMove);
  CHECK_EQ(dnd_mac_op_answer(NEUI_DND_ACTION_MOVE, kEvery), kMove);
}

TEST_CASE("dnd_mac_op_answer: the answer to a MOVE source's mask is inside the mask")
{
  const uint64_t offered = dnd_mac_op_from_action(NEUI_DND_ACTION_MOVE);   // no modifier
  const uint64_t command = offered & kGeneric;                             // Command held
  CHECK((dnd_mac_op_answer(NEUI_DND_ACTION_MOVE, offered) & offered) != 0);
  CHECK((dnd_mac_op_answer(NEUI_DND_ACTION_MOVE, command) & command) != 0);
}

TEST_CASE("dnd_mac_op_answer: COPY and LINK are unchanged, whatever the mask")
{
  CHECK_EQ(dnd_mac_op_answer(NEUI_DND_ACTION_NONE, kEvery), 0ull);
  CHECK_EQ(dnd_mac_op_answer(NEUI_DND_ACTION_COPY, kCopy), kCopy);
  CHECK_EQ(dnd_mac_op_answer(NEUI_DND_ACTION_COPY, kGeneric), kCopy);
  CHECK_EQ(dnd_mac_op_answer(NEUI_DND_ACTION_LINK, kLink), kLink);
  CHECK_EQ(dnd_mac_op_answer(NEUI_DND_ACTION_LINK, kGeneric), kLink);
  CHECK_EQ(dnd_mac_op_answer(NEUI_DND_ACTION_COPY | NEUI_DND_ACTION_LINK, kEvery), kCopy | kLink);
  // A bitmask answer: MOVE follows the mask, COPY stays.
  CHECK_EQ(dnd_mac_op_answer(NEUI_DND_ACTION_COPY | NEUI_DND_ACTION_MOVE, kCopy | kGeneric),
           kCopy | kGeneric);
  CHECK_EQ(dnd_mac_op_answer(NEUI_DND_ACTION_COPY | NEUI_DND_ACTION_MOVE, kCopy | kMove),
           kCopy | kMove);
}

TEST_CASE("dnd_mac_op_answer: MOVE with neither Move nor Generic in the mask stays Move, as before")
{
  CHECK_EQ(dnd_mac_op_answer(NEUI_DND_ACTION_MOVE, 0), kMove);
  CHECK_EQ(dnd_mac_op_answer(NEUI_DND_ACTION_MOVE, kCopy | kLink), kMove);
}

// ---------------------------------------------------------------------------
// What a finished session reports to the source
// ---------------------------------------------------------------------------

TEST_CASE("dnd_action_from_mac_op: a session that ended in Generic is MOVE")
{
  CHECK_EQ(dnd_action_from_mac_op(kGeneric), (uint32_t)NEUI_DND_ACTION_MOVE);
}

TEST_CASE("dnd_action_from_mac_op: Copy, Move and Link map one to one, none is none")
{
  CHECK_EQ(dnd_action_from_mac_op(0), 0u);
  CHECK_EQ(dnd_action_from_mac_op(kCopy), (uint32_t)NEUI_DND_ACTION_COPY);
  CHECK_EQ(dnd_action_from_mac_op(kMove), (uint32_t)NEUI_DND_ACTION_MOVE);
  CHECK_EQ(dnd_action_from_mac_op(kLink), (uint32_t)NEUI_DND_ACTION_LINK);
  // Private and Delete have no action.
  CHECK_EQ(dnd_action_from_mac_op(kPrivate), 0u);
  CHECK_EQ(dnd_action_from_mac_op(kDelete), 0u);
}

TEST_CASE("dnd_action_from_mac_op: several bits keep the order COPY > MOVE > LINK")
{
  CHECK_EQ(dnd_action_from_mac_op(kCopy | kMove | kLink | kGeneric), (uint32_t)NEUI_DND_ACTION_COPY);
  CHECK_EQ(dnd_action_from_mac_op(kMove | kLink), (uint32_t)NEUI_DND_ACTION_MOVE);
  CHECK_EQ(dnd_action_from_mac_op(kGeneric | kLink), (uint32_t)NEUI_DND_ACTION_MOVE);
}

TEST_CASE("a MOVE source's round trip: Command-drag onto a target ends as MOVE")
{
  // Source offers MOVE; Command filters the mask to Generic; the target is told MOVE
  // is available, the client accepts it, the target answers inside the mask, and the
  // session ends with that operation, which the source reads back as MOVE.
  const uint64_t mask      = dnd_mac_op_from_action(NEUI_DND_ACTION_MOVE) & kGeneric;
  const uint32_t suggested = dnd_suggest_action(dnd_available_from_mac_op(mask), false, false);
  CHECK_EQ(suggested, (uint32_t)NEUI_DND_ACTION_MOVE);
  const uint64_t answer = dnd_mac_op_answer(suggested, mask);
  CHECK_EQ(answer, kGeneric);
  CHECK_EQ(dnd_action_from_mac_op(answer), (uint32_t)NEUI_DND_ACTION_MOVE);
}
