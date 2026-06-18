// Scene delegate - builds the neui UI once the UIWindowScene connects.
//
// Written as .mm so it can call neui's C/C++ API directly. The example targets
// the crossplatform (XPL) host explicitly via neui_get_api("neui.host.crossplatform")
// because the native iOS host is still a stub in this milestone.
//
// MILESTONE 3 additions: an IMAGE widget loading a bundled PNG (exercises the
// image_loader_ios.h CGImageSource + bundle-resolution path), the appwindow
// opting into NEUI_ATTR_FOLLOW_SYSTEM_THEME=1 (exercises the
// theme_provider_ios.h light/dark path + traitCollectionDidChange: live update),
// a clipboard round-trip smoke (exercises clipboard_ios.h), and a RESIZE handler
// that grows the SECTION to fill the frame so the UI fills the device screen.

#import "SceneDelegate.h"
#import <UIKit/UIKit.h>   // dispatch a launch toast + capture a screenshot moment

#include <neui/neui.h>
#include <cstring>
#include <cstdio>

// PHASE 2 (investigation): a separate screen exercising the five "complex"
// widgets - GRID / LISTBOX / COMBOBOX / TREEVIEW / TABVIEW - on the
// crossplatform (XPL) host. These widgets are implemented in platform-neutral
// shared C++ (hosts/crossplatform/host.cpp); the hypothesis under test is that
// they render + function on the xpl iOS platform layer with no new host code.
// Enable by configuring with -DNEUI_IOS_PHASE2=1 (the CMake target sets the
// compile define). When set, build_ui() builds the phase-2 screen and forces
// the XPL host regardless of NEUI_IOS_USE_NATIVE_HOST. Default off, so the
// normal milestone-7 native-host demo is unchanged.
#ifndef NEUI_IOS_PHASE2
#define NEUI_IOS_PHASE2 0
#endif

// ---------------------------------------------------------------------------
// neui client glue. A tiny app model + a widget client whose onevent echoes
// the input box into the label on button click and reflects the checkbox /
// slider state, so taps + the system keyboard are visibly working.

