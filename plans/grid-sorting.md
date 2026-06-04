# Plan: GRID column sorting

## Context

The GRID widget is feature-complete across all three hosts (scroll-mode parity just shipped). Sorting is the next-most-common grid feature still missing - clicking a column header to sort by that column is a baseline expectation for a multi-column table.

The grid stores cells as paint-state strings; selection / cell overrides are keyed on a stable logical row index. Whatever sort design we pick has to preserve that contract or every existing client breaks.

Outcome: clients can declare a column sortable + sort-kind, the user clicks the header to cycle sort direction, the grid re-orders the visible rows without disturbing logical row identity, and a new event fires on user-driven sort changes.

## Decisions (resolved)

- **Cycle**: three-state `none → asc → desc → none`. Reaching none removes that level from the sort stack.
- **Multi-column**: Yes. Plain header click replaces the stack; Shift+click pushes / cycles a secondary level. **Soft cap at 8 levels** - Shift+click on a 9th column drops the oldest level (FIFO) and pushes the new one.
- **Sortable + sort kind**: both per-column, both explicit via API. `set_column_sortable` defaults to **true** (every new column is sortable until the client opts out); `set_column_sort_kind` defaults to STRING. No auto-detect. Non-sortable columns ignore header clicks.
- **Re-sort timing**: lazy. Mutations set `sort_dirty`; the next `paint_grid` or `hit_test` rebuilds before reading order.
- **Sort kinds**: STRING (default), INT, FLOAT, NATURAL.
- **Level indicator**: ▲ / ▼ glyph plus a small digit (1..8) showing the level's stack position. Glyph + digit drawn right of the header text; layout shrinks the text area to fit.

## Design

### Data model: indirection, not in-place

Keep `GridModel.rows` in insertion order. Add:

```cpp
struct GridSortLevel {
  int col;
  int dir;   // NEUI_GRID_SORT_ASC or _DESC (NONE never lives in the stack)
};

struct GridModel {
  ...
  std::vector<int>           display_order;     // visual -> logical; empty = identity
  std::vector<int>           logical_to_visual; // inverse, cached for selection paint
  std::vector<GridSortLevel> sort_stack;        // empty = unsorted; [0] = primary
  bool                       sort_dirty = false;
};

struct GridColumn {
  ...
  bool                  sortable  = true;
  neui_grid_sort_kind_t sort_kind = NEUI_GRID_SORT_STRING;
};
```

Why indirection: every public API row index stays logical (stable). `set_cell_text(5, ...)`, `cell_overrides[(5<<32)|col]`, `selected_row`, `add_row` returning `rows.size()-1` all keep working with no migration. "Unsorted" is `sort_stack.empty()` and `display_order.empty()` - the visible-row loop falls through to the existing direct path.

### Public API additions (`include/neui/d/grid.h`)

Appended to `neui_grid_api_t` (vtable end - existing slot offsets stay stable):

```c
typedef enum {
  NEUI_GRID_SORT_NONE = 0,
  NEUI_GRID_SORT_ASC  = 1,
  NEUI_GRID_SORT_DESC = 2,
} neui_grid_sort_dir_t;

typedef enum {
  NEUI_GRID_SORT_STRING  = 0,   // default - lexicographic
  NEUI_GRID_SORT_INT     = 1,   // strtoll; unparseable sorts last on ASC
  NEUI_GRID_SORT_FLOAT   = 2,   // strtod; same fallback policy
  NEUI_GRID_SORT_NATURAL = 3,   // "Item 2" < "Item 10"
} neui_grid_sort_kind_t;

// Per-column configuration.
void (NEUI_ABI *set_column_sortable)  (session, grid, int col, bool sortable);
void (NEUI_ABI *set_column_sort_kind) (session, grid, int col, neui_grid_sort_kind_t);

// Stack-based multi-column sort. set_sort replaces the whole stack with one
// level (NONE clears). add_sort pushes / cycles a level (Shift+click path).
// clear_sort drops every level. The getters expose the stack so a client can
// restore sort state across sessions.
void (NEUI_ABI *set_sort)        (session, grid, int col, neui_grid_sort_dir_t dir);
void (NEUI_ABI *add_sort)        (session, grid, int col, neui_grid_sort_dir_t dir);
void (NEUI_ABI *clear_sort)      (session, grid);
int  (NEUI_ABI *get_sort_count)  (session, grid);
void (NEUI_ABI *get_sort_level)  (session, grid, int level,
                                    int* out_col, neui_grid_sort_dir_t* out_dir);

// Translate between logical (data-identity, stable) and visual (current sort
// position) row indices.
//
// logical_to_visual_row(grid, r) -> the row's POSITION IN THE CURRENT SORT
//   ORDER (0 = topmost displayed row). This is the answer to "where does this
//   row sit in the sorted view?" - call it on the row delivered by any
//   GRID_*_CLICKED / _SELECTED event to know the on-screen sort position.
// visual_to_logical_row(grid, v) -> the data-record (logical) row at sort
//   position v.
//
// Both return -1 on out-of-range. When no sort is active, the mapping is
// identity. Both force a rebuild if sort_dirty is set, so they give correct
// values immediately after add_row / set_cell_text.
int  (NEUI_ABI *logical_to_visual_row)(session, grid, int logical_row);
int  (NEUI_ABI *visual_to_logical_row)(session, grid, int visual_row);
```

