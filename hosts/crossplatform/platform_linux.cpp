// X11 + Cairo platform layer for the crossplatform host on Linux.
//
// Models hosts/crossplatform/platform_macos.mm (software pressed-widget
// tracking, no OS pointer grab; overlay pre-checks; the same Session call
// shapes) cross-checked against platform_win32.cpp (physical-pixel
// create_context / resize, logical-pixel widget coords, the pump/modal
// shape). Cairo renders into an in-memory surface the backend blits to the
// window in end_frame; this file owns window creation + the X11 event loop.
//
// Standalone mode (this phase): one process-global Display connection, an
// appwindow count that drives quit-on-last-close, and a select() loop over
// the X connection fd plus a 16 ms timerfd heartbeat for animation.
//
// Coordinate model: X delivers physical device pixels; neui widget coords are
// logical px at 96 DPI. Every input coordinate is divided by the per-window
// scale (dpi/96) before reaching the Session; the backend multiplies logical
// draws back up by the same scale.

#include "host.h"
#include "platform.h"
#include <neui/neui.h>

#include "../shared/clipboard_item.h"
#include "../shared/linux/keys_linux.h"
#include "../shared/linux/clipboard_linux.h"
#include "../../backends/cairo/cairo_backend.h"

// This TU emits the single stb_image implementation for the Linux host.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_DEPRECATED
#include "../shared/linux/image_loader_linux.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>