namespace {

struct App {
  neui_widget_api_t*    w      = nullptr;
  neui_attr_api_t*      attrs  = nullptr;
  neui_clipboard_api_t* clip   = nullptr;
  neui_tree_api_t*      tree   = nullptr;
  neui_notify_api_t*    notify = nullptr;
  neui_grid_api_t*      grid   = nullptr;
  neui_tabs_api_t*      tabs   = nullptr;
  neui_items_api_t*     items  = nullptr;
  neui_metrics_api_t*   metrics = nullptr;
  neui_dnd_api_t*       dnd    = nullptr;
  neui_behavior_api_t*  behavior = nullptr;
  neui_asset_api_t*     assets = nullptr;
  neui_session_t        s      = {};
  neui_widget_t         win    = {};
  neui_widget_t         mb     = {};
  neui_widget_t         sec    = {};
  neui_widget_t         input  = {};
  neui_widget_t         button = {};
  neui_widget_t         label  = {};
  neui_widget_t         check  = {};
  neui_widget_t         check3 = {};   // tri-state: always the square-glyph control
  neui_widget_t         slider = {};
  neui_widget_t         image  = {};
  neui_widget_t         grid_w = {};   // native-host GRID (M-phase2)
  neui_widget_t         list_w  = {};  // native-host LISTBOX (UITableView)
  neui_widget_t         tree_w  = {};  // native-host TREEVIEW (UITableView)
  neui_widget_t         combo_w = {};  // native-host COMBOBOX (UIButton + UIMenu)
  // Native-host TABVIEW (M-phase2): a chip strip with pages, one per tab.
  neui_widget_t         tv      = {};
  neui_widget_t         tab_lbl = {};  // status label updated on tab switch
  // Drag & drop demo (native host). A CUSTOMDRAW drag source (carrying a
  // DRAG_SOURCE behavior + a text DataItem) and a CUSTOMDRAW drop target whose
  // DROP updates dnd_status. Internal source->target drag + external drag from
  // another app (Notes / Safari) into the target both work.
  neui_widget_t         dnd_src    = {};
  neui_widget_t         dnd_target = {};
  neui_widget_t         dnd_status = {};
  neui_data_item_t      dnd_item   = {};   // text payload the source carries
  neui_asset_t          dnd_behav  = {};   // DRAG_SOURCE behavior asset
  neui_item_t           tab_g   = {};  // grid page index (for relayout)
  // Menu item ids whose TREE_ITEM_ACTIVATED we react to.
  neui_item_t           mi_view_msg = {};
  neui_item_t           mi_file_new = {};
  neui_item_t           mi_view_toast = {};
  neui_item_t           mi_view_alert = {};
};

// Lay the SECTION out to (nearly) fill the frame's CLIENT area, leaving a
// margin. The client rect (origin x/y + w/h) excludes the iOS top inset
// (status-bar/notch safe area + the hamburger band), so the SECTION title chip
// no longer paints under the notch. Called at build time and on every RESIZE
// (which now also fires when the safe-area inset changes).
void relayout(App* a)
{
  if (!a->w || !a->w->get_client_rect) return;
  int cx = 0, cy = 0, cw = 0, ch = 0;
  a->w->get_client_rect(a->s, a->win, &cx, &cy, &cw, &ch);
  if (cw <= 0 || ch <= 0) return;

  // RESPONSIVE LAYOUT via NEUI_API_METRICS. On iOS these grow with the user's
  // Dynamic Type setting (the painted-UI scale), so the controls + spacing get
  // taller at accessibility text sizes instead of clipping. On desktop the
  // metrics are the framework defaults (scale 1.0), so the layout is unchanged.
  // metric() falls back to sensible fixed values if the interface is absent.
  float scale = a->metrics ? a->metrics->ui_scale(a->s) : 1.0f;
  int M      = a->metrics ? a->metrics->metric(a->s, NEUI_METRIC_MARGIN)         : 16;
  int gap    = a->metrics ? a->metrics->metric(a->s, NEUI_METRIC_SPACING)        : 8;
  int ctrl_h = a->metrics ? a->metrics->metric(a->s, NEUI_METRIC_CONTROL_HEIGHT) : 36;
  if (M < 8) M = 8;            // keep a readable outer margin even at tiny sizes
  // The interactive controls (input / button / slider) and the labels stand a
  // bit taller than a plain control so touch targets stay comfortable as text
  // grows; derived from the control height + spacing so they scale together.
  int row_h   = ctrl_h + gap;
  int label_h = ctrl_h;

  int sec_x = cx + M, sec_y = cy + M;
  int sec_w = cw - 2 * M;
  int sec_h = ch - 2 * M;
  if (sec_w < 80) sec_w = 80;
  if (sec_h < 80) sec_h = 80;
  a->w->set_pos(a->s, a->sec, sec_x, sec_y, sec_w, sec_h);

  // Children are body-relative; stretch the full-width controls + the image.
  // Stack them with metric-derived row heights + spacing so the whole column
  // reflows when Dynamic Type changes (no hard-coded 16/64/116... offsets).
  int inner = sec_w - 2 * M;
  int y = M;
  a->w->set_pos(a->s, a->input,  M, y, inner, ctrl_h);  y += row_h;
  // Size the Submit button to its label via measure_text (+ horizontal padding),
  // clamped to a sensible min, so the button never clips its text at large sizes.
  int btn_w = 120;
  if (a->metrics) {
    int tw = a->metrics->measure_text(a->s, "Submit", nullptr, 0.0f, 0);
    btn_w = tw + 2 * M + 2 * gap;        // text + symmetric padding
    if (btn_w < 96) btn_w = 96;
  }
  a->w->set_pos(a->s, a->button, M, y, btn_w, ctrl_h);  y += row_h;
  a->w->set_pos(a->s, a->check,  M, y, inner, ctrl_h);  y += row_h;
  if (a->check3.id != 0) { a->w->set_pos(a->s, a->check3, M, y, inner, ctrl_h); y += row_h; }
  a->w->set_pos(a->s, a->slider, M, y, inner, ctrl_h);  y += row_h;
  // Reflect the current UI scale into the label so the responsive recompute is
  // observable headlessly (it updates on NEUI_EVENT_METRICS_CHANGED).
  {
    char sbuf[64];
    std::snprintf(sbuf, sizeof sbuf, "UI scale: %.2f  (control %dpx)", (double)scale, ctrl_h);
    if (a->label.id != 0) a->w->set_text(a->s, a->label, sbuf);
  }
  a->w->set_pos(a->s, a->label,  M, y, inner, label_h); y += row_h;
  // DnD demo tiles: source + target side by side, status label below.
  if (a->dnd_src.id != 0) {
    int tile_h = ctrl_h + gap + 8;
    int half   = (inner - gap) / 2;
    a->w->set_pos(a->s, a->dnd_src,    M,            y, half, tile_h);
    a->w->set_pos(a->s, a->dnd_target, M + half + gap, y, inner - half - gap, tile_h);
    y += tile_h + gap;
    a->w->set_pos(a->s, a->dnd_status, M, y, inner, label_h);
    y += row_h;
  }
  // Keep the downstream image / tabview block anchored below the reflowed column.
  int below_y_dyn = y;
  // Image, then (native host only) a TABVIEW fills the remaining body space
  // below the image so the native-host TABVIEW port renders + its chip strip is
  // tappable. The GRID lives inside one of the tabview's pages; its size tracks
  // the page body. On the xpl host there is no tabview; the image fills the
  // rest as before.
  int below_y = below_y_dyn;
  if (a->tv.id != 0) {
    int img_h = 140;
    a->w->set_pos(a->s, a->image, M, below_y, inner, img_h);
    int tv_y = below_y + img_h + M;
    int tv_h = sec_h - tv_y - M;
    if (tv_h < 200) tv_h = 200;
    a->w->set_pos(a->s, a->tv, M, tv_y, inner, tv_h);
    // The TABVIEW positions its pages to its content body rect automatically;
    // page children (the label/button/slider/grid) are body-relative. Size the
    // GRID to roughly fill its page: the tabview width minus the page margins,
    // and the tabview height minus the chip strip (~44pt scaled) + margins. The
    // page clips any overflow, so a generous estimate is fine.
    const int kStrip = 48;                   // chip strip + slack
    int page_w = inner - 2 * M;
    int page_h = tv_h - kStrip - 2 * M;
    if (page_w < 80)  page_w = 80;
    if (page_h < 120) page_h = 120;
    // The GRID / LISTBOX / TREEVIEW each fill their own page body. The page
    // clips overflow, so a generous estimate is fine for the inactive pages.
    if (a->grid_w.id != 0) a->w->set_pos(a->s, a->grid_w, M, M, page_w, page_h);
    if (a->list_w.id != 0) a->w->set_pos(a->s, a->list_w, M, M, page_w, page_h);
    if (a->tree_w.id != 0) a->w->set_pos(a->s, a->tree_w, M, M, page_w, page_h);
  } else {
    int img_h = sec_h - below_y - M;
    if (img_h < 60) img_h = 60;
    a->w->set_pos(a->s, a->image, M, below_y, inner, img_h);
  }
}

bool onevent(void* token, neui_event_t* e)
{
  App* a = static_cast<App*>(token);

  // APP_QUIT is informational on iOS (the app can't self-terminate); accept it.
  if (e->type == NEUI_EVENT_APP_QUIT) return true;

  // ---- Drag & drop demo --------------------------------------------------
  // CUSTOMDRAW paint: render the source (blue) + target (green) tiles so the
  // demo is visible + screenshot-able. (The source's DRAG_SOURCE behavior runs
  // independently; CUSTOMDRAW with a behavior asset still fires WIDGET_PAINT.)
  if (e->type == NEUI_EVENT_WIDGET_PAINT) {
    if (e->data.paint.widget.id == a->dnd_src.id) {
      auto* px = e->data.paint.painter_api; auto* ph = e->data.paint.p;
      px->fill_rect(ph, 0, 0, e->data.paint.width, e->data.paint.height, 0xff204060);
      px->draw_rect(ph, 0.5f, 0.5f, e->data.paint.width - 1.0f,
                    e->data.paint.height - 1.0f, 1.0f, 0xff80a0d0);
      px->draw_text(ph, 8, 8, e->data.paint.width - 16, 20,
                    "Drag me", 14.0f, 0xffffffff);
      px->draw_text(ph, 8, 30, e->data.paint.width - 16, 18,
                    "(long-press)", 11.0f, 0xffb0c0e0);
      return false;
    }
    if (e->data.paint.widget.id == a->dnd_target.id) {
      auto* px = e->data.paint.painter_api; auto* ph = e->data.paint.p;
      px->fill_rect(ph, 0, 0, e->data.paint.width, e->data.paint.height, 0xff206040);
      px->draw_rect(ph, 0.5f, 0.5f, e->data.paint.width - 1.0f,
                    e->data.paint.height - 1.0f, 2.0f, 0xff80d0a0);
      px->draw_text(ph, 8, 8, e->data.paint.width - 16, 20,
                    "Drop here", 14.0f, 0xffffffff);
      px->draw_text(ph, 8, 30, e->data.paint.width - 16, 18,
                    "text / url / png", 11.0f, 0xffb0e0c0);
      return false;
    }
  }
  // Drop-target negotiation: accept COPY on ENTER/MOVE so the OS shows the
  // accept badge; on DROP read the payload + report it.
  if ((e->type == NEUI_EVENT_DND_ENTER || e->type == NEUI_EVENT_DND_MOVE) &&
      e->data.dnd.widget.id == a->dnd_target.id) {
    if (a->dnd) a->dnd->accept(a->s, NEUI_DND_ACTION_COPY);
    return false;
  }
  if (e->type == NEUI_EVENT_DND_DROP &&
      e->data.dnd.widget.id == a->dnd_target.id) {
    if (a->dnd) a->dnd->accept(a->s, NEUI_DND_ACTION_COPY);
    char buf[256] = {0};
    neui_data_item_t item = e->data.dnd.data;
    if (a->clip && item.id) {
      if (a->clip->item_get_format(a->s, item, NEUI_MIME_TEXT, nullptr, 0) > 0)
        a->clip->item_get_format(a->s, item, NEUI_MIME_TEXT, buf, sizeof buf);
      else if (a->clip->item_get_format(a->s, item, NEUI_MIME_URI_LIST, nullptr, 0) > 0)
        a->clip->item_get_format(a->s, item, NEUI_MIME_URI_LIST, buf, sizeof buf);
      else if (a->clip->item_get_format(a->s, item, NEUI_MIME_PNG, nullptr, 0) > 0)
        std::snprintf(buf, sizeof buf, "(image/png, %d bytes)",
                      a->clip->item_get_format(a->s, item, NEUI_MIME_PNG, nullptr, 0));
    }
    char out[320];
    std::snprintf(out, sizeof out, "Dropped: %s", buf[0] ? buf : "(no readable text)");
    if (a->dnd_status.id) a->w->set_text(a->s, a->dnd_status, out);
    return false;
  }
  // DRAG_SOURCE result feedback: the behavior runtime writes "demo.drag.result"
  // (the negotiated action) + fires ATTR_CHANGED when our drag completes.
  if (e->type == NEUI_EVENT_ATTR_CHANGED &&
      e->data.attr.widget.id == a->dnd_src.id &&
      e->data.attr.attr_key &&
      std::strcmp(e->data.attr.attr_key, "demo.drag.result") == 0) {
    int action = (int)e->data.attr.value;
    const char* name = action == NEUI_DND_ACTION_COPY ? "copy"
                     : action == NEUI_DND_ACTION_MOVE ? "move"
                     : action == NEUI_DND_ACTION_LINK ? "link" : "cancelled";
    char out[64];
    std::snprintf(out, sizeof out, "Drag finished: %s", name);
    if (a->dnd_status.id) a->w->set_text(a->s, a->dnd_status, out);
    return false;
  }

  // The frame fills its scene on iOS; re-layout to track the true size
  // (scene-seeded at create, then corrected here on rotation / resize / a
  // safe-area inset change). relayout() reads the client rect, so it tracks the
  // top inset reserved for the notch + hamburger band.
  if (e->type == NEUI_EVENT_RESIZE &&
      e->data.resize.widget.id == a->win.id) {
    relayout(a);
    return false;
  }

  // Platform layout metrics changed (iOS Dynamic Type / rotation / safe-area).
  // Re-run the responsive layout so control heights / margins / the Submit
  // button width grow with the new text-size scale - the controls reflow
  // WITHOUT a tap, reducing the truncation seen at accessibility sizes. The
  // payload carries the new ui_scale; relayout() re-queries the metrics anyway.
  if (e->type == NEUI_EVENT_METRICS_CHANGED &&
      e->data.metrics.widget.id == a->win.id) {
    std::printf("[neui-ios] METRICS_CHANGED ui_scale=%.3f -> relayout\n",
                (double)e->data.metrics.ui_scale);
    relayout(a);
    return false;
  }

  // Menu activation: the hamburger UIMenu routes picks through
  // dispatch_menu_event, which fires TREE_ITEM_ACTIVATED for client items (and
  // invokes the focused widget for built-in commands like Copy first). React to
  // the two observable items so the menu is testable.
  if (e->type == NEUI_EVENT_TREE_ITEM_ACTIVATED &&
      e->data.tree.widget.id == a->mb.id) {
    uint32_t item = e->data.tree.item.id;
    if (item == a->mi_view_msg.id)
      a->w->set_text(a->s, a->label, "View > Say Hello tapped!");
    else if (item == a->mi_file_new.id)
      a->w->set_text(a->s, a->label, "File > New tapped!");
    else if (item == a->mi_view_toast.id && a->notify)
      // Fire the shared toast overlay (CADisplayLink heartbeat + paint_toast).
      a->notify->toast(a->s, a->win, "Toast from the menu!\nTap me to dismiss.");
    else if (item == a->mi_view_alert.id && a->notify)
      // Present a UIAlertController (async: returns NEUI_MB_IOS_PENDING).
      a->notify->message_box(a->s, a->win, "This is a neui message box on iOS.",
                             "Message", NEUI_MB_OKCANCEL | NEUI_MB_ICONINFORMATION);
    return false;
  }

  // Native-host TABVIEW: a chip tap fires TAB_SELECTED for the incoming tab.
  // Reflect it into the page-1 status label so the tab switch is observable.
  if (e->type == NEUI_EVENT_TAB_SELECTED && a->tv.id != 0 &&
      e->data.tab.widget.id == a->tv.id) {
    if (a->tab_lbl.id != 0) {
      char buf[64];
      std::snprintf(buf, sizeof buf, "Tab %u selected.", e->data.tab.tab_index + 1);
      a->w->set_text(a->s, a->tab_lbl, buf);
    }
    return false;
  }

  if (e->type == NEUI_EVENT_MOUSE_BUTTON_CLICK &&
      e->data.mouse.widget.id == a->button.id) {
    char in_buf[256] = {0};
    char out_buf[320];
    a->w->get_text(a->s, a->input, in_buf, sizeof in_buf);
    std::snprintf(out_buf, sizeof out_buf, "You typed: %s", in_buf);
    a->w->set_text(a->s, a->label, out_buf);
    // Also surface a toast so the Submit button is a one-tap toast trigger.
    if (a->notify) {
      char toast_buf[360];
      std::snprintf(toast_buf, sizeof toast_buf, "Submitted: %s",
                    in_buf[0] ? in_buf : "(empty)");
      a->notify->toast(a->s, a->win, toast_buf);
    }
    return false;
  }

  if (e->type == NEUI_EVENT_CHECKBOX_CHANGED &&
      e->data.checkbox.widget.id == a->check.id) {
    a->w->set_text(a->s, a->label,
                   e->data.checkbox.state == NEUI_CHECK_CHECKED
                     ? "Checkbox: on" : "Checkbox: off");
    return false;
  }

  if (e->type == NEUI_EVENT_VALUE_CHANGED &&
      e->data.value.widget.id == a->slider.id) {
    char out_buf[64];
    std::snprintf(out_buf, sizeof out_buf, "Slider: %.2f",
                  (double)e->data.value.value);
    a->w->set_text(a->s, a->label, out_buf);
    return false;
  }

  // GRID row tap (native-host GRID port) reflects the row into the label.
  if (e->type == NEUI_EVENT_GRID_ROW_SELECTED &&
      e->data.grid_row.widget.id == a->grid_w.id) {
    char out_buf[64];
    std::snprintf(out_buf, sizeof out_buf, "Grid row: %d",
                  (int)e->data.grid_row.row);
    a->w->set_text(a->s, a->label, out_buf);
    return false;
  }

  // LISTBOX selection (native-host UITableView) reflects the picked item text
  // into the page-1 status label so a tap is observable.
  if (e->type == NEUI_EVENT_ITEM_SELECTED && a->list_w.id != 0 &&
      e->data.item.widget.id == a->list_w.id && a->tab_lbl.id != 0) {
    char txt[64] = {0};
    if (a->items)
      a->items->get_text(a->s, a->list_w, e->data.item.index, txt, sizeof txt);
    char out_buf[96];
    std::snprintf(out_buf, sizeof out_buf, "List: %s", txt[0] ? txt : "(none)");
    a->w->set_text(a->s, a->tab_lbl, out_buf);
    return false;
  }

  // COMBOBOX selection (native-host UIButton + UIMenu pull-down) reflects the
  // picked item text into the page-1 status label so a pick is observable.
  if (e->type == NEUI_EVENT_ITEM_SELECTED && a->combo_w.id != 0 &&
      e->data.item.widget.id == a->combo_w.id && a->tab_lbl.id != 0) {
    char txt[64] = {0};
    if (a->items)
      a->items->get_text(a->s, a->combo_w, e->data.item.index, txt, sizeof txt);
    char out_buf[96];
    std::snprintf(out_buf, sizeof out_buf, "Combo: %s", txt[0] ? txt : "(none)");
    a->w->set_text(a->s, a->tab_lbl, out_buf);
    return false;
  }

  // TREEVIEW selection / activation (native-host UITableView). A row tap fires
  // SELECTED; a tap on an already-selected leaf fires ACTIVATED.
  if ((e->type == NEUI_EVENT_TREE_ITEM_SELECTED ||
       e->type == NEUI_EVENT_TREE_ITEM_ACTIVATED) &&
      a->tree_w.id != 0 && e->data.tree.widget.id == a->tree_w.id &&
      a->tab_lbl.id != 0) {
    char txt[64] = {0};
    if (a->tree)
      a->tree->get_text(a->s, a->tree_w, e->data.tree.item, txt, sizeof txt);
    char out_buf[96];
    std::snprintf(out_buf, sizeof out_buf, "Tree %s: %s",
                  e->type == NEUI_EVENT_TREE_ITEM_ACTIVATED ? "activated" : "selected",
                  txt[0] ? txt : "(none)");
    a->w->set_text(a->s, a->tab_lbl, out_buf);
    return false;
  }

  return false;
}

neui_widget_client_t g_wclient = { NEUI_VERSION, nullptr, onevent };

void* iface(void* /*t*/, const char* n)
{
  return std::strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&g_wclient;
}

// One process-wide app model + client; the example only ever builds one scene.
App         g_app;
neui_client_t g_client = { NEUI_VERSION, iface };

// Set clipboard text then read it back, logging the round-trip. A small smoke
// over clipboard_ios.h (UIPasteboard) so the seam is visibly exercised.
void clipboard_smoke(App* a)
{
  if (!a->clip) return;
  const char* probe = "neui-ios-clipboard-roundtrip";
  a->clip->set_text(a->s, probe);
  char buf[64] = {0};
  a->clip->get_text(a->s, buf, sizeof buf);
  std::printf("[neui-ios] clipboard round-trip: wrote \"%s\", read \"%s\" (%s)\n",
              probe, buf,
              std::strcmp(probe, buf) == 0 ? "OK" : "MISMATCH");
}

// MILESTONE 7: a compile-time switch selecting the NATIVE UIKit iOS host
// (neui.host.ios) vs the crossplatform (xpl) host. Default ON so this
// milestone's verification exercises the native host; flip to 0 to fall back
// to the xpl path (still fully functional).
#ifndef NEUI_IOS_USE_NATIVE_HOST
#define NEUI_IOS_USE_NATIVE_HOST 1
#endif

#if NEUI_IOS_PHASE2
// ---------------------------------------------------------------------------
// PHASE-2 SCREEN: GRID / LISTBOX / COMBOBOX / TREEVIEW / TABVIEW on the XPL
// host. Each widget lives on its own tab page so they all get a generous full-
// width slot; the TABVIEW itself is the fifth widget under test. Forces the
// crossplatform host. No new host code - if these paint + respond, the
// hypothesis (the xpl complex widgets work on iOS as-is) holds.

struct P2 {
  neui_widget_api_t* w     = nullptr;
  neui_attr_api_t*   attrs = nullptr;
  neui_items_api_t*  items = nullptr;
  neui_tree_api_t*   tree  = nullptr;
  neui_grid_api_t*   grid  = nullptr;
  neui_session_t     s     = {};
  neui_widget_t      win   = {};
  neui_widget_t      tv    = {};
  neui_widget_t      grid_w = {};
  neui_widget_t      list  = {};
  neui_widget_t      combo = {};
  neui_widget_t      treev = {};
  neui_widget_t      section = {};
  neui_widget_t      status = {};
};
P2 g_p2;

void p2_relayout(P2* a);   // forward decl (used by p2_onevent's RESIZE branch)

bool p2_onevent(void* token, neui_event_t* e)
{
  P2* a = static_cast<P2*>(token);
  if (e->type == NEUI_EVENT_APP_QUIT) return true;

  // Log a few observable interactions into the status label so a tap is
  // visibly working (where simctl can drive one).
  if (e->type == NEUI_EVENT_RESIZE && e->data.resize.widget.id == a->win.id) {
    p2_relayout(a);
    return false;
  }

  char buf[160];
  switch (e->type) {
    case NEUI_EVENT_ITEM_SELECTED:
      if (a->status.id) {
        std::snprintf(buf, sizeof buf, "ITEM_SELECTED idx=%u",
                      (unsigned)e->data.item.index);
        a->w->set_text(a->s, a->status, buf);
      }
      return false;
    case NEUI_EVENT_GRID_ROW_SELECTED:
      if (a->status.id) {
        std::snprintf(buf, sizeof buf, "GRID row=%d",
                      (int)e->data.grid_row.row);
        a->w->set_text(a->s, a->status, buf);
      }
      return false;
    case NEUI_EVENT_TREE_ITEM_SELECTED:
      if (a->status.id)
        a->w->set_text(a->s, a->status, "TREE item selected");
      return false;
    default:
      return false;
  }
}

neui_widget_client_t g_p2_wclient = { NEUI_VERSION, nullptr, p2_onevent };

void* p2_iface(void* /*t*/, const char* n)
{
  return std::strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&g_p2_wclient;
}

neui_client_t g_p2_client = { NEUI_VERSION, p2_iface };

void p2_relayout(P2* a)
{
  if (!a->w || !a->w->get_client_rect) return;
  int cx = 0, cy = 0, cw = 0, ch = 0;
  a->w->get_client_rect(a->s, a->win, &cx, &cy, &cw, &ch);
  if (cw <= 0 || ch <= 0) return;
  const int M = 12;
  int sx = cx + M, sy = cy + M, sw = cw - 2 * M;
  int sh = ch - 2 * M - 28;            // leave room for the status label
  if (sw < 80) sw = 80;
  if (sh < 120) sh = 120;
  a->w->set_pos(a->s, a->tv, sx, sy, sw, sh);
  a->w->set_pos(a->s, a->status, sx, sy + sh + 4, sw, 22);
}

void build_phase2()
{
  neui_init();
  neui_api_t* api = neui_get_api("neui.host.crossplatform");
  if (!api) api = neui_get_api(nullptr);
  if (!api) return;

  P2* a = &g_p2;
  a->s     = api->create_session(&g_p2_client, a);
  a->w     = (neui_widget_api_t*) api->get_interface(a->s, NEUI_API_WIDGETS);
  a->attrs = (neui_attr_api_t*)   api->get_interface(a->s, NEUI_API_ATTRS);
  a->items = (neui_items_api_t*)  api->get_interface(a->s, NEUI_API_ITEMS);
  a->tree  = (neui_tree_api_t*)   api->get_interface(a->s, NEUI_API_TREE);
  a->grid  = (neui_grid_api_t*)   api->get_interface(a->s, NEUI_API_GRID);
  if (!a->w) return;

  a->win = a->w->create(a->s, widget_none, NEUI_W_APPWINDOW, 0, 0, 390, 800, nullptr);
  if (a->attrs)
    a->attrs->set_int(a->s, a->win, NEUI_ATTR_FOLLOW_SYSTEM_THEME, 1);

  // TABVIEW spanning the frame; one complex widget per page.
  a->tv = a->w->create(a->s, a->win, NEUI_W_TABVIEW, 12, 48, 366, 700, nullptr);

  // --- Page 1: GRID -------------------------------------------------------
  neui_widget_t pg_grid = a->w->create(a->s, a->tv, NEUI_W_TABPAGE, 0, 0, 0, 0, nullptr);
  a->w->set_text(a->s, pg_grid, "Grid");
  a->grid_w = a->w->create(a->s, pg_grid, NEUI_W_GRID, 8, 8, 340, 600, nullptr);
  if (a->grid) {
    a->grid->add_column(a->s, a->grid_w, "Name",  150);
    a->grid->add_column(a->s, a->grid_w, "Kind",  90);
    a->grid->add_column(a->s, a->grid_w, "Size",  80);
    // 120 rows so the body overflows any device viewport - touch-pan scroll is
    // demonstrable (and clamps at the ends). The default 22 px row_h * 120 rows
    // is ~2640 px of content against a < 800 px body.
    static const char* kinds[] = { "file", "dir", "link" };
    static const char* sizes[] = { "12 KB", "-", "4 KB", "880 B", "1 MB", "2.3 MB" };
    for (int i = 0; i < 120; ++i) {
      char name[32];
      std::snprintf(name, sizeof name, "item-%03d", i);
      const char* v[] = { name, kinds[i % 3], sizes[i % 6], nullptr };
      a->grid->add_row(a->s, a->grid_w, v);
    }
  }

  // --- Page 2: LISTBOX ----------------------------------------------------
  neui_widget_t pg_list = a->w->create(a->s, a->tv, NEUI_W_TABPAGE, 0, 0, 0, 0, nullptr);
  a->w->set_text(a->s, pg_list, "List");
  a->list = a->w->create(a->s, pg_list, NEUI_W_LISTBOX, 8, 8, 340, 360, nullptr);
  if (a->items) {
    // 60 items so the list overflows its 360 px slot and scrolls (line-delta
    // touch-pan path).
    for (int i = 0; i < 60; ++i) {
      char li[32];
      std::snprintf(li, sizeof li, "List row %02d", i);
      a->items->add(a->s, a->list, li, nullptr);
    }
    a->items->set_selected(a->s, a->list, 0);
  }

  // --- Page 3: COMBOBOX ---------------------------------------------------
  neui_widget_t pg_combo = a->w->create(a->s, a->tv, NEUI_W_TABPAGE, 0, 0, 0, 0, nullptr);
  a->w->set_text(a->s, pg_combo, "Combo");
  neui_widget_t clbl = a->w->create(a->s, pg_combo, NEUI_W_LABEL, 8, 8, 200, 22, nullptr);
  a->w->set_text(a->s, clbl, "Pick a fruit:");
  a->combo = a->w->create(a->s, pg_combo, NEUI_W_COMBOBOX, 8, 36, 200, 30, nullptr);
  if (a->items) {
    const char* fr[] = { "Apple", "Banana", "Cherry", "Date", "Elderberry" };
    for (auto* s : fr) a->items->add(a->s, a->combo, s, nullptr);
    a->items->set_selected(a->s, a->combo, 0);
  }

  // --- Page 4: TREEVIEW ---------------------------------------------------
  neui_widget_t pg_tree = a->w->create(a->s, a->tv, NEUI_W_TABPAGE, 0, 0, 0, 0, nullptr);
  a->w->set_text(a->s, pg_tree, "Tree");
  a->treev = a->w->create(a->s, pg_tree, NEUI_W_TREEVIEW, 8, 8, 340, 360, nullptr);
  if (a->tree) {
    // Several expanded folders, each with many leaves, so the expanded tree
    // overflows its 360 px slot and scrolls (line-delta touch-pan path).
    neui_item_t root = a->tree->add(a->s, a->treev, tree_item_root, "Project", nullptr);
    for (int f = 0; f < 6; ++f) {
      char fname[32];
      std::snprintf(fname, sizeof fname, "folder-%d", f);
      neui_item_t folder = a->tree->add(a->s, a->treev, root, fname, nullptr);
      for (int leaf = 0; leaf < 8; ++leaf) {
        char lname[40];
        std::snprintf(lname, sizeof lname, "file-%d-%02d.cpp", f, leaf);
        a->tree->add(a->s, a->treev, folder, lname, nullptr);
      }
    }
  }

  // --- Page 5: scrolling SECTION ------------------------------------------
  // A SECTION with scroll_mode="vertical" whose stacked child buttons overflow
  // the body, exercising the section-kinetics touch-pan path directly (the GRID
  // path is page 1; the line-delta path is pages 2/4; TABPAGE is itself a
  // chip-less scrolling section under every page).
  neui_widget_t pg_sec = a->w->create(a->s, a->tv, NEUI_W_TABPAGE, 0, 0, 0, 0, nullptr);
  a->w->set_text(a->s, pg_sec, "Section");
  a->section = a->w->create(a->s, pg_sec, NEUI_W_SECTION, 8, 8, 340, 500, nullptr);
  if (a->attrs)
    a->attrs->set_string(a->s, a->section, NEUI_ATTR_SCROLL_MODE, "vertical");
  a->w->set_text(a->s, a->section, "Scrolling section");
  for (int i = 0; i < 40; ++i) {
    char b[24];
    std::snprintf(b, sizeof b, "Row button %02d", i);
    neui_widget_t btn = a->w->create(a->s, a->section, NEUI_W_BUTTON,
                                     8, 8 + i * 34, 300, 28, nullptr);
    a->w->set_text(a->s, btn, b);
  }

  // Status label below the tabview.
  a->status = a->w->create(a->s, a->win, NEUI_W_LABEL, 12, 752, 366, 22, nullptr);
  a->w->set_text(a->s, a->status, "Phase 2 (XPL host): tap a widget");

  p2_relayout(a);
  a->w->show(a->s, a->win);

#ifdef NEUI_IOS_DEMO_AUTOTAB
  // Verification-only: cycle the selected tab so each complex widget can be
  // screenshotted without a tap (simctl can't synthesize touches reliably).
  if (neui_tabs_api_t* tabs = (neui_tabs_api_t*)api->get_interface(a->s, NEUI_API_TABS)) {
    static neui_tabs_api_t* s_tabs = tabs;
    static neui_widget_t    s_tv   = a->tv;
    static neui_session_t   s_sess = a->s;
    for (int i = 1; i <= 3; ++i) {
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.2 * i * NSEC_PER_SEC)),
                     dispatch_get_main_queue(), ^{
        s_tabs->set_selected(s_sess, s_tv, (uint32_t)i);
      });
    }
  }
