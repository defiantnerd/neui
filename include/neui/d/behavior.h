#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "api.h"
#include "assets.h"

// Behavior API - reached via
//   neui_behavior_api_t* be = (neui_behavior_api_t*)
//     neui_api->get_interface(session, NEUI_API_BEHAVIOR);
// and operates on neui_asset_t handles previously returned from
// neui_asset_api_t::create_behavior.
//
// A behavior asset is a mutable list of input handlers (drag / wheel / key /
// click / context-reset). Attached to a CUSTOMDRAW widget via
// widgets->set_asset (kind-routed: a BEHAVIOR handle lands in the widget's
// behavior slot, independent of the compound visual slot). The framework
// dispatches mouse / key / wheel events to each matching handler in
// insertion order; the handler reads / writes a named float attr in the
// widget's AttrBag (target). Companion compound bindings then re-render
// the visual the same way they would for a programmatic attr write.
//
// The compound + behavior pair lets a CUSTOMDRAW widget become a fully
// interactive painted control without any client-side onevent plumbing.
//
// Optional hit region per handler uses the same 9-point anchor system as
// compound layers (see <neui/d/compound.h>); default is whole widget.

#ifdef __cplusplus
extern "C" {
#endif

#define NEUI_API_BEHAVIOR "com.defiantnerd.neui.extension.behavior/0"

  // Handler handle. Internally (asset_slot << 16) | handler_slot - same
  // layout as neui_compound_layer_t. Slot-reused on remove_handler.
  typedef struct neui_behavior_handler {
    uint32_t id;
  } neui_behavior_handler_t;

  static const neui_behavior_handler_t behavior_handler_none = { UINT32_MAX };

  typedef enum neui_behavior_kind {
    NEUI_BEHAVIOR_KIND_NONE             = 0,
    // Vertical drag - dy / sweep is added to target attr. Up = increase
    // (positive). Honours `min`, `max`, `sweep`, `fine_modifier`,
    // `fine_scale`, `snap_attr`.
    NEUI_BEHAVIOR_KIND_DRAG_VERTICAL    = 1,
    // Horizontal drag - dx / sweep added. Right = increase.
    NEUI_BEHAVIOR_KIND_DRAG_HORIZONTAL  = 2,
    // Rotational drag - cursor angle from widget centre. 1.5*pi sweep,
    // central dead-zone configurable via `deadzone`.
    NEUI_BEHAVIOR_KIND_DRAG_ROTATIONAL  = 3,
    // Biaxial drag - dx writes target, dy writes target_y. Both axes
    // honour the same `min` / `max` / `fine_modifier`. `sweep` controls
    // x-axis; `sweep_y` controls y-axis.
    NEUI_BEHAVIOR_KIND_DRAG_BIAXIAL     = 4,
    // Mouse wheel - delta * step added to target. Honours `step`,
    // `fine_modifier`, `fine_scale`, `min`, `max`.
    NEUI_BEHAVIOR_KIND_WHEEL            = 5,
    // Arrow keys + Home / End + PageUp / PageDown. Requires the widget
    // to be the focused one. step = arrow, coarse = Page, min/max = Home/End.
    NEUI_BEHAVIOR_KIND_KEY_STEP         = 6,
    // Click toggle - LBUTTON click flips between min and max.
    NEUI_BEHAVIOR_KIND_CLICK_TOGGLE     = 7,
    // Click cycle - LBUTTON click steps target to next snap position
    // (modulo `steps` snap count, with `wrap` int prop controlling
    // wrap-vs-clamp at the top).
    NEUI_BEHAVIOR_KIND_CLICK_CYCLE      = 8,
    // Right-click -> "Reset to default" popup_menu. Reads the value held
    // by `target_default` attr key (or NEUI_PARAM_DEFAULT if not set)
    // and writes it to `target`. Useful as a one-handler-per-widget UX
    // standard.
    NEUI_BEHAVIOR_KIND_CONTEXT_RESET    = 9,
    // Drag-source. Arms on MOUSE_BUTTON_DOWN inside the hit region; on the
    // first MOUSE_MOVE that takes the cursor past `threshold_px` it calls
    // dnd_api->begin_drag with the data-item id resolved from the widget's
    // AttrBag at `drag_data_key` and the configured `allowed_actions`
    // bitmask. begin_drag is blocking - the handler does not return until
    // the OS drag loop ends. Independent of the visual compound asset, so
    // a CUSTOMDRAW with a compound + DRAG_SOURCE behavior initiates drags
    // with zero client-side mouse code. The client is responsible for
    // building the data item (clipboard_api->create_item / item_set_format)
    // and stashing its id in the widget's AttrBag before mouse-down.
    NEUI_BEHAVIOR_KIND_DRAG_SOURCE      = 10,
  } neui_behavior_kind_t;

  typedef struct neui_behavior_api {
    uint32_t neui_version;

    // -------- Handler management -------------------------------------------

    // Append a new handler to the behavior asset. Returns
    // behavior_handler_none on bad args or non-behavior asset.
    neui_behavior_handler_t (NEUI_ABI *add_handler)(neui_session_t session,
                                                      neui_asset_t asset,
                                                      neui_behavior_kind_t kind);

    void (NEUI_ABI *remove_handler)(neui_session_t session, neui_asset_t asset,
                                      neui_behavior_handler_t handler);

    void (NEUI_ABI *clear)(neui_session_t session, neui_asset_t asset);

    // -------- Property setters ---------------------------------------------
    //
    // Recognised prop names (unknown names are stored but inert):
    //
    //   common:
    //     "target"        string  attr key written by the handler (the value
    //                             the user manipulates). Required for every
    //                             non-context handler. Default "neui.param.value".
    //     "target_default" string attr key read for the reset value.
    //                             Default "neui.param.default".
    //     "min"           float   lower bound (default 0.0)
    //     "max"           float   upper bound (default 1.0)
    //     "step"          float   nudge per wheel notch / arrow key (default 0.01)
    //     "coarse"        float   nudge per PageUp / PageDown (default 0.10)
    //     "snap_attr"     string  attr key on the widget that holds the
    //                             discrete-step count; <2 = continuous.
    //                             Default "neui.attr.steps".
    //     "fine_modifier" string  "shift"/"ctrl"/"alt"/"none" - which
    //                             modifier scales motion by `fine_scale`.
    //                             Default "shift".
    //     "fine_scale"    float   multiplier under the fine modifier
    //                             (default 0.2 - matches the existing KNOB).
    //     "cursor"        string  advisory cursor hint ("arrow"/"hand"/...) -
    //                             stored but not applied in v1.
    //
    //   drag-specific (DRAG_* kinds):
    //     "sweep"         float   pixels for a full 0..1 sweep on linear
    //                             modes (default 200, matches KNOB).
    //     "deadzone"      float   centre dead-zone radius for rotational
    //                             mode (px, default 4).
    //
    //   cycle-specific (CLICK_CYCLE):
    //     "wrap"          int     0 = clamp at max, 1 = wrap to min
    //                             (default 0).
    //
    //   drag-source-specific (DRAG_SOURCE):
    //     "threshold_px"   float  pixels of cursor motion before begin_drag
    //                             fires (default 4).
    //     "allowed_actions" int   bitmask of NEUI_DND_ACTION_* (default
    //                             NEUI_DND_ACTION_COPY | NEUI_DND_ACTION_MOVE).
    //     "drag_data_key"  string attr key on the widget that holds the
    //                             data-item id (set by the client before
    //                             mouse-down via attrs->set_int). Empty /
    //                             unset = drag with neui_data_item_none.
    //     "drag_preview_key" string attr key on the widget that holds the
    //                             preview-image asset id (any asset that
    //                             resolves to displayable pixels - BITMAP
    //                             today, SURFACE tomorrow). Empty / unset
    //                             = use OS default drag visual.
    //     "drag_hot_x"     int    hot-spot X on the preview image, logical
    //                             px from top-left. -1 = image centre.
    //     "drag_hot_y"     int    hot-spot Y. -1 = image centre.
    //
    //   hit region (any kind):
    //     "anchor_parent" int     neui_anchor_t on widget rect (default
    //                             NEUI_ANCHOR_TOP_LEFT).
    //     "anchor_self"   int     neui_anchor_t on handler rect (default
    //                             NEUI_ANCHOR_TOP_LEFT).
    //     "offset_x"      int     px, relative to anchor.
    //     "offset_y"      int     px.
    //     "width"         int     px (NEUI_COMPOUND_FILL = -1 = full widget).
    //     "height"        int     px (NEUI_COMPOUND_FILL = -1 = full widget).

    void (NEUI_ABI *set_int)   (neui_session_t session, neui_asset_t asset,
                                  neui_behavior_handler_t handler,
                                  const char* prop, int value);
    void (NEUI_ABI *set_float) (neui_session_t session, neui_asset_t asset,
                                  neui_behavior_handler_t handler,
                                  const char* prop, float value);
    void (NEUI_ABI *set_string)(neui_session_t session, neui_asset_t asset,
                                  neui_behavior_handler_t handler,
                                  const char* prop, const char* value);
  } neui_behavior_api_t;

#ifdef __cplusplus
}
#endif