#include <sys/select.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <time.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace xpl_host
{
namespace
{
  // ---- Process-global standalone state -------------------------------------
  Display* g_display          = nullptr;
  int      g_appwindow_count  = 0;
  int      g_timerfd          = -1;
  XIM      g_xim              = nullptr;
  bool     g_quit             = false;

  Atom g_wm_delete    = None;   // WM_DELETE_WINDOW
  Atom g_wm_protocols = None;
  Atom g_net_wm_name  = None;
  Atom g_utf8_string  = None;
  Atom g_motif_hints  = None;

  // CLIPBOARD-selection owner/requestor (X11 selections, hosts/shared/linux).
  neui_detail::ClipboardX11 g_clipboard;

  // Per-frame window record, stashed on WidgetData::native_handle.
  struct LinuxWindow
  {
    Display*     dpy          = nullptr;   // == g_display for standalone; own connection when embedded
    bool         owns_display = false;     // true => embedded (dedicated XOpenDisplay)
    uint64_t     last_tick_ms = 0;         // 16 ms gate for host-driven embedded ticks
    Window       win          = 0;
    Visual*      visual       = nullptr;
    int          depth        = 0;
    XIC          ic           = nullptr;   // per-window input context (UTF-8)
    Session*     session      = nullptr;
    uint32_t     widget_index = 0;
    bool         is_appwindow = false;
    bool         counted      = false;     // contributes to g_appwindow_count
    uint32_t     dpi          = 96;
    bool         needs_paint  = false;
    bool         toast_anim   = false;
    // Double-click synthesis (X has no native double-click).
    Time         last_click_time   = 0;
    int          last_click_x       = 0;
    int          last_click_y       = 0;
    unsigned int last_click_button  = 0;
  };

  std::unordered_map<Window, LinuxWindow*> g_windows;

  LinuxWindow* find_window(Window w)
  {
    auto it = g_windows.find(w);
    return it == g_windows.end() ? nullptr : it->second;
  }

  // ---- Small helpers --------------------------------------------------------

  int utf8_decode(const unsigned char* p, uint32_t* out)
  {
    unsigned char c = p[0];
    if (c < 0x80) { *out = c; return 1; }
    if ((c >> 5) == 0x6) {
      if ((p[1] & 0xC0) != 0x80) return -1;
      *out = ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu); return 2;
    }
    if ((c >> 4) == 0xE) {
      if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return -1;
      *out = ((c & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
      return 3;
    }
    if ((c >> 3) == 0x1E) {
      if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80)
        return -1;
      *out = ((c & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) |
             ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
      return 4;
    }
    return -1;
  }

  // Parse Xft.dpi from the X resource manager; fall back to 96 (scale 1.0).
  uint32_t query_display_dpi(Display* dpy)
  {
    uint32_t dpi = 96;
    char* rms = XResourceManagerString(dpy);
    if (rms) {
      XrmDatabase db = XrmGetStringDatabase(rms);
      if (db) {
        char* type = nullptr;
        XrmValue val;
        if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &val) && val.addr) {
          double d = atof(val.addr);
          if (d >= 48.0 && d <= 480.0) dpi = static_cast<uint32_t>(d + 0.5);
        }
        XrmDestroyDatabase(db);
      }
    }
    return dpi;
  }

  void set_window_title(Display* dpy, Window win, const char* text)
  {
    const char* t = text ? text : "";
    XStoreName(dpy, win, t);   // legacy WM_NAME (Latin-1)
    if (g_net_wm_name != None && g_utf8_string != None)
      XChangeProperty(dpy, win, g_net_wm_name, g_utf8_string, 8,
                      PropModeReplace,
                      reinterpret_cast<const unsigned char*>(t),
                      static_cast<int>(std::strlen(t)));
  }

  void ensure_timerfd()
  {
    if (g_timerfd >= 0) return;
    g_timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (g_timerfd < 0) return;
    struct itimerspec its;
    its.it_interval.tv_sec  = 0;
    its.it_interval.tv_nsec = 16 * 1000 * 1000;  // 16 ms
    its.it_value = its.it_interval;
    timerfd_settime(g_timerfd, 0, &its, nullptr);
  }

  // ---- Painting -------------------------------------------------------------

  void paint_window(LinuxWindow* lw)
  {
    if (!lw || !lw->session) return;
    auto* wd = lw->session->get_widget(lw->widget_index);
    if (wd && wd->render_ctx)
      lw->session->paint_frame(wd->render_ctx, lw->widget_index);
    lw->needs_paint = false;
  }

  void flush_pending_paints()
  {
    // Snapshot first: a paint callback can create / destroy windows.
    std::vector<LinuxWindow*> todo;
    todo.reserve(g_windows.size());
    for (auto& kv : g_windows)
      if (kv.second->needs_paint) todo.push_back(kv.second);
    for (auto* lw : todo)
      if (g_windows.count(lw->win)) paint_window(lw);
  }

  void tick_animations()
  {
    for (auto& kv : g_windows)
      if (kv.second->toast_anim) kv.second->needs_paint = true;
  }

  // ---- Window teardown ------------------------------------------------------

  void destroy_native(LinuxWindow* lw)
  {
    if (!lw) return;
    if (lw->ic) { XDestroyIC(lw->ic); lw->ic = nullptr; }
    if (lw->win) {
      g_windows.erase(lw->win);
      XDestroyWindow(lw->dpy, lw->win);
      lw->win = 0;
    }
    // Embedded windows own a dedicated Display connection; close it last.
    if (lw->owns_display && lw->dpy) { XCloseDisplay(lw->dpy); lw->dpy = nullptr; }
    delete lw;
  }

  // Mirror of macOS windowWillClose: client gets APP_QUIT (and may veto), then
  // the render ctx + window are torn down and the appwindow count drops.
  void handle_close(LinuxWindow* lw)
  {
    Session* s = lw->session;
    neui_event_t ev = {};
    ev.type = NEUI_EVENT_APP_QUIT;
    if (s && !s->dispatch_event(&ev)) return;   // client vetoed

    auto* backend = platform_get_backend();
    auto* wd = s ? s->get_widget(lw->widget_index) : nullptr;
    if (wd && wd->render_ctx && backend) {
      s->_asset_manager.release_context(wd->render_ctx, backend);
      backend->destroy_context(wd->render_ctx);
      wd->render_ctx = nullptr;
    }
    bool was_app = lw->is_appwindow && lw->counted;
    if (wd) wd->native_handle = nullptr;
    destroy_native(lw);
    if (was_app && --g_appwindow_count <= 0) g_quit = true;
  }

  // ---- Input dispatch -------------------------------------------------------

  void send_mouse(LinuxWindow* lw, neui_event_type_t type, uint32_t target,
                  float lx, float ly, unsigned int state, unsigned int extra_mk)
  {
    if (target == 0) return;
    Session* s = lw->session;
    auto* hw = s->get_widget(target);
    if (!hw) return;
    neui_event_t ev = {};
    ev.type                 = type;
    ev.data.mouse.widget    = { hw->widget_id };
    ev.data.mouse.x         = static_cast<int>(lx);
    ev.data.mouse.y         = static_cast<int>(ly);
    ev.data.mouse.buttonmap = neui_detail::x11_buttonmap(state) | extra_mk;
    s->dispatch_mouse_event(target, &ev);
  }

  void dispatch_wheel(LinuxWindow* lw, XButtonEvent& be, float scale)
  {
    Session* s = lw->session;
    float lx = be.x / scale, ly = be.y / scale;
    int  delta = 0;
    bool horizontal = false;
    switch (be.button) {
      case 4: delta =  1; break;                       // wheel up
      case 5: delta = -1; break;                       // wheel down
      case 6: delta =  1; horizontal = true; break;    // tilt left
      case 7: delta = -1; horizontal = true; break;    // tilt right
      default: return;
    }
    if (s->_open_combo && !horizontal && s->handle_combo_wheel(lx, ly, delta))
      return;
    uint32_t hit = s->widget_at(lx, ly, lw->widget_index);
    if (hit == 0) return;
    auto* hw = s->get_widget(hit);
    if (!hw) return;
    neui_event_t ev = {};
    ev.type                     = NEUI_EVENT_MOUSE_WHEEL;
    ev.data.wheel.widget        = { hw->widget_id };
    ev.data.wheel.x             = static_cast<int>(lx);
    ev.data.wheel.y             = static_cast<int>(ly);
    ev.data.wheel.delta         = delta;
    ev.data.wheel.is_horizontal = horizontal ? 1 : 0;
    s->dispatch_wheel_event(hit, &ev);   // bubbles to scrolling ancestors
  }

  void dispatch_button_press(LinuxWindow* lw, XButtonEvent& be)
  {
    Session* s = lw->session;
    float scale = lw->dpi / 96.0f; if (scale <= 0.0f) scale = 1.0f;
    if (be.button >= 4 && be.button <= 7) { dispatch_wheel(lw, be, scale); return; }

    float lx = be.x / scale, ly = be.y / scale;

    if (be.button == 1) {
      // Overlay pre-checks (mirror macOS mouseDown: order).
      if (s->_popup_active) { s->handle_popup_click(lx, ly); return; }
      if (s->handle_toast_click(lw->widget_index, lx, ly)) return;
      if (s->handle_combo_click(lx, ly)) return;

      bool dbl = (lw->last_click_button == 1) &&
                 (be.time - lw->last_click_time <= 400) &&
                 (std::abs(be.x - lw->last_click_x) <= 4) &&
                 (std::abs(be.y - lw->last_click_y) <= 4);
      lw->last_click_button = 1;
      lw->last_click_time   = be.time;
      lw->last_click_x      = be.x;
      lw->last_click_y      = be.y;

      uint32_t hit = s->widget_at(lx, ly, lw->widget_index);
      if (dbl) {
        send_mouse(lw, NEUI_EVENT_MOUSE_BUTTON_DBLCLICK, hit, lx, ly, be.state, 0x0001);
        lw->last_click_button = 0;   // a 3rd press starts a fresh single click
        return;
      }
      s->set_focus(hit);
      s->set_pressed(hit);
      send_mouse(lw, NEUI_EVENT_MOUSE_BUTTON_DOWN, hit, lx, ly, be.state, 0x0001);
      return;
    }

    if (be.button == 3) {
      if (s->_popup_active) { s->handle_popup_click(lx, ly); return; }
      uint32_t hit = s->widget_at(lx, ly, lw->widget_index);
      send_mouse(lw, NEUI_EVENT_MOUSE_RBUTTON_DOWN, hit, lx, ly, be.state, 0x0002);
    }
  }

  void dispatch_button_release(LinuxWindow* lw, XButtonEvent& be)
  {
    if (be.button >= 4 && be.button <= 7) return;  // wheel: press carried it
    Session* s = lw->session;
    float scale = lw->dpi / 96.0f; if (scale <= 0.0f) scale = 1.0f;
    float lx = be.x / scale, ly = be.y / scale;

    if (be.button == 1) {
      if (s->_combo_sb_dragging) { s->_combo_sb_dragging = false; return; }
      uint32_t hit     = s->widget_at(lx, ly, lw->widget_index);
      uint32_t pressed = s->_pressed_widget;
      s->set_pressed(0);
      if (hit == 0) return;
      auto* hw = s->get_widget(hit);
      if (!hw) return;
      neui_event_t ev = {};
      ev.data.mouse.widget = { hw->widget_id };
      ev.data.mouse.x      = static_cast<int>(lx);
      ev.data.mouse.y      = static_cast<int>(ly);
      ev.type = NEUI_EVENT_MOUSE_BUTTON_UP;
      s->dispatch_mouse_event(hit, &ev);
      if (hit == pressed) {  // CLICK only when up lands on the pressed widget
        ev.type = NEUI_EVENT_MOUSE_BUTTON_CLICK;
        s->dispatch_mouse_event(hit, &ev);
      }
      return;
    }
    if (be.button == 3) {
      uint32_t hit = s->widget_at(lx, ly, lw->widget_index);
      if (hit == 0) return;
      auto* hw = s->get_widget(hit);
      if (!hw) return;
      neui_event_t ev = {};
      ev.type              = NEUI_EVENT_MOUSE_RBUTTON_UP;
      ev.data.mouse.widget = { hw->widget_id };
      ev.data.mouse.x      = static_cast<int>(lx);
      ev.data.mouse.y      = static_cast<int>(ly);
      s->dispatch_mouse_event(hit, &ev);
    }
  }

  void dispatch_motion(LinuxWindow* lw, XMotionEvent& me)
  {
    Session* s = lw->session;
    float scale = lw->dpi / 96.0f; if (scale <= 0.0f) scale = 1.0f;
    float lx = me.x / scale, ly = me.y / scale;

    if (s->_popup_active) { s->handle_popup_hover(lx, ly); return; }
    if (s->handle_combo_scroll_drag(ly)) return;
    if (s->handle_combo_hover(lx, ly)) return;

    uint32_t hit = s->widget_at(lx, ly, lw->widget_index);
    s->set_hovered(hit);

    uint32_t target = hit;
    if (s->_pressed_widget != 0 && (me.state & Button1Mask))
      target = s->_pressed_widget;   // route drags to the pressed widget
    send_mouse(lw, NEUI_EVENT_MOUSE_MOVE, target, lx, ly, me.state, 0);
  }

  void dispatch_key_press(LinuxWindow* lw, XKeyEvent& ke)
  {
    Session* s = lw->session;
    char  buf[64];
    KeySym ks = 0;
    Status status = XLookupNone;
    int n = 0;
    if (lw->ic)
      n = Xutf8LookupString(lw->ic, &ke, buf, sizeof(buf) - 1, &ks, &status);
    else
      n = XLookupString(&ke, buf, sizeof(buf) - 1, &ks, nullptr);
    if (n < 0) n = 0;
    buf[n] = '\0';

    uint32_t mods    = neui_detail::x11_modifiers_to_neui(ke.state);
    uint32_t keycode = neui_detail::x11_keysym_to_neui(ks);

    // Tab cycles logical focus (hand-rolled traversal, like win32/macOS).
    if (keycode == NEUI_KEY_TAB) {
      s->focus_next(!(mods & NEUI_KMOD_SHIFT));
      return;
    }

    // KEYDOWN: client first (focused widget), then the widget's on_keydown.
    if (keycode != 0) {
      uint32_t fw = s->_focused_widget;
      if (fw != 0 && s->_widgets.exists(fw)) {
        auto& wd = s->_widgets[fw];
        bool consumed = false;
        if (wd.emit_events) {
          neui_event_t ev = {};
          ev.type     = NEUI_EVENT_KEYDOWN;
          ev.data.key = { { wd.widget_id }, keycode, mods };
          consumed = s->dispatch_event(&ev);
        }
        if (!consumed)
          s->handle_input_key(NEUI_EVENT_KEYDOWN, keycode, mods);
      }
    }

    // KEYCHAR for printable input. Skip when Ctrl is held (command shortcuts
    // were already routed via on_keydown above), matching macOS.
    if (mods & NEUI_KMOD_CTRL) return;
    bool have_chars = (status == XLookupChars || status == XLookupBoth) ||
                      (!lw->ic && n > 0);
    if (!have_chars) return;

    uint32_t fw = s->_focused_widget;
    if (fw == 0 || !s->_widgets.exists(fw)) return;
    auto& wd = s->_widgets[fw];

    const unsigned char* p = reinterpret_cast<const unsigned char*>(buf);
    while (*p) {
      uint32_t cp = 0;
      int adv = utf8_decode(p, &cp);
      if (adv <= 0) break;
      p += adv;
      if (!neui_detail::is_printable_codepoint(cp)) continue;
      bool consumed = false;
      if (wd.emit_events) {
        neui_event_t ev = {};
        ev.type     = NEUI_EVENT_KEYCHAR;
        ev.data.key = { { wd.widget_id }, cp, mods };
        consumed = s->dispatch_event(&ev);
      }
      if (!consumed)
        s->handle_input_key(NEUI_EVENT_KEYCHAR, cp, mods);
    }
  }

  void dispatch_key_release(LinuxWindow* lw, XKeyEvent& ke)
  {
    Session* s = lw->session;
    KeySym ks = XLookupKeysym(&ke, 0);
    uint32_t keycode = neui_detail::x11_keysym_to_neui(ks);
    if (keycode == 0) return;
    uint32_t mods = neui_detail::x11_modifiers_to_neui(ke.state);
    uint32_t fw = s->_focused_widget;
    if (fw == 0 || !s->_widgets.exists(fw)) return;
    auto& wd = s->_widgets[fw];
    if (!wd.emit_events) return;
    neui_event_t ev = {};
    ev.type     = NEUI_EVENT_KEYUP;
    ev.data.key = { { wd.widget_id }, keycode, mods };
    s->dispatch_event(&ev);
  }

  void dispatch_x_event(XEvent& ev)
  {
    if (XFilterEvent(&ev, None)) return;   // let the IME consume composition keys
    if (g_clipboard.handle_event(ev)) return;  // CLIPBOARD selection events
    LinuxWindow* lw = find_window(ev.xany.window);
    if (!lw) return;
    Session* s = lw->session;

    switch (ev.type) {
      case Expose:
        if (ev.xexpose.count == 0) lw->needs_paint = true;
        break;

      case ConfigureNotify: {
        uint32_t wphys = static_cast<uint32_t>(ev.xconfigure.width);
        uint32_t hphys = static_cast<uint32_t>(ev.xconfigure.height);
        s->resize_render_ctx(lw->widget_index, wphys, hphys);
        auto* wd = s->get_widget(lw->widget_index);
        if (wd) {
          float scale = lw->dpi / 96.0f; if (scale <= 0.0f) scale = 1.0f;
          int wlog = static_cast<int>(wphys / scale + 0.5f);
          int hlog = static_cast<int>(hphys / scale + 0.5f);
          if (wd->width != wlog || wd->height != hlog) {
            wd->width  = wlog;
            wd->height = hlog;
            neui_event_t re = {};
            re.type               = NEUI_EVENT_RESIZE;
            re.data.resize.widget = { wd->widget_id };
            re.data.resize.width  = wlog;
            re.data.resize.height = hlog;
            s->dispatch_event(&re);
          }
        }
        lw->needs_paint = true;
        break;
      }

      case ButtonPress:   dispatch_button_press(lw, ev.xbutton);   break;
      case ButtonRelease: dispatch_button_release(lw, ev.xbutton); break;
      case MotionNotify:  dispatch_motion(lw, ev.xmotion);          break;
      case KeyPress:      dispatch_key_press(lw, ev.xkey);          break;
      case KeyRelease:    dispatch_key_release(lw, ev.xkey);        break;

      case FocusIn:  s->_os_focused = true;  lw->needs_paint = true; break;
      case FocusOut: s->_os_focused = false; lw->needs_paint = true; break;

      case LeaveNotify: s->set_hovered(0); break;

      case ClientMessage:
        if (ev.xclient.message_type == g_wm_protocols &&
            static_cast<Atom>(ev.xclient.data.l[0]) == g_wm_delete)
          handle_close(lw);
        break;

      default: break;
    }
  }

  void drain_events()
  {
    while (g_display && XPending(g_display)) {
      XEvent ev;
      XNextEvent(g_display, &ev);
      dispatch_x_event(ev);
    }
  }

  // ---- Window creation ------------------------------------------------------

  // borderless => no WM decorations (PLUGWINDOW). owner_lw non-null => DIALOG
  // with WM_TRANSIENT_FOR. is_appwindow => participates in the quit count.
  // embed_xid != 0 => create as a child of that foreign X11 Window over a
  // dedicated Display connection (DAW embedding); implies borderless, no WM
  // protocols, no quit-count participation, and no neui-owned event loop.
  void create_frame(Session* session, uint32_t widget_index, WidgetData& wd,
                    bool borderless, bool is_appwindow, LinuxWindow* owner_lw,
                    unsigned long embed_xid)
  {
    platform_init();
    const bool embedded = (embed_xid != 0);

    // Embedded windows open their own connection (Xlib is not safe to share
    // the DAW's Display across threads); standalone windows share g_display.
    Display* dpy = embedded ? XOpenDisplay(nullptr) : g_display;
    if (!dpy) return;
    if (embedded) borderless = true;

    int      scr   = DefaultScreen(dpy);
    Window   root  = embedded ? static_cast<Window>(embed_xid) : RootWindow(dpy, scr);
    Visual*  vis   = DefaultVisual(dpy, scr);
    int      depth = DefaultDepth(dpy, scr);

    uint32_t dpi   = query_display_dpi(dpy);
    float    scale = dpi / 96.0f; if (scale <= 0.0f) scale = 1.0f;
    int w_phys = static_cast<int>(wd.width  * scale + 0.5f); if (w_phys < 1) w_phys = 1;
    int h_phys = static_cast<int>(wd.height * scale + 0.5f); if (h_phys < 1) h_phys = 1;
    int x_phys = static_cast<int>(wd.x * scale + 0.5f);
    int y_phys = static_cast<int>(wd.y * scale + 0.5f);

    XSetWindowAttributes swa;
    std::memset(&swa, 0, sizeof(swa));
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     StructureNotifyMask | FocusChangeMask | LeaveWindowMask;
    // No server-side background: the backend always blits a fully-composed
    // frame, so letting X erase the window to a background colour first only
    // produces a flash between the erase and our blit. None + NorthWest bit
    // gravity (keep existing pixels on resize) gives flicker-free redraw.
    swa.background_pixmap = None;
    swa.bit_gravity       = NorthWestGravity;

    Window win = XCreateWindow(dpy, root, x_phys, y_phys, w_phys, h_phys, 0,
                               depth, InputOutput, vis,
                               CWEventMask | CWBackPixmap | CWBitGravity, &swa);
    if (!win) return;

    if (borderless && g_motif_hints != None) {
      // 5 longs: flags, functions, decorations, input_mode, status.
      long hints[5] = { 1L << 1 /*MWM_HINTS_DECORATIONS*/, 0, 0, 0, 0 };
      XChangeProperty(dpy, win, g_motif_hints, g_motif_hints, 32,
                      PropModeReplace,
                      reinterpret_cast<unsigned char*>(hints), 5);
    }

    // WM close-button protocol for managed (non-borderless) windows.
    if (!borderless && g_wm_delete != None)
      XSetWMProtocols(dpy, win, &g_wm_delete, 1);

    if (owner_lw && owner_lw->win)
      XSetTransientForHint(dpy, win, owner_lw->win);

    if (!wd.text.empty())
      set_window_title(dpy, win, wd.text.c_str());

    XSizeHints* sh = XAllocSizeHints();
    if (sh) {
      sh->flags  = USPosition | USSize;
      sh->x = x_phys; sh->y = y_phys;
      sh->width = w_phys; sh->height = h_phys;
      XSetWMNormalHints(dpy, win, sh);
      XFree(sh);
    }

    // The IME context belongs to g_display; embedded windows use their own
    // connection, so they fall back to XLookupString (Latin-1) - acceptable
    // for the embedded path until a per-connection XIM is wired.
    XIC ic = nullptr;
    if (!embedded && g_xim) {
      ic = XCreateIC(g_xim,
                     XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                     XNClientWindow, win,
                     XNFocusWindow,  win,
                     static_cast<void*>(nullptr));
    }

    auto* lw = new LinuxWindow();
    lw->dpy = dpy; lw->owns_display = embedded;
    lw->win = win; lw->visual = vis; lw->depth = depth;
    lw->ic = ic; lw->session = session; lw->widget_index = widget_index;
    lw->is_appwindow = is_appwindow; lw->dpi = dpi;
    g_windows[win] = lw;

    wd.native_handle = lw;
    wd.dpi = dpi;

    auto* backend = platform_get_backend();
    if (backend) {
      neui_cairo_backend::LinuxNativeSurface ns;
      ns.dpy = dpy; ns.win = win; ns.visual = vis; ns.depth = depth;
      wd.render_ctx = backend->create_context(&ns,
                                              static_cast<uint32_t>(w_phys),
                                              static_cast<uint32_t>(h_phys));
      if (wd.render_ctx) backend->update_dpi(wd.render_ctx, dpi);
    }

    // Pre-show attribute-driven size constraints.
    if (wd.attrs) {
      int min_w = wd.attrs->get_int(NEUI_ATTR_MIN_WIDTH,  0);
      int min_h = wd.attrs->get_int(NEUI_ATTR_MIN_HEIGHT, 0);
      int max_w = wd.attrs->get_int(NEUI_ATTR_MAX_WIDTH,  0);
      int max_h = wd.attrs->get_int(NEUI_ATTR_MAX_HEIGHT, 0);
      if (min_w || min_h || max_w || max_h)
        platform_apply_size_constraints(lw, min_w, min_h, max_w, max_h);
    }

    if (is_appwindow) { ++g_appwindow_count; lw->counted = true; }
  }

} // namespace

