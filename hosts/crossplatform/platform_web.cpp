// Web (Emscripten) platform layer for the crossplatform host
// (plans/wasm-host.md).
//
// Each frame (APPWINDOW / PLUGWINDOW / DIALOG) is a <canvas> in a wrapper
// <div>, drawn by the Canvas 2D backend. The browser must never block, so the
// event loop is the Linux select() loop with one change: where Linux sleeps in
// select(), this layer suspends the wasm stack in web_wait() (JSPI or Asyncify)
// and the browser runs. DOM listeners never dispatch: they only queue a record
// (keyed by frame id - the WebFrame may be gone by the time it is read) and
// wake the loop, so every Session dispatch, client callback and paint runs on
// the one suspendable stack inside platform_run / platform_run_modal_until.
// A nested loop (modal show, popup_menu) is the same loop with another exit
// condition on the same stack.
//
// Default actions. Whether the browser may scroll the page on a wheel, move
// focus on Tab or open its context menu is only known after neui dispatched
// the event. The listener keeps the event object and resolves the pending
// wait; the suspended stack resumes in the microtask checkpoint right after
// the listener - still inside the browser's event dispatch - and the drain
// calls preventDefault on the kept event when neui consumed it (plans rule 5;
// proven in Chromium and WebKit in Stage 0).
//
// Input routing mirrors platform_linux.cpp (the overlay pre-check order, the
// key order, double-click synthesis, the scroll router); the Linux code is
// tied to X types and LinuxWindow fields, so it is copied, with "mirrors
// platform_linux.cpp" notes, rather than shared.
//
// Not yet here: clipboard + IME (Stage 5), DnD (Stage 6), message box + the
// blocking-call guards (Stage 4), page embedding (Stage 7).

#include "host.h"
#include "platform.h"
#include <neui/neui.h>

#include <emscripten.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

// Full DataItem definition for the item-based clipboard stubs below.
#include "../shared/clipboard_item.h"
#include "../shared/damage.h"
#include "../shared/metrics.h"
#include "../shared/web/keys_web.h"
#include "../shared/web/theme_provider_web.h"
#include "../../backends/canvas2d/canvas2d_backend.h"

// This TU emits the single stb_image implementation for the web host.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_DEPRECATED
#include "../shared/image_loader_stb.h"

// ---------------------------------------------------------------------------
// JavaScript half: Module.neuiWeb (frames, the wake hook, kept events) and
// Module.neuiCanvases (canvas by frame id, read by the Canvas 2D backend).
//
// Record types (neui_web_push): 1 resize, 2 focus, 3 blur, 10 pointer down,
// 11 pointer up, 12 pointer move, 13 pointer leave, 14 context menu,
// 15 wheel, 20 key down, 21 key up, 30 colour scheme (frame 0).

EM_JS_DEPS(neui_web, "$UTF8ToString,$stringToUTF8");

EM_JS(void, web_setup, (), {
  if (Module.neuiWeb) return;
  var W = Module.neuiWeb = { frames: {}, wake: null, kept: new Map(), nextEv: 1 };
  Module.neuiCanvases = Module.neuiCanvases || {};
  W.wakeUp = function() { if (W.wake) { var w = W.wake; W.wake = null; w(2); } };
  // Queue a record for the loop and wake it if it is waiting.
  W.push = function(type, frame, a, b, c, d, e, f) {
    Module._neui_web_push(type, frame, a || 0, b || 0, c || 0, d || 0, e || 0, f || 0);
    W.wakeUp();
  };
  // Hold an event until the drain decides about its default action.
  W.keep = function(ev) {
    var id = W.nextEv++;
    W.kept.set(id, ev);
    if (W.kept.size > 256) W.kept.delete(W.kept.keys().next().value);
    return id;
  };
  W.mods = function(e) {
    return (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0) | (e.metaKey ? 8 : 0) |
           ((e.getModifierState && e.getModifierState('AltGraph')) ? 16 : 0);
  };
  W.apple = /Mac|iPhone|iPad|iPod/.test((navigator.userAgentData && navigator.userAgentData.platform) ||
                                        navigator.platform || navigator.userAgent || "");
  // Colour scheme: 1 dark, 2 light, 0 no preference.
  W.scheme = function() {
    if (typeof matchMedia !== 'function') return 0;
    if (matchMedia('(prefers-color-scheme: dark)').matches) return 1;
    if (matchMedia('(prefers-color-scheme: light)').matches) return 2;
    return 0;
  };
  // A device-pixel-ratio change (zoom, another monitor) does not always
  // resize a canvas' device-pixel box, so watch the ratio itself: the query
  // for the current ratio stops matching once it changes. Every frame then
  // re-reports its size (device size 0 = derive it from the new ratio).
  W.watchDpr = function() {
    if (typeof matchMedia !== 'function') return;
    var q = matchMedia('(resolution: ' + devicePixelRatio + 'dppx)');
    var once = function() {
      if (q.removeEventListener) q.removeEventListener('change', once); else if (q.removeListener) q.removeListener(once);
      for (var id in W.frames) {
        var cv = W.frames[id].cv;
        W.push(1, +id, cv.clientWidth, cv.clientHeight, 0, 0);
      }
      W.watchDpr();
    };
    if (q.addEventListener) q.addEventListener('change', once); else if (q.addListener) q.addListener(once);
  };
  W.watchDpr();
  if (typeof matchMedia === 'function') {
    var mq = matchMedia('(prefers-color-scheme: dark)');
    var onScheme = function() { W.push(30, 0, W.scheme()); };
    if (mq.addEventListener) mq.addEventListener('change', onScheme); else if (mq.addListener) mq.addListener(onScheme);
  }
});

// Suspend until a record is queued (returns 2), the next animation frame when
// want_raf (1), or timeout_ms (0; < 0 = no timeout).
EM_ASYNC_JS(int, web_wait, (int timeout_ms, int want_raf), {
  var W = Module.neuiWeb;
  return await new Promise(function(resolve) {
    var done = false, tid = 0, rid = 0;
    var fin = function(why) {
      if (done) return;
      done = true;
      W.wake = null;
      if (tid) clearTimeout(tid);
      if (rid) cancelAnimationFrame(rid);
      resolve(why);
    };
    W.wake = fin;
    if (want_raf) rid = requestAnimationFrame(function() { rid = 0; fin(1); });
    if (timeout_ms >= 0) tid = setTimeout(function() { tid = 0; fin(0); }, timeout_ms);
  });
});

// The drain's decision about a kept event's default action.
EM_JS(void, web_verdict, (int ev_id, int consume), {
  var W = Module.neuiWeb, e = W.kept.get(ev_id);
  if (!e) return;
  W.kept.delete(ev_id);
  if (consume) e.preventDefault();
});

EM_JS(double, web_dpr, (), { return (typeof devicePixelRatio === 'number' && devicePixelRatio > 0) ? devicePixelRatio : 1; });
EM_JS(int, web_is_apple, (), { return Module.neuiWeb.apple ? 1 : 0; });
EM_JS(int, web_scheme, (), { return Module.neuiWeb.scheme(); });

// The page URL's NEUI_* query parameters ("?NEUI_DEBUG_DAMAGE=1") as
// "KEY=value\n" lines: the host's debug switches are environment variables.
EM_JS(void, web_neui_query, (char* buf, int cap), {
  var out = "";
  try {
    new URLSearchParams(location.search).forEach(function(v, k) {
      if (k.indexOf("NEUI_") === 0) out += k + "=" + v + "\n";
    });
  } catch (e) {}
  stringToUTF8(out, buf, cap);
});