#endif

#ifdef NEUI_IOS_P2SCROLL_VERIFY
  // Verification-only (headless): simctl cannot synthesize a swipe, so prove the
  // shared-kinetics plumbing end-to-end WITHOUT a gesture - switch to the
  // scrolling-SECTION tab and drive a programmatic scroll via NEUI_API_SCROLL.
  // The committed offset moving + clamping + firing SCROLL_CHANGED confirms the
  // section scroll state is live; the touch-pan handler feeds the very same
  // commit path. A live swipe (device / Simulator mouse-drag) verifies the
  // gesture wiring itself.
  if (neui_scroll_api_t* scroll =
        (neui_scroll_api_t*)api->get_interface(a->s, NEUI_API_SCROLL)) {
    if (neui_tabs_api_t* tabs = (neui_tabs_api_t*)api->get_interface(a->s, NEUI_API_TABS))
      tabs->set_selected(a->s, a->tv, 4);   // the "Section" tab (5th)
    static neui_scroll_api_t* s_scroll = scroll;
    static neui_widget_t      s_sec    = a->section;
    static neui_session_t     s_sess2  = a->s;
    static neui_widget_api_t* s_w      = a->w;
    static neui_widget_t      s_status = a->status;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
      s_scroll->set_scroll(s_sess2, s_sec, 0, 400);   // scroll down 400 px
      int sx = 0, sy = 0;
      s_scroll->get_scroll(s_sess2, s_sec, &sx, &sy);
      char buf[96];
      std::snprintf(buf, sizeof buf, "set_scroll(0,400) -> committed y=%d", sy);
      s_w->set_text(s_sess2, s_status, buf);
    });
  }
