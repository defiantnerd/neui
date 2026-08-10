// Tier-1 coverage for the portable half of the cursor work
// (hosts/shared/cursor_kind.h): the NEUI_ATTR_CURSOR name <-> kind mapping.
//
// The mapping is what a client actually touches - it writes a STRING attr - so
// the alias set, the round-trip, and above all the unknown-name fallback are
// the contract. A typo must leave the cursor INHERITING (DEFAULT), not pin it
// to an arrow, or a misspelling silently overrides a parent's cursor and looks
// like a framework bug.

#include "neui_test.h"

#include "cursor_kind.h"

using namespace neui_detail;

TEST_CASE("cursor_canonical_names_parse_back_to_their_kind")
{
  // Every kind's canonical name must round-trip. This is the invariant that
  // keeps get_string(set_string(x)) == x for a client that stores the name.
  for (int k = 0; k < NEUI_CURSOR_KIND_COUNT; ++k) {
    const char* name = cursor_kind_name(k);
    CHECK_EQ(cursor_kind_from_name(name), k);
  }
}

TEST_CASE("cursor_name_covers_every_kind_distinctly")
{
  // No two kinds may share a name, or the round-trip above would be
  // ambiguous and one kind would be unreachable from a string attr.
  for (int a = 0; a < NEUI_CURSOR_KIND_COUNT; ++a) {
    for (int b = a + 1; b < NEUI_CURSOR_KIND_COUNT; ++b) {
      CHECK(std::strcmp(cursor_kind_name(a), cursor_kind_name(b)) != 0);
    }
  }
}

TEST_CASE("cursor_unknown_name_falls_back_to_inherit")
{
  // The important one: an unrecognised value must mean "inherit", so a typo
  // is invisible rather than actively wrong.
  CHECK_EQ(cursor_kind_from_name("nonsense"),     NEUI_CURSOR_DEFAULT);
  CHECK_EQ(cursor_kind_from_name("EW-RESIZE"),    NEUI_CURSOR_DEFAULT);  // case-sensitive
  CHECK_EQ(cursor_kind_from_name("hand "),        NEUI_CURSOR_DEFAULT);  // no trimming
  CHECK_EQ(cursor_kind_from_name(""),             NEUI_CURSOR_DEFAULT);
  CHECK_EQ(cursor_kind_from_name(nullptr),        NEUI_CURSOR_DEFAULT);
}

TEST_CASE("cursor_overlong_name_is_rejected_not_truncated")
{
  // The parser normalises into a fixed stack buffer. A name longer than the
  // buffer must be REJECTED, never truncated into a false match - otherwise
  // "hand-but-actually-something-else" could land on a real kind.
  CHECK_EQ(cursor_kind_from_name("not-allowed-and-then-some-more"),
             NEUI_CURSOR_DEFAULT);
  // A 23-char non-match still parses (fits the buffer) and still fails to match.
  CHECK_EQ(cursor_kind_from_name("aaaaaaaaaaaaaaaaaaaaaa"), NEUI_CURSOR_DEFAULT);
  // Boundary: exactly the longest canonical names must still work.
  CHECK_EQ(cursor_kind_from_name("not-allowed"), NEUI_CURSOR_NOT_ALLOWED);
  CHECK_EQ(cursor_kind_from_name("closed-hand"), NEUI_CURSOR_CLOSED_HAND);
  CHECK_EQ(cursor_kind_from_name("nesw-resize"), NEUI_CURSOR_NESW_RESIZE);
}

TEST_CASE("cursor_underscore_is_accepted_for_hyphen")
{
  CHECK_EQ(cursor_kind_from_name("ew_resize"),   NEUI_CURSOR_EW_RESIZE);
  CHECK_EQ(cursor_kind_from_name("ns_resize"),   NEUI_CURSOR_NS_RESIZE);
  CHECK_EQ(cursor_kind_from_name("nesw_resize"), NEUI_CURSOR_NESW_RESIZE);
  CHECK_EQ(cursor_kind_from_name("nwse_resize"), NEUI_CURSOR_NWSE_RESIZE);
  CHECK_EQ(cursor_kind_from_name("open_hand"),   NEUI_CURSOR_OPEN_HAND);
  CHECK_EQ(cursor_kind_from_name("closed_hand"), NEUI_CURSOR_CLOSED_HAND);
  CHECK_EQ(cursor_kind_from_name("not_allowed"), NEUI_CURSOR_NOT_ALLOWED);
}