EM_JS(int, web_reduced_motion, (), {
  return (typeof matchMedia === 'function' && matchMedia('(prefers-reduced-motion: reduce)').matches) ? 1 : 0;
});

// kind: 0 APPWINDOW, 1 PLUGWINDOW, 2 DIALOG. An APPWINDOW mounts into
// #neui-root when the page has one, else into <body>. In a container with a
// size of its own the APPWINDOW fills it (and follows it); otherwise the
// canvas takes the frame's create() size. A dialog floats above the page
// (viewport coordinates).
EM_JS(void, web_frame_create, (int id, int kind, int x, int y, int w, int h), {
  var W = Module.neuiWeb;
  var host = (kind === 0 && document.getElementById('neui-root')) || document.body;
  var fill = kind === 0 && host !== document.body && host.clientWidth > 0 && host.clientHeight > 0;
  var wrap = document.createElement('div');
  wrap.className = 'neui-frame';
  wrap.style.cssText = 'display:none;line-height:0;user-select:none;-webkit-user-select:none;' +
                       (fill ? 'width:100%;height:100%;' : "");
  if (kind === 2) {
    wrap.style.position = 'fixed';
    wrap.style.left = x + 'px'; wrap.style.top = y + 'px';
    wrap.style.zIndex = '1000';
  }
  var cv = document.createElement('canvas');
  cv.tabIndex = 0;
  // touch-action:none: a drag on the canvas is a neui drag, not a page pan.
  cv.style.cssText = 'display:block;outline:none;touch-action:none;' +
                     (fill ? 'width:100%;height:100%;' : 'width:' + w + 'px;height:' + h + 'px;');
  wrap.appendChild(cv);
  host.appendChild(wrap);
  Module.neuiCanvases[id] = cv;
  var f = W.frames[id] = { wrap: wrap, cv: cv, ro: null, body: host === document.body, fill: fill };

  // Size + device pixel size (zoom, monitor changes) -> one RESIZE record.
  f.ro = new ResizeObserver(function(entries) {
    var e = entries[entries.length - 1], pw = 0, ph = 0;
    if (e.devicePixelContentBoxSize && e.devicePixelContentBoxSize[0]) {
      pw = e.devicePixelContentBoxSize[0].inlineSize;
      ph = e.devicePixelContentBoxSize[0].blockSize;
    }
    W.push(1, id, e.contentRect.width, e.contentRect.height, pw, ph);
  });
  try { f.ro.observe(cv, { box: 'device-pixel-content-box' }); } catch (err) { f.ro.observe(cv); }
  cv.addEventListener('focus', function() { W.push(2, id); });
  cv.addEventListener('blur',  function() { W.push(3, id); });

  // Pointer: canvas-relative CSS px (= frame-local logical px). A press
  // captures the pointer, so a drag keeps reporting outside the canvas and
  // its release always arrives (the Linux pressed-widget model).
  var at = function(e) { var r = cv.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; };
  cv.addEventListener('pointerdown', function(e) {
    var p = at(e);
    try { cv.setPointerCapture(e.pointerId); } catch (err) {}
    if (document.activeElement !== cv) { try { cv.focus({ preventScroll: true }); } catch (err) { cv.focus(); } }
    W.push(10, id, p[0], p[1], e.button, e.buttons, W.mods(e), e.timeStamp);
  });
  cv.addEventListener('pointerup', function(e) {
    var p = at(e);
    W.push(11, id, p[0], p[1], e.button, e.buttons, W.mods(e), e.timeStamp);
  });
  cv.addEventListener('pointercancel', function(e) {
    var p = at(e);
    W.push(11, id, p[0], p[1], 0, 0, W.mods(e), e.timeStamp);
  });
  cv.addEventListener('pointermove', function(e) {
    var p = at(e);
    W.push(12, id, p[0], p[1], 0, e.buttons, W.mods(e), e.timeStamp);
  });
  cv.addEventListener('pointerleave', function() { W.push(13, id); });
  cv.addEventListener('contextmenu', function(e) {
    var p = at(e);
    W.push(14, id, p[0], p[1], W.keep(e));
  });
  cv.addEventListener('wheel', function(e) {
    var p = at(e);
    W.push(15, id, p[0], p[1], e.deltaX, e.deltaY, e.deltaMode, W.keep(e));
  }, { passive: false });
  // Keys reach the focused canvas. The code / key strings go through a
  // fixed C++ buffer (64 bytes each; both are short names or one character).
  var key = function(type, e) {
    var buf = Module._neui_web_key_buf();
    stringToUTF8(e.code || "", buf, 64);
    stringToUTF8(e.key || "", buf + 64, 64);
    Module._neui_web_push_key(type, id, type === 20 ? W.keep(e) : 0, W.mods(e));
    W.wakeUp();
  };
  cv.addEventListener('keydown', function(e) { if (!e.isComposing) key(20, e); });
  cv.addEventListener('keyup',   function(e) { if (!e.isComposing) key(21, e); });
});

EM_JS(void, web_frame_destroy, (int id), {
  var W = Module.neuiWeb, f = W.frames[id]; if (!f) return;
  if (f.ro) f.ro.disconnect();
  if (f.wrap.parentNode) f.wrap.parentNode.removeChild(f.wrap);
  delete W.frames[id];
  delete Module.neuiCanvases[id];
});

EM_JS(void, web_frame_show, (int id, int show), {
  var f = Module.neuiWeb.frames[id]; if (!f) return;
  f.wrap.style.display = show ? 'block' : 'none';
});

EM_JS(void, web_frame_focus, (int id), {
  var f = Module.neuiWeb.frames[id]; if (!f) return;
  try { f.cv.focus({ preventScroll: true }); } catch (e) { f.cv.focus(); }
});

// Size in CSS px (not for a frame that fills its container); a dialog also
// moves (x / y are viewport coordinates).
EM_JS(void, web_frame_geometry, (int id, int x, int y, int w, int h, int move), {
  var f = Module.neuiWeb.frames[id]; if (!f) return;
  if (!f.fill) { f.cv.style.width = w + 'px'; f.cv.style.height = h + 'px'; }
  if (move) { f.wrap.style.left = x + 'px'; f.wrap.style.top = y + 'px'; }
});

// Min / max size (CSS px, <= 0 = none) for a frame that sizes itself.
EM_JS(void, web_frame_constraints, (int id, int minw, int minh, int maxw, int maxh), {
  var f = Module.neuiWeb.frames[id]; if (!f || f.fill) return;
  var s = f.cv.style;
  s.minWidth = minw > 0 ? minw + 'px' : ""; s.minHeight = minh > 0 ? minh + 'px' : "";
  s.maxWidth = maxw > 0 ? maxw + 'px' : ""; s.maxHeight = maxh > 0 ? maxh + 'px' : "";
});

// The title labels the canvas for assistive technology; only a full-page app
// (mounted straight into <body>) also owns the document title.
EM_JS(void, web_frame_title, (int id, const char* text), {
  var f = Module.neuiWeb.frames[id]; if (!f) return;
  var t = UTF8ToString(text);
  f.cv.setAttribute('aria-label', t);
  if (f.body) document.title = t;
});

EM_JS(void, web_set_cursor, (const char* css), {
  var W = Module.neuiWeb, c = UTF8ToString(css);
  for (var id in W.frames) W.frames[id].cv.style.cursor = c;
});

