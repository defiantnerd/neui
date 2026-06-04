#include "neui_test.h"

#include "grid_model.h"

using namespace neui_detail;

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

// Build a model with the given rows + columns. `cells` is row-major; each row
// must have exactly column_count entries. Column kinds default to STRING; the
// test overrides per-column.
static GridModel make_sorted_grid(int column_count,
                                    const std::vector<std::vector<std::string>>& rows)
{
  GridModel m;
  m.columns.resize((size_t)column_count);
  for (const auto& r : rows) {
    GridRow gr;
    gr.cells = r;
    gr.cells.resize((size_t)column_count);
    m.rows.push_back(std::move(gr));
  }
  return m;
}

// Return the cells of the first column under the current display order. Used
// to assert sort outcomes succinctly.
static std::vector<std::string> col0_in_visual_order(GridModel& m)
{
  grid_ensure_sort_clean(m);
  std::vector<std::string> out;
  int n = (int)m.rows.size();
  out.reserve((size_t)n);
  for (int v = 0; v < n; ++v) {
    int r = grid_visual_to_logical(m, v);
    out.push_back(m.rows[(size_t)r].cells[0]);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Per-kind comparator
// ---------------------------------------------------------------------------

TEST_CASE("grid_compare_cells STRING is lexicographic")
{
  CHECK(grid_compare_cells("apple", "banana", NEUI_GRID_SORT_STRING) < 0);
  CHECK(grid_compare_cells("banana", "apple", NEUI_GRID_SORT_STRING) > 0);
  CHECK_EQ(grid_compare_cells("apple", "apple", NEUI_GRID_SORT_STRING), 0);
  // The classic surprise the INT kind avoids:
  CHECK(grid_compare_cells("10", "9", NEUI_GRID_SORT_STRING) < 0);
}

TEST_CASE("grid_compare_cells INT parses numerically")
{
  CHECK(grid_compare_cells("9", "10", NEUI_GRID_SORT_INT) < 0);
  CHECK(grid_compare_cells("10", "9", NEUI_GRID_SORT_INT) > 0);
  CHECK(grid_compare_cells("-5", "5", NEUI_GRID_SORT_INT) < 0);
  CHECK_EQ(grid_compare_cells("42", "42", NEUI_GRID_SORT_INT), 0);
}

TEST_CASE("grid_compare_cells INT: unparseable values sort last on ASC")
{
  // Parsed < unparseable, so the comparator returns negative.
  CHECK(grid_compare_cells("42", "xyz", NEUI_GRID_SORT_INT) < 0);
  CHECK(grid_compare_cells("xyz", "42", NEUI_GRID_SORT_INT) > 0);
  // Two unparseables fall back to lexicographic.
  CHECK(grid_compare_cells("aaa", "bbb", NEUI_GRID_SORT_INT) < 0);
}

TEST_CASE("grid_compare_cells FLOAT parses numerically")
{
  CHECK(grid_compare_cells("0.5", "1.5", NEUI_GRID_SORT_FLOAT) < 0);
  CHECK(grid_compare_cells("1.5", "0.5", NEUI_GRID_SORT_FLOAT) > 0);
  CHECK(grid_compare_cells("3.14", "3.14", NEUI_GRID_SORT_FLOAT) == 0);
  CHECK(grid_compare_cells("-2.0", "1.0", NEUI_GRID_SORT_FLOAT) < 0);
}

TEST_CASE("grid_compare_cells NATURAL handles 'Item 2' vs 'Item 10'")
{
  CHECK(grid_compare_cells("Item 2", "Item 10", NEUI_GRID_SORT_NATURAL) < 0);
  CHECK(grid_compare_cells("Item 10", "Item 2", NEUI_GRID_SORT_NATURAL) > 0);
  CHECK_EQ(grid_compare_cells("Item 5", "Item 5", NEUI_GRID_SORT_NATURAL), 0);
  // Multiple digit runs.
  CHECK(grid_compare_cells("v1.2.10", "v1.2.9", NEUI_GRID_SORT_NATURAL) > 0);
}

// ---------------------------------------------------------------------------
// Sort engine
// ---------------------------------------------------------------------------

TEST_CASE("grid_rebuild_display_order: empty stack -> identity (no vectors)")
{
  GridModel m = make_sorted_grid(1, {{"b"}, {"a"}, {"c"}});
  grid_ensure_sort_clean(m);
  CHECK(m.display_order.empty());
  CHECK(m.logical_to_visual.empty());
  // Identity mapping fall-through still gives the original order via
  // grid_visual_to_logical (it returns vi unchanged when display_order is
  // empty).
  CHECK_EQ(grid_visual_to_logical(m, 0), 0);
  CHECK_EQ(grid_visual_to_logical(m, 2), 2);
}

TEST_CASE("set_sort + ensure_sort_clean reorder rows ASC")
{
  GridModel m = make_sorted_grid(1, {{"banana"}, {"apple"}, {"cherry"}});
  grid_set_sort(m, 0, NEUI_GRID_SORT_ASC);
  auto v = col0_in_visual_order(m);
  CHECK_EQ((int)v.size(), 3);
  CHECK_EQ(v[0], "apple");
  CHECK_EQ(v[1], "banana");
  CHECK_EQ(v[2], "cherry");
}

TEST_CASE("set_sort DESC reverses order")
{
  GridModel m = make_sorted_grid(1, {{"banana"}, {"apple"}, {"cherry"}});
  grid_set_sort(m, 0, NEUI_GRID_SORT_DESC);
  auto v = col0_in_visual_order(m);
  CHECK_EQ(v[0], "cherry");
  CHECK_EQ(v[1], "banana");
  CHECK_EQ(v[2], "apple");
}

TEST_CASE("set_sort NONE clears the stack and reverts to identity")
{
  GridModel m = make_sorted_grid(1, {{"banana"}, {"apple"}});
  grid_set_sort(m, 0, NEUI_GRID_SORT_ASC);
  grid_set_sort(m, 0, NEUI_GRID_SORT_NONE);
  CHECK(m.sort_stack.empty());
  grid_ensure_sort_clean(m);
  CHECK(m.display_order.empty());   // identity
}

TEST_CASE("stable_sort preserves insertion order for equal keys")
{
  GridModel m = make_sorted_grid(2, {
    {"x", "first"},
    {"x", "second"},
    {"x", "third"},
  });
  grid_set_sort(m, 0, NEUI_GRID_SORT_ASC);
  grid_ensure_sort_clean(m);
  // All keys equal -> logical order preserved.
  CHECK_EQ(grid_visual_to_logical(m, 0), 0);
  CHECK_EQ(grid_visual_to_logical(m, 1), 1);
  CHECK_EQ(grid_visual_to_logical(m, 2), 2);
}

// ---------------------------------------------------------------------------
// Multi-column sort
// ---------------------------------------------------------------------------

TEST_CASE("add_sort: secondary level resolves primary-key ties")
{
  // Column 0 has two ties ("A" / "B" / "A" / "B"); column 1 breaks them.
  GridModel m = make_sorted_grid(2, {
    {"B", "2"},
    {"A", "2"},
    {"B", "1"},
    {"A", "1"},
  });
  m.columns[1].sort_kind = NEUI_GRID_SORT_INT;
  // Primary col 0 ASC, secondary col 1 ASC.
  grid_set_sort(m, 0, NEUI_GRID_SORT_ASC);
  grid_add_sort(m, 1, NEUI_GRID_SORT_ASC);
  grid_ensure_sort_clean(m);
  // Expected logical order under (col0 ASC, col1 ASC):
  //   row 3: A, 1
  //   row 1: A, 2
  //   row 2: B, 1
  //   row 0: B, 2
  CHECK_EQ(grid_visual_to_logical(m, 0), 3);
  CHECK_EQ(grid_visual_to_logical(m, 1), 1);
  CHECK_EQ(grid_visual_to_logical(m, 2), 2);
  CHECK_EQ(grid_visual_to_logical(m, 3), 0);
}

TEST_CASE("add_sort: cycle direction on an existing level")
{
  GridModel m = make_sorted_grid(1, {{"a"}, {"b"}});
  grid_set_sort(m, 0, NEUI_GRID_SORT_ASC);
  // add_sort on a column already in the stack updates the direction.
  grid_add_sort(m, 0, NEUI_GRID_SORT_DESC);
  CHECK_EQ((int)m.sort_stack.size(), 1);
  CHECK_EQ(m.sort_stack[0].dir, NEUI_GRID_SORT_DESC);
  grid_ensure_sort_clean(m);
  CHECK_EQ(grid_visual_to_logical(m, 0), 1);   // "b" sorted first under DESC
  CHECK_EQ(grid_visual_to_logical(m, 1), 0);
}

TEST_CASE("add_sort NONE removes an existing level, keeps the rest")
{
  GridModel m = make_sorted_grid(2, {{"a","1"}, {"b","1"}});
  grid_set_sort(m, 0, NEUI_GRID_SORT_ASC);
  grid_add_sort(m, 1, NEUI_GRID_SORT_DESC);
  grid_add_sort(m, 0, NEUI_GRID_SORT_NONE);
  CHECK_EQ((int)m.sort_stack.size(), 1);
  CHECK_EQ(m.sort_stack[0].col, 1);
  CHECK_EQ(m.sort_stack[0].dir, NEUI_GRID_SORT_DESC);
}

TEST_CASE("add_sort soft-caps at NEUI_GRID_SORT_MAX_LEVELS (FIFO evict)")
{
  GridModel m = make_sorted_grid(NEUI_GRID_SORT_MAX_LEVELS + 1,
                                  std::vector<std::vector<std::string>>(1,
                                    std::vector<std::string>(
                                      (size_t)(NEUI_GRID_SORT_MAX_LEVELS + 1),
                                      "")));
  // Push 8 distinct levels.
  for (int c = 0; c < NEUI_GRID_SORT_MAX_LEVELS; ++c)
    grid_add_sort(m, c, NEUI_GRID_SORT_ASC);
  CHECK_EQ((int)m.sort_stack.size(), NEUI_GRID_SORT_MAX_LEVELS);
  CHECK_EQ(m.sort_stack.front().col, 0);
  // 9th push evicts the oldest (col 0) and lands the new one at the tail.
  grid_add_sort(m, NEUI_GRID_SORT_MAX_LEVELS, NEUI_GRID_SORT_ASC);
  CHECK_EQ((int)m.sort_stack.size(), NEUI_GRID_SORT_MAX_LEVELS);
  CHECK_EQ(m.sort_stack.front().col, 1);  // col 0 evicted
  CHECK_EQ(m.sort_stack.back().col,  NEUI_GRID_SORT_MAX_LEVELS);
}

// ---------------------------------------------------------------------------
// Header-click cycle
// ---------------------------------------------------------------------------

TEST_CASE("grid_apply_header_click plain cycle: none -> asc -> desc -> none")
{
  GridModel m = make_sorted_grid(2, {{"a","1"}, {"b","2"}});
  // First click: empty -> [{col 0, ASC}].
  auto d1 = grid_apply_header_click(m, 0, /*shift=*/false);
  CHECK_EQ((int)d1, (int)NEUI_GRID_SORT_ASC);
  CHECK_EQ((int)m.sort_stack.size(), 1);
  // Second click on same column: cycle to DESC.
  auto d2 = grid_apply_header_click(m, 0, /*shift=*/false);
  CHECK_EQ((int)d2, (int)NEUI_GRID_SORT_DESC);
  CHECK_EQ((int)m.sort_stack.size(), 1);
  // Third click: cycle to NONE (stack empty).
  auto d3 = grid_apply_header_click(m, 0, /*shift=*/false);
  CHECK_EQ((int)d3, (int)NEUI_GRID_SORT_NONE);
  CHECK_EQ((int)m.sort_stack.size(), 0);
}

TEST_CASE("grid_apply_header_click plain click on a different column replaces stack")
{
  GridModel m = make_sorted_grid(2, {{"a","1"}, {"b","2"}});
  grid_apply_header_click(m, 0, false);              // [{0, ASC}]
  grid_apply_header_click(m, 0, false);              // [{0, DESC}]
  auto d = grid_apply_header_click(m, 1, false);     // [{1, ASC}] (replace)
  CHECK_EQ((int)d, (int)NEUI_GRID_SORT_ASC);
  CHECK_EQ((int)m.sort_stack.size(), 1);
  CHECK_EQ(m.sort_stack[0].col, 1);
  CHECK_EQ(m.sort_stack[0].dir, NEUI_GRID_SORT_ASC);
}

TEST_CASE("grid_apply_header_click shift on a new column appends a level")
{
  GridModel m = make_sorted_grid(3, {{"a","1","x"}, {"b","2","y"}});
  grid_apply_header_click(m, 0, false);              // [{0, ASC}]
  auto d = grid_apply_header_click(m, 1, true);      // append [{0, ASC}, {1, ASC}]
  CHECK_EQ((int)d, (int)NEUI_GRID_SORT_ASC);
  CHECK_EQ((int)m.sort_stack.size(), 2);
  CHECK_EQ(m.sort_stack[1].col, 1);
}

TEST_CASE("grid_apply_header_click shift cycles an existing secondary level")
{
  GridModel m = make_sorted_grid(2, {{"a","1"}, {"b","2"}});
  grid_apply_header_click(m, 0, false);
  grid_apply_header_click(m, 1, true);   // secondary added at ASC
  auto d = grid_apply_header_click(m, 1, true);  // cycle to DESC
  CHECK_EQ((int)d, (int)NEUI_GRID_SORT_DESC);
  CHECK_EQ((int)m.sort_stack.size(), 2);
  CHECK_EQ(m.sort_stack[1].dir, NEUI_GRID_SORT_DESC);
  auto d3 = grid_apply_header_click(m, 1, true);  // remove
  CHECK_EQ((int)d3, (int)NEUI_GRID_SORT_NONE);
  CHECK_EQ((int)m.sort_stack.size(), 1);
  CHECK_EQ(m.sort_stack[0].col, 0);
}

// ---------------------------------------------------------------------------
// Mutation-under-sort + sort_dirty
// ---------------------------------------------------------------------------

TEST_CASE("sort_dirty rebuilds on the next grid_ensure_sort_clean")
{
  GridModel m = make_sorted_grid(1, {{"b"}, {"a"}, {"c"}});
  grid_set_sort(m, 0, NEUI_GRID_SORT_ASC);
  CHECK(m.sort_dirty);
  grid_ensure_sort_clean(m);
  CHECK_FALSE(m.sort_dirty);
  // Mutate the sorted column externally + mark dirty (mimicking what each
  // host's set_cell_text does).
  m.rows[2].cells[0] = "0";  // was "c"; should sort first after rebuild
  m.sort_dirty = true;
  auto v = col0_in_visual_order(m);
  CHECK_EQ(v[0], "0");
  CHECK_EQ(v[1], "a");
  CHECK_EQ(v[2], "b");
}

// ---------------------------------------------------------------------------
// Column removal + sort stack
// ---------------------------------------------------------------------------

TEST_CASE("grid_sort_on_column_removed drops the level + shifts later indices")
{
  GridModel m = make_sorted_grid(3, {{"a","b","c"}, {"d","e","f"}});
  grid_set_sort(m, 0, NEUI_GRID_SORT_ASC);
  grid_add_sort(m, 2, NEUI_GRID_SORT_DESC);   // [{0, ASC}, {2, DESC}]
  // Remove column 1 (not in the stack). Col 2 shifts to col 1.
  grid_sort_on_column_removed(m, 1);
  REQUIRE((int)m.sort_stack.size() == 2);
  CHECK_EQ(m.sort_stack[0].col, 0);
  CHECK_EQ(m.sort_stack[1].col, 1);   // was 2
  // Remove column 0 (primary). It vanishes; remaining level stays.
  grid_sort_on_column_removed(m, 0);
  REQUIRE((int)m.sort_stack.size() == 1);
  CHECK_EQ(m.sort_stack[0].col, 0);   // was 1
}

// ---------------------------------------------------------------------------
// Inverse map + logical/visual translation
// ---------------------------------------------------------------------------

TEST_CASE("logical_to_visual is the inverse of visual_to_logical")
{
  GridModel m = make_sorted_grid(1, {{"c"}, {"a"}, {"b"}});
  grid_set_sort(m, 0, NEUI_GRID_SORT_ASC);
  grid_ensure_sort_clean(m);
  // After sort ASC the visual order is a (row 1), b (row 2), c (row 0).
  CHECK_EQ(grid_visual_to_logical(m, 0), 1);
  CHECK_EQ(grid_visual_to_logical(m, 1), 2);
  CHECK_EQ(grid_visual_to_logical(m, 2), 0);
  // Inverse.
  CHECK_EQ(grid_logical_to_visual(m, 1), 0);
  CHECK_EQ(grid_logical_to_visual(m, 2), 1);
  CHECK_EQ(grid_logical_to_visual(m, 0), 2);
}

// ---------------------------------------------------------------------------
// Sortable gate (shared helper that every host's header-click branch uses)
// ---------------------------------------------------------------------------

TEST_CASE("grid_header_click_allowed: gates on the column's sortable flag")
{
  GridModel m = make_sorted_grid(2, {{"a", "x"}, {"b", "y"}});
  // Default: every column is sortable.
  CHECK(grid_header_click_allowed(m, 0));
  CHECK(grid_header_click_allowed(m, 1));

  // Opting a column out blocks the header-click path for that column only.
  m.columns[1].sortable = false;
  CHECK(grid_header_click_allowed(m, 0));
  CHECK_FALSE(grid_header_click_allowed(m, 1));
}

TEST_CASE("grid_header_click_allowed: rejects out-of-range columns")
{
  GridModel m = make_sorted_grid(2, {{"a", "x"}});
  CHECK_FALSE(grid_header_click_allowed(m, -1));
  CHECK_FALSE(grid_header_click_allowed(m, 2));
  CHECK_FALSE(grid_header_click_allowed(m, 99));
}

TEST_CASE("sortable gate blocks the click path but NOT programmatic set/add_sort")
{
  GridModel m = make_sorted_grid(1, {{"c"}, {"a"}, {"b"}});
  m.columns[0].sortable = false;

  // The host glue would skip grid_apply_header_click for this column...
  CHECK_FALSE(grid_header_click_allowed(m, 0));

  // ...but programmatic sorting bypasses the gate by design.
  grid_set_sort(m, 0, NEUI_GRID_SORT_ASC);
  CHECK_EQ(grid_sort_stack_find(m, 0), 0);
  auto asc = col0_in_visual_order(m);
  CHECK_EQ(asc[0], "a");
  CHECK_EQ(asc[1], "b");
  CHECK_EQ(asc[2], "c");

  grid_clear_sort(m);
  grid_add_sort(m, 0, NEUI_GRID_SORT_DESC);
  CHECK_EQ(grid_sort_stack_find(m, 0), 0);
  auto desc = col0_in_visual_order(m);
  CHECK_EQ(desc[0], "c");
  CHECK_EQ(desc[1], "b");
  CHECK_EQ(desc[2], "a");
}
