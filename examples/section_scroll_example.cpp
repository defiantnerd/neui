// section_scroll_example - demonstrates the scrolling NEUI_W_SECTION:
//
//   * Runtime toggle for the title-chip position (left / center / right /
//     none - "none" hides the band entirely so the body fills the rect).
//   * Runtime toggle for the scroll axes (none / vertical / horizontal /
//     both) via NEUI_ATTR_SCROLL_MODE.
//   * Runtime toggle for the wheel kinetics (platform / stepped / smooth)
//     via NEUI_ATTR_SCROLL_KINETICS - lets you compare hard-clamp vs
//     rubber-band + spring-back side by side at runtime.
//   * Content generator: pick a row count, click Regenerate, and the
//     section's children are torn down and rebuilt at that count.
//
// The example picks the default host (native on Win32 + macOS, xpl on
// Linux / null) so the user can compare native + xpl rendering of the
// scrolling SECTION side by side. Set NEUI_FORCE_XPL_HOST=1 in the
// environment (or edit ACTIVE_HOST below) to pin to the xpl host
// regardless of platform.
//
// Each generated row is a horizontal stripe of a LABEL + three BUTTONs,
// laid out wider than the section so horizontal scrolling has something
// to scroll over. Click any row's BUTTONs to confirm the inner widgets
// are still hit-testable through the section's clip rect.

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

// Default: nullptr (first registered host). On Win32 / macOS that's the
// native host; on Linux it's the xpl host. Override here to pin.
#define ACTIVE_HOST nullptr

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

struct AppState {
  neui_api_t*        neui    = nullptr;
  neui_widget_api_t* widgets = nullptr;
  neui_attr_api_t*   attrs   = nullptr;
  neui_items_api_t*  items   = nullptr;
  neui_scroll_api_t* scroll  = nullptr;
  neui_session_t     session = { 0 };

  uint32_t win_id          = 0;
  uint32_t align_combo     = 0;
  uint32_t scroll_combo    = 0;
  uint32_t kinetics_combo  = 0;
  uint32_t count_input     = 0;
  uint32_t regen_btn       = 0;
  uint32_t scroll_to_input = 0;
  uint32_t scroll_to_btn   = 0;
  uint32_t status_label    = 0;
  uint32_t section_id      = 0;

  std::vector<uint32_t> row_widget_ids;  // every widget owned by the section's content
  int                    row_count       = 30;
  std::string            status_text;
};

static const char* const k_align_choices[] = {
  "none", "left", "center", "right"
};
static const int k_align_count =
  static_cast<int>(sizeof(k_align_choices) / sizeof(k_align_choices[0]));

static const char* const k_scroll_choices[] = {
  "none", "vertical", "horizontal", "both"
};
static const int k_scroll_count =
  static_cast<int>(sizeof(k_scroll_choices) / sizeof(k_scroll_choices[0]));

// Labels paired with the numeric NEUI_SCROLL_KINETICS_* values. The index
// in the combo IS the enum value (PLATFORM=0, STEPPED=1, SMOOTH=2).
static const char* const k_kinetics_choices[] = {
  "platform", "stepped", "smooth"
};
static const int k_kinetics_count =
  static_cast<int>(sizeof(k_kinetics_choices) / sizeof(k_kinetics_choices[0]));

// Rebuild the section's content: tear down everything we generated last
// time, then create `row_count` rows of LABEL + three BUTTONs each. Row
// height stays fixed; the row's width intentionally exceeds the section
// body so horizontal scrolling has something to traverse.
static void rebuild_section(AppState* a)
{
  // Destroy previous content. Destroying a widget detaches it from the
  // tree and tears down its children; row widgets are direct children of
  // the section here so destroying each top-level row is enough.
  for (uint32_t wid : a->row_widget_ids) {
    a->widgets->destroy(a->session, neui_widget_t{ wid });
  }
  a->row_widget_ids.clear();

  const int row_h     = 36;
  const int top_pad   = 6;    // body-relative: 6 px below the chip band
                              // (or below the section top when chip="none")
  const int label_w   = 180;
  const int btn_w     = 110;
  const int btn_h     = 24;
  const int btn_gap   = 6;

  neui_widget_t section{ a->section_id };

  for (int i = 0; i < a->row_count; ++i) {
    int row_y = top_pad + i * row_h;
    int row_x = 8;

    neui_widget_t label = a->widgets->create(a->session, section, NEUI_W_LABEL,
                                               row_x, row_y, label_w, btn_h,
                                               nullptr);
    char buf[64];
    snprintf(buf, sizeof(buf), "Row %d - the answer is %d", i + 1, (i + 1) * 7);
    a->widgets->set_text(a->session, label, buf);
    a->row_widget_ids.push_back(label.id);

    int bx = row_x + label_w + btn_gap;
    // 6 buttons per row so the row overflows the section width and
    // horizontal scrolling has something to traverse.
    for (int b = 0; b < 6; ++b) {
      neui_widget_t btn = a->widgets->create(a->session, section, NEUI_W_BUTTON,
                                                bx, row_y, btn_w, btn_h,
                                                nullptr);
      snprintf(buf, sizeof(buf), "R%d-A%d", i + 1, b + 1);
      a->widgets->set_text(a->session, btn, buf);
      a->row_widget_ids.push_back(btn.id);
      bx += btn_w + btn_gap;
    }
  }

  char status[128];
  snprintf(status, sizeof(status), "Generated %d row%s.",
           a->row_count, a->row_count == 1 ? "" : "s");
  a->widgets->set_text(a->session, neui_widget_t{ a->status_label }, status);
}

