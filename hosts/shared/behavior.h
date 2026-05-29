#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <neui/neui.h>

#include "compound.h"   // LayerRect, anchor_point, resolve_layer_rect

// Behavior asset - internal data model. Shared between hosts.
// A behavior asset is a list of input handlers; each handler interprets
// mouse / key / wheel events and writes a named float attr on the
// connected widget (the "target" prop, default NEUI_PARAM_VALUE).
//
// This header owns the slot-reused handler table, the per-handler prop
// bag, the hit-region resolver, and the prop-setter helpers (called by
// the host's neui_behavior_api thunks). The actual event dispatch +
// per-widget drag state live in behavior_runtime.h alongside the value-
// write helpers.

namespace neui_detail
{
  enum class FineModifier : uint8_t {
    Shift = 0,
    Ctrl  = 1,
    Alt   = 2,
    None  = 3,
  };

  // Translate the "fine_modifier" string prop into a typed enum at set time
  // so per-frame dispatch is a single int compare. Unknown values fall
  // back to Shift (matches the existing KNOB feel).
  inline FineModifier parse_fine_modifier(const char* s)
  {
    if (!s) return FineModifier::Shift;
    if (std::strcmp(s, "ctrl")  == 0) return FineModifier::Ctrl;
    if (std::strcmp(s, "alt")   == 0) return FineModifier::Alt;
    if (std::strcmp(s, "none")  == 0) return FineModifier::None;
    return FineModifier::Shift;
  }

  struct BehaviorHandler
  {
    neui_behavior_kind_t kind = NEUI_BEHAVIOR_KIND_NONE;

    // ---- Common props -----------------------------------------------------
    std::string  target          = "neui.param.value";
    std::string  target_default  = "neui.param.default";
    std::string  target_y;                  // DRAG_BIAXIAL only
    std::string  snap_attr       = "neui.attr.steps";
    std::string  cursor;
    FineModifier fine_modifier   = FineModifier::Shift;
    float        min             = 0.0f;
    float        max             = 1.0f;
    float        step            = 0.01f;
    float        coarse          = 0.10f;
    float        fine_scale      = 0.2f;

    // ---- Drag-specific ----------------------------------------------------
    float sweep    = 200.0f;
    float sweep_y  = 200.0f;
    float deadzone = 4.0f;

    // ---- Cycle-specific ---------------------------------------------------
    int wrap = 0;

    // ---- Hit region (compound-style 9-pt anchor) --------------------------
    neui_anchor_t anchor_parent = NEUI_ANCHOR_TOP_LEFT;
    neui_anchor_t anchor_self   = NEUI_ANCHOR_TOP_LEFT;
    int offset_x = 0;
    int offset_y = 0;
    int width    = NEUI_COMPOUND_FILL;
    int height   = NEUI_COMPOUND_FILL;
  };

  // The behavior asset itself - a slot-reused vector of BehaviorHandler.
  // Slot 0 is intentionally unused so a returned 0 means "no handler".
  struct BehaviorAsset
  {
    std::vector<std::unique_ptr<BehaviorHandler>> handlers;
    std::vector<uint32_t>                          free_slots;
  };

  // ---- Handle encoding -----------------------------------------------------

  // neui_behavior_handler_t.id = (asset_slot << 16) | handler_slot.
  inline uint32_t behavior_handler_asset_slot(neui_behavior_handler_t h)
  { return (h.id >> 16) & 0xffff; }
  inline uint32_t behavior_handler_slot(neui_behavior_handler_t h)
  { return h.id & 0xffff; }
  inline neui_behavior_handler_t pack_behavior_handler(uint32_t asset_slot, uint32_t handler_slot)
  { return { ((asset_slot & 0xffff) << 16) | (handler_slot & 0xffff) }; }

  // ---- Handler table management -------------------------------------------

  inline uint32_t behavior_add_handler(BehaviorAsset& ba, neui_behavior_kind_t kind)
  {
    auto h = std::make_unique<BehaviorHandler>();
    h->kind = kind;
    uint32_t slot;
    if (!ba.free_slots.empty()) {
      slot = ba.free_slots.back();
      ba.free_slots.pop_back();
      ba.handlers[slot] = std::move(h);
    } else {
      if (ba.handlers.empty()) ba.handlers.emplace_back(nullptr);  // slot 0 unused
      slot = static_cast<uint32_t>(ba.handlers.size());
      ba.handlers.emplace_back(std::move(h));
    }
    return slot;
  }