namespace xpl_host
{
namespace
{
  // Per-frame record, stashed on WidgetData::native_handle.
  struct WebFrame
  {
    int       id           = 0;        // DOM / canvas id (Module.neuiCanvases)
    Session*  session      = nullptr;
    uint32_t  widget_index = 0;
    bool      is_appwindow = false;
    bool      is_dialog    = false;
    bool      counted      = false;    // contributes to g_appwindow_count
    float     dpr          = 1.0f;     // device px per CSS px (= logical px)
    // needs_paint = a FULL repaint is pending; `damage` collects partial
    // rects while it is false (a pending full repaint absorbs them).
    bool      needs_paint  = false;
    neui_detail::DamageRegion damage;
    bool      toast_anim   = false;
    bool      clock_anim   = false;
    // Set while a modal child is up (its input is dropped).
    bool      input_disabled = false;
    // GRID / SECTION spring-back in flight (the heartbeat steps them). 0 = none.
    uint32_t  bouncing_grid_index    = 0;
    uint32_t  bouncing_section_index = 0;
    // Fractional wheel-notch accumulators for the classic line path.
    double    scroll_v_accum = 0.0;
    double    scroll_h_accum = 0.0;
    // A left press the in-frame menubar consumed: swallow its release.
    bool      swallow_release = false;
    // Double-click synthesis (the browser's dblclick comes after two clicks;
    // neui wants the second press itself).
    double    last_click_ms  = -1.0e9;
    float     last_click_x   = 0.0f;
    float     last_click_y   = 0.0f;
  };

  std::unordered_map<int, WebFrame*> g_frames;
  int      g_next_frame_id  = 1;
  int      g_appwindow_count = 0;
  bool     g_quit           = false;
  bool     g_apple          = false;

  enum : int {
    EV_RESIZE = 1, EV_FOCUS = 2, EV_BLUR = 3,
    EV_PDOWN = 10, EV_PUP = 11, EV_PMOVE = 12, EV_PLEAVE = 13, EV_CONTEXT = 14, EV_WHEEL = 15,
    EV_KEYDOWN = 20, EV_KEYUP = 21,
    EV_SCHEME = 30,
  };
  struct WebEvent
  {
    int type; int frame;
    double a, b, c, d, e, f;
    std::string code, key;   // key records only
  };
  std::deque<WebEvent> g_queue;

  // The animation heartbeat: one 16 ms tick shared by every frame while any
  // of them animates (as the Linux timerfd), 0 = idle.
  double g_next_tick_ms = 0.0;

  // One wheel notch scrolls this many lines (the Win32 default, as on Linux).
  constexpr int WEB_WHEEL_LINES = 3;
  // A pixel-mode wheel delta this large is one notch (Chromium's mouse wheel).
  constexpr double WEB_PX_PER_NOTCH = 100.0;

  WebFrame* find_frame(int id)
  {
    auto it = g_frames.find(id);
    return it == g_frames.end() ? nullptr : it->second;
  }

  uint32_t dpi_for(float dpr)
  {
    long dpi = std::lround(static_cast<double>(dpr) * 96.0);
    return dpi > 0 ? static_cast<uint32_t>(dpi) : 96u;
  }

  bool any_window_animating()
  {
    for (auto& kv : g_frames) {
      const WebFrame* f = kv.second;
      if (f->toast_anim || f->clock_anim || f->bouncing_grid_index || f->bouncing_section_index)
        return true;
    }
    return false;
  }

  void arm_heartbeat()
  {
    if (g_next_tick_ms == 0.0) g_next_tick_ms = emscripten_get_now() + 16.0;
  }

  // ---- Painting (mirrors platform_linux.cpp) --------------------------------

  bool paint_pending(const WebFrame* f)
  {
    return f->needs_paint || !f->damage.empty();
  }

  void paint_window(WebFrame* f)
  {
    // The pending state is taken before painting, so an invalidation raised
    // DURING the paint is kept for the next frame.
    const bool full = f->needs_paint;
    neui_detail::DamageRegion damage = std::move(f->damage);
    f->needs_paint = false;
    f->damage.clear();
    if (!f->session) return;
    auto* wd = f->session->get_widget(f->widget_index);
    if (wd && wd->render_ctx)
      f->session->paint_frame(wd->render_ctx, f->widget_index, full ? nullptr : &damage);
  }

  bool any_paint_pending()
  {
    for (auto& kv : g_frames)
      if (paint_pending(kv.second)) return true;
    return false;
  }

  void flush_pending_paints()
  {
    // Snapshot ids: a paint callback can create / destroy frames.
    std::vector<int> todo;
    for (auto& kv : g_frames)
      if (paint_pending(kv.second)) todo.push_back(kv.first);
    for (int id : todo)
      if (auto* f = find_frame(id)) paint_window(f);
  }

  // ---- Animation (mirrors platform_linux.cpp) -------------------------------

  // Advance any in-flight GRID / SECTION spring-back by one 16 ms step.
  void step_scroll_bounce(WebFrame* f)
  {
    using namespace neui_detail;
    Session* s = f->session;
    if (f->bouncing_grid_index) {
      auto* hw = s->get_widget(f->bouncing_grid_index);
      GridModel* model = hw ? hw->grid_model_ptr() : nullptr;
      if (!model) {
        f->bouncing_grid_index = 0;
      } else {
        auto cfg = grid_read_config(hw->attrs.get());
        GridViewport vp = grid_compute_viewport(*model, hw->width, hw->height,
                                                 cfg.row_h, cfg.header_h);
        bool more = grid_scroll_bounce_step(*model, vp, cfg.row_h);
        s->invalidate_widget_rect(f->bouncing_grid_index);
        if (!more) f->bouncing_grid_index = 0;
      }
    }
    if (f->bouncing_section_index) {
      auto* sw = s->get_widget(f->bouncing_section_index);
      SectionScrollState* st = sw ? sw->scroll_state_ptr() : nullptr;
      const SectionLayout* L = sw ? sw->section_layout_ptr() : nullptr;
      if (!st || !L) {
        f->bouncing_section_index = 0;
      } else {
        bool more_v = section_scroll_bounce_step(*st, *L, false);
        bool more_h = section_scroll_bounce_step(*st, *L, true);
        s->invalidate_widget_rect(f->bouncing_section_index);
        sw->notify_scroll_changed();
        if (!more_v && !more_h) f->bouncing_section_index = 0;
      }
    }
  }

  void tick_animations()
  {
    std::vector<int> ids;
    for (auto& kv : g_frames) ids.push_back(kv.first);
    for (int id : ids) {
      WebFrame* f = find_frame(id);
      if (!f || !f->session) continue;
      if (f->toast_anim) f->needs_paint = true;
      step_scroll_bounce(f);
      if (f->clock_anim)
        f->clock_anim = f->session->animation_tick(f->widget_index) != 0;
    }
  }

  // Run the heartbeat if it is due; disarm it once nothing animates.
  void service_timers()
  {
    if (g_next_tick_ms == 0.0) return;
    const double now = emscripten_get_now();
    if (now < g_next_tick_ms) return;
    tick_animations();
    g_next_tick_ms = any_window_animating() ? now + 16.0 : 0.0;
  }

  // web_wait timeout: until the heartbeat is due, or none.
  int wait_timeout_ms(int cap_ms)
  {
    int t = -1;
    if (g_next_tick_ms != 0.0) {
      double d = g_next_tick_ms - emscripten_get_now();
      t = d <= 0.0 ? 0 : static_cast<int>(std::ceil(d));
    }
    if (cap_ms >= 0 && (t < 0 || t > cap_ms)) t = cap_ms;
    return t;
  }

  // ---- Window teardown (mirrors platform_linux.cpp) -------------------------

  void destroy_native(WebFrame* f)
  {
    g_frames.erase(f->id);
    web_frame_destroy(f->id);
    delete f;
  }