#endif
}
#endif // NEUI_IOS_PHASE2

void build_ui()
{
#if NEUI_IOS_PHASE2
  build_phase2();
  return;
#endif
  neui_init();
#if NEUI_IOS_USE_NATIVE_HOST
  neui_api_t* api = neui_get_api("neui.host.ios");
  if (!api) api = neui_get_api("neui.host.crossplatform");
#else
  neui_api_t* api = neui_get_api("neui.host.crossplatform");
#endif
  if (!api) api = neui_get_api(nullptr);
  if (!api) return;

  g_app.s     = api->create_session(&g_client, &g_app);
  g_app.w     = (neui_widget_api_t*)    api->get_interface(g_app.s, NEUI_API_WIDGETS);
  g_app.attrs = (neui_attr_api_t*)      api->get_interface(g_app.s, NEUI_API_ATTRS);
  g_app.clip   = (neui_clipboard_api_t*) api->get_interface(g_app.s, NEUI_API_CLIPBOARD);
  g_app.tree   = (neui_tree_api_t*)      api->get_interface(g_app.s, NEUI_API_TREE);
  g_app.notify = (neui_notify_api_t*)    api->get_interface(g_app.s, NEUI_API_NOTIFY);
  g_app.grid   = (neui_grid_api_t*)      api->get_interface(g_app.s, NEUI_API_GRID);
  g_app.tabs   = (neui_tabs_api_t*)      api->get_interface(g_app.s, NEUI_API_TABS);
  g_app.items  = (neui_items_api_t*)     api->get_interface(g_app.s, NEUI_API_ITEMS);
  g_app.metrics = (neui_metrics_api_t*)  api->get_interface(g_app.s, NEUI_API_METRICS);
  g_app.dnd      = (neui_dnd_api_t*)      api->get_interface(g_app.s, NEUI_API_DND);
  g_app.behavior = (neui_behavior_api_t*) api->get_interface(g_app.s, NEUI_API_BEHAVIOR);
  g_app.assets   = (neui_asset_api_t*)    api->get_interface(g_app.s, NEUI_API_ASSETS);
  if (g_app.metrics)
    std::printf("[neui-ios] metrics: ui_scale=%.3f control_h=%d margin=%d body_font=%d\n",
                (double)g_app.metrics->ui_scale(g_app.s),
                g_app.metrics->metric(g_app.s, NEUI_METRIC_CONTROL_HEIGHT),
                g_app.metrics->metric(g_app.s, NEUI_METRIC_MARGIN),
                g_app.metrics->metric(g_app.s, NEUI_METRIC_BODY_FONT_SIZE));
  if (!g_app.w) return;

  // The APPWINDOW's (width, height) is a seed; platform_create_appwindow
  // re-seeds it from the connected UIWindowScene's bounds (the device area), so
  // the frame fills the screen. A RESIZE event then drives relayout() on every
  // subsequent size change.
  g_app.win = g_app.w->create(g_app.s, widget_none, NEUI_W_APPWINDOW,
                              0, 0, 390, 800, nullptr);
  // Opt into system dark/light: paints with the theme palette and repaints on a
  // live appearance flip (NEUIView::traitCollectionDidChange:).
  if (g_app.attrs)
    g_app.attrs->set_int(g_app.s, g_app.win, NEUI_ATTR_FOLLOW_SYSTEM_THEME, 1);

  // MENUBAR -> on iOS the xpl host shows a hamburger button in the top inset
  // band that opens this tree as a native UIMenu popover. The same model drives
  // the Linux in-frame band, the macOS NSMenu, and the Win32 HMENU.
  if (g_app.tree) {
    g_app.mb = g_app.w->create(g_app.s, g_app.win, NEUI_W_MENUBAR, 0, 0, 0, 0, nullptr);
    // Log the system-menu-bar verdict so a sim run can confirm the iPad idiom is
    // detected (the host's menu_ios_system_menubar_available() uses this same
    // idiom test: iPad -> contribute the menubar tree via buildMenuWithBuilder:
    // and hide the hamburger; iPhone -> hamburger path).
    BOOL is_pad = (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad);
    NSLog(@"[neui] system menu bar expected = %d (idiom=%ld, iPad)",
          (int)is_pad, (long)UIDevice.currentDevice.userInterfaceIdiom);

    neui_item_t file = g_app.tree->add(g_app.s, g_app.mb, tree_item_root, "File", nullptr);
    g_app.mi_file_new = g_app.tree->add(g_app.s, g_app.mb, file, "New", nullptr);
    g_app.tree->set_shortcut(g_app.s, g_app.mb, g_app.mi_file_new, NEUI_KMOD_CTRL, NEUI_KEY_N);
    g_app.tree->add(g_app.s, g_app.mb, file, "Open", nullptr);

    neui_item_t edit = g_app.tree->add(g_app.s, g_app.mb, tree_item_root, "Edit", nullptr);
    neui_item_t copy = g_app.tree->add(g_app.s, g_app.mb, edit, "Copy", nullptr);
    // Bind Copy to the built-in command (routes to the focused field editor).
    g_app.tree->set_menu_cmd(g_app.s, g_app.mb, copy, NEUI_CMD_COPY);
    g_app.tree->set_shortcut(g_app.s, g_app.mb, copy, NEUI_KMOD_CTRL, NEUI_KEY_C);

    neui_item_t view = g_app.tree->add(g_app.s, g_app.mb, tree_item_root, "View", nullptr);
    // An observable client item: activation rewrites the LABEL text.
    g_app.mi_view_msg = g_app.tree->add(g_app.s, g_app.mb, view, "Say Hello", nullptr);
    // Notify-API items: fire the shared toast overlay + present a UIAlertController.
    g_app.mi_view_toast = g_app.tree->add(g_app.s, g_app.mb, view, "Show Toast", nullptr);
    g_app.mi_view_alert = g_app.tree->add(g_app.s, g_app.mb, view, "Show Message Box", nullptr);
  }

  // A SECTION groups the controls visually; relayout() sizes it to the frame.
  g_app.sec = g_app.w->create(g_app.s, g_app.win, NEUI_W_SECTION,
                              16, 48, 358, 700, nullptr);
#if NEUI_IOS_USE_NATIVE_HOST
  g_app.w->set_text(g_app.s, g_app.sec, "neui on iOS (native host)");
#else
  g_app.w->set_text(g_app.s, g_app.sec, "neui on iOS (XPL host)");
#endif

  g_app.input  = g_app.w->create(g_app.s, g_app.sec, NEUI_W_INPUTBOX,  16, 16, 326, 36, nullptr);
  g_app.button = g_app.w->create(g_app.s, g_app.sec, NEUI_W_BUTTON,    16, 64, 120, 40, nullptr);
  g_app.w->set_text(g_app.s, g_app.button, "Submit");

  // 2-state CHECKBOX. On the native iOS host this renders as a green UISwitch
  // by DEFAULT (iOS-Settings-style label + toggle). To opt back into the
  // square-glyph style, set the per-session override BEFORE creating it:
  //   g_app.attrs->set_session_int(g_app.s, NEUI_IOS_CHECKBOX_STYLE,
  //                                NEUI_IOS_CHECKBOX_GLYPH);
  // (read once at create; changing it later only affects later checkboxes.)
  g_app.check  = g_app.w->create(g_app.s, g_app.sec, NEUI_W_CHECKBOX,  16, 116, 200, 32, nullptr);
  g_app.w->set_text(g_app.s, g_app.check, "Enable feature");
  // Default it on so the green switch is immediately visible.
  g_app.w->set_check(g_app.s, g_app.check, NEUI_CHECK_CHECKED);

  // 3-state CHECKBOX3: a UISwitch has no indeterminate state, so a tri-state
  // checkbox ALWAYS uses the square-glyph control - it ignores the session
  // style. This makes the switch-vs-glyph distinction visible side by side.
  g_app.check3 = g_app.w->create(g_app.s, g_app.sec, NEUI_W_CHECKBOX3, 16, 152, 200, 32, nullptr);
  g_app.w->set_text(g_app.s, g_app.check3, "Tri-state (glyph)");

  g_app.slider = g_app.w->create(g_app.s, g_app.sec, NEUI_W_SLIDER,    16, 188, 326, 32, nullptr);
  if (g_app.attrs) g_app.attrs->set_float(g_app.s, g_app.slider, NEUI_PARAM_VALUE, 0.5f);

  g_app.label  = g_app.w->create(g_app.s, g_app.sec, NEUI_W_LABEL,     16, 236, 326, 28, nullptr);
  g_app.w->set_text(g_app.s, g_app.label, "Tap, type, toggle, slide.");

  // -------------------------------------------------------------------------
  // DRAG & DROP demo. Two CUSTOMDRAW widgets (placeholder geometry; relayout()
  // positions them): a drag source carrying a text DataItem (made draggable by
  // a DRAG_SOURCE behavior asset - the iOS way, since begin_drag is a no-op),
  // and a drop target accepting text / uri-list / png. Drag the source onto the
  // target (long-press-drag) for an internal drag; drag text from Notes or a URL
  // from Safari onto the target for an external drag.
  g_app.dnd_src    = g_app.w->create(g_app.s, g_app.sec, NEUI_W_CUSTOMDRAW, 16, 276, 150, 60, nullptr);
  g_app.dnd_target = g_app.w->create(g_app.s, g_app.sec, NEUI_W_CUSTOMDRAW, 176, 276, 166, 60, nullptr);
  g_app.dnd_status = g_app.w->create(g_app.s, g_app.sec, NEUI_W_LABEL,      16, 344, 326, 28, nullptr);
  g_app.w->set_text(g_app.s, g_app.dnd_status, "Drag the blue tile onto the green one.");

  // Build the text DataItem the source vends, stash its id in the source's attr
  // bag under "demo.drag.item" (the DRAG_SOURCE handler's drag_data_key).
  if (g_app.clip && g_app.attrs) {
    g_app.dnd_item = g_app.clip->create_item(g_app.s);
    const char* payload = "Hello from a neui iOS drag source!";
    g_app.clip->item_set_format(g_app.s, g_app.dnd_item, NEUI_MIME_TEXT,
                                payload, (uint32_t)std::strlen(payload) + 1);
    g_app.attrs->set_int(g_app.s, g_app.dnd_src, "demo.drag.item",
                         (int)g_app.dnd_item.id);
  }
  // Attach a DRAG_SOURCE behavior to the source CUSTOMDRAW.
  if (g_app.behavior && g_app.assets) {
    g_app.dnd_behav = g_app.assets->create_behavior(g_app.s);
    neui_behavior_handler_t h =
        g_app.behavior->add_handler(g_app.s, g_app.dnd_behav,
                                    NEUI_BEHAVIOR_KIND_DRAG_SOURCE);
    g_app.behavior->set_string(g_app.s, g_app.dnd_behav, h,
                               "drag_data_key", "demo.drag.item");
    g_app.behavior->set_int(g_app.s, g_app.dnd_behav, h, "allowed_actions",
                            NEUI_DND_ACTION_COPY | NEUI_DND_ACTION_MOVE);
    // result_attr: the runtime writes the negotiated action here + fires
    // ATTR_CHANGED when the drag completes (we reflect it into dnd_status).
    g_app.behavior->set_string(g_app.s, g_app.dnd_behav, h,
                               "result_attr", "demo.drag.result");
    g_app.w->set_asset(g_app.s, g_app.dnd_src, g_app.dnd_behav);
  }
  // Mark the target a drop target accepting the three demo MIMEs.
  if (g_app.dnd) {
    g_app.dnd->set_drop_target(g_app.s, g_app.dnd_target, true);
    const char* mimes[] = { NEUI_MIME_TEXT, NEUI_MIME_URI_LIST, NEUI_MIME_PNG };
    g_app.dnd->set_accepted_formats(g_app.s, g_app.dnd_target, mimes, 3);
  }

  // IMAGE widget loading a bundled PNG (resolved through image_loader_ios.h's
  // bundle fallback). The plain filename works because CMake copies myimage.png
  // into the .app's Resources root.
  g_app.image = g_app.w->create(g_app.s, g_app.sec, NEUI_W_IMAGE, 16, 276, 326, 150, nullptr);
  g_app.w->set_text(g_app.s, g_app.image, "myimage.png");

#if NEUI_IOS_USE_NATIVE_HOST
  // Native-host TABVIEW (phase-2): a chip strip with three NEUI_W_TABPAGE pages,
  // each carrying a few body-relative widgets. A chip TAP selects a tab (fires
  // TAB_DESELECTED/SELECTED, swaps which page is visible); the GRID lives in the
  // middle page so the native GRID port composes inside a tab. relayout() sizes
  // the tabview into the section's lower body area. Only built on the native
  // host - the xpl host already exercises TABVIEW via the NEUI_IOS_PHASE2 screen.
  // (placeholder geometry; relayout() positions it for real before show.)
  g_app.tv = g_app.w->create(g_app.s, g_app.sec, NEUI_W_TABVIEW, 16, 414, 326, 240, nullptr);

  // --- Page 1: "Info" (a label + a button) --------------------------------
  neui_widget_t pg_info = g_app.w->create(g_app.s, g_app.tv, NEUI_W_TABPAGE, 0, 0, 0, 0, nullptr);
  g_app.w->set_text(g_app.s, pg_info, "Info");
  g_app.tab_lbl = g_app.w->create(g_app.s, pg_info, NEUI_W_LABEL, 16, 16, 280, 28, nullptr);
  g_app.w->set_text(g_app.s, g_app.tab_lbl, "Tab 1 of 3 - tap a chip above.");
  neui_widget_t tab_btn = g_app.w->create(g_app.s, pg_info, NEUI_W_BUTTON, 16, 52, 140, 40, nullptr);
  g_app.w->set_text(g_app.s, tab_btn, "Page 1 button");

  // Native-host COMBOBOX (UIButton + UIMenu pull-down) on the visible default
  // tab so it renders without navigation. A few items + a default selection;
  // ITEM_SELECTED is reflected into the page-1 status label. (No COMBO_* attrs:
  // the UIMenu auto-sizes its pop-out, like NSPopUpButton.)
  g_app.combo_w = g_app.w->create(g_app.s, pg_info, NEUI_W_COMBOBOX, 16, 100, 200, 36, nullptr);
  if (g_app.items) {
    static const char* opts[] = { "Small", "Medium", "Large", "Extra Large" };
    for (const char* o : opts)
      g_app.items->add(g_app.s, g_app.combo_w, o, nullptr);
    g_app.items->set_selected(g_app.s, g_app.combo_w, 1);   // default: "Medium"
  }

  // --- Page 2: "Grid" (the native GRID) -----------------------------------
  neui_widget_t pg_grid = g_app.w->create(g_app.s, g_app.tv, NEUI_W_TABPAGE, 0, 0, 0, 0, nullptr);
  g_app.w->set_text(g_app.s, pg_grid, "Grid");
  if (g_app.grid) {
    // The GRID is a child of the page; relayout() sizes it to the page body.
    g_app.grid_w = g_app.w->create(g_app.s, pg_grid, NEUI_W_GRID, 16, 16, 280, 180, nullptr);
    g_app.grid->add_column(g_app.s, g_app.grid_w, "Name", 150);
    g_app.grid->add_column(g_app.s, g_app.grid_w, "Kind", 90);
    g_app.grid->add_column(g_app.s, g_app.grid_w, "Size", 80);
    static const char* kinds[] = { "file", "dir", "link" };
    static const char* sizes[] = { "12 KB", "-", "4 KB", "880 B", "1 MB", "2.3 MB" };
    for (int i = 0; i < 120; ++i) {
      char name[32];
      std::snprintf(name, sizeof name, "item-%03d", i);
      const char* v[] = { name, kinds[i % 3], sizes[i % 6], nullptr };
      g_app.grid->add_row(g_app.s, g_app.grid_w, v);
    }
  }

  // --- Page 3: "List" (the native LISTBOX, a UITableView) -----------------
  // Enough rows to overflow the page so native UITableView scrolling/momentum
  // is demonstrable. A row tap fires ITEM_SELECTED -> page-1 status label.
  neui_widget_t pg_list = g_app.w->create(g_app.s, g_app.tv, NEUI_W_TABPAGE, 0, 0, 0, 0, nullptr);
  g_app.w->set_text(g_app.s, pg_list, "List");
  g_app.list_w = g_app.w->create(g_app.s, pg_list, NEUI_W_LISTBOX, 16, 16, 280, 180, nullptr);
  if (g_app.items) {
    static const char* fruits[] = { "Apple", "Banana", "Cherry", "Date",
                                    "Elderberry", "Fig", "Grape" };
    for (int i = 0; i < 40; ++i) {
      char li[40];
      std::snprintf(li, sizeof li, "%02d  %s", i, fruits[i % 7]);
      g_app.items->add(g_app.s, g_app.list_w, li, nullptr);
    }
    g_app.items->set_selected(g_app.s, g_app.list_w, 0);
  }

  // --- Page 4: "Tree" (the native TREEVIEW, a UITableView) ----------------
  // A few folders with leaves; the first folder starts expanded (set_selected
  // on a child forces nothing - expand is via tapping the parent row). Enough
  // depth/rows that an expanded tree scrolls. Tap a parent to expand/collapse.
  neui_widget_t pg_tree = g_app.w->create(g_app.s, g_app.tv, NEUI_W_TABPAGE, 0, 0, 0, 0, nullptr);
  g_app.w->set_text(g_app.s, pg_tree, "Tree");
  g_app.tree_w = g_app.w->create(g_app.s, pg_tree, NEUI_W_TREEVIEW, 16, 16, 280, 180, nullptr);
  if (g_app.tree) {
    for (int f = 0; f < 5; ++f) {
      char fname[32];
      std::snprintf(fname, sizeof fname, "Folder %d", f);
      neui_item_t folder = g_app.tree->add(g_app.s, g_app.tree_w, tree_item_root, fname, nullptr);
      for (int leaf = 0; leaf < 6; ++leaf) {
        char lname[40];
        std::snprintf(lname, sizeof lname, "file-%d-%02d.txt", f, leaf);
        g_app.tree->add(g_app.s, g_app.tree_w, folder, lname, nullptr);
      }
    }
  }

  // --- Page 5: "More" (a label + a slider) --------------------------------
  neui_widget_t pg_more = g_app.w->create(g_app.s, g_app.tv, NEUI_W_TABPAGE, 0, 0, 0, 0, nullptr);
  g_app.w->set_text(g_app.s, pg_more, "More");
  neui_widget_t more_lbl = g_app.w->create(g_app.s, pg_more, NEUI_W_LABEL, 16, 16, 280, 28, nullptr);
  g_app.w->set_text(g_app.s, more_lbl, "A slider on the last tab:");
  neui_widget_t more_sl = g_app.w->create(g_app.s, pg_more, NEUI_W_SLIDER, 16, 52, 280, 32, nullptr);
  if (g_app.attrs) g_app.attrs->set_float(g_app.s, more_sl, NEUI_PARAM_VALUE, 0.3f);
#endif

  // Initial layout against the scene-seeded client rect, then show. A RESIZE
  // (incl. the first safe-area inset resolve) re-runs relayout() afterwards.
  relayout(&g_app);

  g_app.w->show(g_app.s, g_app.win);

  // Smoke the clipboard seam now that the session is live.
  clipboard_smoke(&g_app);

  // Fire a toast automatically a moment after the UI builds so the M5 toast
  // path (CADisplayLink heartbeat + Session::paint_toast) is exercised and
  // screenshot-able without needing a tap. Delayed ~0.8 s so the frame has laid
  // out + the safe-area inset has resolved (the toast anchors below it).
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.8 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
    if (g_app.notify && g_app.w)
      g_app.notify->toast(g_app.s, g_app.win,
                          "Welcome to neui on iOS!\nThis toast auto-fired at launch.");
  });

