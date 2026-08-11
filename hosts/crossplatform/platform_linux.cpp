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
#include "../shared/file_dialog_model.h"
#include "../shared/linux/file_dialog_portal.h"
#include "../shared/text_edit.h"           // name field in the drawn browser
#include "../shared/theme_palette.h"
#include "../shared/linux/theme_provider_linux.h"
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
#include <sys/stat.h>
#include <dirent.h>          // neui-drawn file browser directory listing
#include <pwd.h>             // $HOME fallback for the browser's start folder
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
  // Active neui-drawn modal message-box window (on g_display), 0 if none. A
  // click on the input-blocked owner re-raises it so a click-to-raise WM can't
  // bury the modal behind its parent. (DIALOG modals use raise_modal_child
  // instead, since they're real widgets in the tree.)
  Window   g_modal_box_win    = 0;

  Atom g_wm_delete    = None;   // WM_DELETE_WINDOW
  Atom g_wm_protocols = None;
  Atom g_net_wm_name  = None;
  Atom g_utf8_string  = None;
  Atom g_motif_hints  = None;

  // CLIPBOARD-selection owner/requestor (X11 selections, hosts/shared/linux).
  neui_detail::ClipboardX11 g_clipboard;

  // Active cursor shape (enum neui_cursor_kind) + the per-Display cursor cache.
  // Cursors are a per-connection resource, so embedded windows on their own
  // Display get their own entry; the inner map is keyed by kind because
  // XCreateFontCursor is a server round-trip we only want to pay once per
  // shape per connection.
  int g_cursor_kind = NEUI_CURSOR_DEFAULT;
  std::unordered_map<Display*, std::unordered_map<int, Cursor>> g_cursors;

  // Relative (unbounded) pointer mode - see platform_begin_relative_pointer.
  // Declared up here with the other module state because dispatch_motion (far
  // above the implementation) reads all four.
  //
  // Process-wide, not per-window: one pointer, so one relative drag at a time.
  // g_relative_window pins the mode to the window that started it, so a motion
  // event arriving for a DIFFERENT window of ours is handled normally rather
  // than being mistaken for drag motion. The anchor is in ROOT (screen) px,
  // which is what XWarpPointer and MotionNotify's x_root/y_root both speak.
  struct LinuxWindow;
  bool         g_relative_active   = false;
  LinuxWindow* g_relative_window   = nullptr;
  int          g_relative_anchor_x = 0;
  int          g_relative_anchor_y = 0;
  // Previous pointer position in ROOT px, for event-to-event deltas. Anchor-
  // relative deltas double-count whenever more than one MotionNotify is queued
  // before the asynchronous warp lands - see dispatch_motion.
  int          g_relative_last_x   = 0;
  int          g_relative_last_y   = 0;

  // The X font-cursor glyph for a kind. X11 is the richest of the three
  // platforms here - the cursor font has a direct equivalent for every kind
  // except "none", which is a mode (an empty pixmap, built separately).
  unsigned int x11_cursor_glyph(int kind)
  {
    switch (kind) {
      case NEUI_CURSOR_IBEAM:       return XC_xterm;
      case NEUI_CURSOR_CROSSHAIR:   return XC_crosshair;
      case NEUI_CURSOR_HAND:        return XC_hand2;
      case NEUI_CURSOR_OPEN_HAND:   return XC_hand1;
      // The cursor font has no closed/grabbing hand; the four-way move glyph
      // is what GTK and Qt both fall back to for an in-progress grab.
      case NEUI_CURSOR_CLOSED_HAND: return XC_fleur;
      case NEUI_CURSOR_EW_RESIZE:   return XC_sb_h_double_arrow;
      case NEUI_CURSOR_NS_RESIZE:   return XC_sb_v_double_arrow;
      // The cursor font has corner glyphs, not diagonal double-arrows. A
      // bottom-left corner reads as the NESW axis and vice versa.
      case NEUI_CURSOR_NESW_RESIZE: return XC_bottom_left_corner;
      case NEUI_CURSOR_NWSE_RESIZE: return XC_bottom_right_corner;
      case NEUI_CURSOR_MOVE:        return XC_fleur;
      case NEUI_CURSOR_WAIT:        return XC_watch;
      case NEUI_CURSOR_PROGRESS:    return XC_watch;   // no separate glyph
      case NEUI_CURSOR_HELP:        return XC_question_arrow;
      case NEUI_CURSOR_NOT_ALLOWED: return XC_X_cursor;
      case NEUI_CURSOR_ARROW:
      default:                      return XC_left_ptr;
    }
  }

  // A fully transparent 1x1 cursor - X11's only way to hide the pointer over a
  // window. Cached like the others: creating one per motion event would leak a
  // server resource per call.
  Cursor x11_make_empty_cursor(Display* dpy, Window win)
  {
    char zero[8] = {0};   // 1x1 needs 1 byte, but 8 keeps XCreateBitmapFromData happy
    Pixmap bm = XCreateBitmapFromData(dpy, win, zero, 1, 1);
    if (!bm) return None;
    XColor black = {};
    Cursor c = XCreatePixmapCursor(dpy, bm, bm, &black, &black, 0, 0);
    XFreePixmap(dpy, bm);
    return c;
  }

  // The cached Cursor for (Display, kind), creating it on first use.
  Cursor x11_cursor_for(Display* dpy, Window win, int kind)
  {
    auto& per_dpy = g_cursors[dpy];
    auto  it      = per_dpy.find(kind);
    if (it != per_dpy.end()) return it->second;

    Cursor c = neui_detail::cursor_kind_is_hidden(kind)
                 ? x11_make_empty_cursor(dpy, win)
                 : XCreateFontCursor(dpy, x11_cursor_glyph(kind));
    per_dpy[kind] = c;
    return c;
  }

  // Per-frame window record, stashed on WidgetData::native_handle.
  struct LinuxWindow
  {
    // The logical size WE last asked X for, or 0 when none is outstanding.
    // ConfigureNotify is asynchronous, so unlike win32/macOS a set-and-clear
    // flag around the request would never still be live when the notify lands.
    // Instead the notify snaps to this value when the size it derives is within
    // rounding distance: `logical -> physical -> logical` is lossy at fractional
    // zoom (402 px at zoom 0.75 round-trips to 403), and accepting the drifted
    // value would corrupt wd.width and fire a spurious RESIZE.
    int expect_w = 0;
    int expect_h = 0;
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

  // Physical pixels per logical pixel for this window: the display's DPI ratio
  // times the frame's user zoom (NEUI_ATTR_UI_SCALE). THE conversion constant
  // for this platform layer - X gives us physical pixels, while the widget
  // tree, the cached abs_x/abs_y and the paint walk are all logical, so every
  // native->logical divide and logical->native multiply goes through here.
  // Never returns <= 0.
  float window_scale(const LinuxWindow* lw)
  {
    if (!lw) return 1.0f;
    float s = static_cast<float>(lw->dpi) / 96.0f;
    if (!(s > 0.0f)) s = 1.0f;
    if (lw->session) {
      if (auto* wd = lw->session->get_widget(lw->widget_index))
        s *= wd->ui_scale();
    }
    return (s > 0.0f) ? s : 1.0f;
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

  // Sessions with live NEUI_API_TIMER timers, and the shortest interval any of
  // them wants. The heartbeat is SHARED with the animation ticks, so the armed
  // period is the minimum of the two demands - a client asking for 8 ms must not
  // be slowed to the animation's 16 ms, and an animation must not be starved by
  // a client's 1000 ms timer.
  // Immortal (leaked) rather than a destructible global: the `sessions` vector
  // lives in host.cpp, so the relative static-destruction order across the two
  // TUs is unspecified, and ~Session calls platform_timer_stop(). A leaked map
  // at exit costs nothing; a destroyed one is a crash.
  std::unordered_map<Session*, uint32_t>& client_timer_sessions()
  {
    static auto* m = new std::unordered_map<Session*, uint32_t>();
    return *m;
  }

  uint32_t client_timer_min_ms()
  {
    uint32_t best = 0;
    for (auto& kv : client_timer_sessions())
      if (best == 0 || kv.second < best) best = kv.second;
    return best;
  }

  // The period the timerfd is currently armed at (0 = disarmed). Tracked so a
  // re-arm can be skipped when the demand has not moved: timerfd_settime RESETS
  // the phase, so calling it every tick would turn the period into
  // `period + handler time` and drift the animation heartbeat as well.
  uint32_t g_armed_period_ms = 0;

  // Arm the heartbeat at `period_ms`, or disarm when 0. Idempotent.
  void arm_timer_ms(uint32_t period_ms)
  {
    if (g_timerfd < 0) return;
    if (period_ms == g_armed_period_ms) return;   // already correct - don't re-phase
    g_armed_period_ms = period_ms;
    struct itimerspec its; std::memset(&its, 0, sizeof its);
    if (period_ms > 0) {
      its.it_interval.tv_sec  = period_ms / 1000;
      its.it_interval.tv_nsec = (long)(period_ms % 1000) * 1000 * 1000;
      its.it_value = its.it_interval;
    }
    timerfd_settime(g_timerfd, 0, &its, nullptr);
  }

  bool any_window_animating();

  // Single place that decides the heartbeat period from both demands. Every
  // start/stop path routes through here so the two can never fight.
  void refresh_timer_arm()
  {
    const uint32_t client = client_timer_min_ms();
    const uint32_t anim   = any_window_animating() ? 16u : 0u;
    uint32_t period = 0;
    if (client && anim) period = (client < anim) ? client : anim;
    else                period = client ? client : anim;
    arm_timer_ms(period);
  }

  // Back-compat shim for the animation call sites: they only ever meant
  // "animation wants / no longer wants the heartbeat".
  void arm_timer(bool /*on*/) { refresh_timer_arm(); }

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
    // Client timers ride the same heartbeat. Snapshot first: a handler may
    // add or remove a session's timers, which mutates the map we are walking.
    if (!client_timer_sessions().empty()) {
      std::vector<Session*> due;
      due.reserve(client_timer_sessions().size());
      for (auto& kv : client_timer_sessions()) due.push_back(kv.first);
      for (Session* s : due)
        if (client_timer_sessions().count(s)) s->tick_client_timers();
    }
    // Drop back to a blocking select() once NEITHER demand is live (no 60 Hz
    // wakeups). Catches the bounce-settled case; the client-timer paths call
    // refresh_timer_arm themselves.
    refresh_timer_arm();
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
      g_cursors.erase(lw->dpy);
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
    // An open tree popup absorbs the wheel. Without this a scrolling SECTION
    // or GRID *under* the visible menu scrolls away beneath it, which no OS
    // menu does. No scroll-the-menu behaviour yet - a cascade taller than the
    // frame clamps (see docs/deferred-issues.md).
    if (s->_tree_popup_active) return;
    float lx = static_cast<float>(be.x) / scale, ly = static_cast<float>(be.y) / scale;
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
    ev.data.wheel.buttonmap     = neui_detail::x11_buttonmap(be.state);
    s->dispatch_wheel_event(hit, &ev);   // bubbles to scrolling ancestors
  }

  void dispatch_button_press(LinuxWindow* lw, XButtonEvent& be)
  {
    Session* s = lw->session;
    float scale = window_scale(lw);
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

    float lx = static_cast<float>(be.x) / scale, ly = static_cast<float>(be.y) / scale;

    if (be.button == 1) {
      // Overlay pre-checks (mirror macOS mouseDown: order).
      // Standalone tree popup first, and BEFORE the double-click detection
      // below, so a double-click inside an open menu picks the row it lands on
      // instead of bypassing the menu entirely.
      s->tree_popup_discard_pending_release();
      if (s->_tree_popup_active && s->handle_tree_popup_click(lw->widget_index, lx, ly))
        return;   // release swallowed via Session::tree_popup_take_release below
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

    if (be.button == 2) {
      // Middle-click paste (X PRIMARY): into the text widget under the cursor.
      // An open menu owns the button first - otherwise the paste lands in a text
      // widget UNDER the visible menu and the menu stays up.
      s->tree_popup_discard_pending_release();
      if (s->_tree_popup_active) { s->close_tree_popup(); return; }
      if (s->_menu_open) { s->close_menubar_menu(); return; }
      uint32_t hit = s->widget_at(lx, ly, lw->widget_index);
      auto* hw = s->get_widget(hit);
      if (!hw || !hw->is_text_input()) return;
      int n = platform_clipboard_get_primary(nullptr, 0);
      if (n <= 1) return;   // empty PRIMARY (n counts the NUL)
      // Position the caret at the click first (reuse the widget's BUTTON_DOWN
      // caret-placement), then insert the PRIMARY text there.
      s->set_focus(hit);
      send_mouse(lw, NEUI_EVENT_MOUSE_BUTTON_DOWN, hit, lx, ly, be.state, 0);
      std::vector<char> buf(static_cast<size_t>(n));
      platform_clipboard_get_primary(buf.data(), n);
      hw->insert_text(std::string(buf.data(), static_cast<size_t>(n - 1)));
      return;
    }

    if (be.button == 3) {
      // A right-click while a tree popup is open goes to the menu (pick /
      // descend / dismiss) rather than stacking a second one on top.
      s->tree_popup_discard_pending_release();
      if (s->_tree_popup_active && s->handle_tree_popup_click(lw->widget_index, lx, ly))
        return;   // release swallowed via Session::tree_popup_take_release below
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
    float scale = window_scale(lw);
    float lx = static_cast<float>(be.x) / scale, ly = static_cast<float>(be.y) / scale;

    if (be.button == 1) {
      // A press the tree popup consumed owns its release too, or the widget
      // under the dismissed menu sees an UP with no DOWN (and a CLICK).
      if (s->tree_popup_take_release()) return;
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
      if (s->tree_popup_take_release()) return;
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
    // A left-button drag is in progress whenever there's a pressed widget (set
    // from the core ButtonPress, cleared on release). XI_Motion's de->buttons
    // isn't reliably populated, so fold that in - otherwise the move carries no
    // Button1Mask and drag-driven widgets (SLIDER / KNOB) see it as a hover and
    // end the drag. Harmless on the core path (the bit is already set there).
    if (s->_pressed_widget != 0) state |= Button1Mask;
    if (s->_tree_popup_active && s->handle_tree_popup_hover(lw->widget_index, lx, ly))
      return;
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
    // Relative (unbounded) pointer mode. Like win32 and unlike macOS, X11 has no
    // decouple-cursor-from-device primitive, so the pointer is warped back to
    // the anchor after every move - and XWarpPointer generates a fresh
    // MotionNotify for the warp itself, which must be dropped or the handler
    // sees delta then -delta and the drag sits still.
    //
    // The anchor is in ROOT (screen) coordinates because that is what
    // XWarpPointer takes; me.x_root/y_root are the matching space.
    if (g_relative_active && lw == g_relative_window) {
      if (neui_detail::relative_is_warp_echo(me.x_root, me.y_root,
                                               g_relative_anchor_x,
                                               g_relative_anchor_y)) {
        // Our own warp-back landed. Rebase so the next real event measures from
        // the anchor rather than from wherever the pointer was before the warp.
        g_relative_last_x = g_relative_anchor_x;
        g_relative_last_y = g_relative_anchor_y;
        return;
      }

      // Delta is measured EVENT-TO-EVENT, not from the anchor. XWarpPointer is
      // asynchronous (no round-trip), so several MotionNotify can be queued
      // before the warp takes effect - routine during a fast drag, and certain
      // whenever the client lags a paint behind, since every relative move
      // repaints the knob. Anchor-relative deltas then double-count: queued
      // events at a+d1 and a+d1+d2 would report d1 and then d1+d2, so d1 lands
      // twice and the control over-travels in proportion to queue depth.
      const float scale = window_scale(lw);
      if (Session* s = lw->session) {
        s->dispatch_relative_motion(
          static_cast<float>(me.x_root - g_relative_last_x) / scale,
          static_cast<float>(me.y_root - g_relative_last_y) / scale,
          neui_detail::x11_buttonmap(me.state));
      }
      g_relative_last_x = me.x_root;
      g_relative_last_y = me.y_root;

      // Warp back onto the anchor. Uses the EVENT's own root window, not
      // DefaultRootWindow: an embedded frame runs on the DAW's Display, where
      // the default screen's root may not be the one this pointer is on.
      XWarpPointer(lw->dpy, None, me.root, 0, 0, 0, 0,
                    g_relative_anchor_x, g_relative_anchor_y);
      XFlush(lw->dpy);
      return;
    }

    float scale = window_scale(lw);
    do_motion(lw, static_cast<float>(me.x) / scale, static_cast<float>(me.y) / scale, me.state);
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

    // Esc dismisses an open tree popup, before anything else claims the key.
    if (s->handle_tree_popup_key(keycode)) return;

    // Tab cycles logical focus (hand-rolled traversal, like win32/macOS).
    if (keycode == NEUI_KEY_TAB) {
      // Pass the frame that received the key (see Session::focus_next).
      s->focus_next(!(mods & NEUI_KMOD_SHIFT), lw->widget_index);
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
    float scale = static_cast<float>(dt.dpi) / 96.0f; if (scale <= 0.0f) scale = 1.0f;
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
      g_xdrag.last_lx = (int)(static_cast<float>(lx) / scale);
      g_xdrag.last_ly = (int)(static_cast<float>(ly) / scale);

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
        std::vector<uint8_t> bytes(static_cast<size_t>(nb > 0 ? nb : 0));
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
  // `state` is an X11 modifier/button mask (ShiftMask / ControlMask /
  // Button1Mask ...) forwarded into the wheel payload's NEUI_MK_* buttonmap.
  // The XI2 caller passes XIModifierState::effective, which shares the core
  // modifier bit layout but carries no button bits - a wheel notch mid-drag
  // therefore reports modifiers only on that path.
  void feed_scroll(LinuxWindow* lw, float lx, float ly, double dv, double dh,
                    unsigned int state)
  {
    using namespace neui_detail;
    const uint32_t mk = x11_buttonmap(state);
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
        ev.data.wheel.buttonmap = mk;
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
      ev.data.wheel.buttonmap = mk;
      s->dispatch_wheel_event(hit, &ev);
    }
    if (hline != 0) {
      neui_event_t ev = {};
      ev.type = NEUI_EVENT_MOUSE_WHEEL;
      ev.data.wheel.x = (int)lx; ev.data.wheel.y = (int)ly;
      ev.data.wheel.delta = hline; ev.data.wheel.is_horizontal = 1;
      ev.data.wheel.buttonmap = mk;
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

    // window_scale() folds the frame zoom into the DPI factor (the zoom work
    // replaced every open-coded dpi/96 with it); mods.effective carries the
    // NEUI_MK_* modifier bits for the wheel payload.
    float scale = window_scale(lw);
    feed_scroll(lw, static_cast<float>(de->event_x) / scale, static_cast<float>(de->event_y) / scale, dv, dh,
                 static_cast<unsigned int>(de->mods.effective));
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
          // Hover only. While a button is held the core implicit grab delivers
          // the drag as core MotionNotify (handled above), so skip XI motion
          // then to avoid double-dispatching the move.
          if (lw->session->_pressed_widget == 0) {
            unsigned int state = static_cast<unsigned int>(de->mods.effective);
            float scale = window_scale(lw);
            do_motion(lw, static_cast<float>(de->event_x) / scale, static_cast<float>(de->event_y) / scale, state);
          }
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
        case ButtonPress:
          // Surface whatever modal is blocking this owner: a DIALOG child, or
          // a neui-drawn message box (re-raise + refocus so a click-to-raise WM
          // can't leave it buried behind the parent).
          raise_modal_child(lw);
          if (g_modal_box_win) {
            XRaiseWindow(g_display, g_modal_box_win);
            XSetInputFocus(g_display, g_modal_box_win, RevertToParent, CurrentTime);
          }
          return;
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
          float scale = window_scale(lw);
          int wlog = static_cast<int>(static_cast<float>(wphys) / scale + 0.5f);
          int hlog = static_cast<int>(static_cast<float>(hphys) / scale + 0.5f);
          // Snap back to what we asked for when this notify is the echo of our
          // own request, so a fractional-zoom round-trip cannot drift the
          // logical size (and so a zoom change reports no resize at all).
          if (lw->expect_w > 0 && wlog >= lw->expect_w - 1 && wlog <= lw->expect_w + 1 &&
              hlog >= lw->expect_h - 1 && hlog <= lw->expect_h + 1) {
            wlog = lw->expect_w;
            hlog = lw->expect_h;
            lw->expect_w = lw->expect_h = 0;   // consumed
          }
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
        // Once XI_Motion is driving (XI2 active) it replaces core motion for
        // hover, so skip the duplicate core event. BUT a core ButtonPress
        // establishes an implicit CORE grab that delivers MotionNotify (not
        // XI_Motion) for the drag - so still process core motion whenever a
        // mouse button is held, else drags (SLIDER / KNOB / text select) get no
        // motion at all. Without XI2 this is the only motion source.
#ifdef NEUI_HAS_XI2
        if (!g_xi2_motion_seen ||
            (ev.xmotion.state & (Button1Mask | Button2Mask | Button3Mask)))
          dispatch_motion(lw, ev.xmotion);
#else
        dispatch_motion(lw, ev.xmotion);
#endif
        break;
      case KeyPress:      dispatch_key_press(lw, ev.xkey);          break;
      case KeyRelease:    dispatch_key_release(lw, ev.xkey);        break;

      case FocusIn:  s->_os_focused = true;  lw->needs_paint = true; break;
      case FocusOut:
        s->_os_focused = false;
        // Both menu surfaces must go, and close_tree_popup explicitly: the popup
        // borrows _menu_path, so close_menubar_menu would CLEAR the path while
        // leaving _tree_popup_active set - a popup with no cascade to build.
        s->close_menubar_menu();
        s->close_tree_popup();
        lw->needs_paint = true;
        break;

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
    float    scale = static_cast<float>(dpi) / 96.0f; if (scale <= 0.0f) scale = 1.0f;
    // Fold in the frame's zoom (the window doesn't exist yet, so window_scale
    // isn't usable here - read the attr straight off the frame).
    scale *= wd.ui_scale();
    if (!(scale > 0.0f)) scale = 1.0f;
    int w_phys = static_cast<int>(static_cast<float>(wd.width)  * scale + 0.5f); if (w_phys < 1) w_phys = 1;
    int h_phys = static_cast<int>(static_cast<float>(wd.height) * scale + 0.5f); if (h_phys < 1) h_phys = 1;
    int x_phys = static_cast<int>(static_cast<float>(wd.x) * scale + 0.5f);
    int y_phys = static_cast<int>(static_cast<float>(wd.y) * scale + 0.5f);

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

    Window win = XCreateWindow(dpy, root, x_phys, y_phys,
                               static_cast<unsigned int>(w_phys), static_cast<unsigned int>(h_phys), 0,
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

    // Pre-show attribute-driven size constraints + window icon.
    if (wd.attrs) {
      int min_w = wd.attrs->get_int(NEUI_ATTR_MIN_WIDTH,  0);
      int min_h = wd.attrs->get_int(NEUI_ATTR_MIN_HEIGHT, 0);
      int max_w = wd.attrs->get_int(NEUI_ATTR_MAX_WIDTH,  0);
      int max_h = wd.attrs->get_int(NEUI_ATTR_MAX_HEIGHT, 0);
      if (min_w || min_h || max_w || max_h)
        platform_apply_size_constraints(lw, min_w, min_h, max_w, max_h);
      const char* icon = wd.attrs->get_string(NEUI_ATTR_ICON_PATH);
      if (icon && *icon) platform_set_window_icon(wd, icon);
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
    float   scale = static_cast<float>(dpi) / 96.0f; if (scale <= 0.0f) scale = 1.0f;

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
    int ox = 0, oy = 0;
    unsigned ow = static_cast<unsigned>(DisplayWidth(d, scr)), oh = static_cast<unsigned>(DisplayHeight(d, scr));
    if (owner) {
      Window ch; XTranslateCoordinates(d, owner->win, root, 0, 0, &ox, &oy, &ch);
      XWindowAttributes oa;
      if (XGetWindowAttributes(d, owner->win, &oa)) { ow = static_cast<unsigned>(oa.width); oh = static_cast<unsigned>(oa.height); }
    }
    int wpx = (int)(static_cast<float>(win_w) * scale + 0.5f), hpx = (int)(static_cast<float>(win_h) * scale + 0.5f);
    int x = ox + ((int)ow - wpx) / 2, y = oy + ((int)oh - hpx) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    XSetWindowAttributes swa; std::memset(&swa, 0, sizeof swa);
    swa.background_pixmap = None; swa.bit_gravity = NorthWestGravity;
    swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | KeyPressMask | StructureNotifyMask;
    Window win = XCreateWindow(d, root, x, y, static_cast<unsigned int>(wpx), static_cast<unsigned int>(hpx), 0, depth, InputOutput, vis,
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
        backend->draw_text(ctx, (float)(cx - gw / 2), (float)PAD, (float)(gw + 4), (float)ICON, glyph, 18.0f, 0xFFFFFFFF);
        backend->pop_font(ctx);
      }
      int tx = PAD + (spec.icon ? ICON + GAP : 0);
      int ty = PAD;
      backend->push_font(ctx, "", 700);
      for (auto& l : cap_lines) {
        backend->draw_text(ctx, (float)tx, (float)ty, (float)(win_w - tx - PAD), (float)LH_CAP, l.c_str(), SZ_CAP,
                           C(neui_detail::ColorRole::text_primary));
        ty += LH_CAP;
      }
      backend->pop_font(ctx);
      if (!cap_lines.empty() && !txt_lines.empty()) ty += 6;
      for (auto& l : txt_lines) {
        backend->draw_text(ctx, (float)tx, (float)ty, (float)(win_w - tx - PAD), (float)LH, l.c_str(), SZ_TXT,
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
        backend->draw_text(ctx, (float)(bx[i] + (bw[i] - lw) / 2), (float)btn_y, (float)(lw + 4), (float)BTN_H,
                           spec.btn[i].label, SZ_TXT,
                           C(neui_detail::ColorRole::text_primary));
      }
      backend->end_frame(ctx);
    };

    XMapRaised(d, win);

    // Modal: block input to the owner while the box is up. dispatch_x_event
    // swallows input for an input_disabled window (the non-box events routed
    // through it at the top of the loop). Save/restore so a message box opened
    // over an already-modal dialog leaves the owner blocked on return.
    bool prev_disabled = owner ? owner->input_disabled : false;
    if (owner) owner->input_disabled = true;
    Window prev_box = g_modal_box_win;   // re-raise target (supports nesting)
    g_modal_box_win = win;

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
          backend->resize(ctx, static_cast<uint32_t>(e.xconfigure.width), static_cast<uint32_t>(e.xconfigure.height));
          need_paint = true; break;
        case MotionNotify: {
          int h = hit_button((int)(static_cast<float>(e.xmotion.x) / scale), (int)(static_cast<float>(e.xmotion.y) / scale));
          if (h != hover) { hover = h; need_paint = true; }
          break;
        }
        case ButtonPress:
          if (e.xbutton.button == Button1) {
            pressed = hit_button((int)(static_cast<float>(e.xbutton.x) / scale), (int)(static_cast<float>(e.xbutton.y) / scale));
            need_paint = true;
          }
          break;
        case ButtonRelease:
          if (e.xbutton.button == Button1) {
            int h = hit_button((int)(static_cast<float>(e.xbutton.x) / scale), (int)(static_cast<float>(e.xbutton.y) / scale));
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

    if (owner) owner->input_disabled = prev_disabled;
    g_modal_box_win = prev_box;
    backend->destroy_context(ctx);
    XDestroyWindow(d, win);
    XSync(d, False);
    XSetErrorHandler(old_err);
    return result;
  }

  // ---- File dialog ---------------------------------------------------------
  //
  // Two paths, tried in order:
  //
  //   1. The XDG desktop portal (file_dialog_portal.h). The user's own file
  //      chooser, with their bookmarks and recent files. Optional at build
  //      time (NEUI_HAS_DBUS) and at run time (a portal implementation has to
  //      be installed), so any failure falls through to (2). Only an explicit
  //      "cancelled" stops the fall-through.
  //
  //   2. A neui-drawn browser over the Cairo backend, in the same shape as
  //      run_message_box above: own X window, own nested modal loop, owner
  //      input blocked, palette scoped to the owner's session. Deliberately
  //      plain - a path bar, an Up button, a scrolling list, a name field for
  //      save, a filter chip that cycles the type list, and OK / Cancel. It
  //      is the no-desktop fallback, not a file manager.
  //
  // All the fiddly logic (glob matching, listing order, extension completion)
  // lives in hosts/shared/file_dialog_model.h and is Tier-1 tested; what is
  // left here is X11, Cairo, and readdir.

  // Read one directory. Returns false when it cannot be opened at all, so the
  // caller can refuse to navigate into it rather than showing an empty folder
  // that looks like a folder with no files in it.
  bool fd_read_dir(const std::string& dir, std::vector<neui_detail::FileEntry>& out)
  {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    while (struct dirent* e = readdir(d)) {
      neui_detail::FileEntry fe;
      fe.name = e->d_name;
      // d_type is not filled in on every filesystem (notably some network
      // mounts report DT_UNKNOWN); stat() is the fallback so directories
      // stay navigable there.
      if (e->d_type == DT_DIR)          fe.is_dir = true;
      else if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
        struct stat st;
        std::string full = neui_detail::path_join(dir, fe.name);
        fe.is_dir = (stat(full.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
      }
      out.push_back(fe);
    }
    closedir(d);
    return true;
  }

  bool fd_path_exists(const std::string& p)
  {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
  }

  // Where to start when the client named no directory: $HOME, then the passwd
  // entry, then "/" - never the process CWD, which for a plugin is wherever
  // the DAW happened to be launched from.
  std::string fd_default_dir()
  {
    if (const char* h = getenv("HOME")) {
      if (*h && fd_path_exists(h)) return h;
    }
    if (struct passwd* pw = getpwuid(getuid())) {
      if (pw->pw_dir && *pw->pw_dir && fd_path_exists(pw->pw_dir)) return pw->pw_dir;
    }
    return "/";
  }

  // The neui-drawn browser. Appends the chosen path(s) to `out`; returns the
  // count, 0 on cancel.
  int run_file_browser(LinuxWindow* owner, bool save,
                       const neui_file_dialog_t* desc,
                       std::vector<std::string>& out)
  {
    if (!g_display) return -1;
    Display* d = g_display;
    auto* backend = platform_get_backend();
    if (!backend) return -1;

    using namespace neui_detail;

    const uint32_t fdflags  = desc ? desc->flags : 0u;
    const bool dir_mode     = !save && (fdflags & NEUI_FD_DIRECTORY) != 0;
    const bool multi        = !save && (fdflags & NEUI_FD_MULTISELECT) != 0;
    bool       show_hidden  = (fdflags & NEUI_FD_SHOW_HIDDEN) != 0;

    std::vector<FileFilter> filters = parse_filters(desc);
    size_t filter_index = clamp_default_filter(desc, filters);
    // A folder picker filters nothing.
    if (dir_mode) filters.clear();

    std::string cwd = (desc && desc->initial_dir && *desc->initial_dir &&
                       fd_path_exists(desc->initial_dir))
                      ? desc->initial_dir : fd_default_dir();

    TextEditState name;
    if (save && desc && desc->initial_name) name.text = desc->initial_name;
    name.cursor = name.sel_anchor = (int)name.text.size();

    std::vector<FileEntry> rows;
    std::vector<bool>      picked;      // multi-select marks, parallel to rows
    int  sel = -1;                      // focused row, -1 = none
    int  scroll = 0;                    // first visible row

    auto active_filter = [&]() -> const FileFilter* {
      if (filters.empty()) return nullptr;
      return &filters[filter_index < filters.size() ? filter_index : 0];
    };
    // Forward-declared so reload() can clear it: without that, entering a
    // directory by double-click leaves the second click armed, and the NEXT
    // single click on the same row index within the interval counts as a
    // double-click - navigating again, or confirming a file outright.
    int  last_click_row  = -1;
    auto reload = [&]() {
      std::vector<FileEntry> raw;
      fd_read_dir(cwd, raw);
      rows = list_directory_view(raw, active_filter(), show_hidden);
      picked.assign(rows.size(), false);
      sel = rows.empty() ? -1 : 0;
      scroll = 0;
      last_click_row = -1;
    };
    reload();

    uint32_t dpi  = owner ? owner->dpi : query_display_dpi(d);
    float   scale = (float)dpi / 96.0f; if (scale <= 0.0f) scale = 1.0f;

    neui_detail::ScopedPaletteOverride scope(
      (owner && owner->session) ? owner->session->effective_palette_ptr() : nullptr);
    auto C = [](neui_detail::ColorRole r) {
      return neui_detail::current_palette().colors[(size_t)r];
    };
    using neui_detail::shade;

    // Layout, logical px. Fixed size: a resizable browser would need a real
    // layout pass, and the fallback does not earn one.
    const int WIN_W = 560, WIN_H = 420;
    const int PAD = 12, ROW_H = 20, BTN_H = 26, BTN_W = 92, BTN_GAP = 8;
    const int BAR_H = 24, FLD_H = 24;
    const int UP_W = 40;
    const float SZ = 13.0f;

    const int bar_y  = PAD;
    const int list_y = bar_y + BAR_H + 8;
    const int btn_y  = WIN_H - PAD - BTN_H;
    const int fld_y  = btn_y - 8 - FLD_H;
    // Open mode has no name field, so the list takes that row's space rather
    // than leaving a blank strip above the buttons.
    const int list_h = (save ? fld_y : btn_y) - 8 - list_y;
    const int visible_rows = list_h / ROW_H;
    const int list_x = PAD, list_w = WIN_W - PAD * 2;

    const int ok_x     = WIN_W - PAD - BTN_W;
    const int cancel_x = ok_x - BTN_GAP - BTN_W;
    const int up_x     = WIN_W - PAD - UP_W;
    const int chip_x   = PAD, chip_w = 200;

    auto clamp_scroll = [&]() {
      int max_scroll = (int)rows.size() - visible_rows;
      if (max_scroll < 0) max_scroll = 0;
      if (scroll > max_scroll) scroll = max_scroll;
      if (scroll < 0) scroll = 0;
    };
    auto scroll_to_sel = [&]() {
      if (sel < 0) return;
      if (sel < scroll) scroll = sel;
      else if (sel >= scroll + visible_rows) scroll = sel - visible_rows + 1;
      clamp_scroll();
    };

    // The set of paths the OK button would return right now. Empty = OK is
    // not actionable (nothing selected / nothing typed), which is also what
    // greys the button out.
    auto collect_result = [&](std::vector<std::string>& res) {
      res.clear();
      if (save) {
        std::string leaf = name.text;
        if (leaf.empty()) return;
        if (!filters.empty()) leaf = complete_extension(leaf, filters[filter_index]);
        res.push_back(path_join(cwd, leaf));
        return;
      }
      if (multi) {
        for (size_t i = 0; i < rows.size(); ++i)
          if (picked[i] && rows[i].is_dir == dir_mode)
            res.push_back(path_join(cwd, rows[i].name));
        if (!res.empty()) return;
      }
      if (sel >= 0 && sel < (int)rows.size() &&
          rows[(size_t)sel].is_dir == dir_mode) {
        res.push_back(path_join(cwd, rows[(size_t)sel].name));
        return;
      }
      // Folder mode with nothing selected picks the folder the browser is
      // currently IN. Without this, a directory with no subdirectories is a
      // dead end: no row can be selected, so OK would never enable and the
      // only way out is Cancel.
      if (dir_mode) res.push_back(cwd);
    };

    // Centre over the owner (or the screen) - same as the message box.
    int scr = DefaultScreen(d);
    Window root = RootWindow(d, scr);
    Visual* vis = DefaultVisual(d, scr);
    int depth = DefaultDepth(d, scr);
    int ox = 0, oy = 0;
    unsigned ow = (unsigned)DisplayWidth(d, scr), oh = (unsigned)DisplayHeight(d, scr);
    if (owner) {
      Window ch; XTranslateCoordinates(d, owner->win, root, 0, 0, &ox, &oy, &ch);
      XWindowAttributes oa;
      if (XGetWindowAttributes(d, owner->win, &oa)) {
        ow = (unsigned)oa.width; oh = (unsigned)oa.height;
      }
    }
    int wpx = (int)((float)WIN_W * scale + 0.5f), hpx = (int)((float)WIN_H * scale + 0.5f);
    int wx = ox + ((int)ow - wpx) / 2, wy = oy + ((int)oh - hpx) / 2;
    if (wx < 0) wx = 0;
    if (wy < 0) wy = 0;

    XSetWindowAttributes swa; std::memset(&swa, 0, sizeof swa);
    swa.background_pixmap = None; swa.bit_gravity = NorthWestGravity;
    swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | KeyPressMask | StructureNotifyMask;
    Window win = XCreateWindow(d, root, wx, wy, (unsigned)wpx, (unsigned)hpx, 0,
                               depth, InputOutput, vis,
                               CWBackPixmap | CWEventMask | CWBitGravity, &swa);
    if (!win) return -1;

    const char* title = (desc && desc->title && *desc->title)
                        ? desc->title
                        : (save ? "Save File" : (dir_mode ? "Choose Folder" : "Open File"));
    set_window_title(d, win, title);
    if (owner) XSetTransientForHint(d, win, owner->win);
    if (g_wm_delete != None) XSetWMProtocols(d, win, &g_wm_delete, 1);
    {
      Atom st = XInternAtom(d, "_NET_WM_STATE", False);
      Atom md = XInternAtom(d, "_NET_WM_STATE_MODAL", False);
      if (st != None && md != None)
        XChangeProperty(d, win, st, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<unsigned char*>(&md), 1);
    }
    if (XSizeHints* sh = XAllocSizeHints()) {
      sh->flags = PMinSize | PMaxSize | USPosition;
      sh->min_width = sh->max_width = wpx;
      sh->min_height = sh->max_height = hpx;
      sh->x = wx; sh->y = wy;
      XSetWMNormalHints(d, win, sh); XFree(sh);
    }

    neui_cairo_backend::LinuxNativeSurface ns;
    ns.dpy = d; ns.win = win; ns.visual = vis; ns.depth = depth;
    neui_render_ctx_t ctx = backend->create_context(&ns, (uint32_t)wpx, (uint32_t)hpx);
    if (!ctx) { XDestroyWindow(d, win); return -1; }
    backend->update_dpi(ctx, dpi);

    // Overwrite-confirmation overlay. The API documents the prompt as ON by
    // default for save (NEUI_FD_NO_OVERWRITE_PROMPT turns it off), and this
    // browser is the whole dialog on a portal-less box - so it has to ask
    // rather than hand back a path the client will silently clobber. Drawn as
    // an in-window overlay instead of nesting run_message_box: that helper
    // takes a LinuxWindow* owner to disable, and the browser's X window is not
    // one, so a nested box would leave the browser itself clickable
    // underneath.
    const bool  want_overwrite_prompt = save && !(fdflags & NEUI_FD_NO_OVERWRITE_PROMPT);
    bool        confirming = false;      // overlay is up
    std::string confirm_path;            // the path it is asking about
    // Which overlay button the keyboard is on: 0 = Cancel, 1 = Replace. Starts
    // on Cancel so Enter answers non-destructively, but Tab / Left / Right can
    // reach Replace - without a focus index the overlay was mouse-only, and a
    // keyboard-driven user could never overwrite a file at all.
    int         conf_focus = 0;
    const int   CONF_W = 360, CONF_H = 130;
    const int   conf_x = (WIN_W - CONF_W) / 2, conf_y = (WIN_H - CONF_H) / 2;
    const int   conf_btn_y = conf_y + CONF_H - 12 - BTN_H;
    const int   conf_no_x  = conf_x + CONF_W - 12 - BTN_W;
    const int   conf_yes_x = conf_no_x - BTN_GAP - BTN_W;

    // Hit regions. Kept as an enum + one hit() so paint and input cannot
    // disagree about where a control is.
    enum Hit { H_NONE, H_LIST, H_UP, H_OK, H_CANCEL, H_CHIP, H_FIELD,
               H_CONF_YES, H_CONF_NO };
    auto hit = [&](int lx, int ly, int* row_out) -> Hit {
      if (row_out) *row_out = -1;
      // While the overlay is up it swallows everything: the controls behind it
      // must not react to a click aimed at Yes/No.
      if (confirming) {
        if (ly >= conf_btn_y && ly < conf_btn_y + BTN_H) {
          if (lx >= conf_yes_x && lx < conf_yes_x + BTN_W) return H_CONF_YES;
          if (lx >= conf_no_x  && lx < conf_no_x  + BTN_W) return H_CONF_NO;
        }
        return H_NONE;
      }
      if (ly >= btn_y && ly < btn_y + BTN_H) {
        if (lx >= ok_x && lx < ok_x + BTN_W)         return H_OK;
        if (lx >= cancel_x && lx < cancel_x + BTN_W) return H_CANCEL;
        if (!filters.empty() && lx >= chip_x && lx < chip_x + chip_w) return H_CHIP;
        return H_NONE;
      }
      if (ly >= bar_y && ly < bar_y + BAR_H) {
        if (lx >= up_x && lx < up_x + UP_W) return H_UP;
        return H_NONE;
      }
      if (save && ly >= fld_y && ly < fld_y + FLD_H) return H_FIELD;
      if (ly >= list_y && ly < list_y + list_h &&
          lx >= list_x && lx < list_x + list_w) {
        // list_h is not an exact multiple of ROW_H, so the last ~18 px are a
        // dead strip below the final DRAWN row. Bounding by visible_rows as
        // well as by rows.size() keeps a click there from selecting - or
        // double-click-activating - a row the user cannot see.
        int slot = (ly - list_y) / ROW_H;
        int r    = scroll + slot;
        if (slot < visible_rows && r >= 0 && r < (int)rows.size()) {
          if (row_out) *row_out = r;
        }
        return H_LIST;
      }
      return H_NONE;
    };

    int  hover_row = -1;
    Hit  hover     = H_NONE;
    Hit  pressed   = H_NONE;
    bool done = false, need_paint = true, focused = false, cancelled = false;
    // Click-to-open needs a double click; last_click_row is declared above so
    // reload() can reset it on navigation.
    Time last_click_time = 0;

    int (*old_err)(Display*, XErrorEvent*) = XSetErrorHandler(drag_xerror);

    auto text_w = [&](const char* s, float sz) -> int {
      if (!s || !*s) return 0;
      return (int)backend->measure_text(ctx, s, -1, sz);
    };
    // Middle-elide a string to fit `maxw` ("/very/long/…/dir").
    auto elide = [&](const std::string& s, int maxw) -> std::string {
      if (text_w(s.c_str(), SZ) <= maxw) return s;
      std::string tail = s;
      while (!tail.empty() && text_w(("..." + tail).c_str(), SZ) > maxw)
        tail.erase(0, 1);
      return "..." + tail;
    };

    auto draw_button = [&](int bx, int by, int bw, int bh, const char* label,
                           bool is_default, bool enabled, bool is_hover, bool is_pressed) {
      uint32_t fill = C(neui_detail::ColorRole::control_bg);
      if (!enabled)        fill = shade(fill, -6);
      else if (is_pressed) fill = shade(fill, -18);
      else if (is_hover)   fill = shade(fill, +14);
      backend->fill_rect(ctx, (float)bx, (float)by, (float)bw, (float)bh, fill);
      backend->draw_rect(ctx, (float)bx, (float)by, (float)bw, (float)bh,
                         is_default ? 2.0f : 1.0f,
                         is_default ? C(neui_detail::ColorRole::accent)
                                    : C(neui_detail::ColorRole::border));
      int lw = text_w(label, SZ);
      backend->draw_text(ctx, (float)(bx + (bw - lw) / 2), (float)by,
                         (float)(lw + 4), (float)bh, label, SZ,
                         C(enabled ? neui_detail::ColorRole::text_primary
                                   : neui_detail::ColorRole::text_disabled));
    };

    auto render = [&]() {
      std::vector<std::string> preview;
      collect_result(preview);
      const bool ok_enabled = !preview.empty();

      backend->begin_frame(ctx, C(neui_detail::ColorRole::frame_bg));

      // Path bar + Up.
      backend->fill_rect(ctx, (float)PAD, (float)bar_y,
                         (float)(WIN_W - PAD * 2 - UP_W - 8), (float)BAR_H,
                         shade(C(neui_detail::ColorRole::frame_bg), +18));
      {
        std::string shown = elide(cwd, WIN_W - PAD * 2 - UP_W - 8 - 12);
        backend->draw_text(ctx, (float)(PAD + 6), (float)bar_y,
                           (float)(WIN_W - PAD * 2 - UP_W - 20), (float)BAR_H,
                           shown.c_str(), SZ,
                           C(neui_detail::ColorRole::text_primary));
      }
      draw_button(up_x, bar_y, UP_W, BAR_H, "Up", false,
                  cwd != path_parent(cwd), hover == H_UP, pressed == H_UP);

      // List.
      backend->fill_rect(ctx, (float)list_x, (float)list_y, (float)list_w, (float)list_h,
                         shade(C(neui_detail::ColorRole::frame_bg), +10));
      backend->draw_rect(ctx, (float)list_x, (float)list_y, (float)list_w, (float)list_h,
                         1.0f, C(neui_detail::ColorRole::border));
      for (int i = 0; i < visible_rows; ++i) {
        int r = scroll + i;
        if (r < 0 || r >= (int)rows.size()) break;
        int ry = list_y + i * ROW_H;
        bool is_sel  = (r == sel);
        bool is_mark = multi && picked[(size_t)r];
        if (is_sel || is_mark)
          backend->fill_rect(ctx, (float)(list_x + 1), (float)ry,
                             (float)(list_w - 2), (float)ROW_H,
                             is_sel ? C(neui_detail::ColorRole::accent)
                                    : shade(C(neui_detail::ColorRole::accent), -40));
        else if (r == hover_row)
          backend->fill_rect(ctx, (float)(list_x + 1), (float)ry,
                             (float)(list_w - 2), (float)ROW_H,
                             shade(C(neui_detail::ColorRole::frame_bg), +22));
        // A trailing '/' is the whole folder affordance - no icon set here.
        std::string label = rows[(size_t)r].is_dir ? rows[(size_t)r].name + "/"
                                                   : rows[(size_t)r].name;
        // A file that cannot be picked in this mode (any file in folder mode)
        // is still listed, greyed, so the folder does not look empty.
        bool pickable = (rows[(size_t)r].is_dir == dir_mode) || rows[(size_t)r].is_dir;
        backend->draw_text(ctx, (float)(list_x + 8), (float)ry,
                           (float)(list_w - 16), (float)ROW_H, label.c_str(), SZ,
                           is_sel ? C(neui_detail::ColorRole::accent_text)
                                  : C(pickable ? neui_detail::ColorRole::text_primary
                                               : neui_detail::ColorRole::text_disabled));
      }
      if (rows.empty()) {
        const char* empty = "(no matching files)";
        backend->draw_text(ctx, (float)(list_x + 8), (float)(list_y + 6),
                           (float)(list_w - 16), (float)ROW_H, empty, SZ,
                           C(neui_detail::ColorRole::text_disabled));
      }
      // Scroll indicator: a plain proportional thumb, no dragging (the wheel
      // and the arrow keys are the scroll affordances here).
      if ((int)rows.size() > visible_rows) {
        float frac_h = (float)visible_rows / (float)rows.size();
        float frac_y = (float)scroll / (float)rows.size();
        float tb_h = (float)list_h * frac_h; if (tb_h < 16.0f) tb_h = 16.0f;
        backend->fill_rect(ctx, (float)(list_x + list_w - 6),
                           (float)list_y + (float)list_h * frac_y,
                           4.0f, tb_h, C(neui_detail::ColorRole::border));
      }

      // Name field (save only).
      if (save) {
        backend->fill_rect(ctx, (float)PAD, (float)fld_y, (float)(WIN_W - PAD * 2),
                           (float)FLD_H, C(neui_detail::ColorRole::control_bg));
        backend->draw_rect(ctx, (float)PAD, (float)fld_y, (float)(WIN_W - PAD * 2),
                           (float)FLD_H, 1.0f, C(neui_detail::ColorRole::accent));
        backend->draw_text(ctx, (float)(PAD + 6), (float)fld_y,
                           (float)(WIN_W - PAD * 2 - 12), (float)FLD_H,
                           name.text.c_str(), SZ,
                           C(neui_detail::ColorRole::text_primary));
        // Caret at the cursor byte offset (the field always has focus - there
        // is nothing else here that takes typing).
        std::string upto = name.text.substr(0, (size_t)name.cursor);
        int cx = PAD + 6 + text_w(upto.c_str(), SZ);
        backend->fill_rect(ctx, (float)cx, (float)(fld_y + 4), 1.0f,
                           (float)(FLD_H - 8),
                           C(neui_detail::ColorRole::text_primary));
      }

      // Filter chip (click cycles) + buttons.
      if (!filters.empty()) {
        const FileFilter& f = filters[filter_index];
        std::string label = f.label;
        if (filters.size() > 1) label += "  \xE2\x96\xBE";   // ▾
        draw_button(chip_x, btn_y, chip_w, BTN_H,
                    elide(label, chip_w - 12).c_str(), false,
                    filters.size() > 1, hover == H_CHIP, pressed == H_CHIP);
      }
      draw_button(cancel_x, btn_y, BTN_W, BTN_H, "Cancel", false, true,
                  hover == H_CANCEL, pressed == H_CANCEL);
      draw_button(ok_x, btn_y, BTN_W, BTN_H, save ? "Save" : "Open", true,
                  ok_enabled, hover == H_OK, pressed == H_OK);

      // Overwrite confirmation, on top of everything and modal to the browser.
      if (confirming) {
        // Scrim first, so it reads as blocking rather than as a floating panel.
        backend->fill_rect(ctx, 0.0f, 0.0f, (float)WIN_W, (float)WIN_H, 0x60000000u);
        backend->fill_rect(ctx, (float)conf_x, (float)conf_y, (float)CONF_W, (float)CONF_H,
                           C(neui_detail::ColorRole::frame_bg));
        backend->draw_rect(ctx, (float)conf_x, (float)conf_y, (float)CONF_W, (float)CONF_H,
                           1.0f, C(neui_detail::ColorRole::border));
        std::string leaf = path_leaf(confirm_path);
        backend->draw_text(ctx, (float)(conf_x + 16), (float)(conf_y + 16),
                           (float)(CONF_W - 32), 20.0f,
                           elide("\"" + leaf + "\" already exists.", CONF_W - 32).c_str(),
                           SZ, C(neui_detail::ColorRole::text_primary));
        backend->draw_text(ctx, (float)(conf_x + 16), (float)(conf_y + 40),
                           (float)(CONF_W - 32), 20.0f,
                           "Replace it?", SZ,
                           C(neui_detail::ColorRole::text_primary));
        // Cancel starts focused, so a stray Enter never picks the destructive
        // answer; the ring follows conf_focus once the user Tabs.
        draw_button(conf_yes_x, conf_btn_y, BTN_W, BTN_H, "Replace",
                    conf_focus == 1, true,
                    hover == H_CONF_YES, pressed == H_CONF_YES);
        draw_button(conf_no_x, conf_btn_y, BTN_W, BTN_H, "Cancel",
                    conf_focus == 0, true,
                    hover == H_CONF_NO, pressed == H_CONF_NO);
      }

      backend->end_frame(ctx);
    };

    // The OK/Enter path for save: ask before handing back a path that already
    // names an existing file. Returns true when the caller should finish.
    auto try_accept = [&]() -> bool {
      std::vector<std::string> res;
      collect_result(res);
      if (res.empty()) return false;
      if (want_overwrite_prompt && fd_path_exists(res[0])) {
        confirming   = true;
        confirm_path = res[0];
        conf_focus   = 0;          // always re-arm on the safe answer
        pressed      = H_NONE;
        hover        = H_NONE;
        need_paint   = true;
        return false;
      }
      return true;
    };

    // Enter the directory under `r`, or pick it in folder mode.
    auto activate_row = [&](int r) {
      if (r < 0 || r >= (int)rows.size()) return;
      if (rows[(size_t)r].is_dir) {
        std::string next = path_join(cwd, rows[(size_t)r].name);
        std::vector<FileEntry> probe;
        if (!fd_read_dir(next, probe)) return;   // unreadable: stay put
        cwd = next;
        reload();
        need_paint = true;
        return;
      }
      if (save) {
        name.text = rows[(size_t)r].name;
        name.cursor = name.sel_anchor = (int)name.text.size();
        need_paint = true;
        return;
      }
      // A file double-clicked in FILE mode confirms outright. In folder mode a
      // file row is drawn greyed and unpickable, so activating it must do
      // nothing at all - without this guard the gesture fell through to
      // `done = true`, and collect_result's "nothing selected -> pick cwd"
      // fallback then returned the current directory as if the user had chosen
      // it, from a double-click on a row the UI says cannot be chosen.
      if (!dir_mode) done = true;
    };

    XMapRaised(d, win);

    bool prev_disabled = owner ? owner->input_disabled : false;
    if (owner) owner->input_disabled = true;
    Window prev_box = g_modal_box_win;
    g_modal_box_win = win;

    while (!done) {
      if (need_paint) { render(); need_paint = false; }
      flush_pending_paints();
      XFlush(d);
      XEvent e; XNextEvent(d, &e);
      if (e.xany.window != win) { dispatch_x_event(e); continue; }
      switch (e.type) {
        case Expose:
          if (e.xexpose.count == 0) {
            need_paint = true;
            if (!focused) { XSetInputFocus(d, win, RevertToParent, CurrentTime); focused = true; }
          }
          break;
        case ConfigureNotify:
          backend->resize(ctx, (uint32_t)e.xconfigure.width, (uint32_t)e.xconfigure.height);
          need_paint = true;
          break;
        case MotionNotify: {
          int r = -1;
          Hit h = hit((int)((float)e.xmotion.x / scale), (int)((float)e.xmotion.y / scale), &r);
          if (h != hover || r != hover_row) { hover = h; hover_row = r; need_paint = true; }
          break;
        }
        case ButtonPress: {
          int lx = (int)((float)e.xbutton.x / scale), ly = (int)((float)e.xbutton.y / scale);
          if (e.xbutton.button == Button4 || e.xbutton.button == Button5) {
            // Absorbed while the overwrite overlay is up - scrolling the list
            // out from under a modal question is the same bug the popup-menu
            // wheel gate fixes.
            if (!confirming) {
              scroll += (e.xbutton.button == Button4) ? -3 : 3;
              clamp_scroll();
              need_paint = true;
            }
            break;
          }
          if (e.xbutton.button != Button1) break;
          int r = -1;
          Hit h = hit(lx, ly, &r);
          pressed = h;
          if (h == H_LIST && r >= 0) {
            // Ctrl-click toggles a mark in multi-select; a plain click moves
            // the focus row and clears the marks.
            if (multi && (e.xbutton.state & ControlMask)) {
              picked[(size_t)r] = !picked[(size_t)r];
              sel = r;
            } else {
              picked.assign(rows.size(), false);
              sel = r;
              if (save && !rows[(size_t)r].is_dir) {
                name.text = rows[(size_t)r].name;
                name.cursor = name.sel_anchor = (int)name.text.size();
              }
            }
            // Double click within 400 ms on the same row activates it.
            if (r == last_click_row && e.xbutton.time - last_click_time < 400)
              activate_row(r);
            last_click_row = r;
            last_click_time = e.xbutton.time;
          }
          need_paint = true;
          break;
        }
        case ButtonRelease: {
          if (e.xbutton.button != Button1) break;
          int lx = (int)((float)e.xbutton.x / scale), ly = (int)((float)e.xbutton.y / scale);
          int r = -1;
          Hit h = hit(lx, ly, &r);
          if (h == pressed) {
            switch (h) {
              case H_CANCEL: cancelled = true; done = true; break;
              case H_OK:
                if (try_accept()) done = true;
                break;
              // "Replace" accepts the path that is already in the name field;
              // "Cancel" only dismisses the overlay, leaving the browser up so
              // the user can pick a different name.
              case H_CONF_YES: confirming = false; done = true; break;
              case H_CONF_NO:  confirming = false; break;
              case H_UP: {
                std::string up = path_parent(cwd);
                if (up != cwd) { cwd = up; reload(); }
                break;
              }
              case H_CHIP:
                if (filters.size() > 1) {
                  filter_index = (filter_index + 1) % filters.size();
                  reload();
                }
                break;
              default: break;
            }
          }
          pressed = H_NONE;
          need_paint = true;
          break;
        }
        case KeyPress: {
          KeySym ks = 0;
          char   buf[32] = {0};
          int    n = XLookupString(&e.xkey, buf, sizeof buf - 1, &ks, nullptr);
          const bool ctrl = (e.xkey.state & ControlMask) != 0;
          // The overwrite overlay owns the keyboard while it is up, and its
          // default (Enter) is the NON-destructive answer - as is Esc. Typing
          // into the name field behind it would be worse than useless, since
          // the overlay is asking about a path already computed from it.
          if (confirming) {
            if (ks == XK_Escape) {              // always the safe answer
              confirming = false;
              need_paint = true;
            } else if (ks == XK_Tab || ks == XK_Left || ks == XK_Right) {
              conf_focus = conf_focus ? 0 : 1;
              need_paint = true;
            } else if (ks == XK_Return || ks == XK_KP_Enter || ks == XK_space) {
              if (conf_focus == 1) done = true;   // Replace: accept the path
              confirming = false;
              need_paint = true;
            }
            break;
          }
          if (ks == XK_Escape) { cancelled = true; done = true; break; }
          if (ks == XK_Return || ks == XK_KP_Enter) {
            // On a focused directory, Enter navigates rather than confirming -
            // otherwise a folder row would return a path the client cannot use.
            if (!save && sel >= 0 && sel < (int)rows.size() &&
                rows[(size_t)sel].is_dir && !dir_mode) {
              activate_row(sel);
            } else if (try_accept()) {
              done = true;
            }
            break;
          }
          if (ks == XK_Up || ks == XK_Down) {
            if (!rows.empty()) {
              if (sel < 0) sel = 0;
              else sel += (ks == XK_Up) ? -1 : 1;
              if (sel < 0) sel = 0;
              if (sel >= (int)rows.size()) sel = (int)rows.size() - 1;
              scroll_to_sel();
              need_paint = true;
            }
            break;
          }
          if (ks == XK_Page_Up || ks == XK_Page_Down) {
            scroll += (ks == XK_Page_Up ? -visible_rows : visible_rows);
            clamp_scroll();
            need_paint = true;
            break;
          }
          if (ks == XK_BackSpace && !save) {
            // No name field to edit in open mode, so Backspace goes up a level
            // (the shell convention).
            std::string up = path_parent(cwd);
            if (up != cwd) { cwd = up; reload(); need_paint = true; }
            break;
          }
          if (ctrl && (ks == XK_h || ks == XK_H)) {
            show_hidden = !show_hidden;
            reload();
            need_paint = true;
            break;
          }
          if (save) {
            // The name field is the only typing target in the browser, so it
            // always has the keyboard - no focus ring to manage.
            if (ks == XK_BackSpace) {
              te_backspace(name.text, name.cursor, name.sel_anchor, ctrl, nullptr);
              need_paint = true;
            } else if (ks == XK_Delete) {
              te_delete_forward(name.text, name.cursor, name.sel_anchor, ctrl, nullptr);
              need_paint = true;
            } else if (ks == XK_Left) {
              te_move_left(name.text, name.cursor, name.sel_anchor, ctrl, false, nullptr);
              need_paint = true;
            } else if (ks == XK_Right) {
              te_move_right(name.text, name.cursor, name.sel_anchor, ctrl, false, nullptr);
              need_paint = true;
            } else if (ks == XK_Home) {
              te_move_home(name.text, name.cursor, name.sel_anchor, false, nullptr);
              need_paint = true;
            } else if (ks == XK_End) {
              te_move_end(name.text, name.cursor, name.sel_anchor, false, nullptr);
              need_paint = true;
            } else if (n > 0 && (unsigned char)buf[0] >= 0x20 && !ctrl) {
              // XLookupString returns LATIN-1, not UTF-8 (the frames' real text
              // widgets go through XIM / Xutf8LookupString; this field does
              // not). Feeding its bytes straight to te_insert_utf8 put a bare
              // 0xA0-0xFF byte into the name for any ordinary European key -
              // typing "e" with an acute accent produced a garbled field and a
              // returned path that was not valid UTF-8, which the public API
              // promises it is. Transcode the high bytes to two-byte UTF-8.
              char utf8[64];
              int  m = 0;
              for (int i = 0; i < n && m + 2 < (int)sizeof utf8; ++i) {
                unsigned char b = (unsigned char)buf[i];
                if (b < 0x80) utf8[m++] = (char)b;
                else {
                  utf8[m++] = (char)(0xC0 | (b >> 6));
                  utf8[m++] = (char)(0x80 | (b & 0x3F));
                }
              }
              if (m > 0) {
                te_insert_utf8(name.text, name.cursor, name.sel_anchor, false,
                               utf8, m, nullptr);
                need_paint = true;
              }
            }
          }
          break;
        }
        case ClientMessage:
          if (e.xclient.message_type == g_wm_protocols &&
              (Atom)e.xclient.data.l[0] == g_wm_delete) {
            cancelled = true; done = true;
          }
          break;
        default: break;
      }
    }

    if (owner) owner->input_disabled = prev_disabled;
    g_modal_box_win = prev_box;
    backend->destroy_context(ctx);
    XDestroyWindow(d, win);
    XSync(d, False);
    XSetErrorHandler(old_err);

    if (cancelled) return 0;
    collect_result(out);
    return (int)out.size();
  }

  // Keep the app's own windows alive while the portal dialog (another
  // process) is up. Passed to file_dialog_portal as its pump callback.
  void fd_portal_pump(void*)
  {
    drain_events();
    flush_pending_paints();
    if (g_display) XFlush(g_display);
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

    // System dark/light tracking via the XDG portal (no-op without libdbus-1).
    // Runs before any Session is created so the first frame's frozen palette
    // already reflects the system color-scheme.
    neui_detail::ensure_theme_provider_linux();
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
    // wd.embed_parent (set via platform_set_embed_parent) selects the
    // foreign-parent embedded path; 0 = borderless standalone top-level.
    create_frame(session, widget_index, wd, /*borderless*/true,
                 /*is_appwindow*/false, /*owner*/nullptr,
                 static_cast<unsigned long>(wd.embed_parent));
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
                                 void* native_parent)
  {
    if (!session) return;
    auto* wd = session->get_widget(widget_index);
    // The DAW-provided X11 Window id travels cast through void*.
    if (wd) wd->embed_parent = reinterpret_cast<uintptr_t>(native_parent);
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
      neui_detail::theme_dbus_dispatch();   // pick up system dark/light changes
    }
    // Client timers (NEUI_API_TIMER) are NOT gated by the 16 ms animation
    // window: their own deadlines already decide what is due, and a plugin
    // asking for 8 ms must not be halved. There is no timerfd in the picture
    // here - the DAW's cadence is the tick source.
    if (lw->session && client_timer_sessions().count(lw->session))
      lw->session->tick_client_timers();
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
      // Session::end_modal, not a bare flag clear: it also restores the focus the
      // dialog took from its owner, so a user-driven close and a client destroy
      // end the same way on all three platforms.
      if (dynamic_cast<FrameWidget*>(&wd))
        s->end_modal(wd.index);
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
    // w/h are the logical client size; grow by DPI ratio AND the frame's zoom
    // so a zoomed frame occupies proportionally more of the screen while the
    // client keeps thinking in logical units. `dpi` is honoured as an override
    // for the ratio part (callers pass wd.dpi); the zoom always comes from the
    // frame's live attr via window_scale.
    float scale = window_scale(lw);
    if (dpi && lw->dpi && dpi != lw->dpi)
      scale *= static_cast<float>(dpi) / static_cast<float>(lw->dpi);
    if (!(scale > 0.0f)) scale = 1.0f;
    // Record the logical size we are asking for so the asynchronous
    // ConfigureNotify can recognise its own echo (see LinuxWindow::expect_w).
    lw->expect_w = w;
    lw->expect_h = h;
    const unsigned int pw = static_cast<unsigned int>(static_cast<float>(w) * scale + 0.5f);
    const unsigned int ph = static_cast<unsigned int>(static_cast<float>(h) * scale + 0.5f);
    if (x == NEUI_WINDOW_POS_KEEP || y == NEUI_WINDOW_POS_KEEP) {
      // Size-only: XResizeWindow leaves the position to the window manager,
      // which is exactly "keep where the user put it".
      XResizeWindow(lw->dpy, lw->win, pw, ph);
    } else {
      XMoveResizeWindow(lw->dpy, lw->win,
                        static_cast<int>(static_cast<float>(x) * scale + 0.5f),
                        static_cast<int>(static_cast<float>(y) * scale + 0.5f),
                        pw, ph);
    }
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
    float s = static_cast<float>(lw->dpi) / 96.0f;
    return s > 0.0f ? s : 1.0f;
  }

  void platform_invalidate(void* native_handle)
  {
    if (!native_handle) return;
    static_cast<LinuxWindow*>(native_handle)->needs_paint = true;
  }

  void platform_force_paint(void* native_handle)
  {
    if (!native_handle) return;
    // Paint straight into the window's Cairo surface, bypassing the needs_paint
    // / run-loop round trip so the layout caches exist by the time this returns.
    // paint_window self-guards on the frame having a render context, which is
    // exactly the "no usable surface yet" best-effort case.
    paint_window(static_cast<LinuxWindow*>(native_handle));
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
      int dfd = neui_detail::theme_dbus_fd();   // -1 without libdbus / no bus
      if (dfd >= 0) { FD_SET(dfd, &rfds); if (dfd > maxfd) maxfd = dfd; }
      int r = select(maxfd + 1, &rfds, nullptr, nullptr, nullptr);
      if (r < 0) { if (errno == EINTR) continue; break; }
      if (g_timerfd >= 0 && FD_ISSET(g_timerfd, &rfds)) {
        uint64_t exp = 0; ssize_t rd = read(g_timerfd, &exp, sizeof(exp)); (void)rd;
        tick_animations();
      }
      if (dfd >= 0 && FD_ISSET(dfd, &rfds))
        neui_detail::theme_dbus_dispatch();   // -> SettingChanged -> repaint
      // Readable X fd is handled by drain_events() at the top of the loop.
    }
    return true;
  }

  bool platform_pump_once()
  {
    if (!g_display) return true;
    drain_events();
    // Service the heartbeat non-blockingly: platform_run() reads the timerfd
    // out of its select(), but a client driving the loop by hand never gets
    // there, and client timers are documented to work under pump_once too.
    if (g_timerfd >= 0) {
      fd_set rfds; FD_ZERO(&rfds); FD_SET(g_timerfd, &rfds);
      struct timeval zero; zero.tv_sec = 0; zero.tv_usec = 0;
      if (select(g_timerfd + 1, &rfds, nullptr, nullptr, &zero) > 0 &&
          FD_ISSET(g_timerfd, &rfds)) {
        uint64_t exp = 0; ssize_t rd = read(g_timerfd, &exp, sizeof(exp)); (void)rd;
        tick_animations();
      }
    }
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
  // Linux reserves the band via platform_menubar_in_frame(); no extra inset.
  int   platform_frame_extra_top_inset(void* /*nh*/, bool /*has_menubar*/)             { return 0; }
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
  // The in-frame menu reads MenuItemData::checked when it paints; nothing native.
  void  platform_menubar_check_item(void* /*hmenu*/, uint32_t /*cmd*/, bool /*chk*/)    {}
  void  platform_menubar_set_item_text(void* /*hmenu*/, uint32_t /*cmd*/, const char* /*t*/) {}
  void  platform_menubar_set_item_shortcut(void* /*hmenu*/, uint32_t /*cmd*/,
                                            uint32_t /*mods*/, uint32_t /*key*/)        {}

  void platform_set_window_icon(WidgetData& wd, const char* path_utf8)
  {
    if (!wd.native_handle) return;   // applied from create_frame once the window exists
    auto* lw = static_cast<LinuxWindow*>(wd.native_handle);
    Atom prop = XInternAtom(lw->dpy, "_NET_WM_ICON", False);
    if (prop == None) return;

    // Empty path clears the icon.
    if (!path_utf8 || !*path_utf8) {
      XDeleteProperty(lw->dpy, lw->win, prop);
      XFlush(lw->dpy);
      return;
    }

    uint32_t w = 0, h = 0;
    uint8_t* px = platform_load_image(path_utf8, &w, &h);   // BGRA8 premultiplied
    if (!px) return;
    if (w == 0 || h == 0) { platform_free_image(px); return; }

    // _NET_WM_ICON is an array of CARD32 [w, h, then w*h pixels in 0xAARRGGBB],
    // format 32 (Xlib widens to `long` on LP64). The loader gives premultiplied
    // BGRA; un-premultiply back to straight ARGB so the WM doesn't darken
    // anti-aliased edges. A BGRA byte quad reads as 0xAARRGGBB little-endian.
    std::vector<long> data;
    data.reserve(2 + static_cast<size_t>(w) * h);
    data.push_back(static_cast<long>(w));
    data.push_back(static_cast<long>(h));
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
      uint32_t b = px[i * 4 + 0], g = px[i * 4 + 1], r = px[i * 4 + 2], a = px[i * 4 + 3];
      if (a > 0 && a < 255) {     // un-premultiply
        r = std::min<uint32_t>(255, (r * 255 + a / 2) / a);
        g = std::min<uint32_t>(255, (g * 255 + a / 2) / a);
        b = std::min<uint32_t>(255, (b * 255 + a / 2) / a);
      }
      data.push_back(static_cast<long>((a << 24) | (r << 16) | (g << 8) | b));
    }
    platform_free_image(px);

    XChangeProperty(lw->dpy, lw->win, prop, XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(data.data()),
                    static_cast<int>(data.size()));
    XFlush(lw->dpy);
  }

  void platform_apply_size_constraints(void* native_handle,
                                        int min_w, int min_h, int max_w, int max_h)
  {
    if (!native_handle) return;
    auto* lw = static_cast<LinuxWindow*>(native_handle);
    float scale = window_scale(lw);
    XSizeHints* sh = XAllocSizeHints();
    if (!sh) return;
    sh->flags = 0;
    if (min_w > 0 || min_h > 0) {
      sh->flags |= PMinSize;
      sh->min_width  = min_w > 0 ? static_cast<int>(static_cast<float>(min_w) * scale + 0.5f) : 0;
      sh->min_height = min_h > 0 ? static_cast<int>(static_cast<float>(min_h) * scale + 0.5f) : 0;
    }
    if (max_w > 0 || max_h > 0) {
      sh->flags |= PMaxSize;
      sh->max_width  = max_w > 0 ? static_cast<int>(static_cast<float>(max_w) * scale + 0.5f) : 32767;
      sh->max_height = max_h > 0 ? static_cast<int>(static_cast<float>(max_h) * scale + 0.5f) : 32767;
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
  void platform_clipboard_set_primary(const char* utf8, uint32_t length)
  { g_clipboard.set_primary_text(utf8, length); }
  int  platform_clipboard_get_primary(char* buf, int buflen)
  { return g_clipboard.get_primary_text(buf, buflen); }
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
                         : (int)(static_cast<float>(hot_x) * (preview ? preview->scale : 1.0f));
    int hy = (hot_y < 0) ? (int)(preview ? preview->h_px / 2 : 0)
                         : (int)(static_cast<float>(hot_y) * (preview ? preview->scale : 1.0f));
    Window  pvwin = None; GC pvgc = nullptr; XImage* pvimg = nullptr;
    if (preview) {
      int scr = DefaultScreen(d);
      XSetWindowAttributes swa; std::memset(&swa, 0, sizeof swa);
      swa.override_redirect = True; swa.save_under = True; swa.background_pixmap = None;
      pvwin = XCreateWindow(d, root, 0, 0, preview->w_px, preview->h_px, 0,
                            DefaultDepth(d, scr), InputOutput, DefaultVisual(d, scr),
                            CWOverrideRedirect | CWSaveUnder | CWBackPixmap, &swa);
      pvgc  = XCreateGC(d, pvwin, 0, nullptr);
      pvimg = XCreateImage(d, DefaultVisual(d, scr), static_cast<unsigned int>(DefaultDepth(d, scr)), ZPixmap, 0,
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
            float sc = static_cast<float>(cdpi) / 96.0f; if (sc <= 0.0f) sc = 1.0f;
            int llx = (int)(static_cast<float>(lx) / sc), lly = (int)(static_cast<float>(ly) / sc);
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
            float sc = static_cast<float>(cdpi) / 96.0f; if (sc <= 0.0f) sc = 1.0f;
            result = cs->dispatch_dnd_drop(cframe, (int)(static_cast<float>(lx) / sc), (int)(static_cast<float>(ly) / sc),
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

  // linux: window_scale folds the zoom into every conversion.
  bool platform_supports_ui_scale() { return true; }
  bool platform_supports_tree_popup() { return true; }

  void platform_timer_start(Session* session, uint32_t interval_ms)
  {
    if (!session || interval_ms == 0) return;
    ensure_timerfd();
    client_timer_sessions()[session] = interval_ms;
    refresh_timer_arm();
  }

  void platform_timer_stop(Session* session)
  {
    if (!session) return;
    client_timer_sessions().erase(session);
    refresh_timer_arm();
  }

  // Accessibility: Linux has NO provider yet - AT-SPI is its own wave (see
  // plans/accessibility.md 6.5), so a neui window stays opaque to Orca. Both
  // seams are no-ops rather than absent so the shared host code has one shape
  // on every platform; a client's declarations are stored and cost nothing,
  // and start being read the day the AT-SPI provider lands.
  void platform_a11y_notify(void* /*frame_native_handle*/,
                            uint32_t /*widget_id*/, int /*change*/) {}
  void platform_a11y_announce(void* /*frame_native_handle*/,
                              const char* /*utf8*/, bool /*assertive*/) {}

  // -------------------------------------------------------------------------
  // Relative (unbounded) pointer mode. Warp-back model, same as win32; see
  // dispatch_motion for the echo filtering and platform.h for why macOS differs.
  //
  // The hide uses the cached empty-pixmap cursor directly rather than going
  // through platform_set_cursor, so a drag does not clobber the widget's
  // NEUI_ATTR_CURSOR shape - g_cursor_kind is left alone and re-asserted on end.

  bool platform_supports_relative_pointer() { return true; }

  bool platform_begin_relative_pointer(void* native_handle,
                                        int* out_anchor_x, int* out_anchor_y)
  {
    auto* lw = static_cast<LinuxWindow*>(native_handle);
    if (!lw || !lw->dpy || !lw->win) return false;

    // Current pointer position in ROOT coordinates - the space XWarpPointer and
    // MotionNotify's x_root/y_root use.
    Window       root = None, child = None;
    int          rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned int mask = 0;
    if (!XQueryPointer(lw->dpy, lw->win, &root, &child, &rx, &ry, &wx, &wy, &mask))
      return false;

    g_relative_active   = true;
    g_relative_window   = lw;
    g_relative_anchor_x = rx;
    g_relative_anchor_y = ry;
    g_relative_last_x   = rx;   // first delta is measured from the press point
    g_relative_last_y   = ry;
    if (out_anchor_x) *out_anchor_x = rx;
    if (out_anchor_y) *out_anchor_y = ry;

    if (Cursor hidden = x11_cursor_for(lw->dpy, lw->win, NEUI_CURSOR_NONE)) {
      XDefineCursor(lw->dpy, lw->win, hidden);
      XFlush(lw->dpy);
    }
    return true;
  }

  void platform_end_relative_pointer(void* native_handle,
                                      int anchor_x, int anchor_y)
  {
    auto* lw = static_cast<LinuxWindow*>(native_handle);
    g_relative_active = false;
    g_relative_window = nullptr;
    if (!lw || !lw->dpy || !lw->win) return;

    XWarpPointer(lw->dpy, None, DefaultRootWindow(lw->dpy), 0, 0, 0, 0,
                  anchor_x, anchor_y);
    // Re-assert the widget's own shape. platform_set_cursor short-circuits when
    // the kind is unchanged, and g_cursor_kind was never touched by the hide, so
    // the cached value has to be cleared to force the re-apply.
    const int restore = g_cursor_kind;
    g_cursor_kind = -1;
    platform_set_cursor(restore);
  }

  void platform_set_cursor(int kind)
  {
    // Called from the hover walk on every motion event; XDefineCursor is a
    // server round-trip, so no-op when the shape hasn't changed. (Session
    // already dedupes, but this is the seam an internal caller could reach
    // directly and the round-trip is worth guarding twice.)
    if (kind == g_cursor_kind) return;
    g_cursor_kind = kind;
    // Unlike win32/macOS, X cursors are sticky per WINDOW until changed - there
    // is no per-message reset to fight. So set it on every one of our windows
    // (small count) rather than tracking which one is under the pointer.
    for (auto& kv : g_windows) {
      LinuxWindow* lw = kv.second;
      if (!lw->dpy || !lw->win) continue;
      if (kind == NEUI_CURSOR_DEFAULT || kind == NEUI_CURSOR_ARROW) {
        // Inherit from the parent / root rather than pinning our own arrow;
        // this is what restores the WM's cursor at the frame edges.
        XUndefineCursor(lw->dpy, lw->win);
      } else if (Cursor c = x11_cursor_for(lw->dpy, lw->win, kind)) {
        XDefineCursor(lw->dpy, lw->win, c);
      } else {
        XUndefineCursor(lw->dpy, lw->win);   // creation failed: don't pin junk
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

  // File dialog: XDG portal first, neui-drawn browser as the fallback.
  //
  // The fall-through condition is deliberately narrow. `unavailable` means no
  // dialog reached the user (no libdbus, no bus, no portal, malformed reply,
  // timeout) and the browser runs. `cancelled` means the portal DID show a
  // chooser and the user said no - showing a second dialog on top of that
  // would be worse than having no portal path at all.
  int platform_file_dialog(void* native_handle, int save,
                           const neui_file_dialog_t* desc,
                           neui_file_path_cb cb, void* userdata)
  {
    auto* owner = static_cast<LinuxWindow*>(native_handle);

    // The portal wants an "x11:<hex window id>" parent so it can set the
    // dialog transient for our window. An unrealised frame just gets "",
    // which the portal accepts as "no parent".
    std::string parent;
    if (owner && owner->win) {
      char buf[64];
      std::snprintf(buf, sizeof buf, "x11:%lx", (unsigned long)owner->win);
      parent = buf;
    }

    std::vector<std::string> paths;
    // Block the owner for the portal dialog too - it is modal from the user's
    // point of view, and fd_portal_pump keeps our windows repainting, which
    // would otherwise let them accept clicks behind the chooser.
    bool prev_disabled = owner ? owner->input_disabled : false;
    if (owner) owner->input_disabled = true;
    neui_detail::PortalResult pr =
      neui_detail::file_dialog_portal(save != 0, parent, desc, paths,
                                      &fd_portal_pump, nullptr);
    if (owner) owner->input_disabled = prev_disabled;

    if (pr == neui_detail::PortalResult::cancelled) return 0;
    if (pr == neui_detail::PortalResult::unavailable) {
      paths.clear();
      int n = run_file_browser(owner, save != 0, desc, paths);
      if (n <= 0) return n;
    } else if (save) {
      // Apply the documented completion rule to a portal SAVE result. The
      // portal has no equivalent of SetDefaultExtension, and a GTK-style
      // backend does not append the active filter's extension itself - so
      // without this the primary Linux path was the one platform that quietly
      // ignored the rule ("lead" + Presets(*.preset) came back as ".../lead").
      // The drawn browser completes in collect_result and does not come here.
      std::vector<neui_detail::FileFilter> filters = neui_detail::parse_filters(desc);
      if (!paths.empty() && !filters.empty()) {
        size_t fi = neui_detail::clamp_default_filter(desc, filters);
        std::string leaf      = neui_detail::path_leaf(paths[0]);
        std::string completed = neui_detail::complete_extension(leaf, filters[fi]);
        if (completed != leaf)
          paths[0] = neui_detail::path_join(neui_detail::path_parent(paths[0]), completed);
      }
    }

    if (cb)
      for (const auto& p : paths) cb(userdata, p.c_str());
    return (int)paths.size();
  }

} // namespace xpl_host
