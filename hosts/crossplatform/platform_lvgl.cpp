// LVGL platform layer for the crossplatform host (prototype - see
// plans/lvgl-host-approach-c.md). Windows-only for now: each neui frame is an
// LVGL display created through LVGL's native Windows driver (LV_USE_WINDOWS).
//
// Threading model (dictated by the driver): every LVGL display window lives
// on its own thread with its own message pump, and every WndProc entry takes
// lv_lock(). neui's Session is single-threaded, so this layer
//   - subclasses the driver HWND and forwards raw input messages into a
//     mutex-guarded queue (display thread -> main thread), and
//   - drains that queue on the main thread, interleaved with
//     lv_timer_handler(), translating messages into the same Session calls
//     platform_win32.cpp makes (hit-test -> hover -> focus -> dispatch).
//
// Rendering (Milestone 1 - Approach A baseline): the frame's LVGL screen
// object carries a LV_EVENT_DRAW_MAIN callback that binds the neui-backend-
// lvgl context to the event layer and runs Session::paint_frame - i.e. neui's
// full redraw-the-world walk into one LVGL surface. platform_invalidate maps
// to lv_obj_invalidate(screen). The Milestone 2 retained per-widget layer
// replaces exactly this part.
//
// Coordinate model: LVGL application-mode windows have client area ==
// display resolution == physical pixels, so neui logical px, LVGL px and
// physical px are all 1:1 (wd.dpi stays 96, get_scale_factor 1.0). No DPI
// scaling in the prototype - matching a fixed-pixel embedded panel.
//
// Prototype limitations (deliberate, per plan): clipboard / DnD / IME / menus
// / message boxes are no-op stubs; closing a frame hides its window instead
// of destroying it (the driver's display watchdog exit(0)s the process when
// the last display dies mid-loop); frames are not repositionable after
// creation.

#include <algorithm>
#include <string>
#include <deque>
#include <functional>
#include <vector>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "host.h"
#include "platform.h"
#include "../../backends/lvgl/lvgl_backend.h"

#include <lvgl/lvgl.h>
#include <lvgl/drivers/windows/lv_windows_display.h>
#include <lvgl/drivers/windows/lv_windows_input.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>

// platform_load_image uses the shared stb decoder (the WIC loader the native
// Windows platform layer uses needs COM, which this host never initialises).
// This TU emits the single stb_image implementation for the LVGL build.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#include "../shared/image_loader_stb.h"

namespace xpl_host
{
  // -------------------------------------------------------------------------
  // Globals

  struct WindowData;

  // Retained mode (Milestone 2 - Option C): one passive lv_obj per neui
  // widget. MirrorRef is the draw-event user_data (owned by the entry);
  // MirrorEntry.body is the clipped content container for SECTION / TABVIEW
  // style widgets whose children are positioned body-relative.
  struct MirrorRef {
    WindowData* w;
    uint32_t    idx;
  };
  struct MirrorEntry {
    lv_obj_t* obj    = nullptr;
    lv_obj_t* body   = nullptr;
    lv_obj_t* parent = nullptr;   // parent obj at last sync (slot-reuse guard)
    std::unique_ptr<MirrorRef> ref;
    // Body rect + scroll used at the last sync. SECTION / TABVIEW layouts are
    // computed during PAINT (zeros before the first one), so the draw
    // callback compares and re-marks the tree dirty when they move.
    int32_t synced_body_x = 0, synced_body_y = 0;
    int32_t synced_body_w = 0, synced_body_h = 0;
    int32_t synced_scroll_x = 0, synced_scroll_y = 0;
  };

  struct WindowData {
    Session*      session      = nullptr;
    uint32_t      widget_index = 0;
    lv_display_t* display      = nullptr;
    HWND          hwnd         = nullptr;
    lv_obj_t*     screen       = nullptr;
    WNDPROC       prev_proc    = nullptr;
    lv_timer_t*   toast_timer  = nullptr;
    bool          closed       = false;
    // Set instead of calling lv_obj_invalidate when an invalidation arrives
    // from inside an LVGL draw dispatch (client PREUPDATE / PAINT handlers
    // writing attrs -> WidgetData::repaint) - LVGL forbids invalidating
    // while rendering. Flushed right after lv_timer_handler returns.
    bool          pending_invalidate = false;
    // Retained-mode state (main thread only).
    std::unordered_map<uint32_t, MirrorEntry> mirrors;
    bool                  tree_dirty = false;
    std::vector<uint32_t> pending_widget_invals;
    // Input-drain state (main thread only).
    bool          tracking_mouse    = false;   // display thread only
    uint16_t      pending_surrogate = 0;
    uint32_t      last_click_ms     = 0;       // double-click synthesis
    int           last_click_x      = -10000;
    int           last_click_y      = -10000;
  };

  struct InputMsg {
    WindowData* w  = nullptr;
    UINT        msg = 0;
    WPARAM      wp  = 0;
    LPARAM      lp  = 0;
    uint32_t    mods = 0;   // bit0 shift, bit1 ctrl, bit2 alt (platform_win32 layout)
  };

  static CRITICAL_SECTION      g_queue_lock;
  static std::deque<InputMsg>  g_queue;
  static HANDLE                g_wake            = nullptr;
  static bool                  g_inited          = false;
  static bool                  g_quit            = false;
  static int                   g_appwindow_count = 0;
  static std::vector<WindowData*> g_windows;

  // Milestone 2 runtime switch: retained per-widget mirrors (Option C) vs the
  // Milestone 1 whole-frame baseline (Approach A). Defaults ON; set
  // NEUI_LVGL_RETAINED=0 to measure the baseline with the same binary.
  static bool g_retained = true;

  static void read_retained_env()
  {
    char buf[8] = {};
    size_t n = 0;
    if (getenv_s(&n, buf, sizeof(buf), "NEUI_LVGL_RETAINED") == 0 && n > 0)
      g_retained = !(buf[0] == '0');
  }

  // Non-zero while running inside an LVGL callback (draw event / lv timer),
  // where lv_timer_handler already holds the global LVGL lock. The lock
  // guard below must not re-lock there - the OSAL mutex is not recursive.
  static int t_inside_lv = 0;

  struct LvLockGuard {
    bool locked;
    LvLockGuard() : locked(t_inside_lv == 0) { if (locked) lv_lock(); }
    ~LvLockGuard() { if (locked) lv_unlock(); }
  };

  // Same policy exposed to the render backend, which performs LVGL allocations
  // of its own outside any draw dispatch (Tiny TTF instance creation and glyph
  // cache fills from measure_text on the host's non-painting sizing paths; the
  // vector path / dsc deletes in destroy_context). Those mutate LVGL's global
  // allocator + cache state, which a display thread can be inside under
  // lv_lock. Counting into t_inside_lv keeps the guard re-entrant, since the
  // OSAL mutex is not, and makes nested backend calls see the lock as held.
  static void backend_lv_lock()
  {
    if (t_inside_lv == 0) lv_lock();
    ++t_inside_lv;
  }

  static void backend_lv_unlock()
  {
    if (--t_inside_lv == 0) lv_unlock();
  }

