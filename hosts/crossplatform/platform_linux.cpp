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
#include "../shared/dnd_modifier_suggest.h"   // dnd_suggest_action
#include "../shared/linux/keys_linux.h"
#include "../shared/linux/clipboard_linux.h"
#include "../shared/linux/dnd_linux.h"
#include "../shared/linux/message_box_linux.h"
#include "../shared/theme_palette.h"
#include "../../backends/cairo/cairo_backend.h"

// This TU emits the single stb_image implementation for the Linux host.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_DEPRECATED
#include "../shared/linux/image_loader_linux.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/cursorfont.h>
#ifdef NEUI_HAS_XI2
#include <X11/extensions/XInput2.h>   // XI2 smooth-scroll valuators (optional)
#endif

#include <sys/select.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <time.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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

  // Active cursor shape (CursorKind) + per-Display EW-resize cursor cache.
  // Cursors are a per-connection resource, so embedded windows on their own
  // Display get their own entry.
  int g_cursor_kind = NEUI_CURSOR_DEFAULT;
  std::unordered_map<Display*, Cursor> g_ew_cursors;

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
    // Modal blocking: set while a modal child dialog is up (X11 has no native
    // per-window input disable, so dispatch_x_event swallows input to a
    // disabled window). See platform_set_window_enabled.
    bool         input_disabled = false;
    // SMOOTH-scroll spring-back tracking (mirrors platform_win32's per-frame
    // bouncing_*_index): at most one grid + one section bounce in flight per
    // window; the 16 ms heartbeat steps them. 0 = none.
    uint32_t     bouncing_grid_index    = 0;
    uint32_t     bouncing_section_index = 0;
    // Fractional XI2 wheel-notch accumulators for the classic int-line path
    // (combo / listbox / stepped surfaces): smooth devices deliver sub-notch
    // deltas, so lines are emitted only once a whole notch has accrued.
    double       scroll_v_accum = 0.0;
    double       scroll_h_accum = 0.0;
    // Set when a left-press was consumed by the in-frame menubar, so the
    // matching release is swallowed (it must not reach a widget under the
    // now-closed dropdown).
    bool         swallow_release   = false;
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

  // ---- XInput2 smooth scroll ------------------------------------------------
  // Modern X servers expose wheel + trackpad scrolling as XI2 "scroll-class"
  // valuators delivered inside XI_Motion events (pixel-precise, fractional),
  // alongside legacy core Button 4-7 for back-compat. We select XI_Motion per
  // frame window, diff the valuator values into wheel notches, and feed the
  // shared kinetics (the same integrators win32/macOS use). The legacy core
  // buttons are suppressed only AFTER the first XI2 scroll actually arrives
  // (g_xi2_scroll_seen) - so if a server / XWayland setup never delivers scroll
  // valuators, the core Button 4-7 stepped fallback keeps working unchanged.
#ifdef NEUI_HAS_XI2
  int  g_xi2_opcode      = -1;     // major opcode of XInputExtension, -1 = absent
  bool g_xi2_scroll_seen = false;  // a nonzero XI2 scroll delta has arrived
  bool g_xi2_motion_seen = false;  // an XI_Motion has arrived (XI2 now drives hover/
                                   // drag; core MotionNotify is suppressed by the
                                   // XI_Motion selection, so we skip it from here on)
  std::unordered_map<Display*, bool> g_xi2_inited;   // version negotiated per connection

  // One scroll axis on a device: which valuator carries it + its per-unit
  // increment (value change for one wheel notch; may be fractional/negative).
  struct ScrollValuator { int number = 0; bool horizontal = false; double increment = 0.0; };
  // deviceid -> its scroll valuators (queried lazily, server-global ids).
  std::unordered_map<int, std::vector<ScrollValuator>> g_xi2_scroll_classes;
  // (deviceid<<32 | valuator) -> last seen absolute value, for delta diffing.
  std::unordered_map<uint64_t, double> g_xi2_last_value;

  // One wheel notch scrolls this many lines (no Linux SPI_GETWHEELSCROLLLINES
  // analogue; matches the Win32 default so the feel is consistent).
  static constexpr int LINUX_WHEEL_LINES = 3;

  // Negotiate XI2 (>= 2.0) on a connection once. The major opcode is
  // server-global, so g_xi2_opcode is shared across the standalone + any
  // embedded connections; version negotiation is per-connection (cached).
  bool ensure_xi2(Display* dpy)
  {
    if (!dpy) return false;
    auto it = g_xi2_inited.find(dpy);
    if (it != g_xi2_inited.end()) return it->second && g_xi2_opcode >= 0;
    bool ok = false;
    int opcode = 0, ev = 0, err = 0;
    if (XQueryExtension(dpy, "XInputExtension", &opcode, &ev, &err)) {
      int major = 2, minor = 2;
      if (XIQueryVersion(dpy, &major, &minor) == Success && major >= 2) {
        g_xi2_opcode = opcode;
        ok = true;
      }
    }
    g_xi2_inited[dpy] = ok;
    return ok;
  }

  // Ask the server to deliver XI_Motion (which carries scroll-class valuators)
  // for this window. Core button/motion selection is unaffected - we keep
  // using core MotionNotify for hover/drag and only mine XI_Motion for scroll.
  void xi2_select_window(Display* dpy, Window win)
  {
    if (!ensure_xi2(dpy)) return;
    unsigned char mask[XIMaskLen(XI_LASTEVENT)] = { 0 };
    XISetMask(mask, XI_Motion);
    XIEventMask em;
    em.deviceid = XIAllMasterDevices;
    em.mask_len = sizeof(mask);
    em.mask     = mask;
    XISelectEvents(dpy, win, &em, 1);
  }

  // Lazily fetch (and cache) the scroll-class valuators of a device. Server
  // device ids are global, so the cache is keyed by id alone.
  const std::vector<ScrollValuator>& xi2_scroll_classes(Display* dpy, int deviceid)
  {
    auto it = g_xi2_scroll_classes.find(deviceid);
    if (it != g_xi2_scroll_classes.end()) return it->second;
    std::vector<ScrollValuator> out;
    int n = 0;
    XIDeviceInfo* info = XIQueryDevice(dpy, deviceid, &n);
    if (info) {
      for (int i = 0; i < n; ++i)
        for (int c = 0; c < info[i].num_classes; ++c) {
          if (info[i].classes[c]->type != XIScrollClass) continue;
          auto* sc = reinterpret_cast<XIScrollClassInfo*>(info[i].classes[c]);
          ScrollValuator sv;
          sv.number     = sc->number;
          sv.horizontal = (sc->scroll_type == XIScrollTypeHorizontal);
          sv.increment  = sc->increment;
          out.push_back(sv);
        }
      XIFreeDeviceInfo(info);
    }
    return g_xi2_scroll_classes.emplace(deviceid, std::move(out)).first->second;
  }