// ===========================================================================
// Platform seam implementation.

  void platform_init()
  {
    static bool inited = false;
    if (inited) return;
    inited = true;

    XInitThreads();
    XrmInitialize();
    g_display = XOpenDisplay(nullptr);
    if (!g_display) return;   // no DISPLAY - degrade to a no-window null-ish host

    g_wm_protocols = XInternAtom(g_display, "WM_PROTOCOLS",     False);
    g_wm_delete    = XInternAtom(g_display, "WM_DELETE_WINDOW", False);
    g_net_wm_name  = XInternAtom(g_display, "_NET_WM_NAME",     False);
    g_utf8_string  = XInternAtom(g_display, "UTF8_STRING",      False);
    g_motif_hints  = XInternAtom(g_display, "_MOTIF_WM_HINTS",  False);

    // Locale + input method for UTF-8 keyboard input. Failure is non-fatal:
    // create_frame falls back to XLookupString (Latin-1) when g_xim is null.
    XSetLocaleModifiers("");
    g_xim = XOpenIM(g_display, nullptr, nullptr, nullptr);

    g_clipboard.init(g_display);
  }

  neui_render_backend_t* platform_get_backend()
  {
    return neui_cairo_backend::get_backend();
  }

  void platform_create_appwindow(Session* session, uint32_t widget_index,
                                  WidgetData& wd)
  {
    create_frame(session, widget_index, wd, /*borderless*/false,
                 /*is_appwindow*/true, /*owner*/nullptr, /*embed*/0);
  }

  void platform_create_plugwindow(Session* session, uint32_t widget_index,
                                   WidgetData& wd)
  {
    // wd.embed_parent_xid (set via platform_set_embed_parent) selects the
    // foreign-parent embedded path; 0 = borderless standalone top-level.
    create_frame(session, widget_index, wd, /*borderless*/true,
                 /*is_appwindow*/false, /*owner*/nullptr,
                 wd.embed_parent_xid);
  }

  void platform_create_dialog(Session* session, uint32_t widget_index,
                               WidgetData& wd, void* owner_native)
  {
    create_frame(session, widget_index, wd, /*borderless*/false,
                 /*is_appwindow*/false,
                 static_cast<LinuxWindow*>(owner_native), /*embed*/0);
  }

  // ---- DAW-embedding seams (Linux-only). ------------------------------------

  void platform_set_embed_parent(Session* session, uint32_t widget_index,
                                 unsigned long parent_xid)
  {
    if (!session) return;
    auto* wd = session->get_widget(widget_index);
    if (wd) wd->embed_parent_xid = parent_xid;
  }

  int platform_embed_event_fd(void* native_handle)
  {
    if (!native_handle) return -1;
    auto* lw = static_cast<LinuxWindow*>(native_handle);
    return lw->dpy ? ConnectionNumber(lw->dpy) : -1;
  }

  void platform_embed_pump_and_tick(void* native_handle)
  {
    if (!native_handle) return;
    auto* lw = static_cast<LinuxWindow*>(native_handle);
    if (!lw->dpy) return;
    // Drain everything pending on this window's own connection.
    while (XPending(lw->dpy)) {
      XEvent ev;
      XNextEvent(lw->dpy, &ev);
      dispatch_x_event(ev);
    }
    // At most one animation tick per ~16 ms (the host calls this on its own
    // cadence; gate so a fast host doesn't over-advance toast/spring-back).
    uint64_t now = platform_now_ms();
    if (now - lw->last_tick_ms >= 16) {
      lw->last_tick_ms = now;
      if (lw->toast_anim) lw->needs_paint = true;
    }
    if (lw->needs_paint) paint_window(lw);
    XFlush(lw->dpy);
  }

  void platform_destroy_window(WidgetData& wd)
  {
    if (!wd.native_handle) return;
    auto* lw = static_cast<LinuxWindow*>(wd.native_handle);
    auto* backend = platform_get_backend();
    if (wd.render_ctx && backend) {
      if (lw->session) lw->session->_asset_manager.release_context(wd.render_ctx, backend);
      backend->destroy_context(wd.render_ctx);
      wd.render_ctx = nullptr;
    }
    bool was_app = lw->is_appwindow && lw->counted;
    destroy_native(lw);
    wd.native_handle = nullptr;
    if (was_app && g_appwindow_count > 0 && --g_appwindow_count <= 0)
      g_quit = true;
  }

  void platform_show_window(void* native_handle)
  {
    if (!native_handle) return;
    auto* lw = static_cast<LinuxWindow*>(native_handle);
    XMapWindow(lw->dpy, lw->win);
    XRaiseWindow(lw->dpy, lw->win);
    lw->needs_paint = true;
    XFlush(lw->dpy);
  }

  void platform_hide_window(void* native_handle)
  {
    if (!native_handle) return;
    auto* lw = static_cast<LinuxWindow*>(native_handle);
    XUnmapWindow(lw->dpy, lw->win);
    XFlush(lw->dpy);
  }

  void platform_set_window_enabled(void* /*native_handle*/, bool /*enabled*/)
  {
    // X11 has no per-window input disable; modal blocking relies on the
    // nested pump in platform_run_modal_until keeping the dialog on top.
    // Full owner input-block is deferred (Phase 4).
  }

  void platform_activate_window(void* native_handle)
  {
    if (!native_handle) return;
    auto* lw = static_cast<LinuxWindow*>(native_handle);
    XMapRaised(lw->dpy, lw->win);
    XSetInputFocus(lw->dpy, lw->win, RevertToParent, CurrentTime);
    XFlush(lw->dpy);
  }

  void platform_set_window_title(void* native_handle, const char* text)
  {
    if (!native_handle) return;
    auto* lw = static_cast<LinuxWindow*>(native_handle);
    set_window_title(lw->dpy, lw->win, text);
    XFlush(lw->dpy);
  }

  void platform_set_window_pos(void* native_handle,
                                int x, int y, int w, int h, uint32_t dpi)
  {
    if (!native_handle) return;
    auto* lw = static_cast<LinuxWindow*>(native_handle);
    float scale = (dpi ? dpi : lw->dpi) / 96.0f; if (scale <= 0.0f) scale = 1.0f;
    XMoveResizeWindow(lw->dpy, lw->win,
                      static_cast<int>(x * scale + 0.5f),
                      static_cast<int>(y * scale + 0.5f),
                      static_cast<unsigned int>(w * scale + 0.5f),
                      static_cast<unsigned int>(h * scale + 0.5f));
    XFlush(lw->dpy);
  }

  void platform_post_close(void* native_handle)
  {
    if (!native_handle) return;
    auto* lw = static_cast<LinuxWindow*>(native_handle);
    handle_close(lw);   // synchronous; mirrors the WM_DELETE path
  }

  float platform_get_scale_factor(void* native_handle)
  {
    if (!native_handle) return 1.0f;
    auto* lw = static_cast<LinuxWindow*>(native_handle);
    float s = lw->dpi / 96.0f;
    return s > 0.0f ? s : 1.0f;
  }

  void platform_invalidate(void* native_handle)
  {
    if (!native_handle) return;
    static_cast<LinuxWindow*>(native_handle)->needs_paint = true;
  }

  bool platform_run()
  {
    if (!g_display) return true;
    ensure_timerfd();
    g_quit = false;
    int xfd = ConnectionNumber(g_display);
    while (!g_quit && g_appwindow_count > 0) {
      drain_events();
      if (g_quit || g_appwindow_count <= 0) break;
      flush_pending_paints();
      XFlush(g_display);

      fd_set rfds; FD_ZERO(&rfds); FD_SET(xfd, &rfds);
      int maxfd = xfd;
      if (g_timerfd >= 0) { FD_SET(g_timerfd, &rfds); if (g_timerfd > maxfd) maxfd = g_timerfd; }
      int r = select(maxfd + 1, &rfds, nullptr, nullptr, nullptr);
      if (r < 0) { if (errno == EINTR) continue; break; }
      if (g_timerfd >= 0 && FD_ISSET(g_timerfd, &rfds)) {
        uint64_t exp = 0; ssize_t rd = read(g_timerfd, &exp, sizeof(exp)); (void)rd;
        tick_animations();
      }
      // Readable X fd is handled by drain_events() at the top of the loop.
    }
    return true;
  }

  bool platform_pump_once()
  {
    if (!g_display) return true;
    drain_events();
    flush_pending_paints();
    XFlush(g_display);
    return !g_quit;
  }

  bool platform_run_modal_until(bool* keep_running)
  {
    if (!g_display) return true;
    ensure_timerfd();
    int xfd = ConnectionNumber(g_display);
    while (keep_running && *keep_running && !g_quit) {
      drain_events();
      if (!keep_running || !*keep_running || g_quit) break;
      flush_pending_paints();
      XFlush(g_display);

      fd_set rfds; FD_ZERO(&rfds); FD_SET(xfd, &rfds);
      int maxfd = xfd;
      if (g_timerfd >= 0) { FD_SET(g_timerfd, &rfds); if (g_timerfd > maxfd) maxfd = g_timerfd; }
      struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 16000;
      int r = select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
      if (r < 0) { if (errno == EINTR) continue; break; }
      if (r > 0 && g_timerfd >= 0 && FD_ISSET(g_timerfd, &rfds)) {
        uint64_t exp = 0; ssize_t rd = read(g_timerfd, &exp, sizeof(exp)); (void)rd;
        tick_animations();
      }
    }
    return !g_quit;
  }

  // ---- Menu bar: X11 has no native menu bar; render-in-UI is deferred. ------
  void* platform_menubar_create(uint32_t /*widget_id*/)                                 { return nullptr; }
  void  platform_menubar_destroy(void* /*hmenu*/)                                       {}
  void  platform_menubar_attach(void* /*frame*/, void* /*hmenu*/)                       {}
  void  platform_menubar_refresh(void* /*frame*/)                                       {}
  void* platform_menubar_add_popup(void* /*hmenu*/, const char* /*text*/)               { return nullptr; }
  void  platform_menubar_add_item(void* /*hmenu*/, uint32_t /*cmd*/, const char* /*t*/) {}
  void  platform_menubar_add_separator(void* /*hmenu*/, uint32_t /*cmd*/)               {}
  void  platform_menubar_remove_popup(void* /*hmenu*/, void* /*sub*/)                   {}
  void  platform_menubar_remove_item(void* /*hmenu*/, uint32_t /*cmd*/)                 {}
  void  platform_menubar_enable_item(void* /*hmenu*/, uint32_t /*cmd*/, bool /*en*/)    {}
  void  platform_menubar_enable_popup(void* /*hmenu*/, void* /*sub*/, bool /*en*/)      {}
  void  platform_menubar_set_item_text(void* /*hmenu*/, uint32_t /*cmd*/, const char* /*t*/) {}
  void  platform_menubar_set_item_shortcut(void* /*hmenu*/, uint32_t /*cmd*/,
                                            uint32_t /*mods*/, uint32_t /*key*/)        {}

  void platform_set_window_icon(WidgetData& /*wd*/, const char* /*path*/) {}

  void platform_apply_size_constraints(void* native_handle,
                                        int min_w, int min_h, int max_w, int max_h)
  {
    if (!native_handle) return;
    auto* lw = static_cast<LinuxWindow*>(native_handle);
    float scale = lw->dpi / 96.0f; if (scale <= 0.0f) scale = 1.0f;
    XSizeHints* sh = XAllocSizeHints();
    if (!sh) return;
    sh->flags = 0;
    if (min_w > 0 || min_h > 0) {
      sh->flags |= PMinSize;
      sh->min_width  = min_w > 0 ? static_cast<int>(min_w * scale + 0.5f) : 0;
      sh->min_height = min_h > 0 ? static_cast<int>(min_h * scale + 0.5f) : 0;
    }
    if (max_w > 0 || max_h > 0) {
      sh->flags |= PMaxSize;
      sh->max_width  = max_w > 0 ? static_cast<int>(max_w * scale + 0.5f) : 32767;
      sh->max_height = max_h > 0 ? static_cast<int>(max_h * scale + 0.5f) : 32767;
    }
    if (sh->flags) XSetWMNormalHints(lw->dpy, lw->win, sh);
    XFree(sh);
  }

  // ---- Image loading via vendored stb_image. --------------------------------
  uint8_t* platform_load_image(const char* path,
                                uint32_t* width_out, uint32_t* height_out)
  {
    return neui_detail::load_image_bgra8_linux(path, width_out, height_out);
  }
  void platform_free_image(uint8_t* pixels)
  {
    neui_detail::free_image_bgra8_linux(pixels);
  }

  // ---- Clipboard: X11 CLIPBOARD selection (hosts/shared/linux). -------------
  bool platform_clipboard_set_text(const char* utf8, uint32_t length)
  { return g_clipboard.set_text(utf8, length); }
  int  platform_clipboard_get_text(char* buf, int buflen)
  { return g_clipboard.get_text(buf, buflen); }
  bool platform_clipboard_has_text()
  { return g_clipboard.has_text(); }
  bool platform_clipboard_write_item(const neui_detail::DataItem& item)
  { return g_clipboard.write_item(item); }
  bool platform_clipboard_read_item(neui_detail::DataItem& item)
  { return g_clipboard.read_item(item); }

  // ---- DnD: XDND is deferred. -----------------------------------------------

  bool platform_dnd_register_window(void* /*native_handle*/, void* /*session_ptr*/,
                                     uint32_t /*frame_widget_id*/)            { return false; }
  void platform_dnd_unregister_window(void* /*native_handle*/)                {}
  uint32_t platform_dnd_begin_drag(void* /*native_handle*/,
                                     neui_detail::DataItem* /*item*/,
                                     uint32_t /*allowed_actions*/,
                                     void* /*preview_native*/,
                                     int /*hot_x*/, int /*hot_y*/)            { return 0;     }
  void*    platform_make_drag_preview(const uint8_t* /*bgra_premul*/,
                                       uint32_t /*w_px*/, uint32_t /*h_px*/,
                                       float /*scale*/)                       { return nullptr; }

  void platform_set_cursor(int /*kind*/) {}   // per-window cursor deferred (Phase 4)

  void platform_start_toast_animation(void* native_handle)
  {
    if (!native_handle) return;
    static_cast<LinuxWindow*>(native_handle)->toast_anim = true;
  }
  void platform_stop_toast_animation(void* native_handle)
  {
    if (!native_handle) return;
    static_cast<LinuxWindow*>(native_handle)->toast_anim = false;
  }

  uint64_t platform_now_ms()
  {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ull +
           static_cast<uint64_t>(ts.tv_nsec) / 1000000ull;
  }

  int platform_message_box(void* /*native_handle*/, const char* /*text*/,
                           const char* /*caption*/, uint32_t /*flags*/)       { return 0; }

} // namespace xpl_host
