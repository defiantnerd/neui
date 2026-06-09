// neui grid widget demo. Standalone example; kept separate from the
// monolithic neui_example so the grid feature can be exercised on its
// own (and the existing example doesn't grow further).
//
// GRID is now implemented in every host that ships in this build. Pick
// the native host first; fall back to crossplatform if the build is
// xpl-only (e.g. the null platform).

#include "neui/neui.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef _WIN32
#define GRID_HOST "neui.host.win32"
#elif defined(__APPLE__)
#define GRID_HOST "neui.host.macos"
#else
#define GRID_HOST "neui.host.crossplatform"
#endif

static void dbglog(const char* fmt, ...)
{
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
#ifdef _WIN32
  OutputDebugStringA(buf);
#else
  fputs(buf, stderr);
#endif
}

struct AppState {
  neui_api_t*        neui    = nullptr;
  neui_widget_api_t* widgets = nullptr;
  neui_grid_api_t*   grid    = nullptr;
  neui_attr_api_t*   attrs   = nullptr;
  neui_tree_api_t*   tree    = nullptr;
  neui_items_api_t*  items   = nullptr;
  neui_session_t     session = {0};

  uint32_t win_id        = 0;
  uint32_t grid_id       = 0;
  uint32_t status_label  = 0;
  uint32_t add_row_btn   = 0;
  uint32_t del_row_btn   = 0;
  uint32_t clear_btn     = 0;
  uint32_t toggle_btn    = 0;
  uint32_t scroll_chk    = 0;
  uint32_t focus_chk     = 0;
  uint32_t lines_chk     = 0;
  uint32_t rowcolor_combo = 0;
  uint32_t header_combo  = 0;

  int next_row_serial = 0;
  bool cell_focus     = false;
  bool show_focus_row = true;
};

// Demo colours for the grid-lines + row-background pickers. ARGB
// (0xAARRGGBB); kept translucent so the cell text stays readable on both
// light and dark themes.
static const uint32_t k_grid_lines_color = 0x40888888;   // subtle grey
static const uint32_t k_row_blue         = 0x33336699;   // translucent blue
static const uint32_t k_row_green        = 0x3340A040;   // translucent green
static const uint32_t k_header_grey      = 0x55888888;   // translucent grey
static const uint32_t k_header_red       = 0x55D86A6A;   // translucent light red

// Row-colour combo choices. Index maps to the apply logic in
// apply_row_colors_from_combo.
//   0 "none"        - no row backgrounds (body bg shows through)
//   1 "blue"        - row_bg_a only -> every row blue
//   2 "green & blue"- row_bg_a green + row_bg_b blue -> alternating zebra
static const char* k_rowcolor_choices[] = { "none", "blue", "green & blue" };
static const int   k_rowcolor_count = 3;

// Header-background combo choices. Index maps to apply_header_bg_from_combo.
//   0 "none"        - theme panel background (no override)
//   1 "grey"        - translucent grey tint
//   2 "lighter red" - translucent light-red tint
static const char* k_header_choices[] = { "none", "grey", "lighter red" };
static const int   k_header_count = 3;

// CHECKBOX3 state -> scroll mode. The tri-state checkbox cycles
// unchecked -> checked -> indeterminate on each click, which the user reads
// as off / on / platform-default.
static int scroll_mode_from_check(neui_check_state_t s)
{
  switch (s) {
    case NEUI_CHECK_CHECKED:       return NEUI_GRID_SCROLL_SMOOTH;
    case NEUI_CHECK_UNCHECKED:     return NEUI_GRID_SCROLL_STEPPED;
    default:                       return NEUI_GRID_SCROLL_PLATFORM;
  }
}

static const char* scroll_mode_label(int mode)
{
  switch (mode) {
    case NEUI_GRID_SCROLL_STEPPED: return "stepped";
    case NEUI_GRID_SCROLL_SMOOTH:  return "smooth";
    default:                       return "platform default";
  }
}

