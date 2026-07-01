#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include <neui/neui.h>

#include "attrs.h"
#include "behavior.h"

// Behavior runtime - per-widget input state + the host-agnostic dispatch
// helpers. Both xpl + native hosts include this header and call the
// dispatch functions from their per-widget input path.
//
// Host integration shape:
//   1. The host owns a BehaviorRuntime instance per widget that holds a
//      behavior asset (lazy-allocated on first MOUSE_BUTTON_DOWN).
//   2. The host owns a BehaviorDispatchCtx that the dispatch fills in
//      with the AttrBag, widget size, and host-side callbacks for
//      invalidate / emit_attr_changed / popup_menu.
//   3. The host computes (local_x, local_y) in widget-local logical
//      pixels and calls behavior_dispatch_mouse / behavior_dispatch_key.
//   4. When the dispatch returns true the event is consumed.

namespace neui_detail
{
  // ---- Constants ----------------------------------------------------------

  // Matches KnobWidget's KNOB_SWEEP_RAD (1.5 * PI = 270deg). Rotational drag
  // travels this many radians for a full 0..1 sweep, so the value-delta
  // per radian is 1 / KNOB_SWEEP_RAD.
  inline constexpr float BEHAVIOR_KNOB_SWEEP_RAD = 4.71238898f;

  inline constexpr float BEHAVIOR_TWO_PI = 6.28318530717958647692f;
  inline constexpr float BEHAVIOR_PI     = 3.14159265358979323846f;

  // ---- Per-widget runtime state ------------------------------------------

  // Lazy-allocated on first MOUSE_BUTTON_DOWN. Identical lifetime pattern
  // as AttrBag (std::unique_ptr on WidgetData).
  struct BehaviorRuntime
  {
    bool     dragging        = false;
    uint32_t active_handler  = 0;   // handler slot that captured the drag
    int      drag_prev_x     = 0;
    int      drag_prev_y     = 0;
    float    drag_prev_angle = 0.0f;
    // Unsnapped accumulator carried across drag samples - the same shape
    // KnobWidget uses so per-frame deltas survive step snapping.
    float    drag_continuous = 0.0f;
    // BIAXIAL gets a second accumulator for the Y axis.
    float    drag_continuous_y = 0.0f;
  };

  // ---- Dispatch context (host callbacks) ---------------------------------

  // The shared dispatch reads / writes attrs through the widget's AttrBag,
  // and uses these callbacks to invalidate the widget, emit the
  // NEUI_EVENT_ATTR_CHANGED event, and (for CONTEXT_RESET) run a blocking
  // popup menu. Callbacks may be null - the dispatcher skips the
  // corresponding side-effect.
  struct BehaviorDispatchCtx
  {
    AttrBag* bag      = nullptr;  // the widget's AttrBag, ensured by host
    float    widget_w = 0.0f;     // widget size, logical px
    float    widget_h = 0.0f;
    void* host_data   = nullptr;
    void (*invalidate)(void* host_data)                                = nullptr;
    void (*emit_attr_changed)(void* host_data,
                                const char* attr_key, float value)     = nullptr;
    int  (*popup_menu)(void* host_data, int local_x, int local_y,
                        const char* const* items)                       = nullptr;
    // DRAG_SOURCE seam. Blocks until the OS drag loop ends; returns the
    // negotiated NEUI_DND_ACTION_*. Null on hosts that don't have a
    // drag-source impl (currently all three do). The runtime guards
    // re-entry on the Session side, but clears its own dragging state
    // before the call so the same widget can't fire two drags in flight.
    //
    // `preview_image` is a neui_asset_t.id (the public neui_asset_t is a
    // struct wrapping a single uint32_t; we pass the raw id here so the
    // shared runtime stays free of the public-API includes). 0 / asset
    // sentinel = no preview, OS default visual. The host translates this
    // into a platform bitmap inside its begin_drag impl.
    // `hot_x` / `hot_y` are the preview hot-spot in logical px from the
    // image top-left. -1 = image centre on that axis.
    uint32_t (*begin_drag)(void* host_data,
                            neui_data_item_t item,
                            uint32_t allowed_actions,
                            uint32_t preview_image,
                            int hot_x,
                            int hot_y)                                  = nullptr;
  };