TEST_CASE("cursor_css_aliases_map_to_the_right_kind")
{
  // A client porting a web / JUCE UI reaches for these first.
  CHECK_EQ(cursor_kind_from_name("inherit"),    NEUI_CURSOR_DEFAULT);
  CHECK_EQ(cursor_kind_from_name("text"),       NEUI_CURSOR_IBEAM);
  CHECK_EQ(cursor_kind_from_name("cross"),      NEUI_CURSOR_CROSSHAIR);
  CHECK_EQ(cursor_kind_from_name("pointer"),    NEUI_CURSOR_HAND);
  CHECK_EQ(cursor_kind_from_name("grab"),       NEUI_CURSOR_OPEN_HAND);
  CHECK_EQ(cursor_kind_from_name("grabbing"),   NEUI_CURSOR_CLOSED_HAND);
  CHECK_EQ(cursor_kind_from_name("col-resize"), NEUI_CURSOR_EW_RESIZE);
  CHECK_EQ(cursor_kind_from_name("row-resize"), NEUI_CURSOR_NS_RESIZE);
  CHECK_EQ(cursor_kind_from_name("all-scroll"), NEUI_CURSOR_MOVE);
  CHECK_EQ(cursor_kind_from_name("busy"),       NEUI_CURSOR_WAIT);
  CHECK_EQ(cursor_kind_from_name("no-drop"),    NEUI_CURSOR_NOT_ALLOWED);
  CHECK_EQ(cursor_kind_from_name("forbidden"),  NEUI_CURSOR_NOT_ALLOWED);
  CHECK_EQ(cursor_kind_from_name("hidden"),     NEUI_CURSOR_NONE);
}

TEST_CASE("cursor_single_edge_resize_aliases_collapse_to_an_axis")
{
  // CSS has eight edge names; we have two axes plus two diagonals. Each CSS
  // name must land on the axis a client would expect, since a resize cursor
  // is symmetric anyway (a double-headed arrow, not a single one).
  CHECK_EQ(cursor_kind_from_name("e-resize"),  NEUI_CURSOR_EW_RESIZE);
  CHECK_EQ(cursor_kind_from_name("w-resize"),  NEUI_CURSOR_EW_RESIZE);
  CHECK_EQ(cursor_kind_from_name("n-resize"),  NEUI_CURSOR_NS_RESIZE);
  CHECK_EQ(cursor_kind_from_name("s-resize"),  NEUI_CURSOR_NS_RESIZE);
  CHECK_EQ(cursor_kind_from_name("ne-resize"), NEUI_CURSOR_NESW_RESIZE);
  CHECK_EQ(cursor_kind_from_name("sw-resize"), NEUI_CURSOR_NESW_RESIZE);
  CHECK_EQ(cursor_kind_from_name("nw-resize"), NEUI_CURSOR_NWSE_RESIZE);
  CHECK_EQ(cursor_kind_from_name("se-resize"), NEUI_CURSOR_NWSE_RESIZE);
}

TEST_CASE("cursor_hidden_predicate_is_only_none")
{
  for (int k = 0; k < NEUI_CURSOR_KIND_COUNT; ++k)
    CHECK_EQ(cursor_kind_is_hidden(k), k == NEUI_CURSOR_NONE);
}

TEST_CASE("cursor_out_of_range_kind_reads_back_as_default")
{
  // A client that stored a stale int (or a future kind from a newer build)
  // must not walk off the switch.
  CHECK(std::strcmp(cursor_kind_name(-1), "default") == 0);
  CHECK(std::strcmp(cursor_kind_name(NEUI_CURSOR_KIND_COUNT), "default") == 0);
  CHECK(std::strcmp(cursor_kind_name(9999), "default") == 0);
}

TEST_CASE("cursor_default_is_zero_so_a_zeroed_widget_inherits")
{
  // WidgetData is zero-initialised in places; DEFAULT must be 0 so a widget
  // that was never given a cursor inherits rather than forcing a shape.
  CHECK_EQ((int)NEUI_CURSOR_DEFAULT, 0);
}