// Apply the row-colour combo selection to the grid's row-background attrs.
static void apply_row_colors_from_combo(AppState* app)
{
  if (!app->items || !app->attrs || !app->grid_id) return;
  uint32_t sel = app->items->get_selected(app->session, { app->rowcolor_combo });
  switch (sel) {
    case 1:  // blue: A only (uniform)
      app->attrs->set_int(app->session, { app->grid_id },
                          NEUI_ATTR_GRID_ROW_BG_A, (int)k_row_blue);
      app->attrs->remove (app->session, { app->grid_id },
                          NEUI_ATTR_GRID_ROW_BG_B);
      break;
    case 2:  // green & blue: A + B (alternating zebra)
      app->attrs->set_int(app->session, { app->grid_id },
                          NEUI_ATTR_GRID_ROW_BG_A, (int)k_row_green);
      app->attrs->set_int(app->session, { app->grid_id },
                          NEUI_ATTR_GRID_ROW_BG_B, (int)k_row_blue);
      break;
    default: // none: clear both
      app->attrs->remove(app->session, { app->grid_id }, NEUI_ATTR_GRID_ROW_BG_A);
      app->attrs->remove(app->session, { app->grid_id }, NEUI_ATTR_GRID_ROW_BG_B);
      break;
  }
  app->widgets->invalidate(app->session, { app->grid_id });
}

// Apply the header-background combo selection to the grid's header-bg attr.
static void apply_header_bg_from_combo(AppState* app)
{
  if (!app->items || !app->attrs || !app->grid_id) return;
  uint32_t sel = app->items->get_selected(app->session, { app->header_combo });
  switch (sel) {
    case 1:  // grey
      app->attrs->set_int(app->session, { app->grid_id },
                          NEUI_ATTR_GRID_HEADER_BG_COLOR, (int)k_header_grey);
      break;
    case 2:  // lighter red
      app->attrs->set_int(app->session, { app->grid_id },
                          NEUI_ATTR_GRID_HEADER_BG_COLOR, (int)k_header_red);
      break;
    default: // none: clear override -> theme panel bg
      app->attrs->remove(app->session, { app->grid_id },
                         NEUI_ATTR_GRID_HEADER_BG_COLOR);
      break;
  }
  app->widgets->invalidate(app->session, { app->grid_id });
}

