// popup_surface_example - NEUI_W_POPUPSURFACE: an overlay that leaves its frame.
//
// The point of the example is the SIZE MISMATCH, so the window is deliberately
// small (520x300) and the picker it opens is 640x520 - bigger than its own owner
// in both axes. On a platform with the desktop backing the picker is a real
// borderless window hanging over the desktop; where there is none it is clamped
// into the frame instead, which is what the reported clamp box is for.
//
// What to try:
//   - "Open FX picker"  -> an oversized popup, anchored BELOW the button. Note it
//                          extends past the window edges, and that it flips ABOVE
//                          the button if you drag the window near the screen
//                          bottom first.
//   - a row in it       -> a cascade level to the RIGHT of the picker, which
//                          flips LEFT at the screen edge. It carries a real
//                          BUTTON widget, because a popup surface is a frame and
//                          ordinary widgets work inside it.
//   - right-click       -> the same primitive with NEUI_POPUP_AT_POINTER.
//   - click the window  -> dismisses, and the press is SWALLOWED (the button
//                          underneath does not fire).
//   - Escape            -> dismisses.
//   - click another app -> dismisses. Click back: the popup is gone, not
//                          stranded behind the window.
//
// The status line under the button reports the last dismissal reason and the
// clamp box the host offered, i.e. the two facts a client actually consumes.
//
// Crossplatform host only: NEUI_API_POPUP is an xpl-host interface, and on
// win32 / macOS neui_get_api(NULL) would hand back the NATIVE host first.

#include "neui/neui.h"

#include <stdio.h>
#include <string.h>

namespace {

struct AppState
{
  neui_session_t      sess{};
  neui_widget_api_t*  widgets  = nullptr;
  neui_popup_api_t*   popup    = nullptr;

  neui_widget_t win{};
  neui_widget_t open_button{};
  neui_widget_t status{};        // LABEL: last dismissal + clamp box
  neui_widget_t hit_counter{};   // LABEL: proves the swallowed press never lands
  neui_widget_t backdrop{};      // full-size CUSTOMDRAW: the click target for the window

  neui_widget_t picker{};        // level 0 - the oversized one
  neui_widget_t picker_rows{};   // CUSTOMDRAW filling the picker
  neui_widget_t detail{};        // level 1 - the cascade
  neui_widget_t detail_body{};
  neui_widget_t detail_close{};  // a real BUTTON inside a popup

  int  button_hits = 0;
  int  hovered_row = -1;

