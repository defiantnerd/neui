// tabview_example - demonstrates the NEUI_W_TABVIEW / NEUI_W_TABPAGE widgets:
//
//   * Each tab is a NEUI_W_TABPAGE container the client creates as a child of
//     the tabview; the page's text is the chip label, and content widgets are
//     parented INTO the page.
//   * A COMBOBOX cycles NEUI_ATTR_TAB_POSITION through all 13 documented
//     positions (4 edges x 3 alignments + "none") live.
//   * The whole widget carries a background (NEUI_ATTR_BACKGROUND) and a
//     tab-outline border (NEUI_ATTR_TAB_BORDER_COLOR).
//   * One page opts into vertical scrolling (NEUI_ATTR_SCROLL_MODE) with a
//     tall content stack, reusing the SECTION scroll machinery.
//   * One page sets per-chip background + text colours
//     (NEUI_ATTR_TAB_CHIP_BG_COLOR / _TEXT_COLOR).
//   * A status LABEL reports NEUI_EVENT_TAB_DESELECTED / _SELECTED, and the
//     TAB_SELECTED handler updates a label INSIDE the incoming page to prove
//     the event fires before the page is shown.
//
// Picks the default host (native on Win32 + macOS, xpl on Linux). Set
// NEUI_FORCE_XPL_HOST=1 in the environment to pin to the crossplatform host.

#include "neui/neui.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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
#endif
  fputs(buf, stderr);
}

// The 13 documented NEUI_ATTR_TAB_POSITION values, in the COMBOBOX order.
static const char* kPositions[] = {
  "top-left", "top-center", "top-right",
  "bottom-left", "bottom-center", "bottom-right",
  "left-top", "left-center", "left-bottom",
  "right-top", "right-center", "right-bottom",
  "none",
};
static const int kPositionCount = (int)(sizeof(kPositions) / sizeof(kPositions[0]));

struct AppState {
  neui_api_t*        neui    = nullptr;
  neui_widget_api_t* widgets = nullptr;
  neui_attr_api_t*   attrs   = nullptr;
  neui_items_api_t*  items   = nullptr;
  neui_tabs_api_t*   tabs    = nullptr;
  neui_tree_api_t*   tree    = nullptr;
  neui_session_t     session = { 0 };

  uint32_t win_id       = 0;
  uint32_t tabview_id   = 0;
  uint32_t pos_combo    = 0;
  uint32_t status_label = 0;
  uint32_t strip_chk    = 0;   // toggle NEUI_ATTR_TAB_STRIP_BG_COLOR (area beside chips)
  uint32_t border_chk   = 0;   // toggle NEUI_ATTR_TAB_BORDER_COLOR (content + chips outline)
  uint32_t round_chk    = 0;   // toggle NEUI_ATTR_TAB_CHIP_RADIUS

  // The label inside the "About" page we rewrite on each TAB_SELECTED.
  uint32_t about_label  = 0;
  int      view_counts[8] = {0};   // per-tab "viewed N times" counter
  std::string status_text;
};

static void set_status(AppState* a, const char* msg)
{
  if (!a->status_label) return;
  a->status_text = msg;
  a->widgets->set_text(a->session, neui_widget_t{ a->status_label }, a->status_text.c_str());
}