  // ---- Math helpers (shared with KnobWidget) -----------------------------

  inline float behavior_clamp(float v, float lo, float hi)
  {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
  }

  inline float behavior_wrap_pi(float d)
  {
    while (d >  BEHAVIOR_PI) d -= BEHAVIOR_TWO_PI;
    while (d < -BEHAVIOR_PI) d += BEHAVIOR_TWO_PI;
    return d;
  }

  // Snap v onto N evenly-spaced positions across [lo, hi]. steps < 2 = no snap.
  // Mirrors snap_to_steps from hosts/crossplatform/host.cpp but extended to
  // an arbitrary [lo, hi] range (the existing helper assumes [0, 1]).
  inline float behavior_snap_to_steps(float v, int steps, float lo, float hi)
  {
    if (steps < 2 || hi <= lo) return v;
    float t = (v - lo) / (hi - lo);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    int idx = static_cast<int>(t * static_cast<float>(steps - 1) + 0.5f);
    if (idx < 0) idx = 0;
    if (idx >= steps) idx = steps - 1;
    return lo + (hi - lo) * (static_cast<float>(idx) / static_cast<float>(steps - 1));
  }

  inline int behavior_read_steps(const BehaviorHandler& H, const AttrBag* bag)
  {
    if (!bag || H.snap_attr.empty()) return 0;
    return bag->get_int(H.snap_attr, 0);
  }

  inline bool behavior_is_fine(const BehaviorHandler& H, uint32_t buttonmap)
  {
    switch (H.fine_modifier) {
      case FineModifier::Shift: return (buttonmap & NEUI_MK_SHIFT)   != 0;
      case FineModifier::Ctrl:  return (buttonmap & NEUI_MK_CONTROL) != 0;
      case FineModifier::Alt:   return (buttonmap & NEUI_MK_ALT)     != 0;
      case FineModifier::None:  return false;
    }
    return false;
  }

  inline bool behavior_is_fine_kmod(const BehaviorHandler& H, uint32_t kmod)
  {
    switch (H.fine_modifier) {
      case FineModifier::Shift: return (kmod & NEUI_KMOD_SHIFT) != 0;
      case FineModifier::Ctrl:  return (kmod & NEUI_KMOD_CTRL)  != 0;
      case FineModifier::Alt:   return (kmod & NEUI_KMOD_ALT)   != 0;
      case FineModifier::None:  return false;
    }
    return false;
  }

  // ---- Value read / write -------------------------------------------------

  inline float behavior_read_value(const BehaviorHandler& H, const AttrBag* bag)
  {
    if (!bag || H.target.empty()) return H.min;
    float v = attr_as_float(bag, H.target, H.min);
    return behavior_clamp(v, H.min, H.max);
  }

  // Read the BIAXIAL Y target. Returns H.min when target_y is unset / bag null.
  inline float behavior_read_value_y(const BehaviorHandler& H, const AttrBag* bag)
  {
    if (!bag || H.target_y.empty()) return H.min;
    float v = attr_as_float(bag, H.target_y, H.min);
    return behavior_clamp(v, H.min, H.max);
  }

  // Apply (snap -> clamp -> write) for the handler's target attr. Returns
  // true if the stored value actually changed. Emits NEUI_EVENT_ATTR_CHANGED
  // and invalidates the widget on change.
  inline bool behavior_write_value(const BehaviorHandler& H,
                                    const BehaviorDispatchCtx& ctx,
                                    const std::string& attr_key,
                                    float new_value)
  {
    if (!ctx.bag || attr_key.empty()) return false;
    int steps = behavior_read_steps(H, ctx.bag);
    new_value = behavior_clamp(new_value, H.min, H.max);
    new_value = behavior_snap_to_steps(new_value, steps, H.min, H.max);
    float old_value = attr_as_float(ctx.bag, attr_key, H.min);
    if (new_value == old_value) return false;
    ctx.bag->set_float(attr_key, new_value);
    if (ctx.invalidate) ctx.invalidate(ctx.host_data);
    if (ctx.emit_attr_changed)
      ctx.emit_attr_changed(ctx.host_data, attr_key.c_str(), new_value);
    return true;
  }