  static const int kRowH  = 34;
  static const int kRows  = 15;   // 15 * 34 = 510, + 10 padding = 520
};

AppState g_app;

// ---------------------------------------------------------------------------
// Painting

void paint_picker(neui_event_paint_t& p)
{
  auto* pa = p.painter_api;
  auto* h  = p.p;

  pa->fill_rect(h, 0, 0, p.width, p.height, 0xFF23262Bu);
  pa->draw_rect(h, 0, 0, p.width, p.height, 1.0f, 0xFF4A505Au);

  pa->draw_text(h, 12, 6, p.width - 24, 22,
                "FX picker - 640 x 520, opened from a 520 x 300 window",
                12.0f, 0xFF9AA4B2u);

  const float row_top = 30.0f;
  for (int i = 0; i < AppState::kRows; ++i) {
    const float y = row_top + static_cast<float>(i * AppState::kRowH);
    if (y + AppState::kRowH > p.height) break;
    const bool hot = (i == g_app.hovered_row);
    if (hot)
      pa->fill_rect(h, 4, y, p.width - 8, static_cast<float>(AppState::kRowH),
                    0xFF3C4654u);

    char label[64];
    snprintf(label, sizeof(label), "Effect slot %02d", i + 1);
    pa->draw_text(h, 16, y, p.width - 60,
                  static_cast<float>(AppState::kRowH),
                  label, 14.0f, hot ? 0xFFFFFFFFu : 0xFFD2D8E0u);
    // Submenu chevron - these rows cascade.
    pa->draw_text(h, p.width - 26, y, 18,
                  static_cast<float>(AppState::kRowH),
                  ">", 14.0f, 0xFF8892A0u);
  }
}

void paint_detail(neui_event_paint_t& p)
{
  auto* pa = p.painter_api;
  auto* h  = p.p;
  pa->fill_rect(h, 0, 0, p.width, p.height, 0xFF2B3038u);
  pa->draw_rect(h, 0, 0, p.width, p.height, 1.0f, 0xFF5A6270u);
  pa->draw_text(h, 12, 10, p.width - 24, 20, "Cascade level 2", 13.0f,
                0xFFFFFFFFu);
  pa->draw_text(h, 12, 34, p.width - 24, 40,
                "A real BUTTON lives below - a popup surface is a frame, so "
                "ordinary widgets work inside it.",
                11.0f, 0xFF9AA4B2u);
}

// ---------------------------------------------------------------------------
// Status reporting - the two facts a client consumes

void refresh_status(const char* last_dismissal)
{
  int cw = 0, ch = 0;
  g_app.popup->get_clamp_size(g_app.sess, g_app.open_button, &cw, &ch);
  const bool escapes = g_app.popup->escapes_frame(g_app.sess, g_app.open_button);

  char buf[192];
  snprintf(buf, sizeof(buf), "clamp box %d x %d  (%s)   last dismissal: %s",
           cw, ch, escapes ? "own window" : "clamped to frame",
           last_dismissal);
  g_app.widgets->set_text(g_app.sess, g_app.status, buf);
}

const char* reason_name(uint32_t reason)
{
  switch (reason) {
  case NEUI_POPUP_DISMISS_OUTSIDE_PRESS: return "outside press";
  case NEUI_POPUP_DISMISS_DEACTIVATED:   return "deactivated";
  case NEUI_POPUP_DISMISS_OWNER_MOVED:   return "owner moved / resized";
  case NEUI_POPUP_DISMISS_ESCAPE:        return "Escape";
  case NEUI_POPUP_DISMISS_CLIENT:        return "client close";
  case NEUI_POPUP_DISMISS_CASCADE:       return "cascade";
  default:                               return "?";
  }
}

// ---------------------------------------------------------------------------
// Events

bool NEUI_ABI on_event(void*, neui_event_t* e)
{
  auto* w = g_app.widgets;

  switch (e->type) {
  case NEUI_EVENT_APP_QUIT:
    return true;

  case NEUI_EVENT_WIDGET_PAINT:
    if (e->data.paint.widget.id == g_app.picker_rows.id) {
      paint_picker(e->data.paint);
      return true;
    }
    if (e->data.paint.widget.id == g_app.detail_body.id) {
      paint_detail(e->data.paint);
      return true;
    }
    if (e->data.paint.widget.id == g_app.backdrop.id)
      return true;   // draws nothing; it exists to be clicked
    break;

  case NEUI_EVENT_MOUSE_MOVE:
    // Row hover inside the popup. Worth watching: this keeps working while the
    // pointer is over the popup's own window, and the widgets in the OWNER stay
    // un-hovered the whole time - that suppression is what makes it read as a
    // menu rather than as a second window that happens to be on top.
    if (e->data.mouse.widget.id == g_app.picker_rows.id) {
      const int y   = e->data.mouse.y - 30;
      const int row = (y < 0) ? -1 : y / AppState::kRowH;
      if (row != g_app.hovered_row) {
        g_app.hovered_row = (row >= 0 && row < AppState::kRows) ? row : -1;
        w->invalidate(g_app.sess, g_app.picker_rows);
      }
      return true;
    }
    break;

  case NEUI_EVENT_MOUSE_BUTTON_CLICK:
    if (e->data.mouse.widget.id == g_app.open_button.id) {
      // Anchor + offset + side. No screen coordinates anywhere in the API: the
      // host converts, clamps and flips.
      g_app.popup->open(g_app.sess, g_app.picker, g_app.open_button,
                        0, 2, NEUI_POPUP_BELOW);
      refresh_status("-");
      return true;
    }
    if (e->data.mouse.widget.id == g_app.picker_rows.id) {
      if (g_app.hovered_row >= 0) {
        // Cascade: level 1 is anchored to the ROW's widget, opening to the RIGHT
        // and flipping LEFT at the screen edge. Ownership stays flat - both
        // levels are owned by the window, not by each other.
        g_app.popup->open(g_app.sess, g_app.detail, g_app.picker_rows,
                          0, g_app.hovered_row * AppState::kRowH + 30,
                          NEUI_POPUP_RIGHT);
      }
      return true;
    }
    if (e->data.mouse.widget.id == g_app.detail_close.id) {
      g_app.popup->close_all(g_app.sess);
      return true;
    }
    break;

  case NEUI_EVENT_MOUSE_RBUTTON_DOWN:
    // The same primitive as a context menu. AT_POINTER ignores the anchor rect
    // and opens where the cursor is.
    if (e->data.mouse.widget.id == g_app.backdrop.id) {
      g_app.popup->open(g_app.sess, g_app.detail, g_app.backdrop, 0, 0,
                        NEUI_POPUP_AT_POINTER);
      return true;
    }
    break;

  case NEUI_EVENT_POPUP_DISMISSED:
    // One place to drop the state a popup was opened with, for every route out -
    // outside press, deactivation, Escape, owner moved, or our own close_all.
    if (e->data.popup.widget.id == g_app.picker.id) g_app.hovered_row = -1;
    refresh_status(reason_name(e->data.popup.reason));
    return true;

  default:
    break;
  }

  // The press that dismisses a popup must NOT also actuate what is underneath.
  // This counter is the proof: click the window while the picker is open and it
  // does not move.
  if (e->type == NEUI_EVENT_MOUSE_BUTTON_DOWN &&
      e->data.mouse.widget.id == g_app.backdrop.id)
  {
    char buf[64];
    snprintf(buf, sizeof(buf), "presses reaching the window: %d",
             ++g_app.button_hits);
    w->set_text(g_app.sess, g_app.hit_counter, buf);
    return true;
  }
  return false;
}

void* NEUI_ABI get_interface(void*, const char* iface)
{
  static neui_widget_client_t wc;
  if (!strcmp(iface, NEUI_API_WIDGETS)) {
    wc.neui_version = NEUI_VERSION;
    wc.ondestroy    = nullptr;
    wc.onevent      = on_event;
    return &wc;
  }
  return nullptr;
}

} // namespace