### Event contract: logical row, always

`GRID_ROW_SELECTED`, `GRID_CELL_SELECTED`, `GRID_CELL_CLICKED`, `GRID_ROW_ACTIVATED` continue to carry **logical** row + col. A click on the topmost visible row of a sorted grid delivers the logical index of whichever record sorted to the top - the same index that `set_cell_text` / `get_cell_text` / `cell_overrides` use, and the same index `set_selected_row` accepts. No client-side translation is needed for the data path.

A client that wants "which numbered row from the top did the user click?" calls `logical_to_visual_row(grid, ev->data.grid_cell.row)`. A client that wants to introspect the sort itself calls `get_sort_count` + `get_sort_level`.

The hit-test API (`hit_test(grid, x, y, out_row, out_col)`) likewise returns the **logical** row.

### Click semantics (precise)

Plain click on a sortable header:
- If header is the **only** level in the stack: cycle its direction (asc → desc → empty stack).
- Otherwise: replace the stack with one entry, that column at asc.

Shift+click on a sortable header:
- If column is **already** in the stack: cycle that level's direction (asc → desc → remove that level, leaving the rest in place).
- Otherwise: append the column as a new level at asc. If the stack already has 8 levels, drop `sort_stack[0]` (the oldest / primary) before pushing - FIFO behaviour keeps the most recently expressed user intent.

Click on a non-sortable column (or `HeaderDivider`): ignored by the sort path. `HeaderDivider` continues to trigger the existing column-resize drag.

### Event

`NEUI_EVENT_GRID_SORT_CHANGED { widget, col, dir }` in `events.h`. Fires on user-driven header clicks only; programmatic `set_sort` stays silent (mirrors `_COLUMN_RESIZED` and `VALUE_CHANGED` semantics).

### Sort engine (`hosts/shared/grid_model.h`)

```cpp
inline void grid_rebuild_display_order(GridModel& m);
// Returns true if the stack changed (so the caller fires the event).
inline bool grid_apply_header_click(GridModel& m, int col, bool shift_held);
```

`grid_rebuild_display_order`:
- If `sort_stack.empty()`: clear `display_order` + `logical_to_visual`. Done.
- Else build identity 0..n-1, `std::stable_sort` with a comparator that walks
  the `sort_stack` levels:

  ```cpp
  for (auto& lvl : sort_stack) {
    int c = compare_by_kind(rows[a].cells[lvl.col],
                            rows[b].cells[lvl.col],
                            columns[lvl.col].sort_kind);
    if (c != 0) return lvl.dir == ASC ? c < 0 : c > 0;
  }
  return false;  // equal; stable_sort preserves insertion order
  ```

- Per-kind comparator returns -1 / 0 / +1:
  - STRING: `strcmp` (locale-aware deferred to v2).
  - INT: `strtoll`; unparseable values group at one end (last on ASC).
  - FLOAT: `strtod`; same rule.
  - NATURAL: alternating digit / non-digit run compare.
- Build `logical_to_visual` inverse.
- Clear `sort_dirty`.

`stable_sort` is load-bearing: equal keys preserve insertion order so the result is deterministic and the multi-column tie-break is just "insertion order at the deepest tie".

### Hit-test + click handling

The existing `GridHitRegion::Header` already returns the column index. The current header-click branch is a no-op. Add a shared helper that runs the sort cycle + fires the event; each host calls it from its existing `GridHitRegion::Header` switch case.

### Paint additions (`hosts/shared/widget_paint_grid.h`)

1. **Visible-row loop indirection**: every `m.rows[r]` in the body fill loop becomes `m.rows[grid_visual_to_logical(m, r)]`. `selected_row` highlight band uses `logical_to_visual[selected_row]` to find its visual position.
2. **Sort glyph + level number in header**: for each column whose index appears in `sort_stack`, draw `▲` (ASC) or `▼` (DESC) right-aligned in the header text area, followed by the 1-based level number (1..8) in a smaller font. Single-level sorts hide the digit (no point cluttering the common case). Colour: `ColorRole::accent` for the primary level, `text_secondary` for the rest, so the eye lands on the primary first. The text-measure pass shrinks the available header text width by glyph + digit width before truncation / ellipsis.
3. (Optional, behind an attr) subtle column-header tint on sorted columns. Deferred unless it falls out for free.