  // ---- Per-kind helpers --------------------------------------------------

  // Translate event modifiers (NEUI_MK_SHIFT / _CONTROL / _ALT bits on
  // mouse, NEUI_KMOD_* bits on keys) to the fine-scale multiplier the
  // handler wants. 1.0 = normal, fine_scale = fine.
  inline float behavior_fine_mul(const BehaviorHandler& H, uint32_t buttonmap)
  {
    return behavior_is_fine(H, buttonmap) ? H.fine_scale : 1.0f;
  }

  inline float behavior_fine_mul_kmod(const BehaviorHandler& H, uint32_t kmod)
  {
    return behavior_is_fine_kmod(H, kmod) ? H.fine_scale : 1.0f;
  }

  // ---- Mouse dispatch ----------------------------------------------------

  // Find the first handler whose hit-rect contains (local_x, local_y) and
  // whose kind accepts the given event. For drag-kind events we match
  // MOUSE_BUTTON_DOWN; the drag itself (MOUSE_MOVE / BUTTON_UP) is steered
  // by rt.active_handler once a drag is in progress, so the hit-test is
  // only consulted on the initial click.
  inline BehaviorHandler* behavior_find_drag_handler(BehaviorAsset& ba,
                                                       float widget_w, float widget_h,
                                                       float local_x, float local_y,
                                                       uint32_t* out_slot)
  {
    for (uint32_t i = 1; i < ba.handlers.size(); ++i) {
      BehaviorHandler* H = ba.handlers[i].get();
      if (!H) continue;
      switch (H->kind) {
        case NEUI_BEHAVIOR_KIND_DRAG_VERTICAL:
        case NEUI_BEHAVIOR_KIND_DRAG_HORIZONTAL:
        case NEUI_BEHAVIOR_KIND_DRAG_ROTATIONAL:
        case NEUI_BEHAVIOR_KIND_DRAG_BIAXIAL:
        case NEUI_BEHAVIOR_KIND_CLICK_TOGGLE:
        case NEUI_BEHAVIOR_KIND_CLICK_CYCLE:
        case NEUI_BEHAVIOR_KIND_CLICK_SELECT:
        case NEUI_BEHAVIOR_KIND_DRAG_SOURCE:
          break;
        default:
          continue;
      }
      if (behavior_hit_test(*H, widget_w, widget_h, local_x, local_y)) {
        if (out_slot) *out_slot = i;
        return H;
      }
    }
    return nullptr;
  }

  inline BehaviorHandler* behavior_find_kind(BehaviorAsset& ba,
                                               neui_behavior_kind_t kind,
                                               float widget_w, float widget_h,
                                               float local_x, float local_y)
  {
    for (auto& slot : ba.handlers) {
      BehaviorHandler* H = slot.get();
      if (!H) continue;
      if (H->kind != kind) continue;
      if (behavior_hit_test(*H, widget_w, widget_h, local_x, local_y))
        return H;
    }
    return nullptr;
  }

  // First handler of the given kind, regardless of hit region (used for
  // KEY_STEP which fires globally on the focused widget).
  inline BehaviorHandler* behavior_find_kind_any(BehaviorAsset& ba,
                                                   neui_behavior_kind_t kind)
  {
    for (auto& slot : ba.handlers) {
      BehaviorHandler* H = slot.get();
      if (!H) continue;
      if (H->kind == kind) return H;
    }
    return nullptr;
  }