  // The teardown every close path shares: releases the render ctx, ends a
  // dialog's modal pump (a modal dialog also gives its owner input and focus
  // back), removes the frame's DOM and drops the appwindow count.
  void teardown_window(WebFrame* f, WidgetData* wd)
  {
    Session* s = f->session;
    if (s && wd && wd->is_dialog() && wd->owner_index != 0 &&
        s->_widgets.exists(wd->owner_index)) {
      if (auto* fw = dynamic_cast<FrameWidget*>(wd))
        fw->end_modal_pump();   // widget_show's pump unwinds
      const bool modal = !wd->attrs || wd->attrs->get_int(NEUI_ATTR_MODAL, 1) != 0;
      auto& owner = s->_widgets[wd->owner_index];
      if (modal && owner.native_handle) {
        platform_set_window_enabled(owner.native_handle, true);
        platform_activate_window(owner.native_handle);
      }
    }
    auto* backend = platform_get_backend();
    if (wd && wd->render_ctx && backend) {
      if (s) s->_asset_manager.release_context(wd->render_ctx, backend);
      backend->destroy_context(wd->render_ctx);
      wd->render_ctx = nullptr;
    }
    const bool was_app = f->is_appwindow && f->counted;
    if (wd) wd->native_handle = nullptr;
    destroy_native(f);
    if (was_app && g_appwindow_count > 0 && --g_appwindow_count <= 0)
      g_quit = true;
  }

  // Client gets APP_QUIT (and may veto), then the frame is torn down.
  void handle_close(WebFrame* f)
  {
    Session* s = f->session;
    const int id = f->id;
    neui_event_t ev = {};
    ev.type = NEUI_EVENT_APP_QUIT;
    if (auto* fw = s ? s->get_widget(f->widget_index) : nullptr)
      ev.data.mouse.widget.id = fw->widget_id;  // the frame closing
    const bool allow = !s || s->dispatch_event(&ev);
    // The handler may have destroyed the frame itself.
    f = find_frame(id);
    if (!f || !allow) return;
    teardown_window(f, s ? s->get_widget(f->widget_index) : nullptr);
  }

  // ---- Pointer (mirrors platform_linux.cpp) ---------------------------------

  void send_mouse(WebFrame* f, neui_event_type_t type, uint32_t target,
                  float lx, float ly, uint32_t buttonmap)
  {
    if (target == 0) return;
    Session* s = f->session;
    auto* hw = s->get_widget(target);
    if (!hw) return;
    neui_event_t ev = {};
    ev.type                 = type;
    ev.data.mouse.widget    = { hw->widget_id };
    ev.data.mouse.x         = static_cast<int>(lx);
    ev.data.mouse.y         = static_cast<int>(ly);
    ev.data.mouse.buttonmap = buttonmap;
    s->dispatch_mouse_event(target, &ev);
  }

  void pointer_down(WebFrame* f, float lx, float ly, int button, uint32_t buttons,
                    uint32_t mods, double t_ms)
  {
    Session* s = f->session;
    const uint32_t bm = neui_detail::web_buttonmap(buttons, mods);
    if (button == 0) {
      // Overlay pre-checks, in the macOS / Linux order.
      if (s->_popup_active) { s->handle_popup_click(lx, ly); return; }
      if (s->handle_toast_click(f->widget_index, lx, ly)) return;
      if (s->handle_combo_click(lx, ly)) return;
      // The in-frame menubar owns clicks in its band and while open; swallow
      // the matching release so it can't reach a widget under the dropdown.
      if (s->handle_menubar_click(f->widget_index, lx, ly)) {
        f->swallow_release = true;
        return;
      }
      const bool dbl = (t_ms - f->last_click_ms <= 400.0) &&
                       std::fabs(lx - f->last_click_x) <= 4.0f &&
                       std::fabs(ly - f->last_click_y) <= 4.0f;
      f->last_click_ms = t_ms;
      f->last_click_x  = lx;
      f->last_click_y  = ly;
      uint32_t hit = s->widget_at(lx, ly, f->widget_index);
      if (dbl) {
        send_mouse(f, NEUI_EVENT_MOUSE_BUTTON_DBLCLICK, hit, lx, ly, bm | 0x0001);
        f->last_click_ms = -1.0e9;   // a 3rd press starts a fresh single click
        return;
      }
      s->set_focus(hit);
      s->set_pressed(hit);
      send_mouse(f, NEUI_EVENT_MOUSE_BUTTON_DOWN, hit, lx, ly, bm | 0x0001);
      return;
    }
    if (button == 2) {
      if (s->_popup_active) { s->handle_popup_click(lx, ly); return; }
      if (s->_menu_open) { s->close_menubar_menu(); return; }
      uint32_t hit = s->widget_at(lx, ly, f->widget_index);
      send_mouse(f, NEUI_EVENT_MOUSE_RBUTTON_DOWN, hit, lx, ly, bm | 0x0002);
    }
    // Middle button: no PRIMARY selection in a browser - nothing to paste.
  }