  inline void behavior_remove_handler(BehaviorAsset& ba, uint32_t slot)
  {
    if (slot == 0 || slot >= ba.handlers.size()) return;
    if (!ba.handlers[slot]) return;
    ba.handlers[slot].reset();
    ba.free_slots.push_back(slot);
  }

  inline void behavior_clear(BehaviorAsset& ba)
  {
    ba.handlers.clear();
    ba.free_slots.clear();
  }

  inline BehaviorHandler* behavior_get_handler(BehaviorAsset& ba, uint32_t slot)
  {
    if (slot == 0 || slot >= ba.handlers.size()) return nullptr;
    return ba.handlers[slot].get();
  }

  inline const BehaviorHandler* behavior_get_handler(const BehaviorAsset& ba, uint32_t slot)
  {
    if (slot == 0 || slot >= ba.handlers.size()) return nullptr;
    return ba.handlers[slot].get();
  }

  // ---- Prop setters --------------------------------------------------------
  //
  // Recognised prop names land in the typed slots above; unknown names are
  // silently ignored (no error - matches the AttrBag / compound contract
  // and leaves room for forward-compatible client code).

  inline void apply_behavior_set_int(BehaviorHandler& H, const std::string& prop, int v)
  {
    if (prop == "anchor_parent") { H.anchor_parent = static_cast<neui_anchor_t>(v); return; }
    if (prop == "anchor_self")   { H.anchor_self   = static_cast<neui_anchor_t>(v); return; }
    if (prop == "offset_x")      { H.offset_x = v; return; }
    if (prop == "offset_y")      { H.offset_y = v; return; }
    if (prop == "width")         { H.width    = v; return; }
    if (prop == "height")        { H.height   = v; return; }
    if (prop == "wrap")          { H.wrap     = v; return; }
  }

  inline void apply_behavior_set_float(BehaviorHandler& H, const std::string& prop, float v)
  {
    if (prop == "min")        { H.min        = v; return; }
    if (prop == "max")        { H.max        = v; return; }
    if (prop == "step")       { H.step       = v; return; }
    if (prop == "coarse")     { H.coarse     = v; return; }
    if (prop == "fine_scale") { H.fine_scale = v; return; }
    if (prop == "sweep")      { H.sweep      = v; return; }
    if (prop == "sweep_y")    { H.sweep_y    = v; return; }
    if (prop == "deadzone")   { H.deadzone   = v; return; }
  }

  inline void apply_behavior_set_string(BehaviorHandler& H, const std::string& prop, const char* v)
  {
    const char* sv = v ? v : "";
    if (prop == "target")         { H.target          = sv; return; }
    if (prop == "target_default") { H.target_default  = sv; return; }
    if (prop == "target_y")       { H.target_y        = sv; return; }
    if (prop == "snap_attr")      { H.snap_attr       = sv; return; }
    if (prop == "cursor")         { H.cursor          = sv; return; }
    if (prop == "fine_modifier")  { H.fine_modifier   = parse_fine_modifier(sv); return; }
  }

  // ---- Hit region resolution ----------------------------------------------

  // Compute the handler's effective rect in widget-local coordinates,
  // honouring NEUI_COMPOUND_FILL on either axis.
  inline LayerRect behavior_compute_rect(const BehaviorHandler& H,
                                          float widget_w, float widget_h)
  {
    float eff_w = (H.width  == NEUI_COMPOUND_FILL) ? widget_w : static_cast<float>(H.width);
    float eff_h = (H.height == NEUI_COMPOUND_FILL) ? widget_h : static_cast<float>(H.height);
    return resolve_layer_rect(widget_w, widget_h,
                                H.anchor_parent, H.anchor_self,
                                static_cast<float>(H.offset_x),
                                static_cast<float>(H.offset_y),
                                eff_w, eff_h);
  }

  inline bool behavior_hit_test(const BehaviorHandler& H,
                                  float widget_w, float widget_h,
                                  float local_x, float local_y)
  {
    LayerRect r = behavior_compute_rect(H, widget_w, widget_h);
    return local_x >= r.x && local_x < r.x + r.w &&
           local_y >= r.y && local_y < r.y + r.h;
  }

} // namespace neui_detail