  // Deferred main-thread USER32 work.
  //
  // lv_timer_handler holds the global LVGL lock for the whole refresh, and the
  // draw callbacks dispatch WIDGET_PREUPDATE / WIDGET_PAINT into client code
  // that may legally call widgets->set_text / set_pos, or show / close a
  // dialog. Those land on SetWindowTextW / SetWindowPos / EnableWindow /
  // SetForegroundWindow, which BLOCK until the target window's own thread
  // processes the sent message - and that thread's WndProc takes lv_lock on
  // entry. Calling them while holding the lock is a hard deadlock, so queue
  // them and run them from the main loop once the lock is released.
  static std::vector<std::function<void()>> g_deferred_calls;

  static bool defer_if_locked(std::function<void()> fn)
  {
    if (t_inside_lv == 0) return false;
    g_deferred_calls.push_back(std::move(fn));
    SetEvent(g_wake);   // make sure the loop wakes to run it
    return true;
  }

  static void run_deferred_calls()
  {
    while (!g_deferred_calls.empty()) {
      std::vector<std::function<void()>> batch;
      batch.swap(g_deferred_calls);
      for (auto& fn : batch) fn();
    }
  }

  // -------------------------------------------------------------------------
  // Display-thread side: subclass WndProc -> input queue

  static void enqueue(WindowData* w, UINT msg, WPARAM wp, LPARAM lp)
  {
    uint32_t mods = 0;
    if (GetKeyState(VK_SHIFT)   & 0x8000) mods |= 1;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= 2;
    if (GetKeyState(VK_MENU)    & 0x8000) mods |= 4;
    {
      EnterCriticalSection(&g_queue_lock);
      g_queue.push_back(InputMsg{ w, msg, wp, lp, mods });
      LeaveCriticalSection(&g_queue_lock);
    }
    SetEvent(g_wake);
  }