  void pointer_up(WebFrame* f, float lx, float ly, int button)
  {
    Session* s = f->session;
    if (button == 0) {
      if (f->swallow_release) { f->swallow_release = false; return; }
      if (s->_combo_sb_dragging) { s->_combo_sb_dragging = false; return; }
      uint32_t hit     = s->widget_at(lx, ly, f->widget_index);
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
      if (hit == pressed) {   // CLICK only when up lands on the pressed widget
        ev.type = NEUI_EVENT_MOUSE_BUTTON_CLICK;
        s->dispatch_mouse_event(hit, &ev);
      }
      return;
    }
    if (button == 2) {
      uint32_t hit = s->widget_at(lx, ly, f->widget_index);
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

  void pointer_move(WebFrame* f, float lx, float ly, uint32_t buttons, uint32_t mods)
  {
    Session* s = f->session;
    uint32_t bm = neui_detail::web_buttonmap(buttons, mods);
    // A left drag is in progress whenever there is a pressed widget.
    if (s->_pressed_widget != 0) bm |= 0x0001;
    if (s->_popup_active) { s->handle_popup_hover(lx, ly); return; }
    if (s->_menu_open) {
      if (s->handle_menubar_hover(f->widget_index, lx, ly)) return;
    } else if (s->handle_menubar_band_hover(f->widget_index, lx, ly)) {
      return;   // hovering the closed band: highlight only
    }
    if (s->handle_combo_scroll_drag(ly)) return;
    if (s->handle_combo_hover(lx, ly)) return;

    uint32_t hit = s->widget_at(lx, ly, f->widget_index);
    s->set_hovered(hit);
    uint32_t target = hit;
    if (s->_pressed_widget != 0 && (bm & 0x0001))
      target = s->_pressed_widget;   // drags go to the pressed widget
    send_mouse(f, NEUI_EVENT_MOUSE_MOVE, target, lx, ly, bm);
  }

  void pointer_leave(WebFrame* f)
  {
    Session* s = f->session;
    if (s->_pressed_widget != 0) return;   // a captured drag continues outside
    s->set_hovered(0);
    if (s->_menu_band_hover != 0) { s->_menu_band_hover = 0; f->needs_paint = true; }
  }

  // ---- Wheel (mirrors platform_linux.cpp feed_scroll) -----------------------

  uint32_t find_scrolling_section(Session* s, uint32_t hit)
  {
    auto* hw = s->get_widget(hit);
    if (hw && hw->scroll_state_ptr()) return hit;
    for (uint32_t p : s->_widgets.get_all_parents(hit)) {
      auto* pw = s->get_widget(p);
      if (pw && pw->scroll_state_ptr()) return p;
    }
    return 0;
  }

  int take_lines(double& accum, double notches)
  {
    accum += notches;
    int lines = static_cast<int>(accum);   // toward zero
    accum -= lines;
    return lines;
  }

  // Feed a precise delta (logical px; +dv = up, +dh = left) into a scrolling
  // SECTION's kinetics. Browsers scroll smoothly, so smooth is the default.
  bool section_kinetic_wheel(WebFrame* f, uint32_t sec_idx, double dv, double dh)
  {
    using namespace neui_detail;
    auto* sw = f->session->get_widget(sec_idx);
    SectionScrollState* st = sw ? sw->scroll_state_ptr() : nullptr;
    const SectionLayout* L = sw ? sw->section_layout_ptr() : nullptr;
    if (!st || !L) return false;
    bool has_v = section_axis_has_v(st->axis);
    bool has_h = section_axis_has_h(st->axis);
    // A horizontal-only section takes a pure vertical wheel.
    if (!has_v && has_h && dh == 0.0 && dv != 0.0) { dh = dv; dv = 0.0; }
    int  mode   = section_read_kinetics_mode(sw->attrs.get());
    bool smooth = scroll_kinetics_smooth_enabled(mode, /*platform_default_smooth=*/true);
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
    if (changed) { f->session->invalidate_widget_rect(sec_idx); sw->notify_scroll_changed(); }
    if (start_bounce) { f->bouncing_section_index = sec_idx; arm_heartbeat(); }
    return (has_v && dv != 0.0) || (has_h && dh != 0.0);
  }

  // Route one wheel event. dv / dh are notches (+dv = up, +dh = left); pv /
  // ph the same scroll in logical px when the browser reported pixels (the
  // precise paths use them as they are), else 0 and the notches decide.
  // Returns whether neui consumed it (else the page may scroll).
  bool feed_scroll(WebFrame* f, float lx, float ly, double dv, double dh,
                   double pv, double ph, bool have_px)
  {
    using namespace neui_detail;
    Session* s = f->session;
    int vline = take_lines(f->scroll_v_accum, dv);
    int hline = take_lines(f->scroll_h_accum, dh);

    if (s->_open_combo && vline != 0 && s->handle_combo_wheel(lx, ly, vline)) return true;

    uint32_t hit = s->widget_at(lx, ly, f->widget_index);
    if (hit == 0) return false;
    auto* hw = s->get_widget(hit);
    if (!hw) return false;

    // GRID + SMOOTH: the shared kinetics, pixel-precise.
    if (GridModel* model = hw->grid_model_ptr()) {
      auto cfg = grid_read_config(hw->attrs.get());
      if (grid_smooth_enabled(cfg, /*platform_default_smooth=*/true)) {
        GridViewport vp = grid_compute_viewport(*model, hw->width, hw->height,
                                                 cfg.row_h, cfg.header_h);
        GridWheelInput in;
        in.precise  = true;
        in.delta_px = have_px ? pv : dv * static_cast<double>(WEB_WHEEL_LINES) * cfg.row_h;
        GridWheelAction act = grid_scroll_wheel(*model, vp, cfg.row_h, in);
        if (act.changed) s->invalidate_widget_rect(hit);
        if (act.start_bounce) { f->bouncing_grid_index = hit; arm_heartbeat(); }
        return true;
      }
    }

    // Scrolling SECTION: widgets inside get first refusal (classic lines);
    // otherwise the section scrolls.
    uint32_t sec_idx = find_scrolling_section(s, hit);
    if (sec_idx != 0) {
      if (hit != sec_idx && (vline != 0 || hline != 0)) {
        neui_event_t ev = {};
        ev.type         = NEUI_EVENT_MOUSE_WHEEL;
        ev.data.wheel.x = static_cast<int>(lx);
        ev.data.wheel.y = static_cast<int>(ly);
        if (vline != 0) {
          ev.data.wheel.delta = vline; ev.data.wheel.is_horizontal = 0;
          if (s->dispatch_wheel_event(hit, &ev, sec_idx)) return true;
        }
        if (hline != 0) {
          ev.data.wheel.delta = hline; ev.data.wheel.is_horizontal = 1;
          if (s->dispatch_wheel_event(hit, &ev, sec_idx)) return true;
        }
      }
      const double line_px = static_cast<double>(WEB_WHEEL_LINES) * SECTION_WHEEL_LINE_PX;
      return section_kinetic_wheel(f, sec_idx, have_px ? pv : dv * line_px,
                                   have_px ? ph : dh * line_px);
    }

    // Classic line path (listbox / treeview / multiline / custom).
    bool consumed = false;
    if (vline != 0) {
      neui_event_t ev = {};
      ev.type = NEUI_EVENT_MOUSE_WHEEL;
      ev.data.wheel.x = static_cast<int>(lx); ev.data.wheel.y = static_cast<int>(ly);
      ev.data.wheel.delta = vline; ev.data.wheel.is_horizontal = 0;
      consumed |= s->dispatch_wheel_event(hit, &ev);
    }
    if (hline != 0) {
      neui_event_t ev = {};
      ev.type = NEUI_EVENT_MOUSE_WHEEL;
      ev.data.wheel.x = static_cast<int>(lx); ev.data.wheel.y = static_cast<int>(ly);
      ev.data.wheel.delta = hline; ev.data.wheel.is_horizontal = 1;
      consumed |= s->dispatch_wheel_event(hit, &ev);
    }
    return consumed;
  }

  // A WheelEvent: deltaMode 0 = pixels, 1 = lines, 2 = pages; positive
  // deltaY = down, positive deltaX = right (neui's notches run the other way).
  bool handle_wheel(WebFrame* f, float lx, float ly, double dx, double dy, int mode)
  {
    double dv = 0.0, dh = 0.0, pv = 0.0, ph = 0.0;
    bool have_px = false;
    if (mode == 0) {
      have_px = true;
      pv = -dy; ph = -dx;
      dv = pv / WEB_PX_PER_NOTCH; dh = ph / WEB_PX_PER_NOTCH;
    } else {
      const double lines_per = (mode == 1) ? 1.0 : 10.0;   // a page ~ ten lines
      dv = -dy * lines_per / WEB_WHEEL_LINES;
      dh = -dx * lines_per / WEB_WHEEL_LINES;
    }
    if (dv == 0.0 && dh == 0.0) return false;
    return feed_scroll(f, lx, ly, dv, dh, pv, ph, have_px);
  }

  // ---- Keyboard (mirrors platform_linux.cpp dispatch_key_press) -------------

  // Returns whether neui consumed the press (its default action is then
  // prevented: no Tab focus move, no page scroll on Space / arrows, no
  // browser shortcut).
  bool key_down(WebFrame* f, const std::string& code, const std::string& key, uint32_t web_mods)
  {
    Session* s = f->session;
    const uint32_t mods    = neui_detail::web_modifiers_to_neui(web_mods, g_apple);
    const uint32_t keycode = neui_detail::web_key_to_neui(code.c_str(), key.c_str());

    // Tab cycles logical focus.
    if (keycode == NEUI_KEY_TAB) {
      s->focus_next(!(mods & NEUI_KMOD_SHIFT));
      return true;
    }
    // An open in-frame menubar captures the keyboard.
    if (s->_menu_open) { s->handle_menubar_key(keycode, mods); return true; }
    // Menubar accelerators.
    if (keycode != 0 && s->try_menubar_accel(keycode, mods, f->widget_index)) return true;

    bool consumed = false;
    // KEYDOWN: client first (focused widget), then the widget's on_keydown.
    if (keycode != 0) {
      uint32_t fw = s->_focused_widget;
      if (fw != 0 && s->_widgets.exists(fw)) {
        auto& wd = s->_widgets[fw];
        if (wd.emit_events) {
          neui_event_t ev = {};
          ev.type     = NEUI_EVENT_KEYDOWN;
          ev.data.key = { { wd.widget_id }, keycode, mods };
          consumed = s->dispatch_event(&ev);
        }
        if (!consumed)
          consumed = s->handle_input_key(NEUI_EVENT_KEYDOWN, keycode, mods);
      }
    }

    // KEYCHAR for the typed character (none while the primary modifier is
    // held: those are shortcuts, as on Linux / macOS).
    const uint32_t cp = neui_detail::web_key_char(key.c_str(), mods);
    if (cp == 0) return consumed;
    uint32_t fw = s->_focused_widget;
    if (fw == 0 || !s->_widgets.exists(fw)) return consumed;
    auto& wd = s->_widgets[fw];
    bool char_consumed = false;
    if (wd.emit_events) {
      neui_event_t ev = {};
      ev.type     = NEUI_EVENT_KEYCHAR;
      ev.data.key = { { wd.widget_id }, cp, mods };
      char_consumed = s->dispatch_event(&ev);
    }
    if (!char_consumed) s->handle_input_key(NEUI_EVENT_KEYCHAR, cp, mods);
    return true;   // a focused widget received the character
  }

  void key_up(WebFrame* f, const std::string& code, const std::string& key, uint32_t web_mods)
  {
    Session* s = f->session;
    const uint32_t keycode = neui_detail::web_key_to_neui(code.c_str(), key.c_str());
    if (keycode == 0) return;
    const uint32_t mods = neui_detail::web_modifiers_to_neui(web_mods, g_apple);
    uint32_t fw = s->_focused_widget;
    if (fw == 0 || !s->_widgets.exists(fw)) return;
    auto& wd = s->_widgets[fw];
    if (!wd.emit_events) return;
    neui_event_t ev = {};
    ev.type     = NEUI_EVENT_KEYUP;
    ev.data.key = { { wd.widget_id }, keycode, mods };
    s->dispatch_event(&ev);
  }

  // ---- Queue -----------------------------------------------------------------

  // The browser reports the CSS size (= logical px) and, where supported, the
  // exact device-pixel size of the canvas. A device-pixel-ratio change (zoom,
  // another monitor) arrives the same way.
  void handle_resize(WebFrame* f, double css_w, double css_h, double dev_w, double dev_h)
  {
    Session* s = f->session;
    auto* wd = s ? s->get_widget(f->widget_index) : nullptr;
    if (!wd) return;
    const float dpr = static_cast<float>(web_dpr());
    uint32_t pw = dev_w > 0.0 ? static_cast<uint32_t>(dev_w)
                              : static_cast<uint32_t>(std::lround(css_w * dpr));
    uint32_t ph = dev_h > 0.0 ? static_cast<uint32_t>(dev_h)
                              : static_cast<uint32_t>(std::lround(css_h * dpr));
    if (pw == 0) pw = 1;
    if (ph == 0) ph = 1;
    if (dpr != f->dpr) {
      f->dpr = dpr;
      s->on_dpi_changed(f->widget_index, dpi_for(dpr));
      if (wd->render_ctx) neui_canvas2d_backend::set_scale(wd->render_ctx, dpr);
    }
    s->resize_render_ctx(f->widget_index, pw, ph);
    const int wlog = static_cast<int>(std::lround(css_w));
    const int hlog = static_cast<int>(std::lround(css_h));
    if (wd->width != wlog || wd->height != hlog) {
      wd->width  = wlog;
      wd->height = hlog;
      // The in-frame menubar band reserves the top of the client area;
      // report the height below it (as Linux and Win32 do).
      neui_event_t re = {};
      re.type               = NEUI_EVENT_RESIZE;
      re.data.resize.widget = { wd->widget_id };
      re.data.resize.width  = wlog;
      re.data.resize.height = hlog - s->frame_top_inset(f->widget_index);
      s->dispatch_event(&re);
    }
    if (auto* g = find_frame(f->id)) g->needs_paint = true;
  }

  void drain_queue()
  {
    while (!g_queue.empty()) {
      WebEvent e = std::move(g_queue.front());
      g_queue.pop_front();

      if (e.type == EV_SCHEME) {   // page-wide, no frame
        neui_detail::theme_apply_web_scheme(static_cast<int>(e.a));
        neui_detail::broadcast_theme_change();
        continue;
      }

      // Events that carry a default action get their verdict here, also when
      // the frame is gone or blocked (then: leave it to the browser).
      int verdict_id = 0;
      if (e.type == EV_CONTEXT) verdict_id = static_cast<int>(e.c);
      else if (e.type == EV_WHEEL) verdict_id = static_cast<int>(e.f);
      else if (e.type == EV_KEYDOWN) verdict_id = static_cast<int>(e.a);
      bool consumed = false;

      WebFrame* f = find_frame(e.frame);
      Session* s = f ? f->session : nullptr;
      const bool blocked = f && f->input_disabled;
      if (f && s) {
        const float x = static_cast<float>(e.a), y = static_cast<float>(e.b);
        switch (e.type) {
          case EV_RESIZE:
            handle_resize(f, e.a, e.b, e.c, e.d);
            break;
          case EV_FOCUS:
            s->_os_focused = true;
            s->frame_activated(f->widget_index);
            f->needs_paint = true;
            break;
          case EV_BLUR:
            s->_os_focused = false;
            s->close_menubar_menu();
            f->needs_paint = true;
            break;
          case EV_PDOWN:
            if (!blocked)
              pointer_down(f, x, y, static_cast<int>(e.c), static_cast<uint32_t>(e.d),
                           static_cast<uint32_t>(e.e), e.f);
            break;
          case EV_PUP:
            if (!blocked) pointer_up(f, x, y, static_cast<int>(e.c));
            break;
          case EV_PMOVE:
            if (!blocked)
              pointer_move(f, x, y, static_cast<uint32_t>(e.d), static_cast<uint32_t>(e.e));
            break;
          case EV_PLEAVE:
            pointer_leave(f);
            break;
          case EV_CONTEXT:
            // neui handles the right button itself (RBUTTON_DOWN / UP, the
            // knob's popup); the browser's menu only where neui has nothing.
            consumed = blocked || s->_popup_active || s->widget_at(x, y, f->widget_index) != 0;
            break;
          case EV_WHEEL:
            consumed = blocked ? true
                               : handle_wheel(f, x, y, e.c, e.d, static_cast<int>(e.e));
            break;
          case EV_KEYDOWN:
            if (!blocked) consumed = key_down(f, e.code, e.key, static_cast<uint32_t>(e.b));
            break;
          case EV_KEYUP:
            if (!blocked) key_up(f, e.code, e.key, static_cast<uint32_t>(e.b));
            break;
          default: break;
        }
      }
      if (verdict_id) web_verdict(verdict_id, consumed ? 1 : 0);
    }
  }

  // ---- Metrics seam ----------------------------------------------------------

  // NEUI_API_METRICS text width through the browser's own shaping (the
  // desktop default is an average-glyph estimate).
  int metrics_measure_web(neui_session_t /*session*/, const char* text, const char* family,
                          float size_px, int weight)
  {
    if (!text || !*text) return 0;
    if (size_px <= 0.0f) size_px = neui_detail::METRIC_BASE_BODY_FONT * neui_detail::painted_ui_scale();
    static neui_render_ctx_t ctx = nullptr;
    auto* be = platform_get_backend();
    if (!ctx) ctx = be->create_offscreen_context(1, 1, 1.0f);
    if (!ctx) return neui_detail::metrics_measure_text_default(neui_session_t{}, text, family, size_px, weight);
    be->push_font(ctx, family, weight);
    const float w = be->measure_text(ctx, text, -1, size_px);
    be->pop_font(ctx);
    return static_cast<int>(std::ceil(w));
  }

  // ---- Frame creation ---------------------------------------------------------

  void create_frame(Session* session, uint32_t widget_index, WidgetData& wd,
                    int kind, bool is_appwindow)
  {
    platform_init();
    auto* f = new WebFrame();
    f->id = g_next_frame_id++;
    f->session = session;
    f->widget_index = widget_index;
    f->is_appwindow = is_appwindow;
    f->is_dialog = (kind == 2);
    f->dpr = static_cast<float>(web_dpr());
    g_frames[f->id] = f;

    const int w = wd.width  > 0 ? wd.width  : 1;
    const int h = wd.height > 0 ? wd.height : 1;
    web_frame_create(f->id, kind, wd.x, wd.y, w, h);
    if (!wd.text.empty()) web_frame_title(f->id, wd.text.c_str());

    wd.native_handle = f;
    wd.dpi = dpi_for(f->dpr);

    auto* backend = platform_get_backend();
    if (backend) {
      const auto pw = static_cast<uint32_t>(std::lround(static_cast<double>(w) * f->dpr));
      const auto ph = static_cast<uint32_t>(std::lround(static_cast<double>(h) * f->dpr));
      wd.render_ctx = backend->create_context(reinterpret_cast<void*>(static_cast<uintptr_t>(f->id)),
                                              pw ? pw : 1, ph ? ph : 1);
      if (wd.render_ctx) {
        backend->update_dpi(wd.render_ctx, wd.dpi);
        neui_canvas2d_backend::set_scale(wd.render_ctx, f->dpr);
      }
    }

    // Pre-show attribute-driven size constraints.
    if (wd.attrs) {
      int min_w = wd.attrs->get_int(NEUI_ATTR_MIN_WIDTH,  0);
      int min_h = wd.attrs->get_int(NEUI_ATTR_MIN_HEIGHT, 0);
      int max_w = wd.attrs->get_int(NEUI_ATTR_MAX_WIDTH,  0);
      int max_h = wd.attrs->get_int(NEUI_ATTR_MAX_HEIGHT, 0);
      if (min_w || min_h || max_w || max_h)
        platform_apply_size_constraints(f, min_w, min_h, max_w, max_h);
    }

    f->needs_paint = true;
    if (is_appwindow) { ++g_appwindow_count; f->counted = true; }
  }

} // namespace

// Records from the DOM listeners (Module.neuiWeb.push). Consecutive pointer
// moves on one frame collapse into the latest: a drag delivers one move per
// loop turn instead of one per input sample.
extern "C" EMSCRIPTEN_KEEPALIVE void neui_web_push(int type, int frame, double a, double b,
                                                   double c, double d, double e, double f)
{
  if (type == EV_PMOVE && !g_queue.empty()) {
    WebEvent& last = g_queue.back();
    if (last.type == EV_PMOVE && last.frame == frame) {
      last.a = a; last.b = b; last.c = c; last.d = d; last.e = e; last.f = f;
      return;
    }
  }
  g_queue.push_back({ type, frame, a, b, c, d, e, f, std::string(), std::string() });
}

// Key records carry the KeyboardEvent's code and key strings, which the
// listener writes into this buffer just before neui_web_push_key.
static char g_key_buf[128];
extern "C" EMSCRIPTEN_KEEPALIVE char* neui_web_key_buf() { return g_key_buf; }

extern "C" EMSCRIPTEN_KEEPALIVE void neui_web_push_key(int type, int frame, int ev_id, int mods)
{
  g_key_buf[63] = g_key_buf[127] = '\0';
  g_queue.push_back({ type, frame, static_cast<double>(ev_id), static_cast<double>(mods),
                      0, 0, 0, 0, std::string(g_key_buf), std::string(g_key_buf + 64) });
}

// ===========================================================================
// Platform seam implementation.

  void platform_init()
  {
    static bool inited = false;
    if (inited) return;
    inited = true;
    web_setup();
    g_apple = web_is_apple() != 0;
    // Debug switches (NEUI_DEBUG_FULL_REPAINT, NEUI_DEBUG_DAMAGE, ...) from the
    // page URL, before the host first reads them.
    static char query[2048];
    web_neui_query(query, static_cast<int>(sizeof query));
    for (char* line = query; *line;) {
      char* end = std::strchr(line, '\n');
      if (end) *end = '\0';
      if (char* eq = std::strchr(line, '=')) { *eq = '\0'; setenv(line, eq + 1, 1); }
      if (!end) break;
      line = end + 1;
    }
    // Before any Session exists, so the first frame already matches the page.
    neui_detail::theme_apply_web_scheme(web_scheme());
    neui_detail::metrics_add_measure_seam(&metrics_measure_web);
  }

  neui_render_backend_t* platform_get_backend()
  {
    return neui_canvas2d_backend::get_backend();
  }

  void platform_create_appwindow(Session* session, uint32_t widget_index, WidgetData& wd)
  {
    create_frame(session, widget_index, wd, 0, /*is_appwindow*/true);
  }

  void platform_create_plugwindow(Session* session, uint32_t widget_index, WidgetData& wd)
  {
    create_frame(session, widget_index, wd, 1, /*is_appwindow*/false);
  }

  void platform_create_dialog(Session* session, uint32_t widget_index, WidgetData& wd,
                              void* /*owner_native*/)
  {
    create_frame(session, widget_index, wd, 2, /*is_appwindow*/false);
  }

  void platform_destroy_window(WidgetData& wd)
  {
    if (!wd.native_handle) return;
    teardown_window(static_cast<WebFrame*>(wd.native_handle), &wd);
  }

  void platform_show_window(void* native_handle)
  {
    if (!native_handle) return;
    auto* f = static_cast<WebFrame*>(native_handle);
    web_frame_show(f->id, 1);
    f->needs_paint = true;
  }

  void platform_hide_window(void* native_handle)
  {
    if (!native_handle) return;
    web_frame_show(static_cast<WebFrame*>(native_handle)->id, 0);
  }

  void platform_set_window_enabled(void* native_handle, bool enabled)
  {
    if (!native_handle) return;
    static_cast<WebFrame*>(native_handle)->input_disabled = !enabled;
  }

  void platform_activate_window(void* native_handle)
  {
    if (!native_handle) return;
    web_frame_focus(static_cast<WebFrame*>(native_handle)->id);
  }

  void platform_set_window_title(void* native_handle, const char* text)
  {
    if (!native_handle) return;
    web_frame_title(static_cast<WebFrame*>(native_handle)->id, text ? text : "");
  }

  // Logical px are CSS px. The canvas' CSS size changes; the ResizeObserver
  // then reports the new size (and RESIZE fires) like any other resize. A
  // frame that fills its page container keeps the container's size.
  void platform_set_window_pos(void* native_handle, int x, int y, int w, int h, uint32_t /*dpi*/)
  {
    if (!native_handle) return;
    auto* f = static_cast<WebFrame*>(native_handle);
    web_frame_geometry(f->id, x, y, w > 0 ? w : 1, h > 0 ? h : 1, f->is_dialog ? 1 : 0);
  }

  void platform_post_close(void* native_handle)
  {
    if (!native_handle) return;
    handle_close(static_cast<WebFrame*>(native_handle));   // synchronous, as on Linux
  }

  float platform_get_scale_factor(void* native_handle)
  {
    if (!native_handle) return 1.0f;
    const float s = static_cast<WebFrame*>(native_handle)->dpr;
    return s > 0.0f ? s : 1.0f;
  }

  void platform_invalidate(void* native_handle)
  {
    if (!native_handle) return;
    static_cast<WebFrame*>(native_handle)->needs_paint = true;
  }

  void platform_invalidate_rect(void* native_handle, int x, int y, int w, int h)
  {
    if (!native_handle) return;
    auto* f = static_cast<WebFrame*>(native_handle);
    if (f->needs_paint) return;   // a full repaint is already pending
    f->damage.add({ x, y, w, h });
  }

  // One turn of the loop: drain, then wait - for the next animation frame
  // when a paint is pending (painting there, in step with the display), else
  // for a record or the heartbeat. cap_ms bounds the wait (-1 = none).
  static void loop_turn(int cap_ms)
  {
    drain_queue();
    service_timers();
    if (any_paint_pending()) {
      if (web_wait(wait_timeout_ms(cap_ms), 1) == 1) {
        drain_queue();
        flush_pending_paints();
      }
    } else {
      web_wait(wait_timeout_ms(cap_ms), 0);
    }
  }

  bool platform_run()
  {
    platform_init();
    g_quit = false;
    while (!g_quit && g_appwindow_count > 0) loop_turn(-1);
    return true;
  }

  // Non-blocking: drain and paint now. Never suspends, so it is safe from a
  // client's own requestAnimationFrame callback.
  bool platform_pump_once()
  {
    drain_queue();
    service_timers();
    flush_pending_paints();
    return !g_quit;
  }

  bool platform_run_modal_until(bool* keep_running)
  {
    while (keep_running && *keep_running && !g_quit) loop_turn(16);
    return !g_quit;
  }

  // DAW-embedding seams - mounting into a page element comes in Stage 7.
  void platform_set_embed_parent(Session* session, uint32_t widget_index, void* native_parent)
  {
    if (!session) return;
    auto* wd = session->get_widget(widget_index);
    if (wd) wd->embed_parent = reinterpret_cast<uintptr_t>(native_parent);
  }
  int  platform_embed_event_fd(void* /*native_handle*/) { return -1; }
  void platform_embed_pump_and_tick(void* /*native_handle*/) {}

  // ---- Menu bar ---------------------------------------------------------------
  // The browser has no menu bar, so the host draws it inside the frame (the
  // Linux in-frame band: Session::paint_menubar + the handle_menubar_* input
  // path). As on Linux, platform_menubar_create must return a non-null
  // sentinel (widgets.cpp::t_add bails on null); everything else is a no-op.
  bool  platform_menubar_in_frame()                                                     { return true; }
  int   platform_frame_extra_top_inset(void* /*nh*/, bool /*has_menubar*/)              { return 0; }
  void* platform_menubar_create(uint32_t /*widget_id*/)                                 { return reinterpret_cast<void*>(0x1); }
  void  platform_menubar_destroy(void* /*hmenu*/)                                       {}
  void  platform_menubar_attach(void* /*frame*/, void* /*hmenu*/)                       {}
  void  platform_menubar_refresh(void* /*frame*/)                                       {}
  void* platform_menubar_add_popup(void* /*hmenu*/, const char* /*text*/)               { return reinterpret_cast<void*>(0x1); }
  void  platform_menubar_add_item(void* /*hmenu*/, uint32_t /*cmd*/, const char* /*t*/) {}
  void  platform_menubar_add_separator(void* /*hmenu*/, uint32_t /*cmd*/)               {}
  void  platform_menubar_remove_popup(void* /*hmenu*/, void* /*sub*/)                   {}
  void  platform_menubar_remove_item(void* /*hmenu*/, uint32_t /*cmd*/)                 {}
  void  platform_menubar_enable_item(void* /*hmenu*/, uint32_t /*cmd*/, bool /*en*/)    {}
  void  platform_menubar_enable_popup(void* /*hmenu*/, void* /*sub*/, bool /*en*/)      {}
  void  platform_menubar_check_item(void* /*hmenu*/, uint32_t /*cmd*/, bool /*chk*/)    {}
  void  platform_menubar_set_item_text(void* /*hmenu*/, uint32_t /*cmd*/, const char* /*t*/) {}
  void  platform_menubar_set_item_shortcut(void* /*hmenu*/, uint32_t /*cmd*/,
                                            uint32_t /*mods*/, uint32_t /*key*/)        {}

  // The favicon is the page's business.
  void platform_set_window_icon(WidgetData& /*wd*/, const char* /*path*/) {}

  void platform_apply_size_constraints(void* native_handle, int min_w, int min_h,
                                        int max_w, int max_h)
  {
    if (!native_handle) return;
    web_frame_constraints(static_cast<WebFrame*>(native_handle)->id, min_w, min_h, max_w, max_h);
  }

  // ---- Images: vendored stb_image over the in-memory file system -------------
  uint8_t* platform_load_image(const char* path, uint32_t* width_out, uint32_t* height_out)
  {
    return neui_detail::load_image_bgra8_stb(path, width_out, height_out);
  }
  uint8_t* platform_load_image_bytes(const uint8_t* data, size_t len,
                                      uint32_t* width_out, uint32_t* height_out)
  {
    return neui_detail::load_image_bgra8_stb_memory(data, len, width_out, height_out);
  }
  void platform_free_image(uint8_t* pixels)
  {
    neui_detail::free_image_bgra8_stb(pixels);
  }

  // System clipboard - Stage 5.
  bool platform_clipboard_set_text(const char* /*utf8*/, uint32_t /*length*/) { return false; }
  int  platform_clipboard_get_text(char* /*buf*/, int /*buflen*/)             { return 0;     }
  bool platform_clipboard_has_text()                                          { return false; }
  void platform_clipboard_set_primary(const char* /*u*/, uint32_t /*l*/)      {}
  int  platform_clipboard_get_primary(char* /*b*/, int /*n*/)                 { return 0; }
  bool platform_clipboard_write_item(const neui_detail::DataItem& /*item*/)   { return false; }
  bool platform_clipboard_read_item(neui_detail::DataItem& /*item*/)          { return false; }

  // Drag & drop - Stage 6.
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

  void platform_set_cursor(int kind)
  {
    static int current = NEUI_CURSOR_DEFAULT;
    if (kind == current) return;
    current = kind;
    web_set_cursor(kind == NEUI_CURSOR_EW_RESIZE ? "ew-resize" : "");
  }

  void platform_start_toast_animation(void* native_handle)
  {
    if (!native_handle) return;
    static_cast<WebFrame*>(native_handle)->toast_anim = true;
    arm_heartbeat();
  }
  void platform_stop_toast_animation(void* native_handle)
  {
    if (!native_handle) return;
    static_cast<WebFrame*>(native_handle)->toast_anim = false;
  }

  void platform_set_animation_timer(void* native_handle, uint32_t interval_ms)
  {
    if (!native_handle) return;
    auto* f = static_cast<WebFrame*>(native_handle);
    f->clock_anim = interval_ms != 0;
    if (f->clock_anim) arm_heartbeat();
  }

  bool platform_reduced_motion() { return web_reduced_motion() != 0; }

  uint64_t platform_now_ms()
  {
    return static_cast<uint64_t>(emscripten_get_now());
  }

  // Message boxes - Stage 4 (neui-drawn, as on Linux). 0 = failure.
  int platform_message_box(void* /*native_handle*/, const char* /*text*/,
                           const char* /*caption*/, uint32_t /*flags*/)       { return 0; }

} // namespace xpl_host
