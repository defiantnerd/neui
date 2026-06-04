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
#define GRID_HOST "neui.host.crossplatform"
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
  neui_session_t     session = {0};

  uint32_t win_id        = 0;
  uint32_t grid_id       = 0;
  uint32_t status_label  = 0;
  uint32_t add_row_btn   = 0;
  uint32_t del_row_btn   = 0;
  uint32_t clear_btn     = 0;
  uint32_t toggle_btn    = 0;
  uint32_t focus_chk     = 0;

  int next_row_serial = 0;
  bool cell_focus     = false;
  bool show_focus_row = true;
};

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

static void* host_get_iface(void* /*token*/, const char* iface)
{
  if (!strcmp(iface, NEUI_API_WIDGETS)) return &widget_client;
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
  if (!app.widgets || !app.grid || !app.attrs) {
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

  // Toolbar of buttons + checkbox along the top.
  int x = 10, y = 10;
  auto mk_button = [&](const char* label, int w) {
    auto b = app.widgets->create(app.session, win, NEUI_W_BUTTON, x, y, w, 28, &app);
    app.widgets->set_text(app.session, b, label);
    x += w + 6;
    return b.id;
  };
  app.add_row_btn = mk_button("Add row", 90);
  app.del_row_btn = mk_button("Remove selected", 140);
  app.clear_btn   = mk_button("Clear", 70);
  app.toggle_btn  = mk_button("Toggle row/cell focus", 180);

  auto chk = app.widgets->create(app.session, win, NEUI_W_CHECKBOX,
                                  x, y + 5, 160, 22, &app);
  app.widgets->set_text(app.session, chk, "show_focus_row");
  app.widgets->set_check(app.session, chk, NEUI_CHECK_CHECKED);
  app.focus_chk = chk.id;

  // Grid takes the bulk of the window.
  auto g = app.widgets->create(app.session, win, NEUI_W_GRID,
                                10, 50, 880, 500, &app);
  app.grid_id = g.id;
  app.attrs->set_int(app.session, g, NEUI_ATTR_GRID_ROW_HEIGHT,      22);
  app.attrs->set_int(app.session, g, NEUI_ATTR_GRID_HEADER_HEIGHT,   24);
  app.attrs->set_int(app.session, g, NEUI_ATTR_GRID_SHOW_FOCUS_ROW,  1);
  app.attrs->set_int(app.session, g, NEUI_ATTR_GRID_CELL_FOCUS,      0);

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

  populate_initial_rows(&app, 500);

  // A disabled cell to demo per-cell enabled overrides.
  app.grid->set_cell_enabled(app.session, g, 5, 2, false);
  app.grid->set_cell_color  (app.session, g, 5, 2, 0xFFAA4444);

  // Status label at the bottom (resize-naive placement to keep the
  // example dead simple; v1 ships without auto layout).
  auto sl = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                  10, 558, 880, 22, &app);
  app.widgets->set_text(app.session, sl, "Ready - click rows, drag column dividers, scroll");
  app.status_label = sl.id;

  app.widgets->show(app.session, win);
  app.neui->run(app.session);
  app.neui->destroy(app.session);
  return 0;
}