static bool NEUI_ABI on_event(void* token, neui_event_t* event)
{
  AppState* a = static_cast<AppState*>(token);
  switch (event->type) {
    case NEUI_EVENT_APP_QUIT:
      return false;

    case NEUI_EVENT_TREE_ITEM_ACTIVATED: {
      void* ud = a->tree ? a->tree->get_userdata(a->session, event->data.tree.widget,
                                                 event->data.tree.item)
                         : nullptr;
      if (ud == (void*)1 && a->neui)   // File > Quit
        a->neui->endsession(a->session);
      return true;
    }

    case NEUI_EVENT_TAB_DESELECTED: {
      char buf[128];
      snprintf(buf, sizeof(buf), "tab %u deselected", event->data.tab.tab_index);
      dbglog("[tabview] %s\n", buf);
      return true;
    }

    case NEUI_EVENT_TAB_SELECTED: {
      uint32_t idx = event->data.tab.tab_index;
      a->view_counts[idx % 8]++;
      char buf[160];
      snprintf(buf, sizeof(buf), "tab %u selected  (viewed %d time%s)",
               idx, a->view_counts[idx % 8],
               a->view_counts[idx % 8] == 1 ? "" : "s");
      set_status(a, buf);
      // When the About tab (index 3) becomes active, rewrite its label
      // BEFORE the page is shown - proves the event fires pre-repaint, so the
      // user never sees the stale text.
      if (idx == 3 && a->about_label) {
        char ab[128];
        snprintf(ab, sizeof(ab), "About page viewed %d time(s)", a->view_counts[3]);
        a->widgets->set_text(a->session, neui_widget_t{ a->about_label }, ab);
      }
      return true;
    }

    case NEUI_EVENT_CHECKBOX_CHANGED: {
      bool on = (event->data.checkbox.state == NEUI_CHECK_CHECKED);
      uint32_t w = event->data.checkbox.widget.id;
      neui_widget_t tv{ a->tabview_id };
      if (w == a->strip_chk) {
        // Fill the strip area beside the chips, or spare it (remove the attr).
        if (on) a->attrs->set_int(a->session, tv, NEUI_ATTR_TAB_STRIP_BG_COLOR, (int)0xFF14304A);
        else    a->attrs->remove (a->session, tv, NEUI_ATTR_TAB_STRIP_BG_COLOR);
      } else if (w == a->border_chk) {
        // Content + chips outline (follows the tab outline, not a full rect).
        if (on) a->attrs->set_int(a->session, tv, NEUI_ATTR_TAB_BORDER_COLOR, (int)0xFF5A6472);
        else    a->attrs->remove (a->session, tv, NEUI_ATTR_TAB_BORDER_COLOR);
      } else if (w == a->round_chk) {
        a->attrs->set_int(a->session, tv, NEUI_ATTR_TAB_CHIP_RADIUS, on ? 6 : 0);
      } else {
        break;
      }
      a->widgets->invalidate(a->session, tv);
      return true;
    }

    case NEUI_EVENT_ITEM_SELECTED: {
      if (event->data.item.widget.id == a->pos_combo) {
        uint32_t sel = a->items->get_selected(a->session, neui_widget_t{ a->pos_combo });
        if (sel < (uint32_t)kPositionCount) {
          a->attrs->set_string(a->session, neui_widget_t{ a->tabview_id },
                               NEUI_ATTR_TAB_POSITION, kPositions[sel]);
          a->widgets->invalidate(a->session, neui_widget_t{ a->tabview_id });
          char buf[96];
          snprintf(buf, sizeof(buf), "position: %s", kPositions[sel]);
          set_status(a, buf);
        }
        return true;
      }
      break;
    }
    default: break;
  }
  return false;
}

static void* NEUI_ABI get_interface(void* /*token*/, const char* iface)
{
  static neui_widget_client_t widget_client;
  if (!strcmp(iface, NEUI_API_WIDGETS)) {
    widget_client.neui_version = NEUI_VERSION;
    widget_client.ondestroy    = nullptr;
    widget_client.onevent      = on_event;
    return &widget_client;
  }
  return nullptr;
}

// Helper: make a TABPAGE child of the tabview with a chip label.
static neui_widget_t make_page(AppState* a, const char* label)
{
  neui_widget_t page = a->widgets->create(a->session, neui_widget_t{ a->tabview_id },
                                          NEUI_W_TABPAGE, 0, 0, 0, 0, nullptr);
  a->widgets->set_text(a->session, page, label);
  return page;
}