#endif  // NEUI_HAS_XI2

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
    // Created disarmed: arm_timer() turns the 16 ms heartbeat on only while an
    // animation (a toast) is actually running, so an idle window blocks in
    // select() on the X fd alone instead of waking 60x/s for nothing.
  }

  // Arm / disarm the 16 ms animation heartbeat. it_value == 0 disarms.
  void arm_timer(bool on)
  {
    if (g_timerfd < 0) return;
    struct itimerspec its; std::memset(&its, 0, sizeof its);
    if (on) {
      its.it_interval.tv_nsec = 16 * 1000 * 1000;  // 16 ms
      its.it_value = its.it_interval;
    }
    timerfd_settime(g_timerfd, 0, &its, nullptr);
  }

  bool any_window_animating()
  {
    for (auto& kv : g_windows)
      if (kv.second->toast_anim ||
          kv.second->bouncing_grid_index || kv.second->bouncing_section_index)
        return true;
    return false;
  }

  // Advance any in-flight GRID / SECTION spring-back for this window by one
  // 16 ms step (the SMOOTH-scroll twin of paint_toast's self-advancing tick).
  // Clears the bounce slot once the integrator settles; marks needs_paint
  // while animating. Mirrors platform_win32's WM_TIMER bounce handlers.
  void step_scroll_bounce(LinuxWindow* lw)
  {
    using namespace neui_detail;
    Session* s = lw->session;
    if (lw->bouncing_grid_index) {
      auto* hw = s->get_widget(lw->bouncing_grid_index);
      GridModel* model = hw ? hw->grid_model_ptr() : nullptr;
      if (!model) {
        lw->bouncing_grid_index = 0;
      } else {
        auto cfg = grid_read_config(hw->attrs.get());
        GridViewport vp = grid_compute_viewport(*model, hw->width, hw->height,
                                                 cfg.row_h, cfg.header_h);
        bool more = grid_scroll_bounce_step(*model, vp, cfg.row_h);
        lw->needs_paint = true;
        if (!more) lw->bouncing_grid_index = 0;
      }
    }
    if (lw->bouncing_section_index) {
      auto* sw = s->get_widget(lw->bouncing_section_index);
      SectionScrollState* st = sw ? sw->scroll_state_ptr() : nullptr;
      const SectionLayout* L = sw ? sw->section_layout_ptr() : nullptr;
      if (!st || !L) {
        lw->bouncing_section_index = 0;
      } else {
        bool more_v = section_scroll_bounce_step(*st, *L, false);
        bool more_h = section_scroll_bounce_step(*st, *L, true);
        lw->needs_paint = true;
        sw->notify_scroll_changed();
        if (!more_v && !more_h) lw->bouncing_section_index = 0;
      }
    }
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
    for (auto& kv : g_windows) {
      if (kv.second->toast_anim) kv.second->needs_paint = true;
      step_scroll_bounce(kv.second);
    }
    // Once nothing is animating, drop back to a blocking select() (no 60 Hz
    // wakeups). Toast start/stop also touch arm_timer; this catches the
    // bounce-settled case.
    if (!any_window_animating()) arm_timer(false);
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
    // Drop any cached cursor for it first (XCloseDisplay frees the resource,
    // but a stale Display* key could collide with a later XOpenDisplay reuse).
    if (lw->owns_display && lw->dpy) {
      g_ew_cursors.erase(lw->dpy);
      XCloseDisplay(lw->dpy);
      lw->dpy = nullptr;
    }
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
    // Wheel (legacy core Button 4-7): the stepped fallback. Once XI2 scroll has
    // proven it delivers on this server, the same physical scroll also arrives
    // as XI2 valuators, so drop the legacy buttons to avoid double-counting.
    if (be.button >= 4 && be.button <= 7) {
#ifdef NEUI_HAS_XI2
      if (!g_xi2_scroll_seen) dispatch_wheel(lw, be, scale);
#else
      dispatch_wheel(lw, be, scale);
#endif
      return;
    }

    float lx = be.x / scale, ly = be.y / scale;

    if (be.button == 1) {
      // Overlay pre-checks (mirror macOS mouseDown: order).
      if (s->_popup_active) { s->handle_popup_click(lx, ly); return; }
      if (s->handle_toast_click(lw->widget_index, lx, ly)) return;
      if (s->handle_combo_click(lx, ly)) return;
      // In-frame menubar (band + cascading dropdowns) owns clicks in its band
      // and while open; swallow the matching release so it can't fall through
      // to a widget under the dismissed dropdown.
      if (s->handle_menubar_click(lw->widget_index, lx, ly)) {
        lw->swallow_release = true;
        return;
      }

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
      if (s->_menu_open) { s->close_menubar_menu(); return; }
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
      if (lw->swallow_release) { lw->swallow_release = false; return; }
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

  // Pointer-motion dispatch core. `state` is an X11 core button/modifier mask
  // (ShiftMask / ControlMask / Button1Mask ...). Driven by core MotionNotify
  // when XI2 is absent, and by XI_Motion when XI2 is active (selecting XI_Motion
  // suppresses core MotionNotify on this server, so XI2 must drive hover/drag
  // too - not just scroll).
  void do_motion(LinuxWindow* lw, float lx, float ly, unsigned int state)
  {
    Session* s = lw->session;
    if (s->_popup_active) { s->handle_popup_hover(lx, ly); return; }
    if (s->_menu_open) {
      if (s->handle_menubar_hover(lw->widget_index, lx, ly)) return;
    } else if (s->handle_menubar_band_hover(lw->widget_index, lx, ly)) {
      return;  // hovering the (closed) menubar band - highlight only, no widgets here
    }
    if (s->handle_combo_scroll_drag(ly)) return;
    if (s->handle_combo_hover(lx, ly)) return;

    uint32_t hit = s->widget_at(lx, ly, lw->widget_index);
    s->set_hovered(hit);

    uint32_t target = hit;
    if (s->_pressed_widget != 0 && (state & Button1Mask))
      target = s->_pressed_widget;   // route drags to the pressed widget
    send_mouse(lw, NEUI_EVENT_MOUSE_MOVE, target, lx, ly, state, 0);
  }

  void dispatch_motion(LinuxWindow* lw, XMotionEvent& me)
  {
    float scale = lw->dpi / 96.0f; if (scale <= 0.0f) scale = 1.0f;
    do_motion(lw, me.x / scale, me.y / scale, me.state);
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

    // An open in-frame menubar captures the keyboard (Esc/arrows/Enter).
    if (s->_menu_open) { s->handle_menubar_key(keycode, mods); return; }

    // Menubar accelerators (Ctrl+S etc.) - matched here on Linux since the
    // Win32 HACCEL path is MSG-based. On a hit the key is consumed (routed
    // through dispatch_menu_event, which tries the focused widget first).
    if (keycode != 0 && s->try_menubar_accel(keycode, mods)) return;

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

  // ---- XDND drop-target receiver --------------------------------------------
  neui_detail::XdndAtoms g_xa;
  struct DropTarget { void* session = nullptr; uint32_t frame_id = 0; uint32_t dpi = 96;
                      Display* dpy = nullptr; };
  std::unordered_map<Window, DropTarget> g_drop_targets;

  // Active incoming drag (XDND is strictly one drag at a time per display).
  struct {
    Window   source  = 0;
    Window   target  = 0;        // our registered window
    bool     entered = false;
    int      last_lx = 0, last_ly = 0;   // logical, frame-local
    uint32_t last_action = 0;            // neui action from the last Position
    std::vector<Atom>        atoms;
    std::vector<std::string> mimes;
    std::vector<const char*> ptrs;       // point into `mimes`
  } g_xdrag;

  void xdnd_reset_drag()
  {
    g_xdrag.source = 0; g_xdrag.target = 0; g_xdrag.entered = false;
    g_xdrag.atoms.clear(); g_xdrag.mimes.clear(); g_xdrag.ptrs.clear();
  }

  void xdnd_register(Display* dpy, Window win, void* session, uint32_t frame_id, uint32_t dpi)
  {
    long ver = neui_detail::kXdndVersion;
    XChangeProperty(dpy, win, g_xa.aware, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&ver), 1);
    g_drop_targets[win] = DropTarget{session, frame_id, dpi, dpy};
  }
  void xdnd_unregister(Window win)
  {
    auto it = g_drop_targets.find(win);
    if (it != g_drop_targets.end()) {
      XDeleteProperty(it->second.dpy ? it->second.dpy : g_display, win, g_xa.aware);
      g_drop_targets.erase(it);
    }
  }

  // Synchronous XdndSelection conversion for a drop (same pump shape as the
  // clipboard read): convert into our recv property on target_win, wait for
  // SelectionNotify, read the bytes. Leaves unrelated events queued.
  bool xdnd_read_selection(Display* dpy, Window target_win, Atom target_atom, Time t,
                           std::vector<uint8_t>& out)
  {
    out.clear();
    XDeleteProperty(dpy, target_win, g_xa.recv_prop);
    XConvertSelection(dpy, g_xa.selection, target_atom, g_xa.recv_prop,
                      target_win, t);
    XFlush(dpy);
    struct timespec start; clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
      XEvent ev;
      if (XCheckTypedWindowEvent(dpy, target_win, SelectionNotify, &ev)) {
        if (ev.xselection.property == None) return false;
        Atom type = None; int fmt = 0; unsigned long n = 0, after = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(dpy, target_win, g_xa.recv_prop, 0, 0, False,
                               AnyPropertyType, &type, &fmt, &n, &after, &data) != Success)
          return false;
        if (data) { XFree(data); data = nullptr; }
        if (type == g_xa.incr) {          // chunked transfer - unsupported
          XDeleteProperty(dpy, target_win, g_xa.recv_prop);
          return false;
        }
        long longs = ((long)after + 3) / 4;
        if (XGetWindowProperty(dpy, target_win, g_xa.recv_prop, 0,
                               longs ? longs : 1, False, AnyPropertyType,
                               &type, &fmt, &n, &after, &data) == Success && data) {
          size_t unit = (fmt == 32) ? sizeof(long) : static_cast<size_t>(fmt / 8);
          out.assign(data, data + static_cast<size_t>(n) * unit);
          XFree(data);
        }
        XDeleteProperty(dpy, target_win, g_xa.recv_prop);
        return !out.empty();
      }
      struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
      long ms = (now.tv_sec - start.tv_sec) * 1000 +
                (now.tv_nsec - start.tv_nsec) / 1000000;
      if (ms >= 2000) return false;
      usleep(2000);
    }
  }

  void xdnd_send_status(Display* dpy, Window source, Window target, bool accept, Atom action)
  {
    XClientMessageEvent m; std::memset(&m, 0, sizeof(m));
    m.type = ClientMessage; m.display = dpy; m.window = source;
    m.message_type = g_xa.status; m.format = 32;
    m.data.l[0] = (long)target;
    m.data.l[1] = (accept ? 1L : 0L) | 2L;   // bit0 = accept, bit1 = send all positions
    m.data.l[4] = accept ? (long)action : (long)None;
    XSendEvent(dpy, source, False, NoEventMask, reinterpret_cast<XEvent*>(&m));
    XFlush(dpy);
  }
  void xdnd_send_finished(Display* dpy, Window source, Window target, bool accept, Atom action)
  {
    XClientMessageEvent m; std::memset(&m, 0, sizeof(m));
    m.type = ClientMessage; m.display = dpy; m.window = source;
    m.message_type = g_xa.finished; m.format = 32;
    m.data.l[0] = (long)target;
    m.data.l[1] = accept ? 1L : 0L;
    m.data.l[2] = accept ? (long)action : (long)None;
    XSendEvent(dpy, source, False, NoEventMask, reinterpret_cast<XEvent*>(&m));
    XFlush(dpy);
  }

  // Returns true if the message was an XDND target message (and was handled).
  bool xdnd_handle_client_message(XClientMessageEvent& e)
  {
    Atom mt = e.message_type;
    if (mt != g_xa.enter && mt != g_xa.position &&
        mt != g_xa.leave && mt != g_xa.drop)
      return false;

    auto dit = g_drop_targets.find(e.window);
    if (dit == g_drop_targets.end()) return true;   // not one of our targets
    DropTarget& dt = dit->second;
    auto* s = static_cast<Session*>(dt.session);
    uint32_t frame_idx = dt.frame_id & 0xFFFFu;
    float scale = dt.dpi / 96.0f; if (scale <= 0.0f) scale = 1.0f;
    // The drag arrived on the display this window lives on (g_display for a
    // standalone window, the DAW's connection for an embedded one). Reading the
    // selection / answering on g_display would silently fail for embedded plugins.
    Display* dpy = dt.dpy ? dt.dpy : g_display;

    if (mt == g_xa.enter) {
      xdnd_reset_drag();
      g_xdrag.source = (Window)e.data.l[0];
      g_xdrag.target = e.window;
      bool more = (e.data.l[1] & 1L) != 0;   // > 3 types -> read XdndTypeList
      std::vector<Atom> raw;
      if (more) {
        Atom type = None; int fmt = 0; unsigned long n = 0, after = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(dpy, g_xdrag.source, g_xa.type_list, 0, 256,
                               False, XA_ATOM, &type, &fmt, &n, &after, &data) == Success
            && data) {
          Atom* a = reinterpret_cast<Atom*>(data);
          raw.assign(a, a + n);
          XFree(data);
        }
      } else {
        for (int i = 2; i <= 4; ++i)
          if (e.data.l[i]) raw.push_back((Atom)e.data.l[i]);
      }
      for (Atom a : raw) {
        std::string mime = neui_detail::xdnd_atom_to_mime(dpy, a, g_xa);
        if (mime.empty()) continue;
        g_xdrag.atoms.push_back(a);
        g_xdrag.mimes.push_back(std::move(mime));
      }
      for (auto& m : g_xdrag.mimes) g_xdrag.ptrs.push_back(m.c_str());
      // XDND has no coords in Enter; ENTER is dispatched on the first Position.
      return true;
    }

    if (mt == g_xa.position) {
      Window src = (Window)e.data.l[0];
      int rx = (int)((e.data.l[2] >> 16) & 0xFFFF);
      int ry = (int)(e.data.l[2] & 0xFFFF);
      uint32_t suggested = neui_detail::xdnd_action_to_neui(g_xa, (Atom)e.data.l[4]);
      g_xdrag.last_action = suggested;

      int lx = 0, ly = 0; Window child;
      XTranslateCoordinates(dpy, DefaultRootWindow(dpy), e.window,
                            rx, ry, &lx, &ly, &child);
      g_xdrag.last_lx = (int)(lx / scale);
      g_xdrag.last_ly = (int)(ly / scale);

      const char* const* fmts = g_xdrag.ptrs.empty() ? nullptr : g_xdrag.ptrs.data();
      uint32_t count = (uint32_t)g_xdrag.ptrs.size();
      uint32_t accepted;
      if (!g_xdrag.entered) {
        accepted = s->dispatch_dnd_enter(frame_idx, g_xdrag.last_lx, g_xdrag.last_ly,
                                         fmts, count, suggested, 0);
        g_xdrag.entered = true;
      } else {
        accepted = s->dispatch_dnd_move(frame_idx, g_xdrag.last_lx, g_xdrag.last_ly,
                                        fmts, count, suggested, 0);
      }
      xdnd_send_status(dpy, src, e.window, accepted != 0,
                       neui_detail::xdnd_neui_to_action(g_xa, accepted));
      return true;
    }

    if (mt == g_xa.leave) {
      if (g_xdrag.entered) s->dispatch_dnd_leave();
      xdnd_reset_drag();
      return true;
    }

    // XdndDrop
    Window src = (Window)e.data.l[0];
    Time   t   = (Time)e.data.l[2];
    // A compliant source always sends at least one XdndPosition before the
    // drop; if it didn't (no ENTER dispatched, coords still 0,0), refuse rather
    // than synthesize a phantom drop at the origin with a NONE action.
    if (!g_xdrag.entered) {
      xdnd_send_finished(dpy, src, e.window, false, None);
      xdnd_reset_drag();
      return true;
    }
    neui_detail::DataItem item;
    for (size_t i = 0; i < g_xdrag.atoms.size(); ++i) {
      std::vector<uint8_t> bytes;
      if (xdnd_read_selection(dpy, e.window, g_xdrag.atoms[i], t, bytes) && !bytes.empty())
        item.set_format(g_xdrag.mimes[i], bytes.data(), (uint32_t)bytes.size());
    }
    const char* const* fmts = g_xdrag.ptrs.empty() ? nullptr : g_xdrag.ptrs.data();
    uint32_t count = (uint32_t)g_xdrag.ptrs.size();
    uint32_t accepted = s->dispatch_dnd_drop(frame_idx, g_xdrag.last_lx, g_xdrag.last_ly,
                                             fmts, count, g_xdrag.last_action, 0, &item);
    xdnd_send_finished(dpy, src, e.window, accepted != 0,
                       neui_detail::xdnd_neui_to_action(g_xa, accepted));
    xdnd_reset_drag();
    return true;
  }

  // ---- XDND drag-source helpers ---------------------------------------------

  // Heap struct returned by platform_make_drag_preview; consumed by begin_drag.
  struct DragPreview {
    std::vector<uint8_t> bgra;            // BGRA8 premultiplied (== cairo ARGB32 LE)
    uint32_t w_px = 0, h_px = 0;
    float    scale = 1.0f;
  };

  // Swallow transient BadWindow/BadMatch while probing other apps' windows
  // during a drag (a window can vanish mid-drag; the default handler aborts).
  int drag_xerror(Display*, XErrorEvent*) { return 0; }

  // Process-wide error handler. The normal event loop touches foreign windows
  // too - e.g. an XdndEnter names a source window we then XGetWindowProperty,
  // and that source can crash/close between the message and our read. Xlib's
  // default handler calls exit() on any unhandled error, which would take down
  // the whole process (and the host DAW when embedded). Swallow the transient
  // resource-race codes; surface anything genuinely unexpected on stderr but
  // keep running rather than abort.
  int neui_xerror(Display* d, XErrorEvent* e)
  {
    switch (e->error_code) {
      case BadWindow:
      case BadDrawable:
      case BadMatch:
      case BadValue:
      case BadAtom:
        return 0;   // foreign-window / stale-resource race - ignore
      default: {
        char buf[256]; buf[0] = 0;
        if (d) XGetErrorText(d, e->error_code, buf, (int)sizeof buf);
        std::fprintf(stderr, "[neui] X error %d (%s), request %d.%d\n",
                     e->error_code, buf, e->request_code, e->minor_code);
        return 0;   // do not abort
      }
    }
  }

  void xdnd_send(Window to, Atom mt, long l0, long l1, long l2, long l3, long l4)
  {
    XClientMessageEvent m; std::memset(&m, 0, sizeof m);
    m.type = ClientMessage; m.display = g_display; m.window = to;
    m.message_type = mt; m.format = 32;
    m.data.l[0] = l0; m.data.l[1] = l1; m.data.l[2] = l2; m.data.l[3] = l3; m.data.l[4] = l4;
    XSendEvent(g_display, to, False, NoEventMask, reinterpret_cast<XEvent*>(&m));
    XFlush(g_display);
  }

  bool xdnd_has_aware(Window w, long* ver)
  {
    Atom t = None; int f = 0; unsigned long n = 0, a = 0; unsigned char* data = nullptr;
    bool ok = false;
    if (XGetWindowProperty(g_display, w, g_xa.aware, 0, 1, False, AnyPropertyType,
                           &t, &f, &n, &a, &data) == Success && data) {
      if (n > 0) { if (ver) *ver = *reinterpret_cast<long*>(data); ok = true; }
      XFree(data);
    }
    return ok;
  }

  // Descend the pointer chain from `start`; return the deepest XdndAware window.
  Window xdnd_choose_aware(Window start, int rx, int ry, long* out_ver)
  {
    Window root = DefaultRootWindow(g_display);
    Window w = start, target = None; long ver = 0;
    for (int depth = 0; depth < 24; ++depth) {
      long v = 0;
      if (xdnd_has_aware(w, &v)) { target = w; ver = v; }
      Window child = None; int cx = 0, cy = 0;
      if (!XTranslateCoordinates(g_display, root, w, rx, ry, &cx, &cy, &child)) break;
      if (child == None) break;
      w = child;
    }
    if (out_ver) *out_ver = ver;
    return target;
  }

  // Topmost mapped toplevel under (rx,ry) excluding `exclude` (our preview).
  // Fills *out_rect with its root-relative geometry so a caller can cache the
  // result and skip this full XQueryTree scan (one round-trip per toplevel)
  // while the pointer stays inside that rect. None if no toplevel covers it.
  Window xdnd_topmost_at(int rx, int ry, Window exclude, XRectangle* out_rect)
  {
    if (out_rect) *out_rect = XRectangle{0, 0, 0, 0};
    Window root = DefaultRootWindow(g_display);
    Window rr, parent, *kids = nullptr; unsigned int n = 0;
    if (!XQueryTree(g_display, root, &rr, &parent, &kids, &n)) return None;
    Window found = None;
    for (int i = (int)n - 1; i >= 0; --i) {     // top-to-bottom
      Window w = kids[i];
      if (w == exclude) continue;
      XWindowAttributes at;
      if (!XGetWindowAttributes(g_display, w, &at)) continue;
      if (at.map_state != IsViewable) continue;
      if (rx < at.x || ry < at.y || rx >= at.x + at.width || ry >= at.y + at.height) continue;
      found = w;
      if (out_rect) *out_rect = XRectangle{(short)at.x, (short)at.y,
                                           (unsigned short)at.width, (unsigned short)at.height};
      break;                                    // occluding window decided
    }
    if (kids) XFree(kids);
    return found;
  }

  // Topmost-toplevel scan + deepest-aware descent are kept as the two separate
  // helpers above (xdnd_topmost_at + xdnd_choose_aware) so the drag spin can
  // cache the expensive scan and re-run only the cheap descent per motion.

  // Serve a SelectionRequest for the drag's XdndSelection from `item`.
  void xdnd_serve_source_selection(XSelectionRequestEvent& r, neui_detail::DataItem* item,
                                   const std::vector<Atom>& offered)
  {
    static Atom a_targets = XInternAtom(g_display, "TARGETS", False);
    XSelectionEvent se; std::memset(&se, 0, sizeof se);
    se.type = SelectionNotify; se.display = g_display; se.requestor = r.requestor;
    se.selection = r.selection; se.target = r.target; se.time = r.time; se.property = None;
    Atom prop = r.property ? r.property : r.target;

    if (r.target == a_targets) {
      std::vector<Atom> t = offered; t.push_back(a_targets);
      XChangeProperty(g_display, r.requestor, prop, XA_ATOM, 32, PropModeReplace,
                      reinterpret_cast<unsigned char*>(t.data()), (int)t.size());
      se.property = prop;
    } else {
      std::string mime = neui_detail::xdnd_atom_to_mime(g_display, r.target, g_xa);
      if (!mime.empty() && item->has_format(mime)) {
        int nb = item->get_format(mime, nullptr, 0);
        std::vector<uint8_t> bytes(nb > 0 ? nb : 0);
        if (nb > 0) item->get_format(mime, bytes.data(), nb);
        XChangeProperty(g_display, r.requestor, prop, r.target, 8, PropModeReplace,
                        bytes.data(), (int)bytes.size());
        se.property = prop;
      }
    }
    XSendEvent(g_display, r.requestor, False, NoEventMask, reinterpret_cast<XEvent*>(&se));
    XFlush(g_display);
  }

  // ---- XI2 scroll routing ---------------------------------------------------