  // Apply one drag sample. Returns true if a value write occurred.
  inline bool behavior_apply_drag_move(BehaviorHandler& H,
                                         BehaviorRuntime& rt,
                                         const BehaviorDispatchCtx& ctx,
                                         float local_x, float local_y,
                                         uint32_t buttonmap)
  {
    float fine_mul = behavior_fine_mul(H, buttonmap);
    float range    = H.max - H.min;
    if (range <= 0.0f) return false;

    if (H.kind == NEUI_BEHAVIOR_KIND_DRAG_VERTICAL) {
      // Up = increase: negative pixel delta -> positive value delta.
      float sweep = (H.sweep > 0.0f) ? H.sweep : 1.0f;
      float dy = static_cast<float>(static_cast<int>(local_y) - rt.drag_prev_y);
      rt.drag_prev_y = static_cast<int>(local_y);
      rt.drag_continuous += -dy * (range * fine_mul / sweep);
      rt.drag_continuous  = behavior_clamp(rt.drag_continuous, H.min, H.max);
      return behavior_write_value(H, ctx, H.target, rt.drag_continuous);
    }
    if (H.kind == NEUI_BEHAVIOR_KIND_DRAG_HORIZONTAL) {
      float sweep = (H.sweep > 0.0f) ? H.sweep : 1.0f;
      float dx = static_cast<float>(static_cast<int>(local_x) - rt.drag_prev_x);
      rt.drag_prev_x = static_cast<int>(local_x);
      rt.drag_continuous += dx * (range * fine_mul / sweep);
      rt.drag_continuous  = behavior_clamp(rt.drag_continuous, H.min, H.max);
      return behavior_write_value(H, ctx, H.target, rt.drag_continuous);
    }
    if (H.kind == NEUI_BEHAVIOR_KIND_DRAG_BIAXIAL) {
      float sweep_x = (H.sweep   > 0.0f) ? H.sweep   : 1.0f;
      float sweep_y = (H.sweep_y > 0.0f) ? H.sweep_y : 1.0f;
      float dx = static_cast<float>(static_cast<int>(local_x) - rt.drag_prev_x);
      float dy = static_cast<float>(static_cast<int>(local_y) - rt.drag_prev_y);
      rt.drag_prev_x = static_cast<int>(local_x);
      rt.drag_prev_y = static_cast<int>(local_y);
      rt.drag_continuous   += dx * (range * fine_mul / sweep_x);
      rt.drag_continuous_y += -dy * (range * fine_mul / sweep_y);
      rt.drag_continuous   = behavior_clamp(rt.drag_continuous,   H.min, H.max);
      rt.drag_continuous_y = behavior_clamp(rt.drag_continuous_y, H.min, H.max);
      bool a = behavior_write_value(H, ctx, H.target, rt.drag_continuous);
      bool b = !H.target_y.empty()
                 ? behavior_write_value(H, ctx, H.target_y, rt.drag_continuous_y)
                 : false;
      return a || b;
    }
    if (H.kind == NEUI_BEHAVIOR_KIND_DRAG_ROTATIONAL) {
      float cx = ctx.widget_w * 0.5f;
      float cy = ctx.widget_h * 0.5f;
      float dx = local_x - cx;
      float dy = local_y - cy;
      float r2 = dx*dx + dy*dy;
      float dz = (H.deadzone > 0.0f) ? H.deadzone : 0.0f;
      if (r2 < dz * dz) return false;
      float cur_angle = std::atan2(dy, dx);
      float dtheta    = behavior_wrap_pi(cur_angle - rt.drag_prev_angle);
      rt.drag_prev_angle = cur_angle;
      rt.drag_continuous += dtheta * (range * fine_mul / BEHAVIOR_KNOB_SWEEP_RAD);
      rt.drag_continuous  = behavior_clamp(rt.drag_continuous, H.min, H.max);
      return behavior_write_value(H, ctx, H.target, rt.drag_continuous);
    }
    return false;
  }

