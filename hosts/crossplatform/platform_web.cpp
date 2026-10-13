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
// Stage 2 scope: frames, the loop, paint + partial repaint, resize, device
// pixel ratio, focus, timers, images, cursor. Input arrives in Stage 3; the
// remaining seams are still the null layer's no-ops.

#include "host.h"
#include "platform.h"
#include <neui/neui.h>

#include <emscripten.h>

#include <cmath>
#include <deque>
#include <unordered_map>
#include <vector>

// Full DataItem definition for the item-based clipboard stubs below.
#include "../shared/clipboard_item.h"
#include "../shared/damage.h"
#include "../../backends/canvas2d/canvas2d_backend.h"

// This TU emits the single stb_image implementation for the web host.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_DEPRECATED
#include "../shared/image_loader_stb.h"

// ---------------------------------------------------------------------------
// JavaScript half: Module.neuiWeb (frames, the wake hook) and
// Module.neuiCanvases (canvas by frame id, read by the Canvas 2D backend).

EM_JS_DEPS(neui_web, "$UTF8ToString");

EM_JS(void, web_setup, (), {
  if (Module.neuiWeb) return;
  var W = Module.neuiWeb = { frames: {}, wake: null };
  Module.neuiCanvases = Module.neuiCanvases || {};
  // Queue a record for the loop and wake it if it is waiting.
  W.push = function(type, frame, a, b, c, d) {
    Module._neui_web_push(type, frame, a || 0, b || 0, c || 0, d || 0);
    if (W.wake) { var w = W.wake; W.wake = null; w(2); }
  };
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

EM_JS(double, web_dpr, (), { return (typeof devicePixelRatio === 'number' && devicePixelRatio > 0) ? devicePixelRatio : 1; });

EM_JS(int, web_reduced_motion, (), {
  return (typeof matchMedia === 'function' && matchMedia('(prefers-reduced-motion: reduce)').matches) ? 1 : 0;
});

// kind: 0 APPWINDOW, 1 PLUGWINDOW, 2 DIALOG. An APPWINDOW mounts into
// #neui-root when the page has one, else into <body>; a dialog floats above
// the page (viewport coordinates).
EM_JS(void, web_frame_create, (int id, int kind, int x, int y, int w, int h), {
  var W = Module.neuiWeb;
  var wrap = document.createElement('div');
  wrap.className = 'neui-frame';
  wrap.style.cssText = 'display:none;line-height:0;';
  if (kind === 2) {
    wrap.style.position = 'fixed';
    wrap.style.left = x + 'px'; wrap.style.top = y + 'px';
    wrap.style.zIndex = '1000';
  }
  var cv = document.createElement('canvas');
  cv.tabIndex = 0;
  cv.style.cssText = 'display:block;outline:none;width:' + w + 'px;height:' + h + 'px;';
  wrap.appendChild(cv);
  var host = (kind === 0 && document.getElementById('neui-root')) || document.body;
  host.appendChild(wrap);
  Module.neuiCanvases[id] = cv;
  var f = W.frames[id] = { wrap: wrap, cv: cv, ro: null, body: host === document.body };
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

// Size in CSS px; a dialog also moves (x / y are viewport coordinates).
EM_JS(void, web_frame_geometry, (int id, int x, int y, int w, int h, int move), {
  var f = Module.neuiWeb.frames[id]; if (!f) return;
  f.cv.style.width = w + 'px'; f.cv.style.height = h + 'px';
  if (move) { f.wrap.style.left = x + 'px'; f.wrap.style.top = y + 'px'; }
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
    // Set while a modal child is up (the input path swallows its events).
    bool      input_disabled = false;
  };

  std::unordered_map<int, WebFrame*> g_frames;
  int      g_next_frame_id  = 1;
  int      g_appwindow_count = 0;
  bool     g_quit           = false;

  // DOM -> loop records. type: 1 resize (a, b = CSS size; c, d = device px,
  // 0 when the browser does not report them), 2 focus, 3 blur.
  enum : int { EV_RESIZE = 1, EV_FOCUS = 2, EV_BLUR = 3 };
  struct WebEvent { int type; int frame; double a, b, c, d; };
  std::deque<WebEvent> g_queue;

  // The animation heartbeat: one 16 ms tick shared by every frame while any
  // of them animates (as the Linux timerfd), 0 = idle.
  double g_next_tick_ms = 0.0;

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
    for (auto& kv : g_frames)
      if (kv.second->toast_anim || kv.second->clock_anim) return true;
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

  void tick_animations()
  {
    std::vector<int> ids;
    for (auto& kv : g_frames) ids.push_back(kv.first);
    for (int id : ids) {
      WebFrame* f = find_frame(id);
      if (!f) continue;
      if (f->toast_anim) f->needs_paint = true;
      if (f->clock_anim && f->session)
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
      WebEvent e = g_queue.front();
      g_queue.pop_front();
      WebFrame* f = find_frame(e.frame);
      if (!f) continue;   // the frame went away after the record was queued
      Session* s = f->session;
      switch (e.type) {
        case EV_RESIZE:
          handle_resize(f, e.a, e.b, e.c, e.d);
          break;
        case EV_FOCUS:
          if (s) { s->_os_focused = true; s->frame_activated(f->widget_index); }
          f->needs_paint = true;
          break;
        case EV_BLUR:
          if (s) { s->_os_focused = false; s->close_menubar_menu(); }
          f->needs_paint = true;
          break;
        default: break;
      }
    }
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
    f->needs_paint = true;
    if (is_appwindow) { ++g_appwindow_count; f->counted = true; }
  }

} // namespace

// Records from the DOM listeners (Module.neuiWeb.push).
extern "C" EMSCRIPTEN_KEEPALIVE void neui_web_push(int type, int frame,
                                                   double a, double b, double c, double d)
{
  g_queue.push_back({ type, frame, a, b, c, d });
}

// ===========================================================================
// Platform seam implementation.

  void platform_init()
  {
    static bool inited = false;
    if (inited) return;
    inited = true;
    web_setup();
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
  // then reports the new size (and RESIZE fires) like any other resize.
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

  // The favicon is the page's business; constraints arrive with container
  // sizing in Stage 3.
  void platform_set_window_icon(WidgetData& /*wd*/, const char* /*path*/) {}
  void platform_apply_size_constraints(void* /*nh*/, int /*minw*/, int /*minh*/,
                                        int /*maxw*/, int /*maxh*/) {}

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