#ifdef NEUI_HAS_XI2

  // Nearest scrolling-SECTION ancestor of `hit` (or `hit` itself). 0 = none.
  // Mirrors platform_win32::find_scrolling_section.
  uint32_t find_scrolling_section_linux(Session* s, uint32_t hit)
  {
    auto* hw = s->get_widget(hit);
    if (hw && hw->scroll_state_ptr()) return hit;
    for (uint32_t p : s->_widgets.get_all_parents(hit)) {
      auto* pw = s->get_widget(p);
      if (pw && pw->scroll_state_ptr()) return p;
    }
    return 0;
  }

  // Pull whole wheel lines out of a fractional notch accumulator (truncates
  // toward zero; keeps the remainder for the next sub-notch event).
  int take_lines(double& accum, double notches)
  {
    accum += notches;
    int lines = (int)accum;   // toward zero
    accum -= lines;
    return lines;
  }

  // Feed a precise wheel delta into a scrolling SECTION's per-axis kinetics -
  // the Linux twin of section_kinetic_wheel_w32 (same shared math + tuning).
  // dv/dh are logical px in the kinetics' sign convention (positive dv = up,
  // positive dh = left). Starts the spring-back heartbeat on overscroll.
  void section_kinetic_wheel_linux(LinuxWindow* lw, uint32_t sec_idx,
                                   double dv, double dh)
  {
    using namespace neui_detail;
    auto* sw = lw->session->get_widget(sec_idx);
    SectionScrollState* st = sw ? sw->scroll_state_ptr() : nullptr;
    const SectionLayout* L = sw ? sw->section_layout_ptr() : nullptr;
    if (!st || !L) return;

    bool has_v = section_axis_has_v(st->axis);
    bool has_h = section_axis_has_h(st->axis);
    // A horizontal-only section absorbs a pure vertical wheel (classic mice
    // have no H axis); a vertical-only section ignores explicit H intent.
    if (!has_v && has_h && dh == 0.0 && dv != 0.0) { dh = dv; dv = 0.0; }

    int  mode   = section_read_kinetics_mode(sw->attrs.get());
    bool smooth = scroll_kinetics_smooth_enabled(mode,
                    /*platform_default_smooth=*/g_xi2_scroll_seen);

    bool changed = false, start_bounce = false;
    if (smooth) {
      ScrollWheelAction av{}, ah{};
      if (has_v && dv != 0.0) {
        ScrollWheelInput in; in.precise = true; in.delta_px = dv;
        av = section_scroll_wheel_kinetic(*st, *L, in, false);
      }
      if (has_h && dh != 0.0) {
        ScrollWheelInput in; in.precise = true; in.delta_px = dh;
        ah = section_scroll_wheel_kinetic(*st, *L, in, true);
      }
      changed      = av.changed      || ah.changed;
      start_bounce = av.start_bounce || ah.start_bounce;
    } else {
      if (has_v && dv != 0.0 && section_scroll_step_px(*st, *L, dv, false)) changed = true;
      if (has_h && dh != 0.0 && section_scroll_step_px(*st, *L, dh, true))  changed = true;
    }
    if (changed) { lw->needs_paint = true; sw->notify_scroll_changed(); }
    if (start_bounce) { lw->bouncing_section_index = sec_idx; arm_timer(true); }
  }

  // Route an accumulated wheel delta (in notches; +dv = up, +dh = left) at
  // logical (lx, ly). Mirrors platform_win32's WM_MOUSEWHEEL routing: combo
  // overlay first, then GRID-smooth / SECTION kinetics (pixel-precise), else a
  // classic line-quantised MOUSE_WHEEL event for stepped surfaces.
  void feed_scroll(LinuxWindow* lw, float lx, float ly, double dv, double dh)
  {
    using namespace neui_detail;
    Session* s = lw->session;
    int vline = take_lines(lw->scroll_v_accum, dv);
    int hline = take_lines(lw->scroll_h_accum, dh);

    // Combo overlay intercepts vertical wheel over its drop area.
    if (s->_open_combo && vline != 0 && s->handle_combo_wheel(lx, ly, vline)) return;

    uint32_t hit = s->widget_at(lx, ly, lw->widget_index);
    if (hit == 0) return;
    auto* hw = s->get_widget(hit);
    if (!hw) return;

    // GRID + SMOOTH: feed the pixel-precise delta into the shared kinetics.
    if (GridModel* model = hw->grid_model_ptr()) {
      auto cfg = grid_read_config(hw->attrs.get());
      if (grid_smooth_enabled(cfg, /*platform_default_smooth=*/g_xi2_scroll_seen)) {
        GridViewport vp = grid_compute_viewport(*model, hw->width, hw->height,
                                                 cfg.row_h, cfg.header_h);
        GridWheelInput in;
        in.precise  = true;
        in.delta_px = dv * (double)LINUX_WHEEL_LINES * (double)cfg.row_h;
        GridWheelAction act = grid_scroll_wheel(*model, vp, cfg.row_h, in);
        if (act.changed) lw->needs_paint = true;
        if (act.start_bounce) { lw->bouncing_grid_index = hit; arm_timer(true); }
        return;
      }
      // STEPPED grid: fall through to the classic line path below.
    }

    // Scrolling SECTION: widgets below get first refusal via a bounded bubble
    // (classic lines); when nothing consumes, the section eats it via kinetics.
    uint32_t sec_idx = find_scrolling_section_linux(s, hit);
    if (sec_idx != 0) {
      if (hit != sec_idx && (vline != 0 || hline != 0)) {
        neui_event_t ev = {};
        ev.type          = NEUI_EVENT_MOUSE_WHEEL;
        ev.data.wheel.x  = (int)lx;
        ev.data.wheel.y  = (int)ly;
        if (vline != 0) {
          ev.data.wheel.delta = vline; ev.data.wheel.is_horizontal = 0;
          if (s->dispatch_wheel_event(hit, &ev, sec_idx)) return;
        }
        if (hline != 0) {
          ev.data.wheel.delta = hline; ev.data.wheel.is_horizontal = 1;
          if (s->dispatch_wheel_event(hit, &ev, sec_idx)) return;
        }
      }
      double pv = dv * (double)LINUX_WHEEL_LINES * SECTION_WHEEL_LINE_PX;
      double ph = dh * (double)LINUX_WHEEL_LINES * SECTION_WHEEL_LINE_PX;
      section_kinetic_wheel_linux(lw, sec_idx, pv, ph);
      return;
    }

    // Classic line-quantised path (listbox / treeview / multiline / custom).
    if (vline != 0) {
      neui_event_t ev = {};
      ev.type = NEUI_EVENT_MOUSE_WHEEL;
      ev.data.wheel.x = (int)lx; ev.data.wheel.y = (int)ly;
      ev.data.wheel.delta = vline; ev.data.wheel.is_horizontal = 0;
      s->dispatch_wheel_event(hit, &ev);
    }
    if (hline != 0) {
      neui_event_t ev = {};
      ev.type = NEUI_EVENT_MOUSE_WHEEL;
      ev.data.wheel.x = (int)lx; ev.data.wheel.y = (int)ly;
      ev.data.wheel.delta = hline; ev.data.wheel.is_horizontal = 1;
      s->dispatch_wheel_event(hit, &ev);
    }
  }

  // Extract scroll-valuator deltas from an XI_Motion event and route them.
  void handle_xi2_scroll(LinuxWindow* lw, XIDeviceEvent* de)
  {
    const auto& classes = xi2_scroll_classes(de->display, de->deviceid);
    if (classes.empty()) return;

    double dv = 0.0, dh = 0.0;   // notches: +dv = up, +dh = left
    const double* vals = de->valuators.values;
    int idx = 0;
    for (int v = 0; v < de->valuators.mask_len * 8; ++v) {
      if (!XIMaskIsSet(de->valuators.mask, v)) continue;
      double value = vals[idx++];
      for (const auto& sv : classes) {
        if (sv.number != v || sv.increment == 0.0) continue;
        uint64_t key = ((uint64_t)de->deviceid << 32) | (uint32_t)v;
        auto lit = g_xi2_last_value.find(key);
        if (lit != g_xi2_last_value.end()) {
          // units > 0 = scroll down / right (server convention); flip to the
          // neui notch convention (+dv up, +dh left, matching core Button 4/6).
          double units = (value - lit->second) / sv.increment;
          if (sv.horizontal) dh -= units; else dv -= units;
        }
        g_xi2_last_value[key] = value;
        break;
      }
    }
    if (dv == 0.0 && dh == 0.0) return;
    g_xi2_scroll_seen = true;   // XI2 scroll works here -> suppress core Button 4-7

    float scale = lw->dpi / 96.0f; if (scale <= 0.0f) scale = 1.0f;
    feed_scroll(lw, de->event_x / scale, de->event_y / scale, dv, dh);
  }

  // Unpack an XI2 GenericEvent cookie and dispatch XI_Motion scroll. Returns
  // true if the event was an XI2 event we own (so the caller stops).
  bool handle_xi2_event(XEvent& ev)
  {
    if (g_xi2_opcode < 0 || ev.xcookie.extension != g_xi2_opcode) return false;
    if (!XGetEventData(ev.xcookie.display, &ev.xcookie)) return true;
    if (ev.xcookie.evtype == XI_Motion) {
      auto* de = static_cast<XIDeviceEvent*>(ev.xcookie.data);
      LinuxWindow* lw = find_window(de->event);
      if (lw) {
        // XI_Motion now owns motion delivery (it suppresses core MotionNotify):
        // drive hover/drag from it as well as scroll, both gated on the modal
        // input-block. Rebuild an X11-core button/modifier mask from the XI
        // device state so do_motion routes drags to the pressed widget.
        g_xi2_motion_seen = true;
        if (!lw->input_disabled) {
          handle_xi2_scroll(lw, de);
          unsigned int state = static_cast<unsigned int>(de->mods.effective);
          if (de->buttons.mask_len > 0 && XIMaskIsSet(de->buttons.mask, 1))
            state |= Button1Mask;
          float scale = lw->dpi / 96.0f; if (scale <= 0.0f) scale = 1.0f;
          do_motion(lw, de->event_x / scale, de->event_y / scale, state);
        }
      }
    }
    XFreeEventData(ev.xcookie.display, &ev.xcookie);
    return true;
  }