  static LRESULT CALLBACK subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
  {
    auto* w = reinterpret_cast<WindowData*>(GetPropW(hwnd, L"neui.lvgl.wdata"));
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
      case WM_MOUSEMOVE:
        if (!w->tracking_mouse) {
          TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
          TrackMouseEvent(&tme);
          w->tracking_mouse = true;
        }
        enqueue(w, msg, wp, lp);
        break;

      case WM_MOUSELEAVE:
        w->tracking_mouse = false;
        enqueue(w, msg, wp, lp);
        break;

      case WM_LBUTTONDOWN:
        SetCapture(hwnd);  // keep move messages during out-of-window drags
        enqueue(w, msg, wp, lp);
        break;

      case WM_LBUTTONUP:
        ReleaseCapture();
        enqueue(w, msg, wp, lp);
        break;

      case WM_RBUTTONDOWN:
      case WM_RBUTTONUP:
      case WM_MOUSEWHEEL:
      case WM_MOUSEHWHEEL:
      case WM_KEYDOWN:
      case WM_KEYUP:
      case WM_CHAR:
      case WM_SIZE:
      case WM_SETFOCUS:
      case WM_KILLFOCUS:
        enqueue(w, msg, wp, lp);
        break;

      case WM_CLOSE:
        // Consume: the driver's DefWindowProc would DestroyWindow, delete the
        // display, and the driver watchdog would exit(0) the process. neui
        // decides on the main thread instead (APP_QUIT event).
        enqueue(w, msg, wp, lp);
        return 0;

      default:
        break;
    }
    return CallWindowProcW(w->prev_proc, hwnd, msg, wp, lp);
  }

  // -------------------------------------------------------------------------
  // Main-thread side: the paint bridge (Milestone 1: whole frame -> screen)

  static void screen_draw_cb(lv_event_t* e)
  {
    auto* w = static_cast<WindowData*>(lv_event_get_user_data(e));
    if (!w || !w->session) return;
    auto* wd = w->session->get_widget(w->widget_index);
    if (!wd || !wd->render_ctx) return;
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer) return;

    ++t_inside_lv;
    neui_lvgl_backend::bind_layer(wd->render_ctx, layer, 0, 0);
    w->session->paint_frame(wd->render_ctx, w->widget_index);
    neui_lvgl_backend::unbind_layer(wd->render_ctx);
    --t_inside_lv;
  }

  // -------------------------------------------------------------------------
  // Milestone 2 (Option C): retained lv_obj mirror per widget.
  //
  // The screen object paints the frame background (DRAW_MAIN, below all
  // mirrors) and the overlays (DRAW_POST, above all mirrors: combo drop,
  // popup menu, toast). Each widget mirror paints its widget in DRAW_MAIN
  // (parent-relative coords, exactly like the immediate-mode walk) and its
  // after-children pass in DRAW_POST (widget-local coords). LVGL owns
  // invalidation: lv_obj_invalidate on a mirror repaints only objects
  // intersecting that widget's rect.

  static neui_render_ctx_t frame_ctx_of(WindowData* w)
  {
    if (!w || !w->session) return nullptr;
    auto* fwd = w->session->get_widget(w->widget_index);
    return fwd ? fwd->render_ctx : nullptr;
  }

  static void frame_bg_draw_cb(lv_event_t* e)
  {
    auto* w = static_cast<WindowData*>(lv_event_get_user_data(e));
    neui_render_ctx_t ctx = frame_ctx_of(w);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!ctx || !layer) return;
    ++t_inside_lv;
    neui_lvgl_backend::bind_layer(ctx, layer, 0, 0);
    w->session->paint_frame_background_retained(ctx, w->widget_index);
    neui_lvgl_backend::unbind_layer(ctx);
    --t_inside_lv;
  }

  static void frame_overlay_draw_cb(lv_event_t* e)
  {
    auto* w = static_cast<WindowData*>(lv_event_get_user_data(e));
    neui_render_ctx_t ctx = frame_ctx_of(w);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!ctx || !layer) return;
    ++t_inside_lv;
    neui_lvgl_backend::bind_layer(ctx, layer, 0, 0);
    w->session->paint_overlays_retained(ctx, w->widget_index);
    neui_lvgl_backend::unbind_layer(ctx);
    --t_inside_lv;
  }

  static void widget_draw_cb(lv_event_t* e)
  {
    auto* ref = static_cast<MirrorRef*>(lv_event_get_user_data(e));
    WindowData* w = ref ? ref->w : nullptr;
    if (!w || !w->session) return;
    Session* s = w->session;
    if (!s->_widgets.exists(ref->idx)) return;
    neui_render_ctx_t ctx = frame_ctx_of(w);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!ctx || !layer) return;

    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    auto& wd = s->_widgets[ref->idx];

    ++t_inside_lv;
    // wd.paint draws at the widget's parent-relative (x, y): bind the base so
    // that position lands on the mirror's display-coord origin.
    neui_lvgl_backend::bind_layer(ctx, layer, coords.x1 - wd.x, coords.y1 - wd.y);
    s->paint_widget_retained(ctx, ref->idx, /*after_children=*/false);
    neui_lvgl_backend::unbind_layer(ctx);
    --t_inside_lv;

    // SECTION / TABVIEW body layout is computed by the paint above; if it
    // moved since the last sync (first paint in particular - the layout
    // reads zeros before it), re-sync so the body container + children
    // follow. Converges one refresh later.
    if (const auto* slay = wd.section_layout_ptr()) {
      auto it = w->mirrors.find(ref->idx);
      if (it != w->mirrors.end()) {
        auto* sst = wd.scroll_state_ptr();
        const int sx = sst ? sst->scroll_x : 0;
        const int sy = sst ? sst->scroll_y : 0;
        MirrorEntry& me = it->second;
        if (me.synced_body_x != slay->body_x || me.synced_body_y != slay->body_y ||
            me.synced_body_w != slay->body_w || me.synced_body_h != slay->body_h ||
            me.synced_scroll_x != sx || me.synced_scroll_y != sy)
          w->tree_dirty = true;
      }
    }
  }

  static void widget_draw_post_cb(lv_event_t* e)
  {
    auto* ref = static_cast<MirrorRef*>(lv_event_get_user_data(e));
    WindowData* w = ref ? ref->w : nullptr;
    if (!w || !w->session) return;
    Session* s = w->session;
    if (!s->_widgets.exists(ref->idx)) return;
    neui_render_ctx_t ctx = frame_ctx_of(w);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!ctx || !layer) return;

    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);

    ++t_inside_lv;
    // paint_after_children runs in widget-local coords (the immediate-mode
    // walk has translate(x, y) active there) - base is the mirror origin.
    neui_lvgl_backend::bind_layer(ctx, layer, coords.x1, coords.y1);
    s->paint_widget_retained(ctx, ref->idx, /*after_children=*/true);
    neui_lvgl_backend::unbind_layer(ctx);
    --t_inside_lv;
  }

  static void make_passive(lv_obj_t* obj)
  {
    lv_obj_remove_style_all(obj);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_click_focusable(obj, false);
  }

  // neui widgets draw centered 1px borders / focus outlines that extend a
  // hair past the widget rect; give every mirror a small ext draw margin so
  // LVGL neither clips them nor misses them during invalidation.
  static void widget_ext_draw_cb(lv_event_t* e)
  {
    lv_event_set_ext_draw_size(e, 4);
  }

  // One pass of the mirror sync: walk the widget tree exactly like
  // paint_widgets_recursive's geometry logic (abs-coord recompute, SECTION /
  // TABVIEW body offsets and scroll), creating / positioning / hiding the
  // mirror objects to match. LVGL invalidates whatever actually moved.
  static void sync_children(WindowData* w, uint32_t parent_index,
                            lv_obj_t* parent_obj,
                            int parent_abs_x, int parent_abs_y,
                            int origin_x, int origin_y,
                            std::unordered_set<uint32_t>& live)
  {
    Session* s = w->session;
    for (uint32_t idx = s->_widgets.child(parent_index); idx != 0;
         idx = s->_widgets.next(idx)) {
      if (!s->_widgets.exists(idx)) continue;
      auto& wd = s->_widgets[idx];
      wd.abs_x = parent_abs_x + wd.x;
      wd.abs_y = parent_abs_y + wd.y;

      const bool paintable = !wd.native_handle && !wd.is_menubar() &&
                             wd.width > 0 && wd.height > 0;
      MirrorEntry& me = w->mirrors[idx];
      live.insert(idx);

      if (paintable) {
        if (me.obj && me.parent != parent_obj) {
          // Tree slot reused under a different parent - rebuild the mirror.
          lv_obj_delete(me.obj);
          me.obj = nullptr;
          me.body = nullptr;
        }
        if (!me.obj) {
          me.obj    = lv_obj_create(parent_obj);
          me.parent = parent_obj;
          make_passive(me.obj);
          // neui's immediate-mode walk does not clip children to their
          // parent (only SECTION bodies clip); match that.
          lv_obj_set_overflow_visible(me.obj, true);
          me.ref = std::make_unique<MirrorRef>(MirrorRef{ w, idx });
          lv_obj_add_event_cb(me.obj, widget_draw_cb, LV_EVENT_DRAW_MAIN, me.ref.get());
          lv_obj_add_event_cb(me.obj, widget_draw_post_cb, LV_EVENT_DRAW_POST, me.ref.get());
          lv_obj_add_event_cb(me.obj, widget_ext_draw_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, nullptr);
        }
        lv_obj_set_pos(me.obj, origin_x + wd.x, origin_y + wd.y);
        lv_obj_set_size(me.obj, wd.width, wd.height);
        lv_obj_set_hidden(me.obj, !wd.visible);
      } else if (me.obj) {
        lv_obj_set_hidden(me.obj, true);
      }

      // Descent. Container + origins for the children:
      lv_obj_t* container      = me.obj ? me.obj : parent_obj;
      int       child_origin_x = me.obj ? 0 : origin_x + wd.x;
      int       child_origin_y = me.obj ? 0 : origin_y + wd.y;
      int       abs_ox         = wd.abs_x;
      int       abs_oy         = wd.abs_y;

      const auto* slay = wd.section_layout_ptr();
      auto*       sst  = wd.scroll_state_ptr();
      if (slay && me.obj) {
        const int sx = sst ? sst->scroll_x : 0;
        const int sy = sst ? sst->scroll_y : 0;
        if (!me.body) {
          me.body = lv_obj_create(me.obj);
          make_passive(me.body);
          // The body container CLIPS its children (LVGL default) - the
          // SECTION body clip the immediate-mode walk pushes explicitly.
        }
        lv_obj_set_pos(me.body, slay->body_x, slay->body_y);
        lv_obj_set_size(me.body, slay->body_w, slay->body_h);
        me.synced_body_x = slay->body_x;
        me.synced_body_y = slay->body_y;
        me.synced_body_w = slay->body_w;
        me.synced_body_h = slay->body_h;
        me.synced_scroll_x = sx;
        me.synced_scroll_y = sy;
        container      = me.body;
        child_origin_x = -sx;
        child_origin_y = -sy;
        abs_ox = wd.abs_x + slay->body_x - sx;
        abs_oy = wd.abs_y + slay->body_y - sy;
      }

      sync_children(w, idx, container, abs_ox, abs_oy,
                    child_origin_x, child_origin_y, live);
    }
  }

  static void sync_mirror_tree(WindowData* w)
  {
    if (!w->session || !w->screen) return;
    LvLockGuard lock;
    std::unordered_set<uint32_t> live;
    sync_children(w, w->widget_index, w->screen, 0, 0, 0, 0, live);

    // Sweep mirrors whose widget vanished. Deleting an lv_obj deletes its
    // subtree, so delete only the roots of dead subtrees (entries whose
    // parent obj itself belongs to a dead entry are freed by that delete).
    std::unordered_set<lv_obj_t*> dead_objs;
    for (auto& kv : w->mirrors) {
      if (live.count(kv.first)) continue;
      if (kv.second.obj)  dead_objs.insert(kv.second.obj);
      if (kv.second.body) dead_objs.insert(kv.second.body);
    }
    for (auto it = w->mirrors.begin(); it != w->mirrors.end();) {
      if (live.count(it->first)) { ++it; continue; }
      if (it->second.obj && !dead_objs.count(it->second.parent))
        lv_obj_delete(it->second.obj);
      it = w->mirrors.erase(it);
    }
  }

  static void sync_dirty_trees()
  {
    if (!g_retained) return;
    for (auto* w : g_windows) {
      if (!w->tree_dirty) continue;
      w->tree_dirty = false;
      sync_mirror_tree(w);
    }
  }

  // -------------------------------------------------------------------------
  // Input drain (main thread)

  static uint32_t wheel_lines(int raw_delta, bool horizontal)
  {
    UINT n = 3;
    SystemParametersInfoW(horizontal ? SPI_GETWHEELSCROLLCHARS
                                     : SPI_GETWHEELSCROLLLINES, 0, &n, 0);
    (void)raw_delta;
    return n;
  }

  static void dispatch_key_to_focused(Session* sess, neui_event_type_t type,
                                      uint32_t keycode, uint32_t mods)
  {
    if (!sess) return;
    uint32_t fw = sess->_focused_widget;
    if (fw == 0 || !sess->_widgets.exists(fw)) return;
    auto& wd = sess->_widgets[fw];
    bool consumed = false;
    if (wd.emit_events) {
      neui_event_t ev = {};
      ev.type     = type;
      ev.data.key = { { wd.widget_id }, keycode, mods };
      consumed = sess->dispatch_event(&ev);
    }
    if (!consumed)
      sess->handle_input_key(type, keycode, mods);
  }

  static void invalidate_window(WindowData* w)
  {
    if (!w || !w->screen) return;
    if (t_inside_lv > 0) { w->pending_invalidate = true; return; }
    LvLockGuard lock;
    lv_obj_invalidate(w->screen);
  }

  static void flush_pending_invalidates()
  {
    for (auto* w : g_windows) {
      if (w->pending_invalidate && w->screen) {
        w->pending_invalidate = false;
        w->pending_widget_invals.clear();  // whole frame covers them
        LvLockGuard lock;
        lv_obj_invalidate(w->screen);
      }
      if (!w->pending_widget_invals.empty()) {
        LvLockGuard lock;
        for (uint32_t idx : w->pending_widget_invals) {
          auto it = w->mirrors.find(idx);
          if (it != w->mirrors.end() && it->second.obj)
            lv_obj_invalidate(it->second.obj);
          else if (w->screen)
            lv_obj_invalidate(w->screen);
        }
        w->pending_widget_invals.clear();
      }
    }
  }

  // Owning frame's WindowData for a widget (nullptr when the widget has no
  // frame ancestor with a live window yet).
  static WindowData* find_window(void* native_handle);

  static WindowData* window_for_widget(Session* s, uint32_t widget_index)
  {
    if (!s || widget_index == 0) return nullptr;
    // The widget may BE the frame (invalidate / repaint on a frame widget):
    // find_parent_native_handle walks strict ancestors only.
    void* native = nullptr;
    if (s->_widgets.exists(widget_index) && s->_widgets[widget_index].native_handle)
      native = s->_widgets[widget_index].native_handle;
    else
      native = s->find_parent_native_handle(widget_index);
    return native ? find_window(native) : nullptr;
  }

  // ---- Retained-mode seams (called from host.cpp / widgets.cpp) -----------

  void platform_retained_widget_invalidate(Session* session, uint32_t widget_index)
  {
    WindowData* w = window_for_widget(session, widget_index);
    if (!w) return;
    if (!g_retained) { invalidate_window(w); return; }
    auto it = w->mirrors.find(widget_index);
    if (it == w->mirrors.end() || !it->second.obj) {
      // No mirror yet (created this frame, or the frame widget itself):
      // fall back to the whole-frame invalidate.
      invalidate_window(w);
      return;
    }
    if (t_inside_lv > 0) {
      w->pending_widget_invals.push_back(widget_index);
      return;
    }
    LvLockGuard lock;
    lv_obj_invalidate(it->second.obj);
  }

  void platform_retained_tree_changed(Session* session, uint32_t widget_index)
  {
    WindowData* w = window_for_widget(session, widget_index);
    if (!w) return;
    if (!g_retained) { invalidate_window(w); return; }
    w->tree_dirty = true;
    SetEvent(g_wake);
  }

  static void handle_close_request(WindowData* w);

  static void drain_one(const InputMsg& m)
  {
    WindowData* w = m.w;
    Session*    s = w ? w->session : nullptr;
    if (!s) return;
    auto* fwd = s->get_widget(w->widget_index);
    if (!fwd) return;

#ifdef NEUI_LVGL_INPUT_TRACE
    if (m.msg != WM_MOUSEMOVE)
      printf("drain: msg=0x%04x wp=%llx x=%d y=%d\n", m.msg,
             (unsigned long long)m.wp, GET_X_LPARAM(m.lp), GET_Y_LPARAM(m.lp));
#endif

    switch (m.msg) {

      case WM_MOUSEMOVE: {
        const float lx = static_cast<float>(GET_X_LPARAM(m.lp));
        const float ly = static_cast<float>(GET_Y_LPARAM(m.lp));
        if (s->_popup_active) { s->handle_popup_hover(lx, ly); break; }
        if (s->handle_combo_scroll_drag(ly)) break;
        if (s->handle_combo_hover(lx, ly)) break;

        uint32_t hit = s->widget_at(lx, ly, w->widget_index);
        s->set_hovered(hit);

        uint32_t pressed = s->_pressed_widget;
        uint32_t target  = (pressed != 0 && (m.wp & MK_LBUTTON)) ? pressed : hit;
        if (target != 0) {
          if (auto* hw = s->get_widget(target)) {
            neui_event_t ev = {};
            ev.type                 = NEUI_EVENT_MOUSE_MOVE;
            ev.data.mouse.widget    = { hw->widget_id };
            ev.data.mouse.x         = static_cast<int>(lx);
            ev.data.mouse.y         = static_cast<int>(ly);
            ev.data.mouse.buttonmap = static_cast<uint32_t>(m.wp)
                                      & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON);
            s->dispatch_mouse_event(target, &ev);
          }
        }
        break;
      }

      case WM_MOUSELEAVE:
        s->set_hovered(0);
        break;

      case WM_LBUTTONDOWN: {
        const float lx = static_cast<float>(GET_X_LPARAM(m.lp));
        const float ly = static_cast<float>(GET_Y_LPARAM(m.lp));

        // Double-click synthesis: the LVGL window class lacks CS_DBLCLKS, so
        // fold a rapid same-spot second DOWN into MOUSE_BUTTON_DBLCLICK
        // (widgets that ignore DBLCLICK still see the later UP -> CLICK,
        // matching the win32 host's parity behaviour).
        const uint32_t now = static_cast<uint32_t>(GetTickCount64());
        const int      ix  = static_cast<int>(lx), iy = static_cast<int>(ly);
        bool dblclick = (now - w->last_click_ms) <= GetDoubleClickTime() &&
                        std::abs(ix - w->last_click_x) <= 4 &&
                        std::abs(iy - w->last_click_y) <= 4;
        w->last_click_ms = dblclick ? 0 : now;   // third click = plain again
        w->last_click_x  = ix;
        w->last_click_y  = iy;

        if (s->_popup_active) { s->handle_popup_click(lx, ly); break; }
        if (s->handle_toast_click(w->widget_index, lx, ly)) break;
        if (s->handle_combo_click(lx, ly)) break;

        uint32_t hit = s->widget_at(lx, ly, w->widget_index);
        s->set_focus(hit);
        s->set_pressed(hit);
        if (hit != 0) {
          if (auto* hw = s->get_widget(hit)) {
            neui_event_t ev = {};
            ev.type                 = dblclick ? NEUI_EVENT_MOUSE_BUTTON_DBLCLICK
                                               : NEUI_EVENT_MOUSE_BUTTON_DOWN;
            ev.data.mouse.widget    = { hw->widget_id };
            ev.data.mouse.x         = ix;
            ev.data.mouse.y         = iy;
            ev.data.mouse.buttonmap = static_cast<uint32_t>(m.wp);
            s->dispatch_mouse_event(hit, &ev);
          }
        }
        break;
      }

      case WM_LBUTTONUP: {
        if (s->_combo_sb_dragging) { s->_combo_sb_dragging = false; break; }
        const float lx = static_cast<float>(GET_X_LPARAM(m.lp));
        const float ly = static_cast<float>(GET_Y_LPARAM(m.lp));
        uint32_t hit     = s->widget_at(lx, ly, w->widget_index);
        uint32_t pressed = s->_pressed_widget;
        s->set_pressed(0);
        if (hit != 0) {
          if (auto* hw = s->get_widget(hit)) {
            neui_event_t ev = {};
            ev.data.mouse.widget    = { hw->widget_id };
            ev.data.mouse.x         = static_cast<int>(lx);
            ev.data.mouse.y         = static_cast<int>(ly);
            ev.data.mouse.buttonmap = 0;
            ev.type = NEUI_EVENT_MOUSE_BUTTON_UP;
            s->dispatch_mouse_event(hit, &ev);
            if (hit == pressed) {
              ev.type = NEUI_EVENT_MOUSE_BUTTON_CLICK;
              s->dispatch_mouse_event(hit, &ev);
            }
          }
        }
        break;
      }

      case WM_RBUTTONDOWN: {
        const float lx = static_cast<float>(GET_X_LPARAM(m.lp));
        const float ly = static_cast<float>(GET_Y_LPARAM(m.lp));
        if (s->_popup_active) { s->handle_popup_click(lx, ly); break; }
        uint32_t hit = s->widget_at(lx, ly, w->widget_index);
        if (hit == 0) break;
        if (auto* hw = s->get_widget(hit)) {
          neui_event_t ev = {};
          ev.type                 = NEUI_EVENT_MOUSE_RBUTTON_DOWN;
          ev.data.mouse.widget    = { hw->widget_id };
          ev.data.mouse.x         = static_cast<int>(lx);
          ev.data.mouse.y         = static_cast<int>(ly);
          s->dispatch_mouse_event(hit, &ev);
        }
        break;
      }

      case WM_RBUTTONUP: {
        const float lx = static_cast<float>(GET_X_LPARAM(m.lp));
        const float ly = static_cast<float>(GET_Y_LPARAM(m.lp));
        uint32_t hit = s->widget_at(lx, ly, w->widget_index);
        if (hit == 0) break;
        if (auto* hw = s->get_widget(hit)) {
          neui_event_t ev = {};
          ev.type                 = NEUI_EVENT_MOUSE_RBUTTON_UP;
          ev.data.mouse.widget    = { hw->widget_id };
          ev.data.mouse.x         = static_cast<int>(lx);
          ev.data.mouse.y         = static_cast<int>(ly);
          s->dispatch_mouse_event(hit, &ev);
        }
        break;
      }

      case WM_MOUSEWHEEL:
      case WM_MOUSEHWHEEL: {
        const bool horizontal = (m.msg == WM_MOUSEHWHEEL);
        POINT pt = { GET_X_LPARAM(m.lp), GET_Y_LPARAM(m.lp) };  // screen coords
        ScreenToClient(w->hwnd, &pt);
        const float lx = static_cast<float>(pt.x);
        const float ly = static_cast<float>(pt.y);

        const int raw   = GET_WHEEL_DELTA_WPARAM(m.wp);
        const int lines = static_cast<int>(wheel_lines(raw, horizontal));
        int delta = (raw * lines) / WHEEL_DELTA;
        const bool is_horiz = horizontal || (m.wp & MK_SHIFT) != 0;

        // The combo drop list scrolls on the vertical convention - intercept
        // before the horizontal sign flip below, so shift+wheel over an open
        // list scrolls the same direction a plain wheel does (platform_win32
        // hands handle_combo_wheel the unflipped delta too).
        if (!horizontal && s->handle_combo_wheel(lx, ly, delta)) break;

        // WM_MOUSEHWHEEL's positive delta means "to the right", the opposite of
        // the vertical convention; platform_win32 flips it the same way. Shift +
        // vertical wheel is the same gesture.
        if (is_horiz) delta = -delta;

        uint32_t hit = s->widget_at(lx, ly, w->widget_index);
        if (hit == 0) break;
        if (auto* hw = s->get_widget(hit)) {
          neui_event_t ev = {};
          ev.type                     = NEUI_EVENT_MOUSE_WHEEL;
          ev.data.wheel.widget        = { hw->widget_id };
          ev.data.wheel.x             = static_cast<int>(lx);
          ev.data.wheel.y             = static_cast<int>(ly);
          ev.data.wheel.delta         = delta;
          ev.data.wheel.is_horizontal = is_horiz ? 1 : 0;
          // Stepped scrolling only in the prototype (no kinetics timers) -
          // ancestors get the wheel via the normal bubble.
          s->dispatch_wheel_event(hit, &ev);
        }
        break;
      }

      case WM_KEYDOWN: {
        if (s->_popup_active &&
            s->handle_popup_key(static_cast<uint32_t>(m.wp)))
          break;
        if (m.wp == VK_TAB) {
          s->focus_next(!(m.mods & 1));
          break;
        }
        dispatch_key_to_focused(s, NEUI_EVENT_KEYDOWN,
                                static_cast<uint32_t>(m.wp), m.mods);
        break;
      }

      case WM_KEYUP:
        dispatch_key_to_focused(s, NEUI_EVENT_KEYUP,
                                static_cast<uint32_t>(m.wp), m.mods);
        break;

      case WM_CHAR: {
        // Assemble UTF-16 surrogate pairs into full codepoints (same logic
        // as platform_win32.cpp).
        const uint16_t ch = static_cast<uint16_t>(m.wp);
        uint32_t codepoint;
        if (ch >= 0xD800 && ch <= 0xDBFF) { w->pending_surrogate = ch; break; }
        if (ch >= 0xDC00 && ch <= 0xDFFF) {
          if (w->pending_surrogate == 0) break;
          codepoint = 0x10000u
            + (static_cast<uint32_t>(w->pending_surrogate - 0xD800) << 10)
            + static_cast<uint32_t>(ch - 0xDC00);
          w->pending_surrogate = 0;
        } else {
          w->pending_surrogate = 0;
          codepoint = ch;
        }
        dispatch_key_to_focused(s, NEUI_EVENT_KEYCHAR, codepoint, m.mods);
        break;
      }

      case WM_SIZE: {
        if (m.wp == SIZE_MINIMIZED) break;
        const int cw = LOWORD(m.lp);
        const int ch = HIWORD(m.lp);
        if (cw <= 0 || ch <= 0) break;
        s->resize_render_ctx(w->widget_index, static_cast<uint32_t>(cw),
                             static_cast<uint32_t>(ch));
        fwd->width  = cw;   // logical == physical == LVGL px
        fwd->height = ch;
        neui_event_t ev = {};
        ev.type               = NEUI_EVENT_RESIZE;
        ev.data.resize.widget = { fwd->widget_id };
        ev.data.resize.width  = cw;
        ev.data.resize.height = ch;
        s->dispatch_event(&ev);
        invalidate_window(w);
        break;
      }

      case WM_SETFOCUS:
      case WM_KILLFOCUS: {
        const bool gained = (m.msg == WM_SETFOCUS);
        s->_os_focused = gained;
        if (s->_focused_widget != 0 && s->_widgets.exists(s->_focused_widget)) {
          auto& wd = s->_widgets[s->_focused_widget];
          if (wd.emit_events) {
            neui_event_t ev = {};
            ev.type               = NEUI_EVENT_WIDGET_FOCUS;
            ev.data.focus.widget  = { wd.widget_id };
            ev.data.focus.focused = gained;
            s->dispatch_event(&ev);
          }
        }
        invalidate_window(w);
        break;
      }

      case WM_CLOSE:
        handle_close_request(w);
        break;

      default:
        break;
    }
  }

  static void drain_input()
  {
    for (;;) {
      InputMsg m;
      {
        EnterCriticalSection(&g_queue_lock);
        if (g_queue.empty()) { LeaveCriticalSection(&g_queue_lock); return; }
        m = g_queue.front();
        g_queue.pop_front();
        LeaveCriticalSection(&g_queue_lock);
      }
      drain_one(m);
      if (g_quit) return;
    }
  }

  // Close semantics: APPWINDOW asks the client (APP_QUIT event) and, when
  // allowed, hides the window + decrements the quit count; DIALOG re-enables
  // its owner and unwinds a modal pump. Windows are hidden, not destroyed
  // (see the file header note about the driver watchdog).
  //
  // This is the bookkeeping half - it asks the client nothing. The APP_QUIT veto
  // belongs to handle_close_request alone, so the teardown path
  // (platform_destroy_window) cannot re-enter the client mid-destroy.
  static void close_window_silently(WindowData* w)
  {
    Session* s  = w->session;
    if (!s || w->closed) return;
    auto*    wd = s->get_widget(w->widget_index);

    w->closed = true;
    ShowWindowAsync(w->hwnd, SW_HIDE);
    if (!wd) return;

    if (wd->type && !strcmp(wd->type, NEUI_W_APPWINDOW)) {
      if (--g_appwindow_count <= 0) g_quit = true;
      return;
    }

    // DIALOG (and PLUGWINDOW): for modal dialogs re-enable + refocus the owner
    // and drop the nested pump so widget_show returns. Both USER32 calls block
    // on the owner's thread, so they go through the deferring seams.
    if (wd->is_dialog() && wd->owner_index != 0 &&
        s->_widgets.exists(wd->owner_index)) {
      bool is_modal = !wd->attrs || wd->attrs->get_int(NEUI_ATTR_MODAL, 1) != 0;
      void* owner_native = s->_widgets[wd->owner_index].native_handle;
      if (is_modal && owner_native) {
        platform_set_window_enabled(owner_native, true);
        platform_activate_window(owner_native);
      }
    }
    if (auto* fw = dynamic_cast<FrameWidget*>(wd))
      fw->modal_pump_active = false;
  }

  static void handle_close_request(WindowData* w)
  {
    Session* s  = w->session;
    if (!s || w->closed) return;
    auto*    wd = s->get_widget(w->widget_index);
    if (!wd) return;

    if (wd->type && !strcmp(wd->type, NEUI_W_APPWINDOW)) {
      neui_event_t ev = {};
      ev.type = NEUI_EVENT_APP_QUIT;
      if (!s->dispatch_event(&ev)) return;   // client vetoed the close
    }
    close_window_silently(w);
  }

  // -------------------------------------------------------------------------
  // Platform seam: init / backend / windows / loop

  void platform_init()
  {
    if (g_inited) return;
    g_inited = true;
    InitializeCriticalSection(&g_queue_lock);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    read_retained_env();
    lv_init();   // also runs lv_windows_platform_init() under LV_USE_WINDOWS
    // Let the backend guard the LVGL allocations it makes outside a draw
    // dispatch (font instances / glyph caches from measure_text, path deletes
    // in destroy_context) with this layer's re-entrant lock policy.
    neui_lvgl_backend::set_lock_hooks(backend_lv_lock, backend_lv_unlock);
  }

  neui_render_backend_t* platform_get_backend()
  {
    return neui_lvgl_backend::get_backend();
  }

  static std::wstring to_wide(const char* utf8)
  {
    if (!utf8 || !*utf8) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &out[0], n);
    return out;
  }

  static void create_lvgl_window(Session* session, uint32_t widget_index,
                                 WidgetData& wd)
  {
    platform_init();

    // NOTE: must NOT hold lv_lock() here - lv_windows_create_display blocks
    // on its display thread's WM_CREATE, whose WndProc itself takes the lock.
    lv_display_t* disp = lv_windows_create_display(
        to_wide(wd.text.c_str()).c_str(),
        wd.width, wd.height,
        100,      // zoom
        false,    // allow_dpi_override: keep the driver's DPI bookkeeping
        false);   // application mode: client area == display resolution
    if (!disp) return;

    LvLockGuard lock;  // main thread, outside lv_timer_handler

    HWND hwnd = lv_windows_get_display_window_handle(disp);

    auto* w = new WindowData();
    w->session      = session;
    w->widget_index = widget_index;
    w->display      = disp;
    w->hwnd         = hwnd;
    w->screen       = lv_display_get_screen_active(disp);
    g_windows.push_back(w);

    // Strip the theme style from the screen; neui paints the background.
    lv_obj_remove_style_all(w->screen);
    if (g_retained) {
      // Option C: screen paints frame bg below the widget mirrors and the
      // overlays above them; widgets are mirrored by sync_mirror_tree.
      lv_obj_add_event_cb(w->screen, frame_bg_draw_cb, LV_EVENT_DRAW_MAIN, w);
      lv_obj_add_event_cb(w->screen, frame_overlay_draw_cb, LV_EVENT_DRAW_POST, w);
      w->tree_dirty = true;
    } else {
      // Approach A baseline: the whole frame paints into the screen object.
      lv_obj_add_event_cb(w->screen, screen_draw_cb, LV_EVENT_DRAW_MAIN, w);
    }

    // Input bridge: context prop + WndProc subclass (the window lives on the
    // driver's thread; the subclass only enqueues). Read and store the chained
    // proc BEFORE swapping: the window is already being pumped by the driver's
    // own thread, so subclass_proc can run the instant the swap takes effect -
    // and it tail-calls CallWindowProcW(w->prev_proc, ...).
    w->prev_proc = reinterpret_cast<WNDPROC>(
        GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    SetPropW(hwnd, L"neui.lvgl.wdata", w);
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(subclass_proc));

    wd.native_handle = hwnd;
    wd.dpi           = 96;   // logical == LVGL px == physical (see header)

    auto* backend = platform_get_backend();
    if (backend)
      wd.render_ctx = backend->create_context(
          disp, static_cast<uint32_t>(wd.width), static_cast<uint32_t>(wd.height));

    if (wd.type && !strcmp(wd.type, NEUI_W_APPWINDOW))
      ++g_appwindow_count;
  }

  void platform_create_appwindow(Session* session, uint32_t widget_index,
                                 WidgetData& wd)
  {
    create_lvgl_window(session, widget_index, wd);
  }

  void platform_create_plugwindow(Session* session, uint32_t widget_index,
                                  WidgetData& wd)
  {
    create_lvgl_window(session, widget_index, wd);
  }

  void platform_create_dialog(Session* session, uint32_t widget_index,
                              WidgetData& wd, void* /*owner_native*/)
  {
    // Own display window; owner blocking happens via
    // platform_set_window_enabled from widget_show. (Resizable in the
    // prototype - the driver offers no per-window style control.)
    create_lvgl_window(session, widget_index, wd);
  }

  static WindowData* find_window(void* native_handle)
  {
    for (auto* w : g_windows)
      if (w->hwnd == native_handle) return w;
    return nullptr;
  }

  // The driver owns the HWND + lv_display, and its watchdog exit(0)s the
  // process if a display dies mid-loop (see the file header), so a destroyed
  // frame keeps its window - hidden - for the process lifetime. Everything neui
  // put on top of it does go away here: the retained mirror objects, the screen
  // draw callbacks, the toast timer, the WndProc subclass, and any input still
  // queued for this window.
  //
  // The WindowData itself is retired rather than freed: subclass_proc runs on
  // the driver's thread and a call may already be in flight holding this
  // pointer, with no way to join that thread. It is inert once `session` is
  // null - drain_one and every draw callback bail on that.
  static std::vector<WindowData*> g_retired_windows;

  static void retire_window(WindowData* w)
  {
    // 1. Stop the display thread referencing it, then drop what it already
    //    queued (those InputMsgs would otherwise drain against a dead window).
    if (w->hwnd) {
      if (w->prev_proc)
        SetWindowLongPtrW(w->hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(w->prev_proc));
      RemovePropW(w->hwnd, L"neui.lvgl.wdata");
    }
    {
      EnterCriticalSection(&g_queue_lock);
      for (auto it = g_queue.begin(); it != g_queue.end();)
        it = (it->w == w) ? g_queue.erase(it) : it + 1;
      LeaveCriticalSection(&g_queue_lock);
    }

    // 2. Release the LVGL objects neui created. Every mirror (and SECTION body)
    //    is a descendant of the screen, so one clean drops the whole tree; the
    //    MirrorRefs die with the map entries afterwards.
    {
      LvLockGuard lock;
      if (w->toast_timer) {
        lv_timer_delete(w->toast_timer);
        w->toast_timer = nullptr;
      }
      if (w->screen) {
        lv_obj_clean(w->screen);
        lv_obj_remove_event_cb(w->screen, screen_draw_cb);
        lv_obj_remove_event_cb(w->screen, frame_bg_draw_cb);
        lv_obj_remove_event_cb(w->screen, frame_overlay_draw_cb);
      }
    }
    w->mirrors.clear();
    w->pending_widget_invals.clear();
    w->pending_invalidate = false;

    // 3. Out of g_windows so the per-turn walks skip it, and inert for anything
    //    that still holds the pointer.
    w->session = nullptr;
    w->screen  = nullptr;
    g_windows.erase(std::remove(g_windows.begin(), g_windows.end(), w),
                    g_windows.end());
    g_retired_windows.push_back(w);
  }

  void platform_set_embed_parent(Session*, uint32_t, unsigned long) {}
  int  platform_embed_event_fd(void*) { return -1; }
  void platform_embed_pump_and_tick(void*) {}

  void platform_destroy_window(WidgetData& wd)
  {
    WindowData* w = find_window(wd.native_handle);
    if (w) {
      // Programmatic destroy (client closing a dialog, or session teardown):
      // run the close bookkeeping WITHOUT asking the client. handle_close_request
      // would dispatch APP_QUIT into a client whose widgets are being destroyed,
      // and a veto there would leave the window visible while the render context
      // below is freed anyway.
      close_window_silently(w);
      retire_window(w);
    }
    if (wd.render_ctx) {
      auto* backend = platform_get_backend();
      if (backend && wd.session) {
        wd.session->_asset_manager.release_context(wd.render_ctx, backend);
        backend->destroy_context(wd.render_ctx);
      }
      wd.render_ctx = nullptr;
    }
    wd.native_handle = nullptr;
  }

  void platform_show_window(void* native_handle)
  {
    if (!native_handle) return;
    ShowWindowAsync(static_cast<HWND>(native_handle), SW_SHOW);
    if (WindowData* w = find_window(native_handle)) {
      w->closed = false;
      if (g_retained) w->tree_dirty = true;   // widgets created pre-show
      invalidate_window(w);
    }
  }

  void platform_hide_window(void* native_handle)
  {
    if (native_handle)
      ShowWindowAsync(static_cast<HWND>(native_handle), SW_HIDE);
  }

  // The four seams below all issue BLOCKING cross-thread USER32 calls - see the
  // defer_if_locked note near the top of the file. Each captures its arguments
  // by value so the deferred copy stays valid.

  void platform_set_window_enabled(void* native_handle, bool enabled)
  {
    HWND hwnd = static_cast<HWND>(native_handle);
    if (!hwnd) return;
    if (defer_if_locked([hwnd, enabled] {
          EnableWindow(hwnd, enabled ? TRUE : FALSE);
        }))
      return;
    EnableWindow(hwnd, enabled ? TRUE : FALSE);
  }

  void platform_activate_window(void* native_handle)
  {
    HWND hwnd = static_cast<HWND>(native_handle);
    if (!hwnd) return;
    if (defer_if_locked([hwnd] { SetForegroundWindow(hwnd); })) return;
    SetForegroundWindow(hwnd);
  }

  void platform_set_window_title(void* native_handle, const char* text)
  {
    HWND hwnd = static_cast<HWND>(native_handle);
    if (!hwnd) return;
    std::wstring wide = to_wide(text ? text : "");
    if (defer_if_locked([hwnd, wide] { SetWindowTextW(hwnd, wide.c_str()); }))
      return;
    SetWindowTextW(hwnd, wide.c_str());
  }

  void platform_set_window_pos(void* native_handle,
                               int x, int y, int w, int h, uint32_t /*dpi*/)
  {
    // Reposition only; resizing must go through the driver's WM_SIZE path,
    // which our subclass sees and forwards. Sizes here are the client area,
    // so grow by the current non-client frame.
    HWND hwnd = static_cast<HWND>(native_handle);
    if (!hwnd) return;
    if (defer_if_locked([hwnd, x, y, w, h] {
          platform_set_window_pos(hwnd, x, y, w, h, 96);
        }))
      return;
    RECT wr = { 0, 0, w, h };
    DWORD style    = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    AdjustWindowRectEx(&wr, style, FALSE, ex_style);
    SetWindowPos(hwnd, nullptr, x, y, wr.right - wr.left, wr.bottom - wr.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
  }

  void platform_post_close(void* native_handle)
  {
    if (native_handle)
      PostMessageW(static_cast<HWND>(native_handle), WM_CLOSE, 0, 0);
  }

  void platform_invalidate(void* native_handle)
  {
    invalidate_window(find_window(native_handle));
  }

  // Toast heartbeat: a 16 ms lv_timer that invalidates the frame; the paint
  // pass advances the toast phase exactly like the other platforms.
  static void toast_timer_cb(lv_timer_t* t)
  {
    auto* w = static_cast<WindowData*>(lv_timer_get_user_data(t));
    ++t_inside_lv;   // lv_timer_handler already holds the LVGL lock
    if (w && w->screen) lv_obj_invalidate(w->screen);
    --t_inside_lv;
  }

  void platform_start_toast_animation(void* native_handle)
  {
    WindowData* w = find_window(native_handle);
    if (!w) return;
    LvLockGuard lock;
    if (w->toast_timer) lv_timer_reset(w->toast_timer);
    else                w->toast_timer = lv_timer_create(toast_timer_cb, 16, w);
  }

  void platform_stop_toast_animation(void* native_handle)
  {
    WindowData* w = find_window(native_handle);
    if (!w || !w->toast_timer) return;
    LvLockGuard lock;
    lv_timer_delete(w->toast_timer);
    w->toast_timer = nullptr;
  }

  uint64_t platform_now_ms()
  {
    return GetTickCount64();
  }

  int platform_message_box(void*, const char*, const char*, uint32_t)
  {
    return 0;  // prototype stub
  }

  float platform_get_scale_factor(void*)
  {
    return 1.0f;
  }

  uint8_t* platform_load_image(const char* path,
                               uint32_t* width_out, uint32_t* height_out)
  {
    return neui_detail::load_image_bgra8_stb(path, width_out, height_out);
  }

  uint8_t* platform_load_image_bytes(const uint8_t* data, size_t len,
                                     uint32_t* width_out, uint32_t* height_out)
  {
    return neui_detail::load_image_bgra8_stb_memory(data, len,
                                                   width_out, height_out);
  }

  void platform_free_image(uint8_t* pixels)
  {
    neui_detail::free_image_bgra8_stb(pixels);
  }

  // One main-loop turn: drain queued input, run LVGL timers/refresh, wait for
  // the next wake (input arrival or timer deadline).
  // Free the backends' per-draw deferred resources once the refresh that
  // consumed them has finished (lv_timer_handler returned).
  static void collect_deferred_all()
  {
    for (auto* w : g_windows) {
      if (!w->session) continue;
      if (auto* fwd = w->session->get_widget(w->widget_index))
        if (fwd->render_ctx)
          neui_lvgl_backend::collect_deferred(fwd->render_ctx);
    }
  }

  static void loop_turn()
  {
    drain_input();
    if (g_quit) return;
    sync_dirty_trees();
    uint32_t wait = lv_timer_handler();
    collect_deferred_all();
    sync_dirty_trees();          // a draw dispatch may have marked dirt
    flush_pending_invalidates();
    run_deferred_calls();        // blocking USER32 work a draw dispatch queued
    if (g_quit) return;
    if (wait == LV_NO_TIMER_READY) wait = 10;
    else if (wait > 10) wait = 10;
    if (wait) WaitForSingleObject(g_wake, wait);
  }

  bool platform_run()
  {
    while (!g_quit)
      loop_turn();
    return true;
  }

  bool platform_pump_once()
  {
    if (g_quit) return false;
    drain_input();
    if (!g_quit) {
      sync_dirty_trees();
      lv_timer_handler();
      collect_deferred_all();
      sync_dirty_trees();
      flush_pending_invalidates();
      run_deferred_calls();
    }
    return !g_quit;
  }

  bool platform_run_modal_until(bool* keep_running)
  {
    if (!keep_running) return true;
    while (*keep_running && !g_quit)
      loop_turn();
    return !g_quit;
  }

  // -------------------------------------------------------------------------
  // Clipboard / DnD / menubar / polish - prototype stubs (compiled out per
  // the plan; neui degrades gracefully, same as the null platform).

  bool platform_clipboard_set_text(const char*, uint32_t) { return false; }
  int  platform_clipboard_get_text(char*, int) { return 0; }
  bool platform_clipboard_has_text() { return false; }
  void platform_clipboard_set_primary(const char*, uint32_t) {}
  int  platform_clipboard_get_primary(char*, int) { return 0; }
  bool platform_clipboard_write_item(const neui_detail::DataItem&) { return false; }
  bool platform_clipboard_read_item(neui_detail::DataItem&) { return false; }

  bool platform_dnd_register_window(void*, void*, uint32_t) { return false; }
  void platform_dnd_unregister_window(void*) {}
  uint32_t platform_dnd_begin_drag(void*, neui_detail::DataItem*, uint32_t,
                                   void*, int, int) { return 0; }
  void* platform_make_drag_preview(const uint8_t*, uint32_t, uint32_t, float)
  { return nullptr; }

  bool  platform_menubar_in_frame() { return false; }
  int   platform_frame_extra_top_inset(void*, bool) { return 0; }
  void* platform_menubar_create(uint32_t) { return nullptr; }
  void  platform_menubar_destroy(void*) {}
  void  platform_menubar_attach(void*, void*) {}
  void  platform_menubar_refresh(void*) {}
  void* platform_menubar_add_popup(void*, const char*) { return nullptr; }
  void  platform_menubar_add_item(void*, uint32_t, const char*) {}
  void  platform_menubar_add_separator(void*, uint32_t) {}
  void  platform_menubar_remove_popup(void*, void*) {}
  void  platform_menubar_remove_item(void*, uint32_t) {}
  void  platform_menubar_enable_item(void*, uint32_t, bool) {}
  void  platform_menubar_enable_popup(void*, void*, bool) {}
  void  platform_menubar_check_item(void*, uint32_t, bool) {}
  void  platform_menubar_set_item_text(void*, uint32_t, const char*) {}
  void  platform_menubar_set_item_shortcut(void*, uint32_t, uint32_t, uint32_t) {}

  void platform_set_window_icon(WidgetData&, const char*) {}
  void platform_apply_size_constraints(void*, int, int, int, int) {}
  void platform_set_cursor(int) {}

} // namespace xpl_host