static void set_status(AppState* app, const char* fmt, ...)
{
  if (!app->widgets || !app->status_label) return;
  char buf[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  app->widgets->set_text(app->session, { app->status_label }, buf);
}

static void append_row(AppState* app, int index)
{
  if (!app->grid || !app->grid_id) return;
  char c0[32], c1[64], c2[64], c3[32], c4[32], c5[64];
  snprintf(c0, sizeof(c0), "%d", index);
  snprintf(c1, sizeof(c1), "Item %d", index);
  static const char* sample_categories[] = {
    "Alpha", "Beta", "Gamma", "Delta", "Epsilon", "Zeta"
  };
  snprintf(c2, sizeof(c2), "%s", sample_categories[index % 6]);
  snprintf(c3, sizeof(c3), "%d", (index * 37 + 13) % 1000);
  snprintf(c4, sizeof(c4), "%.2f", (index % 200) * 0.5f);
  snprintf(c5, sizeof(c5), "Notes %d - lorem ipsum dolor", index);
  const char* values[] = { c0, c1, c2, c3, c4, c5, nullptr };
  app->grid->add_row(app->session, { app->grid_id }, values);
}

static void populate_initial_rows(AppState* app, int count)
{
  for (int i = 0; i < count; ++i) {
    append_row(app, app->next_row_serial++);
  }
}

static bool on_event(void* token, neui_event_t* ev)
{
  AppState* app = (AppState*)token;
  if (!app || !ev) return false;

  switch (ev->type) {
  case NEUI_EVENT_APP_QUIT:
    dbglog("[grid_example] APP_QUIT\n");
    return true;

  case NEUI_EVENT_TREE_ITEM_ACTIVATED: {
    // Menubar pick. The Quit item carries userdata (void*)1; end the
    // session to unwind the run loop on every host.
    void* ud = app->tree
      ? app->tree->get_userdata(app->session, ev->data.tree.widget, ev->data.tree.item)
      : nullptr;
    if (ud == (void*)1 && app->neui)
      app->neui->endsession(app->session);
    return true;
  }

  case NEUI_EVENT_GRID_ROW_SELECTED:
    set_status(app, "ROW_SELECTED row=%d", ev->data.grid_row.row);
    // Return false so the dispatch ladder continues to CELL_SELECTED / CELL_CLICKED.
    return false;

  case NEUI_EVENT_GRID_CELL_SELECTED:
    set_status(app, "CELL_SELECTED row=%d col=%d",
               ev->data.grid_cell.row, ev->data.grid_cell.col);
    return false;

  case NEUI_EVENT_GRID_CELL_CLICKED:
    set_status(app, "CELL_CLICKED row=%d col=%d (fallback)",
               ev->data.grid_cell.row, ev->data.grid_cell.col);
    return true;

  case NEUI_EVENT_GRID_ROW_ACTIVATED:
    set_status(app, "ROW_ACTIVATED row=%d", ev->data.grid_row.row);
    return true;

  case NEUI_EVENT_GRID_COLUMN_RESIZED:
    set_status(app, "COLUMN_RESIZED col=%d %d -> %d",
               ev->data.grid_column_resize.col,
               ev->data.grid_column_resize.old_width,
               ev->data.grid_column_resize.new_width);
    return true;

  case NEUI_EVENT_GRID_SORT_CHANGED: {
    int n = app->grid->get_sort_count(app->session, { app->grid_id });
    const char* dirname = "?";
    switch (ev->data.grid_sort.dir) {
      case NEUI_GRID_SORT_ASC:  dirname = "asc";   break;
      case NEUI_GRID_SORT_DESC: dirname = "desc";  break;
      case NEUI_GRID_SORT_NONE: dirname = "none";  break;
    }
    set_status(app, "SORT_CHANGED col=%d dir=%s (%d level%s active)",
               ev->data.grid_sort.col, dirname, n, n == 1 ? "" : "s");
    return true;
  }

  case NEUI_EVENT_GRID_CELL_EDIT_BEGIN:
    set_status(app, "EDIT_BEGIN row=%d col=%d (Enter to commit, Esc to cancel)",
               ev->data.grid_cell.row, ev->data.grid_cell.col);
    return true;

  case NEUI_EVENT_GRID_CELL_CHANGED: {
    char buf[256];
    (void)app->grid->get_cell_text(app->session, { app->grid_id },
                                     ev->data.grid_cell.row,
                                     ev->data.grid_cell.col,
                                     buf, (int)sizeof(buf));
    set_status(app, "CELL_CHANGED row=%d col=%d -> \"%s\"",
               ev->data.grid_cell.row, ev->data.grid_cell.col, buf);
    return true;
  }

  case NEUI_EVENT_GRID_CELL_EDIT_CANCEL:
    set_status(app, "EDIT_CANCEL row=%d col=%d",
               ev->data.grid_cell.row, ev->data.grid_cell.col);
    return true;

  case NEUI_EVENT_MOUSE_BUTTON_CLICK: {
    uint32_t w = ev->data.mouse.widget.id;
    if (w == app->add_row_btn) {
      append_row(app, app->next_row_serial++);
      set_status(app, "Added row %d", app->next_row_serial - 1);
      return true;
    }
    if (w == app->del_row_btn) {
      int sel = app->grid->get_selected_row(app->session, { app->grid_id });
      if (sel >= 0) {
        app->grid->remove_row(app->session, { app->grid_id }, sel);
        set_status(app, "Removed row %d", sel);
      } else {
        set_status(app, "No row selected");
      }
      return true;
    }
    if (w == app->clear_btn) {
      app->grid->clear_rows(app->session, { app->grid_id });
      app->next_row_serial = 0;
      set_status(app, "Cleared all rows");
      return true;
    }
    if (w == app->toggle_btn) {
      app->cell_focus = !app->cell_focus;
      app->attrs->set_int(app->session, { app->grid_id },
                            NEUI_ATTR_GRID_CELL_FOCUS, app->cell_focus ? 1 : 0);
      app->widgets->invalidate(app->session, { app->grid_id });
      set_status(app, "Focus mode: %s",
                 app->cell_focus ? "cell" : "row");
      return true;
    }
    return false;
  }

  case NEUI_EVENT_CHECKBOX_CHANGED: {
    uint32_t w = ev->data.checkbox.widget.id;
    if (w == app->focus_chk) {
      app->show_focus_row = (ev->data.checkbox.state == NEUI_CHECK_CHECKED);
      app->attrs->set_int(app->session, { app->grid_id },
                            NEUI_ATTR_GRID_SHOW_FOCUS_ROW,
                            app->show_focus_row ? 1 : 0);
      app->widgets->invalidate(app->session, { app->grid_id });
      set_status(app, "show_focus_row = %d", app->show_focus_row ? 1 : 0);
      return true;
    }
    if (w == app->scroll_chk) {
      int mode = scroll_mode_from_check(ev->data.checkbox.state);
      app->attrs->set_int(app->session, { app->grid_id },
                            NEUI_ATTR_GRID_SCROLL_MODE, mode);
      app->widgets->invalidate(app->session, { app->grid_id });
      set_status(app, "Scroll: %s", scroll_mode_label(mode));
      return true;
    }
    if (w == app->lines_chk) {
      bool on = (ev->data.checkbox.state == NEUI_CHECK_CHECKED);
      if (on)
        app->attrs->set_int(app->session, { app->grid_id },
                            NEUI_ATTR_GRID_LINES_COLOR, (int)k_grid_lines_color);
      else
        app->attrs->remove(app->session, { app->grid_id },
                           NEUI_ATTR_GRID_LINES_COLOR);
      app->widgets->invalidate(app->session, { app->grid_id });
      set_status(app, "Grid lines: %s", on ? "on" : "off");
      return true;
    }
    return false;
  }

  case NEUI_EVENT_ITEM_SELECTED: {
    if (ev->data.item.widget.id == app->rowcolor_combo) {
      apply_row_colors_from_combo(app);
      uint32_t sel = app->items->get_selected(app->session,
                                               { app->rowcolor_combo });
      set_status(app, "Row colors: %s",
                 sel < (uint32_t)k_rowcolor_count ? k_rowcolor_choices[sel] : "?");
      return true;
    }
    if (ev->data.item.widget.id == app->header_combo) {
      apply_header_bg_from_combo(app);
      uint32_t sel = app->items->get_selected(app->session,
                                               { app->header_combo });
      set_status(app, "Header bg: %s",
                 sel < (uint32_t)k_header_count ? k_header_choices[sel] : "?");
      return true;
    }
    return false;
  }

  default:
    return false;
  }
}

static void on_destroy(void*, neui_widget_t, void*) {}

static neui_widget_client_t widget_client = {
  NEUI_VERSION,
  on_destroy,
  on_event,
};

// NEUI_API_GRID_CLIENT - validate-on-commit hook. Rejects empty strings as a
// trivial demonstration; a real client would also do format / range / db
// uniqueness checks. Returning false leaves the editor open so the user can
// fix the value.
static bool NEUI_ABI validate_cell(void* /*token*/, neui_widget_t /*grid*/,
                                     int /*row*/, int /*col*/,
                                     const char* new_text)
{
  if (!new_text || new_text[0] == '\0') return false;
  return true;
}

static neui_grid_client_t grid_client = {
  NEUI_VERSION,
  validate_cell,
};

static void* host_get_iface(void* /*token*/, const char* iface)
{
  if (!strcmp(iface, NEUI_API_WIDGETS))     return &widget_client;
  if (!strcmp(iface, NEUI_API_GRID_CLIENT)) return &grid_client;
  return nullptr;
}

static neui_client_t host_client = {
  NEUI_VERSION,
  host_get_iface,
};

// Win32 subsystem links to mainCRTStartup -> main(), same as the other
// neui examples; no separate wWinMain needed.
int main(int /*argc*/, char** /*argv*/)
{
  neui_init();
  AppState app;
  app.neui = neui_get_api(GRID_HOST);
  if (!app.neui) {
    dbglog("[grid_example] failed to acquire host %s\n", GRID_HOST);
    return 1;
  }

  app.session = app.neui->create_session(&host_client, &app);
  app.widgets = (neui_widget_api_t*)app.neui->get_interface(app.session, NEUI_API_WIDGETS);
  app.grid    = (neui_grid_api_t*)  app.neui->get_interface(app.session, NEUI_API_GRID);
  app.attrs   = (neui_attr_api_t*)  app.neui->get_interface(app.session, NEUI_API_ATTRS);
  app.tree    = (neui_tree_api_t*)  app.neui->get_interface(app.session, NEUI_API_TREE);
  app.items   = (neui_items_api_t*) app.neui->get_interface(app.session, NEUI_API_ITEMS);
  if (!app.widgets || !app.grid || !app.attrs || !app.tree || !app.items) {
    dbglog("[grid_example] missing required interface(s)\n");
    return 1;
  }

  // Main window.
  auto win = app.widgets->create(app.session, widget_none, NEUI_W_APPWINDOW,
                                  100, 100, 900, 600, &app);
  app.win_id = win.id;
  app.widgets->set_text(app.session, win, "neui grid example");
  app.attrs->set_int(app.session, win, NEUI_ATTR_MIN_WIDTH,  500);
  app.attrs->set_int(app.session, win, NEUI_ATTR_MIN_HEIGHT, 360);
  app.attrs->set_int(app.session, win, NEUI_ATTR_FOLLOW_SYSTEM_THEME, 1);

  // Menubar with a File > Quit item. Host-agnostic: win32 builds an HMENU,
  // macOS a system NSMenu, xpl a painted menubar. Quit carries userdata
  // (void*)1 and is handled in on_event via TREE_ITEM_ACTIVATED.
  auto menubar = app.widgets->create(app.session, win, NEUI_W_MENUBAR,
                                      0, 0, 0, 0, &app);
  auto file_menu = app.tree->add(app.session, menubar, tree_item_root, "File", nullptr);
  auto quit_item = app.tree->add(app.session, menubar, file_menu, "Quit", (void*)1);
  app.tree->set_shortcut(app.session, menubar, quit_item, NEUI_KMOD_CTRL, NEUI_KEY_Q);

  // Toolbar laid out in two rows. Every control is vertically centred
  // within a fixed-height row "band" so buttons (28 px), comboboxes /
  // labels (22 px collapsed bar) and checkboxes (22 px) line up on a
  // common centreline instead of sitting at staggered tops.
  const int kBandH   = 28;          // toolbar row height
  const int kBtnH    = 28;
  const int kCtrlH   = 22;          // checkbox / label / combo collapsed bar
  const int kRow1Y   = 10;
  const int kRow2Y   = kRow1Y + kBandH + 8;   // 8 px gap between rows
  // Centre a control of height h within a band of height kBandH at band_top.
  auto centred = [&](int band_top, int h) { return band_top + (kBandH - h) / 2; };

  // ---- Row 1: action buttons + state checkboxes ----
  int x = 10;
  auto mk_button = [&](const char* label, int w) {
    auto b = app.widgets->create(app.session, win, NEUI_W_BUTTON,
                                  x, centred(kRow1Y, kBtnH), w, kBtnH, &app);
    app.widgets->set_text(app.session, b, label);
    x += w + 6;
    return b.id;
  };
  app.add_row_btn = mk_button("Add row", 90);
  app.del_row_btn = mk_button("Remove selected", 140);
  app.clear_btn   = mk_button("Clear", 70);
  app.toggle_btn  = mk_button("Toggle row/cell focus", 180);

  auto chk = app.widgets->create(app.session, win, NEUI_W_CHECKBOX,
                                  x, centred(kRow1Y, kCtrlH), 160, kCtrlH, &app);
  app.widgets->set_text(app.session, chk, "show_focus_row");
  app.widgets->set_check(app.session, chk, NEUI_CHECK_CHECKED);
  app.focus_chk = chk.id;
  x += 160 + 6;

  // Tri-state scroll-mode picker. Unchecked = stepped, checked = smooth,
  // indeterminate = platform default (Win32 = stepped, macOS = smooth).
  auto schk = app.widgets->create(app.session, win, NEUI_W_CHECKBOX3,
                                    x, centred(kRow1Y, kCtrlH), 180, kCtrlH, &app);
  app.widgets->set_text(app.session, schk, "smooth scroll");
  app.widgets->set_check(app.session, schk, NEUI_CHECK_INDETERMINATE);
  app.scroll_chk = schk.id;

  // ---- Row 2: grid-lines switch + colour pickers ----
  const int kRow2Ctrl = centred(kRow2Y, kCtrlH);
  int x2 = 10;
  auto lchk = app.widgets->create(app.session, win, NEUI_W_CHECKBOX,
                                   x2, kRow2Ctrl, 110, kCtrlH, &app);
  app.widgets->set_text(app.session, lchk, "grid lines");
  app.widgets->set_check(app.session, lchk, NEUI_CHECK_UNCHECKED);
  app.lines_chk = lchk.id;
  x2 += 110 + 12;

  auto rc_lbl = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                     x2, kRow2Ctrl, 80, kCtrlH, &app);
  app.widgets->set_text(app.session, rc_lbl, "Row colors:");
  x2 += 80 + 4;

  // ComboBox height = collapsed-bar (22) + drop capacity (3 * 18) so all
  // three choices fit when open.
  auto rc_combo = app.widgets->create(app.session, win, NEUI_W_COMBOBOX,
                                       x2, kRow2Ctrl, 130, 76, &app);
  app.rowcolor_combo = rc_combo.id;
  for (int i = 0; i < k_rowcolor_count; ++i)
    app.items->add(app.session, rc_combo, k_rowcolor_choices[i], nullptr);
  app.items->set_selected(app.session, rc_combo, 0);  // "none"
  x2 += 130 + 12;

  auto hdr_lbl = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                      x2, kRow2Ctrl, 60, kCtrlH, &app);
  app.widgets->set_text(app.session, hdr_lbl, "Header:");
  x2 += 60 + 4;

  auto hdr_combo = app.widgets->create(app.session, win, NEUI_W_COMBOBOX,
                                        x2, kRow2Ctrl, 130, 76, &app);
  app.header_combo = hdr_combo.id;
  for (int i = 0; i < k_header_count; ++i)
    app.items->add(app.session, hdr_combo, k_header_choices[i], nullptr);
  app.items->set_selected(app.session, hdr_combo, 0);  // "none"

  // Grid takes the bulk of the window (pushed down for the 2nd toolbar row).
  auto g = app.widgets->create(app.session, win, NEUI_W_GRID,
                                10, 82, 880, 468, &app);
  app.grid_id = g.id;
  app.attrs->set_int(app.session, g, NEUI_ATTR_GRID_ROW_HEIGHT,      22);
  app.attrs->set_int(app.session, g, NEUI_ATTR_GRID_HEADER_HEIGHT,   24);
  app.attrs->set_int(app.session, g, NEUI_ATTR_GRID_SHOW_FOCUS_ROW,  1);
  // Cell-focus on so the in-place editor demo is reachable via Enter.
  app.attrs->set_int(app.session, g, NEUI_ATTR_GRID_CELL_FOCUS,      1);
  app.cell_focus = true;

  // Columns.
  app.grid->add_column(app.session, g, "#",        50);
  app.grid->add_column(app.session, g, "Name",     160);
  app.grid->add_column(app.session, g, "Category", 110);
  app.grid->add_column(app.session, g, "Code",     90);
  app.grid->add_column(app.session, g, "Value",    90);
  app.grid->add_column(app.session, g, "Notes",    260);
  app.grid->set_column_align(app.session, g, 0, "right");
  app.grid->set_column_align(app.session, g, 3, "right");
  app.grid->set_column_align(app.session, g, 4, "right");

  // Mark Name and Notes editable. In cell-focus mode (above), pressing
  // Enter on a cell in an editable column opens the in-place editor; the
  // grid_client.validate_cell callback rejects empty strings.
  app.grid->set_column_editable(app.session, g, 1, true);
  app.grid->set_column_editable(app.session, g, 5, true);

  // Sort kinds: numeric columns sort numerically so "9" < "10" (the default
  // STRING kind would give the lexicographic surprise). Click a header to
  // toggle asc / desc / none; Shift+click to stack a secondary level.
  app.grid->set_column_sort_kind(app.session, g, 0, NEUI_GRID_SORT_INT);
  app.grid->set_column_sort_kind(app.session, g, 3, NEUI_GRID_SORT_INT);
  app.grid->set_column_sort_kind(app.session, g, 4, NEUI_GRID_SORT_FLOAT);
  // Name column uses natural ordering so "Item 2" < "Item 10".
  app.grid->set_column_sort_kind(app.session, g, 1, NEUI_GRID_SORT_NATURAL);

  populate_initial_rows(&app, 500);

  // A disabled cell to demo per-cell enabled overrides.
  app.grid->set_cell_enabled(app.session, g, 5, 2, false);
  app.grid->set_cell_color  (app.session, g, 5, 2, 0xFFAA4444);

  // Status label at the bottom (resize-naive placement to keep the
  // example dead simple; v1 ships without auto layout).
  auto sl = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                  10, 558, 880, 22, &app);
  app.widgets->set_text(app.session, sl,
    "Ready - click a Name/Notes cell + press Enter to edit (Esc cancels)");
  app.status_label = sl.id;

  app.widgets->show(app.session, win);
  app.neui->run(app.session);
  app.neui->destroy(app.session);
  return 0;
}