// Reads the count INPUTBOX, clamps to a sane range, mirrors back to the
// box if the user typed something stupid, and rebuilds the section.
static void apply_row_count(AppState* a)
{
  char buf[64] = {0};
  int n = a->widgets->get_text(a->session, neui_widget_t{ a->count_input },
                                 buf, sizeof(buf));
  (void)n;
  int v = atoi(buf);
  if (v < 0)    v = 0;
  if (v > 1000) v = 1000;
  a->row_count = v;
  snprintf(buf, sizeof(buf), "%d", v);
  a->widgets->set_text(a->session, neui_widget_t{ a->count_input }, buf);
  rebuild_section(a);
}

static void apply_align_from_combo(AppState* a)
{
  uint32_t sel = a->items->get_selected(a->session,
                                          neui_widget_t{ a->align_combo });
  if (sel >= (uint32_t)k_align_count) sel = 0;
  a->attrs->set_string(a->session, neui_widget_t{ a->section_id },
                        NEUI_ATTR_ALIGN_TEXT, k_align_choices[sel]);
}

static void apply_scroll_from_combo(AppState* a)
{
  uint32_t sel = a->items->get_selected(a->session,
                                          neui_widget_t{ a->scroll_combo });
  if (sel >= (uint32_t)k_scroll_count) sel = 0;
  a->attrs->set_string(a->session, neui_widget_t{ a->section_id },
                        NEUI_ATTR_SCROLL_MODE, k_scroll_choices[sel]);
}

static void apply_kinetics_from_combo(AppState* a)
{
  uint32_t sel = a->items->get_selected(a->session,
                                          neui_widget_t{ a->kinetics_combo });
  if (sel >= (uint32_t)k_kinetics_count) sel = 0;
  // Combo index matches the public enum: 0=PLATFORM, 1=STEPPED, 2=SMOOTH.
  a->attrs->set_int(a->session, neui_widget_t{ a->section_id },
                     NEUI_ATTR_SCROLL_KINETICS, (int32_t)sel);
}