#ifdef NEUI_IOS_DEMO_SELECTTAB
  // Verification-only (headless): simctl can't tap a chip, so programmatically
  // select the native TABVIEW tab named by NEUI_IOS_DEMO_SELECTTAB (an index)
  // so the LISTBOX ("List", tab 3) or TREEVIEW ("Tree", tab 4) page is visible
  // for a screenshot. A live tap verifies the chip wiring + row interaction.
  if (g_app.tabs && g_app.tv.id != 0) {
    static neui_tabs_api_t*   s_tabs = g_app.tabs;
    static neui_widget_t      s_tv   = g_app.tv;
    static neui_session_t     s_sess = g_app.s;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
      s_tabs->set_selected(s_sess, s_tv, (uint32_t)(NEUI_IOS_DEMO_SELECTTAB));
    });
  }
#endif

#ifdef NEUI_IOS_DEMO_AUTOALERT
  // Verification-only: auto-present the message box after the toast clears so a
  // screenshot can capture the UIAlertController. Compiled out of the normal
  // build (the menu's "Show Message Box" item is the real trigger).
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3.4 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
    if (g_app.notify && g_app.w)
      g_app.notify->message_box(g_app.s, g_app.win,
                                "This is a neui message box on iOS.", "Message",
                                NEUI_MB_OKCANCEL | NEUI_MB_ICONINFORMATION);
  });
#endif

  // NOTE: deliberately NOT calling api->run(g_app.s) - UIApplicationMain owns
  // the run loop on iOS.
}

} // namespace

// ---------------------------------------------------------------------------

@implementation SceneDelegate

- (void)scene:(UIScene*)scene
    willConnectToSession:(UISceneSession*)session
                 options:(UISceneConnectionOptions*)connectionOptions
{
  (void)session;
  (void)connectionOptions;
  if (![scene isKindOfClass:[UIWindowScene class]]) return;
  // The scene is connected (and foreground-activating) now, so neui's
  // platform_create_appwindow can bind the frame's UIWindow to it.
  build_ui();
}

@end