#endif  // NEUI_HAS_XI2

  // A modal child dialog is blocking this owner: raise + focus it so a click
  // on the disabled owner surfaces what's in the way (X11 has no native modal).
  void raise_modal_child(LinuxWindow* owner)
  {
    Session* s = owner->session;
    if (!s) return;
    for (auto& kv : g_windows) {
      LinuxWindow* lw = kv.second;
      if (lw == owner || lw->session != s) continue;
      auto* wd = s->get_widget(lw->widget_index);
      if (wd && wd->is_dialog() && wd->owner_index == owner->widget_index) {
        XRaiseWindow(lw->dpy, lw->win);
        XSetInputFocus(lw->dpy, lw->win, RevertToParent, CurrentTime);
      }
    }
  }

  void dispatch_x_event(XEvent& ev)
  {
    if (XFilterEvent(&ev, None)) return;   // let the IME consume composition keys
#ifdef NEUI_HAS_XI2
    // XI2 scroll arrives as a GenericEvent cookie - it carries no xany.window,
    // so resolve + dispatch it before the window lookup below.
    if (ev.type == GenericEvent && handle_xi2_event(ev)) return;
#endif
    if (g_clipboard.handle_event(ev)) return;  // CLIPBOARD selection events
    LinuxWindow* lw = find_window(ev.xany.window);
    if (!lw) return;
    Session* s = lw->session;

    // Modal block: swallow all input destined for a disabled owner window
    // (a modal child is up). A press surfaces the blocker; everything else
    // is dropped. Non-input events (paint / resize / focus / WM messages)
    // still flow so the owner keeps rendering underneath.
    if (lw->input_disabled) {
      switch (ev.type) {
        case ButtonPress:   raise_modal_child(lw); return;
        case ButtonRelease:
        case MotionNotify:
        case KeyPress:
        case KeyRelease:    return;
        default: break;
      }
    }

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
            // The in-frame menubar band reserves the top of the client area;
            // report the height below it so client layout matches Win32 (where
            // the menu is non-client). 0 when there's no menubar.
            int inset = s->frame_top_inset(lw->widget_index);
            neui_event_t re = {};
            re.type               = NEUI_EVENT_RESIZE;
            re.data.resize.widget = { wd->widget_id };
            re.data.resize.width  = wlog;
            re.data.resize.height = hlog - inset;
            s->dispatch_event(&re);
          }
        }
        lw->needs_paint = true;
        break;
      }

      case ButtonPress:   dispatch_button_press(lw, ev.xbutton);   break;
      case ButtonRelease: dispatch_button_release(lw, ev.xbutton); break;
      case MotionNotify:
        // Once XI_Motion is driving (XI2 active), it has replaced core motion;
        // skip the core event to avoid double-dispatch. Without XI2 this is the
        // only motion source.