int main(int, char*[])
{
  neui_init();
  // Explicitly the crossplatform host: NEUI_API_POPUP is xpl-only, and taking
  // the default on win32 / macOS would hand back the native host.
  neui_api_t* host = neui_get_api("neui.host.crossplatform");
  if (!host) return 1;

  neui_client_t client;
  client.neui_version  = NEUI_VERSION;
  client.get_interface = get_interface;

  g_app.sess = host->create_session(&client, &g_app);
  if (!g_app.sess.session) return 1;

  g_app.widgets = (neui_widget_api_t*) host->get_interface(g_app.sess,
                                                          NEUI_API_WIDGETS);
  g_app.popup   = (neui_popup_api_t*)  host->get_interface(g_app.sess,
                                                          NEUI_API_POPUP);
  if (!g_app.widgets || !g_app.popup) return 1;
  auto* w = g_app.widgets;
  const neui_widget_t none = { UINT32_MAX };

  // Deliberately smaller than the popup it opens.
  g_app.win = w->create(g_app.sess, none, NEUI_W_APPWINDOW,
                        160, 140, 520, 300, nullptr);
  w->set_text(g_app.sess, g_app.win,
              "neui popup surface - the picker is bigger than this window");
  // A full-size backdrop child, created FIRST so it sits behind everything.
  // A FRAME is never a hit-test result on this host (the walk only ever returns
  // children, and a frame carries a native handle), so "click anywhere in the
  // window" needs a real widget to land on - this is what makes the right-click
  // AT_POINTER demo and the swallowed-press counter work at all.
  g_app.backdrop = w->create(g_app.sess, g_app.win, NEUI_W_CUSTOMDRAW,
                             0, 0, 520, 300, nullptr);

  neui_widget_t intro = w->create(g_app.sess, g_app.win, NEUI_W_LABEL,
                                  16, 16, 480, 20, nullptr);
  w->set_text(g_app.sess, intro,
              "A popup surface may leave its frame. Right-click for AT_POINTER.");

  g_app.open_button = w->create(g_app.sess, g_app.win, NEUI_W_BUTTON,
                                16, 52, 160, 30, nullptr);
  w->set_text(g_app.sess, g_app.open_button, "Open FX picker");

  g_app.status = w->create(g_app.sess, g_app.win, NEUI_W_LABEL,
                           16, 96, 490, 20, nullptr);
  g_app.hit_counter = w->create(g_app.sess, g_app.win, NEUI_W_LABEL,
                                16, 120, 490, 20, nullptr);
  w->set_text(g_app.sess, g_app.hit_counter, "presses reaching the window: 0");

  // ---- The popup surfaces. Created like any other frame (parent widget_none),
  // never shown with widgets->show - NEUI_API_POPUP::open places them.
  g_app.picker = w->create(g_app.sess, none, NEUI_W_POPUPSURFACE,
                           0, 0, 640, 520, nullptr);
  g_app.picker_rows = w->create(g_app.sess, g_app.picker, NEUI_W_CUSTOMDRAW,
                                0, 0, 640, 520, nullptr);

  g_app.detail = w->create(g_app.sess, none, NEUI_W_POPUPSURFACE,
                           0, 0, 260, 150, nullptr);
  g_app.detail_body = w->create(g_app.sess, g_app.detail, NEUI_W_CUSTOMDRAW,
                                0, 0, 260, 150, nullptr);
  g_app.detail_close = w->create(g_app.sess, g_app.detail, NEUI_W_BUTTON,
                                 12, 100, 120, 28, nullptr);
  w->set_text(g_app.sess, g_app.detail_close, "Close all");

  w->show(g_app.sess, g_app.win);
  refresh_status("-");
  host->run(g_app.sess);
  return 0;
}