  // Begin a drag on the given handler. Captures the initial cursor /
  // angle and seeds the continuous accumulator from the current value.
  inline void behavior_begin_drag(BehaviorHandler& H,
                                    BehaviorRuntime& rt,
                                    const BehaviorDispatchCtx& ctx,
                                    uint32_t handler_slot,
                                    float local_x, float local_y)
  {
    rt.dragging       = true;
    rt.active_handler = handler_slot;
    rt.drag_prev_x    = static_cast<int>(local_x);
    rt.drag_prev_y    = static_cast<int>(local_y);
    rt.drag_continuous = behavior_read_value(H, ctx.bag);
    if (H.kind == NEUI_BEHAVIOR_KIND_DRAG_ROTATIONAL) {
      float cx = ctx.widget_w * 0.5f;
      float cy = ctx.widget_h * 0.5f;
      float dx = local_x - cx;
      float dy = local_y - cy;
      float r2 = dx*dx + dy*dy;
      float dz = (H.deadzone > 0.0f) ? H.deadzone : 0.0f;
      if (r2 < dz * dz) {
        // Click bang in the centre: don't start a drag we can't track.
        rt.dragging       = false;
        rt.active_handler = 0;
        return;
      }
      rt.drag_prev_angle = std::atan2(dy, dx);
    }
    if (H.kind == NEUI_BEHAVIOR_KIND_DRAG_BIAXIAL && !H.target_y.empty()) {
      rt.drag_continuous_y = behavior_read_value_y(H, ctx.bag);
    }
  }

  // ---- Main mouse dispatch -----------------------------------------------