#ifdef NEUI_HAS_XI2
        if (!g_xi2_motion_seen) dispatch_motion(lw, ev.xmotion);
#else
        dispatch_motion(lw, ev.xmotion);
#endif
        break;
      case KeyPress:      dispatch_key_press(lw, ev.xkey);          break;
      case KeyRelease:    dispatch_key_release(lw, ev.xkey);        break;

      case FocusIn:  s->_os_focused = true;  lw->needs_paint = true; break;
      case FocusOut: s->_os_focused = false; s->close_menubar_menu(); lw->needs_paint = true; break;

      case LeaveNotify:
        s->set_hovered(0);
        if (s->_menu_band_hover != 0) { s->_menu_band_hover = 0; lw->needs_paint = true; }
        break;

      case ClientMessage:
        if (xdnd_handle_client_message(ev.xclient)) break;   // XDND target messages
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

#ifdef NEUI_HAS_XI2
    // Request XI2 smooth-scroll valuators for this window (no-op if XI2 is
    // unavailable; on the embedded path `dpy` is the DAW's own connection).
    xi2_select_window(dpy, win);
#endif

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

    if (owner_lw && owner_lw->win) {
      XSetTransientForHint(dpy, win, owner_lw->win);
      // Mark owned dialogs _NET_WM_STATE_MODAL so a cooperative WM reinforces
      // the software input-block (focus/stacking); same hint the message box
      // uses. The actual input blocking is done in dispatch_x_event since not
      // every WM honours the modal state.
      Atom st = XInternAtom(dpy, "_NET_WM_STATE", False);
      Atom md = XInternAtom(dpy, "_NET_WM_STATE_MODAL", False);
      if (st != None && md != None)
        XChangeProperty(dpy, win, st, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<unsigned char*>(&md), 1);
    }

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

  // ---- Message box (neui-drawn modal; X11 has no native one). ---------------

  int run_message_box(LinuxWindow* owner, const char* text, const char* caption,
                      uint32_t flags)
  {
    if (!g_display) return 0;
    Display* d = g_display;
    auto* backend = platform_get_backend();
    if (!backend) return 0;

    neui_detail::MsgBoxSpec spec = neui_detail::msgbox_parse(flags);

    int     scr   = DefaultScreen(d);
    Window  root  = RootWindow(d, scr);
    Visual* vis   = DefaultVisual(d, scr);
    int     depth = DefaultDepth(d, scr);
    uint32_t dpi  = owner ? owner->dpi : query_display_dpi(d);
    float   scale = dpi / 96.0f; if (scale <= 0.0f) scale = 1.0f;

    // Scope the palette to the owner's session so dark/light + accent match.
    neui_detail::ScopedPaletteOverride scope(
      (owner && owner->session) ? owner->session->effective_palette_ptr() : nullptr);
    auto C = [](neui_detail::ColorRole r) {
      return neui_detail::current_palette().colors[(size_t)r];
    };
    using neui_detail::shade;   // shared ARGB channel shade (theme_palette.h)

    // Offscreen ctx for text measurement during layout (logical px).
    neui_render_ctx_t mctx = backend->create_offscreen_context(8, 8, 1.0f);
    auto meas = [&](const char* s, float sz, int weight) -> int {
      if (!mctx || !s || !*s) return 0;
      if (weight) backend->push_font(mctx, "", weight);
      int w = (int)backend->measure_text(mctx, s, -1, sz);
      if (weight) backend->pop_font(mctx);
      return w;
    };

    // Word-wrap UTF-8 `s` to maxw logical px (honours embedded '\n').
    const float SZ_TXT = 13.0f, SZ_CAP = 14.0f;
    const int   LH = 18, LH_CAP = 20, WRAP = 360;
    auto wrap = [&](const char* s, float sz, int weight, int maxw,
                    std::vector<std::string>& out) {
      if (!s || !*s) return;
      std::string cur, word;
      auto flush_word = [&]() {
        if (word.empty()) return;
        std::string trial = cur.empty() ? word : cur + " " + word;
        if (cur.empty() || meas(trial.c_str(), sz, weight) <= maxw) cur = trial;
        else { out.push_back(cur); cur = word; }
        word.clear();
      };
      for (const char* p = s;; ++p) {
        char c = *p;
        if (c == '\n' || c == 0) { flush_word(); out.push_back(cur); cur.clear();
                                   if (c == 0) break; }
        else if (c == ' ') flush_word();
        else word += c;
      }
    };

    std::vector<std::string> cap_lines, txt_lines;
    wrap(caption, SZ_CAP, 700, WRAP, cap_lines);
    wrap(text,    SZ_TXT, 0,   WRAP, txt_lines);

    // Layout (logical px).
    const int PAD = 18, GAP = 12, BTN_H = 28, BTN_GAP = 8, BTN_MINW = 84, ICON = 32;
    int text_w = 0;
    for (auto& l : cap_lines) text_w = std::max(text_w, meas(l.c_str(), SZ_CAP, 700));
    for (auto& l : txt_lines) text_w = std::max(text_w, meas(l.c_str(), SZ_TXT, 0));
    int content_h = (int)cap_lines.size() * LH_CAP + (int)txt_lines.size() * LH;
    if (!cap_lines.empty() && !txt_lines.empty()) content_h += 6;

    int bw[3] = {0,0,0}, total_b = 0;
    for (int i = 0; i < spec.count; ++i) {
      bw[i] = std::max(BTN_MINW, meas(spec.btn[i].label, SZ_TXT, 0) + 24);
      total_b += bw[i] + (i ? BTN_GAP : 0);
    }
    int icon_w   = spec.icon ? ICON + GAP : 0;
    int left_h   = std::max(content_h, spec.icon ? ICON : 0);
    int win_w = PAD * 2 + std::max(icon_w + text_w, total_b);
    int win_h = PAD * 2 + left_h + GAP + BTN_H;
    backend->destroy_context(mctx); mctx = nullptr;

    int btn_y  = win_h - PAD - BTN_H;
    int btn_x0 = win_w - PAD - total_b;
    int bx[3] = {0,0,0};
    for (int i = 0, cx = btn_x0; i < spec.count; ++i) { bx[i] = cx; cx += bw[i] + BTN_GAP; }
    auto hit_button = [&](int lx, int ly) -> int {
      if (ly < btn_y || ly >= btn_y + BTN_H) return -1;
      for (int i = 0; i < spec.count; ++i)
        if (lx >= bx[i] && lx < bx[i] + bw[i]) return i;
      return -1;
    };

    // Centre over the owner (or the screen).
    int ox = 0, oy = 0; unsigned ow = DisplayWidth(d, scr), oh = DisplayHeight(d, scr);
    if (owner) {
      Window ch; XTranslateCoordinates(d, owner->win, root, 0, 0, &ox, &oy, &ch);
      XWindowAttributes oa;
      if (XGetWindowAttributes(d, owner->win, &oa)) { ow = oa.width; oh = oa.height; }
    }
    int wpx = (int)(win_w * scale + 0.5f), hpx = (int)(win_h * scale + 0.5f);
    int x = ox + ((int)ow - wpx) / 2, y = oy + ((int)oh - hpx) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    XSetWindowAttributes swa; std::memset(&swa, 0, sizeof swa);
    swa.background_pixmap = None; swa.bit_gravity = NorthWestGravity;
    swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | KeyPressMask | StructureNotifyMask;
    Window win = XCreateWindow(d, root, x, y, wpx, hpx, 0, depth, InputOutput, vis,
                               CWBackPixmap | CWEventMask | CWBitGravity, &swa);
    if (!win) return 0;
    if (caption && *caption) set_window_title(d, win, caption);
    if (owner) XSetTransientForHint(d, win, owner->win);
    if (g_wm_delete != None) XSetWMProtocols(d, win, &g_wm_delete, 1);
    {  // mark modal so the WM blocks the owner
      Atom st = XInternAtom(d, "_NET_WM_STATE", False);
      Atom md = XInternAtom(d, "_NET_WM_STATE_MODAL", False);
      if (st != None && md != None)
        XChangeProperty(d, win, st, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<unsigned char*>(&md), 1);
    }
    if (XSizeHints* sh = XAllocSizeHints()) {  // fixed size
      sh->flags = PMinSize | PMaxSize | USPosition;
      sh->min_width = sh->max_width = wpx;
      sh->min_height = sh->max_height = hpx;
      sh->x = x; sh->y = y;
      XSetWMNormalHints(d, win, sh); XFree(sh);
    }

    neui_cairo_backend::LinuxNativeSurface ns;
    ns.dpy = d; ns.win = win; ns.visual = vis; ns.depth = depth;
    neui_render_ctx_t ctx = backend->create_context(&ns, (uint32_t)wpx, (uint32_t)hpx);
    if (!ctx) { XDestroyWindow(d, win); return 0; }
    backend->update_dpi(ctx, dpi);

    int  hover = -1, pressed = -1, result = 0;
    bool done = false, need_paint = true, focused = false;
    // Swallow transient X errors (e.g. focusing a not-yet-viewable window).
    int (*old_err)(Display*, XErrorEvent*) = XSetErrorHandler(drag_xerror);

    auto render = [&]() {
      backend->begin_frame(ctx, C(neui_detail::ColorRole::frame_bg));
      if (spec.icon) {
        uint32_t icol; const char* glyph;
        switch (spec.icon) {
          case NEUI_MB_ICONERROR:       icol = 0xFFD13438; glyph = "\xC3\x97"; break; // ×
          case NEUI_MB_ICONWARNING:     icol = 0xFFF7630C; glyph = "!";        break;
          case NEUI_MB_ICONQUESTION:    icol = 0xFF0078D4; glyph = "?";        break;
          default: /* INFORMATION */    icol = 0xFF0078D4; glyph = "i";        break;
        }
        int cx = PAD + ICON / 2, cy = PAD + ICON / 2, rr = ICON / 2;
        backend->begin_path(ctx);
        backend->arc(ctx, (float)cx, (float)cy, (float)rr, 0.0f, 6.2831853f);
        backend->close_path(ctx);
        backend->fill_path(ctx, icol);
        backend->push_font(ctx, "", 700);
        int gw = (int)backend->measure_text(ctx, glyph, -1, 18.0f);
        backend->draw_text(ctx, cx - gw / 2, PAD, gw + 4, ICON, glyph, 18.0f, 0xFFFFFFFF);
        backend->pop_font(ctx);
      }
      int tx = PAD + (spec.icon ? ICON + GAP : 0);
      int ty = PAD;
      backend->push_font(ctx, "", 700);
      for (auto& l : cap_lines) {
        backend->draw_text(ctx, tx, ty, win_w - tx - PAD, LH_CAP, l.c_str(), SZ_CAP,
                           C(neui_detail::ColorRole::text_primary));
        ty += LH_CAP;
      }
      backend->pop_font(ctx);
      if (!cap_lines.empty() && !txt_lines.empty()) ty += 6;
      for (auto& l : txt_lines) {
        backend->draw_text(ctx, tx, ty, win_w - tx - PAD, LH, l.c_str(), SZ_TXT,
                           C(neui_detail::ColorRole::text_primary));
        ty += LH;
      }
      for (int i = 0; i < spec.count; ++i) {
        uint32_t fill = C(neui_detail::ColorRole::control_bg);
        if (pressed == i)     fill = shade(fill, -18);
        else if (hover == i)  fill = shade(fill, +14);
        backend->fill_rect(ctx, (float)bx[i], (float)btn_y, (float)bw[i], (float)BTN_H, fill);
        bool def = (i == spec.def_index);
        backend->draw_rect(ctx, (float)bx[i], (float)btn_y, (float)bw[i], (float)BTN_H,
                           def ? 2.0f : 1.0f,
                           def ? C(neui_detail::ColorRole::accent)
                               : C(neui_detail::ColorRole::border));
        int lw = (int)backend->measure_text(ctx, spec.btn[i].label, -1, SZ_TXT);
        backend->draw_text(ctx, bx[i] + (bw[i] - lw) / 2, btn_y, lw + 4, BTN_H,
                           spec.btn[i].label, SZ_TXT,
                           C(neui_detail::ColorRole::text_primary));
      }
      backend->end_frame(ctx);
    };

    XMapRaised(d, win);

    while (!done) {
      if (need_paint) { render(); need_paint = false; }
      flush_pending_paints();      // keep other neui windows alive behind the modal
      XFlush(d);
      XEvent e; XNextEvent(d, &e);
      if (e.xany.window != win) { dispatch_x_event(e); continue; }
      switch (e.type) {
        case Expose:
          if (e.xexpose.count == 0) {
            need_paint = true;
            // Window is viewable now -> safe to take keyboard focus.
            if (!focused) { XSetInputFocus(d, win, RevertToParent, CurrentTime); focused = true; }
          }
          break;
        case ConfigureNotify:
          backend->resize(ctx, e.xconfigure.width, e.xconfigure.height);
          need_paint = true; break;
        case MotionNotify: {
          int h = hit_button((int)(e.xmotion.x / scale), (int)(e.xmotion.y / scale));
          if (h != hover) { hover = h; need_paint = true; }
          break;
        }
        case ButtonPress:
          if (e.xbutton.button == Button1) {
            pressed = hit_button((int)(e.xbutton.x / scale), (int)(e.xbutton.y / scale));
            need_paint = true;
          }
          break;
        case ButtonRelease:
          if (e.xbutton.button == Button1) {
            int h = hit_button((int)(e.xbutton.x / scale), (int)(e.xbutton.y / scale));
            if (h >= 0 && h == pressed) { result = spec.btn[h].id; done = true; }
            pressed = -1; need_paint = true;
          }
          break;
        case KeyPress: {
          KeySym ks = XLookupKeysym(&e.xkey, 0);
          if (ks == XK_Return || ks == XK_KP_Enter) {
            result = spec.btn[spec.def_index].id; done = true;
          } else if (ks == XK_Escape && spec.cancel_index >= 0) {
            result = spec.btn[spec.cancel_index].id; done = true;
          }
          break;
        }
        case ClientMessage:
          if (e.xclient.message_type == g_wm_protocols &&
              (Atom)e.xclient.data.l[0] == g_wm_delete) {
            result = spec.cancel_index >= 0 ? spec.btn[spec.cancel_index].id : 0;
            done = true;
          }
          break;
        default: break;
      }
    }

    backend->destroy_context(ctx);
    XDestroyWindow(d, win);
    XSync(d, False);
    XSetErrorHandler(old_err);
    return result;
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
    XSetErrorHandler(neui_xerror);   // keep foreign-window races from aborting us
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
    g_xa.intern(g_display);
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
    // The CLIPBOARD owner window lives on g_display (selections are X-server
    // global, so it serves the same server the embedded window is on). In
    // embedded mode the DAW only pumps lw->dpy, so g_display's queue - where
    // other apps' SelectionRequests for our owned clipboard arrive - would
    // never be serviced. Drain its clipboard events here so a plugin that
    // copied data can still answer pastes. Non-clipboard g_display events have
    // no registered window in embedded mode and are dropped by dispatch_x_event.
    if (g_display && g_display != lw->dpy) {
      while (XPending(g_display)) {
        XEvent ev;
        XNextEvent(g_display, &ev);
        dispatch_x_event(ev);
      }
    }
    // At most one animation tick per ~16 ms (the host calls this on its own
    // cadence; gate so a fast host doesn't over-advance toast/spring-back).
    uint64_t now = platform_now_ms();
    if (now - lw->last_tick_ms >= 16) {
      lw->last_tick_ms = now;
      if (lw->toast_anim) lw->needs_paint = true;
      step_scroll_bounce(lw);   // advance any GRID/SECTION spring-back
    }
    if (lw->needs_paint) paint_window(lw);
    XFlush(lw->dpy);
  }

  void platform_destroy_window(WidgetData& wd)
  {
    if (!wd.native_handle) return;
    auto* lw = static_cast<LinuxWindow*>(wd.native_handle);
    Session* s = lw->session;

    // Modal dialog teardown (mirror platform_win32's WM_DESTROY path): clear
    // the pump flag so platform_run_modal_until in widget_show unwinds, and
    // re-enable + raise the owner so input returns there. Must run BEFORE the
    // widget is freed (destroy_recursive removes it right after this), while
    // owner_index + the FrameWidget are still valid.
    if (s && wd.is_dialog() && wd.owner_index != 0 &&
        s->_widgets.exists(wd.owner_index)) {
      if (auto* fw = dynamic_cast<FrameWidget*>(&wd))
        fw->modal_pump_active = false;
      auto& owner = s->_widgets[wd.owner_index];
      if (owner.native_handle) {
        platform_set_window_enabled(owner.native_handle, true);
        auto* ow = static_cast<LinuxWindow*>(owner.native_handle);
        XRaiseWindow(ow->dpy, ow->win);
        XSetInputFocus(ow->dpy, ow->win, RevertToParent, CurrentTime);
      }
    }

    auto* backend = platform_get_backend();
    if (wd.render_ctx && backend) {
      if (s) s->_asset_manager.release_context(wd.render_ctx, backend);
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

  void platform_set_window_enabled(void* native_handle, bool enabled)
  {
    // X11 has no native per-window input disable, so we flag the window and
    // swallow its input in dispatch_x_event (+ the XI2 scroll path) while a
    // modal child is up. Clearing the flag restores normal input. The
    // dialog is also marked _NET_WM_STATE_MODAL (create_frame) so a
    // cooperative WM reinforces focus/stacking.
    if (!native_handle) return;
    static_cast<LinuxWindow*>(native_handle)->input_disabled = !enabled;
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

  // ---- Menu bar -------------------------------------------------------------
  // X11 has no native menu bar, so the host draws it itself inside the frame's
  // client area (Session::paint_menubar + the handle_menubar_* input path). The
  // menu *model* still lives in MenubarWidget; these platform hooks that mutate
  // a native HMENU/NSMenu are therefore no-ops EXCEPT platform_menubar_create,
  // which must return a non-null handle: widgets.cpp::t_add bails when
  // mb.hmenu is null, so a null here would leave the model empty. The handle is
  // an opaque sentinel - never dereferenced - so a fixed non-null value
  // suffices (destroy is a no-op; the model is reconstructed from parent_item_id
  // links at paint time).
  bool  platform_menubar_in_frame()                                                    { return true; }
  void* platform_menubar_create(uint32_t /*widget_id*/)                                 { return reinterpret_cast<void*>(0x1); }
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

  // ---- Drag & drop: XDND drop-target (receive). Drag-source still deferred. -

  bool platform_dnd_register_window(void* native_handle, void* session_ptr,
                                     uint32_t frame_widget_id)
  {
    if (!native_handle || !g_display) return false;
    auto* lw = static_cast<LinuxWindow*>(native_handle);
    xdnd_register(lw->dpy ? lw->dpy : g_display, lw->win, session_ptr, frame_widget_id, lw->dpi);
    return true;
  }
  void platform_dnd_unregister_window(void* native_handle)
  {
    if (!native_handle || !g_display) return;
    auto* lw = static_cast<LinuxWindow*>(native_handle);
    xdnd_unregister(lw->win);
  }
  void* platform_make_drag_preview(const uint8_t* bgra_premul,
                                    uint32_t w_px, uint32_t h_px, float scale)
  {
    if (!bgra_premul || w_px == 0 || h_px == 0) return nullptr;
    auto* p = new DragPreview();
    p->bgra.assign(bgra_premul, bgra_premul + static_cast<size_t>(w_px) * h_px * 4);
    p->w_px = w_px; p->h_px = h_px; p->scale = scale > 0.0f ? scale : 1.0f;
    return p;   // consumed by platform_dnd_begin_drag
  }

  // Blocking XDND drag-source spin. Returns the negotiated neui_dnd_action_t
  // (0 = cancelled). Foreign targets go through the XDND ClientMessage
  // handshake; our own windows (internal drags) dispatch Session::dispatch_dnd_*
  // directly, passing the DataItem straight through (no X selection round-trip,
  // which would self-deadlock against this very spin).
  uint32_t platform_dnd_begin_drag(void* native_handle, neui_detail::DataItem* item,
                                   uint32_t allowed_actions, void* preview_native,
                                   int hot_x, int hot_y)
  {
    std::unique_ptr<DragPreview> preview(static_cast<DragPreview*>(preview_native));
    if (!native_handle || !item || !g_display) return 0;
    Display* d = g_display;
    auto* lw = static_cast<LinuxWindow*>(native_handle);
    Window   src  = lw->win;
    Window   root = DefaultRootWindow(d);
    if (allowed_actions == 0)
      allowed_actions = NEUI_DND_ACTION_COPY | NEUI_DND_ACTION_MOVE;

    // Offered target atoms + the MIME list (for internal dispatch).
    std::vector<Atom>        offered;
    std::vector<std::string> mimes;
    auto add_atom = [&](Atom a) {
      for (Atom x : offered) if (x == a) return;
      offered.push_back(a);
    };
    item->for_each_mime([&](const std::string& mime) {
      mimes.push_back(mime);
      if (mime == "text/plain;charset=utf-8" || mime == "text/plain") {
        add_atom(g_xa.utf8);
        add_atom(XA_STRING);
        add_atom(XInternAtom(d, "text/plain;charset=utf-8", False));
      } else {
        add_atom(XInternAtom(d, mime.c_str(), False));
      }
    });
    std::vector<const char*> mptrs;
    for (auto& m : mimes) mptrs.push_back(m.c_str());
    const char* const* fmts = mptrs.empty() ? nullptr : mptrs.data();
    uint32_t fcount = (uint32_t)mptrs.size();

    XSetSelectionOwner(d, g_xa.selection, src, CurrentTime);
    if (offered.size() > 3)
      XChangeProperty(d, src, g_xa.type_list, XA_ATOM, 32, PropModeReplace,
                      reinterpret_cast<unsigned char*>(offered.data()), (int)offered.size());

    // Grab on the root window (always viewable; the source frame can be
    // momentarily non-viewable during WM reparenting). owner_events=False
    // routes every pointer event to us in root coordinates regardless.
    if (XGrabPointer(d, root, False,
          ButtonReleaseMask | ButtonMotionMask | PointerMotionMask,
          GrabModeAsync, GrabModeAsync, None, None, CurrentTime) != GrabSuccess)
      return 0;
    XGrabKeyboard(d, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);

    int (*old_err)(Display*, XErrorEvent*) = XSetErrorHandler(drag_xerror);

    // Preview hot-spot (physical px) + follow window.
    int hx = (hot_x < 0) ? (int)(preview ? preview->w_px / 2 : 0)
                         : (int)(hot_x * (preview ? preview->scale : 1.0f));
    int hy = (hot_y < 0) ? (int)(preview ? preview->h_px / 2 : 0)
                         : (int)(hot_y * (preview ? preview->scale : 1.0f));
    Window  pvwin = None; GC pvgc = nullptr; XImage* pvimg = nullptr;
    if (preview) {
      int scr = DefaultScreen(d);
      XSetWindowAttributes swa; std::memset(&swa, 0, sizeof swa);
      swa.override_redirect = True; swa.save_under = True; swa.background_pixmap = None;
      pvwin = XCreateWindow(d, root, 0, 0, preview->w_px, preview->h_px, 0,
                            DefaultDepth(d, scr), InputOutput, DefaultVisual(d, scr),
                            CWOverrideRedirect | CWSaveUnder | CWBackPixmap, &swa);
      pvgc  = XCreateGC(d, pvwin, 0, nullptr);
      pvimg = XCreateImage(d, DefaultVisual(d, scr), DefaultDepth(d, scr), ZPixmap, 0,
                           reinterpret_cast<char*>(preview->bgra.data()),
                           preview->w_px, preview->h_px, 32, (int)preview->w_px * 4);
      XMapRaised(d, pvwin);
    }
    auto put_preview = [&](int rx, int ry) {
      if (!pvwin) return;
      XMoveWindow(d, pvwin, rx - hx, ry - hy);
      if (pvimg) XPutImage(d, pvwin, pvgc, pvimg, 0, 0, 0, 0, preview->w_px, preview->h_px);
    };

    // Drag state.
    Window   cur = None; bool cur_internal = false, cur_entered = false;
    Session* cs = nullptr; uint32_t cframe = 0, cdpi = 96; long cver = 5;
    bool     accept = false; uint32_t neg_action = 0;
    bool     dropped = false, finished = false; uint32_t result = 0;

    {  // prime preview at the current pointer
      Window rr, ch; int rx, ry, wx, wy; unsigned mask;
      XQueryPointer(d, root, &rr, &ch, &rx, &ry, &wx, &wy, &mask);
      put_preview(rx, ry);
    }

    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    struct timespec t_drop = {0, 0}; bool waiting_finish = false;

    // Cache the topmost toplevel under the pointer + its rect, so a stream of
    // motion events inside the same window skips the full XQueryTree scan
    // (dozens of round-trips on a busy desktop). The cheap aware-descent still
    // runs every motion, so sub-window targeting stays correct.
    Window    top_cache = None; XRectangle top_rect{0, 0, 0, 0}; bool top_valid = false;

    while (!finished) {
      if (XPending(d)) {
        XEvent e; XNextEvent(d, &e);

        if (e.type == MotionNotify) {
          int rx = e.xmotion.x_root, ry = e.xmotion.y_root;
          uint32_t proposed = neui_detail::dnd_suggest_action(
            allowed_actions, e.xmotion.state & ControlMask, e.xmotion.state & ShiftMask);
          put_preview(rx, ry);
          if (waiting_finish) continue;

          long ver = 5;
          Window top;
          if (top_valid && rx >= top_rect.x && ry >= top_rect.y &&
              rx < top_rect.x + top_rect.width && ry < top_rect.y + top_rect.height) {
            top = top_cache;                          // still inside the cached toplevel
          } else {
            top = xdnd_topmost_at(rx, ry, pvwin, &top_rect);
            top_cache = top; top_valid = (top != None);
          }
          Window tw = (top == None) ? None : xdnd_choose_aware(top, rx, ry, &ver);
          bool tw_internal = (tw != None) && (g_drop_targets.count(tw) > 0);
          if (tw != cur) {
            if (cur_internal && cs) cs->dispatch_dnd_leave();
            else if (cur != None && !cur_internal) xdnd_send(cur, g_xa.leave, (long)src, 0, 0, 0, 0);
            cur = tw; cur_internal = tw_internal; cur_entered = false;
            accept = false; neg_action = 0;
            if (tw_internal) {
              auto& dt = g_drop_targets[tw];
              cs = static_cast<Session*>(dt.session); cframe = dt.frame_id & 0xFFFFu; cdpi = dt.dpi;
            } else if (tw != None) {
              cver = ver;
              long l1 = ((long)neui_detail::kXdndVersion << 24) | (offered.size() > 3 ? 1L : 0L);
              xdnd_send(tw, g_xa.enter, (long)src, l1,
                        offered.size() > 0 ? (long)offered[0] : 0,
                        offered.size() > 1 ? (long)offered[1] : 0,
                        offered.size() > 2 ? (long)offered[2] : 0);
            }
          }
          if (cur_internal && cs) {
            int lx, ly; Window ch; XTranslateCoordinates(d, root, cur, rx, ry, &lx, &ly, &ch);
            float sc = cdpi / 96.0f; if (sc <= 0.0f) sc = 1.0f;
            int llx = (int)(lx / sc), lly = (int)(ly / sc);
            uint32_t a = cur_entered
              ? cs->dispatch_dnd_move (cframe, llx, lly, fmts, fcount, proposed, 0)
              : cs->dispatch_dnd_enter(cframe, llx, lly, fmts, fcount, proposed, 0);
            cur_entered = true; accept = a != 0; neg_action = a;
          } else if (cur != None) {
            xdnd_send(cur, g_xa.position, (long)src, 0,
                      ((long)rx << 16) | (ry & 0xFFFF), CurrentTime,
                      (long)neui_detail::xdnd_neui_to_action(g_xa, proposed));
          }
        }
        else if (e.type == ButtonRelease && e.xbutton.button == Button1) {
          int rx = e.xbutton.x_root, ry = e.xbutton.y_root;
          uint32_t proposed = neui_detail::dnd_suggest_action(
            allowed_actions, e.xbutton.state & ControlMask, e.xbutton.state & ShiftMask);
          if (cur_internal && cs && accept) {
            int lx, ly; Window ch; XTranslateCoordinates(d, root, cur, rx, ry, &lx, &ly, &ch);
            float sc = cdpi / 96.0f; if (sc <= 0.0f) sc = 1.0f;
            result = cs->dispatch_dnd_drop(cframe, (int)(lx / sc), (int)(ly / sc),
                                           fmts, fcount, proposed, 0, item);
            finished = true;
          } else if (cur != None && !cur_internal && accept) {
            xdnd_send(cur, g_xa.drop, (long)src, 0, CurrentTime, 0, 0);
            dropped = true; waiting_finish = true;
            clock_gettime(CLOCK_MONOTONIC, &t_drop);
          } else {
            if (cur != None && !cur_internal) xdnd_send(cur, g_xa.leave, (long)src, 0, 0, 0, 0);
            result = 0; finished = true;
          }
        }
        else if (e.type == KeyPress) {
          if (XLookupKeysym(&e.xkey, 0) == XK_Escape) {
            if (cur != None && !cur_internal) xdnd_send(cur, g_xa.leave, (long)src, 0, 0, 0, 0);
            result = 0; finished = true;
          }
        }
        else if (e.type == SelectionRequest && e.xselectionrequest.owner == src) {
          xdnd_serve_source_selection(e.xselectionrequest, item, offered);
        }
        else if (e.type == ClientMessage && e.xclient.window == src &&
                 e.xclient.message_type == g_xa.status) {
          // data.l[0] = the target that sent this status. Ignore a late status
          // from a target we've already left, or it would set a stale accept
          // and we'd drop on a window that never agreed to it.
          if ((Window)e.xclient.data.l[0] == cur) {
            accept = (e.xclient.data.l[1] & 1) != 0;
            neg_action = neui_detail::xdnd_action_to_neui(g_xa, (Atom)e.xclient.data.l[4]);
          }
        }
        else if (e.type == ClientMessage && e.xclient.window == src &&
                 e.xclient.message_type == g_xa.finished) {
          bool acc = (cver >= 5) ? ((e.xclient.data.l[1] & 1) != 0) : true;
          Atom aa  = (Atom)e.xclient.data.l[2];
          result = acc ? (aa ? neui_detail::xdnd_action_to_neui(g_xa, aa) : neg_action) : 0;
          finished = true;
        }
        else {
          dispatch_x_event(e);   // exposes / resizes keep windows painting
        }
      } else {
        usleep(2000);
      }

      struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
      long ms = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000;
      if (ms > 60000) { result = dropped ? neg_action : 0; finished = true; }
      if (waiting_finish) {
        long dms = (now.tv_sec - t_drop.tv_sec) * 1000 + (now.tv_nsec - t_drop.tv_nsec) / 1000000;
        if (dms > 4000) { result = neg_action; finished = true; }  // assume accepted
      }
    }

    XUngrabPointer(d, CurrentTime);
    XUngrabKeyboard(d, CurrentTime);
    if (pvwin) {
      if (pvimg) { pvimg->data = nullptr; XDestroyImage(pvimg); }  // data owned by `preview`
      if (pvgc) XFreeGC(d, pvgc);
      XDestroyWindow(d, pvwin);
    }
    XSync(d, False);
    XSetErrorHandler(old_err);
    return result;
  }

  void platform_set_cursor(int kind)
  {
    // Called from the GRID header-divider hover on every motion event; an
    // XDefineCursor is a round-trip, so no-op when the shape hasn't changed.
    if (kind == g_cursor_kind) return;
    g_cursor_kind = kind;
    // X cursors are sticky per window until changed, so set it on every one of
    // our windows (small count) rather than tracking which is under the pointer.
    for (auto& kv : g_windows) {
      LinuxWindow* lw = kv.second;
      if (!lw->dpy || !lw->win) continue;
      if (kind == NEUI_CURSOR_EW_RESIZE) {
        Cursor& c = g_ew_cursors[lw->dpy];
        if (!c) c = XCreateFontCursor(lw->dpy, XC_sb_h_double_arrow);
        XDefineCursor(lw->dpy, lw->win, c);
      } else {
        XUndefineCursor(lw->dpy, lw->win);   // revert to the inherited arrow
      }
      XFlush(lw->dpy);
    }
  }

  void platform_start_toast_animation(void* native_handle)
  {
    if (!native_handle) return;
    static_cast<LinuxWindow*>(native_handle)->toast_anim = true;
    ensure_timerfd();
    arm_timer(true);   // turn the heartbeat on for the duration of the toast
  }
  void platform_stop_toast_animation(void* native_handle)
  {
    if (!native_handle) return;
    static_cast<LinuxWindow*>(native_handle)->toast_anim = false;
    if (!any_window_animating()) arm_timer(false);   // back to idle: no wakeups
  }

  uint64_t platform_now_ms()
  {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ull +
           static_cast<uint64_t>(ts.tv_nsec) / 1000000ull;
  }

  int platform_message_box(void* native_handle, const char* text,
                           const char* caption, uint32_t flags)
  {
    return run_message_box(static_cast<LinuxWindow*>(native_handle), text, caption, flags);
  }

} // namespace xpl_host