### Per-host glue

Each of these grows by one branch in the header-click switch:
- `hosts/win32/widgets.cpp` `painted_msg_grid_w32` - existing `GridHitRegion::Header` case.
- `hosts/macos/window.mm` `mouseDown:` GRID branch.
- `hosts/crossplatform/host.cpp` `GridWidget::on_mouse_event`.

No changes to keyboard nav, drag, paint structure beyond the indirection.

### Selection + keyboard semantics

- `selected_row` stays a logical index. The highlight band paints at `logical_to_visual[selected_row]`.
- Up/Down arrow keys move in **visual** order (matching what users see), then store the new logical row corresponding to the new visual position.
- `ensure_row_visible(logical_row)` translates to visual position before clamping `scroll_offset_y`.
- Click in body: hit-test returns the logical row at the clicked visual position (one indirection in the body branch of `grid_hit_test`).

### Mutation under a live sort

(See open decision on lazy vs eager.) Either way:
- `add_row`: append to `rows`; mark dirty (or insert via binary search into `display_order`).
- `remove_row`: drop from `rows`, prune from `display_order`, decrement indices > removed, rebuild inverse. Selection adjusts if the selected row was removed.
- `set_cell_text` on the sorted column: dirty (or re-sort).
- `clear_rows`, `clear_columns`, `remove_column`: clear sort state (column index would shift; safer than trying to remap).
- `set_column_sort_kind` on the currently sorted column: re-sort.

## Critical files

- `include/neui/d/grid.h` - enums + vtable append.
- `include/neui/d/events.h` - new event constant + payload struct.
- `hosts/shared/grid_model.h` - `display_order` + inverse + sort kind on `GridColumn` + `grid_rebuild_display_order` + `grid_handle_header_click` + indirection helpers.
- `hosts/shared/widget_paint_grid.h` - visible-row indirection, selection paint via inverse, sort glyph.
- `hosts/win32/widgets.cpp` - one branch in `painted_msg_grid_w32`'s `Header` hit case.
- `hosts/macos/window.mm` - one branch in `mouseDown:` GRID handler.
- `hosts/crossplatform/host.cpp` - one branch in `GridWidget::on_mouse_event`.
- `examples/grid_example.cpp` - call `set_column_sort_kind` on numeric columns; add a sort-changed status handler.
- `CLAUDE.md` - API rows + a paragraph in the GRID section.

## Verification

- Build all hosts; the existing test suite must still pass (unsorted = identity, so paint + hit-test paths are unchanged for existing clients).
- Add `tests/test_grid_sort.cpp`:
  - Per-kind comparator: STRING / INT / FLOAT / NATURAL produce the expected orderings; unparseable values group at the end on ASC.
  - Stability under stable_sort (equal keys keep insertion order).
  - Multi-column chain: primary + secondary resolves ties correctly.
  - Soft cap at 8: a 9th `add_sort` evicts level 0.
  - `set_column_sortable(false)` blocks `grid_apply_header_click` for that column but does not block programmatic `set_sort` / `add_sort`.
  - Mutation under sort: `sort_dirty` flips on `add_row` / `set_cell_text` / `remove_row`; rebuild produces the right order.
- Manual end-to-end in `neui_grid_example`:
  1. Click `#` header three times → asc / desc / unsorted.
  2. Sort `Value` numerically; verify 9 < 10 (catches the STRING-default trap, which the example now avoids by calling `set_column_sort_kind(FLOAT)`).
  3. Shift+click `Category` after sorting `Name` → primary by Name, secondary by Category; header shows ▲1 / ▲2.
  4. Sort, then click `Add row` → the new row appears at its correct sorted position; selection (if any) stays on the same logical record visually.
  4a. Sort, then click a body cell → the `GRID_CELL_CLICKED` payload carries the **logical** row (verifiable in the status label: clicking the topmost visible row after sort should NOT report row=0 unless logical row 0 happens to be on top). `logical_to_visual_row` on the payload's row returns 0.
  5. Sort, then keyboard Up/Down → cursor steps through visual order.
  6. Sort, then `Clear` → both rows and sort stack are cleared.
  7. Toggle `set_column_sortable(false)` on a column → its header no longer responds to clicks (verify via a temporary debug toggle in the example, or skip if not implemented in the demo).
  8. Push 8 sort levels by Shift+clicking through columns; Shift+click a 9th → the oldest level disappears and the new one appears at the tail.