  // Returns true if any handler consumed the event.
  inline bool behavior_dispatch_mouse(BehaviorAsset& ba,
                                       BehaviorRuntime& rt,
                                       const BehaviorDispatchCtx& ctx,
                                       neui_event_t* event,
                                       float local_x, float local_y)
  {
    if (!event) return false;

    // ---- In-progress drag steering -------------------------------------
    if (rt.dragging) {
      BehaviorHandler* H = behavior_get_handler(ba, rt.active_handler);
      if (!H) {
        // Handler was removed mid-drag: cancel the drag.
        rt.dragging = false;
        rt.active_handler = 0;
        return false;
      }
      if (event->type == NEUI_EVENT_MOUSE_BUTTON_UP ||
          (event->type == NEUI_EVENT_MOUSE_MOVE &&
            !(event->data.mouse.buttonmap & NEUI_MK_LBUTTON))) {
        rt.dragging = false;
        rt.active_handler = 0;
        return true;
      }
      if (event->type == NEUI_EVENT_MOUSE_MOVE) {
        // DRAG_SOURCE arms on BUTTON_DOWN and fires begin_drag once the
        // cursor crosses threshold_px. Until then we just consume MOVE so
        // no other handler picks it up; after firing we clear our state
        // (the OS owns the cursor for the rest of the drag).
        if (H->kind == NEUI_BEHAVIOR_KIND_DRAG_SOURCE) {
          float dx = local_x - static_cast<float>(rt.drag_prev_x);
          float dy = local_y - static_cast<float>(rt.drag_prev_y);
          float thr = (H->threshold_px > 0.0f) ? H->threshold_px : 4.0f;
          if (dx*dx + dy*dy < thr*thr) return true;
          // Clear BEFORE firing - begin_drag is blocking and re-enters the
          // dispatcher via drop events on targets in the same session.
          rt.dragging = false;
          rt.active_handler = 0;
          if (ctx.begin_drag) {
            neui_data_item_t item = neui_data_item_none;
            if (ctx.bag && !H->drag_data_key.empty()) {
              int v = ctx.bag->get_int(H->drag_data_key, 0);
              if (v != 0) item.id = static_cast<uint32_t>(v);
            }
            uint32_t preview_id = 0;
            if (ctx.bag && !H->drag_preview_key.empty()) {
              int v = ctx.bag->get_int(H->drag_preview_key, 0);
              if (v != 0) preview_id = static_cast<uint32_t>(v);
            }
            uint32_t action = ctx.begin_drag(ctx.host_data, item,
                                              H->allowed_actions,
                                              preview_id,
                                              H->drag_hot_x, H->drag_hot_y);
            if (ctx.bag && !H->result_attr.empty()) {
              ctx.bag->set_int(H->result_attr, static_cast<int>(action));
              if (ctx.emit_attr_changed) {
                ctx.emit_attr_changed(ctx.host_data, H->result_attr.c_str(),
                                       static_cast<float>(action));
              }
              if (ctx.invalidate) ctx.invalidate(ctx.host_data);
            }
          }
          return true;
        }
        behavior_apply_drag_move(*H, rt, ctx, local_x, local_y,
                                  event->data.mouse.buttonmap);
        return true;
      }
      return false;
    }

    // ---- Mouse wheel ----------------------------------------------------
    if (event->type == NEUI_EVENT_MOUSE_WHEEL) {
      BehaviorHandler* H = behavior_find_kind(ba,
                                                NEUI_BEHAVIOR_KIND_WHEEL,
                                                ctx.widget_w, ctx.widget_h,
                                                local_x, local_y);
      if (!H) return false;
      // Wheel up DECREASES, wheel down INCREASES (audio-plugin convention,
      // matches the existing KNOB). Delta arrives in lines (Win32 converts
      // WHEEL_DELTA notches via SPI_GETWHEELSCROLLLINES; macOS / xpl pass
      // +/-1 per notch). Multiply by |delta| so one notch advances by
      // `step * lines_per_notch` rather than a single `step`, otherwise
      // wheel feels imperceptible at typical step values (~0.01..0.05).
      // The wheel event payload doesn't carry modifier bits today
      // (neui_event_wheel_t has no buttonmap), so fine_modifier on WHEEL
      // is a no-op in v1.
      int   delta   = event->data.wheel.delta;
      if (delta == 0) return true;
      float sign    = (delta > 0) ? -1.0f : 1.0f;
      int   mag     = (delta > 0) ? delta : -delta;
      float change  = sign * H->step * static_cast<float>(mag);
      float current = behavior_read_value(*H, ctx.bag);
      behavior_write_value(*H, ctx, H->target, current + change);
      return true;
    }

    // ---- Right-click context menu (CONTEXT_RESET) -----------------------
    if (event->type == NEUI_EVENT_MOUSE_RBUTTON_DOWN) {
      BehaviorHandler* H = behavior_find_kind(ba,
                                                NEUI_BEHAVIOR_KIND_CONTEXT_RESET,
                                                ctx.widget_w, ctx.widget_h,
                                                local_x, local_y);
      if (!H || !ctx.popup_menu) return false;
      static const char* k_items[] = { "Reset to default", nullptr };
      int picked = ctx.popup_menu(ctx.host_data,
                                    static_cast<int>(local_x),
                                    static_cast<int>(local_y),
                                    k_items);
      if (picked == 1) {
        float def_v = H->min;
        if (ctx.bag && !H->target_default.empty()) {
          def_v = attr_as_float(ctx.bag, H->target_default, H->min);
        }
        behavior_write_value(*H, ctx, H->target, def_v);
      }
      return true;
    }

    // ---- Mouse button down: click + drag start --------------------------
    if (event->type == NEUI_EVENT_MOUSE_BUTTON_DOWN) {
      uint32_t slot = 0;
      BehaviorHandler* H = behavior_find_drag_handler(ba,
                                                       ctx.widget_w, ctx.widget_h,
                                                       local_x, local_y, &slot);
      if (!H) return false;

      if (H->kind == NEUI_BEHAVIOR_KIND_CLICK_TOGGLE) {
        float current = behavior_read_value(*H, ctx.bag);
        float mid = 0.5f * (H->min + H->max);
        float new_v = (current >= mid) ? H->min : H->max;
        behavior_write_value(*H, ctx, H->target, new_v);
        return true;
      }
      if (H->kind == NEUI_BEHAVIOR_KIND_CLICK_SELECT) {
        // Toggle: flip target between min (deselected) and max (selected),
        // and mirror the on/off state into the selected int attr. The float
        // write goes through behavior_write_value (snap / clamp / invalidate /
        // emit); the int write mirrors the DRAG_SOURCE result_attr pattern
        // (direct set_int + its own change-guard + emit + invalidate).
        float current = behavior_read_value(*H, ctx.bag);
        float mid = 0.5f * (H->min + H->max);
        bool now_selected = !(current >= mid);
        float new_v = now_selected ? H->max : H->min;
        behavior_write_value(*H, ctx, H->target, new_v);
        if (ctx.bag && !H->selected_attr.empty()) {
          int sv = now_selected ? 1 : 0;
          if (ctx.bag->get_int(H->selected_attr, -1) != sv) {
            ctx.bag->set_int(H->selected_attr, sv);
            if (ctx.emit_attr_changed)
              ctx.emit_attr_changed(ctx.host_data, H->selected_attr.c_str(),
                                    static_cast<float>(sv));
            if (ctx.invalidate) ctx.invalidate(ctx.host_data);
          }
        }
        return true;
      }
      if (H->kind == NEUI_BEHAVIOR_KIND_CLICK_CYCLE) {
        int steps = behavior_read_steps(*H, ctx.bag);
        if (steps < 2) {
          // No discrete positions configured - degrade to toggle.
          float current = behavior_read_value(*H, ctx.bag);
          float mid = 0.5f * (H->min + H->max);
          float new_v = (current >= mid) ? H->min : H->max;
          behavior_write_value(*H, ctx, H->target, new_v);
          return true;
        }
        float current = behavior_read_value(*H, ctx.bag);
        float t = (H->max > H->min)
                    ? (current - H->min) / (H->max - H->min) : 0.0f;
        int idx = static_cast<int>(t * static_cast<float>(steps - 1) + 0.5f);
        int next = idx + 1;
        if (next >= steps) next = H->wrap ? 0 : steps - 1;
        float new_v = H->min + (H->max - H->min) *
                      (static_cast<float>(next) / static_cast<float>(steps - 1));
        behavior_write_value(*H, ctx, H->target, new_v);
        return true;
      }

      if (H->kind == NEUI_BEHAVIOR_KIND_DRAG_SOURCE) {
        // Just arm: store anchor + slot, set dragging so subsequent MOVE
        // events steer through the in-progress branch above. We don't seed
        // a continuous accumulator (there's no value to mutate); the next
        // MOVE past threshold_px fires begin_drag.
        rt.dragging       = true;
        rt.active_handler = slot;
        rt.drag_prev_x    = static_cast<int>(local_x);
        rt.drag_prev_y    = static_cast<int>(local_y);
        return true;
      }

      behavior_begin_drag(*H, rt, ctx, slot, local_x, local_y);
      // The click landed on a drag-kind handler; we claim the event even
      // when behavior_begin_drag refused (e.g. centre dead-zone) - matches
      // the existing KNOB behaviour ("click bang in the centre is a no-op,
      // not a fall-through").
      return true;
    }

    return false;
  }