int main(int /*argc*/, char* /*argv*/[])
{
  neui_init();

  const char* host_id = nullptr;
  const char* force = getenv("NEUI_FORCE_XPL_HOST");
  if (force && force[0] == '1') host_id = "neui.host.crossplatform";

  neui_api_t* host = neui_get_api(host_id);
  if (!host) host = neui_get_api(nullptr);
  if (!host) { dbglog("[tabview_example] no host\n"); return 1; }

  AppState app;
  app.neui = host;

  neui_client_t client;
  client.neui_version  = NEUI_VERSION;
  client.get_interface = get_interface;
  app.session = host->create_session(&client, &app);
  if (!app.session.session) { dbglog("[tabview_example] no session\n"); return 1; }

  app.widgets = (neui_widget_api_t*)host->get_interface(app.session, NEUI_API_WIDGETS);
  app.attrs   = (neui_attr_api_t*)  host->get_interface(app.session, NEUI_API_ATTRS);
  app.items   = (neui_items_api_t*) host->get_interface(app.session, NEUI_API_ITEMS);
  app.tabs    = (neui_tabs_api_t*)  host->get_interface(app.session, NEUI_API_TABS);
  app.tree    = (neui_tree_api_t*)  host->get_interface(app.session, NEUI_API_TREE);
  if (!app.widgets || !app.attrs || !app.items) {
    dbglog("[tabview_example] missing API\n"); return 1;
  }
  if (!app.tabs) dbglog("[tabview_example] NEUI_API_TABS unavailable\n");

  neui_widget_t win = app.widgets->create(app.session, neui_widget_t{ UINT32_MAX },
                                          NEUI_W_APPWINDOW, 120, 120, 720, 480, nullptr);
  app.widgets->set_text(app.session, win, "neui tabbed view");
  app.win_id = win.id;

  // Menu bar with a File > Quit item (Ctrl/Cmd+Q).
  if (app.tree) {
    neui_widget_t mb = app.widgets->create(app.session, win, NEUI_W_MENUBAR,
                                           0, 0, 0, 0, nullptr);
    neui_item_t file = app.tree->add(app.session, mb, tree_item_root, "File", nullptr);
    neui_item_t quit = app.tree->add(app.session, mb, file, "Quit", (void*)1);
    app.tree->set_shortcut(app.session, mb, quit, NEUI_KMOD_CTRL, NEUI_KEY_Q);
  }

  // Position selector (top strip) + status label (bottom).
  neui_widget_t poslbl = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                             12, 12, 60, 22, nullptr);
  app.widgets->set_text(app.session, poslbl, "Position:");
  neui_widget_t combo = app.widgets->create(app.session, win, NEUI_W_COMBOBOX,
                                            78, 10, 150, 26, nullptr);
  app.pos_combo = combo.id;
  for (int i = 0; i < kPositionCount; ++i)
    app.items->add(app.session, combo, kPositions[i], nullptr);
  app.items->set_selected(app.session, combo, 0);

  // Live toggles for the new style options.
  neui_widget_t strip_chk = app.widgets->create(app.session, win, NEUI_W_CHECKBOX,
                                                240, 12, 150, 22, nullptr);
  app.widgets->set_text(app.session, strip_chk, "Fill strip area");
  app.strip_chk = strip_chk.id;
  neui_widget_t border_chk = app.widgets->create(app.session, win, NEUI_W_CHECKBOX,
                                                 400, 12, 110, 22, nullptr);
  app.widgets->set_text(app.session, border_chk, "Border");
  app.border_chk = border_chk.id;
  neui_widget_t round_chk = app.widgets->create(app.session, win, NEUI_W_CHECKBOX,
                                                520, 12, 130, 22, nullptr);
  app.widgets->set_text(app.session, round_chk, "Round chips");
  app.round_chk = round_chk.id;

  neui_widget_t status = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                             12, 450, 680, 22, nullptr);
  app.status_label = status.id;

  // The tabbed view, with a whole-area background + tab-outline border.
  neui_widget_t tv = app.widgets->create(app.session, win, NEUI_W_TABVIEW,
                                         12, 48, 696, 392, nullptr);
  app.tabview_id = tv.id;
  app.attrs->set_string(app.session, tv, NEUI_ATTR_TAB_POSITION, "top-left");
  // Note: NEUI_ATTR_BACKGROUND on a TABVIEW colours the CONTENT body only -
  // the strip area beside the chips stays transparent unless the "Fill strip
  // area" toggle sets NEUI_ATTR_TAB_STRIP_BG_COLOR. (Left unset here so the
  // body uses the default panel shade and the strip is transparent.)
  app.attrs->set_int(app.session, tv, NEUI_ATTR_TAB_BORDER_COLOR, (int)0xFF5A6472);
  app.attrs->set_int(app.session, tv, NEUI_ATTR_TAB_CHIP_RADIUS, 6);
  // No explicit NEUI_ATTR_TAB_STRIP_SIZE: top/bottom use the default band
  // height, and left/right strips auto-fit the widest chip label so the
  // text stays readable.

  // Reflect the initial tabview state in the toggle checkboxes (border +
  // rounded chips on; strip-area fill off / spared).
  app.widgets->set_check(app.session, border_chk, NEUI_CHECK_CHECKED);
  app.widgets->set_check(app.session, round_chk,  NEUI_CHECK_CHECKED);
  app.widgets->set_check(app.session, strip_chk,  NEUI_CHECK_UNCHECKED);

  // --- Page 1: General -----------------------------------------------------
  {
    neui_widget_t p = make_page(&app, "General");
    neui_widget_t l = app.widgets->create(app.session, p, NEUI_W_LABEL, 16, 16, 400, 22, nullptr);
    app.widgets->set_text(app.session, l, "General settings live here.");
    neui_widget_t b = app.widgets->create(app.session, p, NEUI_W_BUTTON, 16, 50, 120, 28, nullptr);
    app.widgets->set_text(app.session, b, "A button");
    neui_widget_t c = app.widgets->create(app.session, p, NEUI_W_CHECKBOX, 16, 90, 200, 24, nullptr);
    app.widgets->set_text(app.session, c, "Enable widgets");
  }

  // --- Page 2: Display (vertical scroll, tall content) ---------------------
  {
    neui_widget_t p = make_page(&app, "Display");
    app.attrs->set_string(app.session, p, NEUI_ATTR_SCROLL_MODE, "vertical");
    for (int i = 0; i < 25; ++i) {
      neui_widget_t l = app.widgets->create(app.session, p, NEUI_W_LABEL,
                                            16, 12 + i * 30, 300, 22, nullptr);
      char buf[48]; snprintf(buf, sizeof(buf), "Display option row %d", i + 1);
      app.widgets->set_text(app.session, l, buf);
    }
  }

  // --- Page 3: Audio (per-chip colours) ------------------------------------
  {
    neui_widget_t p = make_page(&app, "Audio");
    app.attrs->set_int(app.session, p, NEUI_ATTR_TAB_CHIP_BG_COLOR,   (int)0xFFD4AF37); // gold
    app.attrs->set_int(app.session, p, NEUI_ATTR_TAB_CHIP_TEXT_COLOR, (int)0xFF332B00); // dark for contrast
    neui_widget_t l = app.widgets->create(app.session, p, NEUI_W_LABEL, 16, 16, 400, 22, nullptr);
    app.widgets->set_text(app.session, l, "Audio - this chip is custom-coloured.");
    neui_widget_t k = app.widgets->create(app.session, p, NEUI_W_KNOB, 16, 50, 80, 80, nullptr);
    app.widgets->set_text(app.session, k, "Gain");
  }

  // --- Page 4: About (label rewritten on each TAB_SELECTED) ----------------
  {
    neui_widget_t p = make_page(&app, "About");
    neui_widget_t l = app.widgets->create(app.session, p, NEUI_W_LABEL, 16, 16, 400, 22, nullptr);
    app.widgets->set_text(app.session, l, "About page viewed 0 time(s)");
    app.about_label = l.id;
  }

  if (app.tabs) app.tabs->set_selected(app.session, tv, 0);

  app.widgets->show(app.session, win);
  set_status(&app, "position: top-left");
  host->run(app.session);
  return 0;
}