static bool NEUI_ABI on_event(void* token, neui_event_t* event)
{
  auto* a = static_cast<AppState*>(token);
  switch (event->type) {
    case NEUI_EVENT_APP_QUIT:
      return true;

    case NEUI_EVENT_MOUSE_BUTTON_CLICK: {
      uint32_t wid = event->data.mouse.widget.id;
      if (wid == a->regen_btn) {
        apply_row_count(a);
        return true;
      }
      if (wid == a->scroll_to_btn) {
        // Read the "scroll to row N" input + call ensure_visible on the
        // first widget of that row. The first widget per row is the
        // LABEL (row_widget_ids holds 7 entries per row: 1 LABEL + 6
        // BUTTONs). 1-based input matches the LABEL text.
        char buf[32] = {0};
        a->widgets->get_text(a->session,
                              neui_widget_t{ a->scroll_to_input },
                              buf, sizeof(buf));
        int row = atoi(buf);
        if (row >= 1 && row <= a->row_count && a->scroll) {
          int per_row = 7;
          size_t idx = (size_t)(row - 1) * per_row;
          if (idx < a->row_widget_ids.size()) {
            a->scroll->ensure_visible(a->session,
                                       neui_widget_t{ a->row_widget_ids[idx] });
          }
        }
        return true;
      }
      // Generated content buttons - log clicks so the user can verify
      // the inner widgets receive events through the section's clip.
      for (uint32_t r : a->row_widget_ids) {
        if (wid == r) {
          char buf[128] = {0};
          a->widgets->get_text(a->session, neui_widget_t{ wid }, buf, sizeof(buf));
          dbglog("[section_scroll] click: %s\n", buf);
          return true;
        }
      }
      break;
    }

    case NEUI_EVENT_SCROLL_CHANGED: {
      // Demonstrates the new event; mirror the position into the status
      // label so the user can see scroll motion at a glance.
      if (event->data.scroll.widget.id == a->section_id) {
        char buf[96];
        snprintf(buf, sizeof(buf), "Scroll: (%d, %d)",
                 event->data.scroll.scroll_x,
                 event->data.scroll.scroll_y);
        a->widgets->set_text(a->session,
                              neui_widget_t{ a->status_label }, buf);
        return true;
      }
      break;
    }

    case NEUI_EVENT_ITEM_SELECTED: {
      uint32_t wid = event->data.item.widget.id;
      if (wid == a->align_combo) {
        apply_align_from_combo(a);
        return true;
      }
      if (wid == a->scroll_combo) {
        apply_scroll_from_combo(a);
        return true;
      }
      if (wid == a->kinetics_combo) {
        apply_kinetics_from_combo(a);
        return true;
      }
      break;
    }

    default:
      break;
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

int main(int /*argc*/, char* /*argv*/[])
{
  neui_init();
  neui_api_t* host = neui_get_api(ACTIVE_HOST);
  if (!host) host = neui_get_api(nullptr);
  if (!host) { dbglog("[section_scroll_example] no host\n"); return 1; }

  AppState app;
  app.neui = host;

  neui_client_t client;
  client.neui_version  = NEUI_VERSION;
  client.get_interface = get_interface;
  app.session = host->create_session(&client, &app);
  if (!app.session.session) {
    dbglog("[section_scroll_example] no session\n"); return 1;
  }

  app.widgets = (neui_widget_api_t*)host->get_interface(app.session, NEUI_API_WIDGETS);
  app.attrs   = (neui_attr_api_t*)  host->get_interface(app.session, NEUI_API_ATTRS);
  app.items   = (neui_items_api_t*) host->get_interface(app.session, NEUI_API_ITEMS);
  app.scroll  = (neui_scroll_api_t*)host->get_interface(app.session, NEUI_API_SCROLL);
  if (!app.widgets || !app.attrs || !app.items) {
    dbglog("[section_scroll_example] missing API\n"); return 1;
  }
  // NEUI_API_SCROLL is optional: log if unavailable so the user knows the
  // "Scroll to row" button + live position readout won't work.
  if (!app.scroll) dbglog("[section_scroll_example] NEUI_API_SCROLL unavailable\n");

  // Window oversize so the 740-wide section + a 740-wide toolbar fit
  // inside the client area (Win32 client < window by border + title).
  neui_widget_t win = app.widgets->create(app.session,
                                            neui_widget_t{ UINT32_MAX },
                                            NEUI_W_APPWINDOW,
                                            100, 100, 800, 580, nullptr);
  app.widgets->set_text(app.session, win, "neui scrolling SECTION");
  app.win_id = win.id;

  // ---- Toolbar across the top --------------------------------------------
  int toolbar_y = 10;
  int x = 10;

  auto align_lbl = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                         x, toolbar_y, 80, 22, nullptr);
  app.widgets->set_text(app.session, align_lbl, "Chip pos:");
  x += 80 + 4;

  // ComboBox coords describe just the collapsed bar; the drop list sizes
  // itself from the item count when opened.
  auto align_combo = app.widgets->create(app.session, win, NEUI_W_COMBOBOX,
                                           x, toolbar_y, 110, 22, nullptr);
  app.align_combo = align_combo.id;
  for (int i = 0; i < k_align_count; ++i)
    app.items->add(app.session, align_combo, k_align_choices[i], nullptr);
  app.items->set_selected(app.session, align_combo, 2);  // "center"
  x += 110 + 12;

  auto scroll_lbl = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                          x, toolbar_y, 80, 22, nullptr);
  app.widgets->set_text(app.session, scroll_lbl, "Scroll:");
  x += 80 + 4;

  auto scroll_combo = app.widgets->create(app.session, win, NEUI_W_COMBOBOX,
                                            x, toolbar_y, 110, 22, nullptr);
  app.scroll_combo = scroll_combo.id;
  for (int i = 0; i < k_scroll_count; ++i)
    app.items->add(app.session, scroll_combo, k_scroll_choices[i], nullptr);
  app.items->set_selected(app.session, scroll_combo, 1);  // "vertical"
  x += 110 + 12;

  auto count_lbl = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                         x, toolbar_y, 50, 22, nullptr);
  app.widgets->set_text(app.session, count_lbl, "Rows:");
  x += 50 + 4;

  auto count_input = app.widgets->create(app.session, win, NEUI_W_INPUTBOX,
                                           x, toolbar_y, 60, 22, nullptr);
  app.count_input = count_input.id;
  app.widgets->set_text(app.session, count_input, "30");
  x += 60 + 8;

  auto regen_btn = app.widgets->create(app.session, win, NEUI_W_BUTTON,
                                         x, toolbar_y, 100, 22, nullptr);
  app.widgets->set_text(app.session, regen_btn, "Regenerate");
  app.regen_btn = regen_btn.id;
  x += 100 + 12;

  auto status_label = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                            x, toolbar_y, 240, 22, nullptr);
  app.status_label = status_label.id;
  app.widgets->set_text(app.session, status_label, "Ready.");

  // ---- Second toolbar row (kinetics selector + ensure_visible demo) ------
  int row2_y = toolbar_y + 28;
  int x2 = 10;

  auto kin_lbl = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                       x2, row2_y, 80, 22, nullptr);
  app.widgets->set_text(app.session, kin_lbl, "Kinetics:");
  x2 += 80 + 4;

  auto kin_combo = app.widgets->create(app.session, win, NEUI_W_COMBOBOX,
                                         x2, row2_y, 110, 22, nullptr);
  app.kinetics_combo = kin_combo.id;
  for (int i = 0; i < k_kinetics_count; ++i)
    app.items->add(app.session, kin_combo, k_kinetics_choices[i], nullptr);
  app.items->set_selected(app.session, kin_combo, 0);  // "platform"
  x2 += 110 + 24;

  auto stl_lbl = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                       x2, row2_y, 100, 22, nullptr);
  app.widgets->set_text(app.session, stl_lbl, "Scroll to row:");
  x2 += 100 + 4;

  auto stl_input = app.widgets->create(app.session, win, NEUI_W_INPUTBOX,
                                         x2, row2_y, 50, 22, nullptr);
  app.scroll_to_input = stl_input.id;
  app.widgets->set_text(app.session, stl_input, "20");
  x2 += 50 + 6;

  auto stl_btn = app.widgets->create(app.session, win, NEUI_W_BUTTON,
                                       x2, row2_y, 70, 22, nullptr);
  app.widgets->set_text(app.session, stl_btn, "Go");
  app.scroll_to_btn = stl_btn.id;

  // ---- The scrolling SECTION ---------------------------------------------
  int section_y = row2_y + 30;
  auto section = app.widgets->create(app.session, win, NEUI_W_SECTION,
                                       10, section_y, 740, 580 - section_y - 50,
                                       nullptr);
  app.widgets->set_text(app.session, section, "Scrollable content");
  app.attrs->set_string(app.session, section, NEUI_ATTR_ALIGN_TEXT, "center");
  app.attrs->set_string(app.session, section, NEUI_ATTR_SCROLL_MODE, "vertical");
  // Default kinetics: PLATFORM (matches the combo's initial selection).
  app.attrs->set_int(app.session, section, NEUI_ATTR_SCROLL_KINETICS,
                      NEUI_SCROLL_KINETICS_PLATFORM);
  // Leave NEUI_ATTR_BACKGROUND unset so the body uses the theme-derived
  // section shade (a lifted frame_bg): a panel that stays distinct from the
  // window AND legible with the theme's text colour under both light and dark
  // system themes. Hardcoding a light colour here (e.g. 0xFFE0E8F0) painted a
  // light body under a dark theme, leaving the theme-coloured (white) child
  // labels/buttons low-contrast - the framework paints child widgets from the
  // palette, it does not re-theme them to a section's custom background.
  app.section_id = section.id;

  // First-time generation BEFORE show() so children are present at the
  // first paint. show() creates the native window + descends into all
  // descendants on the win32 native + macOS native paths; the xpl host
  // (which we explicitly target) is lazy but it's still cleaner to have
  // the tree built up-front.
  rebuild_section(&app);

  app.widgets->show(app.session, win);
  host->run(app.session);
  host->destroy(app.session);
  return 0;
}
