#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <neui/neui.h>

#ifdef __cplusplus
extern "C" {
#endif

// Grid (table) API - reached via
//   neui_grid_api_t* grid = (neui_grid_api_t*)
//     neui_api->get_interface(session, NEUI_API_GRID);
// and operates on NEUI_W_GRID widgets.
//
// A grid is a scrollable, multi-column row-of-cells widget. The column
// model is configured up front (add_column, set_column_width, ...) and
// rows are populated via add_row + set_cell_text. Cells default to a
// single text string; per-cell colour / enabled overrides are sparse.
//
// Cells are NOT widgets - they exist only as paint-state. A 10000 x 8
// grid is a single widget, not 80 000 widgets, and would otherwise
// exceed the per-session 16-bit widget-id slot space.
//
// Two focus modes, toggled by NEUI_ATTR_GRID_CELL_FOCUS:
//   0 (default) - row-focus. One selected row; arrows move row.
//   1           - cell-focus. (row, col) cursor; arrows move cell.
//
// Event delivery on a click (each fired only if the previous one in the
// ladder was not consumed by the client's onevent):
//   GRID_ROW_SELECTED  (always; fires first)
//   GRID_CELL_SELECTED (only when cell_focus = 1)
//   GRID_CELL_CLICKED  (raw fallback; carries row + col regardless of mode)
// On the second click of a double-click sequence:
//   GRID_ROW_ACTIVATED
//
// Column resize fires GRID_COLUMN_RESIZED on release; programmatic
// set_column_width does NOT fire it (mirrors VALUE_CHANGED semantics).

#define NEUI_API_GRID "com.defiantnerd.neui.extension.grid/0"

// Widget type string used with widgets->create.
#define NEUI_W_GRID "neui.std.grid"

// ---- Well-known GRID attributes ------------------------------------------

// int (logical pixels): height of one row in the body. Default 22.
#define NEUI_ATTR_GRID_ROW_HEIGHT "neui.attr.grid.row_height"

// int (logical pixels): height of the sticky column-header band. Default 24.
#define NEUI_ATTR_GRID_HEADER_HEIGHT "neui.attr.grid.header_height"

// int (ARGB): colour of the focus-row highlight band painted under the
// selected row. 0 / unset = use the active theme's accent role at 0x33
// alpha so it reads on both light and dark backgrounds.
#define NEUI_ATTR_GRID_FOCUS_ROW_COLOR "neui.attr.grid.focus_row_color"

// int (bool): paint the focus-row highlight under the selected row.
// Default 1. Independent of NEUI_ATTR_GRID_CELL_FOCUS - the highlight
// follows the selected row in both modes.
#define NEUI_ATTR_GRID_SHOW_FOCUS_ROW "neui.attr.grid.show_focus_row"

// int (logical pixels): default minimum width applied to new columns
// that don't have their own min_width set via set_column_min_width.
// Default 24.
#define NEUI_ATTR_GRID_COLUMN_MIN_WIDTH_DEFAULT \
  "neui.attr.grid.column_min_width_default"

// int (bool): cursor mode.
//   0 (default) - row-focus. Arrow keys move the selected row.
//   1           - cell-focus. A (row, col) cursor is drawn as a 1 px
//                 accent outline; Left / Right move the cursor between
//                 columns, Home / End jump to the first / last column
//                 of the current row, Ctrl+Home / Ctrl+End jump to the
//                 grid corners.
// Live - flipping the attr updates the cursor + next paint.
#define NEUI_ATTR_GRID_CELL_FOCUS "neui.attr.grid.cell_focus"

// int (neui_grid_scroll_mode_t): wheel kinetics selector.
//   NEUI_GRID_SCROLL_PLATFORM (0, default) - host picks the natural feel.
//                              macOS: SMOOTH. Win32 / null: STEPPED.
//   NEUI_GRID_SCROLL_STEPPED  (1) - row-quantized wheel, hard-clamp at
//                              top / bottom, no momentum, no rubber-band.
//   NEUI_GRID_SCROLL_SMOOTH   (2) - pixel-precise sub-row motion with
//                              WebKit-style elastic overscroll and a
//                              60 Hz spring-back to the boundary.
// Live - the next wheel tick uses the new mode. Flipping from SMOOTH to
// STEPPED while a spring-back is animating cancels the animation and
// snaps to the nearest row.
#define NEUI_ATTR_GRID_SCROLL_MODE "neui.attr.grid.scroll_mode"

typedef enum neui_grid_scroll_mode {
  NEUI_GRID_SCROLL_PLATFORM = 0,
  NEUI_GRID_SCROLL_STEPPED  = 1,
  NEUI_GRID_SCROLL_SMOOTH   = 2,
} neui_grid_scroll_mode_t;

// ---- Sorting -------------------------------------------------------------

// Direction of a single sort level. NEUI_GRID_SORT_NONE used by set_sort to
// clear the stack; never appears inside an active level.
typedef enum neui_grid_sort_dir {
  NEUI_GRID_SORT_NONE = 0,
  NEUI_GRID_SORT_ASC  = 1,
  NEUI_GRID_SORT_DESC = 2,
} neui_grid_sort_dir_t;

// How a column's cell strings are compared.
//   STRING  - lexicographic (strcmp).
//   INT     - parse via strtoll; unparseable values sort last on ASC.
//   FLOAT   - parse via strtod; same fallback policy.
//   NATURAL - alternating digit / non-digit runs ("Item 2" < "Item 10").
typedef enum neui_grid_sort_kind {
  NEUI_GRID_SORT_STRING  = 0,
  NEUI_GRID_SORT_INT     = 1,
  NEUI_GRID_SORT_FLOAT   = 2,
  NEUI_GRID_SORT_NATURAL = 3,
} neui_grid_sort_kind_t;

// Soft cap on the number of active sort levels. A Shift+click that would
// exceed this drops the oldest level (FIFO eviction) before pushing.
#define NEUI_GRID_SORT_MAX_LEVELS 8

// Event payload structs (neui_event_grid_row_t, neui_event_grid_cell_t,
// neui_event_grid_column_resize_t) and event type constants
// (NEUI_EVENT_GRID_ROW_SELECTED, _CELL_SELECTED, _ROW_ACTIVATED,
// _CELL_CLICKED, _COLUMN_RESIZED) live in <neui/d/events.h> alongside
// the rest of the event taxonomy.

// ---- API surface ---------------------------------------------------------

typedef struct neui_grid_api {
  uint32_t neui_version;

  // -------- Columns ------------------------------------------------------

  // Append a column. Returns the new column index (0-based) on success
  // or -1 on bad widget. `header` is UTF-8; pass "" for an unlabeled
  // column. `width_logical` is the initial width in logical pixels at
  // 96 DPI.
  int  (NEUI_ABI *add_column)        (neui_session_t session, neui_widget_t grid,
                                       const char* header, int width_logical);

  int  (NEUI_ABI *get_column_count)  (neui_session_t session, neui_widget_t grid);

  void (NEUI_ABI *set_column_width)  (neui_session_t session, neui_widget_t grid,
                                       int col, int width_logical);
  int  (NEUI_ABI *get_column_width)  (neui_session_t session, neui_widget_t grid,
                                       int col);

  void (NEUI_ABI *set_column_min_width)(neui_session_t session, neui_widget_t grid,
                                          int col, int min_w_logical);

  // Per-column horizontal text alignment. align is "left" (default),
  // "center", or "right". Unknown strings are ignored.
  void (NEUI_ABI *set_column_align)  (neui_session_t session, neui_widget_t grid,
                                       int col, const char* align);

  // Set / get the column's header text (UTF-8).
  void (NEUI_ABI *set_column_header) (neui_session_t session, neui_widget_t grid,
                                       int col, const char* text);
  // Returns bytes needed including null terminator (call with buf=NULL to
  // query). Copies up to buflen bytes including null if buf is non-NULL.
  // Returns -1 on error.
  int  (NEUI_ABI *get_column_header) (neui_session_t session, neui_widget_t grid,
                                       int col, char* buf, int buflen);

  void (NEUI_ABI *remove_column)     (neui_session_t session, neui_widget_t grid,
                                       int col);
  void (NEUI_ABI *clear_columns)     (neui_session_t session, neui_widget_t grid);

  // -------- Rows ---------------------------------------------------------

  // Append a row. `values` is a NULL-terminated array of UTF-8 strings,
  // one per column from column 0 onwards; missing trailing entries are
  // treated as empty strings. Excess entries past the last column are
  // ignored. Returns the new row index, or -1 on bad widget.
  int  (NEUI_ABI *add_row)           (neui_session_t session, neui_widget_t grid,
                                       const char* const* values_utf8);

  int  (NEUI_ABI *get_row_count)     (neui_session_t session, neui_widget_t grid);

  void (NEUI_ABI *remove_row)        (neui_session_t session, neui_widget_t grid,
                                       int row);
  void (NEUI_ABI *clear_rows)        (neui_session_t session, neui_widget_t grid);

  // -------- Cells --------------------------------------------------------

  void        (NEUI_ABI *set_cell_text)(neui_session_t session, neui_widget_t grid,
                                         int row, int col, const char* utf8);
  // Returns bytes needed including null terminator (call with buf=NULL to
  // query). Returns -1 on error.
  int         (NEUI_ABI *get_cell_text)(neui_session_t session, neui_widget_t grid,
                                         int row, int col, char* buf, int buflen);

  // Per-cell text colour override (0xAARRGGBB). Pass 0 to clear.
  void (NEUI_ABI *set_cell_color)    (neui_session_t session, neui_widget_t grid,
                                       int row, int col, uint32_t argb);

  // Per-cell enabled override. A disabled cell paints dimmed and does not
  // fire GRID_CELL_CLICKED for that cell (the click still selects the row).
  void (NEUI_ABI *set_cell_enabled)  (neui_session_t session, neui_widget_t grid,
                                       int row, int col, bool enabled);

  // Remove all per-cell overrides at (row, col) - reverts to row/column
  // defaults. Does NOT clear the cell's text.
  void (NEUI_ABI *clear_cell_overrides)(neui_session_t session, neui_widget_t grid,
                                         int row, int col);

  // -------- Selection + scrolling ---------------------------------------

  // Row-focus selection. -1 = none. Always available; in cell-focus mode
  // this also moves the cell cursor to (row, 0).
  void (NEUI_ABI *set_selected_row)  (neui_session_t session, neui_widget_t grid,
                                       int row);
  int  (NEUI_ABI *get_selected_row)  (neui_session_t session, neui_widget_t grid);

  // Cell-focus selection. Meaningful when grid.cell_focus = 1; in
  // row-focus mode set_selected_cell only updates the row.
  // Pass row = -1 / col = -1 to clear.
  void (NEUI_ABI *set_selected_cell) (neui_session_t session, neui_widget_t grid,
                                       int row, int col);
  // out_col is set to -1 in row-focus mode. Either out pointer may be NULL.
  void (NEUI_ABI *get_selected_cell) (neui_session_t session, neui_widget_t grid,
                                       int* out_row, int* out_col);

  void (NEUI_ABI *ensure_row_visible) (neui_session_t session, neui_widget_t grid,
                                        int row);
  void (NEUI_ABI *ensure_cell_visible)(neui_session_t session, neui_widget_t grid,
                                        int row, int col);

  void (NEUI_ABI *set_scroll_x)      (neui_session_t session, neui_widget_t grid,
                                       int x_logical);
  int  (NEUI_ABI *get_scroll_x)      (neui_session_t session, neui_widget_t grid);

  // -------- Hit-test ----------------------------------------------------

  // Resolve a widget-local point to (row, col). Returns 1 if the point
  // lies inside a body cell (out_row and out_col populated), 0 otherwise
  // (header band, scrollbar gutter, empty area below the last row).
  // Either out pointer may be NULL. Row is LOGICAL (data-record index),
  // not the sort-order position - use logical_to_visual_row to translate.
  int (NEUI_ABI *hit_test)(neui_session_t session, neui_widget_t grid,
                            int local_x, int local_y,
                            int* out_row, int* out_col);

  // -------- Sorting -----------------------------------------------------
  // Multi-column sort with a per-column on / off switch and per-column
  // compare kind. The active sort is a stack of (col, dir) levels; level 0
  // is primary. User-driven header clicks fire GRID_SORT_CHANGED;
  // programmatic mutators below stay silent (mirrors COLUMN_RESIZED /
  // VALUE_CHANGED semantics).
  //
  // All click events (GRID_ROW_SELECTED / _CELL_SELECTED / _CELL_CLICKED /
  // _ROW_ACTIVATED) continue to deliver LOGICAL row indices - the same
  // ones set_cell_text / cell_overrides / set_selected_row use. Call
  // logical_to_visual_row(grid, row) to learn the row's position in the
  // current sort order.

  // Mark a column sortable / not-sortable. Default true. Non-sortable
  // columns ignore header clicks but still accept programmatic set_sort /
  // add_sort.
  void (NEUI_ABI *set_column_sortable)  (neui_session_t session, neui_widget_t grid,
                                          int col, bool sortable);

  // Per-column compare kind. Default NEUI_GRID_SORT_STRING.
  void (NEUI_ABI *set_column_sort_kind) (neui_session_t session, neui_widget_t grid,
                                          int col, neui_grid_sort_kind_t kind);

  // Replace the sort stack with a single level (col, dir). Pass dir =
  // NEUI_GRID_SORT_NONE to clear the stack entirely (equivalent to
  // clear_sort).
  void (NEUI_ABI *set_sort)             (neui_session_t session, neui_widget_t grid,
                                          int col, neui_grid_sort_dir_t dir);

  // Push or update a level on the sort stack (Shift+click path).
  //   - If `col` is already in the stack, that level's direction is set to
  //     `dir`. Passing NEUI_GRID_SORT_NONE removes that level.
  //   - Otherwise the new (col, dir) is appended. If the stack is already
  //     at NEUI_GRID_SORT_MAX_LEVELS, the oldest level is evicted first.
  //   - NEUI_GRID_SORT_NONE on a column not in the stack is a no-op.
  void (NEUI_ABI *add_sort)             (neui_session_t session, neui_widget_t grid,
                                          int col, neui_grid_sort_dir_t dir);

  void (NEUI_ABI *clear_sort)           (neui_session_t session, neui_widget_t grid);

  // Inspect the active sort stack. get_sort_count returns the number of
  // levels (0 = unsorted). get_sort_level fills out_col + out_dir for the
  // given level (0 = primary); out_col is set to -1 and out_dir to
  // NEUI_GRID_SORT_NONE on out-of-range.
  int  (NEUI_ABI *get_sort_count)       (neui_session_t session, neui_widget_t grid);
  void (NEUI_ABI *get_sort_level)       (neui_session_t session, neui_widget_t grid,
                                          int level,
                                          int* out_col,
                                          neui_grid_sort_dir_t* out_dir);

  // Translate between logical (data-identity, stable) and visual (current
  // sort position) row indices.
  //   logical_to_visual_row -> the row's POSITION IN THE CURRENT SORT
  //     ORDER (0 = topmost displayed row). Call on the row delivered by
  //     any GRID_*_CLICKED / _SELECTED event to learn where the user saw
  //     it on screen.
  //   visual_to_logical_row -> the data-record (logical) row at sort
  //     position v.
  // Both return -1 on out-of-range. When no sort is active the mapping is
  // identity. Both rebuild the sort if it is dirty, so they are safe to
  // call right after add_row / set_cell_text.
  int (NEUI_ABI *logical_to_visual_row) (neui_session_t session, neui_widget_t grid,
                                          int logical_row);
  int (NEUI_ABI *visual_to_logical_row) (neui_session_t session, neui_widget_t grid,
                                          int visual_row);
} neui_grid_api_t;

#ifdef __cplusplus
}
#endif