  // ---- Key dispatch ------------------------------------------------------

  inline bool behavior_dispatch_key(BehaviorAsset& ba,
                                     BehaviorRuntime& /*rt*/,
                                     const BehaviorDispatchCtx& ctx,
                                     uint32_t keycode, uint32_t modifiers)
  {
    BehaviorHandler* H = behavior_find_kind_any(ba, NEUI_BEHAVIOR_KIND_KEY_STEP);
    if (!H) return false;

    float fine_mul = behavior_fine_mul_kmod(*H, modifiers);
    float current  = behavior_read_value(*H, ctx.bag);
    float new_v    = current;

    switch (keycode) {
      case NEUI_KEY_RIGHT:
      case NEUI_KEY_UP:
        new_v = current + H->step * fine_mul;
        break;
      case NEUI_KEY_LEFT:
      case NEUI_KEY_DOWN:
        new_v = current - H->step * fine_mul;
        break;
      case NEUI_KEY_HOME:
        new_v = H->min;
        break;
      case NEUI_KEY_END:
        new_v = H->max;
        break;
      case NEUI_KEY_PAGEUP:
        new_v = current + H->coarse * fine_mul;
        break;
      case NEUI_KEY_PAGEDOWN:
        new_v = current - H->coarse * fine_mul;
        break;
      default:
        return false;
    }
    behavior_write_value(*H, ctx, H->target, new_v);
    return true;
  }

} // namespace neui_detail
